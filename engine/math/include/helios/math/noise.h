// helios/math/noise.h — deterministic gradient/cellular noise for procedural generation.
//
// Bit-identical on every platform/compiler (ADR-006 deterministic procgen): the implementation
// (src/noise.cpp) uses only IEEE +, -, *, /, sqrt and integer hashing, floors via exact casts,
// and is compiled with FP contraction disabled. Golden-value tests in tests/test_noise.cpp fail
// if any platform ever diverges.
//
// Lattice gradients are selected by a seeded integer hash instead of Perlin's 256-entry permutation
// table: every lattice coordinate is mixed by its own seeded bijective hash (a permutation of the
// 32-bit axis) and the per-axis results are summed and mixed again. The pattern therefore only
// repeats when a coordinate wraps at 2^32 cells (no 256-unit period, important at planet scale)
// and it ports directly to shaders. (A single mix of a linear combination x*Px + y*Py + ... is NOT
// enough: it repeats along short kernel vectors, ~1.6k units in 3D and ~220 in 4D; see noise.cpp.)
//
// Algorithms (implemented from the publications, no third-party code):
//   perlin    K. Perlin, "Improving Noise" (SIGGRAPH 2002): quintic fade, edge gradients.
//   simplex   K. Perlin (2001) as described by S. Gustavson, "Simplex noise demystified" (2005);
//             kernel radius r^2 = 0.5 so the noise is continuous in every dimension.
//             (US patent 6,867,776 on simplex noise expired in 2022.)
//   fbm       fractional Brownian motion (sum of octaves), normalized to [-1, 1].
//   ridged    F. K. Musgrave's ridged multifractal (Texturing & Modeling, 3rd ed.), in [0, 1].
//   warp      domain warping (offset the input by vector-valued fBm).
//   cellular  S. Worley, "A Cellular Texture Basis Function" (1996): F1/F2 Euclidean distances
//             to jittered feature points (one per cell), exact search (expanding rings with
//             box-distance pruning, so F1/F2 are never missed even at jitter 1).
//
// Ranges: perlin/simplex/fbm are in [-1, 1] (scaled so the observed extremes over 10^7 samples
// are ~0.95..1, then clamped); ridged in [0, 1]; cellular distances in cell units.
// f32 overloads compute in f32 and f64 overloads in f64 (both deterministic, different values).
// Coordinates must satisfy |p| < 2^31 (f32) / 2^52 (f64); lattice hashing wraps every 2^32 cells
// per axis.
//
// Performance budget (x64 ~3 GHz, optimized build, one core, f64): perlin/simplex 2D <= 40 ns,
// 3D <= 60 ns, 4D <= 120 ns per sample; cellular 3D <= 350 ns; fbm = octaves x basis. The
// "noise throughput" test measures and reports these (its assertion is deliberately generous so
// Debug/shared CI machines pass; track the printed numbers). Reference run (GCC 13, -O2, shared
// container): perlin3 ~38 ns, simplex3 ~53 ns, simplex2 (f32) ~33 ns, cellular3 ~285 ns,
// 6-octave fbm3 ~470 ns.
//
// Threading: all functions are pure and thread-safe (no global tables).
#pragma once

#include "helios/math/vec.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios::noise {

// --- Gradient noise ----------------------------------------------------------------------------
[[nodiscard]] f32 perlin(const Vec2& p, u32 seed) noexcept;
[[nodiscard]] f32 perlin(const Vec3& p, u32 seed) noexcept;
[[nodiscard]] f32 perlin(const Vec4& p, u32 seed) noexcept;
[[nodiscard]] f64 perlin(const DVec2& p, u32 seed) noexcept;
[[nodiscard]] f64 perlin(const DVec3& p, u32 seed) noexcept;
[[nodiscard]] f64 perlin(const DVec4& p, u32 seed) noexcept;

[[nodiscard]] f32 simplex(const Vec2& p, u32 seed) noexcept;
[[nodiscard]] f32 simplex(const Vec3& p, u32 seed) noexcept;
[[nodiscard]] f32 simplex(const Vec4& p, u32 seed) noexcept;
[[nodiscard]] f64 simplex(const DVec2& p, u32 seed) noexcept;
[[nodiscard]] f64 simplex(const DVec3& p, u32 seed) noexcept;
[[nodiscard]] f64 simplex(const DVec4& p, u32 seed) noexcept;

// --- Fractal sums ------------------------------------------------------------------------------
enum class Basis : u8 { Perlin, Simplex };

struct FbmParams {
    int octaves = 6;        ///< 1..32
    f64 frequency = 1.0;    ///< frequency of the first octave
    f64 lacunarity = 2.0;   ///< frequency multiplier per octave
    f64 persistence = 0.5;  ///< amplitude multiplier per octave ("gain")
    Basis basis = Basis::Simplex;
};

/// Fractional Brownian motion; each octave uses an independent seed. Result in [-1, 1].
[[nodiscard]] f32 fbm(const Vec2& p, u32 seed, const FbmParams& params = {}) noexcept;
[[nodiscard]] f32 fbm(const Vec3& p, u32 seed, const FbmParams& params = {}) noexcept;
[[nodiscard]] f32 fbm(const Vec4& p, u32 seed, const FbmParams& params = {}) noexcept;
[[nodiscard]] f64 fbm(const DVec2& p, u32 seed, const FbmParams& params = {}) noexcept;
[[nodiscard]] f64 fbm(const DVec3& p, u32 seed, const FbmParams& params = {}) noexcept;
[[nodiscard]] f64 fbm(const DVec4& p, u32 seed, const FbmParams& params = {}) noexcept;

struct RidgedParams {
    int octaves = 6;
    f64 frequency = 1.0;
    f64 lacunarity = 2.0;
    f64 persistence = 0.5;  ///< spectral weight multiplier per octave (= lacunarity^-H)
    f64 offset = 1.0;       ///< ridge height; signal = (offset - |noise|)^2
    f64 gain = 2.0;         ///< feedback: weight of the next octave = clamp(signal * gain, 0, 1)
    Basis basis = Basis::Simplex;
};

/// Musgrave ridged multifractal, normalized to [0, 1] (sharp ridges near 1).
[[nodiscard]] f32 ridged(const Vec2& p, u32 seed, const RidgedParams& params = {}) noexcept;
[[nodiscard]] f32 ridged(const Vec3& p, u32 seed, const RidgedParams& params = {}) noexcept;
[[nodiscard]] f64 ridged(const DVec2& p, u32 seed, const RidgedParams& params = {}) noexcept;
[[nodiscard]] f64 ridged(const DVec3& p, u32 seed, const RidgedParams& params = {}) noexcept;

struct WarpParams {
    f64 amplitude = 1.0;  ///< maximum displacement (input units)
    FbmParams fbm{3, 1.0, 2.0, 0.5, Basis::Simplex};
};

/// Domain warp: p + amplitude * (fbm_x(p), fbm_y(p)[, fbm_z(p)]) with decorrelated seeds. Feed
/// the result into any other noise function.
[[nodiscard]] Vec2 domainWarp(const Vec2& p, u32 seed, const WarpParams& params = {}) noexcept;
[[nodiscard]] Vec3 domainWarp(const Vec3& p, u32 seed, const WarpParams& params = {}) noexcept;
[[nodiscard]] DVec2 domainWarp(const DVec2& p, u32 seed, const WarpParams& params = {}) noexcept;
[[nodiscard]] DVec3 domainWarp(const DVec3& p, u32 seed, const WarpParams& params = {}) noexcept;

// --- Cellular ----------------------------------------------------------------------------------
template <FloatingPoint T>
struct TCellular {
    T f1 = T(0);     ///< distance to the nearest feature point (cell units)
    T f2 = T(0);     ///< distance to the second nearest
    u32 cellId = 0;  ///< hash of the nearest feature point's cell (stable Voronoi region id)
};
using Cellular = TCellular<f32>;
using DCellular = TCellular<f64>;

/// A cellular feature point ("site") in absolute coordinates plus its region id.
struct CellularSite2 {
    DVec2 position;
    u32 id = 0;
};
struct CellularSite3 {
    DVec3 position;
    u32 id = 0;
};
/// The site of lattice cell `cell` exactly as cellular() uses it (id = TCellular::cellId when it
/// is the nearest site). Useful for placing objects at Voronoi sites (asteroids, settlements).
[[nodiscard]] CellularSite2 cellularSite(const IVec2& cell, u32 seed, f64 jitter = 1.0) noexcept;
[[nodiscard]] CellularSite3 cellularSite(const IVec3& cell, u32 seed, f64 jitter = 1.0) noexcept;

/// Worley noise with one feature point per unit cell, jittered by `jitter` in [0, 1]
/// (0 = regular grid, 1 = anywhere in the cell).
[[nodiscard]] Cellular cellular(const Vec2& p, u32 seed, f32 jitter = 1.0f) noexcept;
[[nodiscard]] Cellular cellular(const Vec3& p, u32 seed, f32 jitter = 1.0f) noexcept;
[[nodiscard]] DCellular cellular(const DVec2& p, u32 seed, f64 jitter = 1.0) noexcept;
[[nodiscard]] DCellular cellular(const DVec3& p, u32 seed, f64 jitter = 1.0) noexcept;

}  // namespace helios::noise

#pragma pop_macro("max")
#pragma pop_macro("min")
