#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "helios/core/async_io.h"
#include "helios/core/fs.h"
#include "helios/core/jobs.h"
#include "helios/core/thread.h"

using namespace helios;

// Everything in this file, the test cases included, lives in an unnamed namespace. doctest names each
// test function DOCTEST_ANON_FUNC_<__COUNTER__> with internal linkage, so other test files have
// functions of the same names. MSVC (cl, not clang-cl) mangles a lambda local to such a function
// without anything unique to the file, so templates instantiated with it (Job, std::function, the
// lambda's operator()) become COMDATs that the linker merges across files: once this file's 8th test
// and test_jobs.cpp's 7th both passed a `[&] {...}` to a job, one of them ran the other's lambda and
// "jobs: higher priorities run first" hung. Unnamed-namespace names are unique per file on MSVC.
namespace {

struct TempFile {
    fs::Path dir;
    fs::Path path;
    std::vector<u8> bytes;
    explicit TempFile(usize size) {
        dir = fs::createUniqueTempDirectory("helios-async").value();
        path = dir / "data.bin";
        bytes.resize(size);
        for (usize i = 0; i < size; ++i) bytes[i] = static_cast<u8>((i * 2654435761u) >> 13);
        REQUIRE(fs::writeFile(path, bytes).ok());
    }
    ~TempFile() { (void)fs::removeAll(dir); }
};

TEST_CASE("async io: whole-file and ranged reads match the synchronous read") {
    TempFile file(1 << 20);
    jobs::BackgroundPool pool(2, "IO");
    fs::AsyncRead whole = fs::readFileAsync(pool, file.path);
    fs::AsyncReadOptions range;
    range.offset = 1000;
    range.size = 4096;
    fs::AsyncRead part = fs::readFileAsync(pool, file.path, range);
    fs::AsyncReadOptions tail;
    tail.offset = file.bytes.size() - 10;
    tail.size = 100; // past the end: truncated
    fs::AsyncRead end = fs::readFileAsync(pool, file.path, tail);
    fs::AsyncReadOptions beyond;
    beyond.offset = file.bytes.size() + 5;
    fs::AsyncRead none = fs::readFileAsync(pool, file.path, beyond);

    Result<std::vector<u8>> w = whole.take();
    REQUIRE(w.ok());
    CHECK(*w == file.bytes);
    CHECK(whole.isReady());
    CHECK(whole.counter().isDone());
    CHECK(whole.take().errorCode() == ErrorCode::InvalidState); // bytes were moved out
    CHECK(*whole.bytesRead() == file.bytes.size());

    Result<std::vector<u8>> p = part.take();
    REQUIRE(p.ok());
    CHECK(*p == std::vector<u8>(file.bytes.begin() + 1000, file.bytes.begin() + 1000 + 4096));
    Result<std::vector<u8>> e = end.take();
    REQUIRE(e.ok());
    CHECK(e->size() == 10);
    Result<std::vector<u8>> n = none.take();
    REQUIRE(n.ok());
    CHECK(n->empty());
}

TEST_CASE("async io: errors are reported through the handle") {
    jobs::BackgroundPool pool(1, "IO");
    fs::AsyncRead r = fs::readFileAsync(pool, fs::pathFromUtf8("does/not/exist.bin"));
    CHECK(r.take().errorCode() == ErrorCode::NotFound);
    CHECK(r.bytesRead().errorCode() == ErrorCode::NotFound);
    fs::AsyncRead empty;
    CHECK(!empty.isValid());
    CHECK(!empty.isReady());
    CHECK(empty.counter().isDone());
    CHECK(empty.take().errorCode() == ErrorCode::InvalidState);
}

TEST_CASE("async io: many concurrent ranged reads into caller memory") {
    TempFile file(256 * 1024);
    Result<fs::File> f = fs::File::open(file.path, fs::OpenMode::Read);
    REQUIRE(f.ok());
    jobs::BackgroundPool pool(3, "IO");
    constexpr usize kChunk = 4096;
    constexpr usize kCount = 64;
    std::vector<u8> dst(kChunk * kCount, 0);
    std::vector<fs::AsyncRead> reads;
    std::atomic<int> callbacks{0};
    for (usize i = 0; i < kCount; ++i) {
        // Reverse order so completion order differs from the file order.
        const usize chunk = kCount - 1 - i;
        reads.push_back(fs::readAsync(pool, *f, chunk * kChunk, std::span<u8>(dst.data() + chunk * kChunk, kChunk),
                                      jobs::Priority::Normal, [&](fs::AsyncRead& done) {
                                          CHECK(done.isReady());
                                          callbacks.fetch_add(1);
                                      }));
    }
    for (fs::AsyncRead& r : reads) {
        Result<usize> n = r.bytesRead();
        REQUIRE(n.ok());
        CHECK(*n == kChunk);
    }
    CHECK(callbacks.load() == static_cast<int>(kCount));
    CHECK(dst == std::vector<u8>(file.bytes.begin(), file.bytes.begin() + dst.size()));
    // readAsync fills caller memory; take() has no bytes to hand out.
    CHECK(reads[0].take().errorCode() == ErrorCode::InvalidState);
}

TEST_CASE("async io: a read is ready for other threads only after its onComplete has returned") {
    // Regression (the case above failed in CI with "63 == 64"): readiness was published before
    // onComplete ran and isReady()/wait()/bytesRead() went by it, so a thread waiting for a read could
    // return while the callback was still running and miss its effects. The callback blocks here until
    // released, which makes the old ordering fail deterministically.
    TempFile file(4096);
    ManualResetEvent entered;
    ManualResetEvent release;
    std::atomic<bool> readyInside{false};
    std::atomic<bool> callbackReturning{false};
    jobs::BackgroundPool pool(1, "IO");
    fs::AsyncReadOptions options;
    options.onComplete = [&](fs::AsyncRead& r) {
        readyInside.store(r.isReady()); // the callback's own thread already sees the result
        entered.set();
        (void)release.waitFor(std::chrono::seconds(20));
        callbackReturning.store(true);
    };
    fs::AsyncRead read = fs::readFileAsync(pool, file.path, options);
    REQUIRE(entered.waitFor(std::chrono::seconds(20)));
    CHECK(readyInside.load());
    CHECK(!read.isReady());
    CHECK(!read.counter().isDone());

    ManualResetEvent waiterReturned;
    Result<usize> count = usize{0};
    bool sawCallbackReturn = false;
    Thread waiter("AsyncWaiter", [&] {
        count = read.bytesRead();
        sawCallbackReturn = callbackReturning.load();
        waiterReturned.set();
    });
    // The callback is still blocked, so the waiter must be too.
    CHECK(!waiterReturned.waitFor(std::chrono::milliseconds(50)));
    release.set();
    waiter.join();
    CHECK(sawCallbackReturn);
    REQUIRE(count.ok());
    CHECK(*count == file.bytes.size());
    CHECK(read.isReady());
    CHECK(read.counter().isDone());
}

TEST_CASE("async io: onComplete's captures are destroyed before the read becomes ready") {
    // A waiter may free what the callback's captures refer to once the read is ready, so the callback
    // object must be gone by then. Its last capture's destructor blocks here until released.
    struct Probe {
        ManualResetEvent* entered;
        ManualResetEvent* release;
        Probe(ManualResetEvent* e, ManualResetEvent* r) : entered(e), release(r) {}
        ~Probe() {
            entered->set();
            (void)release->waitFor(std::chrono::seconds(20));
        }
    };
    TempFile file(4096);
    ManualResetEvent entered;
    ManualResetEvent release;
    ManualResetEvent unblock;
    jobs::Counter blocker;
    jobs::BackgroundPool pool(1, "IO");
    // Hold the only IO thread so every temporary copy of the callback is gone before the read runs.
    pool.run([&] { (void)unblock.waitFor(std::chrono::seconds(20)); }, &blocker);
    fs::AsyncRead read = [&] {
        fs::AsyncReadOptions options;
        options.onComplete = [probe = std::make_shared<Probe>(&entered, &release)](fs::AsyncRead&) {
            (void)probe;
        };
        return fs::readFileAsync(pool, file.path, std::move(options));
    }();
    unblock.set();
    REQUIRE(entered.waitFor(std::chrono::seconds(20)));
    CHECK(!read.isReady());
    CHECK(!read.counter().isDone());
    release.set();
    read.wait();
    CHECK(read.isReady());
    CHECK(read.take()->size() == file.bytes.size());
}

TEST_CASE("async io: a capture's destructor may wait on its own request") {
    // The callable is destroyed on the IO thread before the counter is released, so a capture that owns
    // a handle to its own request and waits on it while being destroyed must still see the request as
    // ready there; otherwise the IO thread waits for itself.
    struct Holder {
        fs::AsyncRead read;
        ManualResetEvent* done = nullptr;
        std::atomic<bool>* readyThere = nullptr;
        std::atomic<usize>* countThere = nullptr;
        ~Holder() {
            read.wait();
            readyThere->store(read.isReady());
            countThere->store(read.bytesRead().valueOr(0));
            done->set();
        }
    };
    TempFile file(4096);
    // Leaked if the IO thread deadlocks: destroying a pool whose thread is stuck would hang the test.
    auto* pool = new jobs::BackgroundPool(1, "IO");
    ManualResetEvent unblock;
    ManualResetEvent done;
    std::atomic<bool> readyThere{false};
    std::atomic<usize> countThere{0};
    jobs::Counter blocker;
    // Hold the only IO thread until the holder owns its handle and every temporary copy of the callback
    // is gone.
    pool->run([&] { (void)unblock.waitFor(std::chrono::seconds(20)); }, &blocker);
    fs::AsyncRead read;
    {
        auto holder = std::make_shared<Holder>();
        holder->done = &done;
        holder->readyThere = &readyThere;
        holder->countThere = &countThere;
        fs::AsyncReadOptions options;
        options.onComplete = [holder](fs::AsyncRead&) { (void)holder; };
        holder->read = fs::readFileAsync(*pool, file.path, std::move(options));
        read = holder->read;
    } // the callback now owns the last reference to the holder
    unblock.set();
    REQUIRE(done.waitFor(std::chrono::seconds(20)));
    CHECK(readyThere.load());
    CHECK(countThere.load() == file.bytes.size());
    read.wait();
    CHECK(read.isReady());
    pool->waitIdle();
    delete pool;
}

TEST_CASE("async io: JobSystem::wait helps while a read completes") {
    TempFile file(64 * 1024);
    jobs::JobSystem js(jobs::JobSystemDesc{2, 1, "AsyncTest"});
    REQUIRE(js.background() != nullptr);
    fs::AsyncRead r = fs::readFileAsync(*js.background(), file.path);
    std::atomic<int> helped{0};
    jobs::Counter other;
    for (int i = 0; i < 16; ++i) js.run([&] { helped.fetch_add(1); }, &other);
    js.wait(r.counter());
    js.wait(other);
    CHECK(r.isReady());
    CHECK(helped.load() == 16);
    CHECK(r.take()->size() == file.bytes.size());
}

TEST_CASE("async io: cancelling a queued request") {
    TempFile file(1024);
    jobs::BackgroundPool pool(1, "IO");
    // Block the single IO thread so the next request stays queued.
    ManualResetEvent release;
    jobs::Counter blocker;
    pool.run([&] { release.wait(); }, &blocker);
    fs::AsyncRead queued = fs::readFileAsync(pool, file.path);
    CHECK(queued.cancel());
    CHECK(queued.cancel()); // idempotent
    release.set();
    pool.wait(blocker);
    CHECK(queued.take().errorCode() == ErrorCode::Cancelled);
    // A finished request cannot be cancelled.
    fs::AsyncRead done = fs::readFileAsync(pool, file.path);
    done.wait();
    CHECK(!done.cancel());
    CHECK(done.take().ok());
}

TEST_CASE("async io: fire-and-forget requests keep their state alive until the pool is done with it") {
    // Regression: the pool decremented the request's counter after destroying the job, and the job
    // held the last reference to the request state when the caller dropped the handle, so the
    // decrement wrote freed memory (valgrind/ASan: invalid write in BackgroundPool::workerMain).
    TempFile file(4096);
    std::atomic<int> completed{0};
    {
        jobs::BackgroundPool pool(2, "IO");
        for (int i = 0; i < 200; ++i) {
            fs::AsyncReadOptions options;
            options.onComplete = [&](fs::AsyncRead& done) {
                if (done.bytesRead().valueOr(0) == file.bytes.size()) completed.fetch_add(1);
            };
            (void)fs::readFileAsync(pool, file.path, options); // handle dropped at once
        }
        std::vector<u8> sink(64 * 512); // one slice per request (no two reads share memory)
        Result<fs::File> f = fs::File::open(file.path, fs::OpenMode::Read);
        REQUIRE(f.ok());
        for (usize i = 0; i < 64; ++i) {
            (void)fs::readAsync(pool, *f, 0, std::span<u8>(sink.data() + i * 512, 512)); // dropped as well
        }
        pool.waitIdle();
    }
    CHECK(completed.load() == 200);
}

TEST_CASE("async io: onComplete may take the result (no self-deadlock on the IO thread)") {
    // Regression: take()/bytesRead() waited for the request counter, which only reaches zero after
    // onComplete returns, so calling them from onComplete (the natural way to hand the bytes on)
    // blocked the IO thread forever.
    TempFile file(10000);
    // Heap-allocated and leaked if the callback never finishes: destroying a pool whose thread is
    // stuck would hang the test instead of failing it.
    auto* pool = new jobs::BackgroundPool(1, "IO");
    ManualResetEvent done;
    std::vector<u8> got;
    Result<usize> count = usize{0};
    fs::AsyncReadOptions options;
    options.onComplete = [&](fs::AsyncRead& r) {
        count = r.bytesRead();
        Result<std::vector<u8>> bytes = r.take();
        if (bytes) got = std::move(*bytes);
        done.set();
    };
    fs::AsyncRead read = fs::readFileAsync(*pool, file.path, options);
    REQUIRE(done.waitFor(std::chrono::seconds(20)));
    pool->waitIdle();
    delete pool;
    CHECK(read.counter().isDone() == true);
    REQUIRE(count.ok());
    CHECK(*count == file.bytes.size());
    CHECK(got == file.bytes);
    CHECK(read.take().errorCode() == ErrorCode::InvalidState); // already taken inside the callback
}

} // namespace
