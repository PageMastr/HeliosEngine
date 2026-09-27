#include "lock_mutex.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/fs.h"

namespace helios::schemac {

namespace {

using Clock = std::chrono::steady_clock;

// Errors that other runs' create/remove cycles cause for a moment. acquire() and release() retry
// them (for at most LockMutexTiming::transientFor / releaseFor) instead of failing the build:
// - no_such_file_or_directory: MSVC's create_directory() gets ERROR_ALREADY_EXISTS from
//   CreateDirectoryW, then checks with GetFileAttributesExW that the path is a directory. If the
//   holder removed it in between, that check fails with ERROR_FILE_NOT_FOUND and the STL returns
//   it (CI run 36292817769: "cannot create '...writing': The system cannot find the file
//   specified."). Also a parent directory removed under us, which acquire() re-creates.
// - file_exists: the same race in libstdc++ and libc++, which report mkdir()'s EEXIST when their
//   follow-up is-a-directory check finds nothing. A file in the way is detected separately.
// - permission_denied: Windows reports ERROR_ACCESS_DENIED for a "delete pending" directory
//   (removed while another handle to it is still open), and the STLs map ERROR_SHARING_VIOLATION
//   (an indexer or virus scanner holding it) to the same condition.
// - busy, try-again, lock-violation and EINTR results.
bool isTransient(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::file_exists ||
           ec == std::errc::permission_denied || ec == std::errc::device_or_resource_busy ||
           ec == std::errc::resource_unavailable_try_again || ec == std::errc::no_lock_available ||
           ec == std::errc::interrupted;
}

/// True if something other than a directory occupies `path`; create_directory() then reports
/// file_exists for good rather than because of a race.
bool occupiedByNonDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(path, ec);
    return !ec && std::filesystem::exists(st) && !std::filesystem::is_directory(st);
}

std::chrono::milliseconds nextDelay(std::chrono::milliseconds delay, const LockMutexTiming& timing) {
    return std::min(delay * 2, timing.maxDelay);
}

} // namespace

LockFileMutex::LockFileMutex() : LockFileMutex(LockMutexTiming{}) {}

LockFileMutex::LockFileMutex(const LockMutexTiming& timing, LockMutexFileOps ops)
    : m_timing(timing), m_ops(std::move(ops)) {
    if (!m_ops.createDirectory) {
        m_ops.createDirectory = [](const std::filesystem::path& p, std::error_code& ec) {
            return std::filesystem::create_directory(p, ec);
        };
    }
    if (!m_ops.remove) {
        m_ops.remove = [](const std::filesystem::path& p, std::error_code& ec) {
            return std::filesystem::remove(p, ec);
        };
    }
    if (!m_ops.lastWriteTime) {
        m_ops.lastWriteTime = [](const std::filesystem::path& p, std::error_code& ec) {
            return std::filesystem::last_write_time(p, ec);
        };
    }
}

LockFileMutex::~LockFileMutex() {
    std::string ignored;
    release(ignored);
}

bool LockFileMutex::acquire(const std::string& lockPath, std::string& err) {
    HELIOS_ASSERT(!held(), "LockFileMutex::acquire() while already held");
    const std::filesystem::path dir = fs::pathFromUtf8(lockPath + ".writing");
    const std::filesystem::path takeover = fs::pathFromUtf8(lockPath + ".writing-takeover");
    const std::filesystem::path parent = dir.parent_path();
    const auto ensureParent = [&parent] {
        std::error_code ignored; // a new lock may live in a directory that does not exist yet
        if (!parent.empty()) std::filesystem::create_directories(parent, ignored);
    };
    const auto fail = [&err, &dir](std::string_view why) {
        err += std::format("helios-schemac: error: cannot create '{}': {}\n", fs::pathToUtf8(dir), why);
        return false;
    };
    ensureParent();
    const Clock::time_point start = Clock::now();
    std::optional<Clock::time_point> failingSince; // start of an unbroken run of transient errors
    std::chrono::milliseconds delay = m_timing.firstDelay;
    for (;;) {
        std::error_code ec;
        if (m_ops.createDirectory(dir, ec)) {
            m_dir = dir;
            return true;
        }
        const Clock::time_point now = Clock::now();
        bool retryNow = false;
        if (!ec) { // another run holds it
            failingSince.reset();
            if (isStale(dir)) {
                std::error_code removeEc;
                retryNow = takeOverStale(dir, takeover, removeEc);
                if (removeEc && !isTransient(removeEc)) {
                    return fail(std::format("a killed run left it behind and it cannot be removed: {}",
                                            removeEc.message()));
                }
            }
        } else if (isTransient(ec)) {
            if (ec == std::errc::file_exists && occupiedByNonDirectory(dir)) {
                return fail("a file of that name is in the way (remove it)");
            }
            if (ec == std::errc::no_such_file_or_directory) ensureParent();
            if (!failingSince) failingSince = now;
            if (now - *failingSince > m_timing.transientFor) {
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - *failingSince).count();
                return fail(std::format("{} (still failing after {} ms)", ec.message(), ms));
            }
        } else {
            return fail(ec.message());
        }
        if (now - start > m_timing.giveUpAfter) {
            err += std::format("helios-schemac: error: timed out waiting for '{}' (another helios-schemac is "
                               "updating the lock; remove the directory if none is running)\n",
                               fs::pathToUtf8(dir));
            return false;
        }
        if (!retryNow) {
            std::this_thread::sleep_for(delay);
            delay = nextDelay(delay, m_timing);
        }
    }
}

void LockFileMutex::release(std::string& err) {
    if (m_dir.empty()) return;
    const std::filesystem::path dir = std::exchange(m_dir, {});
    std::error_code ec;
    if (removeRetrying(dir, ec)) return;
    const auto staleSeconds = std::chrono::duration_cast<std::chrono::seconds>(m_timing.staleAfter).count();
    err += std::format("helios-schemac: warning: cannot remove '{}': {} (later runs wait until it is {} s "
                       "old; remove it)\n",
                       fs::pathToUtf8(dir), ec.message(), staleSeconds);
}

bool LockFileMutex::isStale(const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::file_time_type modified = m_ops.lastWriteTime(dir, ec);
    return !ec && std::filesystem::file_time_type::clock::now() - modified > m_timing.staleAfter;
}

// Two waiters that both saw the stale mutex must not both remove "it": the second would remove
// the fresh mutex the first had just created, and both would hold the lock. So the check and the
// removal happen while holding a second directory, `takeover`. While a stale mutex exists nobody
// can create a new one, and only the takeover holder removes it (its owner is dead: a live run
// holds the mutex for milliseconds, far below staleAfter), so the directory checked is the one
// removed. Returns true if it removed `dir`; `ec` is the error removing it, if any.
bool LockFileMutex::takeOverStale(const std::filesystem::path& dir, const std::filesystem::path& takeover,
                                  std::error_code& ec) {
    ec.clear();
    std::error_code takeoverEc;
    if (!m_ops.createDirectory(takeover, takeoverEc)) {
        // Another waiter is taking over. A takeover directory is left behind only by a run killed
        // within the few calls below; it goes stale like the mutex.
        if (!takeoverEc && isStale(takeover)) m_ops.remove(takeover, takeoverEc);
        return false;
    }
    const bool removed = isStale(dir) && m_ops.remove(dir, ec);
    std::error_code ignored; // if it stays behind, it goes stale and is removed like the mutex
    removeRetrying(takeover, ignored);
    return removed;
}

bool LockFileMutex::removeRetrying(const std::filesystem::path& dir, std::error_code& ec) {
    const Clock::time_point start = Clock::now();
    std::chrono::milliseconds delay = m_timing.firstDelay;
    for (;;) {
        // false without an error: already gone (a waiter took it over as stale).
        if (m_ops.remove(dir, ec) || !ec) return true;
        if (!isTransient(ec) || Clock::now() - start > m_timing.releaseFor) return false;
        std::this_thread::sleep_for(delay);
        delay = nextDelay(delay, m_timing);
    }
}

} // namespace helios::schemac
