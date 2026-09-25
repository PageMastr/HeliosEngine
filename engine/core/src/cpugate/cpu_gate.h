/* Internal C interface of the CPU gate (02 §1.1, 08 §2.2). Not a public header: C++ code uses
 * helios/core/cpu.h. Shared by cpu_gate.c (the probe), the per-OS pre-initializer hooks in
 * src/platform/{win32,posix}/cpu_gate_hook.c and src/cpu.cpp.
 *
 * Rules for every gate TU (checked by the ISA audit, tools/lint/isa_audit.cmake):
 *  - C only; no libc or C++ runtime calls (the hooks run before the C/C++ runtimes are initialized);
 *  - compiled at the x86-64-v1 baseline (cmake/HeliosIsa.cmake helios_isa_base_sources): no SSE3+,
 *    POPCNT, LZCNT, BMI, AVX or AVX-512 encodings;
 *  - only static functions plus the extern "C" entry helios_cpu_gate_run(); no weak, COMDAT or
 *    selectany symbols, so no AVX2 copy of anything can be picked for the gate at link time.
 */
#ifndef HELIOS_CORE_CPU_GATE_H
#define HELIOS_CORE_CPU_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feature bits (HeliosCpuGateReport::detected / required / missing). */
enum {
    HELIOS_CPU_SSE2 = 1u << 0,
    HELIOS_CPU_SSE3 = 1u << 1,
    HELIOS_CPU_SSSE3 = 1u << 2,
    HELIOS_CPU_SSE41 = 1u << 3,
    HELIOS_CPU_SSE42 = 1u << 4,
    HELIOS_CPU_POPCNT = 1u << 5,
    HELIOS_CPU_CX16 = 1u << 6,
    HELIOS_CPU_LAHF = 1u << 7,
    HELIOS_CPU_AVX = 1u << 8,
    HELIOS_CPU_AVX2 = 1u << 9,
    HELIOS_CPU_FMA = 1u << 10,
    HELIOS_CPU_BMI1 = 1u << 11,
    HELIOS_CPU_BMI2 = 1u << 12,
    HELIOS_CPU_F16C = 1u << 13,
    HELIOS_CPU_LZCNT = 1u << 14,
    HELIOS_CPU_MOVBE = 1u << 15,
    HELIOS_CPU_AVX512F = 1u << 16,
    HELIOS_CPU_XSAVE = 1u << 17,
    HELIOS_CPU_OSXSAVE = 1u << 18,
    HELIOS_CPU_FEATURE_COUNT = 19
};

/* x86-64-v2 floor (the bootstrap's check, 08 §2.1.1). */
#define HELIOS_CPU_REQUIRE_V2 \
    (HELIOS_CPU_SSE2 | HELIOS_CPU_SSE3 | HELIOS_CPU_SSSE3 | HELIOS_CPU_SSE41 | HELIOS_CPU_SSE42 | \
     HELIOS_CPU_POPCNT | HELIOS_CPU_CX16 | HELIOS_CPU_LAHF)
/* What every `avx2` image needs (08 §2.2 plus BMI2, 02 §1.1). FMA is reported but never required:
 * Helios never emits FMA (determinism). */
#define HELIOS_CPU_REQUIRE_AVX2_IMAGE \
    (HELIOS_CPU_REQUIRE_V2 | HELIOS_CPU_AVX | HELIOS_CPU_AVX2 | HELIOS_CPU_BMI1 | HELIOS_CPU_BMI2 | \
     HELIOS_CPU_F16C | HELIOS_CPU_LZCNT)

/* Raw CPUID/XGETBV values. Filled from the running CPU, or by tests to emulate other CPUs. */
typedef struct HeliosCpuidRaw {
    uint32_t leaf0[4];    /* EAX EBX ECX EDX of CPUID(0): max leaf, vendor */
    uint32_t leaf1[4];    /* CPUID(1) */
    uint32_t leaf7[4];    /* CPUID(7, 0); zero if max leaf < 7 */
    uint32_t ext0[4];     /* CPUID(0x80000000): max extended leaf */
    uint32_t ext1[4];     /* CPUID(0x80000001); zero if unavailable */
    uint32_t brand[12];   /* CPUID(0x80000002..4); zero if unavailable */
    uint64_t xcr0;        /* XGETBV(0) when OSXSAVE is set, else 0 */
    int isX86;            /* 0 on non-x86 builds: nothing else is valid */
} HeliosCpuidRaw;

typedef struct HeliosCpuGateReport {
    int supported;        /* 1 if (usable & required) == required */
    int isX86;
    int osAvxState;       /* OSXSAVE and XCR0 has SSE+YMM state */
    int osAvx512State;    /* ... and opmask/ZMM state */
    uint32_t detected;    /* CPUID feature bits */
    uint32_t usable;      /* detected, minus AVX-class features the OS does not enable */
    uint32_t required;
    uint32_t missing;     /* required & ~usable */
    uint32_t family, model, stepping;
    char vendor[13];
    char brand[49];       /* trimmed brand string, or "<vendor> family F model M" */
    char message[512];    /* human-readable verdict, ASCII, no trailing newline */
} HeliosCpuGateReport;

/* Evaluates `raw` (or the running CPU if raw is NULL) against `required` and fills `out`.
 * `display_name` names the product in the message ("Helios" if NULL). Returns out->supported.
 * Async-signal-safe, allocation-free, callable before the C runtime is initialized. */
int helios_cpu_gate_run(const HeliosCpuidRaw* raw, uint32_t required, const char* display_name,
                        HeliosCpuGateReport* out);

#ifdef __cplusplus
}
#endif

#endif /* HELIOS_CORE_CPU_GATE_H */
