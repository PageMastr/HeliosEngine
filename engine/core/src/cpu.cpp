// C++ face of the CPU gate. The probe is the C unit cpugate/cpu_gate.c (x86-64-v1, no AVX2 flags);
// this file only converts its report, so it may be compiled at any ISA level.
#include "helios/core/cpu.h"

#include <algorithm>
#include <cstring>

#include "cpugate/cpu_gate.h"

namespace helios {

namespace {

static_assert(static_cast<u32>(CpuFeature::Count) == HELIOS_CPU_FEATURE_COUNT);
static_assert((1u << static_cast<u32>(CpuFeature::AVX2)) == HELIOS_CPU_AVX2);
static_assert((1u << static_cast<u32>(CpuFeature::BMI2)) == HELIOS_CPU_BMI2);
static_assert((1u << static_cast<u32>(CpuFeature::OSXSAVE)) == HELIOS_CPU_OSXSAVE);

constexpr std::array<std::string_view, static_cast<usize>(CpuFeature::Count)> kFeatureNames = {
    "SSE2", "SSE3", "SSSE3", "SSE4.1", "SSE4.2", "POPCNT", "CMPXCHG16B", "LAHF-SAHF", "AVX", "AVX2",
    "FMA",  "BMI1", "BMI2",  "F16C",   "LZCNT",  "MOVBE",  "AVX-512F",   "XSAVE",     "OSXSAVE",
};

u32 requiredBits(CpuRequirement requirement) noexcept {
    return requirement == CpuRequirement::X86_64_V2 ? static_cast<u32>(HELIOS_CPU_REQUIRE_V2)
                                                    : static_cast<u32>(HELIOS_CPU_REQUIRE_AVX2_IMAGE);
}

CpuGateReport convert(const HeliosCpuGateReport& r) {
    CpuGateReport out;
    out.supported = r.supported != 0;
    out.isX86 = r.isX86 != 0;
    out.osAvxState = r.osAvxState != 0;
    out.osAvx512State = r.osAvx512State != 0;
    out.detected.bits = r.detected;
    out.usable.bits = r.usable;
    out.required.bits = r.required;
    out.missing.bits = r.missing;
    out.family = r.family;
    out.model = r.model;
    out.stepping = r.stepping;
    out.vendor = r.vendor;
    out.brand = r.brand;
    out.message = r.message;
    return out;
}

CpuGateReport run(const HeliosCpuidRaw* raw, CpuRequirement requirement, std::string_view displayName) {
    char name[64] = {};
    usize n = std::min<usize>(displayName.size(), sizeof(name) - 1);
    // Never cut a UTF-8 sequence in half (the name ends up in a user-facing message).
    if (n < displayName.size()) {
        while (n > 0 && (static_cast<u8>(displayName[n]) & 0xC0u) == 0x80u) --n;
    }
    if (n != 0) std::memcpy(name, displayName.data(), n); // data() may be null for an empty view
    HeliosCpuGateReport report;
    helios_cpu_gate_run(raw, requiredBits(requirement), n ? name : nullptr, &report);
    return convert(report);
}

} // namespace

std::string_view cpuFeatureName(CpuFeature feature) noexcept {
    const auto i = static_cast<usize>(feature);
    return i < kFeatureNames.size() ? kFeatureNames[i] : std::string_view("?");
}

std::string CpuFeatureSet::toString() const {
    std::string out;
    for (u32 i = 0; i < static_cast<u32>(CpuFeature::Count); ++i) {
        if (!has(static_cast<CpuFeature>(i))) continue;
        if (!out.empty()) out += ", ";
        out += cpuFeatureName(static_cast<CpuFeature>(i));
    }
    return out;
}

CpuFeatureSet cpuRequiredFeatures(CpuRequirement requirement) noexcept {
    CpuFeatureSet set;
    set.bits = requiredBits(requirement);
    return set;
}

const CpuGateReport& cpuGate() noexcept {
    static const CpuGateReport report = run(nullptr, CpuRequirement::Avx2Image, {});
    return report;
}

CpuGateReport cpuGate(CpuRequirement requirement, std::string_view displayName) {
    return run(nullptr, requirement, displayName);
}

CpuGateReport cpuGateEvaluate(const CpuidSnapshot& snapshot, CpuRequirement requirement,
                              std::string_view displayName) {
    HeliosCpuidRaw raw;
    std::memcpy(raw.leaf0, snapshot.leaf0.data(), sizeof(raw.leaf0));
    std::memcpy(raw.leaf1, snapshot.leaf1.data(), sizeof(raw.leaf1));
    std::memcpy(raw.leaf7, snapshot.leaf7.data(), sizeof(raw.leaf7));
    std::memcpy(raw.ext0, snapshot.ext0.data(), sizeof(raw.ext0));
    std::memcpy(raw.ext1, snapshot.ext1.data(), sizeof(raw.ext1));
    std::memcpy(raw.brand, snapshot.brand.data(), sizeof(raw.brand));
    raw.xcr0 = snapshot.xcr0;
    raw.isX86 = snapshot.isX86 ? 1 : 0;
    return run(&raw, requirement, displayName);
}

} // namespace helios
