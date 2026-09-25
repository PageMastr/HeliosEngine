#include "test_util.h"

#include <algorithm>
#include <initializer_list>
#include <ios>
#include <bit>
#include <vector>

using namespace helios;
using helios::test::Rng;

namespace {
// Independent reference for f32 -> f16: exact half values from the bit layout, nearest with
// ties-to-even, computed in f64 (all differences involved are exactly representable).
struct HalfReference {
    std::vector<f64> values;  // value of positive finite half h (index = bits, 0..0x7BFF)
    HalfReference() {
        values.resize(0x7C00);
        for (u32 h = 0; h < 0x7C00; ++h) {
            const u32 e = h >> 10, m = h & 0x3FF;
            values[h] = e == 0 ? std::ldexp(static_cast<f64>(m), -24)
                               : std::ldexp(static_cast<f64>(1024 + m), static_cast<int>(e) - 25);
        }
    }
    u16 convert(f32 f) const {
        const u16 sign = std::signbit(f) ? 0x8000 : 0;
        if (std::isnan(f)) return 0x7E00;
        const f64 a = std::fabs(static_cast<f64>(f));
        if (a >= 65520.0) return static_cast<u16>(sign | 0x7C00);
        // Largest half <= a.
        const auto it = std::upper_bound(values.begin(), values.end(), a);
        const u32 lo = static_cast<u32>((it - values.begin()) - 1);
        if (lo == 0x7BFF) return static_cast<u16>(sign | lo);  // (65504, 65520) rounds down
        const f64 dl = a - values[lo], dh = values[lo + 1] - a;
        u32 r = lo;
        if (dh < dl || (dh == dl && (lo & 1u))) r = lo + 1;
        return static_cast<u16>(sign | r);
    }
};
const HalfReference& halfRef() {
    static const HalfReference ref;
    return ref;
}
bool isHalfNan(u16 h) { return (h & 0x7C00) == 0x7C00 && (h & 0x03FF) != 0; }
}  // namespace

TEST_CASE("pack: half float known values") {
    CHECK(floatToHalf(0.0f) == 0x0000);
    CHECK(floatToHalf(-0.0f) == 0x8000);
    CHECK(floatToHalf(1.0f) == 0x3C00);
    CHECK(floatToHalf(-2.0f) == 0xC000);
    CHECK(floatToHalf(0.1f) == 0x2E66);
    CHECK(floatToHalf(65504.0f) == 0x7BFF);
    CHECK(floatToHalf(65519.99f) == 0x7BFF);
    CHECK(floatToHalf(65520.0f) == 0x7C00);  // tie with an odd mantissa rounds up to inf
    CHECK(floatToHalf(1e10f) == 0x7C00);
    CHECK(floatToHalf(-kInfinity) == 0xFC00);
    CHECK(floatToHalf(std::ldexp(1.0f, -14)) == 0x0400);  // smallest normal
    CHECK(floatToHalf(std::ldexp(1.0f, -24)) == 0x0001);  // smallest denormal
    CHECK(floatToHalf(std::ldexp(1.0f, -25)) == 0x0000);  // exact tie -> even (zero)
    CHECK(floatToHalf(std::ldexp(1.0000001f, -25)) == 0x0001);
    CHECK(floatToHalf(std::ldexp(3.0f, -25)) == 0x0002);  // 1.5 ulp tie -> even (2)
    CHECK(floatToHalf(std::ldexp(1023.5f, -24)) ==
          0x0400);                                         // denormal rounding carries into the normal range
    CHECK(floatToHalf(-std::ldexp(1.0f, -30)) == 0x8000);  // underflow keeps the sign
    CHECK(floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);         // tie, even stays
    CHECK(floatToHalf(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02);  // tie, odd rounds up
    CHECK(isHalfNan(floatToHalf(std::nanf(""))));
    CHECK(isHalfNan(floatToHalf(std::bit_cast<f32>(0x7F800001u))));  // payload in low bits stays NaN
    CHECK((floatToHalf(-std::nanf("")) & 0x8000) != 0);
    CHECK(halfToFloat(0x3C00) == 1.0f);
    CHECK(halfToFloat(0x7BFF) == 65504.0f);
    CHECK(halfToFloat(0x0001) == std::ldexp(1.0f, -24));
    CHECK(halfToFloat(0x7C00) == kInfinity);
    CHECK(std::isnan(halfToFloat(0x7E00)));
    CHECK(std::signbit(halfToFloat(0x8000)));
}

TEST_CASE("pack: half float is exact for every half value (exhaustive)") {
    int nans = 0;
    for (u32 h = 0; h <= 0xFFFF; ++h) {
        const u16 hb = static_cast<u16>(h);
        const f32 f = halfToFloat(hb);
        if (isHalfNan(hb)) {
            CHECK(std::isnan(f));
            CHECK(isHalfNan(floatToHalf(f)));
            ++nans;
            continue;
        }
        if (floatToHalf(f) != hb) {
            FAIL_CHECK("round trip failed for half 0x" << std::hex << h);
        }
    }
    CHECK(nans == 2 * 1023);
}

TEST_CASE("pack: half float rounding matches an independent reference") {
    const HalfReference& ref = halfRef();
    int checked = 0;
    auto check = [&](f32 f) {
        const u16 got = floatToHalf(f);
        const u16 want = ref.convert(f);
        if (got != want) FAIL_CHECK("f=" << f << " got 0x" << std::hex << got << " want 0x" << want);
        ++checked;
    };
    // Every midpoint between adjacent halves, and one float ulp either side of it.
    for (u32 h = 0; h < 0x7BFF; ++h) {
        const f32 mid = static_cast<f32>(0.5 * (ref.values[h] + ref.values[h + 1]));
        check(mid);
        check(std::nextafter(mid, 0.0f));
        check(std::nextafter(mid, kInfinity));
        check(-mid);
    }
    // A stride through all float bit patterns plus random patterns.
    for (u64 b = 0; b <= 0xFFFFFFFFull; b += 4099) {
        const f32 f = std::bit_cast<f32>(static_cast<u32>(b));
        if (!std::isnan(f)) check(f);
    }
    Rng rng(80);
    for (int i = 0; i < 300000; ++i) {
        const f32 f = std::bit_cast<f32>(rng.nextU32());
        if (!std::isnan(f)) check(f);
    }
    MESSAGE("half conversions checked: " << checked);
    CHECK(packHalf2x16(Vec2(1.0f, -2.0f)) == (0x3C00u | (0xC000u << 16)));
    CHECK(unpackHalf2x16(0x3C00u | (0xC000u << 16)) == Vec2(1.0f, -2.0f));
}

TEST_CASE("pack: UNORM / SNORM") {
    CHECK(packUnorm8(0.0f) == 0);
    CHECK(packUnorm8(1.0f) == 255);
    CHECK(packUnorm8(0.5f) == 128);
    CHECK(packUnorm8(-3.0f) == 0);
    CHECK(packUnorm8(7.0f) == 255);
    CHECK(packUnorm8(std::nanf("")) == 0);
    CHECK(packUnorm16(1.0f) == 65535);
    CHECK(packSnorm8(1.0f) == 127);
    CHECK(packSnorm8(-1.0f) == -127);
    CHECK(packSnorm8(-5.0f) == -127);
    CHECK(packSnorm8(std::nanf("")) == 0);
    CHECK(packSnorm8(0.0f) == 0);
    CHECK(unpackSnorm8(-128) == -1.0f);  // the extra code clamps
    CHECK(packSnorm16(-0.5f) == -16384);
    for (int i = 0; i < 256; ++i) {
        CHECK(packUnorm8(unpackUnorm8(static_cast<u8>(i))) == i);
        CHECK(unpackUnorm8(static_cast<u8>(i)) == static_cast<f32>(static_cast<f64>(i) / 255.0));
        if (i >= 1) CHECK(packSnorm8(unpackSnorm8(static_cast<i8>(i - 128))) == max(i - 128, -127));
    }
    for (int i = 0; i < 65536; i += 7) CHECK(packUnorm16(unpackUnorm16(static_cast<u16>(i))) == i);
    CHECK(packUnorm4x8(Vec4(1.0f, 0.0f, 0.0f, 0.0f)) == 0xFFu);  // x in the low byte (GLSL)
    CHECK(packUnorm4x8(Vec4(0.0f, 0.0f, 0.0f, 1.0f)) == 0xFF000000u);
    CHECK(packSnorm4x8(Vec4(-1.0f, 1.0f, 0.0f, 0.0f)) == (0x81u | (0x7Fu << 8)));
    CHECK(unpackSnorm4x8(packSnorm4x8(Vec4(-1.0f, 1.0f, 0.0f, 0.5f))) ==
          Vec4(-1.0f, 1.0f, 0.0f, unpackSnorm8(64)));
    CHECK(unpackUnorm4x8(0xFF00FF00u) == Vec4(0.0f, 1.0f, 0.0f, 1.0f));
    CHECK(packUnorm2x16(Vec2(1.0f, 0.0f)) == 0xFFFFu);
    CHECK(unpackUnorm2x16(0xFFFF0000u) == Vec2(0.0f, 1.0f));
    CHECK(packSnorm2x16(Vec2(-1.0f, 1.0f)) == (0x8001u | (0x7FFFu << 16)));
    CHECK(unpackSnorm2x16(packSnorm2x16(Vec2(-1.0f, 1.0f))) == Vec2(-1.0f, 1.0f));
    // Generic ranges: endpoints exact, error <= half a step.
    CHECK(dequantizeRange(quantizeRange(-50.0f, -50.0f, 150.0f, 12), -50.0f, 150.0f, 12) == -50.0f);
    CHECK(dequantizeRange(quantizeRange(150.0f, -50.0f, 150.0f, 12), -50.0f, 150.0f, 12) == 150.0f);
    CHECK(quantizeUnorm(1.0f, 32) == 0xFFFFFFFFu);
    CHECK(dequantizeUnorm(0xFFFFFFFFu, 32) == 1.0f);
    Rng rng(81);
    for (int i = 0; i < 10000; ++i) {
        const f32 v = rng.rangef(-50.0f, 150.0f);
        const f32 d = dequantizeRange(quantizeRange(v, -50.0f, 150.0f, 12), -50.0f, 150.0f, 12);
        CHECK(std::fabs(d - v) <= 0.5f * 200.0f / 4095.0f + 1e-5f);
        const f32 s = rng.rangef(-1.0f, 1.0f);
        CHECK(std::fabs(dequantizeSnorm(quantizeSnorm(s, 10), 10) - s) <= 0.5f / 511.0f + 1e-7f);
    }
    // Regression: quantizeRange used to round the normalized parameter to f32 before quantizing,
    // adding up to 2^-25 of the range: half a code at 24 bits, 128 codes at 32 bits. The code must
    // be the nearest one for every bit count (checked in f64 against the exact parameter).
    for (int bits : {12, 20, 24, 28, 32}) {
        const f64 maxv = static_cast<f64>((bits == 32 ? 0xFFFFFFFFull : ((1ull << bits) - 1ull)));
        f64 worstCodes = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const f32 lo = -1000.0f, hi = 3000.0f;
            const f32 v = rng.rangef(lo, hi);
            const u32 q = quantizeRange(v, lo, hi, bits);
            const f64 exact = (static_cast<f64>(v) - lo) / (static_cast<f64>(hi) - lo) * maxv;
            worstCodes = max(worstCodes, std::fabs(static_cast<f64>(q) - exact));
        }
        CAPTURE(bits);
        CHECK(worstCodes <= 0.5 + 1e-6);
    }
    CHECK(quantizeRange(std::nanf(""), 0.0f, 1.0f, 16) == 0u);
    CHECK(quantizeRange(5.0f, 0.0f, 1.0f, 32) == 0xFFFFFFFFu);
}

TEST_CASE("pack: octahedral normals") {
    // Axes are exact.
    const Vec3 axes[] = {{0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},
                         {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f}};
    for (const Vec3& a : axes) {
        CHECK(unpackOct16(packOct16(a)) == a);
        CHECK(unpackOct24(packOct24(a)) == a);
        CHECK(unpackOct32(packOct32(a)) == a);
        CHECK(approxEqual(octDecode(octEncode(a)), a, 1e-7f));
    }
    CHECK(octEncode(Vec3{}) == Vec2(0.0f, 0.0f));
    struct Case {
        int bits;
        f32 maxErrorDeg;
    };
    const Case cases[] = {{8, 0.65f}, {12, 0.040f}, {16, 0.0025f}};  // documented in pack.h
    Rng rng(82);
    for (const Case& c : cases) {
        f64 worst = 0.0, worstNaive = 0.0;
        for (int i = 0; i < 200000; ++i) {
            const Vec3 n = rng.unit();
            const Vec3 d = unpackOctahedral(packOctahedral(n, c.bits), c.bits);
            CHECK(approxEqual(length(d), 1.0f, 1e-6f));
            worst = max(worst, static_cast<f64>(angleBetween(n, d)));
            // Naive rounding for comparison.
            const f32 scale = static_cast<f32>((1 << (c.bits - 1)) - 1);
            const Vec2 e = octEncode(n);
            const Vec3 nd = octDecode(round(e * scale) / scale);
            worstNaive = max(worstNaive, static_cast<f64>(angleBetween(n, nd)));
        }
        MESSAGE("oct " << c.bits << "+" << c.bits << " bits: max error " << degrees(worst) << " deg (naive "
                       << degrees(worstNaive) << ")");
        CHECK(degrees(worst) <= c.maxErrorDeg);
        CHECK(worst <= worstNaive);
    }
    // Continuity of octEncode across the fold (lower hemisphere).
    CHECK(approxEqual(octDecode(octEncode(normalize(Vec3(0.3f, -0.4f, -0.8f)))),
                      normalize(Vec3(0.3f, -0.4f, -0.8f)), 1e-6f));
    CHECK(packOct24(Vec3(0.0f, 0.0f, 1.0f)) <= 0xFFFFFFu);
}

TEST_CASE("pack: smallest-three quaternions") {
    CHECK(unpackQuat32(packQuat32(Quat{})) == Quat{});  // identity is exact
    // Layout: the dropped component index sits in the top two bits.
    CHECK((packQuatSmallestThree(Quat{}, 10) >> 30) == 3u);
    CHECK((packQuatSmallestThree(Quat(1.0f, 0.0f, 0.0f, 0.0f), 10) >> 30) == 0u);
    CHECK((packQuatSmallestThree(Quat(0.0f, 0.0f, 1.0f, 0.0f), 15) >> 45) == 2u);
    Rng rng(83);
    for (int bits : {9, 10, 12, 15, 20}) {
        const f64 bound = smallestThreeMaxError(bits);
        f64 worst = 0.0;
        for (int i = 0; i < 100000; ++i) {
            const Quat q = (i < 4) ? normalize(Quat(0.5f, -0.5f, 0.5f, i == 0 ? 0.5f : -0.5f)) : rng.quat();
            const u64 p = packQuatSmallestThree(q, bits);
            CHECK(p < (1ull << (2 + 3 * bits)));
            CHECK(p == packQuatSmallestThree(-q, bits));  // q and -q are the same rotation
            const Quat d = unpackQuatSmallestThree(p, bits);
            CHECK(isNormalized(d, 1e-6f));
            worst = max(worst, static_cast<f64>(angleBetween(toF64(q), toF64(d))));
        }
        MESSAGE("smallest-three " << bits << " bits/comp: max error " << degrees(worst) << " deg (bound "
                                  << degrees(bound) << ")");
        CHECK(worst <= bound);
        CHECK(worst >= 0.1 * bound);  // the bound is not absurdly loose
    }
    // Re-encoding a decoded quaternion is stable (what the receiver extrapolates from), unless
    // quantization made another component the largest (then only the index/order changes).
    int reindexed = 0;
    for (int i = 0; i < 10000; ++i) {
        const u32 p = packQuat32(rng.quat());
        const u32 p2 = packQuat32(unpackQuat32(p));
        if ((p2 >> 30) != (p >> 30)) {
            ++reindexed;
            CHECK(angleBetween(unpackQuat32(p2), unpackQuat32(p)) <=
                  static_cast<f32>(smallestThreeMaxError(10)));
            continue;
        }
        CHECK(p2 == p);
    }
    CHECK(reindexed < 100);
}

namespace {
bool q8kmValid(const DVec3& origin) {
    const PositionQuantizer q = PositionQuantizer::fromExtent(origin, 8000.0, 0.001);
    return q.isValid() && !PositionQuantizer{origin, 0.0, 24}.isValid() &&
           !PositionQuantizer{origin, 0.001, 33}.isValid() && !PositionQuantizer{origin, 0.001, 1}.isValid();
}
}  // namespace

TEST_CASE("pack: fixed-point positions relative to a cell origin") {
    const DVec3 origin{1.0e9, -2.0e8, 3.0e10};
    const PositionQuantizer q8km = PositionQuantizer::fromExtent(origin, 8000.0, 0.001);
    CHECK(q8km.bits == 24);  // 2^23 - 1 >= 8e6 mm
    CHECK(PositionQuantizer::fromExtent(origin, 33500.0, 0.001).bits == 26);
    CHECK(PositionQuantizer::fromExtent(origin, 2.0e6, 0.001).bits == 32);
    CHECK(PositionQuantizer::fromExtent(origin, 3.0e6, 0.001).bits == 0);  // needs more than 32 bits
    // The failure result is a usable value (no shift by -1 / UB in its accessors).
    const PositionQuantizer tooBig = PositionQuantizer::fromExtent(origin, 3.0e6, 0.001);
    CHECK_FALSE(tooBig.isValid());
    CHECK(tooBig.halfRange() == 0.0);
    CHECK_FALSE(tooBig.inRange(origin));
    CHECK(q8kmValid(origin));
    CHECK(approxEqual(q8km.halfRange(), 8388.608, 1e-9));
    CHECK(q8km.maxError() == 0.0005);

    Rng rng(84);
    for (const PositionQuantizer& q :
         {q8km, PositionQuantizer{origin, 0.01, 18}, PositionQuantizer{{}, 1.0 / 1024.0, 32}}) {
        const f64 extent = q.halfRange() * 0.999;
        for (int i = 0; i < 20000; ++i) {
            const DVec3 p = q.origin + rng.dvec3(-extent, extent);
            CHECK(q.inRange(p));
            const UVec3 code = q.encode(p);
            const DVec3 d = q.decode(code);
            CHECK(maxComponent(abs(d - p)) <= q.maxError() * (1.0 + 1e-6) + 4e-6);  // + 1 ulp at 3e10 m
            CHECK(q.encode(d) == code);  // idempotent: decoded positions re-encode bit-exactly
        }
        // Out of range clamps to the extreme codes.
        const DVec3 outside = q.origin + DVec3(q.halfRange() * 3.0, -q.halfRange() * 3.0, 0.0);
        CHECK_FALSE(q.inRange(outside));
        const UVec3 c = q.encode(outside);
        const u32 maxCode = static_cast<u32>((1ull << q.bits) - 1ull);
        CHECK(c.x == maxCode);
        CHECK(c.y == 0u);
        CHECK(q.encode(DVec3(std::nan(""), q.origin.y, q.origin.z)).x == 0u);
    }
    // The origin maps to the middle code; one step is exactly one resolution unit.
    const UVec3 mid = q8km.encode(origin);
    CHECK(mid == UVec3(1u << 23));
    CHECK(q8km.encode(origin + DVec3(0.001, -0.001, 0.0004)) ==
          UVec3((1u << 23) + 1u, (1u << 23) - 1u, 1u << 23));
}

TEST_CASE("pack: golden encodings (cross-platform bit identity)") {
    // Encoded bits are exchanged between Linux servers and Windows clients and must never drift.
    struct QuatGolden {
        Quat q;
        u32 packed32;
        u64 packed47;
    };
    // clang-format off
    static const QuatGolden quats[] = {
        {Quat(0.0f, 0.0f, 0.0f, 1.0f), 0xdff7fdffu, 0x00006fffdfffbfffull},
        {Quat(0.102597833f, 0.205195665f, 0.307793498f, 0.923380494f), 0xe49a4eddu, 0x000072522948dbdaull},
        {Quat(-0.702639818f, 0.10037712f, 0.05018856f, 0.702639818f), 0x1b676c03u, 0x00000dba5dba0067ull},
        {Quat(0.5f, 0.5f, -0.5f, 0.5f), 0x36825b68u, 0x00001b50095f6d40ull},
        {Quat(0.0f, -0.999997497f, 0.000999997486f, 0.00199999497f), 0x5ff7f9feu, 0x00002fffdff43fd1ull},
    };
    // clang-format on
    for (const QuatGolden& g : quats) {
        CHECK(packQuat32(g.q) == g.packed32);
        CHECK(packQuatSmallestThree(g.q, 15) == g.packed47);
    }
    struct OctGolden {
        Vec3 n;
        u32 oct16, oct24, oct32;
    };
    // clang-format off
    static const OctGolden octs[] = {
        {Vec3(0.267261237f, 0.534522474f, 0.801783681f), 0xa994u, 0xaa9954u, 0xaaa99554u},
        {Vec3(-0.309426397f, 0.928279161f, -0.20628427f), 0xe352u, 0xe47524u, 0xe4915249u},
        {Vec3(0.00999750011f, -0.0199950002f, -0.999750078f), 0x01fcu, 0x014fd6u, 0x013efd82u},
        {Vec3(-0.975900054f, -0.195180014f, 0.0975900069f), 0x6c1du, 0x6c41d8u, 0x6c4e1d8au},
    };
    // clang-format on
    for (const OctGolden& g : octs) {
        CHECK(packOct16(g.n) == g.oct16);
        CHECK(packOct24(g.n) == g.oct24);
        CHECK(packOct32(g.n) == g.oct32);
    }
    struct PosGolden {
        DVec3 p;
        UVec3 code;
    };
    const PositionQuantizer q{{1.0e9, -2.0e8, 3.0e10}, 0.001, 26};
    // clang-format off
    static const PosGolden positions[] = {
        {DVec3(1000001234.5678, -200009876.54321, 30000000000.000401), UVec3(34789000u, 23677889u, 33554432u)},
        {DVec3(999967000.0, -199999999.87654999, 30000000017.25), UVec3(554432u, 33554555u, 33571682u)},
    };
    // clang-format on
    for (const PosGolden& g : positions) CHECK(q.encode(g.p) == g.code);
}
