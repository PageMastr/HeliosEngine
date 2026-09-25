// POSIX (Linux first) implementation of time, threads, process, memory, console, dynamic
// libraries, randomness and error helpers.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#include <sys/syscall.h>
#endif

#include "helios/core/cmdline.h"
#include "platform/os.h"

namespace helios::os {

// ---- errors ---------------------------------------------------------------------------------

namespace {
ErrorCode mapErrno(int e) noexcept {
    switch (e) {
    case ENOENT:
    case ENOTDIR: return ErrorCode::NotFound;
    case EACCES:
    case EPERM:
    case EROFS: return ErrorCode::PermissionDenied;
    case EEXIST: return ErrorCode::AlreadyExists;
    case EINVAL:
    case EISDIR:
    case ENAMETOOLONG: return ErrorCode::InvalidArgument;
    case ENOMEM: return ErrorCode::OutOfMemory;
    case EBUSY:
    case EAGAIN: return ErrorCode::Busy;
    case ETIMEDOUT: return ErrorCode::Timeout;
    case ENOSYS:
    case ENOTSUP: return ErrorCode::Unsupported;
    default: return ErrorCode::IoError;
    }
}
} // namespace

Error lastError(std::string_view context) {
    const int e = errno;
    return Error{mapErrno(e), std::format("{}: {} (errno {})", context, std::generic_category().message(e), e)};
}

// ---- time -----------------------------------------------------------------------------------

u64 monotonicNanos() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
}

u64 monotonicTicks() noexcept { return monotonicNanos(); }
u64 monotonicFrequency() noexcept { return 1'000'000'000ull; }

void sleepNanos(u64 nanos) noexcept {
    timespec req{};
    req.tv_sec = static_cast<time_t>(nanos / 1'000'000'000ull);
    req.tv_nsec = static_cast<long>(nanos % 1'000'000'000ull);
    timespec rem{};
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) req = rem;
}

void highResolutionSleepNanos(u64 nanos) noexcept { sleepNanos(nanos); }

u64 sleepSpinMarginNanos() noexcept { return 200'000; } // typical Linux timer slack + wakeup latency

// ---- threads / process ----------------------------------------------------------------------

u64 currentThreadId() noexcept {
#if defined(__linux__)
    return static_cast<u64>(syscall(SYS_gettid));
#else
    u64 id = 0;
    const pthread_t self = pthread_self();
    std::memcpy(&id, &self, sizeof(id) < sizeof(self) ? sizeof(id) : sizeof(self));
    return id;
#endif
}

void setCurrentThreadName(const std::string& name) noexcept {
#if defined(__linux__)
    // The kernel limit is 16 bytes including the terminator; cut on a UTF-8 boundary.
    usize n = name.size() < 15 ? name.size() : 15;
    while (n > 0 && n < name.size() && (static_cast<u8>(name[n]) & 0xC0) == 0x80) --n;
    char buffer[16] = {};
    std::memcpy(buffer, name.data(), n);
    pthread_setname_np(pthread_self(), buffer);
#elif defined(__APPLE__)
    pthread_setname_np(name.c_str());
#else
    (void)name;
#endif
}

bool setCurrentThreadAffinity(u64 mask) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (mask & (1ull << cpu)) CPU_SET(cpu, &set);
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)mask;
    return false;
#endif
}

bool setCurrentThreadPriority(ThreadPriority priority) noexcept {
#if defined(__linux__)
    // Linux applies nice values per thread (tid) for SCHED_OTHER threads.
    const int nice = priority == ThreadPriority::Low ? 5 : priority == ThreadPriority::High ? -5 : 0;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice) == 0;
#else
    (void)priority;
    return false;
#endif
}

u32 processorCount() noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        const int count = CPU_COUNT(&set);
        if (count > 0) return static_cast<u32>(count);
    }
#endif
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<u32>(n) : 1u;
}

u32 currentProcessId() noexcept { return static_cast<u32>(getpid()); }

std::vector<std::string> processArguments() {
    std::vector<std::string> args;
#if defined(__linux__)
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    usize start = 0;
    while (start < all.size()) {
        const usize end = all.find('\0', start);
        args.emplace_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
#endif
    return args;
}

// ---- memory ---------------------------------------------------------------------------------

usize pageSize() noexcept {
    static const usize size = [] {
        const long n = sysconf(_SC_PAGESIZE);
        return n > 0 ? static_cast<usize>(n) : usize(4096);
    }();
    return size;
}

usize allocationGranularity() noexcept { return pageSize(); }

void* vmReserve(usize size) noexcept {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* p = mmap(nullptr, size, PROT_NONE, flags, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool vmCommit(void* ptr, usize size) noexcept { return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0; }

bool vmDecommit(void* ptr, usize size) noexcept {
    // Drop the pages (private anonymous memory reads back as zero) and make the range inaccessible.
    if (madvise(ptr, size, MADV_DONTNEED) != 0) return false;
    return mprotect(ptr, size, PROT_NONE) == 0;
}

bool vmRelease(void* ptr, usize size) noexcept { return munmap(ptr, size) == 0; }

// ---- console / debugger ---------------------------------------------------------------------

bool consoleEnableColors(ConsoleStream stream) noexcept {
    const int fd = stream == ConsoleStream::Out ? STDOUT_FILENO : STDERR_FILENO;
    if (!isatty(fd)) return false;
    if (std::getenv("NO_COLOR") != nullptr) return false;
    const char* term = std::getenv("TERM");
    return term == nullptr || std::strcmp(term, "dumb") != 0;
}

void consoleWrite(ConsoleStream stream, std::string_view utf8) noexcept {
    const int fd = stream == ConsoleStream::Out ? STDOUT_FILENO : STDERR_FILENO;
    const char* p = utf8.data();
    usize left = utf8.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        left -= static_cast<usize>(n);
    }
}

bool isDebuggerPresent() noexcept {
#if defined(__linux__)
    // TracerPid in /proc/self/status is non-zero while a debugger (ptrace) is attached.
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[4096];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buffer[n] = '\0';
    const char* tracer = std::strstr(buffer, "TracerPid:");
    if (!tracer) return false;
    tracer += 10;
    while (*tracer == ' ' || *tracer == '\t') ++tracer;
    return *tracer != '0' && *tracer != '\0' && *tracer != '\n';
#else
    return false;
#endif
}

void debugOutput(std::string_view) noexcept {}

bool getEnv(const char* name, std::string& out) {
    const char* value = std::getenv(name);
    if (!value) return false;
    out = value;
    return true;
}

// ---- dynamic libraries ----------------------------------------------------------------------

void* libraryOpen(const std::filesystem::path& path, std::string& error) noexcept {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* msg = dlerror();
        error = msg ? msg : "dlopen failed";
    }
    return handle;
}

void* librarySymbol(void* handle, const char* name) noexcept { return dlsym(handle, name); }

void libraryClose(void* handle) noexcept { dlclose(handle); }

std::string_view libraryExtension() noexcept {
#if defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

std::string_view libraryPrefix() noexcept { return "lib"; }

// ---- randomness -----------------------------------------------------------------------------

bool secureRandomBytes(void* buffer, usize size) noexcept {
    auto* out = static_cast<u8*>(buffer);
#if defined(__linux__)
    usize done = 0;
    while (done < size) {
        const ssize_t n = getrandom(out + done, size - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; // ENOSYS on ancient kernels: fall back to /dev/urandom
        }
        done += static_cast<usize>(n);
    }
    if (done == size) return true;
#endif
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    usize got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, out + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<usize>(n);
    }
    ::close(fd);
    return got == size;
}

} // namespace helios::os
