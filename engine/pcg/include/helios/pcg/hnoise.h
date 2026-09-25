// helios/pcg/hnoise.h — fixed-point `hnoise`, the deterministic noise every terrain height is built
// from (02 §5.8, 03 §5.5 and §5.5a).
//
// Everything that feeds heights is integer arithmetic, so the CPU kernels (scalar, 4-lane SSE4.2,
// 8-lane AVX2) and the 32-bit-only Slang twin (shaders/pcg/hnoise.slang) produce the same bits on
// every compiler, CPU and GPU. This header is the normative scalar reference: the SIMD kernels and
// the Slang twin are checked against these functions by the conformance corpus
// (tests/corpus/hnoise). Changing any constant or rounding rule here changes every terrain in the
// game and must update the corpus, the Slang twin and all three kernels in the same change.
//
// Formats (helios/math/fixed.h):
//   * positions: Q32.32 metres per axis (i64, `FixedPos`); the f64 conversion is exact.
//   * noise internals: Q16.16 in an i32 (`Q16` raw); fractions are [0, 65535].
//   * face coordinates and unit directions: Q2.30 in an i32 (`Q30` below).
//   * heights: Q32.32 metres (`Q32`).
// Products round like helios::Fixed (add half an ulp, arithmetic shift: ties toward +infinity).
//
// Algorithm (sources, all implemented from the publications; no third-party code):
//   * Lattice hash: xxHash32 (Y. Collet, BSD-2; the vendored third_party/xxhash is used only by the
//     tests to prove equality) of the 12-byte little-endian cell (x, y, z) with the octave seed.
//   * Gradients: K. Perlin, "Improving Noise" (SIGGRAPH 2002): the 12 cube-edge gradients chosen by
//     the top four hash bits (16 entries, 4 repeated), quintic fade 6t^5 - 15t^4 + 10t^3 (evaluated with
//     a single rounding, see fade()).
//   * Octaves: wavelength 2^e metres, e decreasing by one per octave (lacunarity 2); octave k uses
//     seed + k * kOctaveSeedStep.
//   * Cube-sphere domain: math's CubeFace bases (helios/math/spherical.h), an odd degree-11
//     polynomial approximation of the EquiAngular warp tan(pi/4 * s) (max 1.2e-7 from tan, exactly
//     +-1 at the face edges so faces meet bit-exactly), and a 4-step integer Newton reciprocal square
//     root (<= 2 ulp of Q2.30 from the exact floor, math's rsqrtQ30).
//
// Threading: pure functions; thread-safe.
#pragma once

#include <array>

#include "helios/core/types.h"
#include "helios/math/fixed.h"
#include "helios/math/spherical.h"

namespace helios::pcg {

/// Q2.30 (face coordinates, unit directions).
using Q30 = Fixed<i32, 30>;

/// A position in Q32.32 metres (raw values).
struct FixedPos {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    friend constexpr bool operator==(const FixedPos&, const FixedPos&) = default;
};

/// A position split at an octave's lattice: the integer cell (mod 2^32) and the Q16 fraction.
struct LatticeCoord {
    std::array<i32, 3> cell{};
    std::array<i32, 3> frac{}; ///< [0, 65535]
};

namespace hnoise {

// xxHash32 primes (the lattice hash is XXH32 of the 12-byte cell).
inline constexpr u32 kPrime1 = 0x9E3779B1u;
inline constexpr u32 kPrime2 = 0x85EBCA77u;
inline constexpr u32 kPrime3 = 0xC2B2AE3Du;
inline constexpr u32 kPrime4 = 0x27D4EB2Fu;
inline constexpr u32 kPrime5 = 0x165667B1u;

/// Seed increment between octaves of one generator.
inline constexpr u32 kOctaveSeedStep = 0x9E3779B9u;
/// Seed increment between the three axis noises of a domain warp.
inline constexpr u32 kWarpAxisSeedStep = 0x632BE5ABu;

/// Octave wavelengths are 2^e metres with e in [kMinWavelengthExp, kMaxWavelengthExp].
inline constexpr i32 kMinWavelengthExp = -16;
inline constexpr i32 kMaxWavelengthExp = 30;

/// Face coordinates use the exact shift form a = (tile * 64 + i) << (25 - level) - 2^30, so the
/// cube-sphere domain supports levels 0..25 (4.7 mm sample spacing on a 6,400 km body).
inline constexpr u32 kMaxCubeLevel = 25;

/// EquiAngular warp w(s) = s * (c1 + s^2 (c3 + s^2 (c5 + s^2 (c7 + s^2 (c9 + s^2 c11))))) in Q2.30.
/// c1 is chosen so that c1 + ... + c11 == 2^30 exactly, i.e. w(+-1) == +-1 bit-exactly.
inline constexpr std::array<i32, 6> kWarpCoeffs = {
    (1 << 30) - (173443014 + 42458960 + 11716253 + 1124922 + 1685457), // c1 ~ 0.785397 (pi/4)
    173443014, 42458960, 11716253, 1124922, 1685457,                    // c3 .. c11
};
/// Slope of the Newton initial guess y0 = 1 - k (s - 1), k = (1 - 1/sqrt(3)) / 2, in Q2.30.
inline constexpr u32 kRsqrtSlopeQ30 = 226908346u;
inline constexpr u32 kRsqrtIterations = 4;

/// xxHash32 of the little-endian bytes of (x, y, z) with `seed`.
[[nodiscard]] constexpr u32 latticeHash(u32 seed, i32 x, i32 y, i32 z) noexcept {
    const auto round = [](u32 h, u32 v) {
        h += v * kPrime3;
        h = (h << 17) | (h >> 15);
        return h * kPrime4;
    };
    u32 h = seed + kPrime5 + 12u;
    h = round(h, static_cast<u32>(x));
    h = round(h, static_cast<u32>(y));
    h = round(h, static_cast<u32>(z));
    h ^= h >> 15;
    h *= kPrime2;
    h ^= h >> 13;
    h *= kPrime3;
    h ^= h >> 16;
    return h;
}

/// Perlin's improved-noise gradient (top four hash bits) dotted with the Q16 offset (x, y, z).
/// Inputs in [-65536, 65535]; result in [-131072, 131070].
[[nodiscard]] constexpr i32 gradientDot(u32 hash, i32 x, i32 y, i32 z) noexcept {
    const u32 g = hash >> 28;
    const i32 u = g < 8 ? x : y;
    const i32 v = g < 4 ? y : ((g == 12 || g == 14) ? x : z);
    return ((g & 1u) ? -u : u) + ((g & 2u) ? -v : v);
}

/// Q16 product with helios::Fixed rounding: (a * b + 2^15) >> 16.
[[nodiscard]] constexpr i32 mulQ16(i32 a, i32 b) noexcept {
    return static_cast<i32>((static_cast<i64>(a) * b + (i64(1) << 15)) >> 16);
}

/// Quintic fade 6t^5 - 15t^4 + 10t^3 of a Q16 fraction t in [0, 65535]; result in [0, 65536].
/// One rounding only, so the result is monotonic and within 0.54 ulp of the exact polynomial:
///   t2 = t^2 (exact, 32 bits), t3 = floor(t2 * t / 2^24) (t^3 with 24 fraction bits),
///   b  = 10 * 2^32 + t * (6t - 15 * 2^16) (exact, 64 bits), fade = (t3 * b + 2^39) >> 40.
/// Every twin needs only 32 x 32 -> 64 products for it (the GPU: two umulExtended, one imulExtended).
[[nodiscard]] constexpr i32 fade(i32 t) noexcept {
    const u32 tu = static_cast<u32>(t);
    const u32 t2 = tu * tu;
    const u32 t3 = static_cast<u32>((static_cast<u64>(t2) * tu) >> 24);
    const i64 b = (i64(10) << 32) + static_cast<i64>(t) * (6 * t - 15 * 65536);
    return static_cast<i32>((static_cast<u64>(t3) * static_cast<u64>(b) + (u64(1) << 39)) >> 40);
}

/// a + w (b - a) with w in [0, 65536] (Q16).
[[nodiscard]] constexpr i32 lerp(i32 a, i32 b, i32 w) noexcept { return a + mulQ16(w, b - a); }

/// Seed of octave k of a generator seeded with `seed`.
[[nodiscard]] constexpr u32 octaveSeed(u32 seed, u32 octave) noexcept { return seed + octave * kOctaveSeedStep; }

/// Splits a Q32.32 coordinate at wavelength 2^e metres: cell = floor(p / 2^(32+e)) mod 2^32,
/// frac = floor(p / 2^(16+e)) mod 2^16. Precondition: e in [kMinWavelengthExp, kMaxWavelengthExp].
constexpr void splitCoord(i64 p, i32 e, i32& cell, i32& frac) noexcept {
    cell = static_cast<i32>(static_cast<u32>(static_cast<u64>(p >> (32 + e))));
    frac = static_cast<i32>((p >> (16 + e)) & 0xFFFF);
}

/// Lattice cell and fraction of a position at wavelength 2^e metres.
[[nodiscard]] LatticeCoord latticeCoord(const FixedPos& p, i32 wavelengthExp) noexcept;

/// 3D gradient noise at a lattice coordinate, Q16 (range about [-1.04, 1.04]).
[[nodiscard]] i32 noise3(u32 seed, const LatticeCoord& c) noexcept;

/// Q2.30 product (rounded, like Q30's operator*). Results must fit in an i32.
[[nodiscard]] constexpr i32 mulQ30(i32 a, i32 b) noexcept {
    return static_cast<i32>((static_cast<i64>(a) * b + (i64(1) << 29)) >> 30);
}
/// Unsigned Q2.30 product (rounded). Results must fit in a u32.
[[nodiscard]] constexpr u32 mulQ30u(u32 a, u32 b) noexcept {
    return static_cast<u32>((static_cast<u64>(a) * b + (u64(1) << 29)) >> 30);
}

/// The fixed-point EquiAngular warp of a Q2.30 face coordinate in [-2^30, 2^30].
[[nodiscard]] i32 equiAngularWarp(i32 sQ30) noexcept;

/// 1 / sqrt(s) for s in [1, 3] (Q2.30 in, Q2.30 out) by kRsqrtIterations integer Newton steps.
[[nodiscard]] u32 rsqrtNewtonQ30(u32 sQ30) noexcept;

/// Q2.30 face coordinate of sample `sample` (0..64) of tile column/row `tile` at `level`:
/// (tile * 64 + sample) * 2^(25 - level) - 2^30. Precondition: level <= kMaxCubeLevel.
[[nodiscard]] constexpr i32 faceCoordQ30(u32 tile, u32 sample, u32 level) noexcept {
    return static_cast<i32>(((tile * 64u + sample) << (25u - level)) - (1u << 30));
}

/// Unit direction (Q2.30 per axis) of face coordinates (s, t) on `face`, EquiAngular.
[[nodiscard]] std::array<i32, 3> cubeSphereDirection(CubeFace face, i32 sQ30, i32 tQ30) noexcept;

/// Position (Q32.32 metres) on a sphere of radius radiusQ8 (Q24.8 metres, < 2^31) of face
/// coordinates (s, t): floor(direction * radius / 2^6).
[[nodiscard]] FixedPos cubeSpherePosition(CubeFace face, i32 sQ30, i32 tQ30, u32 radiusQ8) noexcept;

/// Radius in metres to Q24.8 (rounded, clamped to [0, 2^31 - 1]).
[[nodiscard]] u32 radiusToQ8(f64 metres) noexcept;

} // namespace hnoise
} // namespace helios::pcg
