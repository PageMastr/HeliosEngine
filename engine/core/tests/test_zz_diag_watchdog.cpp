// TEMPORARY CI DIAGNOSTIC for PR #15 (core_tests times out on MSVC only). Reverted before merge.
// Prints test progress to stderr (unbuffered, so it survives a ctest timeout kill) and, if no test
// finishes for kStuckSeconds or the process does not exit after the run, dumps every thread's stack
// (MSVC: RtlVirtualUnwind + dbghelp symbols) and exits with a distinctive code.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_MSC_VER)
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace {

constexpr long long kStuckSeconds = 120;
constexpr long long kExitSeconds = 60;

std::atomic<long long> g_lastProgressMs{0};
std::atomic<int> g_phase{0}; // 0 before the run, 1 running, 2 run ended
std::atomic<const doctest::TestCaseData*> g_current{nullptr};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#if defined(_MSC_VER)
void dumpAllThreads() {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(process, nullptr, TRUE)) {
        std::fprintf(stderr, "[diag] SymInitialize failed: %lu\n", GetLastError());
    }
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                               te.th32ThreadID);
        if (!th) continue;
        DWORD64 frames[64];
        int n = 0;
        // Walk while suspended without allocating (the thread may hold the heap lock); symbolize after.
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx)) {
                while (n < 64 && ctx.Rip != 0) {
                    frames[n++] = ctx.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                    if (!fn) {
                        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                    } else {
                        void* handlerData = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher,
                                         nullptr);
                    }
                }
            }
            ResumeThread(th);
        }
        char name[128] = "?";
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
            LocalFree(desc);
        }
        std::fprintf(stderr, "[diag] thread %lu '%s' (%d frames)\n", te.th32ThreadID, name, n);
        for (int i = 0; i < n; ++i) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            const bool haveSym = SymFromAddr(process, frames[i], &disp, sym) != FALSE;
            const bool haveLine = SymGetLineFromAddr64(process, frames[i], &lineDisp, &line) != FALSE;
            std::fprintf(stderr, "[diag]   #%02d %016llx %s+0x%llx  %s:%lu\n", i,
                         static_cast<unsigned long long>(frames[i]), haveSym ? sym->Name : "?",
                         static_cast<unsigned long long>(disp), haveLine ? line.FileName : "?",
                         haveLine ? line.LineNumber : 0ul);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    std::fflush(stderr);
}
#else
void dumpAllThreads() { std::fprintf(stderr, "[diag] (no stack dump on this toolchain)\n"); }
#endif

void watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const long long idle = (nowMs() - g_lastProgressMs.load()) / 1000;
        const int phase = g_phase.load();
        if (phase == 1 && idle >= kStuckSeconds) {
            const doctest::TestCaseData* tc = g_current.load();
            std::fprintf(stderr, "[diag] STUCK for %llds in test '%s' (%s:%d)\n", idle, tc ? tc->m_name : "?",
                         tc ? tc->m_file.c_str() : "?", tc ? static_cast<int>(tc->m_line) : 0);
            dumpAllThreads();
            std::_Exit(97);
        }
        if (phase == 2 && idle >= kExitSeconds) {
            std::fprintf(stderr, "[diag] STUCK for %llds after the run ended (process exit)\n", idle);
            dumpAllThreads();
            std::_Exit(98);
        }
    }
}

struct DiagListener : doctest::IReporter {
    explicit DiagListener(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {
        g_lastProgressMs.store(nowMs());
        g_phase.store(1);
        std::thread(watchdog).detach();
    }
    void test_run_end(const doctest::TestRunStats& stats) override {
        std::fprintf(stderr, "[diag] run end: %u cases, %u failed\n", stats.numTestCasesPassingFilters,
                     stats.numTestCasesFailed);
        std::fflush(stderr);
        g_lastProgressMs.store(nowMs());
        g_phase.store(2);
    }
    void test_case_start(const doctest::TestCaseData& tc) override {
        g_current.store(&tc);
        g_lastProgressMs.store(nowMs());
        std::fprintf(stderr, "[diag] > %s\n", tc.m_name);
        std::fflush(stderr);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& stats) override {
        const doctest::TestCaseData* tc = g_current.load();
        std::fprintf(stderr, "[diag] < %s (%.3fs)%s\n", tc ? tc->m_name : "?", stats.seconds,
                     stats.testCaseSuccess ? "" : " FAILED");
        std::fflush(stderr);
        g_lastProgressMs.store(nowMs());
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData& ad) override {
        if (!ad.m_failed) return;
        std::fprintf(stderr, "[diag] assert failed %s:%d: %s\n", ad.m_file, ad.m_line, ad.m_expr);
        std::fflush(stderr);
    }
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("helios_diag", 1, DiagListener);

// Static destruction runs after main returns; report it so an exit-time hang can be placed.
struct ExitReporter {
    ~ExitReporter() {
        std::fprintf(stderr, "[diag] static destructors of test_zz_diag_watchdog.cpp\n");
        std::fflush(stderr);
    }
} g_exitReporter;

} // namespace
