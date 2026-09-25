// POSIX crash handler: fatal signals -> "<dir>/<app>-<unix time>-<pid>-<seq>.txt" with signal info
// and a backtrace (glibc only), then the signal is re-raised with the previous disposition.
// Everything reachable from the signal handler is async-signal-safe (open/write/close/time/getpid,
// backtrace_symbols_fd after a warm-up call at install time).

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#define HELIOS_HAS_EXECINFO 1
#endif

#include "platform/os.h"

namespace helios::os {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr usize kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);
constexpr usize kPrefixCapacity = 2048;
constexpr usize kAltStackSize = 64 * 1024;

struct sigaction g_previous[kSignalCount];
char g_prefix[kPrefixCapacity]; // "<dir>/<app>-"
bool g_installed = false;
void* g_altStack = nullptr;
std::atomic<u32> g_sequence{0};
volatile std::sig_atomic_t g_inHandler = 0;

usize cstrLen(const char* s) noexcept {
    usize n = 0;
    while (s[n]) ++n;
    return n;
}

void writeAll(int fd, const char* data, usize size) noexcept {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data += n;
        size -= static_cast<usize>(n);
    }
}

void writeStr(int fd, const char* s) noexcept { writeAll(fd, s, cstrLen(s)); }

/// Appends the decimal representation of v to buf at pos (no allocation).
void appendUInt(char* buf, usize cap, usize& pos, u64 v) noexcept {
    char tmp[24];
    usize n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0 && pos + 1 < cap) buf[pos++] = tmp[--n];
    buf[pos] = '\0';
}

void appendStr(char* buf, usize cap, usize& pos, const char* s) noexcept {
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    buf[pos] = '\0';
}

void writeHex(int fd, u64 v) noexcept {
    char buf[19] = "0x";
    for (int i = 0; i < 16; ++i) buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = '\0';
    writeStr(fd, buf);
}

void writeUInt(int fd, u64 v) noexcept {
    char buf[24];
    usize pos = 0;
    appendUInt(buf, sizeof(buf), pos, v);
    writeStr(fd, buf);
}

const char* signalName(int sig) noexcept {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGFPE: return "SIGFPE (arithmetic exception)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "signal";
    }
}

/// Builds "<prefix><time>-<pid>-<seq>.txt" into `out`.
void buildReportPath(char* out, usize cap) noexcept {
    usize pos = 0;
    out[0] = '\0';
    appendStr(out, cap, pos, g_prefix);
    appendUInt(out, cap, pos, static_cast<u64>(std::time(nullptr)));
    appendStr(out, cap, pos, "-");
    appendUInt(out, cap, pos, static_cast<u64>(getpid()));
    appendStr(out, cap, pos, "-");
    appendUInt(out, cap, pos, g_sequence.fetch_add(1, std::memory_order_relaxed));
    appendStr(out, cap, pos, ".txt");
}

void writeBacktrace(int fd) noexcept {
#if defined(HELIOS_HAS_EXECINFO)
    void* frames[128];
    const int n = backtrace(frames, 128);
    writeStr(fd, "backtrace:\n");
    backtrace_symbols_fd(frames, n, fd);
#else
    writeStr(fd, "backtrace: unavailable on this C library\n");
#endif
}

void crashSignalHandler(int sig, siginfo_t* info, void*) {
    if (g_inHandler) {
        // Crashed while reporting: give up immediately with the default action.
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    g_inHandler = 1;

    char path[kPrefixCapacity + 64];
    buildReportPath(path, sizeof(path));
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        writeStr(fd, "Helios crash report\nsignal: ");
        writeUInt(fd, static_cast<u64>(sig));
        writeStr(fd, " ");
        writeStr(fd, signalName(sig));
        writeStr(fd, "\nfault address: ");
        writeHex(fd, info ? reinterpret_cast<u64>(info->si_addr) : 0);
        writeStr(fd, "\npid: ");
        writeUInt(fd, static_cast<u64>(getpid()));
        writeStr(fd, "\n");
        writeBacktrace(fd);
        ::close(fd);
    }
    writeStr(STDERR_FILENO, "Helios: fatal ");
    writeStr(STDERR_FILENO, signalName(sig));
    writeStr(STDERR_FILENO, "; crash report: ");
    writeStr(STDERR_FILENO, path);
    writeStr(STDERR_FILENO, "\n");

    // Chain to the previous disposition (default action, sanitizer, debugger helper...).
    for (usize i = 0; i < kSignalCount; ++i) {
        if (kSignals[i] == sig) sigaction(sig, &g_previous[i], nullptr);
    }
    std::raise(sig); // delivered when this handler returns (the signal is blocked meanwhile)
}

} // namespace

Result<void> installCrashHandler(const std::filesystem::path& dumpDir, const CrashHandlerOptions& options) {
    std::string prefix = dumpDir.string();
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    prefix.append(options.appName.empty() ? "helios" : options.appName);
    prefix.push_back('-');
    if (prefix.size() + 1 > kPrefixCapacity) return Error{ErrorCode::InvalidArgument, "crash dump path too long"};
    std::memcpy(g_prefix, prefix.c_str(), prefix.size() + 1);

    if (g_installed) return {}; // directory/options updated in place

#if defined(HELIOS_HAS_EXECINFO)
    // backtrace() may allocate on first use (it loads libgcc); do that now, not in the handler.
    void* warmup[4];
    (void)backtrace(warmup, 4);
#endif

    // Alternate stack so stack overflows on this (main) thread can still be reported.
    g_altStack = std::malloc(kAltStackSize);
    if (g_altStack) {
        stack_t ss{};
        ss.ss_sp = g_altStack;
        ss.ss_size = kAltStackSize;
        ss.ss_flags = 0;
        sigaltstack(&ss, nullptr);
    }

    struct sigaction sa{};
    sa.sa_sigaction = &crashSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (usize i = 0; i < kSignalCount; ++i) {
        if (sigaction(kSignals[i], &sa, &g_previous[i]) != 0) return lastError("sigaction");
    }
    g_installed = true;
    return {};
}

void uninstallCrashHandler() noexcept {
    if (!g_installed) return;
    for (usize i = 0; i < kSignalCount; ++i) sigaction(kSignals[i], &g_previous[i], nullptr);
    g_installed = false;
    // The alternate stack stays registered (and allocated): freeing it while registered is unsafe.
}

Result<std::filesystem::path> writeCrashReport(std::string_view reason) {
    char path[kPrefixCapacity + 64];
    buildReportPath(path, sizeof(path));
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return lastError(std::format("open '{}'", path));
    writeStr(fd, "Helios crash report (requested)\nreason: ");
    writeAll(fd, reason.data(), reason.size());
    writeStr(fd, "\npid: ");
    writeUInt(fd, static_cast<u64>(getpid()));
    writeStr(fd, "\n");
    writeBacktrace(fd);
    ::close(fd);
    return std::filesystem::path(path);
}

} // namespace helios::os
