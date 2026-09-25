#pragma once
// CPU identification and the CPU gate (02 §1.1 "ISA levels", 08 §2.2, WP-0.5).
//
// Every AVX2 image (client, cell, editor, bot, gateway, voice, tools) links a pre-initializer that
// runs the same check before any C++ initializer and exits with kCpuGateExitCode on an unsupported
// CPU (helios_cpu_gate() in cmake/HeliosIsa.cmake). cpuGate() exposes the check to code: the
// launcher shows the message in its UI, `--gate-report` style tooling prints it, and kernels that
// dispatch on ISA (pcg's *_avx2.cpp) read the usable features from it.
//
// The probe itself lives in a C translation unit compiled at the x86-64-v1 baseline with no AVX2
// flags (engine/core/src/cpugate/cpu_gate.c; the ISA audit checks its flags and disassembly), so
// calling cpuGate() is safe on any x86-64 CPU.
//
// Threading: all functions are thread-safe. cpuGate() probes once and caches the report.

#include <array>
#include <string>
#include <string_view>

#include "helios/core/types.h"

namespace helios {

/// Instruction-set features reported by the gate (bit positions of CpuFeatureSet).
enum class CpuFeature : u8 {
    SSE2 = 0,
    SSE3,
    SSSE3,
    SSE41,
    SSE42,
    POPCNT,
    CX16,
    LAHF,
    AVX,
    AVX2,
    FMA,
    BMI1,
    BMI2,
    F16C,
    LZCNT,
    MOVBE,
    AVX512F,
    XSAVE,
    OSXSAVE,
    Count
};

/// Short display name ("AVX2", "SSE4.2", "AVX-512F").
std::string_view cpuFeatureName(CpuFeature feature) noexcept;

/// A set of CpuFeature bits.
struct CpuFeatureSet {
    u32 bits = 0;
    constexpr bool has(CpuFeature f) const noexcept { return (bits >> static_cast<u32>(f)) & 1u; }
    constexpr bool containsAll(CpuFeatureSet other) const noexcept { return (bits & other.bits) == other.bits; }
    constexpr CpuFeatureSet& add(CpuFeature f) noexcept {
        bits |= 1u << static_cast<u32>(f);
        return *this;
    }
    constexpr bool empty() const noexcept { return bits == 0; }
    /// Comma-separated names in enum order ("AVX, AVX2, BMI1").
    std::string toString() const;
    friend constexpr bool operator==(CpuFeatureSet, CpuFeatureSet) = default;
};

/// Requirement levels. Avx2Image is what every AVX2 executable needs (08 §2.2 plus BMI2): the
/// x86-64-v2 floor plus AVX, AVX2, BMI1, BMI2, F16C, LZCNT and OS-enabled YMM state. FMA is reported
/// but never required (Helios does not emit FMA, for determinism).
enum class CpuRequirement : u8 {
    X86_64_V2, ///< The bootstrap's floor (08 §2.1.1): SSE3, SSSE3, SSE4.1/4.2, POPCNT, CX16, LAHF.
    Avx2Image,
};

/// Features a requirement level needs.
CpuFeatureSet cpuRequiredFeatures(CpuRequirement requirement) noexcept;

/// Exit code of an executable refused by the pre-initializer gate (EX_CONFIG).
inline constexpr int kCpuGateExitCode = 78;

/// Raw CPUID/XGETBV register values; tests fill one to emulate other CPUs.
struct CpuidSnapshot {
    std::array<u32, 4> leaf0{}; ///< CPUID(0) EAX EBX ECX EDX
    std::array<u32, 4> leaf1{}; ///< CPUID(1)
    std::array<u32, 4> leaf7{}; ///< CPUID(7, 0)
    std::array<u32, 4> ext0{};  ///< CPUID(0x80000000)
    std::array<u32, 4> ext1{};  ///< CPUID(0x80000001)
    std::array<u32, 12> brand{}; ///< CPUID(0x80000002..0x80000004)
    u64 xcr0 = 0;                ///< XGETBV(0)
    bool isX86 = true;
};

/// The gate's verdict.
struct CpuGateReport {
    bool supported = false;   ///< usable features contain every required one
    bool isX86 = false;       ///< false on non-x86 builds (then supported = true, nothing else is set)
    bool osAvxState = false;  ///< OSXSAVE set and XCR0 enables SSE+YMM state
    bool osAvx512State = false;
    CpuFeatureSet detected;   ///< CPUID bits
    CpuFeatureSet usable;     ///< detected minus AVX-class features without OS state support
    CpuFeatureSet required;
    CpuFeatureSet missing;    ///< required minus usable
    u32 family = 0;
    u32 model = 0;
    u32 stepping = 0;
    std::string vendor;       ///< "GenuineIntel", "AuthenticAMD", ...
    std::string brand;        ///< trimmed brand string ("Intel(R) Core(TM) i7-4770 CPU @ 3.40GHz")
    /// Human-readable verdict: "<name> requires an AVX2 CPU (Intel Haswell / AMD Excavator or
    /// newer). Detected: <brand>. Missing: AVX2, BMI2." or "CPU supported: <brand> (...)".
    std::string message;
};

/// Probes the running CPU against CpuRequirement::Avx2Image (cached after the first call).
const CpuGateReport& cpuGate() noexcept;
/// Probes the running CPU against an explicit requirement; `displayName` names the product in the
/// message (default "Helios").
CpuGateReport cpuGate(CpuRequirement requirement, std::string_view displayName = {});
/// Evaluates recorded or synthetic CPUID values (tests, crash reports from other machines).
CpuGateReport cpuGateEvaluate(const CpuidSnapshot& snapshot, CpuRequirement requirement = CpuRequirement::Avx2Image,
                              std::string_view displayName = {});

} // namespace helios
