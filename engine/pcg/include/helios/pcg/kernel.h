// helios/pcg/kernel.h — CPU VM kernel selection (02 §5.8 "SIMD kernels", "Selection",
// "Measurement guard").
//
// The VM's op bodies are written once (src/kernels/vm_ops.inl) and compiled three times:
//   avx2    8 x i32 lanes, the budgeted default (src/kernels/vm_avx2.cpp)
//   sse42   4 x i32 lanes, the width-4 conformance twin (src/kernels/vm_sse42.cpp); never budgeted
//   scalar  1 lane, the reference and the template for a later NEON port (src/kernels/vm_scalar.cpp)
// Integer results are exact, so all three produce identical bits (RT-04).
//
// Selection: initializePcgModule() (called explicitly by the host at start-up, or lazily by the
// first TileEvaluator) reads the `pcg.kernel` CVar (default "avx2") and stores one kernel table.
// Until WP-0.2r builds whole images at the avx2 ISA level, every image cannot be assumed to have
// passed the CPU gate, so the choice is also checked against CPUID (core::cpuGate()): a kernel the
// CPU cannot run falls back to the widest one it can, with a warning. The chosen kernel is logged
// as "pcg.kernel=<name>" (WP-0.9c, RC-13, RT-20 and the RT-04 bench assert it).
//
// Threading: selection functions are thread-safe; the active kernel is read atomically.
#pragma once

#include <optional>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::pcg {

enum class KernelKind : u8 { Scalar = 0, Sse42 = 1, Avx2 = 2 };

/// "scalar", "sse42", "avx2".
std::string_view kernelName(KernelKind kind) noexcept;
std::optional<KernelKind> parseKernelName(std::string_view name) noexcept;
/// SIMD width in i32 lanes (1, 4, 8).
u32 kernelWidth(KernelKind kind) noexcept;

/// True if the kernel is compiled into this build and the CPU supports its instructions.
bool kernelSupported(KernelKind kind) noexcept;
/// Widest supported kernel.
KernelKind bestSupportedKernel() noexcept;

/// Reads `pcg.kernel`, selects the kernel (falling back per the header comment), logs
/// "pcg.kernel=<name>" and returns the choice. Idempotent; later calls re-read the CVar.
KernelKind initializePcgModule();

/// The kernel TileEvaluators use by default (initializes the module on first use).
KernelKind activeKernel() noexcept;
/// Forces a kernel (tests, `pcg.kernel` changes). Fails with Unsupported if the CPU cannot run it.
Result<void> selectKernel(KernelKind kind);

} // namespace helios::pcg
