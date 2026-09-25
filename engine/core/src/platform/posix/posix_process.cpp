// POSIX child processes and pipes (process.h): posix_spawn with file actions for the standard
// streams, the inherit-handle whitelist and the working directory.
//
// Every descriptor Helios creates is O_CLOEXEC, and whitelisted descriptors are re-enabled in the
// child only with posix_spawn_file_actions_adddup2(fd, fd) (POSIX.1-2024; glibc >= 2.29, musl >=
// 1.1.24). Descriptors that other code opened without O_CLOEXEC (sockets, fopen, third-party
// libraries) are closed in the child as well: close actions for the gaps between whitelisted
// descriptors plus posix_spawn_file_actions_addclosefrom_np (glibc >= 2.34), or
// POSIX_SPAWN_CLOEXEC_DEFAULT on macOS. The working directory uses
// posix_spawn_file_actions_addchdir_np (glibc >= 2.29, musl >= 1.1.24, macOS 10.15+).

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "helios/core/time.h"
#include "platform/os.h"

extern char** environ;

#if defined(__GLIBC__)
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29)
#define HELIOS_HAVE_SPAWN_CHDIR 1
#endif
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)
#define HELIOS_HAVE_SPAWN_CLOSEFROM 1
#endif
#elif defined(__APPLE__)
#define HELIOS_HAVE_SPAWN_CHDIR 1
#endif

namespace helios::os {

namespace {

Error errnoError(int e, std::string_view context) {
    errno = e;
    return lastError(context);
}

#if !defined(__linux__)
Result<void> setCloexec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return lastError("fcntl(FD_CLOEXEC)");
    return {};
}
#endif

// Owns the descriptors of one spawn attempt; closes whatever is left on every exit path.
struct FdSet {
    std::vector<int> fds;
    ~FdSet() {
        for (int fd : fds) {
            if (fd >= 0) ::close(fd);
        }
    }
    int take(int fd) {
        for (int& f : fds) {
            if (f == fd) f = -1;
        }
        return fd;
    }
};

struct FileActions {
    posix_spawn_file_actions_t fa;
    bool ok = false;
    FileActions() { ok = posix_spawn_file_actions_init(&fa) == 0; }
    ~FileActions() {
        if (ok) posix_spawn_file_actions_destroy(&fa);
    }
};

struct SpawnAttr {
    posix_spawnattr_t attr;
    bool ok = false;
    SpawnAttr() { ok = posix_spawnattr_init(&attr) == 0; }
    ~SpawnAttr() {
        if (ok) posix_spawnattr_destroy(&attr);
    }
};

bool envNameMatches(std::string_view entry, std::string_view name) {
    return entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 && entry[name.size()] == '=';
}

std::vector<std::string> buildEnvironment(const ProcessDesc& desc) {
    std::vector<std::string> env;
    if (desc.inheritEnvironment && environ) {
        for (char** e = environ; *e; ++e) env.emplace_back(*e);
    }
    auto removeName = [&](std::string_view name) {
        std::erase_if(env, [&](const std::string& entry) { return envNameMatches(entry, name); });
    };
    for (const std::string& name : desc.unsetEnvironment) removeName(name);
    for (const auto& [name, value] : desc.environment) {
        removeName(name);
        env.push_back(name + "=" + value);
    }
    return env;
}

i32 exitCodeFromStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

// ---- pipes ------------------------------------------------------------------------------------

Result<void> pipeCreate(NativeHandle& readEnd, NativeHandle& writeEnd) {
    int fds[2];
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) return lastError("pipe2");
#else
    if (::pipe(fds) != 0) return lastError("pipe");
    if (auto r = setCloexec(fds[0]); !r) return r;
    if (auto r = setCloexec(fds[1]); !r) return r;
#endif
    readEnd = fds[0];
    writeEnd = fds[1];
    return {};
}

void handleClose(NativeHandle handle) noexcept {
    if (handle >= 0) ::close(static_cast<int>(handle));
}

Result<usize> pipeRead(NativeHandle handle, void* buffer, usize size) {
    for (;;) {
        const ssize_t n = ::read(static_cast<int>(handle), buffer, size);
        if (n >= 0) return static_cast<usize>(n);
        if (errno != EINTR) return lastError("read(pipe)");
    }
}

Result<usize> pipeWrite(NativeHandle handle, const void* data, usize size) {
    // SIGPIPE would kill the process when the reader is gone; ignore it for this thread's write by
    // blocking it and consuming a pending one (portable alternative to MSG_NOSIGNAL for pipes).
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    ssize_t n;
    do {
        n = ::write(static_cast<int>(handle), data, size);
    } while (n < 0 && errno == EINTR);
    const int writeErrno = errno;
    if (n < 0 && writeErrno == EPIPE) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE)) {
            const timespec zero{};
            sigtimedwait(&block, nullptr, &zero);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    if (n < 0) return errnoError(writeErrno, "write(pipe)");
    return static_cast<usize>(n);
}

// ---- processes ----------------------------------------------------------------------------------

Result<SpawnedProcess> processSpawn(const ProcessDesc& desc) {
    FileActions actions;
    SpawnAttr attr;
    if (!actions.ok || !attr.ok) return Error{ErrorCode::OutOfMemory, "posix_spawn: initialization failed"};

    FdSet owned;
    SpawnedProcess out;
    int childIn = -1, childOut = -1, childErr = -1;
    auto makePipe = [&](int& parentEnd, int& childEnd, bool parentReads) -> Result<void> {
        NativeHandle r = -1, w = -1;
        if (auto res = pipeCreate(r, w); !res) return res;
        owned.fds.push_back(static_cast<int>(r));
        owned.fds.push_back(static_cast<int>(w));
        // A parent whose standard streams are closed (a daemon) gets pipe descriptors 0-2. The
        // file actions below would then overwrite one before duplicating it (e.g. /dev/null opened
        // onto fd 1 while fd 1 is still the source of the stderr dup2), so keep them above 2.
        for (NativeHandle* fd : {&r, &w}) {
            if (*fd > 2) continue;
            const int moved = ::fcntl(static_cast<int>(*fd), F_DUPFD_CLOEXEC, 3);
            if (moved < 0) return lastError("fcntl(F_DUPFD_CLOEXEC)");
            owned.fds.push_back(moved);
            ::close(owned.take(static_cast<int>(*fd)));
            *fd = moved;
        }
        parentEnd = static_cast<int>(parentReads ? r : w);
        childEnd = static_cast<int>(parentReads ? w : r);
        return {};
    };
    int parentIn = -1, parentOut = -1, parentErr = -1;
    if (desc.stdinMode == StdioMode::Pipe) {
        if (auto r = makePipe(parentIn, childIn, false); !r) return r.error();
    }
    if (desc.stdoutMode == StdioMode::Pipe) {
        if (auto r = makePipe(parentOut, childOut, true); !r) return r.error();
    }
    if (!desc.mergeStderrIntoStdout && desc.stderrMode == StdioMode::Pipe) {
        if (auto r = makePipe(parentErr, childErr, true); !r) return r.error();
    }

    posix_spawn_file_actions_t* fa = &actions.fa;
    int rc = 0;
    auto check = [&](int result) {
        if (rc == 0) rc = result;
    };
    // stdin
    if (desc.stdinMode == StdioMode::Pipe) check(posix_spawn_file_actions_adddup2(fa, childIn, 0));
    if (desc.stdinMode == StdioMode::Null) check(posix_spawn_file_actions_addopen(fa, 0, "/dev/null", O_RDONLY, 0));
    // stdout
    if (desc.stdoutMode == StdioMode::Pipe) check(posix_spawn_file_actions_adddup2(fa, childOut, 1));
    if (desc.stdoutMode == StdioMode::Null) check(posix_spawn_file_actions_addopen(fa, 1, "/dev/null", O_WRONLY, 0));
    // stderr
    if (desc.mergeStderrIntoStdout) {
        check(posix_spawn_file_actions_adddup2(fa, 1, 2));
    } else if (desc.stderrMode == StdioMode::Pipe) {
        check(posix_spawn_file_actions_adddup2(fa, childErr, 2));
    } else if (desc.stderrMode == StdioMode::Null) {
        check(posix_spawn_file_actions_addopen(fa, 2, "/dev/null", O_WRONLY, 0));
    }
    // Whitelisted descriptors: dup2 onto themselves clears FD_CLOEXEC in the child only.
    std::vector<int> whitelist;
    for (NativeHandle h : desc.inheritHandles) {
        if (h < 0 || h > 0x7fffffff || ::fcntl(static_cast<int>(h), F_GETFD) == -1) {
            return Error{ErrorCode::InvalidArgument, "Process::spawn: invalid handle in inheritHandles"};
        }
        if (h <= 2) continue; // standard streams are inherited anyway
        whitelist.push_back(static_cast<int>(h));
        check(posix_spawn_file_actions_adddup2(fa, static_cast<int>(h), static_cast<int>(h)));
    }
    std::sort(whitelist.begin(), whitelist.end());
    whitelist.erase(std::unique(whitelist.begin(), whitelist.end()), whitelist.end());
#if defined(HELIOS_HAVE_SPAWN_CLOSEFROM)
    // Close everything else above 2 in the child (after the dup2 actions above, which have already
    // consumed the pipe ends): the gaps between whitelisted descriptors one by one (a close of a
    // descriptor that is not open is ignored), then everything above the highest one.
    {
        constexpr int kMaxGapCloses = 1 << 16;
        int next = 3;
        for (int fd : whitelist) {
            if (fd - next > kMaxGapCloses) {
                next = fd + 1; // absurdly high whitelisted descriptor: leave the gap to O_CLOEXEC
                continue;
            }
            for (; next < fd; ++next) check(posix_spawn_file_actions_addclose(fa, next));
            next = fd + 1;
        }
        check(posix_spawn_file_actions_addclosefrom_np(fa, next));
    }
#endif
    const std::string workingDir = desc.workingDirectory.empty() ? std::string() : desc.workingDirectory.string();
    if (!workingDir.empty()) {
#if defined(HELIOS_HAVE_SPAWN_CHDIR)
        check(posix_spawn_file_actions_addchdir_np(fa, workingDir.c_str()));
#else
        return Error{ErrorCode::Unsupported, "Process::spawn: this C library cannot set a child's working directory"};
#endif
    }
    if (rc != 0) return errnoError(rc, "posix_spawn_file_actions");

    // The child starts with default signal dispositions and an empty signal mask, whatever the
    // parent (or its engine threads) blocked or ignored.
    sigset_t emptyMask, allSignals;
    sigemptyset(&emptyMask);
    sigfillset(&allSignals);
    check(posix_spawnattr_setsigmask(&attr.attr, &emptyMask));
    check(posix_spawnattr_setsigdefault(&attr.attr, &allSignals));
    short spawnFlags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // only descriptors named by file actions survive
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1) check(posix_spawn_file_actions_addinherit_np(fa, fd));
    }
#endif
    check(posix_spawnattr_setflags(&attr.attr, spawnFlags));
    if (rc != 0) return errnoError(rc, "posix_spawnattr");

    // A relative program path names a file relative to the parent's working directory on every
    // platform (CreateProcessW resolves it that way). posix_spawn would resolve it after the
    // child's chdir, so make it absolute first. Bare names looked up in PATH stay as they are.
    std::string program = desc.executable.string();
    const bool pathLookup = desc.searchPath && program.find('/') == std::string::npos;
    if (!pathLookup && !workingDir.empty() && desc.executable.is_relative()) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(desc.executable, ec);
        if (ec) return Error{ErrorCode::IoError, "Process::spawn: cannot resolve '" + program + "': " + ec.message()};
        program = absolute.string();
    }
    std::vector<std::string> argStorage;
    argStorage.reserve(desc.args.size() + 1);
    argStorage.push_back(desc.executable.string()); // argv[0] as given
    for (const std::string& a : desc.args) argStorage.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : argStorage) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStorage = buildEnvironment(desc);
    std::vector<char*> envp;
    for (std::string& e : envStorage) envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = 0;
    rc = desc.searchPath ? posix_spawnp(&pid, program.c_str(), fa, &attr.attr, argv.data(), envp.data())
                         : posix_spawn(&pid, program.c_str(), fa, &attr.attr, argv.data(), envp.data());
    if (rc != 0) return errnoError(rc, std::string("posix_spawn '") + program + "'");

    // The child has its copies; close ours and hand the parent ends to the Process.
    if (childIn >= 0) ::close(owned.take(childIn));
    if (childOut >= 0) ::close(owned.take(childOut));
    if (childErr >= 0) ::close(owned.take(childErr));
    out.pid = static_cast<u32>(pid);
    if (parentIn >= 0) out.stdinWrite = owned.take(parentIn);
    if (parentOut >= 0) out.stdoutRead = owned.take(parentOut);
    if (parentErr >= 0) out.stderrRead = owned.take(parentErr);
    return out;
}

Result<std::optional<i32>> processWait(NativeHandle, u32 pid, i64 timeoutMs) {
    const u64 start = monotonicNanos();
    u64 sleepNs = 50'000; // back off from 50 us to 5 ms
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, timeoutMs < 0 ? 0 : WNOHANG);
        if (r == static_cast<pid_t>(pid)) return std::optional<i32>(exitCodeFromStatus(status));
        if (r < 0) {
            if (errno == EINTR) continue;
            return lastError("waitpid");
        }
        if (timeoutMs >= 0) {
            const u64 elapsed = monotonicNanos() - start;
            const u64 limit = static_cast<u64>(timeoutMs) * 1'000'000ull;
            if (elapsed >= limit) return std::optional<i32>();
            sleepNanos(std::min(sleepNs, limit - elapsed));
            sleepNs = std::min<u64>(sleepNs * 2, 5'000'000);
        }
    }
}

Result<void> processKill(NativeHandle, u32 pid) {
    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0 && errno != ESRCH) return lastError("kill");
    return {};
}

void processClose(NativeHandle) noexcept {}

bool activeCodePageIsUtf8() noexcept { return true; }

NativeHandle standardHandle(int which) noexcept {
    return (which >= 0 && which <= 2 && fcntl(which, F_GETFD) != -1) ? which : kInvalidNativeHandle;
}

} // namespace helios::os
