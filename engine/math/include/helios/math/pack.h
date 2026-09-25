// helios/math/pack.h — quantization and bit packing for networking, storage and GPU data.
//
// Everything here is deterministic: the out-of-line functions are compiled with FP contraction
// disabled and use only exactly-specified IEEE operations, so a value encoded on a Linux server
// decodes to bit-identical floats on a Windows client (R07: "quantize on both sides").
//
//   Half floats    IEEE 754 binary16, round-to-nearest-even, denormals, +-inf, NaN preserved.
//   UNORM/SNORM    Vulkan/D3D conventions (SNORM: -1 has two codes, both decode to -1).
//   Octahedral     unit vectors in 2 x N bits (Cigolle et al. 2014), "precise" encoder.
//   Smallest-3     quaternions in 2 + 3 x N bits (index of the dropped largest component + 3
//                  components in [-1/sqrt2, 1/sqrt2]); zero is exactly representable.
//   Positions      fixed-point offsets from a cell origin at a chosen resolution.
//
// Threading: all functions are pure and thread-safe.
#pragma once

#include "helios/math/quat.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

// ---------------------------------------------------------------------------------------------
// Half precision (binary16).
// ---------------------------------------------------------------------------------------------
/// f32 -> binary16 bits, round-to-nearest-even. Overflow (|f| >= 65520) -> inf; values that
/// round below the smallest denormal (2^-25 or less) -> signed zero; NaN stays NaN (quiet, top
/// payload bits kept).
[[nodiscard]] u16 floatToHalf(f32 f) noexcept;
/// binary16 bits -> f32 (exact; every half is representable).
[[nodiscard]] f32 halfToFloat(u16 h) noexcept;
/// Two halves in one u32 (x in the low 16 bits), as GLSL packHalf2x16.
[[nodiscard]] inline u32 packHalf2x16(const Vec2& v) noexcept {
    return static_cast<u32>(floatToHalf(v.x)) | (static_cast<u32>(floatToHalf(v.y)) << 16);
}
[[nodiscard]] inline Vec2 unpackHalf2x16(u32 p) noexcept {
    return {halfToFloat(static_cast<u16>(p & 0xFFFFu)), halfToFloat(static_cast<u16>(p >> 16))};
}

// ---------------------------------------------------------------------------------------------
// UNORM / SNORM (round to nearest, ties away from zero; NaN -> 0).
// ---------------------------------------------------------------------------------------------
/// Quantizes x in [0, 1] (clamped) to `bits` (1..32) bits.
[[nodiscard]] inline u32 quantizeUnorm(f32 x, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 1 && bits <= 32);
    const f64 maxv = static_cast<f64>((bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1ull)));
    const f64 c = x > 0.0f ? (x < 1.0f ? static_cast<f64>(x) : 1.0) : 0.0;
    return static_cast<u32>(c * maxv + 0.5);
}
[[nodiscard]] inline f32 dequantizeUnorm(u32 q, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 1 && bits <= 32);
    const f64 maxv = static_cast<f64>((bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1ull)));
    return static_cast<f32>(static_cast<f64>(q) / maxv);
}
/// Quantizes x in [-1, 1] (clamped) to a two's-complement `bits`-bit SNORM value (2..32 bits),
/// returned sign-extended. Codes span [-(2^(bits-1)-1), 2^(bits-1)-1].
[[nodiscard]] inline i32 quantizeSnorm(f32 x, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 2 && bits <= 32);
    const f64 maxv = static_cast<f64>((1ull << (bits - 1)) - 1ull);
    const f64 c = x > -1.0f ? (x < 1.0f ? static_cast<f64>(x) : 1.0) : (x <= -1.0f ? -1.0 : 0.0);
    const f64 s = c * maxv;
    return static_cast<i32>(s >= 0.0 ? s + 0.5 : s - 0.5);
}
[[nodiscard]] inline f32 dequantizeSnorm(i32 q, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 2 && bits <= 32);
    const f64 maxv = static_cast<f64>((1ull << (bits - 1)) - 1ull);
    return static_cast<f32>(max(-1.0, static_cast<f64>(q) / maxv));
}
/// Maps x in [lo, hi] (clamped; NaN -> lo) to `bits` (1..32) bits; the endpoints are exactly
/// representable. Error <= half a step for every bit count. Precondition: lo < hi.
[[nodiscard]] inline u32 quantizeRange(f32 x, f32 lo, f32 hi, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 1 && bits <= 32 && lo < hi);
    // The normalized parameter stays in f64: rounding it to f32 would add up to 2^-25 of the range,
    // i.e. up to half a code at 24 bits and 128 codes at 32 bits.
    const f64 maxv = static_cast<f64>((bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1ull)));
    const f64 t = (static_cast<f64>(x) - lo) / (static_cast<f64>(hi) - lo);
    const f64 c = t > 0.0 ? (t < 1.0 ? t : 1.0) : 0.0;
    return static_cast<u32>(c * maxv + 0.5);
}
[[nodiscard]] inline f32 dequantizeRange(u32 q, f32 lo, f32 hi, int bits) noexcept {
    HELIOS_MATH_ASSERT(bits >= 1 && bits <= 32);
    const f64 maxv = static_cast<f64>((bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1ull)));
    return static_cast<f32>(static_cast<f64>(lo) +
                            (static_cast<f64>(hi) - lo) * (static_cast<f64>(q) / maxv));
}

[[nodiscard]] inline u8 packUnorm8(f32 x) noexcept { return static_cast<u8>(quantizeUnorm(x, 8)); }
[[nodiscard]] inline f32 unpackUnorm8(u8 v) noexcept { return dequantizeUnorm(v, 8); }
[[nodiscard]] inline u16 packUnorm16(f32 x) noexcept { return static_cast<u16>(quantizeUnorm(x, 16)); }
[[nodiscard]] inline f32 unpackUnorm16(u16 v) noexcept { return dequantizeUnorm(v, 16); }
[[nodiscard]] inline i8 packSnorm8(f32 x) noexcept { return static_cast<i8>(quantizeSnorm(x, 8)); }
[[nodiscard]] inline f32 unpackSnorm8(i8 v) noexcept { return dequantizeSnorm(v, 8); }
[[nodiscard]] inline i16 packSnorm16(f32 x) noexcept { return static_cast<i16>(quantizeSnorm(x, 16)); }
[[nodiscard]] inline f32 unpackSnorm16(i16 v) noexcept { return dequantizeSnorm(v, 16); }

/// GLSL-compatible 4x8 / 2x16 packs (component 0 in the low bits).
[[nodiscard]] inline u32 packUnorm4x8(const Vec4& v) noexcept {
    return quantizeUnorm(v.x, 8) | (quantizeUnorm(v.y, 8) << 8) | (quantizeUnorm(v.z, 8) << 16) |
           (quantizeUnorm(v.w, 8) << 24);
}
[[nodiscard]] inline Vec4 unpackUnorm4x8(u32 p) noexcept {
    return {unpackUnorm8(static_cast<u8>(p)), unpackUnorm8(static_cast<u8>(p >> 8)),
            unpackUnorm8(static_cast<u8>(p >> 16)), unpackUnorm8(static_cast<u8>(p >> 24))};
}
[[nodiscard]] inline u32 packSnorm4x8(const Vec4& v) noexcept {
    return (static_cast<u32>(quantizeSnorm(v.x, 8)) & 0xFFu) |
           ((static_cast<u32>(quantizeSnorm(v.y, 8)) & 0xFFu) << 8) |
           ((static_cast<u32>(quantizeSnorm(v.z, 8)) & 0xFFu) << 16) |
           ((static_cast<u32>(quantizeSnorm(v.w, 8)) & 0xFFu) << 24);
}
[[nodiscard]] inline Vec4 unpackSnorm4x8(u32 p) noexcept {
    return {unpackSnorm8(static_cast<i8>(static_cast<u8>(p))),
            unpackSnorm8(static_cast<i8>(static_cast<u8>(p >> 8))),
            unpackSnorm8(static_cast<i8>(static_cast<u8>(p >> 16))),
            unpackSnorm8(static_cast<i8>(static_cast<u8>(p >> 24)))};
}
[[nodiscard]] inline u32 packUnorm2x16(const Vec2& v) noexcept {
    return quantizeUnorm(v.x, 16) | (quantizeUnorm(v.y, 16) << 16);
}
[[nodiscard]] inline Vec2 unpackUnorm2x16(u32 p) noexcept {
    return {unpackUnorm16(static_cast<u16>(p)), unpackUnorm16(static_cast<u16>(p >> 16))};
}
[[nodiscard]] inline u32 packSnorm2x16(const Vec2& v) noexcept {
    return (static_cast<u32>(quantizeSnorm(v.x, 16)) & 0xFFFFu) |
           ((static_cast<u32>(quantizeSnorm(v.y, 16)) & 0xFFFFu) << 16);
}
[[nodiscard]] inline Vec2 unpackSnorm2x16(u32 p) noexcept {
    return {unpackSnorm16(static_cast<i16>(static_cast<u16>(p))),
            unpackSnorm16(static_cast<i16>(static_cast<u16>(p >> 16)))};
}

// ---------------------------------------------------------------------------------------------
// Octahedral unit-vector encoding.
// ---------------------------------------------------------------------------------------------
/// Projects a unit vector onto the octahedron and unfolds it into [-1, 1]^2 (continuous in the
/// upper hemisphere +Z; the lower hemisphere is folded over the diagonals).
[[nodiscard]] Vec2 octEncode(const Vec3& n) noexcept;
/// Inverse of octEncode (result is unit length).
[[nodiscard]] Vec3 octDecode(const Vec2& e) noexcept;
/// Packs a unit vector into 2 x `bitsPerComponent` bits (2..16 each; x in the low half). Uses the
/// precise encoder: the best of the 4 neighbouring grid points is chosen by decoded angle.
/// Max angular error (measured over 2e5 random directions, tested): 8+8 bits 0.64°, 12+12 bits
/// 0.039°, 16+16 bits 0.0025°.
[[nodiscard]] u32 packOctahedral(const Vec3& n, int bitsPerComponent) noexcept;
[[nodiscard]] Vec3 unpackOctahedral(u32 packed, int bitsPerComponent) noexcept;
[[nodiscard]] inline u16 packOct16(const Vec3& n) noexcept { return static_cast<u16>(packOctahedral(n, 8)); }
[[nodiscard]] inline Vec3 unpackOct16(u16 p) noexcept { return unpackOctahedral(p, 8); }
[[nodiscard]] inline u32 packOct24(const Vec3& n) noexcept { return packOctahedral(n, 12); }
[[nodiscard]] inline Vec3 unpackOct24(u32 p) noexcept { return unpackOctahedral(p & 0xFFFFFFu, 12); }
[[nodiscard]] inline u32 packOct32(const Vec3& n) noexcept { return packOctahedral(n, 16); }
[[nodiscard]] inline Vec3 unpackOct32(u32 p) noexcept { return unpackOctahedral(p, 16); }

// ---------------------------------------------------------------------------------------------
// Smallest-three quaternion encoding.
// Layout (MSB -> LSB): [index:2][a:N][b:N][c:N], total 2 + 3N bits (N = 2..20), where index is the
// dropped largest component (0=x .. 3=w, made positive) and a, b, c are the remaining components
// in x, y, z, w order, each c*sqrt2 in [-1, 1] quantized symmetrically to 2^N - 1 codes.
// Max rotation error (worst case, first order): sqrt(6) / (2^(N-1) - 1) radians, i.e.
// N = 9 (29 bits) 0.55°, N = 10 (32 bits) 0.28°, N = 15 (47 bits) 0.0086°.
// ---------------------------------------------------------------------------------------------
[[nodiscard]] u64 packQuatSmallestThree(const Quat& q, int bitsPerComponent) noexcept;
[[nodiscard]] Quat unpackQuatSmallestThree(u64 packed, int bitsPerComponent) noexcept;
/// Documented worst-case rotation error (radians) of smallest-three with N bits per component.
[[nodiscard]] inline f64 smallestThreeMaxError(int bitsPerComponent) noexcept {
    // sqrt(6)/H to first order; the 2% margin covers second-order terms and f32 rounding.
    return 2.449489742783178 * 1.02 / static_cast<f64>((1ull << (bitsPerComponent - 1)) - 1ull);
}
/// 32-bit variant (2 + 3 x 10 bits).
[[nodiscard]] inline u32 packQuat32(const Quat& q) noexcept {
    return static_cast<u32>(packQuatSmallestThree(q, 10));
}
[[nodiscard]] inline Quat unpackQuat32(u32 p) noexcept { return unpackQuatSmallestThree(p, 10); }

// ---------------------------------------------------------------------------------------------
// Fixed-point positions relative to a cell origin.
// ---------------------------------------------------------------------------------------------
/// Quantizes frame-local f64 positions to `bits`-bit integers per axis at resolution `step`
/// metres, relative to `origin`: code = round((p - origin) / step) + 2^(bits-1) (offset binary).
/// Representable offsets per axis: [-2^(bits-1), 2^(bits-1) - 1] * step, e.g. 1 mm with 26 bits
/// covers +-33.5 km; 1 mm with 32 bits covers +-2147 km. Error <= step / 2 inside the range.
struct PositionQuantizer {
    DVec3 origin{};
    f64 step = 0.001;
    int bits = 24;

    /// Quantizer covering +-halfExtent metres around `origin` at `step` resolution with the
    /// fewest bits (<= 32). Returns an invalid quantizer (bits = 0, see isValid()) if 32 bits are
    /// not enough.
    [[nodiscard]] static PositionQuantizer fromExtent(const DVec3& cellOrigin, f64 halfExtent,
                                                      f64 resolution) noexcept;

    /// True if bits is in [2, 32] and step is positive and finite. encode()/decode() require it.
    [[nodiscard]] bool isValid() const noexcept {
        return bits >= 2 && bits <= 32 && step > 0.0 && step <= std::numeric_limits<f64>::max();
    }
    /// Largest representable offset from the origin per axis (metres), on the negative side.
    /// 0 for an invalid quantizer.
    [[nodiscard]] f64 halfRange() const noexcept {
        return isValid() ? static_cast<f64>(1ull << (bits - 1)) * step : 0.0;
    }
    /// True if every axis of p encodes without clamping (always false for an invalid quantizer).
    [[nodiscard]] bool inRange(const DVec3& p) const noexcept;
    /// Encodes (clamping out-of-range axes to the nearest code). Precondition: isValid().
    [[nodiscard]] UVec3 encode(const DVec3& p) const noexcept;
    /// Precondition: isValid().
    [[nodiscard]] DVec3 decode(const UVec3& q) const noexcept;
    /// Worst-case reconstruction error per axis for in-range positions.
    [[nodiscard]] f64 maxError() const noexcept { return step * 0.5; }
};

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
