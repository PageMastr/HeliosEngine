#pragma once
// Deterministic, platform-stable random number generators.
//
// Procedural generation and simulation must produce bit-identical results on Windows clients and
// Linux servers (ADR-006), so:
//   * generators are fully specified here (SplitMix64, PCG32 XSH-RR, Xoshiro256**);
//   * integer ranges use exact, unbiased algorithms (Lemire's multiply-shift with rejection for
//     32-bit ranges, bitmask rejection for wider ones);
//   * floats are built from raw bits (24 bits for f32, 53 bits for f64) — never
//     std::uniform_*_distribution, whose algorithms differ between standard libraries.
// Golden-sequence unit tests pin every output.
//
// Threading: generators are plain values; give each thread/job its own (use jump()/split seeds).

#include <bit>
#include <concepts>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include "helios/core/types.h"

namespace helios {

/// SplitMix64 (Steele, Lea, Flood). Tiny and fast; used to seed the others.
class SplitMix64 {
public:
    using ResultType = u64;
    constexpr explicit SplitMix64(u64 seed = 0) noexcept : m_state(seed) {}
    constexpr u64 next() noexcept {
        u64 z = (m_state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    constexpr u64 nextU64() noexcept { return next(); }
    constexpr u32 nextU32() noexcept { return static_cast<u32>(next() >> 32); }
    constexpr u64 state() const noexcept { return m_state; }

private:
    u64 m_state;
};

/// PCG32 (O'Neill) — XSH-RR output, 64-bit LCG state, selectable stream.
class Pcg32 {
public:
    using ResultType = u32;
    /// Same seeding as the reference pcg32_srandom_r(initState, streamId).
    constexpr explicit Pcg32(u64 initState = 0x853c49e6748fea9bull, u64 streamId = 0xda3e39cb94b95bdbull) noexcept {
        seed(initState, streamId);
    }
    constexpr void seed(u64 initState, u64 streamId) noexcept {
        m_state = 0;
        m_inc = (streamId << 1u) | 1u;
        next();
        m_state += initState;
        next();
    }
    constexpr u32 next() noexcept {
        const u64 old = m_state;
        m_state = old * 6364136223846793005ull + m_inc;
        const u32 xorShifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot = static_cast<u32>(old >> 59u);
        return (xorShifted >> rot) | (xorShifted << ((0u - rot) & 31u));
    }
    constexpr u32 nextU32() noexcept { return next(); }
    constexpr u64 nextU64() noexcept {
        const u64 hi = next();
        return (hi << 32) | next();
    }

private:
    u64 m_state = 0;
    u64 m_inc = 0;
};

/// Xoshiro256** (Blackman, Vigna): the engine's default general-purpose generator.
class Xoshiro256 {
public:
    using ResultType = u64;
    /// Expands `seed` into 256 bits of state with SplitMix64 (the authors' recommendation).
    constexpr explicit Xoshiro256(u64 seed = 0) noexcept { reseed(seed); }
    constexpr void reseed(u64 seed) noexcept {
        SplitMix64 sm(seed);
        for (u64& s : m_s) s = sm.next();
    }
    constexpr u64 next() noexcept {
        const u64 result = rotl(m_s[1] * 5, 7) * 9;
        const u64 t = m_s[1] << 17;
        m_s[2] ^= m_s[0];
        m_s[3] ^= m_s[1];
        m_s[1] ^= m_s[2];
        m_s[0] ^= m_s[3];
        m_s[2] ^= t;
        m_s[3] = rotl(m_s[3], 45);
        return result;
    }
    constexpr u64 nextU64() noexcept { return next(); }
    constexpr u32 nextU32() noexcept { return static_cast<u32>(next() >> 32); }
    /// Advances by 2^128 steps: yields non-overlapping streams for parallel jobs.
    constexpr void jump() noexcept {
        constexpr u64 kJump[] = {0x180ec6d33cfd0abaull, 0xd5a61266f0c9392cull, 0xa9582618e03fc9aaull,
                                 0x39abdc4529b1661cull};
        u64 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (const u64 j : kJump) {
            for (int b = 0; b < 64; ++b) {
                if (j & (1ull << b)) {
                    s0 ^= m_s[0];
                    s1 ^= m_s[1];
                    s2 ^= m_s[2];
                    s3 ^= m_s[3];
                }
                next();
            }
        }
        m_s[0] = s0;
        m_s[1] = s1;
        m_s[2] = s2;
        m_s[3] = s3;
    }

private:
    static constexpr u64 rotl(u64 x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    u64 m_s[4] = {};
};

// ---------------------------------------------------------------------------------------------
// Distributions (exact and portable). `Rng` is any generator above.
// ---------------------------------------------------------------------------------------------

/// Unbiased integer in [0, bound) for bound in [1, 2^32] (Lemire 2019, with rejection).
template <class Rng>
constexpr u32 uniformU32Below(Rng& rng, u32 bound) noexcept {
    if (bound <= 1) return 0;
    u64 m = static_cast<u64>(rng.nextU32()) * bound;
    u32 low = static_cast<u32>(m);
    if (low < bound) {
        const u32 threshold = (0u - bound) % bound;
        while (low < threshold) {
            m = static_cast<u64>(rng.nextU32()) * bound;
            low = static_cast<u32>(m);
        }
    }
    return static_cast<u32>(m >> 32);
}

/// Unbiased integer in [0, range] (inclusive) for any 64-bit range.
template <class Rng>
constexpr u64 uniformU64Inclusive(Rng& rng, u64 range) noexcept {
    if (range == 0) return 0;
    if (range < 0xFFFFFFFFull) return uniformU32Below(rng, static_cast<u32>(range + 1));
    if (range == ~0ull) return rng.nextU64();
    // Bitmask rejection: expected < 2 draws, exact on every platform.
    u64 mask = range;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    u64 x;
    do {
        x = rng.nextU64() & mask;
    } while (x > range);
    return x;
}

/// Unbiased integer in [lo, hi] (inclusive). Requires lo <= hi.
template <class Rng>
constexpr i64 uniformInt(Rng& rng, i64 lo, i64 hi) noexcept {
    const u64 range = static_cast<u64>(hi) - static_cast<u64>(lo);
    return static_cast<i64>(static_cast<u64>(lo) + uniformU64Inclusive(rng, range));
}

/// Float in [0, 1) with 24 random bits.
template <class Rng>
constexpr f32 uniformFloat01(Rng& rng) noexcept {
    return static_cast<f32>(rng.nextU32() >> 8) * (1.0f / 16777216.0f);
}

/// Double in [0, 1) with 53 random bits.
template <class Rng>
constexpr f64 uniformDouble01(Rng& rng) noexcept {
    return static_cast<f64>(rng.nextU64() >> 11) * (1.0 / 9007199254740992.0);
}

namespace detail {
/// Largest value strictly below finite `v` (bit-exact and constexpr, unlike std::nextafter).
constexpr f32 floatBelow(f32 v) noexcept {
    if (v == 0.0f) return -std::numeric_limits<f32>::denorm_min();
    const u32 bits = std::bit_cast<u32>(v);
    return std::bit_cast<f32>(v > 0.0f ? bits - 1u : bits + 1u);
}
constexpr f64 doubleBelow(f64 v) noexcept {
    if (v == 0.0) return -std::numeric_limits<f64>::denorm_min();
    const u64 bits = std::bit_cast<u64>(v);
    return std::bit_cast<f64>(v > 0.0 ? bits - 1u : bits + 1u);
}
} // namespace detail

/// Float in [lo, hi) for lo < hi (lo == hi returns lo). Plain IEEE arithmetic (no FMA contraction in
/// Helios builds), so identical on every platform. lo + (hi - lo) * u can round up to exactly hi
/// (e.g. 1 + (1 - 2^-24) rounds to 2), so that case maps to the largest value below hi.
template <class Rng>
constexpr f32 uniformFloat(Rng& rng, f32 lo, f32 hi) noexcept {
    const f32 r = lo + (hi - lo) * uniformFloat01(rng);
    return (lo < hi && !(r < hi)) ? detail::floatBelow(hi) : r;
}
template <class Rng>
constexpr f64 uniformDouble(Rng& rng, f64 lo, f64 hi) noexcept {
    const f64 r = lo + (hi - lo) * uniformDouble01(rng);
    return (lo < hi && !(r < hi)) ? detail::doubleBelow(hi) : r;
}

/// Convenience wrapper around Xoshiro256** with the common distributions.
class Random {
public:
    constexpr explicit Random(u64 seed = 0) noexcept : m_rng(seed) {}
    constexpr void reseed(u64 seed) noexcept { m_rng.reseed(seed); }

    constexpr u64 nextU64() noexcept { return m_rng.nextU64(); }
    constexpr u32 nextU32() noexcept { return m_rng.nextU32(); }
    /// Integer in [lo, hi] inclusive (lo <= hi).
    template <std::integral I>
    constexpr I range(I lo, I hi) noexcept {
        if constexpr (std::is_signed_v<I>) {
            return static_cast<I>(uniformInt(m_rng, static_cast<i64>(lo), static_cast<i64>(hi)));
        } else {
            return static_cast<I>(static_cast<u64>(lo) +
                                  uniformU64Inclusive(m_rng, static_cast<u64>(hi) - static_cast<u64>(lo)));
        }
    }
    /// Index in [0, count) (count > 0).
    constexpr u64 index(u64 count) noexcept { return uniformU64Inclusive(m_rng, count - 1); }
    constexpr f32 nextFloat() noexcept { return uniformFloat01(m_rng); }
    constexpr f64 nextDouble() noexcept { return uniformDouble01(m_rng); }
    constexpr f32 range(f32 lo, f32 hi) noexcept { return uniformFloat(m_rng, lo, hi); }
    constexpr f64 range(f64 lo, f64 hi) noexcept { return uniformDouble(m_rng, lo, hi); }
    /// True with probability p (53-bit resolution).
    constexpr bool chance(f64 p) noexcept { return nextDouble() < p; }

    /// Fisher-Yates shuffle with the deterministic integer distribution (unlike std::shuffle).
    template <class T>
    constexpr void shuffle(std::span<T> items) noexcept {
        for (usize i = items.size(); i > 1; --i) {
            const usize j = static_cast<usize>(index(i));
            using std::swap;
            swap(items[i - 1], items[j]);
        }
    }

    Xoshiro256& generator() noexcept { return m_rng; }

private:
    Xoshiro256 m_rng;
};

} // namespace helios
