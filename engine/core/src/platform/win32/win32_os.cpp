// Win32 implementation of time, threads, process, memory, console, dynamic libraries, randomness
// and error helpers.

#include "platform/win32/win32_common.h"

#include <bcrypt.h>

#include <algorithm>
#include <format>

#include "helios/core/cmdline.h"
#include "platform/os.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif

namespace helios {

namespace win32 {

std::string errorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (len > 0 && buffer) {
        text = narrow(std::wstring_view(buffer, len));
        LocalFree(buffer);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
            text.pop_back();
        }
    } else {
        text = "unknown error";
    }
    return text;
}

ErrorCode mapError(DWORD code) noexcept {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_MOD_NOT_FOUND:
    case ERROR_PROC_NOT_FOUND: return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return ErrorCode::PermissionDenied;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return ErrorCode::AlreadyExists;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_DIRECTORY: return ErrorCode::InvalidArgument;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ErrorCode::OutOfMemory;
    case ERROR_HANDLE_EOF: return ErrorCode::EndOfFile;
    case ERROR_BUSY: return ErrorCode::Busy;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT: return ErrorCode::Timeout;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return ErrorCode::Unsupported;
    case ERROR_OPERATION_ABORTED: return ErrorCode::Cancelled;
    default: return ErrorCode::IoError;
    }
}

Error makeWin32Error(DWORD code, std::string_view context) {
    return Error{mapError(code), std::format("{}: {} (Win32 error {})", context, errorMessage(code), code)};
}

} // namespace win32

namespace os {

Error lastError(std::string_view context) { return win32::makeWin32Error(GetLastError(), context); }

// ---- time -----------------------------------------------------------------------------------

namespace {

u64 qpcFrequency() noexcept {
    static const u64 frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<u64>(f.QuadPart);
    }();
    return frequency;
}

/// Per-thread high-resolution waitable timer (Windows 10 1803+); nullptr if unsupported.
struct ThreadTimer {
    HANDLE handle = nullptr;
    bool highResolution = false;
    ThreadTimer() {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution = handle != nullptr;
        if (!handle) handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~ThreadTimer() {
        if (handle) CloseHandle(handle);
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
};

ThreadTimer& threadTimer() {
    thread_local ThreadTimer timer;
    return timer;
}

} // namespace

u64 monotonicTicks() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<u64>(t.QuadPart);
}

u64 monotonicFrequency() noexcept { return qpcFrequency(); }

u64 monotonicNanos() noexcept {
    const u64 ticks = monotonicTicks();
    const u64 freq = qpcFrequency();
    // Split to avoid overflow of ticks * 1e9.
    return (ticks / freq) * 1'000'000'000ull + ((ticks % freq) * 1'000'000'000ull) / freq;
}

void sleepNanos(u64 nanos) noexcept {
    ThreadTimer& timer = threadTimer();
    if (timer.handle) {
        LARGE_INTEGER due;
        const u64 units = std::max<u64>(1, nanos / 100); // 100 ns units, negative = relative
        due.QuadPart = -static_cast<LONGLONG>(units);
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.handle, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(std::max<u64>(1, (nanos + 999'999) / 1'000'000)));
}

void highResolutionSleepNanos(u64 nanos) noexcept { sleepNanos(nanos); }

u64 sleepSpinMarginNanos() noexcept {
    // High-resolution timers overshoot by ~0.5 ms; legacy timers by up to the 15.6 ms tick.
    return threadTimer().highResolution ? 700'000ull : 2'000'000ull;
}

// ---- threads / process ----------------------------------------------------------------------

u64 currentThreadId() noexcept { return static_cast<u64>(GetCurrentThreadId()); }

void setCurrentThreadName(const std::string& name) noexcept {
    // SetThreadDescription exists since Windows 10 1607; resolve dynamically for older systems.
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetThreadDescriptionFn fn = [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel ? win32::procAs<SetThreadDescriptionFn>(GetProcAddress(kernel, "SetThreadDescription"))
                      : nullptr;
    }();
    if (fn) {
        const std::wstring wide = win32::widen(name);
        fn(GetCurrentThread(), wide.c_str());
    }
}

bool setCurrentThreadAffinity(u64 mask) noexcept {
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(mask)) != 0;
}

bool setCurrentThreadPriority(ThreadPriority priority) noexcept {
    const int value = priority == ThreadPriority::Low    ? THREAD_PRIORITY_BELOW_NORMAL
                      : priority == ThreadPriority::High ? THREAD_PRIORITY_ABOVE_NORMAL
                                                         : THREAD_PRIORITY_NORMAL;
    return SetThreadPriority(GetCurrentThread(), value) != 0;
}

u32 processorCount() noexcept {
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? static_cast<u32>(n) : 1u;
}

u32 currentProcessId() noexcept { return static_cast<u32>(GetCurrentProcessId()); }

std::vector<std::string> processArguments() {
    const wchar_t* cmd = GetCommandLineW();
    return CommandLine::splitWindows(cmd ? std::wstring_view(cmd) : std::wstring_view());
}

// ---- memory ---------------------------------------------------------------------------------

namespace {
const SYSTEM_INFO& systemInfo() noexcept {
    static const SYSTEM_INFO info = [] {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si;
    }();
    return info;
}
} // namespace

usize pageSize() noexcept { return static_cast<usize>(systemInfo().dwPageSize); }
usize allocationGranularity() noexcept { return static_cast<usize>(systemInfo().dwAllocationGranularity); }

void* vmReserve(usize size) noexcept { return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS); }

bool vmCommit(void* ptr, usize size) noexcept { return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr; }

bool vmDecommit(void* ptr, usize size) noexcept { return VirtualFree(ptr, size, MEM_DECOMMIT) != 0; }

bool vmRelease(void* ptr, usize) noexcept { return VirtualFree(ptr, 0, MEM_RELEASE) != 0; }

// ---- console / debugger ---------------------------------------------------------------------

namespace {
HANDLE streamHandle(ConsoleStream stream) noexcept {
    return GetStdHandle(stream == ConsoleStream::Out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}
} // namespace

bool consoleEnableColors(ConsoleStream stream) noexcept {
    if (GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) > 0) return false;
    HANDLE h = streamHandle(stream);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false; // redirected to a file/pipe
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

void consoleWrite(ConsoleStream stream, std::string_view utf8) noexcept {
    HANDLE h = streamHandle(stream);
    if (h == nullptr || h == INVALID_HANDLE_VALUE || utf8.empty()) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        // Real console: write UTF-16 so non-ASCII text renders regardless of the code page.
        const std::wstring wide = win32::widen(utf8);
        const wchar_t* p = wide.data();
        usize left = wide.size();
        while (left > 0) {
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<usize>(left, 16 * 1024));
            // Never split a surrogate pair across two writes (each half would render as U+FFFD).
            if (chunk < left && chunk > 1 && p[chunk - 1] >= 0xD800 && p[chunk - 1] <= 0xDBFF) --chunk;
            if (!WriteConsoleW(h, p, chunk, &written, nullptr) || written == 0) return;
            p += written;
            left -= written;
        }
        return;
    }
    const char* p = utf8.data();
    usize left = utf8.size();
    while (left > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<usize>(left, 1u << 30));
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) return;
        p += written;
        left -= written;
    }
}

bool isDebuggerPresent() noexcept { return IsDebuggerPresent() != 0; }

void debugOutput(std::string_view utf8) noexcept {
    const std::wstring wide = win32::widen(utf8);
    OutputDebugStringW(wide.c_str());
}

bool getEnv(const char* name, std::string& out) {
    const std::wstring wname = win32::widen(name);
    const DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) return false;
    std::wstring value(needed, L'\0');
    const DWORD len = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    value.resize(len);
    out = win32::narrow(value);
    return true;
}

// ---- dynamic libraries ----------------------------------------------------------------------

void* libraryOpen(const std::filesystem::path& path, std::string& error) noexcept {
    // For absolute paths, let the loader find the plugin's own dependencies next to it.
    const DWORD flags = path.is_absolute() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, flags);
    if (!module) {
        const DWORD code = GetLastError();
        error = std::format("{} (Win32 error {})", win32::errorMessage(code), code);
        return nullptr;
    }
    return reinterpret_cast<void*>(module);
}

void* librarySymbol(void* handle, const char* name) noexcept {
    const FARPROC proc = GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    void* p = nullptr;
    static_assert(sizeof(p) == sizeof(proc));
    std::memcpy(&p, &proc, sizeof(p));
    return p;
}

void libraryClose(void* handle) noexcept { FreeLibrary(reinterpret_cast<HMODULE>(handle)); }

std::string_view libraryExtension() noexcept { return ".dll"; }
std::string_view libraryPrefix() noexcept { return ""; }

// ---- randomness -----------------------------------------------------------------------------

bool secureRandomBytes(void* buffer, usize size) noexcept {
    auto* out = static_cast<u8*>(buffer);
    while (size > 0) {
        const ULONG chunk = static_cast<ULONG>(std::min<usize>(size, 0x10000000));
        const NTSTATUS status = BCryptGenRandom(nullptr, out, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) return false;
        out += chunk;
        size -= chunk;
    }
    return true;
}

} // namespace os
} // namespace helios
