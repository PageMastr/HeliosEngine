// Out-of-line parts of helios/math/fixed.h: 128-bit division, 128-bit integer square root, the
// integer reciprocal square root and rounded fixed-point division. Integer-only, so the results are
// identical on every compiler and CPU.
#include "helios/math/fixed.h"

#include <bit>

namespace helios {

namespace {

// (u1:u0) / v for u1 < v (quotient fits in 64 bits). Hacker's Delight `divlu`, 64-bit version:
// normalize v, then two 32-bit quotient digits, each corrected at most twice (Knuth D).
u64 divlu(u64 u1, u64 u0, u64 v, u64* rem) noexcept {
    constexpr u64 kBase = u64(1) << 32;
    const int s = std::countl_zero(v);
    v <<= s;
    const u64 vn1 = v >> 32, vn0 = v & 0xffffffffu;
    const u64 un32 = s == 0 ? u1 : (u1 << s) | (u0 >> (64 - s));
    const u64 un10 = u0 << s;
    const u64 un1 = un10 >> 32, un0 = un10 & 0xffffffffu;
    u64 q1 = un32 / vn1;
    u64 rhat = un32 - q1 * vn1;
    while (q1 >= kBase || q1 * vn0 > kBase * rhat + un1) {
        --q1;
        rhat += vn1;
        if (rhat >= kBase) break;
    }
    const u64 un21 = un32 * kBase + un1 - q1 * v; // modulo 2^64 by design
    u64 q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= kBase || q0 * vn0 > kBase * rhat + un0) {
        --q0;
        rhat += vn1;
        if (rhat >= kBase) break;
    }
    if (rem) *rem = (un21 * kBase + un0 - q0 * v) >> s;
    return q1 * kBase + q0;
}

} // namespace

U128 divU128(U128 n, u64 d, u64* remainder) noexcept {
    HELIOS_MATH_ASSERT(d != 0);
    if (d == 0) {
        if (remainder) *remainder = 0;
        return {~u64(0), ~u64(0)};
    }
    const u64 qhi = n.hi / d;
    const u64 r = n.hi % d;
    u64 rem = 0;
    const u64 qlo = divlu(r, n.lo, d, &rem);
    if (remainder) *remainder = rem;
    return {qhi, qlo};
}

u64 isqrt(U128 x) noexcept {
    if (x.hi == 0) return isqrt(x.lo);
    // Digit-by-digit (base 4) square root over 128 bits.
    U128 op = x, res{};
    U128 one{u64(1) << 62, 0}; // 2^126
    while (one > op) one = shr(one, 2);
    while (one != U128{}) {
        const U128 t = add(res, one);
        if (op >= t) {
            op = sub(op, t);
            res = add(shr(res, 1), one);
        } else {
            res = shr(res, 1);
        }
        one = shr(one, 2);
    }
    return res.lo;
}

u64 rsqrtFixed(u64 x, u32 fracIn, u32 fracOut) noexcept {
    const u32 e = fracIn + 2 * fracOut;
    if (x == 0 || e > 127 || fracOut > 63) return ~u64(0);
    const U128 numerator = shl(U128{0, 1}, e);
    const U128 q = divU128(numerator, x);
    return q.hi == 0 ? u64(isqrt(q.lo)) : isqrt(q);
}

namespace detail {

i64 fixedDiv64(i64 a, i64 b, u32 fracBits) noexcept {
    constexpr i64 kMax = std::numeric_limits<i64>::max();
    constexpr i64 kMin = std::numeric_limits<i64>::min();
    if (b == 0) return a > 0 ? kMax : (a < 0 ? kMin : 0);
    const bool negative = (a < 0) != (b < 0);
    const u64 ua = a < 0 ? u64(0) - static_cast<u64>(a) : static_cast<u64>(a);
    const u64 ub = b < 0 ? u64(0) - static_cast<u64>(b) : static_cast<u64>(b);
    u64 rem = 0;
    U128 q = divU128(shl(U128{0, ua}, fracBits), ub, &rem);
    if (rem >= ub - rem) q = add(q, U128{0, 1}); // round half away from zero (2 rem >= ub, no overflow)
    const u64 limit = negative ? (u64(1) << 63) : static_cast<u64>(kMax);
    if (q.hi != 0 || q.lo > limit) return negative ? kMin : kMax;
    return negative ? static_cast<i64>(u64(0) - q.lo) : static_cast<i64>(q.lo);
}

} // namespace detail

} // namespace helios
