// CPU VM kernel selection (helios/pcg/kernel.h).
#include "helios/pcg/kernel.h"

#include <atomic>
#include <mutex>
#include <string>

#include "helios/core/cpu.h"
#include "helios/core/cvar.h"
#include "helios/core/log.h"
#include "kernels/vm_kernels.h"

HELIOS_LOG_CHANNEL(LogPcg, "Pcg");

namespace helios::pcg {

namespace {

HELIOS_CVAR(std::string, cvarPcgKernel, "pcg.kernel", "avx2",
            "Terrain VM kernel: avx2 (default, budgeted), sse42 (4-lane conformance twin) or scalar (reference)");

std::atomic<u8> g_active{static_cast<u8>(KernelKind::Scalar)};
std::atomic<bool> g_initialized{false};
std::mutex g_initMutex;

bool cpuSupports(KernelKind kind) noexcept {
    const CpuGateReport& gate = cpuGate();
    if (!gate.isX86) return kind == KernelKind::Scalar;
    switch (kind) {
        case KernelKind::Avx2:
            // vm_avx2.cpp is compiled with the whole avx2 flag set (BMI1/2, LZCNT, POPCNT, F16C), so the
            // compiler may emit any of them: require the full image level, not just AVX2.
            return gate.usable.containsAll(cpuRequiredFeatures(CpuRequirement::Avx2Image));
        case KernelKind::Sse42:
            return gate.usable.has(CpuFeature::SSE42) && gate.usable.has(CpuFeature::SSE41) &&
                   gate.usable.has(CpuFeature::SSSE3) && gate.usable.has(CpuFeature::POPCNT);
        default: return true;
    }
}

const vm::Kernels* compiledKernels(KernelKind kind) noexcept {
    switch (kind) {
        case KernelKind::Avx2: return vm::kAvx2Kernels;
        case KernelKind::Sse42: return vm::kSse42Kernels;
        default: return vm::kScalarKernels;
    }
}

} // namespace

std::string_view kernelName(KernelKind kind) noexcept {
    switch (kind) {
        case KernelKind::Avx2: return "avx2";
        case KernelKind::Sse42: return "sse42";
        default: return "scalar";
    }
}

std::optional<KernelKind> parseKernelName(std::string_view name) noexcept {
    if (name == "avx2") return KernelKind::Avx2;
    if (name == "sse42") return KernelKind::Sse42;
    if (name == "scalar") return KernelKind::Scalar;
    return std::nullopt;
}

u32 kernelWidth(KernelKind kind) noexcept {
    const vm::Kernels* k = compiledKernels(kind);
    return k ? k->width : 0;
}

bool kernelSupported(KernelKind kind) noexcept { return compiledKernels(kind) != nullptr && cpuSupports(kind); }

KernelKind bestSupportedKernel() noexcept {
    if (kernelSupported(KernelKind::Avx2)) return KernelKind::Avx2;
    if (kernelSupported(KernelKind::Sse42)) return KernelKind::Sse42;
    return KernelKind::Scalar;
}

KernelKind initializePcgModule() {
    std::lock_guard lock(g_initMutex);
    const std::string requested = cvarPcgKernel.get();
    KernelKind kind = KernelKind::Avx2;
    if (const auto parsed = parseKernelName(requested)) {
        kind = *parsed;
    } else {
        HELIOS_LOG_WARN(LogPcg, "pcg.kernel='{}' is not avx2, sse42 or scalar; using avx2", requested);
    }
    if (!kernelSupported(kind)) {
        const KernelKind fallback = bestSupportedKernel();
        HELIOS_LOG_WARN(LogPcg, "pcg.kernel={} is not supported by this CPU or build; falling back to {}",
                        kernelName(kind), kernelName(fallback));
        kind = fallback;
    }
    g_active.store(static_cast<u8>(kind), std::memory_order_release);
    const bool first = !g_initialized.exchange(true, std::memory_order_acq_rel);
    HELIOS_LOG_INFO(LogPcg, "pcg.kernel={} ({} lanes){}", kernelName(kind), kernelWidth(kind),
                    first ? "" : " (re-selected)");
    return kind;
}

KernelKind activeKernel() noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) initializePcgModule();
    return static_cast<KernelKind>(g_active.load(std::memory_order_acquire));
}

Result<void> selectKernel(KernelKind kind) {
    if (!kernelSupported(kind)) {
        return makeError(ErrorCode::Unsupported, "pcg kernel '{}' is not supported by this CPU or build", kernelName(kind));
    }
    std::lock_guard lock(g_initMutex);
    g_active.store(static_cast<u8>(kind), std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
    HELIOS_LOG_INFO(LogPcg, "pcg.kernel={} ({} lanes) (selected)", kernelName(kind), kernelWidth(kind));
    return {};
}

} // namespace helios::pcg
