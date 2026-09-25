// Child for the CPU-gate tests (tests/test_cpu.cpp). Linked with the gate's pre-initializer hook.
// It prints "static-init" from a C++ dynamic initializer and "main reached" from main(), so the
// test can check that a refusing gate exits with code 78 before either runs. `--sigill` executes
// an invalid opcode (0x06, PUSH ES, which does not exist in 64-bit mode: what a CPU lacking an
// extension reports) to exercise the gate's SIGILL / STATUS_ILLEGAL_INSTRUCTION backstop; `--trap`
// executes ud2 (__builtin_trap), a deliberate crash the backstop must leave alone.
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#include "helios/core/cpu.h"

namespace {
struct StaticInitMarker {
    StaticInitMarker() {
        std::fputs("static-init\n", stdout);
        std::fflush(stdout);
    }
};
StaticInitMarker g_marker;
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--sigill") == 0) {
        std::fflush(stdout);
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
        __asm__ volatile(".byte 0x06"); // #UD in 64-bit mode
#else
        std::puts("sigill-unsupported"); // MSVC x64 has no inline assembly
        return 3;
#endif
    }
    if (argc > 1 && std::strcmp(argv[1], "--trap") == 0) {
        std::fflush(stdout);
#if defined(_MSC_VER) && !defined(__clang__)
        __ud2();
#else
        __builtin_trap();
#endif
    }
    const helios::CpuGateReport& report = helios::cpuGate();
    std::printf("main reached: %s\n", report.message.c_str());
    std::fflush(stdout);
    return report.supported ? 0 : 1;
}
