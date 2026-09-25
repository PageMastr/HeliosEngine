// Internal interface between the tile evaluator and the three kernel TUs (vm_scalar.cpp,
// vm_sse42.cpp, vm_avx2.cpp). Kept free of inline functions: the kernel TUs are compiled with ISA
// flags above the image baseline, and an inline function emitted there could win the linker's
// COMDAT pick for callers outside the kernel (02 §1.1). Only plain data and declarations live here.
//
// Only integer ops go in the kernel TUs (02 §5.8): the `pcg_lint_kernels` CTest rejects `float` and
// `double` in src/kernels/.
#pragma once

#include "helios/core/types.h"

namespace helios::pcg::vm {

inline constexpr u32 kEdge = 65;
inline constexpr u32 kSamples = 65 * 65;
inline constexpr u32 kStride = (kSamples + 7) & ~7u;
inline constexpr u32 kHeightRegs = 16;
inline constexpr u32 kPosRegs = 4;
inline constexpr u32 kOpCount = 13; // Opcode::Count
inline constexpr u32 kInstrWords = 16;

/// SoA register file of one tile. Every plane holds kStride samples and is 64-byte aligned.
/// Positions are split into hi (i32) and lo (u32) words, the layout of the GPU twin's int2.
struct Registers {
    i64* h[kHeightRegs];
    i32* posHi[kPosRegs][3];
    u32* posLo[kPosRegs][3];
    const u32* sampleI; ///< sample column i (0..64) per index (padding entries repeat valid samples)
    const u32* sampleJ; ///< sample row j
};

/// Packed tile domain (TileDomain::pack()).
struct Domain {
    u32 w[16];
};

using DomainFn = void (*)(const Domain& domain, Registers& regs);
using OpFn = void (*)(const u32* instr, Registers& regs);

struct Kernels {
    const char* name;
    u32 width;
    DomainFn domain;       ///< fills p0
    OpFn ops[kOpCount];    ///< indexed by Opcode
};

/// Defined by the kernel TUs; a kernel that is not compiled for this target is null.
extern const Kernels* const kScalarKernels;
extern const Kernels* const kSse42Kernels;
extern const Kernels* const kAvx2Kernels;

} // namespace helios::pcg::vm
