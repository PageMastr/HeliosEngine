/* CPU gate pre-initializer for Windows executables (02 §1.1, 08 §2.2).
 *
 * Linked into every AVX2 image by helios_cpu_gate() (cmake/HeliosIsa.cmake) as an object file. The
 * entry sits in .CRT$XIB, the C initializer table, which the CRT (MSVC and mingw-w64) runs before
 * every C++ dynamic initializer (.CRT$XC*), so no Helios-compiled C++ code runs before the check.
 * On an unsupported CPU it writes the gate message to stderr, or shows it in a message box when the
 * process has no console (MessageBoxW is resolved at run time: the gate links only kernel32), and
 * exits with code 78. On a supported CPU it installs a vectored handler that turns
 * STATUS_ILLEGAL_INSTRUCTION into the same kind of message (deliberate ud2 traps excepted).
 *
 * Gate TU rules (cpu_gate.h): C, x86-64-v1 flags, static functions only, and only GetStdHandle,
 * WriteFile, LoadLibraryExW, GetProcAddress, ExitProcess and AddVectoredExceptionHandler.
 */
#include "../../cpugate/cpu_gate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define HCG_EXIT_CODE 78

#if defined(HELIOS_CPU_GATE_TEST_EMULATE_SANDY_BRIDGE)
/* Test builds only (core_cpugate_child_snb): evaluate a recorded Core i7-2600K instead of the live
 * CPU, so the refusal path runs on any machine. */
static const HeliosCpuidRaw hcg_test_cpu = {
    {0x0000000du, 0x756e6547u, 0x6c65746eu, 0x49656e69u},
    {0x000206a7u, 0x00100800u, 0x1fbae3ffu, 0xbfebfbffu},
    {0u, 0u, 0u, 0u},
    {0x80000008u, 0u, 0u, 0u},
    {0u, 0u, 0x00000001u, 0x28100800u},
    {0x65746e49u, 0x2952286cu, 0x726f4320u, 0x4d542865u, 0x37692029u, 0x3036322du, 0x43204b30u, 0x40205550u,
     0x342e3320u, 0x7a484730u, 0u, 0u},
    7u,
    1};
#define HCG_GATE_INPUT (&hcg_test_cpu)
#else
#define HCG_GATE_INPUT 0
#endif

static char hcg_backstop[640];
static DWORD hcg_backstop_len;

typedef int(WINAPI* HcgMessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT);

static DWORD hcg_append(char* dst, DWORD len, DWORD cap, const char* s) {
    while (*s && len + 1 < cap) dst[len++] = *s++;
    dst[len] = '\0';
    return len;
}

static void hcg_notify(const char* msg, DWORD len) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err != NULL && err != INVALID_HANDLE_VALUE && WriteFile(err, msg, len, &written, NULL) && written == len) {
        return;
    }
    {
        /* No usable stderr (GUI subsystem): a message box from user32, loaded from System32 only. */
        HMODULE user32 = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (user32 != NULL) {
            /* A union converts the FARPROC without a function-pointer cast (which GCC's
             * -Wcast-function-type and MSVC's C4054/C4055 would flag). */
            union {
                FARPROC proc;
                HcgMessageBoxW box;
            } fn;
            fn.proc = GetProcAddress(user32, "MessageBoxW");
            if (fn.proc != NULL) {
                const HcgMessageBoxW box = fn.box;
                WCHAR wide[640];
                DWORD i;
                for (i = 0; i < len && i + 1 < 640; ++i) wide[i] = (WCHAR)(unsigned char)msg[i];
                wide[i] = 0;
                box(NULL, wide, L"Helios", MB_OK | MB_ICONERROR);
            }
        }
    }
}

/* ud2 (0F 0B), ud1 (0F B9) and ud0 (0F FF) are deliberate traps (__builtin_trap, clang-cl's
 * trap-on-unreachable, sanitizer traps), not a missing instruction-set extension. A vectored handler
 * sees every exception first, so without this check each such trap would be misreported as an
 * unsupported CPU and exit with 78 instead of reaching the crash handler (minidump). */
static int hcg_is_deliberate_trap(const void* pc) {
    const unsigned char* op = (const unsigned char*)pc;
    if (op == NULL) return 0;
    return op[0] == 0x0f && (op[1] == 0x0b || op[1] == 0xb9 || op[1] == 0xff);
}

static LONG CALLBACK hcg_on_exception(PEXCEPTION_POINTERS info) {
    if (info != NULL && info->ExceptionRecord != NULL &&
        info->ExceptionRecord->ExceptionCode == (DWORD)STATUS_ILLEGAL_INSTRUCTION &&
        !hcg_is_deliberate_trap(info->ExceptionRecord->ExceptionAddress)) {
        hcg_notify(hcg_backstop, hcg_backstop_len);
        ExitProcess(HCG_EXIT_CODE);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int __cdecl hcg_gate(void) {
    HeliosCpuGateReport report;
    if (!helios_cpu_gate_run(HCG_GATE_INPUT, HELIOS_CPU_REQUIRE_AVX2_IMAGE, NULL, &report)) {
        char line[600];
        DWORD n = 0;
        n = hcg_append(line, n, sizeof(line), report.message);
        n = hcg_append(line, n, sizeof(line), "\r\n");
        hcg_notify(line, n);
        ExitProcess(HCG_EXIT_CODE);
    }
    hcg_backstop_len = hcg_append(hcg_backstop, 0, sizeof(hcg_backstop),
                                  "Helios stopped: illegal instruction. The CPU may lack an instruction set "
                                  "extension this build requires (AVX2, BMI1, BMI2, F16C, LZCNT). Detected: ");
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), report.brand);
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), "\r\n");
    AddVectoredExceptionHandler(0, hcg_on_exception);
    return 0;
}

#if defined(_MSC_VER)
/* External (and /include'd) so /OPT:REF, /Gw or LTCG can never discard the table entry; it is the
 * only external symbol this TU defines. */
#pragma section(".CRT$XIB", long, read)
#pragma comment(linker, "/include:helios_cpu_gate_crt_entry")
__declspec(allocate(".CRT$XIB")) int(__cdecl* const helios_cpu_gate_crt_entry)(void) = hcg_gate;
#else
__attribute__((section(".CRT$XIB"), used)) static int (*hcg_entry)(void) = hcg_gate;
#endif
