// Windows crash handler: SetUnhandledExceptionFilter + MiniDumpWriteDump.
//
// The dump is written from a helper thread so that stack overflows (where the faulting thread has
// almost no stack left) are still captured. Paths are prepared at install time; the crash paths use
// static buffers instead of the (possibly exhausted) stack and avoid heap allocation.
//
// SEH exceptions (access violations, stack overflow, ...) reach the unhandled-exception filter.
// abort() does not: the UCRT raises SIGABRT and then __fastfail()s, bypassing the filter. That
// covers failed asserts, HELIOS_LOG_FATAL and std::terminate, and MSVC keeps set_terminate()
// per thread, so an exception escaping a worker thread never sees our terminate handler. The CRT's
// SIGABRT action is process-wide, so it is hooked as well (plus the MSVC CRT's pure-call and
// invalid-parameter handlers, which also fast-fail by default).

#include "platform/win32/win32_common.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4091) // older SDK dbghelp.h: "typedef ignored" warnings
#endif
#include <dbghelp.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>

#include "platform/os.h"

namespace helios::os {

namespace {

constexpr usize kPathCapacity = 1024;

wchar_t g_prefix[kPathCapacity]; // "<dir>\<app>-"
bool g_fullDump = false;
bool g_installed = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::terminate_handler g_previousTerminate = nullptr;
using SignalHandler = void(__cdecl*)(int);
SignalHandler g_previousAbort = SIG_DFL;
#if defined(_MSC_VER)
_purecall_handler g_previousPurecall = nullptr;
_invalid_parameter_handler g_previousInvalidParameter = nullptr;
#endif
std::atomic<u32> g_sequence{0};
std::atomic<bool> g_inHandler{false};

// Pseudo exception code for dumps taken on abort()/terminate (STATUS_FATAL_APP_EXIT).
constexpr DWORD kAbortExceptionCode = 0x40000015;

void appendW(wchar_t* buf, usize cap, usize& pos, const wchar_t* s) noexcept {
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    buf[pos] = L'\0';
}

void appendUIntW(wchar_t* buf, usize cap, usize& pos, u64 v) noexcept {
    wchar_t tmp[24];
    usize n = 0;
    do {
        tmp[n++] = static_cast<wchar_t>(L'0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0 && pos + 1 < cap) buf[pos++] = tmp[--n];
    buf[pos] = L'\0';
}

/// "<prefix><unix time>-<pid>-<seq>.dmp"
void buildDumpPath(wchar_t* out, usize cap) noexcept {
    usize pos = 0;
    out[0] = L'\0';
    appendW(out, cap, pos, g_prefix);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const u64 fileTime = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    appendUIntW(out, cap, pos, fileTime / 10'000'000ull - 11'644'473'600ull); // FILETIME -> Unix seconds
    appendW(out, cap, pos, L"-");
    appendUIntW(out, cap, pos, GetCurrentProcessId());
    appendW(out, cap, pos, L"-");
    appendUIntW(out, cap, pos, g_sequence.fetch_add(1, std::memory_order_relaxed));
    appendW(out, cap, pos, L".dmp");
}

struct DumpRequest {
    EXCEPTION_POINTERS* exception = nullptr;
    DWORD threadId = 0;
    wchar_t path[kPathCapacity + 64] = {};
    BOOL ok = FALSE;
};

DWORD WINAPI dumpThreadProc(LPVOID param) {
    auto* request = static_cast<DumpRequest*>(param);
    HANDLE file = CreateFileW(request->path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = request->threadId;
    mei.ExceptionPointers = request->exception;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type =
        g_fullDump ? static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
                   : static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    request->ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                    request->exception ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return 0;
}

/// Writes a minidump from a helper thread; `exception` may be null (snapshot of the process).
bool writeDump(EXCEPTION_POINTERS* exception, DumpRequest& request) noexcept {
    request.exception = exception;
    request.threadId = GetCurrentThreadId();
    buildDumpPath(request.path, kPathCapacity + 64);
    HANDLE thread = CreateThread(nullptr, 256 * 1024, &dumpThreadProc, &request, 0, nullptr);
    if (!thread) {
        dumpThreadProc(&request); // last resort: write from this thread
    } else {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    return request.ok != FALSE;
}

// Crash-path state in static storage (only the first crashing thread uses it; see g_inHandler):
// after a stack overflow the faulting thread has a few KiB of stack left at best.
DumpRequest g_crashRequest;
char g_crashNarrowPath[kPathCapacity * 3];
CONTEXT g_abortContext;
EXCEPTION_RECORD g_abortRecord;
EXCEPTION_POINTERS g_abortPointers;

void reportToStderr(const wchar_t* path, bool ok) noexcept {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (!err || err == INVALID_HANDLE_VALUE) return;
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, g_crashNarrowPath, static_cast<int>(sizeof(g_crashNarrowPath)),
                                      nullptr, nullptr);
    const char* prefix = ok ? "Helios: fatal error; minidump: " : "Helios: fatal error; minidump FAILED: ";
    DWORD written = 0;
    WriteFile(err, prefix, static_cast<DWORD>(std::strlen(prefix)), &written, nullptr);
    if (n > 1) WriteFile(err, g_crashNarrowPath, static_cast<DWORD>(n - 1), &written, nullptr);
    WriteFile(err, "\n", 1, &written, nullptr);
}

/// Writes the crash dump; the caller has claimed g_inHandler (one dump per process).
void writeClaimedCrashDump(EXCEPTION_POINTERS* exception) noexcept {
    const bool ok = writeDump(exception, g_crashRequest);
    reportToStderr(g_crashRequest.path, ok);
}

/// Dump for an SEH exception; later/concurrent crashes are ignored.
void crashDumpOnce(EXCEPTION_POINTERS* exception) noexcept {
    if (g_inHandler.exchange(true)) return;
    writeClaimedCrashDump(exception);
}

/// Dump for fatal paths without an SEH exception (abort, terminate, CRT checks): captures the
/// calling thread's context so the debugger opens the dump on the failing thread. Claims
/// g_inHandler before touching the static context so concurrent aborts cannot corrupt it.
void crashDumpHere() noexcept {
    if (g_inHandler.exchange(true)) return;
    RtlCaptureContext(&g_abortContext);
    std::memset(&g_abortRecord, 0, sizeof(g_abortRecord));
    g_abortRecord.ExceptionCode = kAbortExceptionCode;
#if defined(_M_X64) || defined(__x86_64__)
    g_abortRecord.ExceptionAddress = reinterpret_cast<PVOID>(g_abortContext.Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
    g_abortRecord.ExceptionAddress = reinterpret_cast<PVOID>(g_abortContext.Pc);
#endif
    g_abortPointers.ExceptionRecord = &g_abortRecord;
    g_abortPointers.ContextRecord = &g_abortContext;
    writeClaimedCrashDump(&g_abortPointers);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
    crashDumpOnce(exception);
    return g_previousFilter ? g_previousFilter(exception) : EXCEPTION_EXECUTE_HANDLER;
}

/// SIGABRT: abort() from any thread. Returning lets abort() finish terminating the process.
void __cdecl abortSignalHandler(int) { crashDumpHere(); }

[[noreturn]] void terminateHandler() {
    crashDumpHere();
    std::abort();
}

#if defined(_MSC_VER)
void __cdecl purecallHandler() {
    crashDumpHere();
    std::abort();
}

void __cdecl invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    crashDumpHere();
    std::abort();
}
#endif

} // namespace

Result<void> installCrashHandler(const std::filesystem::path& dumpDir, const CrashHandlerOptions& options) {
    std::wstring prefix = dumpDir.wstring();
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') prefix.push_back(L'\\');
    prefix.append(win32::widen(options.appName.empty() ? std::string_view("helios") : std::string_view(options.appName)));
    prefix.push_back(L'-');
    if (prefix.size() + 1 > kPathCapacity) return Error{ErrorCode::InvalidArgument, "crash dump path too long"};
    std::memcpy(g_prefix, prefix.c_str(), (prefix.size() + 1) * sizeof(wchar_t));
    g_fullDump = options.fullMemoryDump;
    if (g_installed) return {};
    g_previousFilter = SetUnhandledExceptionFilter(&unhandledExceptionFilter);
    g_previousTerminate = std::set_terminate(&terminateHandler); // this thread only on MSVC
    const SignalHandler previousAbort = std::signal(SIGABRT, &abortSignalHandler); // process-wide
    g_previousAbort = previousAbort == SIG_ERR ? SIG_DFL : previousAbort;
#if defined(_MSC_VER)
    g_previousPurecall = _set_purecall_handler(&purecallHandler);
    g_previousInvalidParameter = _set_invalid_parameter_handler(&invalidParameterHandler);
#endif
    g_installed = true;
    return {};
}

void uninstallCrashHandler() noexcept {
    if (!g_installed) return;
    SetUnhandledExceptionFilter(g_previousFilter);
    std::set_terminate(g_previousTerminate);
    std::signal(SIGABRT, g_previousAbort);
#if defined(_MSC_VER)
    _set_purecall_handler(g_previousPurecall);
    _set_invalid_parameter_handler(g_previousInvalidParameter);
    g_previousPurecall = nullptr;
    g_previousInvalidParameter = nullptr;
#endif
    g_previousFilter = nullptr;
    g_previousTerminate = nullptr;
    g_previousAbort = SIG_DFL;
    g_installed = false;
}

Result<std::filesystem::path> writeCrashReport(std::string_view reason) {
    (void)reason; // minidumps carry no free-form text; callers log the reason
    DumpRequest request;
    if (!writeDump(nullptr, request)) return lastError("MiniDumpWriteDump");
    return std::filesystem::path(request.path);
}

} // namespace helios::os
