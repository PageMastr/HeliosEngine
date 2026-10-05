// Child for the CPU-gate tests (tests/test_cpu.cpp), built three ways (engine/core/CMakeLists.txt): with the
// real pre-initializer (core_cpugate_child), with one that evaluates a recorded Sandy Bridge
// (core_cpugate_child_snb) and with none (core_cpugate_child_nohook, the "dropped hook" case). It prints
// "static-init" from a C++ dynamic initializer, then main() runs core::platformInit() first, as every gated
// executable does, and prints "main reached", so the test can check that a refusing gate exits with code 78
// before either runs and that platformInit() stops an image without a gate.
//
// Options, handled before platformInit() so that every build can answer them:
//   --sigill       executes an invalid opcode (0x06, PUSH ES, which does not exist in 64-bit mode: what a
//                  CPU lacking an extension reports) to exercise the gate's SIGILL /
//                  STATUS_ILLEGAL_INSTRUCTION backstop;
//   --trap         executes ud2 (__builtin_trap), a deliberate crash the backstop must leave alone;
//   --xlb-verdict  (Windows) prints the gate's verdict as a TLS callback in .CRT$XLB, mimalloc's slot, saw it
//                  when the loader called it: "pass" proves the gate ran ahead of that slot;
//   --tls-order    (Windows) prints whether the gate's .CRT$XLA0 slot is AddressOfCallBacks[0] of this
//                  executable's TLS directory ("first"), or "in-runtime-dll" when a modular build carries the
//                  gate in helios_runtime.dll (HELIOS_CPUGATE_CHILD_HOOK_IN_IMAGE is not defined then).
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "helios/core/cpu.h"
#include "helios/core/platform_init.h"

namespace {
struct StaticInitMarker {
    StaticInitMarker() {
        std::fputs("static-init\n", stdout);
        std::fflush(stdout);
    }
};
StaticInitMarker g_marker;
} // namespace

#if defined(_WIN32)
namespace {
// -1 until the .CRT$XLB callback runs; then the CpuGateVerdict it read. Constant-initialized (no
// initializer code), so it is valid when the loader calls the callback, before the CRT starts.
int g_verdictAtXlb = -1;

void NTAPI xlbCallback(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_verdictAtXlb = static_cast<int>(helios::cpuGateVerdict());
}

const char* verdictName(int v) {
    switch (v) {
    case -1: return "not-called";
    case static_cast<int>(helios::CpuGateVerdict::NotRun): return "not-run";
    case static_cast<int>(helios::CpuGateVerdict::Pass): return "pass";
    case static_cast<int>(helios::CpuGateVerdict::Fail): return "fail";
    default: return "?";
    }
}
} // namespace

// The callback slot, in mimalloc's .CRT$XLB (02 §1.1's table: after the gate's .CRT$XLA0, before the CRT's
// .CRT$XLC). _tls_used is pulled in here too: with the gate in helios_runtime.dll, or with no gate, nothing
// else would give this executable a TLS directory.
// The extern declaration gives the const definition external linkage, so /INCLUDE can name it.
extern "C" const PIMAGE_TLS_CALLBACK helios_cpugate_child_xlb_entry;
#if defined(_MSC_VER)
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:helios_cpugate_child_xlb_entry")
#pragma const_seg(".CRT$XLB")
extern "C" {
const PIMAGE_TLS_CALLBACK helios_cpugate_child_xlb_entry = xlbCallback;
}
#pragma const_seg()
#else
extern "C" {
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK helios_cpugate_child_xlb_entry = xlbCallback;
}
#endif

#if defined(HELIOS_CPUGATE_CHILD_HOOK_IN_IMAGE)
extern "C" const PIMAGE_TLS_CALLBACK helios_cpu_gate_tls_entry; // the gate's slot (win32/cpu_gate_hook.c)
#endif

namespace {
const char* tlsOrder() {
#if defined(HELIOS_CPUGATE_CHILD_HOOK_IN_IMAGE)
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (dir.VirtualAddress == 0) return "no-tls-directory";
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY*>(base + dir.VirtualAddress);
    // AddressOfCallBacks is a virtual address, already relocated by the loader.
    const auto* first =
        reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(static_cast<std::uintptr_t>(tls->AddressOfCallBacks));
    return first == &helios_cpu_gate_tls_entry ? "first" : "not-first";
#else
    return "in-runtime-dll";
#endif
}
} // namespace
#endif // _WIN32

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
#if defined(_WIN32)
    if (argc > 1 && std::strcmp(argv[1], "--xlb-verdict") == 0) {
        std::printf("xlb-verdict: %s\n", verdictName(g_verdictAtXlb));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--tls-order") == 0) {
        std::printf("tls-order: %s\n", tlsOrder());
        return 0;
    }
#endif
    helios::core::platformInit();
    const helios::CpuGateReport& report = helios::cpuGate();
    std::printf("main reached: %s\n", report.message.c_str());
    std::fflush(stdout);
    return report.supported ? 0 : 1;
}
