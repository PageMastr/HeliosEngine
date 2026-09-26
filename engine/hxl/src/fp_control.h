// Private header: pins floating-point code generation for the HXL VM and curve sampling (06 §1.2
// cross-language float rules). Include it FIRST. Contraction (a*b+c -> fma) would change results
// between compilers. The guarantee is the build: CMake passes -ffp-contract=off to this target
// (GCC/Clang; clang-cl gets /clang:-ffp-contract=off) and MSVC's /fp:precise never contracts. The
// pragmas below are only a second line of defence against a stray global flag: GCC honours its
// pragma even under -ffp-contract=fast, but Clang documents that "fast" disregards pragmas, so a
// Clang build with -ffp-contract=fast would fuse regardless; the FMA-sensitive rows of the HXL
// corpus (tests/corpus/hxl) and the pinned golden hashes fail in that case. Same scheme as
// engine/math/src/fp_control.h.
#pragma once

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#if defined(__FAST_MATH__)
#error "HXL must not be compiled with -ffast-math (06 §1.2)"
#endif
