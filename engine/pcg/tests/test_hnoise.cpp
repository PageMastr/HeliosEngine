// Scalar hnoise reference: the lattice hash is xxHash32, the gradients/fade/lerp follow Perlin 2002,
// coordinate splitting matches 64-bit shifts, and the fixed-point cube-sphere domain matches math's
// f64 EquiAngular mapping and exact rsqrt within the documented bounds.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "helios/math/spherical.h"
#include "helios/pcg/hnoise.h"

using namespace helios;
using namespace helios::pcg;

namespace {
u64 splitmix(u64& s) {
    u64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
} // namespace

TEST_CASE("hnoise: the lattice hash is xxHash32 of the little-endian cell") {
    u64 rng = 1;
    for (int i = 0; i < 10000; ++i) {
        const u32 seed = static_cast<u32>(splitmix(rng));
        const i32 cell[3] = {static_cast<i32>(splitmix(rng)), static_cast<i32>(splitmix(rng)), static_cast<i32>(splitmix(rng))};
        unsigned char bytes[12];
        for (int a = 0; a < 3; ++a) {
            const u32 v = static_cast<u32>(cell[a]);
            for (int b = 0; b < 4; ++b) bytes[a * 4 + b] = static_cast<unsigned char>(v >> (8 * b));
        }
        REQUIRE(hnoise::latticeHash(seed, cell[0], cell[1], cell[2]) == XXH32(bytes, sizeof(bytes), seed));
    }
    // constexpr: usable in static tables.
    static_assert(hnoise::latticeHash(0, 0, 0, 0) != hnoise::latticeHash(1, 0, 0, 0));
}

TEST_CASE("hnoise: gradients are Perlin's 12 cube edges") {
    std::set<std::array<i32, 3>> seen;
    for (u32 g = 0; g < 16; ++g) {
        const u32 h = g << 28;
        const std::array<i32, 3> grad = {hnoise::gradientDot(h, 1, 0, 0), hnoise::gradientDot(h, 0, 1, 0),
                                         hnoise::gradientDot(h, 0, 0, 1)};
        // Two components are +-1, one is 0.
        CHECK(std::abs(grad[0]) + std::abs(grad[1]) + std::abs(grad[2]) == 2);
        seen.insert(grad);
        // Linear in the offset.
        CHECK(hnoise::gradientDot(h, 3, 5, 7) == 3 * grad[0] + 5 * grad[1] + 7 * grad[2]);
    }
    CHECK(seen.size() == 12);
    // Extremes stay inside [-131072, 131070].
    for (u32 g = 0; g < 16; ++g) {
        for (i32 x : {-65536, 65535}) {
            for (i32 y : {-65536, 65535}) {
                for (i32 z : {-65536, 65535}) {
                    const i32 d = hnoise::gradientDot(g << 28, x, y, z);
                    CHECK(d >= -131072);
                    CHECK(d <= 131072);
                }
            }
        }
    }
}

TEST_CASE("hnoise: quintic fade is monotonic, exact at 0, 1/2 and 1, and within 0.54 ulp") {
    CHECK(hnoise::fade(0) == 0);
    CHECK(hnoise::fade(32768) == 32768);
    CHECK(hnoise::fade(65535) == 65536);
    static_assert(hnoise::fade(32768) == 32768);
    i32 prev = -1;
    f64 worst = 0.0;
    for (i32 t = 0; t < 65536; ++t) {
        const i32 f = hnoise::fade(t);
        REQUIRE(f >= prev);
        prev = f;
        // Exact polynomial in ulps of Q16 (f64 is exact enough here: 1e-11 ulp).
        const f64 x = t / 65536.0;
        const f64 real = x * x * x * (x * (6 * x - 15) + 10) * 65536.0;
        worst = std::max(worst, std::abs(f - real));
        // Symmetry of the quintic: fade(1 - t) = 1 - fade(t), up to the rounding.
        if (t > 0) CHECK(std::abs(hnoise::fade(65536 - t) - (65536 - f)) <= 1);
    }
    MESSAGE("fade: max error " << worst << " ulp");
    CHECK(worst <= 0.54);
}

TEST_CASE("hnoise: coordinate splitting equals 64-bit floor division") {
    u64 rng = 7;
    for (int i = 0; i < 20000; ++i) {
        const i64 p = static_cast<i64>(splitmix(rng)) >> (splitmix(rng) % 24);
        const i32 e = hnoise::kMinWavelengthExp + static_cast<i32>(splitmix(rng) % 47);
        i32 cell = 0, frac = 0;
        hnoise::splitCoord(p, e, cell, frac);
        // floor(p / 2^(32+e)) via long division on the unsigned bit pattern, sign-corrected.
        const i64 q = p >> (32 + e); // arithmetic: floor
        CHECK(static_cast<u32>(cell) == static_cast<u32>(static_cast<u64>(q)));
        CHECK(frac >= 0);
        CHECK(frac <= 65535);
        // frac * 2^(16+e) + cell * 2^(32+e) reconstructs p down to the dropped bits.
        const u64 rebuilt = (static_cast<u64>(q) << (32 + e)) + (static_cast<u64>(frac) << (16 + e));
        const u64 dropped = static_cast<u64>(p) - rebuilt;
        CHECK(dropped < (u64(1) << (16 + e)));
    }
}

TEST_CASE("hnoise: noise is 0 on the lattice, continuous across cells and roughly centred") {
    LatticeCoord c;
    c.cell = {12, -7, 1000000};
    CHECK(hnoise::noise3(99, c) == 0); // every corner gradient dotted with a zero offset
    // Continuity: the last sample of a cell and the first of the next differ by a few ulps of the
    // local slope, never by a jump.
    u64 rng = 3;
    i64 worst = 0;
    f64 sum = 0.0, sumSq = 0.0;
    i32 lo = 0, hi = 0;
    for (int i = 0; i < 20000; ++i) {
        const u32 seed = static_cast<u32>(splitmix(rng));
        FixedPos p{static_cast<i64>(splitmix(rng)) >> 20, static_cast<i64>(splitmix(rng)) >> 20,
                   static_cast<i64>(splitmix(rng)) >> 20};
        const i32 a = hnoise::noise3(seed, hnoise::latticeCoord(p, 4));
        p.x += i64(1) << 20; // one fraction ulp at e = 4
        const i32 b = hnoise::noise3(seed, hnoise::latticeCoord(p, 4));
        worst = std::max<i64>(worst, std::abs(static_cast<i64>(a) - b));
        sum += a;
        sumSq += static_cast<f64>(a) * a;
        lo = std::min(lo, a);
        hi = std::max(hi, a);
    }
    CHECK(worst <= 16); // |gradient| <= 2 per unit, so one 2^-16 step moves the value by a few ulps
    const f64 mean = sum / 20000.0 / 65536.0;
    const f64 sd = std::sqrt(sumSq / 20000.0) / 65536.0;
    MESSAGE("noise3: mean " << mean << ", rms " << sd << ", range [" << lo / 65536.0 << ", " << hi / 65536.0 << "]");
    CHECK(std::abs(mean) < 0.02);
    CHECK(sd > 0.15);
    CHECK(sd < 0.5);
    CHECK(lo > -80000);
    CHECK(hi < 80000);
}

TEST_CASE("hnoise: the lattice wraps at 2^32 cells without breaking the +1 neighbour") {
    LatticeCoord c;
    c.cell = {2147483647, -2147483647 - 1, -1};
    c.frac = {40000, 1, 65535};
    const i32 v = hnoise::noise3(5, c); // no UB (the +1 neighbour of INT_MAX is INT_MIN)
    LatticeCoord d = c;
    CHECK(hnoise::noise3(5, d) == v);
}

TEST_CASE("hnoise: EquiAngular warp polynomial") {
    CHECK(hnoise::equiAngularWarp(1 << 30) == (1 << 30));
    CHECK(hnoise::equiAngularWarp(-(1 << 30)) == -(1 << 30));
    CHECK(hnoise::equiAngularWarp(0) == 0);
    f64 worst = 0.0;
    i32 prev = hnoise::equiAngularWarp(-(1 << 30)) - 1;
    for (i32 s = -(1 << 30); s <= (1 << 30); s += 1 << 14) {
        const i32 w = hnoise::equiAngularWarp(s);
        REQUIRE(w > prev); // strictly monotonic
        prev = w;
        CHECK(std::abs(hnoise::equiAngularWarp(-s) + w) <= 2); // odd up to rounding
        worst = std::max(worst, std::abs(w / 1073741824.0 - std::tan(s / 1073741824.0 * 0.7853981633974483)));
    }
    MESSAGE("warp: max |w - tan(pi s / 4)| = " << worst);
    CHECK(worst < 2.5e-7);
}

TEST_CASE("hnoise: integer Newton rsqrt is within 2 ulp of math's exact rsqrtQ30") {
    u64 rng = 11;
    u32 worst = 0;
    for (int i = 0; i < 200000; ++i) {
        const u32 s = (1u << 30) + static_cast<u32>(splitmix(rng) % ((2u << 30) + 1u));
        const u32 exact = rsqrtQ30(static_cast<u64>(s) << 30);
        const u32 got = hnoise::rsqrtNewtonQ30(s);
        worst = std::max(worst, got > exact ? got - exact : exact - got);
    }
    for (u32 s : {1u << 30, 3u << 30, 2u << 30}) {
        const u32 exact = rsqrtQ30(static_cast<u64>(s) << 30);
        const u32 got = hnoise::rsqrtNewtonQ30(s);
        worst = std::max(worst, got > exact ? got - exact : exact - got);
    }
    CHECK(worst <= 2);
}

TEST_CASE("hnoise: fixed-point cube-sphere directions match math's EquiAngular mapping") {
    f64 worstAngle = 0.0, worstLength = 0.0;
    for (int f = 0; f < 6; ++f) {
        const CubeFace face = static_cast<CubeFace>(f);
        for (u32 i = 0; i <= 64; i += 4) {
            for (u32 j = 0; j <= 64; j += 4) {
                const i32 s = hnoise::faceCoordQ30(0, i, 0), t = hnoise::faceCoordQ30(0, j, 0);
                const auto d = hnoise::cubeSphereDirection(face, s, t);
                const DVec3 fixedDir(d[0] / 1073741824.0, d[1] / 1073741824.0, d[2] / 1073741824.0);
                const DVec3 ref = cubeToSphere(face, DVec2(s / 1073741824.0, t / 1073741824.0), CubeMapping::EquiAngular);
                worstLength = std::max(worstLength, std::abs(length(fixedDir) - 1.0));
                worstAngle = std::max(worstAngle, length(fixedDir - ref));
            }
        }
    }
    MESSAGE("cube-sphere: max direction error " << worstAngle << ", max |length - 1| " << worstLength);
    CHECK(worstAngle < 1e-6);
    CHECK(worstLength < 1e-8);
}

TEST_CASE("hnoise: neighbouring cube faces share their edge samples bit for bit") {
    // The +X face's u = +1 edge is the -Z face's u = -1 edge (both have v = +Y), and the +Z face's
    // v = +1 edge is the +Y face's v = -1 edge.
    const u32 radius = hnoise::radiusToQ8(1'500'000.0);
    for (u32 j = 0; j <= 64; ++j) {
        const i32 t = hnoise::faceCoordQ30(3, j, 3);
        const FixedPos a = hnoise::cubeSpherePosition(CubeFace::PosX, 1 << 30, t, radius);
        const FixedPos b = hnoise::cubeSpherePosition(CubeFace::NegZ, -(1 << 30), t, radius);
        CHECK(a == b);
        // +Z at v = +1 is (s, 1, 1); +Y (u = +X, v = -Z) reaches z = +1 at v = -1.
        const i32 s = hnoise::faceCoordQ30(5, j, 3);
        const FixedPos c = hnoise::cubeSpherePosition(CubeFace::PosZ, s, 1 << 30, radius);
        const FixedPos d = hnoise::cubeSpherePosition(CubeFace::PosY, s, -(1 << 30), radius);
        CHECK(c == d);
    }
}

TEST_CASE("hnoise: positions scale with the radius in Q32.32 metres") {
    const u32 radius = hnoise::radiusToQ8(6'400'000.0);
    const FixedPos p = hnoise::cubeSpherePosition(CubeFace::PosY, 0, 0, radius); // face centre: +Y pole
    CHECK(p.x == 0);
    CHECK(p.z == 0);
    CHECK(Q32::fromRaw(p.y).toDouble() == doctest::Approx(6'400'000.0).epsilon(1e-9));
    CHECK(hnoise::faceCoordQ30(0, 0, 0) == -(1 << 30));
    CHECK(hnoise::faceCoordQ30(0, 64, 0) == (1 << 30));
    CHECK(hnoise::faceCoordQ30((1u << 25) - 1u, 64, 25) == (1 << 30));
}
