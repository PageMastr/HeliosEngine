#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <vector>

#include "helios/core/random.h"

using namespace helios;

// Golden sequences pin the exact output on every platform/compiler. Reference values come from the
// published reference implementations (pcg32-demo, splitmix64.c, xoshiro256starstar.c) and an
// independent Python model of the distribution algorithms.

TEST_CASE("random: SplitMix64 golden sequence") {
    SplitMix64 a(0);
    CHECK(a.next() == 0xe220a8397b1dcdafull);
    CHECK(a.next() == 0x6e789e6aa1b965f4ull);
    CHECK(a.next() == 0x06c45d188009454full);
    CHECK(a.next() == 0xf88bb8a8724c81ecull);
    SplitMix64 b(1234567);
    const u64 expected[] = {0x599ed017fb08fc85ull, 0x2c73f08458540fa5ull, 0x883ebce5a3f27c77ull,
                            0x3fbef740e9177b3full, 0xe3b8346708cb5ecdull};
    for (u64 e : expected) CHECK(b.next() == e);
    constexpr u64 first = [] {
        SplitMix64 s(0);
        return s.next();
    }();
    static_assert(first == 0xe220a8397b1dcdafull);
}

TEST_CASE("random: PCG32 golden sequence (pcg32-demo, seed 42 stream 54)") {
    Pcg32 rng(42, 54);
    const u32 expected[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u, 0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
    for (u32 e : expected) CHECK(rng.next() == e);
}

TEST_CASE("random: Xoshiro256** golden sequence") {
    Xoshiro256 zero(0);
    const u64 e0[] = {0x99ec5f36cb75f2b4ull, 0xbf6e1f784956452aull, 0x1a5f849d4933e6e0ull, 0x6aa594f1262d2d2cull,
                      0xbba5ad4a1f842e59ull};
    for (u64 e : e0) CHECK(zero.next() == e);
    Xoshiro256 answer(42);
    const u64 e42[] = {0x15780b2e0c2ec716ull, 0x6104d9866d113a7eull, 0xae17533239e499a1ull, 0xecb8ad4703b360a1ull,
                       0xfde6dc7fe2ec5e64ull};
    for (u64 e : e42) CHECK(answer.next() == e);
    Xoshiro256 jumped(0);
    jumped.jump();
    CHECK(jumped.next() == 0x376215edc846d62cull);
    CHECK(jumped.next() == 0x57c0611de8350ca7ull);
}

TEST_CASE("random: distributions are exact and platform-stable") {
    Xoshiro256 rng(7);
    const i64 ints[] = {4, -5, 7, 10, 10, 8, -9, -8};
    for (i64 e : ints) CHECK(uniformInt(rng, -10, 10) == e);

    Xoshiro256 wide(7);
    const u64 big[] = {645575224530ull, 316035332502ull, 494432973376ull, 978013228105ull};
    for (u64 e : big) CHECK(uniformU64Inclusive(wide, 1'000'000'000'000ull) == e);

    Xoshiro256 fr(7);
    const u32 floatBits[] = {11753722u, 4676669u, 14086611u, 16460088u};
    for (u32 e : floatBits) CHECK(uniformFloat01(fr) == static_cast<f32>(e) / 16777216.0f);

    Xoshiro256 dr(7);
    const u64 doubleBits[] = {6310231968177966ull, 2510767866374405ull, 7562691848873359ull, 8836942697582606ull};
    for (u64 e : doubleBits) CHECK(uniformDouble01(dr) == static_cast<f64>(e) / 9007199254740992.0);

    Random shuffler(99);
    std::array<int, 10> items{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    shuffler.shuffle(std::span<int>(items));
    CHECK(items == std::array<int, 10>{2, 7, 0, 6, 1, 4, 8, 9, 5, 3});
}

namespace {
/// Generator stub that always returns all-one bits: the largest possible unit value 1 - 2^-24/-53.
struct MaxBitsRng {
    constexpr u32 nextU32() noexcept { return 0xFFFFFFFFu; }
    constexpr u64 nextU64() noexcept { return ~0ull; }
};
} // namespace

TEST_CASE("random: float ranges stay half-open even when rounding reaches hi") {
    // Regression: 1 + (2 - 1) * (1 - 2^-24) rounds (ties-to-even) to exactly 2.0f, so a value "in
    // [1, 2)" could be 2 and an index computed from it could run out of bounds.
    MaxBitsRng rng;
    CHECK(uniformFloat01(rng) < 1.0f);
    const f32 f = uniformFloat(rng, 1.0f, 2.0f);
    CHECK(f < 2.0f);
    CHECK(f == helios::detail::floatBelow(2.0f));
    CHECK(uniformDouble(rng, 1.0, 2.0) < 2.0);
    CHECK(uniformFloat(rng, -3.0f, -1.0f) < -1.0f);
    CHECK(uniformFloat(rng, -1.0f, 0.0f) < 0.0f);
    CHECK(uniformDouble(rng, 100.0, 101.0) < 101.0);
    CHECK(uniformFloat(rng, 5.0f, 5.0f) == 5.0f); // degenerate range returns lo

    static_assert(helios::detail::floatBelow(1.0f) == 0.99999994f);
    static_assert(helios::detail::floatBelow(-1.0f) == -1.00000012f);
    static_assert(helios::detail::floatBelow(0.0f) < 0.0f);
    static_assert(helios::detail::doubleBelow(1.0) == 0.99999999999999989);
    // Typical values are untouched (the golden sequences above still hold).
    Random r(7);
    for (int i = 0; i < 100'000; ++i) {
        const f32 v = r.range(0.0f, 10.0f);
        REQUIRE(v >= 0.0f);
        REQUIRE(v < 10.0f);
    }
}

TEST_CASE("random: ranges, bounds and rough uniformity") {
    Random r(123);
    std::array<int, 6> buckets{};
    for (int i = 0; i < 60000; ++i) {
        const int v = r.range(1, 6);
        REQUIRE(v >= 1);
        REQUIRE(v <= 6);
        ++buckets[static_cast<usize>(v - 1)];
    }
    for (int b : buckets) CHECK(std::abs(b - 10000) < 500);
    for (int i = 0; i < 10000; ++i) {
        const f32 f = r.nextFloat();
        CHECK(f >= 0.0f);
        CHECK(f < 1.0f);
        const f64 d = r.range(-2.0, 3.0);
        CHECK(d >= -2.0);
        CHECK(d < 3.0);
        CHECK(r.index(7) < 7);
    }
    CHECK(r.range(5, 5) == 5);
    (void)r.range(u64{0}, ~u64{0}); // full 64-bit range takes the raw-output path
    CHECK(r.range(i64{-3}, i64{-3}) == -3);
    CHECK(uniformU32Below(r.generator(), 1) == 0);
    int hits = 0;
    for (int i = 0; i < 10000; ++i) hits += r.chance(0.25) ? 1 : 0;
    CHECK(std::abs(hits - 2500) < 250);

    // Same seed, same stream; jump() separates streams.
    Random a(5);
    Random b(5);
    for (int i = 0; i < 100; ++i) CHECK(a.nextU64() == b.nextU64());
    Xoshiro256 s1(5);
    Xoshiro256 s2(5);
    s2.jump();
    CHECK(s1.next() != s2.next());
}
