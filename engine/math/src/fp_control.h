// Private header: pins floating-point code generation for determinism-critical translation units
// (noise, packing, deterministic trig, cube-sphere mapping, colour). Include it FIRST in such
// .cpp files so it also covers every inline function the TU instantiates.
//
// Contraction (a*b+c -> fma) is the one IEEE-conforming optimisation that changes results between
// compilers/targets (the ADR-013 AVX2 baseline makes FMA available everywhere). Defences:
//  * engine/math/CMakeLists.txt compiles this module with -ffp-contract=off (GCC/Clang) and
//    /fp:precise (MSVC, which does not contract);
//  * the pragmas below keep contraction off even if those flags change: clang honours its pragma
//    in the default "on" mode (clang-cl maps /fp:precise to that mode), GCC honours the optimize
//    pragma even under -ffp-contract=fast, MSVC honours fp_contract.
// The golden-value tests (tests/test_noise.cpp, test_scalar.cpp, ...) catch any regression.
#pragma once

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#if defined(__FAST_MATH__)
#error "helios/math determinism-critical code must not be compiled with -ffast-math"
#endif
