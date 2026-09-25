/* CPU gate pre-initializer for ELF executables (02 §1.1, 08 §2.2).
 *
 * Linked into every AVX2 image by helios_cpu_gate() (cmake/HeliosIsa.cmake) as an object file, so
 * the .preinit_array entry below is always present. glibc runs .preinit_array before the
 * initializers of the executable and of every shared library, so no Helios-compiled C++ code can
 * execute before the check. On an unsupported CPU it prints the gate message to stderr and exits
 * with code 78; on a supported one it installs a SIGILL backstop that prints a CPU hint instead of
 * dying silently (core's crash handler replaces it once main() installs one). Deliberate traps
 * (ud2) are passed through, so they still crash normally.
 *
 * Gate TU rules (cpu_gate.h): C, x86-64-v1 flags, static functions only, and only write, _exit
 * and sigaction from libc.
 */
/* sigaction, siginfo_t, SA_SIGINFO and SA_RESETHAND are POSIX/XSI: request them explicitly so
 * the hook also builds with a strict -std=c11 (CMAKE_C_EXTENSIONS OFF). */
#if !defined(_XOPEN_SOURCE) && !defined(__APPLE__)
#define _XOPEN_SOURCE 700
#endif

#include "../../cpugate/cpu_gate.h"

#include <signal.h>
#include <unistd.h>

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
static unsigned hcg_backstop_len;

static void hcg_write_stderr(const char* s, unsigned n) {
    while (n > 0) {
        const ssize_t w = write(2, s, n);
        if (w <= 0) return;
        s += w;
        n -= (unsigned)w;
    }
}

static unsigned hcg_append(char* dst, unsigned len, unsigned cap, const char* s) {
    while (*s && len + 1 < cap) dst[len++] = *s++;
    dst[len] = '\0';
    return len;
}

/* ud2 (0F 0B), ud1 (0F B9) and ud0 (0F FF) are deliberate traps (__builtin_trap, clang's
 * trap-on-unreachable, sanitizer traps), not a missing instruction-set extension: they must crash
 * normally (core dump, crash handler) instead of being reported as an unsupported CPU. */
static int hcg_is_deliberate_trap(const void* pc) {
    const unsigned char* op = (const unsigned char*)pc;
    if (!op) return 0;
    return op[0] == 0x0f && (op[1] == 0x0b || op[1] == 0xb9 || op[1] == 0xff);
}

static void hcg_on_sigill(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)context;
    if (info && hcg_is_deliberate_trap(info->si_addr)) {
        /* SA_RESETHAND restored SIG_DFL: returning re-executes the trap, which now kills the
         * process with SIGILL as if the gate had never been installed. */
        return;
    }
    hcg_write_stderr(hcg_backstop, hcg_backstop_len);
    _exit(HCG_EXIT_CODE);
}

static void hcg_gate(int argc, char** argv, char** envp) {
    HeliosCpuGateReport report;
    struct sigaction sa;
    unsigned char* bytes = (unsigned char*)&sa;
    unsigned i;
    (void)argc;
    (void)argv;
    (void)envp;
    if (!helios_cpu_gate_run(HCG_GATE_INPUT, HELIOS_CPU_REQUIRE_AVX2_IMAGE, 0, &report)) {
        unsigned n = 0;
        char line[600];
        n = hcg_append(line, n, sizeof(line), report.message);
        n = hcg_append(line, n, sizeof(line), "\n");
        hcg_write_stderr(line, n);
        _exit(HCG_EXIT_CODE);
    }
    hcg_backstop_len = hcg_append(hcg_backstop, 0, sizeof(hcg_backstop),
                                  "Helios stopped: illegal instruction. The CPU may lack an instruction set "
                                  "extension this build requires (AVX2, BMI1, BMI2, F16C, LZCNT). Detected: ");
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), report.brand);
    hcg_backstop_len = hcg_append(hcg_backstop, hcg_backstop_len, sizeof(hcg_backstop), "\n");
    /* Zeroed byte by byte: no memset/sigemptyset (not on the gate's allowlist). An all-zero
     * sa_mask is the empty set on every POSIX system Helios targets. */
    for (i = 0; i < sizeof(sa); ++i) bytes[i] = 0;
    sa.sa_sigaction = hcg_on_sigill;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGILL, &sa, 0);
}

#if defined(__ELF__)
__attribute__((section(".preinit_array"), used)) static void (*hcg_preinit_entry)(int, char**, char**) = hcg_gate;
#else
/* Non-ELF POSIX (not a supported target): the earliest portable hook. */
__attribute__((constructor(101))) static void hcg_constructor(void) { hcg_gate(0, 0, 0); }
#endif
