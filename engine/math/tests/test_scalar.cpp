#include "test_util.h"

#include <cmath>
#include <limits>
#include <set>

using namespace helios;
using helios::test::Rng;
using helios::test::ulpDistance;

namespace {
// The det:: accuracy tests compare against the C runtime. glibc's sin/cos/atan2/asin are
// (almost always) correctly rounded, so the measured distance is det's own error. Other CRTs
// (MSVC UCRT, MinGW's msvcrt) may themselves be off by an ulp in the opposite direction; allow
// that there instead of failing on the primary platform. det's results are bit-identical
// everywhere (golden tests), so accuracy verified against glibc holds on Windows too.
// Large arguments (|x| > 1e5) are only compared against glibc, whose reduction is exact for any
// argument; other CRTs are not required to be (det's large-argument bits are pinned by the golden
// test on every platform instead).
#if defined(__GLIBC__)
constexpr u64 kCrtSlackUlp = 0;
constexpr bool kCrtReducesHugeArgs = true;
#else
constexpr u64 kCrtSlackUlp = 1;
constexpr bool kCrtReducesHugeArgs = false;
#endif
}  // namespace

TEST_CASE("scalar: constants") {
    CHECK(kPiD == std::numbers::pi);
    CHECK(kPi == static_cast<f32>(std::numbers::pi));
    CHECK(kTwoPi == 2.0f * kPi);
    CHECK(kHalfPiD * 2.0 == kPiD);
    CHECK(approxEqual(kDegToRadD * 180.0, kPiD));
    CHECK(approxEqual(kRadToDeg * kPi, 180.0f));
    CHECK(approxEqual(kInvSqrt2D * kSqrt2D, 1.0));
    CHECK(approxEqual(kSqrt3 * kSqrt3, 3.0f));
    CHECK(kEpsilonT<f32> == 1e-6f);
    CHECK(kEpsilonT<f64> == 1e-12);
    static_assert(kPiT<f64> > 3.14159 && kPiT<f64> < 3.1416);
}

TEST_CASE("scalar: min/max/abs/sign/clamp") {
    static_assert(min(3, 5) == 3 && max(3, 5) == 5);
    // Qualified: with `using namespace helios`, unqualified abs(int) resolves to the C library's
    // non-template ::abs(int), which is not constexpr.
    static_assert(helios::abs(-4) == 4 && helios::abs(4u) == 4u && helios::abs(-2.5) == 2.5);
    static_assert(sign(-3.0) == -1.0 && sign(0.0) == 0.0 && sign(7) == 1 && sign(0u) == 0u);
    static_assert(clamp(5, 0, 3) == 3 && clamp(-1, 0, 3) == 0 && clamp(2, 0, 3) == 2);
    static_assert(saturate(1.5f) == 1.0f && saturate(-0.5f) == 0.0f);
    CHECK(std::signbit(helios::abs(-0.0)) == false);
    CHECK(std::isnan(clamp(std::nan(""), 0.0, 1.0)));  // NaN propagates (documented)
    CHECK(sign(std::nan("")) == 0.0);
    CHECK(square(-3.0f) == 9.0f);
}

TEST_CASE("scalar: interpolation and remapping") {
    static_assert(lerp(2.0, 6.0, 0.25) == 3.0);
    CHECK(lerp(1.0f, 3.0f, 0.0f) == 1.0f);
    CHECK(lerp(1.0f, 3.0f, 1.0f) == 3.0f);
    CHECK(inverseLerp(2.0, 6.0, 3.0) == 0.25);
    CHECK(remap(5.0, 0.0, 10.0, 100.0, 200.0) == 150.0);
    CHECK(remap(15.0, 0.0, 10.0, 100.0, 200.0) == 250.0);
    CHECK(remapClamped(15.0, 0.0, 10.0, 100.0, 200.0) == 200.0);
    CHECK(smoothstep(0.0, 1.0, -1.0) == 0.0);
    CHECK(smoothstep(0.0, 1.0, 2.0) == 1.0);
    CHECK(smoothstep(0.0, 1.0, 0.5) == 0.5);
    CHECK(smoothstep(2.0, 4.0, 3.0) == 0.5);
    CHECK(smootherstep(0.0, 1.0, 0.5) == 0.5);
    CHECK(smootherstep(0.0, 1.0, 1.0) == 1.0);
    // smoothstep is monotonic with zero slope at the ends
    f64 prev = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const f64 v = smoothstep(0.0, 1.0, i / 100.0);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(smoothstep(0.0, 1.0, 1e-4) < 1e-7);
    CHECK(step(0.5, 0.49) == 0.0);
    CHECK(step(0.5, 0.5) == 1.0);
    CHECK(fract(-0.25) == 0.75);
    CHECK(fract(2.5f) == 0.5f);
    CHECK(moveTowards(0.0, 10.0, 3.0) == 3.0);
    CHECK(moveTowards(9.0, 10.0, 3.0) == 10.0);
    CHECK(moveTowards(0.0, -10.0, 3.0) == -3.0);
}

TEST_CASE("scalar: approximate comparisons") {
    CHECK(approxEqual(1.0, 1.0 + 1e-13));
    CHECK_FALSE(approxEqual(1.0, 1.0 + 1e-9));
    CHECK(approxEqual(1e9, 1e9 * (1.0 + 1e-13)));  // relative tolerance
    CHECK(approxEqual(1e-20, 0.0));                // absolute tolerance
    CHECK(approxEqual(kInfinityD, kInfinityD));
    CHECK_FALSE(approxEqual(kInfinityD, -kInfinityD));
    CHECK_FALSE(approxEqual(std::nan(""), std::nan("")));
    CHECK(approxEqual(1.0f, 1.1f, 0.2f));
    CHECK(approxZero(1e-7f));
    CHECK_FALSE(approxZero(1e-5f));
    CHECK(isFinite(1.0));
    CHECK_FALSE(isFinite(kInfinity));
}

TEST_CASE("scalar: safe reciprocal square root and division") {
    CHECK(rsqrt(4.0) == 0.5);
    CHECK(safeRsqrt(4.0f) == 0.5f);
    CHECK(safeRsqrt(0.0) == 0.0);
    CHECK(safeRsqrt(-1.0, 7.0) == 7.0);
    CHECK(safeRsqrt(std::nan("")) == 0.0);
    CHECK(safeRsqrt(kInfinityD) == 0.0);
    CHECK(safeRsqrt(std::numeric_limits<f64>::denorm_min()) == 0.0);
    CHECK(safeRsqrt(std::numeric_limits<f32>::min()) > 0.0f);
    CHECK(safeDiv(1.0, 4.0) == 0.25);
    CHECK(safeDiv(1.0, 0.0, -1.0) == -1.0);
    CHECK(safeDiv(1.0f, 1e-40f) == 0.0f);
}

TEST_CASE("scalar: angles") {
    CHECK(approxEqual(radians(180.0), kPiD));
    CHECK(approxEqual(degrees(kHalfPi), 90.0f));
    Rng rng(1);
    for (int i = 0; i < 10000; ++i) {
        const f64 a = rng.range(-1000.0, 1000.0);
        const f64 w = wrapAngle(a);
        CHECK(w >= -kPiD);
        CHECK(w <= kPiD);
        // Same angle: difference is a multiple of 2*pi.
        const f64 k = (a - w) / kTwoPiD;
        CHECK(std::fabs(k - std::round(k)) < 1e-9);
        const f64 wp = wrapAnglePositive(a);
        CHECK(wp >= 0.0);
        CHECK(wp < kTwoPiD);
        const f32 wf = wrapAnglePositive(static_cast<f32>(a));
        CHECK(wf >= 0.0f);
        CHECK(wf < kTwoPi);
    }
    CHECK(wrapAnglePositive(-1e-9f) < kTwoPi);
    CHECK(wrapAnglePositive(-1e-30) >= 0.0);
    CHECK(approxEqual(wrapAngle(-1.5 * kPiD), 0.5 * kPiD));
    CHECK(approxEqual(angleDelta(3.0, -3.0), kTwoPiD - 6.0));
    CHECK(approxEqual(angleDelta(-3.0, 3.0), 6.0 - kTwoPiD));
    CHECK(approxEqual(lerpAngle(3.0, -3.0, 0.5), 3.0 + (kTwoPiD - 6.0) * 0.5));
}

TEST_CASE("scalar: fast deterministic floor/ceil/round") {
    static_assert(floorToI32(-0.5) == -1 && floorToI32(-1.0) == -1 && floorToI32(1.999f) == 1);
    static_assert(floorToI32(0.0) == 0 && floorToI32(-0.0) == 0 && ceilToI32(0.1) == 1 &&
                  ceilToI32(-0.9) == 0);
    CHECK(floorToI32(2.0e9) == 2000000000);
    CHECK(floorToI32(-2.0e9 - 0.5) == -2000000001);
    CHECK(floorToI64(1e15 + 0.5) == 1000000000000000LL);
    CHECK(floorToI64(-1e15 - 0.5) == -1000000000000001LL);
    CHECK(roundToI32(2.5) == 3);
    CHECK(roundToI32(-2.5) == -3);
    CHECK(roundToI32(0.49999997f) == 0);
    Rng rng(2);
    for (int i = 0; i < 10000; ++i) {
        const f64 v = rng.range(-1e6, 1e6);
        CHECK(floorToI32(v) == static_cast<i32>(std::floor(v)));
        CHECK(ceilToI32(v) == static_cast<i32>(std::ceil(v)));
    }
    CHECK(modPositive(-1, 5) == 4);
    CHECK(modPositive(-5, 5) == 0);
    CHECK(modPositive(7, -5) == 2);
    CHECK(modPositive(7u, 5u) == 2u);
    CHECK(modPositive(-0.5, 2.0) == 1.5);
    CHECK(modPositive(4.5, 2.0) == 0.5);
    // Regression: just below a multiple of m, a / m rounds up to that multiple and the naive
    // a - m * floor(a / m) went slightly negative (e.g. -9e-13), violating [0, m).
    Rng rngM(21);
    for (int i = 0; i < 100000; ++i) {
        const f64 m = rngM.range(0.001, 10.0);
        const f64 a = std::nextafter(static_cast<f64>(1 + i % 1000) * m, 0.0);
        const f64 r = modPositive(a, m);
        CHECK(r >= 0.0);
        CHECK(r < m);
        const f32 mf = static_cast<f32>(m);
        const f32 rf = modPositive(std::nextafter(static_cast<f32>(1 + i % 1000) * mf, 0.0f), mf);
        CHECK(rf >= 0.0f);
        CHECK(rf < mf);
    }
}

TEST_CASE("scalar: integer hashing") {
    // hashU32 is a bijection: no collisions over a large contiguous range.
    std::set<u32> seen;
    for (u32 i = 0; i < 65536; ++i) seen.insert(hashU32(i));
    CHECK(seen.size() == 65536);
    // Avalanche: flipping one input bit flips ~half the output bits.
    Rng rng(3);
    f64 total = 0.0;
    int samples = 0;
    for (int i = 0; i < 2000; ++i) {
        const u32 x = rng.nextU32();
        for (int b = 0; b < 32; ++b) {
            total += std::popcount(hashU32(x) ^ hashU32(x ^ (1u << b)));
            ++samples;
        }
    }
    const f64 mean = total / samples;
    CHECK(mean > 15.5);
    CHECK(mean < 16.5);
    // Pinned values: the hashes feed deterministic procgen and must never change.
    CHECK(hashU32(0u) == 0u);
    CHECK(hashU32(1u) == 0x688990c0u);
    CHECK(hashU32(0xdeadbeefu) == 0xe628c683u);
    CHECK(hashU64(1ull) == 0x5692161d100b05e5ull);
    CHECK(hashU64(0x0123456789abcdefull) == 0xb2c058e4ebb5112cull);
    CHECK(hashCombineU32(1u, 2u) != hashCombineU32(2u, 1u));
    CHECK(deriveSeed(42ull, 0u) != deriveSeed(42ull, 1u));
    CHECK(deriveSeed(42ull, 7u) == deriveSeed(42ull, 7u));
    CHECK(hashToUnitF32(0xFFFFFFFFu) < 1.0f);
    CHECK(hashToUnitF32(0u) == 0.0f);
    CHECK(hashToUnitF64(~0ull) < 1.0);
}

// ---------------------------------------------------------------------------------------------
// Deterministic trigonometry
// ---------------------------------------------------------------------------------------------
TEST_CASE("det: sin/cos accuracy against the CRT") {
    Rng rng(4);
    u64 worstSin = 0, worstCos = 0;
    // Up to 2^30 (where the fmod fold starts). The 1e7..1e9 ranges are a regression test: the
    // 3-part Cody-Waite reduction's products stop being exact at |n| >= 2^22 (|x| > ~6.6e6), which
    // used to cost up to ~3e11 ulp at 1e9.
    const f64 ranges[] = {1e5, 100.0, 4.0, 1e-3, 3e6, 1e7, 1e8, 1073741823.0};
    const int rangeCount = kCrtReducesHugeArgs ? 8 : 4;
    for (int i = 0; i < 240000; ++i) {
        const f64 range = ranges[i % rangeCount];
        const f64 x = rng.range(-range, range);
        const f64 s = det::sin(x), c = det::cos(x);
        const f64 rs = std::sin(x), rc = std::cos(x);
        // Compare in ulps of the result, with an absolute floor for results near zero.
        if (std::fabs(rs) > 1e-3)
            worstSin = max(worstSin, ulpDistance(s, rs));
        else
            CHECK(std::fabs(s - rs) <= 1e-16);
        if (std::fabs(rc) > 1e-3) worstCos = max(worstCos, ulpDistance(c, rc));
        f64 ss = 0.0, cc = 0.0;
        det::sinCos(x, ss, cc);
        CHECK(ss == s);
        CHECK(cc == c);
    }
    MESSAGE("det::sin worst ulp " << worstSin << ", det::cos worst ulp " << worstCos);
    CHECK(worstSin <= 2 + kCrtSlackUlp);
    CHECK(worstCos <= 2 + kCrtSlackUlp);
    // Arguments next to large multiples of pi/2 (maximal cancellation in the reduction).
    u64 worstNear = 0;
    const f64 kMax = kCrtReducesHugeArgs ? 6.8e8 : 6.3e4;  // |x| < 2^30 resp. 1e5
    for (f64 k = 3.0; k < kMax; k = std::floor(k * 1.125) + 7.0) {
        const f64 x0 = k * kHalfPiD;
        for (f64 x : {x0, std::nextafter(x0, 0.0), std::nextafter(x0, 2.0 * x0)}) {
            worstNear = max(worstNear, ulpDistance(det::sin(x), std::sin(x)));
            worstNear = max(worstNear, ulpDistance(det::cos(x), std::cos(x)));
        }
    }
    MESSAGE("det::sin/cos worst ulp next to multiples of pi/2: " << worstNear);
    CHECK(worstNear <= 2 + kCrtSlackUlp);
    // Exact special cases.
    CHECK(det::sin(0.0) == 0.0);
    CHECK(std::signbit(det::sin(-0.0)));
    CHECK(det::cos(0.0) == 1.0);
    CHECK(std::isnan(det::sin(kInfinityD)));
    CHECK(std::isnan(det::cos(std::nan(""))));
    CHECK(det::sin(1e-300) == 1e-300);
    // Huge arguments stay finite and bounded (deterministic, reduced accuracy).
    CHECK(std::fabs(det::sin(1e22)) <= 1.0);
    CHECK(std::fabs(det::cos(-3e300)) <= 1.0);
    // f32 overloads round the f64 result once.
    CHECK(det::sin(0.5f) == static_cast<f32>(det::sin(0.5)));
}

TEST_CASE("det: atan/atan2/asin/acos accuracy and special cases") {
    Rng rng(5);
    u64 worst = 0;
    for (int i = 0; i < 200000; ++i) {
        const f64 scale = std::pow(10.0, rng.range(-6.0, 6.0));
        const f64 y = rng.range(-1.0, 1.0) * scale;
        const f64 x = rng.range(-1.0, 1.0) * std::pow(10.0, rng.range(-6.0, 6.0));
        worst = max(worst, ulpDistance(det::atan2(y, x), std::atan2(y, x)));
        worst = max(worst, ulpDistance(det::atan(y), std::atan(y)));
    }
    MESSAGE("det::atan2/atan worst ulp " << worst);
    CHECK(worst <= 2 + kCrtSlackUlp);
    u64 worstInv = 0;
    for (int i = 0; i < 100000; ++i) {
        const f64 v = rng.range(-1.0, 1.0);
        worstInv = max(worstInv, ulpDistance(det::asin(v), std::asin(v)));
        if (v > -0.99) worstInv = max(worstInv, ulpDistance(det::acos(v), std::acos(v)));
    }
    MESSAGE("det::asin/acos worst ulp " << worstInv);
    CHECK(worstInv <= 3 + kCrtSlackUlp);
    // C99 Annex F special cases must match std::atan2 exactly (including signed zeros).
    const f64 inf = kInfinityD;
    const f64 specials[] = {0.0, -0.0, 1.0, -1.0, inf, -inf, 1e-310, -1e-310};
    for (f64 y : specials) {
        for (f64 x : specials) {
            const f64 d = det::atan2(y, x), r = std::atan2(y, x);
            CAPTURE(y);
            CAPTURE(x);
            CHECK(ulpDistance(d, r) <= 1 + kCrtSlackUlp);
            CHECK(std::signbit(d) == std::signbit(r));
        }
    }
    CHECK(std::isnan(det::atan2(std::nan(""), 1.0)));
    CHECK(det::atan(inf) == kHalfPiD);
    CHECK(std::signbit(det::atan(-0.0)));
    CHECK(det::asin(1.0) == kHalfPiD);
    CHECK(det::acos(1.0) == 0.0);
    CHECK(det::acos(-1.0) == kPiD);
    CHECK(std::isnan(det::asin(1.5)));
}

TEST_CASE("det: golden values (cross-platform bit identity)") {
    // Pinned bit patterns. If this fails on one platform only, that platform's compiler is not
    // honouring the FP contract (see src/fp_control.h); fix the build, not the numbers.
    struct G {
        f64 x;
        u64 sinBits, cosBits, atanBits;
    };
    // clang-format off
    static const G golden[] = {
        {0.10000000000000001, 0x3fb98eaecb8bcb2cull, 0x3fefd712f9a817c1ull, 0x3fc0f772d81fdc0bull},
        {-0.5, 0xbfdeaee8744b05f0ull, 0x3fec1528065b7d50ull, 0xbfe2d0ead6066395ull},
        {1.0, 0x3feaed548f090ceeull, 0x3fe14a280fb5068cull, 0x3fedac670561bb50ull},
        {2.5, 0x3fe326af0dcfcab0ull, 0xbfe9a2f7ef858b7dull, 0x3ff4782cbabc8157ull},
        {-3.1415899999999999, 0xbec6428a6aa44cd1ull, 0xbfefffffffff8420ull, 0xbff562197232d475ull},
        {10.0, 0xbfe1689ef5f34f53ull, 0xbfead9ac890c6b1full, 0x3ff7ef5b16e8f4a2ull},
        {123.456, 0xbfe9b9dadc41aeb5ull, 0xbfe307e5980a1559ull, 0x3ff90919447ed555ull},
        {-9876.5432099999998, 0x3fe2b316abe9cf8cull, 0x3fe9f7c14a7ffd2full, 0xbff921abb3f2ebeeull},
        {100000.10000000001, 0xbfb06f5f37ac2a23ull, 0xbfefef19c3fb0a7aull, 0x3ff921f377009db4ull},
        {0.78539816339744828, 0x3fe6a09e667f3bccull, 0x3fe6a09e667f3bcdull, 0x3fe9ded0009ac5aeull},
        {1e-08, 0x3e45798ee2308c3aull, 0x3ff0000000000000ull, 0x3e4ca213d840baf8ull},
        {4000000.2999999998, 0xbfecf1d76dc0be6cull, 0x3fdb4a973d2a425cull, 0x3ff921fb21ef4675ull},
        // 2^20 <= |n| < 2^30: five-part double-double reduction.
        {12345678.9, 0xbfecb6bcc6c2b873ull, 0x3fdc4004c876f9c8ull, 0x3ff921fb43f577c8ull},
        {-123456789.125, 0xbfeffefcc2ee7d37ull, 0x3f9019b317c2d95eull, 0xbff921fb52a2b490ull},
        {987654321.5, 0x3febc43e2a6664feull, 0xbfdfd02371fd7022ull, 0x3ff921fb540ffe07ull},
        {1073741823.0, 0xbfefdb6a55d0ac16ull, 0xbfb82ac4b53cde6bull, 0x3ff921fb54142d18ull},
        {-2000000000.0, 0xbfed454eef10bd7full, 0x3fd9dcef4acb960dull, 0xbff921fb542a6806ull},
        {1000000000000000.0, 0x3feacdc9d9884216ull, 0xbfe17ae632761c3aull, 0x3ff921fb54442d15ull},
    };
    // clang-format on
    for (const G& g : golden) {
        CAPTURE(g.x);
        CHECK(helios::test::bits(det::sin(g.x)) == g.sinBits);
        CHECK(helios::test::bits(det::cos(g.x)) == g.cosBits);
        CHECK(helios::test::bits(det::atan2(g.x, 0.75)) == g.atanBits);
    }
}
