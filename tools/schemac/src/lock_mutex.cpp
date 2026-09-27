#include "lock_mutex.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/fs.h"
#include "helios/core/types.h"

namespace helios::schemac {

namespace {

namespace stdfs = std::filesystem;
using Clock = std::chrono::steady_clock;
using FailingSince = std::optional<Clock::time_point>; // start of an unbroken run of transient errors

// Errors that other runs' create/remove cycles cause for a moment. They are retried (for at most
// LockMutexTiming::transientFor, or releaseFor when removing) instead of failing the build:
// - no_such_file_or_directory: MSVC's create_directory() gets ERROR_ALREADY_EXISTS from
//   CreateDirectoryW, then checks with GetFileAttributesExW that the path is a directory. If the
//   holder removed it in between, that check fails with ERROR_FILE_NOT_FOUND and the STL returns
//   it (CI run 36292817769: "cannot create '...writing': The system cannot find the file
//   specified."). Also a parent directory removed under us, which is re-created.
// - file_exists: the same race in libstdc++ and libc++, which report mkdir()'s EEXIST when their
//   follow-up is-a-directory check finds nothing. Something in the way is detected separately.
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

/// True if something other than a directory occupies `path`: a file, or a symlink even if it
/// dangles. create_directory() then fails for good but reports what the race reports (file_exists,
/// or no_such_file_or_directory from MSVC for a dangling symlink).
bool occupiedByNonDirectory(const stdfs::path& path) {
    std::error_code ec;
    const stdfs::file_status st = stdfs::symlink_status(path, ec);
    return !ec && stdfs::exists(st) && !stdfs::is_directory(st);
}

std::chrono::milliseconds nextDelay(std::chrono::milliseconds delay, const LockMutexTiming& timing) {
    return std::min(delay * 2, timing.maxDelay);
}

void appendCannotCreate(std::string& err, const stdfs::path& dir, std::string_view why) {
    err += std::format("helios-schemac: error: cannot create '{}': {}\n", fs::pathToUtf8(dir), why);
}

/// The protocol's directories for one lock file.
struct Dirs {
    stdfs::path mutex;     ///< `<lock>.writing`: held while a run reads and rewrites the lock.
    stdfs::path takeover;  ///< `<lock>.writing-takeover`: held while removing a stale mutex.
    stdfs::path takeover2; ///< `<lock>.writing-takeover2`: held while removing a stale takeover.
};

Dirs dirsOf(const std::string& lockPath) {
    return {fs::pathFromUtf8(lockPath + ".writing"), fs::pathFromUtf8(lockPath + ".writing-takeover"),
            fs::pathFromUtf8(lockPath + ".writing-takeover2")};
}

enum class Created : u8 {
    Yes,    ///< The caller now holds the directory.
    Exists, ///< Another run holds it, or one left it behind.
    Retry,  ///< A transient error; try again later.
    Failed, ///< A permanent error, appended to the error text.
};

enum class Takeover : u8 {
    None,     ///< Nothing removed; poll again after the back-off.
    Progress, ///< A stale directory was removed; try again at once.
    Failed,   ///< A permanent error, appended to the error text.
};

/// The file-system steps of the protocol for one acquire() or release(), with that object's
/// timing, file ops and error text.
class Protocol {
public:
    Protocol(const LockMutexTiming& timing, const LockMutexFileOps& ops, std::string& err)
        : m_timing(timing), m_ops(ops), m_err(err) {}

    /// Creates `dir`. A transient error may persist for at most transientFor, tracked in `since`
    /// (which a success or an existing directory resets); after that, or for any other error, it
    /// fails with a message naming `dir`.
    Created create(const stdfs::path& dir, FailingSince& since) {
        std::error_code ec;
        if (m_ops.createDirectory(dir, ec)) {
            since.reset();
            return Created::Yes;
        }
        if (!ec) {
            since.reset();
            return Created::Exists;
        }
        if (!isTransient(ec)) {
            appendCannotCreate(m_err, dir, ec.message());
            return Created::Failed;
        }
        const bool maybeInTheWay = ec == std::errc::file_exists || ec == std::errc::no_such_file_or_directory;
        if (maybeInTheWay && occupiedByNonDirectory(dir)) {
            appendCannotCreate(m_err, dir, "a file of that name is in the way (remove it)");
            return Created::Failed;
        }
        if (ec == std::errc::no_such_file_or_directory) ensureParent(dir);
        const Clock::time_point now = Clock::now();
        if (!since) since = now;
        if (now - *since > m_timing.transientFor) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *since).count();
            appendCannotCreate(m_err, dir, std::format("{} (still failing after {} ms)", ec.message(), ms));
            return Created::Failed;
        }
        return Created::Retry;
    }

    /// A new lock may live in a directory that does not exist yet, and one may be removed under us.
    static void ensureParent(const stdfs::path& dir) {
        std::error_code ignored;
        if (dir.has_parent_path()) stdfs::create_directories(dir.parent_path(), ignored);
    }

    /// Older than staleAfter (see LockMutexTiming for what that assumes). Missing is not stale.
    bool isStale(const stdfs::path& dir) {
        std::error_code ec;
        const stdfs::file_time_type modified = m_ops.lastWriteTime(dir, ec);
        return !ec && stdfs::file_time_type::clock::now() - modified > m_timing.staleAfter;
    }

    /// Removes `dir`, retrying transient errors for up to releaseFor. Already gone counts as removed.
    bool removeRetrying(const stdfs::path& dir, std::error_code& ec) {
        const Clock::time_point start = Clock::now();
        std::chrono::milliseconds delay = m_timing.firstDelay;
        for (;;) {
            if (m_ops.remove(dir, ec) || !ec) return true;
            if (!isTransient(ec) || Clock::now() - start > m_timing.releaseFor) return false;
            std::this_thread::sleep_for(delay);
            delay = nextDelay(delay, m_timing);
        }
    }

    /// Removes a directory this run holds, with a warning if it stays behind: it then blocks the
    /// others until it is stale, and is taken over as described below.
    void release(const stdfs::path& dir) {
        std::error_code ec;
        if (removeRetrying(dir, ec)) return;
        using std::chrono::seconds;
        const auto staleS = std::chrono::duration_cast<seconds>(m_timing.staleAfter).count();
        m_err += std::format("helios-schemac: warning: cannot remove '{}': {} (later runs wait until it is "
                             "{} s old; remove it)\n",
                             fs::pathToUtf8(dir), ec.message(), staleS);
    }

    // A stale directory may be removed only if the directory checked is the directory removed.
    // Two waiters that both saw the stale mutex must not both remove "it": the second would remove
    // the fresh mutex the first had just created, and both would hold the lock. So the check is
    // repeated, and the removal done, only while holding `takeover`, which one waiter at a time
    // holds (see reapStaleTakeover). While the stale mutex exists nobody can create a new one, and
    // nobody else removes it (its owner is gone, by the staleAfter assumption), so what the holder
    // checked is what it removes.
    Takeover takeOverStaleMutex(const Dirs& d) {
        switch (create(d.takeover, m_takeoverSince)) {
        case Created::Failed:
            return Takeover::Failed;
        case Created::Retry:
            return Takeover::None;
        case Created::Exists:
            // Another waiter is taking over, or a run left `takeover` behind: it was killed within
            // the few calls below, or release() could not remove it and warned. Only a stale one
            // is reaped.
            return isStale(d.takeover) ? reapStaleTakeover(d) : Takeover::None;
        case Created::Yes:
            break;
        }
        const Takeover result = removeIfStale(d.mutex, "a killed run left it behind");
        release(d.takeover);
        return result;
    }

private:
    // A stale `takeover` is reaped the same way one level up, while holding `takeover2`, so two
    // waiters cannot both reap it: the second would remove the first's fresh `takeover`, and two
    // waiters could then both remove "the" stale mutex. A stale `takeover2` is never reaped, since
    // that would need a third level. It is left behind only by a run killed, or unable to remove
    // it, within the few calls below, which run only after a `takeover` was itself left behind;
    // a person removes it.
    Takeover reapStaleTakeover(const Dirs& d) {
        switch (create(d.takeover2, m_takeover2Since)) {
        case Created::Failed:
            return Takeover::Failed;
        case Created::Retry:
            return Takeover::None;
        case Created::Exists:
            if (!isStale(d.takeover2)) return Takeover::None; // another waiter is reaping `takeover`
            m_err += std::format("helios-schemac: error: '{}' was left behind by a run that was recovering "
                                 "the lock from a killed run; delete it by hand once no build is running\n",
                                 fs::pathToUtf8(d.takeover2));
            return Takeover::Failed;
        case Created::Yes:
            break;
        }
        const Takeover result = removeIfStale(d.takeover, "a run left it behind");
        release(d.takeover2);
        return result;
    }

    /// Re-checks, under the next level's directory, that `dir` is stale, and removes it.
    Takeover removeIfStale(const stdfs::path& dir, std::string_view leftBy) {
        if (!isStale(dir)) return Takeover::None; // taken over, or removed, since the caller looked
        std::error_code ec;
        if (m_ops.remove(dir, ec)) return Takeover::Progress;
        if (!ec || isTransient(ec)) return Takeover::None; // polled again after the back-off
        appendCannotCreate(m_err, dir, std::format("{} and it cannot be removed: {}", leftBy, ec.message()));
        return Takeover::Failed;
    }

    const LockMutexTiming& m_timing;
    const LockMutexFileOps& m_ops;
    std::string& m_err;
    FailingSince m_takeoverSince;
    FailingSince m_takeover2Since;
};

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
    const Dirs dirs = dirsOf(lockPath);
    Protocol protocol(m_timing, m_ops, err);
    Protocol::ensureParent(dirs.mutex);
    const Clock::time_point start = Clock::now();
    FailingSince mutexSince;
    std::chrono::milliseconds delay = m_timing.firstDelay;
    for (;;) {
        bool retryNow = false;
        switch (protocol.create(dirs.mutex, mutexSince)) {
        case Created::Yes:
            m_dir = dirs.mutex;
            return true;
        case Created::Failed:
            return false;
        case Created::Retry:
            break;
        case Created::Exists: // another run holds it
            if (protocol.isStale(dirs.mutex)) {
                const Takeover takeover = protocol.takeOverStaleMutex(dirs);
                if (takeover == Takeover::Failed) return false;
                retryNow = takeover == Takeover::Progress;
            }
            break;
        }
        if (Clock::now() - start > m_timing.giveUpAfter) {
            err += std::format("helios-schemac: error: timed out waiting for '{}' (another helios-schemac is "
                               "updating the lock; remove the directory if none is running)\n",
                               fs::pathToUtf8(dirs.mutex));
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
    Protocol(m_timing, m_ops, err).release(std::exchange(m_dir, {}));
}

} // namespace helios::schemac
