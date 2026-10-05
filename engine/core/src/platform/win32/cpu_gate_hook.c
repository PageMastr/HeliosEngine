/* CPU gate pre-initializer for Windows images (02 §1.1 "Windows: the first TLS callback", 08 §2.2).
 *
 * The entry is a PIMAGE_TLS_CALLBACK in section .CRT$XLA0, which the linker sorts directly after the CRT's
 * __xl_a sentinel (.CRT$XLA): it becomes IMAGE_TLS_DIRECTORY.AddressOfCallBacks[0], so the loader calls it
 * before every other TLS callback of the image (mimalloc's .CRT$XLB mi_tls_attach, the CRT's .CRT$XLC
 * __dyn_tls_init, MinGW's winpthreads) and before the entry point runs any C or C++ initializer. The image is
 * the executable in a shipping build and helios_runtime.dll in a modular dev build (02 §1.1 "Which image";
 * cmake/HeliosModular.cmake), so the same object serves both.
 *
 * It checks on DLL_PROCESS_ATTACH and returns at once for every other reason. On a supported CPU it records
 * the verdict (helios_cpu_gate_verdict, which core::platformInit() checks) and installs the
 * illegal-instruction backstop. On an unsupported CPU it never returns, because the next callback is AVX2
 * code, and it runs under the loader lock before the CRT exists: it writes the message to stderr, shows it in
 * a message box only when the process's executable is a WINDOWS_GUI image and HELIOS_CPU_GATE_SILENT is not
 * 1, and ends with TerminateProcess(GetCurrentProcess(), 78). It never calls the Win32 process-exit function,
 * which would send DLL_PROCESS_DETACH through the TLS callbacks, so mimalloc's .CRT$XLY hook would run AVX2
 * code on the way out (and in a modular build that exit would run inside DllMain under the loader lock).
 *
 * Gate TU rules (cpu_gate.h): C, x86-64-v1 flags, static functions and data plus the one external symbol
 * helios_cpu_gate_tls_entry, and calls only to GetStdHandle, WriteFile, GetModuleHandleW,
 * GetEnvironmentVariableW, LoadLibraryExW, GetProcAddress, GetCurrentProcess, TerminateProcess and
 * AddVectoredExceptionHandler (cmake/isa_allowlist.cmake). CONF-12 (tools/conformance) checks the section,
 * the /INCLUDE: options and the exit path.
 */
#include "../../cpugate/cpu_gate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define HCG_EXIT_CODE 78

#if defined(HELIOS_CPU_GATE_TEST_EMULATE_SANDY_BRIDGE)
/* Test builds only (core_cpugate_child_snb, core_cpugate_child_snb_gui): evaluate a recorded Core i7-2600K
 * instead of the live CPU, so the refusal path runs on any machine. */
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

static void hcg_write_stderr(const char* msg, DWORD len) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (err != NULL && err != INVALID_HANDLE_VALUE) WriteFile(err, msg, len, &written, NULL);
}

/* Whether the process's executable is a WINDOWS_GUI image (02 §1.1 failure path, step 2). The executable's
 * header, not this image's: in a modular build the gate sits in helios_runtime.dll. */
static int hcg_gui_process(void) {
    const unsigned char* base = (const unsigned char*)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt;
    if (base == NULL || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
    nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI;
}

/* HELIOS_CPU_GATE_SILENT=1 skips the dialog (CI runs GUI fixtures with nobody to dismiss it). */
static int hcg_silent(void) {
    WCHAR value[4];
    const DWORD n = GetEnvironmentVariableW(L"HELIOS_CPU_GATE_SILENT", value, 4);
    return n == 1 && value[0] == L'1';
}

/* user32 comes from System32 only. In the shipping client the executable imports it already (SDL3), so this
 * only adds a reference; in a modular build the load may nest under the loader lock, as the UCRT's own
 * fatal-error dialog does, and no application thread exists yet that could hold a lock the dialog needs. */
static void hcg_dialog(const char* msg, DWORD len) {
    HMODULE user32 = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (user32 != NULL) {
        /* A union converts the FARPROC without a function-pointer cast (which GCC's -Wcast-function-type and
         * MSVC's C4054/C4055 would flag). */
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

/* Never returns. TerminateProcess on the current process ends the calling thread too; the loop only makes
 * "never returns into AVX2 code" independent of that. */
static void hcg_refuse(const char* msg, DWORD len) {
    hcg_write_stderr(msg, len);
    if (hcg_gui_process() && !hcg_silent()) hcg_dialog(msg, len);
    for (;;) TerminateProcess(GetCurrentProcess(), HCG_EXIT_CODE);
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

/* The illegal-instruction backstop: it ends the process the way the gate does (TerminateProcess, 78). */
static LONG CALLBACK hcg_on_exception(PEXCEPTION_POINTERS info) {
    if (info != NULL && info->ExceptionRecord != NULL &&
        info->ExceptionRecord->ExceptionCode == (DWORD)STATUS_ILLEGAL_INSTRUCTION &&
        !hcg_is_deliberate_trap(info->ExceptionRecord->ExceptionAddress)) {
        hcg_refuse(hcg_backstop, hcg_backstop_len);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void NTAPI hcg_tls_callback(PVOID module, DWORD reason, PVOID reserved) {
    HeliosCpuGateReport report;
    (void)module;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return;
    if (!helios_cpu_gate_run(HCG_GATE_INPUT, HELIOS_CPU_REQUIRE_AVX2_IMAGE | HELIOS_CPU_GATE_RECORD, NULL,
                             &report)) {
        char line[600];
        DWORD n = 0;
        n = hcg_append(line, n, sizeof(line), report.message);
        n = hcg_append(line, n, sizeof(line), "\r\n");
        hcg_refuse(line, n);
    }
    hcg_backstop_len = hcg_append(hcg_backstop, 0, sizeof(hcg_backstop),
                                  "Helios stopped: illegal instruction. The CPU may lack an instruction set "
                                  "extension this build requires (AVX2, BMI1, BMI2, F16C, LZCNT). Detected: ");
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), report.brand);
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), "\r\n");
    AddVectoredExceptionHandler(0, hcg_on_exception);
}

/* The table slot. External, and kept by /INCLUDE (MSVC, clang-cl) or `used` (MinGW), so neither /OPT:REF, /Gw
 * nor LTCG can drop it; /INCLUDE:_tls_used (MinGW: a reference to _tls_used) links the CRT's TLS directory,
 * whose AddressOfCallBacks starts right after __xl_a. A file-scope const object has external linkage in C. */
#if defined(_MSC_VER)
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:helios_cpu_gate_tls_entry")
#pragma const_seg(".CRT$XLA0")
const PIMAGE_TLS_CALLBACK helios_cpu_gate_tls_entry = hcg_tls_callback;
#pragma const_seg()
#else
extern const IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((used)) static const void* const hcg_tls_directory = &_tls_used;
__attribute__((section(".CRT$XLA0"), used)) const PIMAGE_TLS_CALLBACK helios_cpu_gate_tls_entry =
    hcg_tls_callback;
#endif
