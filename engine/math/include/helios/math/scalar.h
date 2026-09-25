// helios/math/scalar.h — scalar types, constants and scalar helpers.
//
// Part of the leaf math module (std only). Everything here is header-only and constexpr where
// the language allows; the deterministic trigonometry in `helios::det` lives in
// src/deterministic.cpp so its floating-point contraction is controlled in exactly one place.
//
// Threading: every function in this header is pure and thread-safe.
#pragma once

#include <cassert>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

// The math module is a leaf (it must not depend on helios/core), so it has its own assertion hook.
// Defaults to <cassert>; a build may redefine it (e.g. to HELIOS_ASSERT) before including.
#ifndef HELIOS_MATH_ASSERT
#define HELIOS_MATH_ASSERT(expr) assert(expr)
#endif

namespace helios {

// Fixed-width aliases. These are identical to the helios/core aliases; redeclaring an alias to
// the same type is legal C++, so both headers can be included together.
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;

/// Any built-in integer or floating-point type.
template <class T>
concept Arithmetic = std::is_arithmetic_v<T>;
/// f32 or f64 (long double is not used by the engine).
template <class T>
concept FloatingPoint = std::floating_point<T>;

// ---------------------------------------------------------------------------------------------
// Constants. Generic versions are variable templates (kPiT<T>); f32 versions have no suffix and
// f64 versions end in D. Angles are always radians.
// ---------------------------------------------------------------------------------------------
template <FloatingPoint T>
inline constexpr T kPiT = std::numbers::pi_v<T>;
template <FloatingPoint T>
inline constexpr T kTwoPiT = T(2) * std::numbers::pi_v<T>;
template <FloatingPoint T>
inline constexpr T kHalfPiT = T(0.5) * std::numbers::pi_v<T>;
template <FloatingPoint T>
inline constexpr T kQuarterPiT = T(0.25) * std::numbers::pi_v<T>;
template <FloatingPoint T>
inline constexpr T kInvPiT = std::numbers::inv_pi_v<T>;
template <FloatingPoint T>
inline constexpr T kInvTwoPiT = T(0.5) * std::numbers::inv_pi_v<T>;
template <FloatingPoint T>
inline constexpr T kDegToRadT = std::numbers::pi_v<T> / T(180);
template <FloatingPoint T>
inline constexpr T kRadToDegT = T(180) / std::numbers::pi_v<T>;
template <FloatingPoint T>
inline constexpr T kSqrt2T = std::numbers::sqrt2_v<T>;
template <FloatingPoint T>
inline constexpr T kInvSqrt2T = T(0.7071067811865475244008443621048490L);
template <FloatingPoint T>
inline constexpr T kSqrt3T = std::numbers::sqrt3_v<T>;
/// Default tolerance for approximate comparisons: 1e-6 for f32, 1e-12 for f64.
template <FloatingPoint T>
inline constexpr T kEpsilonT = sizeof(T) == 4 ? T(1e-6) : T(1e-12);
template <FloatingPoint T>
inline constexpr T kInfinityT = std::numeric_limits<T>::infinity();

inline constexpr f32 kPi = kPiT<f32>;
inline constexpr f32 kTwoPi = kTwoPiT<f32>;
inline constexpr f32 kHalfPi = kHalfPiT<f32>;
inline constexpr f32 kQuarterPi = kQuarterPiT<f32>;
inline constexpr f32 kInvPi = kInvPiT<f32>;
inline constexpr f32 kInvTwoPi = kInvTwoPiT<f32>;
inline constexpr f32 kDegToRad = kDegToRadT<f32>;
inline constexpr f32 kRadToDeg = kRadToDegT<f32>;
inline constexpr f32 kSqrt2 = kSqrt2T<f32>;
inline constexpr f32 kInvSqrt2 = kInvSqrt2T<f32>;
inline constexpr f32 kSqrt3 = kSqrt3T<f32>;
inline constexpr f32 kEpsilon = kEpsilonT<f32>;
inline constexpr f32 kInfinity = kInfinityT<f32>;

inline constexpr f64 kPiD = kPiT<f64>;
inline constexpr f64 kTwoPiD = kTwoPiT<f64>;
inline constexpr f64 kHalfPiD = kHalfPiT<f64>;
inline constexpr f64 kQuarterPiD = kQuarterPiT<f64>;
inline constexpr f64 kInvPiD = kInvPiT<f64>;
inline constexpr f64 kInvTwoPiD = kInvTwoPiT<f64>;
inline constexpr f64 kDegToRadD = kDegToRadT<f64>;
inline constexpr f64 kRadToDegD = kRadToDegT<f64>;
inline constexpr f64 kSqrt2D = kSqrt2T<f64>;
inline constexpr f64 kInvSqrt2D = kInvSqrt2T<f64>;
inline constexpr f64 kSqrt3D = kSqrt3T<f64>;
inline constexpr f64 kEpsilonD = kEpsilonT<f64>;
inline constexpr f64 kInfinityD = kInfinityT<f64>;

// ---------------------------------------------------------------------------------------------
// Basic scalar helpers. Deliberately named like their std counterparts but constexpr and
// constrained to arithmetic types so vector overloads in vec.h never collide with them.
// ---------------------------------------------------------------------------------------------

/// Smaller of a and b (returns a when equal, like std::min).
template <Arithmetic T>
[[nodiscard]] constexpr T min(T a, T b) noexcept {
    return b < a ? b : a;
}
/// Larger of a and b (returns a when equal, like std::max).
template <Arithmetic T>
[[nodiscard]] constexpr T max(T a, T b) noexcept {
    return a < b ? b : a;
}
/// Absolute value. For floats, -0 maps to +0 at runtime (std::fabs).
template <Arithmetic T>
[[nodiscard]] constexpr T abs(T x) noexcept {
    if constexpr (std::is_unsigned_v<T>) {
        return x;
    } else {
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::is_constant_evaluated()) return std::fabs(x);
        }
        return x < T(0) ? -x : x;
    }
}
/// -1, 0 or +1 (as T). NaN maps to 0.
template <Arithmetic T>
[[nodiscard]] constexpr T sign(T x) noexcept {
    if constexpr (std::is_unsigned_v<T>) {
        return x > T(0) ? T(1) : T(0);
    } else {
        return x > T(0) ? T(1) : (x < T(0) ? T(-1) : T(0));
    }
}
/// Clamps x to [lo, hi]. Precondition: lo <= hi. NaN propagates.
template <Arithmetic T>
[[nodiscard]] constexpr T clamp(T x, std::type_identity_t<T> lo, std::type_identity_t<T> hi) noexcept {
    return x < lo ? lo : (hi < x ? hi : x);
}
/// Clamps to [0, 1].
template <FloatingPoint T>
[[nodiscard]] constexpr T saturate(T x) noexcept {
    return clamp(x, T(0), T(1));
}
/// x * x.
template <Arithmetic T>
[[nodiscard]] constexpr T square(T x) noexcept {
    return x * x;
}
/// Linear interpolation a + (b - a) * t. Exact at t = 0; not clamped.
template <FloatingPoint T>
[[nodiscard]] constexpr T lerp(T a, T b, std::type_identity_t<T> t) noexcept {
    return a + (b - a) * t;
}
/// Inverse of lerp: the t for which lerp(a, b, t) == x. Precondition: a != b.
template <FloatingPoint T>
[[nodiscard]] constexpr T inverseLerp(T a, T b, std::type_identity_t<T> x) noexcept {
    return (x - a) / (b - a);
}
/// Maps x from [inA, inB] to [outA, outB] linearly (not clamped). Precondition: inA != inB.
template <FloatingPoint T>
[[nodiscard]] constexpr T remap(T x, std::type_identity_t<T> inA, std::type_identity_t<T> inB,
                                std::type_identity_t<T> outA, std::type_identity_t<T> outB) noexcept {
    return lerp(outA, outB, inverseLerp(inA, inB, x));
}
/// remap() with the input parameter clamped to [0, 1] first.
template <FloatingPoint T>
[[nodiscard]] constexpr T remapClamped(T x, std::type_identity_t<T> inA, std::type_identity_t<T> inB,
                                       std::type_identity_t<T> outA, std::type_identity_t<T> outB) noexcept {
    return lerp(outA, outB, saturate(inverseLerp(inA, inB, x)));
}
/// Hermite smoothstep: 0 for x <= e0, 1 for x >= e1, 3t^2 - 2t^3 in between. Precondition: e0 != e1.
template <FloatingPoint T>
[[nodiscard]] constexpr T smoothstep(T e0, std::type_identity_t<T> e1, std::type_identity_t<T> x) noexcept {
    const T t = saturate((x - e0) / (e1 - e0));
    return t * t * (T(3) - T(2) * t);
}
/// Perlin's smootherstep (C2 continuous): 6t^5 - 15t^4 + 10t^3 on the clamped parameter.
template <FloatingPoint T>
[[nodiscard]] constexpr T smootherstep(T e0, std::type_identity_t<T> e1, std::type_identity_t<T> x) noexcept {
    const T t = saturate((x - e0) / (e1 - e0));
    return t * t * t * (t * (t * T(6) - T(15)) + T(10));
}
/// 0 if x < edge, else 1.
template <FloatingPoint T>
[[nodiscard]] constexpr T step(T edge, std::type_identity_t<T> x) noexcept {
    return x < edge ? T(0) : T(1);
}
/// x - floor(x), in [0, 1).
template <FloatingPoint T>
[[nodiscard]] inline T fract(T x) noexcept {
    return x - std::floor(x);
}
/// Moves `current` towards `target` by at most `maxDelta` (>= 0) without overshooting.
template <FloatingPoint T>
[[nodiscard]] constexpr T moveTowards(T current, std::type_identity_t<T> target,
                                      std::type_identity_t<T> maxDelta) noexcept {
    const T d = target - current;
    if (abs(d) <= maxDelta) return target;
    return current + sign(d) * maxDelta;
}

/// True if |a - b| <= max(absTol, relTol * max(|a|, |b|)). Equal infinities compare equal; NaN
/// never does.
template <FloatingPoint T>
[[nodiscard]] constexpr bool approxEqual(T a, std::type_identity_t<T> b,
                                         std::type_identity_t<T> absTol = kEpsilonT<T>,
                                         std::type_identity_t<T> relTol = kEpsilonT<T>) noexcept {
    if (a == b) return true;
    const T diff = abs(a - b);
    if (!(diff < kInfinityT<T>)) return false;  // unequal infinities, NaN or overflow
    return diff <= max(absTol, relTol * max(abs(a), abs(b)));
}
/// True if |x| <= tol.
template <FloatingPoint T>
[[nodiscard]] constexpr bool approxZero(T x, std::type_identity_t<T> tol = kEpsilonT<T>) noexcept {
    return abs(x) <= tol;
}
/// True unless x is infinite or NaN.
template <FloatingPoint T>
[[nodiscard]] inline bool isFinite(T x) noexcept {
    return std::isfinite(x);
}

/// 1 / sqrt(x). Precondition: x > 0.
template <FloatingPoint T>
[[nodiscard]] inline T rsqrt(T x) noexcept {
    return T(1) / std::sqrt(x);
}
/// 1 / sqrt(x), or `fallback` when x is zero, denormal, negative, infinite or NaN.
template <FloatingPoint T>
[[nodiscard]] inline T safeRsqrt(T x, std::type_identity_t<T> fallback = T(0)) noexcept {
    return (x >= std::numeric_limits<T>::min() && x <= std::numeric_limits<T>::max()) ? T(1) / std::sqrt(x)
                                                                                      : fallback;
}
/// a / b, or `fallback` when |b| is below the smallest normal number (or NaN).
template <FloatingPoint T>
[[nodiscard]] constexpr T safeDiv(T a, std::type_identity_t<T> b,
                                  std::type_identity_t<T> fallback = T(0)) noexcept {
    return abs(b) >= std::numeric_limits<T>::min() ? a / b : fallback;
}

/// Degrees to radians.
template <FloatingPoint T>
[[nodiscard]] constexpr T radians(T degrees) noexcept {
    return degrees * kDegToRadT<T>;
}
/// Radians to degrees.
template <FloatingPoint T>
[[nodiscard]] constexpr T degrees(T radians) noexcept {
    return radians * kRadToDegT<T>;
}

/// Wraps an angle to [-pi, pi]. Uses IEEE remainder, which is exact and therefore identical on
/// every platform (relative to the rounded value of 2*pi).
template <FloatingPoint T>
[[nodiscard]] inline T wrapAngle(T angle) noexcept {
    return std::remainder(angle, kTwoPiT<T>);
}
/// Wraps an angle to [0, 2*pi).
template <FloatingPoint T>
[[nodiscard]] inline T wrapAnglePositive(T angle) noexcept {
    T r = std::remainder(angle, kTwoPiT<T>);
    if (r < T(0)) {
        r += kTwoPiT<T>;
        if (r >= kTwoPiT<T>) r = T(0);  // tiny negative inputs round up to exactly 2*pi
    }
    return r;
}
/// Shortest signed angle from `from` to `to`, in [-pi, pi].
template <FloatingPoint T>
[[nodiscard]] inline T angleDelta(T from, std::type_identity_t<T> to) noexcept {
    return wrapAngle(to - from);
}
/// Interpolates angles along the shortest arc. The result is not wrapped.
template <FloatingPoint T>
[[nodiscard]] inline T lerpAngle(T a, std::type_identity_t<T> b, std::type_identity_t<T> t) noexcept {
    return a + angleDelta(a, b) * t;
}

// ---------------------------------------------------------------------------------------------
// Fast deterministic conversions. Casts are exact, so these give identical results on every
// compiler, unlike code relying on the current rounding mode (nearbyint/lrint).
// ---------------------------------------------------------------------------------------------

/// floor(x) as i32 via truncating casts. Precondition: x in (-2^31, 2^31) and not NaN.
template <FloatingPoint T>
[[nodiscard]] constexpr i32 floorToI32(T x) noexcept {
    HELIOS_MATH_ASSERT(x > T(-2147483648.0) && x < T(2147483648.0));
    const i32 i = static_cast<i32>(x);
    return static_cast<T>(i) > x ? i - 1 : i;
}
/// floor(x) as i64. Precondition: |x| < 2^63 and not NaN.
template <FloatingPoint T>
[[nodiscard]] constexpr i64 floorToI64(T x) noexcept {
    HELIOS_MATH_ASSERT(x > T(-9.2233720368547758e18) && x < T(9.2233720368547758e18));
    const i64 i = static_cast<i64>(x);
    return static_cast<T>(i) > x ? i - 1 : i;
}
/// ceil(x) as i32. Precondition as floorToI32.
template <FloatingPoint T>
[[nodiscard]] constexpr i32 ceilToI32(T x) noexcept {
    HELIOS_MATH_ASSERT(x > T(-2147483648.0) && x < T(2147483648.0));
    const i32 i = static_cast<i32>(x);
    return static_cast<T>(i) < x ? i + 1 : i;
}
/// Round half away from zero, as i32. Precondition as floorToI32.
template <FloatingPoint T>
[[nodiscard]] inline i32 roundToI32(T x) noexcept {
    return static_cast<i32>(std::round(x));
}
/// Non-negative modulo for integers: result in [0, |m|). Precondition: m != 0.
template <std::integral T>
[[nodiscard]] constexpr T modPositive(T a, T m) noexcept {
    const T r = a % m;
    if constexpr (std::is_signed_v<T>) {
        return r < 0 ? r + (m < 0 ? -m : m) : r;
    } else {
        return r;
    }
}
/// Floored modulo for floats: result in [0, m) for m > 0.
template <FloatingPoint T>
[[nodiscard]] inline T modPositive(T a, T m) noexcept {
    T r = a - m * std::floor(a / m);
    // a / m can round up to an integer when a is just below a multiple of m, leaving r slightly
    // negative (e.g. a = nextafter(k * m, 0)); fold that back into [0, m).
    if (r < T(0)) r += m;
    return r >= m ? T(0) : r;
}

// ---------------------------------------------------------------------------------------------
// Integer hashing (deterministic, platform independent). Used by noise, procedural placement and
// seed derivation. Not cryptographic.
// ---------------------------------------------------------------------------------------------

/// 32-bit avalanche mixer ("lowbias32", C. Wellons' hash-prospector, public domain). A bijection
/// on u32, so keying it with a seed yields a seeded permutation of the 32-bit integers.
[[nodiscard]] constexpr u32 hashU32(u32 x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
/// 64-bit avalanche mixer (SplitMix64 finalizer, public domain). A bijection on u64.
[[nodiscard]] constexpr u64 hashU64(u64 x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
/// Order-dependent combination of a running 32-bit hash with a new value.
/// (Deliberately not called hashCombine: helios/core declares hashCombine(u64, u64) in the same
/// namespace, and a u32 overload would make mixed-width calls ambiguous or silently change which
/// hash a u32 call site gets depending on the headers included.)
[[nodiscard]] constexpr u32 hashCombineU32(u32 seed, u32 value) noexcept {
    return hashU32(seed ^ (value + 0x9e3779b9U + (seed << 6) + (seed >> 2)));
}
/// Derives an independent 32-bit seed from a 64-bit seed and a salt (e.g. a layer index).
[[nodiscard]] constexpr u32 deriveSeed(u64 seed, u32 salt) noexcept {
    return static_cast<u32>(hashU64(seed ^ (static_cast<u64>(salt) * 0x9e3779b97f4a7c15ULL)) >> 32);
}
/// Maps a hash to a float in [0, 1) using its top 24 bits (exactly representable).
[[nodiscard]] constexpr f32 hashToUnitF32(u32 h) noexcept {
    return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
}
/// Maps a hash to a double in [0, 1) using its top 53 bits (exactly representable).
[[nodiscard]] constexpr f64 hashToUnitF64(u64 h) noexcept {
    return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------------------------------------
// Deterministic transcendental functions. std::sin/cos/atan2 differ in the last bit between the
// MSVC CRT and glibc, which breaks client/server agreement in procedural generation. These use
// only +, -, *, /, sqrt and floor (all exactly specified by IEEE 754) and are compiled out of
// line with FP contraction disabled, so they return bit-identical results everywhere.
// Accuracy (measured against glibc): sin/cos <= 2 ulp for |x| < 2^30 (~1.07e9), including next to
// multiples of pi/2; larger arguments are folded with fmod and are deterministic but not
// accurate. atan/atan2 <= 2 ulp, asin/acos <= 3 ulp. Roughly as fast as the CRT versions.
// ---------------------------------------------------------------------------------------------
namespace det {
[[nodiscard]] f64 sin(f64 x) noexcept;
[[nodiscard]] f64 cos(f64 x) noexcept;
void sinCos(f64 x, f64& outSin, f64& outCos) noexcept;
[[nodiscard]] f64 atan(f64 x) noexcept;
[[nodiscard]] f64 atan2(f64 y, f64 x) noexcept;
[[nodiscard]] f64 asin(f64 x) noexcept;
[[nodiscard]] f64 acos(f64 x) noexcept;

// Exponentials and logarithms (06's `hmath` built-ins for HXL; src/det_exp.cpp). Double-double
// intermediates make them nearly correctly rounded (measured against 64-bit-mantissa references:
// exp, ln, asinh < 0.51 ulp, pow < 0.52 ulp; subnormal results of exp/pow <= 1 ulp). Special values
// follow C99 Annex F like std::exp/log/pow/asinh (pow(x, 0) = 1 even for NaN, pow(-8, 1/3) = NaN,
// ln(-1) = NaN, ln(0) = -inf, ...). Cost (2.8 GHz x86-64): exp ~35 ns, ln ~50 ns, asinh ~85 ns,
// pow ~120 ns; budget <= 150 ns per call (HXL formulas, not per-pixel work).
/// e^x. Overflows to +inf above 709.782712893384, underflows to 0 below -745.1332191019411.
[[nodiscard]] f64 exp(f64 x) noexcept;
/// Natural logarithm.
[[nodiscard]] f64 ln(f64 x) noexcept;
/// x^y. Exact for y = 1, 2, -1 and 0.5 (x, x*x, 1/x, sqrt(x)); the general case forms y * ln|x|
/// in double-double, so accuracy does not degrade for large exponents.
[[nodiscard]] f64 pow(f64 x, f64 y) noexcept;
/// Inverse hyperbolic sine (odd; accurate near 0 and for huge |x|).
[[nodiscard]] f64 asinh(f64 x) noexcept;

// f32 overloads evaluate in f64 and round once (still deterministic).
[[nodiscard]] inline f32 sin(f32 x) noexcept { return static_cast<f32>(sin(static_cast<f64>(x))); }
[[nodiscard]] inline f32 cos(f32 x) noexcept { return static_cast<f32>(cos(static_cast<f64>(x))); }
[[nodiscard]] inline f32 atan(f32 x) noexcept { return static_cast<f32>(atan(static_cast<f64>(x))); }
[[nodiscard]] inline f32 atan2(f32 y, f32 x) noexcept {
    return static_cast<f32>(atan2(static_cast<f64>(y), static_cast<f64>(x)));
}
[[nodiscard]] inline f32 asin(f32 x) noexcept { return static_cast<f32>(asin(static_cast<f64>(x))); }
[[nodiscard]] inline f32 acos(f32 x) noexcept { return static_cast<f32>(acos(static_cast<f64>(x))); }
[[nodiscard]] inline f32 exp(f32 x) noexcept { return static_cast<f32>(exp(static_cast<f64>(x))); }
[[nodiscard]] inline f32 ln(f32 x) noexcept { return static_cast<f32>(ln(static_cast<f64>(x))); }
[[nodiscard]] inline f32 pow(f32 x, f32 y) noexcept {
    return static_cast<f32>(pow(static_cast<f64>(x), static_cast<f64>(y)));
}
[[nodiscard]] inline f32 asinh(f32 x) noexcept { return static_cast<f32>(asinh(static_cast<f64>(x))); }
}  // namespace det

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
