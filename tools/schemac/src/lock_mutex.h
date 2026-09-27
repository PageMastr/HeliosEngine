#pragma once
// Cross-process mutex that serializes the helios-schemac runs rewriting one schema lock file.

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

namespace helios::schemac {

/// Timing of LockFileMutex. The defaults are the production values; tests shorten them.
struct LockMutexTiming {
    /// A protocol directory older than this was left behind by a killed run and is taken over.
    /// Staleness is decided by age alone, so a live holder must finish within staleAfter (a run
    /// holds the mutex for milliseconds). One suspended for longer, e.g. in a debugger or a paused
    /// VM, is treated as dead: another run takes the mutex over, and the suspended run's release
    /// then removes that run's mutex. The lock must be on a local file system: on a network share
    /// the times come from the server's clock, and skew beyond staleAfter makes every mutex stale.
    std::chrono::milliseconds staleAfter{std::chrono::seconds(120)};
    /// acquire() gives up after waiting this long in total.
    std::chrono::milliseconds giveUpAfter{std::chrono::seconds(300)};
    /// How long an unbroken run of transient errors (see lock_mutex.cpp) may last before acquire()
    /// reports it: long enough for a virus scanner or indexer to let go, short enough that a real
    /// permission problem is not reported only after giveUpAfter.
    std::chrono::milliseconds transientFor{std::chrono::seconds(10)};
    /// How long release() retries a transient failure to remove a directory it holds.
    std::chrono::milliseconds releaseFor{std::chrono::seconds(2)};
    /// Poll back-off while another run holds the mutex: starts at firstDelay, doubles up to maxDelay.
    std::chrono::milliseconds firstDelay{2};
    std::chrono::milliseconds maxDelay{100};
};

/// The file-system calls LockFileMutex makes; an empty member means the std::filesystem function
/// of the same name. Test seam: lets tests inject the results that a racing standard library
/// returns, which real runs hit only intermittently.
struct LockMutexFileOps {
    std::function<bool(const std::filesystem::path&, std::error_code&)> createDirectory;
    std::function<bool(const std::filesystem::path&, std::error_code&)> remove;
    std::function<std::filesystem::file_time_type(const std::filesystem::path&, std::error_code&)>
        lastWriteTime;
};

/// Cross-process mutex for a schema lock file. Several helios_schema() calls (possibly running in
/// parallel in one build) may share one lock, and each run reads, extends and rewrites it; without
/// mutual exclusion a concurrent run would overwrite another's new ids, tombstones and renames.
///
/// The mutex is the directory `<lock>.writing`: creating a directory is atomic on every OS, and the
/// holder removes it on release. One older than LockMutexTiming::staleAfter was left behind by a
/// killed run; it is removed by one waiter at a time, while holding `<lock>.writing-takeover`. A
/// stale `...-takeover` is removed the same way while holding `<lock>.writing-takeover2`, and a
/// stale `...-takeover2` (a second failure inside that recovery) stops with an error asking for it
/// to be deleted by hand. Under the staleAfter assumption (see there), at most one run holds the
/// mutex. Errors that the protocol itself causes for a moment (a directory removed between the OS
/// call and the standard library's follow-up check, a delete-pending directory on Windows) are
/// retried with a bounded back-off; every wait is bounded and ends in a clear error.
///
/// Threading: one object is used by one thread at a time. Objects in different threads or
/// processes that name the same lock exclude each other.
class LockFileMutex {
public:
    LockFileMutex();
    explicit LockFileMutex(const LockMutexTiming& timing, LockMutexFileOps ops = {});
    /// Releases the mutex if it is still held (without reporting a failure to remove it).
    ~LockFileMutex();
    LockFileMutex(const LockFileMutex&) = delete;
    LockFileMutex& operator=(const LockFileMutex&) = delete;

    /// Blocks until this object holds the mutex of the lock file `lockPath` (UTF-8) and returns
    /// true. Returns false after appending a "helios-schemac: error: ..." line to `err` if the
    /// mutex cannot be created or the wait times out. May append a "helios-schemac: warning: ..."
    /// line even when it succeeds. The mutex must not already be held.
    bool acquire(const std::string& lockPath, std::string& err);

    /// Releases the mutex; does nothing if it is not held. Appends a "helios-schemac: warning: ..."
    /// line to `err` if the directory cannot be removed (later runs then wait until it is stale).
    void release(std::string& err);

    /// Whether this object holds the mutex.
    bool held() const noexcept { return !m_dir.empty(); }

private:
    LockMutexTiming m_timing;
    LockMutexFileOps m_ops;
    std::filesystem::path m_dir; ///< The mutex directory while held, empty otherwise.
};

} // namespace helios::schemac
