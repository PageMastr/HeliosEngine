// helios/math/fixed.h — fixed-point numbers and exact integer roots (02 §2.1, §5.8; 04 §5.4).
//
// Integer arithmetic is exact and identical on every compiler and CPU, which makes fixed point the
// representation for everything two machines (or a CPU and a GPU) must compute bit-identically:
//   * Q16    — Q16.16 in an i32: hnoise fractions, 32-bit-only GPU twin (02 §5.8, 03 §5.5);
//   * Q32    — Q32.32 in an i64: PCG heights in metres (exact in f64 for |h| < 2^20 m);
//   * Fixed64 — i64 in units of 2^-10 m (~0.98 mm): the command-replication integrator (04 §5.4),
//     exact in f64 up to 2^43 units = 8.8e12 m.
// Semantics (all formats):
//   * + and - wrap on overflow (two's complement; never UB).
//   * * rounds to nearest with ties toward +infinity (add half an ulp, arithmetic shift) and wraps.
//   * / rounds to nearest with ties away from zero; it saturates when the quotient does not fit and
//     for division by zero (0/0 = 0).
//   * fromDouble rounds half away from zero and saturates; NaN becomes 0. toDouble is exact while
//     |raw| < 2^53.
//   * sqrt and rsqrt are exact floors of the real results (see rsqrtFixed).
// 128-bit intermediates use U128 (portable; no __int128).
//
// Threading: plain values and pure functions; thread-safe.
#pragma once

#include <bit>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "helios/math/scalar.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) away from Fixed::min()/max().
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

// ---------------------------------------------------------------------------------------------
// 128-bit helpers and exact integer roots
// ---------------------------------------------------------------------------------------------

/// Unsigned 128-bit integer (two's complement when used for signed products).
struct U128 {
    u64 hi = 0;
    u64 lo = 0;
    friend constexpr bool operator==(U128, U128) = default;
    friend constexpr std::strong_ordering operator<=>(U128 a, U128 b) noexcept {
        if (a.hi != b.hi) return a.hi <=> b.hi;
        return a.lo <=> b.lo;
    }
};

[[nodiscard]] constexpr U128 add(U128 a, U128 b) noexcept {
    const u64 lo = a.lo + b.lo;
    return {a.hi + b.hi + (lo < a.lo ? 1u : 0u), lo};
}
[[nodiscard]] constexpr U128 sub(U128 a, U128 b) noexcept {
    return {a.hi - b.hi - (a.lo < b.lo ? 1u : 0u), a.lo - b.lo};
}
[[nodiscard]] constexpr U128 shl(U128 a, u32 s) noexcept {
    if (s == 0) return a;
    if (s >= 128) return {};
    if (s >= 64) return {a.lo << (s - 64), 0};
    return {(a.hi << s) | (a.lo >> (64 - s)), a.lo << s};
}
[[nodiscard]] constexpr U128 shr(U128 a, u32 s) noexcept {
    if (s == 0) return a;
    if (s >= 128) return {};
    if (s >= 64) return {0, a.hi >> (s - 64)};
    return {a.hi >> s, (a.lo >> s) | (a.hi << (64 - s))};
}

/// Full 64 x 64 -> 128-bit unsigned product.
[[nodiscard]] constexpr U128 mulU64(u64 a, u64 b) noexcept {
    constexpr u64 kMask = 0xffffffffu;
    const u64 a0 = a & kMask, a1 = a >> 32, b0 = b & kMask, b1 = b >> 32;
    const u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const u64 mid = (p00 >> 32) + (p01 & kMask) + (p10 & kMask);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & kMask)};
}

/// Full 64 x 64 -> 128-bit signed product, as a two's-complement U128.
[[nodiscard]] constexpr U128 mulI64(i64 a, i64 b) noexcept {
    const u64 ua = static_cast<u64>(a), ub = static_cast<u64>(b);
    U128 p = mulU64(ua, ub);
    if (a < 0) p.hi -= ub;
    if (b < 0) p.hi -= ua;
    return p;
}

/// floor(sqrt(x)), exact. At run time an IEEE sqrt estimate (correctly rounded, so identical on
/// every platform) is corrected to the exact floor; constant evaluation uses the digit loop.
[[nodiscard]] constexpr u32 isqrt(u64 x) noexcept {
    if (!std::is_constant_evaluated()) {
        u64 s = static_cast<u64>(std::sqrt(static_cast<f64>(x)));
        if (s > 0xffffffffu) s = 0xffffffffu;
        while (s * s > x) --s;
        while (s < 0xffffffffu && (s + 1) * (s + 1) <= x) ++s;
        return static_cast<u32>(s);
    }
    u64 op = x, res = 0, one = u64(1) << 62;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return static_cast<u32>(res);
}

/// floor(sqrt(x)) of a 128-bit value, exact.
[[nodiscard]] u64 isqrt(U128 x) noexcept;

/// 128 / 64 division: quotient (128-bit) and remainder. Precondition: d != 0.
[[nodiscard]] U128 divU128(U128 n, u64 d, u64* remainder = nullptr) noexcept;

/// Integer reciprocal square root on unsigned fixed point (02 §5.8's `rsqrt`): with x holding
/// fracIn fraction bits, returns floor(2^fracOut / sqrt(x / 2^fracIn)) = floor(sqrt(2^(fracIn +
/// 2 fracOut) / x)), exactly. Requires fracIn + 2 fracOut <= 127. Returns UINT64_MAX for x == 0
/// or an invalid precision. Deterministic everywhere (integer only). Budget: <= 60 ns when the
/// intermediate quotient fits in 64 bits (the PCG and Q16/Q32 cases), <= 300 ns otherwise.
[[nodiscard]] u64 rsqrtFixed(u64 x, u32 fracIn, u32 fracOut) noexcept;

/// The PCG domain's normalization (02 §5.8): x = |p|^2 in Q.60 (e.g. u^2 + v^2 + 1 for Q2.30
/// face coordinates, in [1, 3]) -> 1/|p| in Q2.30.
[[nodiscard]] inline u32 rsqrtQ30(u64 xQ60) noexcept {
    const u64 r = rsqrtFixed(xQ60, 60, 30);
    return r > 0xffffffffu ? 0xffffffffu : static_cast<u32>(r);
}

// ---------------------------------------------------------------------------------------------
// Fixed<Raw, FracBits>
// ---------------------------------------------------------------------------------------------

namespace detail {
// Signed 64-bit fixed-point division with rounding (ties away from zero) and saturation.
[[nodiscard]] i64 fixedDiv64(i64 a, i64 b, u32 fracBits) noexcept;
} // namespace detail

template <std::signed_integral Raw, int FracBits>
    requires(sizeof(Raw) == 4 || sizeof(Raw) == 8) && (FracBits > 0) && (FracBits < int(sizeof(Raw) * 8) - 1)
class Fixed {
public:
    using RawType = Raw;
    static constexpr int kFracBits = FracBits;
    static constexpr Raw kOneRaw = Raw(1) << FracBits;

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed fromRaw(Raw raw) noexcept {
        Fixed f;
        f.m_raw = raw;
        return f;
    }
    /// Wraps if i does not fit.
    [[nodiscard]] static constexpr Fixed fromInt(i64 i) noexcept {
        return fromRaw(static_cast<Raw>(static_cast<u64>(i) << FracBits));
    }
    /// Rounds half away from zero, saturates, NaN -> 0.
    [[nodiscard]] static Fixed fromDouble(f64 v) noexcept {
        if (std::isnan(v)) return Fixed();
        const f64 scaled = std::round(v * kScale);
        constexpr f64 kMaxD = static_cast<f64>(std::numeric_limits<Raw>::max());
        constexpr f64 kMinD = static_cast<f64>(std::numeric_limits<Raw>::min());
        if (scaled >= kMaxD) return fromRaw(std::numeric_limits<Raw>::max());
        if (scaled <= kMinD) return fromRaw(std::numeric_limits<Raw>::min());
        return fromRaw(static_cast<Raw>(scaled));
    }
    [[nodiscard]] static Fixed fromFloat(f32 v) noexcept { return fromDouble(static_cast<f64>(v)); }

    [[nodiscard]] static constexpr Fixed one() noexcept { return fromRaw(kOneRaw); }
    [[nodiscard]] static constexpr Fixed half() noexcept { return fromRaw(kOneRaw / 2); }
    [[nodiscard]] static constexpr Fixed epsilon() noexcept { return fromRaw(1); }
    [[nodiscard]] static constexpr Fixed max() noexcept { return fromRaw(std::numeric_limits<Raw>::max()); }
    [[nodiscard]] static constexpr Fixed min() noexcept { return fromRaw(std::numeric_limits<Raw>::min()); }

    [[nodiscard]] constexpr Raw raw() const noexcept { return m_raw; }
    /// Exact while |raw| < 2^53.
    [[nodiscard]] constexpr f64 toDouble() const noexcept { return static_cast<f64>(m_raw) / kScale; }
    [[nodiscard]] constexpr f32 toFloat() const noexcept { return static_cast<f32>(toDouble()); }
    /// floor(value) as an integer.
    [[nodiscard]] constexpr i64 floorToInt() const noexcept { return static_cast<i64>(m_raw) >> FracBits; }
    /// Round half toward +infinity.
    [[nodiscard]] constexpr i64 roundToInt() const noexcept {
        // floor + the first fraction bit: no intermediate that could overflow near max().
        return (static_cast<i64>(m_raw) >> FracBits) + ((static_cast<i64>(m_raw) >> (FracBits - 1)) & 1);
    }
    [[nodiscard]] constexpr Fixed floor() const noexcept { return fromRaw(static_cast<Raw>(m_raw & ~(kOneRaw - 1))); }
    /// value - floor(value), in [0, 1).
    [[nodiscard]] constexpr Fixed frac() const noexcept { return fromRaw(static_cast<Raw>(m_raw & (kOneRaw - 1))); }

    friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
        return fromRaw(static_cast<Raw>(static_cast<U>(a.m_raw) + static_cast<U>(b.m_raw)));
    }
    friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
        return fromRaw(static_cast<Raw>(static_cast<U>(a.m_raw) - static_cast<U>(b.m_raw)));
    }
    friend constexpr Fixed operator-(Fixed a) noexcept { return fromRaw(static_cast<Raw>(U(0) - static_cast<U>(a.m_raw))); }
    friend constexpr Fixed operator*(Fixed a, Fixed b) noexcept {
        if constexpr (sizeof(Raw) == 4) {
            const i64 p = static_cast<i64>(a.m_raw) * static_cast<i64>(b.m_raw) + (i64(1) << (FracBits - 1));
            return fromRaw(static_cast<Raw>(p >> FracBits));
        } else {
            const U128 p = add(mulI64(a.m_raw, b.m_raw), U128{0, u64(1) << (FracBits - 1)});
            return fromRaw(static_cast<Raw>((p.lo >> FracBits) | (p.hi << (64 - FracBits))));
        }
    }
    friend Fixed operator/(Fixed a, Fixed b) noexcept {
        if constexpr (sizeof(Raw) == 4) {
            const i64 q = detail::fixedDiv64(static_cast<i64>(a.m_raw) * kOneRaw, b.m_raw, 0);
            if (q > std::numeric_limits<Raw>::max()) return max();
            if (q < std::numeric_limits<Raw>::min()) return min();
            return fromRaw(static_cast<Raw>(q));
        } else {
            return fromRaw(detail::fixedDiv64(a.m_raw, b.m_raw, FracBits));
        }
    }
    constexpr Fixed& operator+=(Fixed o) noexcept { return *this = *this + o; }
    constexpr Fixed& operator-=(Fixed o) noexcept { return *this = *this - o; }
    constexpr Fixed& operator*=(Fixed o) noexcept { return *this = *this * o; }
    Fixed& operator/=(Fixed o) noexcept { return *this = *this / o; }

    friend constexpr bool operator==(Fixed, Fixed) = default;
    friend constexpr auto operator<=>(Fixed a, Fixed b) noexcept { return a.m_raw <=> b.m_raw; }

private:
    using U = std::make_unsigned_t<Raw>;
    static constexpr f64 kScale = static_cast<f64>(u64(1) << FracBits);
    Raw m_raw = 0;
};

/// Q16.16 (i32).
using Q16 = Fixed<i32, 16>;
/// Q32.32 (i64).
using Q32 = Fixed<i64, 32>;
/// 64-bit distance in units of 2^-10 m (04 §5.4's command integrator).
using Fixed64 = Fixed<i64, 10>;

static_assert(sizeof(Q16) == 4 && sizeof(Q32) == 8 && sizeof(Fixed64) == 8);
static_assert(std::is_trivially_copyable_v<Q16> && std::is_trivially_copyable_v<Fixed64>);

/// Converts between formats: rounds half toward +infinity when dropping fraction bits, wraps when
/// the integer part does not fit.
template <class To, class From>
[[nodiscard]] constexpr To fixedCast(From v) noexcept {
    constexpr int shift = To::kFracBits - From::kFracBits;
    const i64 raw = static_cast<i64>(v.raw());
    if constexpr (shift >= 0) {
        return To::fromRaw(static_cast<typename To::RawType>(static_cast<u64>(raw) << shift));
    } else {
        const i64 rounded = (raw >> (-shift)) + ((raw >> (-shift - 1)) & 1); // no overflow near the limits
        return To::fromRaw(static_cast<typename To::RawType>(rounded));
    }
}

template <class Raw, int F>
[[nodiscard]] constexpr Fixed<Raw, F> abs(Fixed<Raw, F> v) noexcept {
    return v.raw() < 0 ? -v : v;
}

/// floor(sqrt(v)) in the same format (exact); negative values give 0.
template <class Raw, int F>
[[nodiscard]] Fixed<Raw, F> sqrt(Fixed<Raw, F> v) noexcept {
    if (v.raw() <= 0) return {};
    const U128 scaled = shl(U128{0, static_cast<u64>(v.raw())}, static_cast<u32>(F));
    return Fixed<Raw, F>::fromRaw(static_cast<Raw>(scaled.hi == 0 ? u64(isqrt(scaled.lo)) : isqrt(scaled)));
}

/// floor(1 / sqrt(v)) in the same format (exact, integer-only); v <= 0 gives max().
template <class Raw, int F>
[[nodiscard]] Fixed<Raw, F> rsqrt(Fixed<Raw, F> v) noexcept {
    if (v.raw() <= 0) return Fixed<Raw, F>::max();
    const u64 r = rsqrtFixed(static_cast<u64>(v.raw()), static_cast<u32>(F), static_cast<u32>(F));
    if (r > static_cast<u64>(std::numeric_limits<Raw>::max())) return Fixed<Raw, F>::max();
    return Fixed<Raw, F>::fromRaw(static_cast<Raw>(r));
}

} // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
