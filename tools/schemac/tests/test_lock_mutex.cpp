// LockFileMutex, the cross-process mutex of a schema lock file: the errors a racing
// create_directory() reports on each standard library (injected, since real runs hit them only
// intermittently), bounded waits with clear errors, release retries, stale takeover by exactly one
// waiter, and mutual exclusion without lost updates under real contention.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "helios/core/fs.h"
#include "lock_mutex.h"

namespace {

namespace fs = std::filesystem;
using helios::schemac::LockFileMutex;
using helios::schemac::LockMutexFileOps;
using helios::schemac::LockMutexTiming;
using Ms = std::chrono::milliseconds;

fs::path freshDir(const std::string& name) {
    const fs::path dir = fs::path(HELIOS_TEST_WORK_DIR) / "lock_mutex" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string utf8(const fs::path& p) { return helios::fs::pathToUtf8(p); }
fs::path mutexDirOf(const std::string& lock) { return helios::fs::pathFromUtf8(lock + ".writing"); }
fs::path takeoverDirOf(const std::string& lock) {
    return helios::fs::pathFromUtf8(lock + ".writing-takeover");
}

/// Short waits so that failing paths finish quickly; staleAfter keeps its production value.
LockMutexTiming fastTiming() {
    LockMutexTiming t;
    t.giveUpAfter = Ms(20'000);
    t.transientFor = Ms(2'000);
    t.releaseFor = Ms(500);
    t.firstDelay = Ms(1);
    t.maxDelay = Ms(2);
    return t;
}

Ms elapsedSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<Ms>(std::chrono::steady_clock::now() - start);
}

/// Returns `injected[i]` from the i-th createDirectory() call without touching the disk, then
/// defers to std::filesystem.
LockMutexFileOps injectCreateErrors(const std::vector<std::error_code>& injected, std::size_t& calls) {
    LockMutexFileOps ops;
    ops.createDirectory = [&injected, &calls](const fs::path& p, std::error_code& ec) {
        if (calls < injected.size()) {
            ec = injected[calls++];
            return false;
        }
        ++calls;
        return fs::create_directory(p, ec);
    };
    return ops;
}

TEST_CASE("lock mutex: acquire retries the errors a racing create_directory reports") {
    // create_directory() fails with "already exists", then the library checks that the path is a
    // directory; if the holder removed it in between, each library reports something else.
    const std::vector<std::error_code> races = {
        std::make_error_code(std::errc::no_such_file_or_directory), // MSVC STL (CI run 36292817769)
        std::make_error_code(std::errc::file_exists),               // libstdc++, libc++
        std::make_error_code(std::errc::permission_denied),         // Windows: delete pending
        std::make_error_code(std::errc::device_or_resource_busy),
    };
    const fs::path dir = freshDir("race");
    const std::string lock = utf8(dir / "sub" / "schema.lock.jsonc"); // "sub" does not exist yet
    for (const std::error_code& race : races) {
        CAPTURE(race.message());
        const std::vector<std::error_code> injected = {race, race};
        std::size_t calls = 0;
        LockFileMutex mutex(fastTiming(), injectCreateErrors(injected, calls));
        std::string err;
        REQUIRE_MESSAGE(mutex.acquire(lock, err), err);
        CHECK(err.empty());
        CHECK(calls >= injected.size() + 1); // more if a real call meets a delete-pending directory
        CHECK(mutex.held());
        CHECK(fs::is_directory(mutexDirOf(lock)));
        mutex.release(err);
        CHECK(err.empty());
        CHECK_FALSE(mutex.held());
        CHECK_FALSE(fs::exists(mutexDirOf(lock)));
    }

    // All of them in a row, as a heavily contended run may see them.
    std::size_t calls = 0;
    LockFileMutex mutex(fastTiming(), injectCreateErrors(races, calls));
    std::string err;
    REQUIRE_MESSAGE(mutex.acquire(lock, err), err);
    CHECK(err.empty());
    CHECK(calls >= races.size() + 1);
}

TEST_CASE("lock mutex: a parent directory removed under a waiter is re-created") {
    const fs::path dir = freshDir("parent");
    const fs::path parent = dir / "lock";
    const std::string lock = utf8(parent / "schema.lock.jsonc");
    std::size_t calls = 0;
    LockMutexFileOps ops;
    ops.createDirectory = [&](const fs::path& p, std::error_code& ec) {
        if (calls++ == 0) fs::remove_all(parent); // a real "path not found" from the OS this time
        return fs::create_directory(p, ec);
    };
    LockFileMutex mutex(fastTiming(), ops);
    std::string err;
    REQUIRE_MESSAGE(mutex.acquire(lock, err), err);
    CHECK(err.empty());
    CHECK(calls >= 2);
    CHECK(fs::is_directory(mutexDirOf(lock)));
}

TEST_CASE("lock mutex: a transient error that persists fails with a clear error") {
    const fs::path dir = freshDir("persistent");
    const std::string lock = utf8(dir / "schema.lock.jsonc");
    std::size_t calls = 0;
    LockMutexFileOps ops;
    ops.createDirectory = [&calls](const fs::path&, std::error_code& ec) {
        ++calls;
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    };
    LockMutexTiming timing = fastTiming();
    timing.transientFor = Ms(100);
    LockFileMutex mutex(timing, ops);
    std::string err;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(mutex.acquire(lock, err));
    CHECK(elapsedSince(start) >= Ms(100));
    CHECK(elapsedSince(start) < Ms(10'000)); // not giveUpAfter (20 s)
    CHECK(calls > 2);
    CHECK_FALSE(mutex.held());
    CHECK_MESSAGE(err.starts_with("helios-schemac: error: cannot create '"), err);
    const std::string denied = std::make_error_code(std::errc::permission_denied).message();
    CHECK_MESSAGE(err.find(denied) != std::string::npos, err);
    CHECK_MESSAGE(err.find("(still failing after ") != std::string::npos, err);
}

TEST_CASE("lock mutex: permanent errors and a file in the way fail at once") {
    const fs::path dir = freshDir("permanent");
    const std::string lock = utf8(dir / "schema.lock.jsonc");

    const std::vector<std::error_code> injected = {std::make_error_code(std::errc::read_only_file_system)};
    std::size_t calls = 0;
    LockFileMutex readOnly(LockMutexTiming{}, injectCreateErrors(injected, calls));
    std::string err;
    CHECK_FALSE(readOnly.acquire(lock, err));
    CHECK(calls == 1);
    CHECK_MESSAGE(err.starts_with("helios-schemac: error: cannot create '"), err);

    // A file where the mutex directory belongs reports file_exists like the race does, but it
    // never goes away: fail at once (production timing: no transientFor wait).
    REQUIRE(helios::fs::writeTextFile(mutexDirOf(lock), "not a directory").ok());
    LockFileMutex blocked;
    err.clear();
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(blocked.acquire(lock, err));
    CHECK(elapsedSince(start) < Ms(5'000));
    CHECK_MESSAGE(err.find("a file of that name is in the way") != std::string::npos, err);
    CHECK(fs::is_regular_file(mutexDirOf(lock)));
}

TEST_CASE("lock mutex: release retries a transient failure to remove the directory") {
    const fs::path dir = freshDir("release");
    const std::string lock = utf8(dir / "schema.lock.jsonc");
    int failuresLeft = 3; // e.g. a virus scanner holding the directory without FILE_SHARE_DELETE
    LockMutexFileOps ops;
    ops.remove = [&failuresLeft](const fs::path& p, std::error_code& ec) {
        if (failuresLeft > 0) {
            --failuresLeft;
            ec = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        return fs::remove(p, ec);
    };
    {
        LockFileMutex mutex(fastTiming(), ops);
        std::string err;
        REQUIRE_MESSAGE(mutex.acquire(lock, err), err);
        mutex.release(err);
        CHECK(err.empty());
        CHECK(failuresLeft == 0);
        CHECK_FALSE(fs::exists(mutexDirOf(lock)));
    }

    // A failure that outlasts releaseFor is reported as a warning; the directory stays behind.
    failuresLeft = 1'000'000;
    LockMutexTiming timing = fastTiming();
    timing.releaseFor = Ms(50);
    LockFileMutex mutex(timing, ops);
    std::string err;
    REQUIRE_MESSAGE(mutex.acquire(lock, err), err);
    mutex.release(err);
    CHECK_FALSE(mutex.held());
    CHECK_MESSAGE(err.starts_with("helios-schemac: warning: cannot remove '"), err);
    CHECK(fs::is_directory(mutexDirOf(lock)));
    fs::remove(mutexDirOf(lock));
}

TEST_CASE("lock mutex: a stale mutex is taken over by exactly one waiter") {
    constexpr int kWaiters = 4;
    const fs::path dir = freshDir("stale_race");
    const std::string lock = utf8(dir / "schema.lock.jsonc");
    fs::create_directories(mutexDirOf(lock));
    fs::last_write_time(mutexDirOf(lock), fs::file_time_type::clock::now() - std::chrono::hours(1));
    // Widen the window between a waiter's staleness check and its removal of the directory. Without
    // a serialized takeover, a second waiter that saw the same stale directory removes the fresh
    // mutex the first waiter has just created, and both hold the lock.
    LockMutexFileOps ops;
    ops.lastWriteTime = [](const fs::path& p, std::error_code& ec) {
        const fs::file_time_type t = fs::last_write_time(p, ec);
        std::this_thread::sleep_for(Ms(20));
        return t;
    };
    std::atomic<int> ready{0};
    std::atomic<bool> inside{false};
    std::atomic<int> overlaps{0};
    std::vector<std::string> errors(kWaiters);
    std::vector<std::thread> threads;
    for (int i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&, i] {
            LockFileMutex mutex(fastTiming(), ops);
            std::string& err = errors[static_cast<std::size_t>(i)];
            ++ready;
            while (ready.load() < kWaiters) std::this_thread::yield();
            if (!mutex.acquire(lock, err)) return;
            if (inside.exchange(true)) ++overlaps;
            std::this_thread::sleep_for(Ms(30));
            inside = false;
            mutex.release(err);
        });
    }
    for (std::thread& t : threads) t.join();
    for (const std::string& err : errors) CHECK_MESSAGE(err.empty(), err);
    CHECK(overlaps.load() == 0);
    CHECK_FALSE(fs::exists(mutexDirOf(lock)));
    CHECK_FALSE(fs::exists(takeoverDirOf(lock)));
}

TEST_CASE("lock mutex: contending threads never overlap, fail or lose an update") {
    // Real directories, no injection: the threads hand the mutex around thousands of times, so
    // create_directory() often meets a directory that its holder is removing. Before the fix this
    // failed on Linux in 20 of 20 runs ("cannot create ...: File exists", the libstdc++ face of the
    // race behind CI run 36292817769). The shared counter is read and written in separate steps,
    // so an overlap would also lose increments.
    constexpr int kThreads = 8;
    constexpr int kRounds = 400;
    const fs::path dir = freshDir("contention");
    const std::string lock = utf8(dir / "schema.lock.jsonc");
    std::atomic<int> counter{0};
    std::atomic<bool> inside{false};
    std::atomic<int> overlaps{0};
    std::vector<std::string> errors(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            std::string& err = errors[static_cast<std::size_t>(i)];
            for (int round = 0; round < kRounds && err.empty(); ++round) {
                LockFileMutex mutex(fastTiming());
                if (!mutex.acquire(lock, err)) return;
                if (inside.exchange(true)) ++overlaps;
                const int value = counter.load();
                std::this_thread::yield();
                counter.store(value + 1);
                inside = false;
                mutex.release(err);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    for (const std::string& err : errors) CHECK_MESSAGE(err.empty(), err);
    CHECK(overlaps.load() == 0);
    CHECK(counter.load() == kThreads * kRounds);
    CHECK_FALSE(fs::exists(mutexDirOf(lock)));
}

} // namespace
