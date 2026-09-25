// Quantization and packing (see helios/math/pack.h). Determinism-critical: contraction disabled.
#include "fp_control.h"

#include "helios/math/pack.h"

#include <bit>
#include <cmath>

namespace helios {

// ---------------------------------------------------------------------------------------------
// Half precision
// ---------------------------------------------------------------------------------------------
u16 floatToHalf(f32 f) noexcept {
    const u32 x = std::bit_cast<u32>(f);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 ax = x & 0x7FFFFFFFu;

    if (ax >= 0x7F800000u) {
        if (ax > 0x7F800000u) {
            // NaN: keep the top 10 payload bits and force the quiet bit so it cannot become inf.
            return static_cast<u16>(sign | 0x7C00u | 0x0200u | ((ax >> 13) & 0x03FFu));
        }
        return static_cast<u16>(sign | 0x7C00u);  // +-inf
    }
    if (ax >= 0x477FF000u) return static_cast<u16>(sign | 0x7C00u);  // >= 65520 rounds to inf
    if (ax >= 0x38800000u) {
        // Normal half: rebias the exponent (127 -> 15) and round the mantissa to 10 bits, ties to
        // even. A mantissa carry correctly bumps the exponent (up to 65504 -> 0x7BFF).
        const u32 lsb = (ax >> 13) & 1u;
        return static_cast<u16>(sign | ((ax - 0x38000000u + 0x0FFFu + lsb) >> 13));
    }
    if (ax <= 0x33000000u) return static_cast<u16>(sign);  // <= 2^-25: ties-to-even gives zero
    // Subnormal half: value / 2^-24 = m * 2^(e - 126), rounded to nearest even.
    const u32 e = ax >> 23;  // 102..112
    const u32 m = (ax & 0x007FFFFFu) | 0x00800000u;
    const u32 shift = 126u - e;  // 14..24
    u32 hm = m >> shift;
    const u32 rem = m & ((1u << shift) - 1u);
    const u32 halfway = 1u << (shift - 1u);
    if (rem > halfway || (rem == halfway && (hm & 1u))) ++hm;  // may carry into the smallest normal
    return static_cast<u16>(sign | hm);
}

f32 halfToFloat(u16 h) noexcept {
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    const u32 e = (h >> 10) & 0x1Fu;
    const u32 m = h & 0x03FFu;
    if (e == 0) {
        if (m == 0) return std::bit_cast<f32>(sign);
        const f32 v = static_cast<f32>(m) * 5.9604644775390625e-8f;  // m * 2^-24, exact
        return sign ? -v : v;
    }
    if (e == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (m << 13));  // inf / NaN
    return std::bit_cast<f32>(sign | ((e + 112u) << 23) | (m << 13));
}

// ---------------------------------------------------------------------------------------------
// Octahedral
// ---------------------------------------------------------------------------------------------
namespace {
f32 signNotZero(f32 v) noexcept { return v >= 0.0f ? 1.0f : -1.0f; }
}  // namespace

Vec2 octEncode(const Vec3& n) noexcept {
    const f32 l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (!(l1 > 0.0f)) return {0.0f, 0.0f};
    Vec2 p{n.x / l1, n.y / l1};
    if (n.z < 0.0f) {
        p = {(1.0f - std::fabs(p.y)) * signNotZero(p.x), (1.0f - std::fabs(p.x)) * signNotZero(p.y)};
    }
    return p;
}

Vec3 octDecode(const Vec2& e) noexcept {
    Vec3 v{e.x, e.y, 1.0f - std::fabs(e.x) - std::fabs(e.y)};
    if (v.z < 0.0f) {
        const f32 ox = v.x;
        v.x = (1.0f - std::fabs(v.y)) * signNotZero(ox);
        v.y = (1.0f - std::fabs(ox)) * signNotZero(v.y);
    }
    return normalize(v);
}

u32 packOctahedral(const Vec3& n, int bitsPerComponent) noexcept {
    HELIOS_MATH_ASSERT(bitsPerComponent >= 2 && bitsPerComponent <= 16);
    const i32 maxCode = (1 << (bitsPerComponent - 1)) - 1;  // symmetric SNORM codes
    const f32 scale = static_cast<f32>(maxCode);
    const Vec2 e = octEncode(n);
    const f32 fx = std::floor(clamp(e.x, -1.0f, 1.0f) * scale);
    const f32 fy = std::floor(clamp(e.y, -1.0f, 1.0f) * scale);
    // Precise encoding: try the 4 surrounding grid points and keep the one whose (f32) decode is
    // closest to n. The distance is evaluated in f64: at 16 bits the angular errors (~1e-5 rad)
    // are below f32's resolution of a dot product near 1.
    const DVec3 nd = toF64(n);
    i32 bestX = 0, bestY = 0;
    f64 bestDist = kInfinityD;
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const i32 qx = min(static_cast<i32>(fx) + dx, maxCode);
            const i32 qy = min(static_cast<i32>(fy) + dy, maxCode);
            const Vec3 d = octDecode({static_cast<f32>(qx) / scale, static_cast<f32>(qy) / scale});
            const f64 dist = distanceSq(toF64(d), nd);
            if (dist < bestDist) {
                bestDist = dist;
                bestX = qx;
                bestY = qy;
            }
        }
    }
    const u32 ux = static_cast<u32>(bestX + maxCode);
    const u32 uy = static_cast<u32>(bestY + maxCode);
    return ux | (uy << bitsPerComponent);
}

Vec3 unpackOctahedral(u32 packed, int bitsPerComponent) noexcept {
    HELIOS_MATH_ASSERT(bitsPerComponent >= 2 && bitsPerComponent <= 16);
    const i32 maxCode = (1 << (bitsPerComponent - 1)) - 1;
    const u32 mask = (1u << bitsPerComponent) - 1u;
    const f32 scale = static_cast<f32>(maxCode);
    const i32 qx = min(static_cast<i32>(packed & mask) - maxCode, maxCode);
    const i32 qy = min(static_cast<i32>((packed >> bitsPerComponent) & mask) - maxCode, maxCode);
    return octDecode({static_cast<f32>(qx) / scale, static_cast<f32>(qy) / scale});
}

// ---------------------------------------------------------------------------------------------
// Smallest three
// ---------------------------------------------------------------------------------------------
u64 packQuatSmallestThree(const Quat& qIn, int bitsPerComponent) noexcept {
    HELIOS_MATH_ASSERT(bitsPerComponent >= 2 && bitsPerComponent <= 20);
    const Quat q = normalize(qIn);
    const f32 c[4] = {q.x, q.y, q.z, q.w};
    int largest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
    }
    const f32 flip = c[largest] < 0.0f ? -1.0f : 1.0f;
    const i64 maxCode = (i64{1} << (bitsPerComponent - 1)) - 1;
    const f64 scale = static_cast<f64>(maxCode) * 1.4142135623730951;  // c * sqrt2 in [-1, 1]
    u64 packed = static_cast<u64>(largest);
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const f64 s = static_cast<f64>(c[i] * flip) * scale;
        i64 code = static_cast<i64>(s >= 0.0 ? s + 0.5 : s - 0.5);
        code = code < -maxCode ? -maxCode : (code > maxCode ? maxCode : code);
        packed = (packed << bitsPerComponent) | static_cast<u64>(code + maxCode);
    }
    return packed;
}

Quat unpackQuatSmallestThree(u64 packed, int bitsPerComponent) noexcept {
    HELIOS_MATH_ASSERT(bitsPerComponent >= 2 && bitsPerComponent <= 20);
    const i64 maxCode = (i64{1} << (bitsPerComponent - 1)) - 1;
    const u64 mask = (u64{1} << bitsPerComponent) - 1u;
    const f64 inv = 0.70710678118654752 / static_cast<f64>(maxCode);
    const int largest = static_cast<int>((packed >> (3 * bitsPerComponent)) & 3u);
    f64 c[4] = {0.0, 0.0, 0.0, 0.0};
    f64 sumSq = 0.0;
    int shift = 2 * bitsPerComponent;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        i64 code = static_cast<i64>((packed >> shift) & mask) - maxCode;
        code = code > maxCode ? maxCode : code;
        c[i] = static_cast<f64>(code) * inv;
        sumSq += c[i] * c[i];
        shift -= bitsPerComponent;
    }
    c[largest] = std::sqrt(sumSq < 1.0 ? 1.0 - sumSq : 0.0);
    return normalize(
        Quat{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2]), static_cast<f32>(c[3])});
}

// ---------------------------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------------------------
PositionQuantizer PositionQuantizer::fromExtent(const DVec3& cellOrigin, f64 halfExtent,
                                                f64 resolution) noexcept {
    HELIOS_MATH_ASSERT(resolution > 0.0 && halfExtent >= 0.0);
    const f64 codesNeeded = std::ceil(halfExtent / resolution);  // largest |code| that must be representable
    for (int b = 2; b <= 32; ++b) {
        // Codes span [-2^(b-1), 2^(b-1) - 1]; the positive side is the binding one.
        if (static_cast<f64>((1ull << (b - 1)) - 1ull) >= codesNeeded) return {cellOrigin, resolution, b};
    }
    return {cellOrigin, resolution, 0};
}

namespace {
struct AxisCode {
    i64 code;
    bool clamped;
};
AxisCode encodeAxis(f64 offset, f64 step, int bits) noexcept {
    const f64 lo = -static_cast<f64>(1ull << (bits - 1));
    const f64 hi = static_cast<f64>((1ull << (bits - 1)) - 1ull);
    const f64 scaled = std::floor(offset / step + 0.5);
    if (!(scaled >= lo)) return {static_cast<i64>(lo), true};  // also catches NaN
    if (scaled > hi) return {static_cast<i64>(hi), true};
    return {static_cast<i64>(scaled), false};
}
}  // namespace

bool PositionQuantizer::inRange(const DVec3& p) const noexcept {
    if (!isValid()) return false;
    const DVec3 o = p - origin;
    return !encodeAxis(o.x, step, bits).clamped && !encodeAxis(o.y, step, bits).clamped &&
           !encodeAxis(o.z, step, bits).clamped;
}

UVec3 PositionQuantizer::encode(const DVec3& p) const noexcept {
    HELIOS_MATH_ASSERT(isValid());
    if (!isValid()) return {};  // release builds: no out-of-range shifts
    const DVec3 o = p - origin;
    const i64 bias = static_cast<i64>(1ull << (bits - 1));
    return {static_cast<u32>(encodeAxis(o.x, step, bits).code + bias),
            static_cast<u32>(encodeAxis(o.y, step, bits).code + bias),
            static_cast<u32>(encodeAxis(o.z, step, bits).code + bias)};
}

DVec3 PositionQuantizer::decode(const UVec3& q) const noexcept {
    HELIOS_MATH_ASSERT(isValid());
    if (!isValid()) return origin;
    const i64 bias = static_cast<i64>(1ull << (bits - 1));
    return {origin.x + static_cast<f64>(static_cast<i64>(q.x) - bias) * step,
            origin.y + static_cast<f64>(static_cast<i64>(q.y) - bias) * step,
            origin.z + static_cast<f64>(static_cast<i64>(q.z) - bias) * step};
}

}  // namespace helios
