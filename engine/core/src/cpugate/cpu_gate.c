/* CPU gate probe: CPUID/XGETBV feature detection and the verdict message (02 §1.1, 08 §2.2).
 *
 * This TU is compiled at the x86-64-v1 baseline (helios_isa_base_sources in engine/core) and must
 * stay free of libc calls, so it can run from the pre-initializer hooks before the C runtime is up
 * and on CPUs that lack every extension Helios otherwise assumes. See cpu_gate.h for the rules the
 * ISA audit enforces. Architecture- and compiler-specific code only; no OS calls (those live in
 * src/platform/{win32,posix}/cpu_gate_hook.c).
 */
#include "cpu_gate.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#if defined(_M_X64) || defined(_M_IX86)
#define HCG_HAVE_X86 1
#endif
#elif defined(__x86_64__) || defined(__i386__)
#define HCG_HAVE_X86 1
#endif

#ifndef HCG_HAVE_X86
#define HCG_HAVE_X86 0
#endif

/* ---- CPUID / XGETBV ---------------------------------------------------------------------- */
#if HCG_HAVE_X86
static void hcg_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
    int r[4];
    __cpuidex(r, (int)leaf, (int)subleaf);
    regs[0] = (uint32_t)r[0];
    regs[1] = (uint32_t)r[1];
    regs[2] = (uint32_t)r[2];
    regs[3] = (uint32_t)r[3];
#else
    /* GNU inline asm (GCC, Clang, clang-cl, MinGW). Intrinsic headers would require target
     * attributes (xsave) that the baseline TU must not enable. */
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(subleaf));
    regs[0] = a;
    regs[1] = b;
    regs[2] = c;
    regs[3] = d;
#endif
}

/* Only valid when CPUID.1:ECX.OSXSAVE is set (otherwise XGETBV raises #UD). */
static uint64_t hcg_xgetbv0(void) {
#if defined(_MSC_VER) && !defined(__clang__)
    return (uint64_t)_xgetbv(0);
#else
    uint32_t lo, hi;
    /* xgetbv spelled as bytes so old assemblers and the baseline -march accept it. */
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    return ((uint64_t)hi << 32) | lo;
#endif
}

static void hcg_zero4(uint32_t r[4]) {
    r[0] = r[1] = r[2] = r[3] = 0;
}

static void hcg_read(HeliosCpuidRaw* raw) {
    uint32_t i;
    hcg_cpuid(0, 0, raw->leaf0);
    hcg_zero4(raw->leaf1);
    hcg_zero4(raw->leaf7);
    hcg_zero4(raw->ext1);
    for (i = 0; i < 12; ++i) raw->brand[i] = 0;
    raw->xcr0 = 0;
    raw->isX86 = 1;
    if (raw->leaf0[0] >= 1) hcg_cpuid(1, 0, raw->leaf1);
    if (raw->leaf0[0] >= 7) hcg_cpuid(7, 0, raw->leaf7);
    hcg_cpuid(0x80000000u, 0, raw->ext0);
    if (raw->ext0[0] >= 0x80000001u) hcg_cpuid(0x80000001u, 0, raw->ext1);
    if (raw->ext0[0] >= 0x80000004u) {
        hcg_cpuid(0x80000002u, 0, raw->brand + 0);
        hcg_cpuid(0x80000003u, 0, raw->brand + 4);
        hcg_cpuid(0x80000004u, 0, raw->brand + 8);
    }
    if (raw->leaf1[2] & (1u << 27)) raw->xcr0 = hcg_xgetbv0();
}
#endif /* HCG_HAVE_X86 */

/* ---- Text helpers (no libc) ------------------------------------------------------------- */
typedef struct HcgText {
    char* buf;
    uint32_t cap;
    uint32_t len;
} HcgText;

static void hcg_put(HcgText* t, const char* s) {
    while (*s && t->len + 1 < t->cap) t->buf[t->len++] = *s++;
    t->buf[t->len] = '\0';
}

static void hcg_put_u32(HcgText* t, uint32_t v) {
    char digits[11];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < 10);
    while (n > 0 && t->len + 1 < t->cap) t->buf[t->len++] = digits[--n];
    t->buf[t->len] = '\0';
}

static void hcg_regs_to_chars(const uint32_t* regs, uint32_t count, char* out) {
    uint32_t i, k;
    for (i = 0; i < count; ++i) {
        for (k = 0; k < 4; ++k) out[i * 4 + k] = (char)((regs[i] >> (8 * k)) & 0xffu);
    }
}

static const char* hcg_feature_name(uint32_t bit) {
    switch (bit) {
    case HELIOS_CPU_SSE2: return "SSE2";
    case HELIOS_CPU_SSE3: return "SSE3";
    case HELIOS_CPU_SSSE3: return "SSSE3";
    case HELIOS_CPU_SSE41: return "SSE4.1";
    case HELIOS_CPU_SSE42: return "SSE4.2";
    case HELIOS_CPU_POPCNT: return "POPCNT";
    case HELIOS_CPU_CX16: return "CMPXCHG16B";
    case HELIOS_CPU_LAHF: return "LAHF-SAHF";
    case HELIOS_CPU_AVX: return "AVX";
    case HELIOS_CPU_AVX2: return "AVX2";
    case HELIOS_CPU_FMA: return "FMA";
    case HELIOS_CPU_BMI1: return "BMI1";
    case HELIOS_CPU_BMI2: return "BMI2";
    case HELIOS_CPU_F16C: return "F16C";
    case HELIOS_CPU_LZCNT: return "LZCNT";
    case HELIOS_CPU_MOVBE: return "MOVBE";
    case HELIOS_CPU_AVX512F: return "AVX-512F";
    case HELIOS_CPU_XSAVE: return "XSAVE";
    case HELIOS_CPU_OSXSAVE: return "OSXSAVE";
    default: return "?";
    }
}

static void hcg_put_features(HcgText* t, uint32_t bits) {
    uint32_t i;
    int first = 1;
    for (i = 0; i < HELIOS_CPU_FEATURE_COUNT; ++i) {
        const uint32_t bit = 1u << i;
        if (!(bits & bit)) continue;
        if (!first) hcg_put(t, ", ");
        hcg_put(t, hcg_feature_name(bit));
        first = 0;
    }
}

/* ---- Evaluation --------------------------------------------------------------------------- */
static uint32_t hcg_bit(uint32_t reg, uint32_t bit, uint32_t feature) {
    return (reg >> bit) & 1u ? feature : 0u;
}

int helios_cpu_gate_run(const HeliosCpuidRaw* raw, uint32_t required, const char* display_name,
                        HeliosCpuGateReport* out) {
    HcgText msg;
    uint32_t i;
#if HCG_HAVE_X86
    HeliosCpuidRaw live;
    if (!raw) {
        hcg_read(&live);
        raw = &live;
    }
#else
    HeliosCpuidRaw none;
    if (!raw) {
        none.isX86 = 0; /* the only field read on non-x86 builds */
        raw = &none;
    }
#endif
    if (!display_name || !*display_name) display_name = "Helios";

    out->isX86 = raw->isX86;
    out->detected = out->usable = out->missing = 0;
    out->required = required;
    out->osAvxState = out->osAvx512State = 0;
    out->family = out->model = out->stepping = 0;
    for (i = 0; i < sizeof(out->vendor); ++i) out->vendor[i] = '\0';
    for (i = 0; i < sizeof(out->brand); ++i) out->brand[i] = '\0';
    msg.buf = out->message;
    msg.cap = (uint32_t)sizeof(out->message);
    msg.len = 0;
    out->message[0] = '\0';

    if (!raw->isX86) {
        out->supported = 1;
        hcg_put(&msg, "CPU gate: not an x86-64 build; no instruction-set requirement applies");
        return 1;
    }

    {
        /* Vendor: EBX, EDX, ECX of leaf 0. */
        uint32_t v[3];
        v[0] = raw->leaf0[1];
        v[1] = raw->leaf0[3];
        v[2] = raw->leaf0[2];
        hcg_regs_to_chars(v, 3, out->vendor);
        out->vendor[12] = '\0';
    }
    {
        const uint32_t eax = raw->leaf1[0];
        const uint32_t baseFamily = (eax >> 8) & 0xfu;
        const uint32_t baseModel = (eax >> 4) & 0xfu;
        out->stepping = eax & 0xfu;
        out->family = baseFamily == 0xfu ? baseFamily + ((eax >> 20) & 0xffu) : baseFamily;
        out->model = (baseFamily == 0x6u || baseFamily == 0xfu) ? (((eax >> 16) & 0xfu) << 4) | baseModel : baseModel;
    }
    {
        char brand[49];
        uint32_t start = 0, end;
        hcg_regs_to_chars(raw->brand, 12, brand);
        brand[48] = '\0';
        while (start < 48 && brand[start] == ' ') ++start;
        end = start;
        while (end < 48 && brand[end] != '\0') ++end;
        while (end > start && brand[end - 1] == ' ') --end;
        if (end > start) {
            for (i = 0; start + i < end; ++i) {
                const char c = brand[start + i];
                out->brand[i] = (c >= 32 && c < 127) ? c : '?';
            }
            out->brand[i] = '\0';
        } else {
            HcgText b;
            b.buf = out->brand;
            b.cap = (uint32_t)sizeof(out->brand);
            b.len = 0;
            hcg_put(&b, out->vendor[0] ? out->vendor : "unknown vendor");
            hcg_put(&b, " family ");
            hcg_put_u32(&b, out->family);
            hcg_put(&b, " model ");
            hcg_put_u32(&b, out->model);
        }
    }

    {
        const uint32_t ecx1 = raw->leaf1[2], edx1 = raw->leaf1[3];
        const uint32_t ebx7 = raw->leaf7[1];
        const uint32_t ecxE = raw->ext1[2];
        uint32_t f = 0;
        f |= hcg_bit(edx1, 26, HELIOS_CPU_SSE2);
        f |= hcg_bit(ecx1, 0, HELIOS_CPU_SSE3);
        f |= hcg_bit(ecx1, 9, HELIOS_CPU_SSSE3);
        f |= hcg_bit(ecx1, 12, HELIOS_CPU_FMA);
        f |= hcg_bit(ecx1, 13, HELIOS_CPU_CX16);
        f |= hcg_bit(ecx1, 19, HELIOS_CPU_SSE41);
        f |= hcg_bit(ecx1, 20, HELIOS_CPU_SSE42);
        f |= hcg_bit(ecx1, 22, HELIOS_CPU_MOVBE);
        f |= hcg_bit(ecx1, 23, HELIOS_CPU_POPCNT);
        f |= hcg_bit(ecx1, 26, HELIOS_CPU_XSAVE);
        f |= hcg_bit(ecx1, 27, HELIOS_CPU_OSXSAVE);
        f |= hcg_bit(ecx1, 28, HELIOS_CPU_AVX);
        f |= hcg_bit(ecx1, 29, HELIOS_CPU_F16C);
        f |= hcg_bit(ebx7, 3, HELIOS_CPU_BMI1);
        f |= hcg_bit(ebx7, 5, HELIOS_CPU_AVX2);
        f |= hcg_bit(ebx7, 8, HELIOS_CPU_BMI2);
        f |= hcg_bit(ebx7, 16, HELIOS_CPU_AVX512F);
        f |= hcg_bit(ecxE, 0, HELIOS_CPU_LAHF);
        f |= hcg_bit(ecxE, 5, HELIOS_CPU_LZCNT);
        out->detected = f;
    }
    {
        /* The OS must save SSE (bit 1) and AVX/YMM (bit 2) state across context switches, otherwise
         * AVX instructions fault (#UD) even though CPUID lists them. AVX-512 also needs opmask,
         * ZMM_Hi256 and Hi16_ZMM (bits 5-7). */
        const int osxsave = (out->detected & HELIOS_CPU_OSXSAVE) != 0;
        out->osAvxState = osxsave && (raw->xcr0 & 0x6u) == 0x6u;
        out->osAvx512State = out->osAvxState && (raw->xcr0 & 0xe0u) == 0xe0u;
        out->usable = out->detected;
        if (!out->osAvxState) {
            out->usable &= ~(uint32_t)(HELIOS_CPU_AVX | HELIOS_CPU_AVX2 | HELIOS_CPU_FMA | HELIOS_CPU_F16C |
                                       HELIOS_CPU_AVX512F);
        }
        if (!out->osAvx512State) out->usable &= ~(uint32_t)HELIOS_CPU_AVX512F;
    }
    out->missing = required & ~out->usable;
    out->supported = out->missing == 0;

    if (out->supported) {
        hcg_put(&msg, "CPU supported: ");
        hcg_put(&msg, out->brand);
        hcg_put(&msg, " (");
        hcg_put_features(&msg, out->usable & (HELIOS_CPU_AVX2 | HELIOS_CPU_FMA | HELIOS_CPU_BMI1 | HELIOS_CPU_BMI2 |
                                             HELIOS_CPU_F16C | HELIOS_CPU_LZCNT | HELIOS_CPU_POPCNT |
                                             HELIOS_CPU_AVX512F));
        hcg_put(&msg, ")");
        return 1;
    }
    /* Same wording as the launcher and client (08 §2.2). */
    hcg_put(&msg, display_name);
    if (required & HELIOS_CPU_AVX2) {
        hcg_put(&msg, " requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: ");
    } else {
        hcg_put(&msg, " requires an x86-64-v2 CPU (SSE4.2 and POPCNT). Detected: ");
    }
    hcg_put(&msg, out->brand);
    hcg_put(&msg, ". Missing: ");
    hcg_put_features(&msg, out->missing);
    hcg_put(&msg, ".");
    if ((out->detected & required & out->missing) != 0) {
        hcg_put(&msg, " The CPU has AVX, but the operating system has not enabled AVX (XSAVE/YMM) state.");
    }
    return 0;
}
