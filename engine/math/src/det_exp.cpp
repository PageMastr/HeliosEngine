// Deterministic exp, ln, pow and asinh (helios::det, 06's `hmath`; see scalar.h).
//
// Method. Everything is built from +, -, *, / and sqrt (exactly rounded IEEE operations) plus exact
// bit manipulation, with FP contraction disabled (fp_control.h), so every compiler produces the
// same bits. Accuracy comes from double-double ("dd", an unevaluated sum hi + lo) intermediates
// formed with error-free transformations (Knuth's TwoSum, Dekker's TwoProduct with Veltkamp
// splitting, which needs no FMA):
//   ln(x):   x = m * 2^e with m in [sqrt(1/2), sqrt(2)); ln m = 2 atanh(u), u = (m-1)/(m+1) formed
//            as a dd; the first two series terms in dd, the rest (to u^29) in double; e * ln 2 with
//            ln 2 split three ways (a 40-bit head, so e * head is exact).
//   exp(x):  Cody-Waite reduction x = k ln2 + r with the same split, |r| <= ln2/2, r kept as a dd;
//            exp(r) = 1 + r + r^2/2 (dd) + r^3 P(r) (double, to r^15/15!); scaled by 2^k exactly
//            (two steps near overflow and in the subnormal range).
//   pow:     C99 Annex F special cases, then exp(y * ln|x|) with the product formed in dd, so the
//            error does not grow with |y * ln x|.
//   asinh:   ln(|x| + sqrt(x^2 + 1)) with the sum and the square root in dd (no cancellation for
//            small x), ln|x| + ln 2 for |x| > 2^28, x itself for |x| < 2^-28.
// Accuracy is measured against 64-bit-mantissa long double references in tests/test_det_exp.cpp
// (documented in README.md); bit-exactness is pinned by golden hashes in the same file.
#include "fp_control.h"

#include "helios/math/scalar.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace helios::det {
namespace {

// ---- double-double helpers (exact under round-to-nearest without contraction) ---------------
struct DD {
    f64 hi;
    f64 lo;
};

// s + e == a + b exactly.
inline DD twoSum(f64 a, f64 b) noexcept {
    const f64 s = a + b;
    const f64 bb = s - a;
    const f64 e = (a - (s - bb)) + (b - bb);
    return {s, e};
}

// Requires |a| >= |b| (or a == 0).
inline DD fastTwoSum(f64 a, f64 b) noexcept {
    const f64 s = a + b;
    return {s, b - (s - a)};
}

// Veltkamp split: a == hi + lo with both halves of <= 26 significant bits. |a| < 2^996.
inline DD split(f64 a) noexcept {
    const f64 c = 0x1.0000002p+27 * a; // 2^27 + 1
    const f64 hi = c - (c - a);
    return {hi, a - hi};
}

// p + e == a * b exactly (Dekker). |a|, |b| < 2^996 and no underflow in the partial products.
inline DD twoProd(f64 a, f64 b) noexcept {
    const f64 p = a * b;
    const DD as = split(a);
    const DD bs = split(b);
    const f64 e = ((as.hi * bs.hi - p) + as.hi * bs.lo + as.lo * bs.hi) + as.lo * bs.lo;
    return {p, e};
}

inline DD ddMul(DD a, DD b) noexcept {
    DD p = twoProd(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return fastTwoSum(p.hi, p.lo);
}

// ---- constants (hex literals: exact, identical on every compiler) ---------------------------
constexpr f64 kLn2Head = 0x1.62e42fefa4000p-1;  // ln 2 to 40 bits: e * head is exact for |e| < 2^13
constexpr f64 kLn2Mid = -0x1.8432a1b0e2634p-43;
constexpr f64 kLn2Tail = 0x1.f97b57a079a19p-103;
constexpr f64 kLn2Hi = 0x1.62e42fefa39efp-1;    // correctly rounded ln 2
constexpr f64 kLn2Lo = 0x1.abc9e3b39803fp-56;
constexpr f64 kInvLn2 = 0x1.71547652b82fep+0;
constexpr f64 kTwoThirdsHi = 0x1.5555555555555p-1;
constexpr f64 kTwoThirdsLo = 0x1.5555555555555p-55;
constexpr f64 kSqrt2 = 0x1.6a09e667f3bcdp+0;
constexpr f64 kExpOverflow = 0x1.62e42fefa39efp+9;   // 709.782712893384: exp beyond it is > DBL_MAX
constexpr f64 kExpUnderflow = -0x1.74910d52d3051p+9; // -745.1332191019411: exp below it rounds to 0
constexpr f64 kTwo28 = 0x1p+28;
constexpr f64 kTwoMinus28 = 0x1p-28;
constexpr f64 kTwo64 = 0x1p+64;

// 2/(2k+1) for k = 2..14: ln m = 2u + 2u^3/3 + u^5 * (c5 + u^2 (c7 + ...)).
constexpr f64 kAtanhC[] = {
    0x1.999999999999ap-2, 0x1.2492492492492p-2, 0x1.c71c71c71c71cp-3, 0x1.745d1745d1746p-3,
    0x1.3b13b13b13b14p-3, 0x1.1111111111111p-3, 0x1.e1e1e1e1e1e1ep-4, 0x1.af286bca1af28p-4,
    0x1.8618618618618p-4, 0x1.642c8590b2164p-4, 0x1.47ae147ae147bp-4, 0x1.2f684bda12f68p-4,
    0x1.1a7b9611a7b96p-4,
};
// 1/n! for n = 3..15: exp(r) - 1 - r - r^2/2 = r^3 * (c3 + r (c4 + ...)).
constexpr f64 kExpC[] = {
    0x1.5555555555555p-3,  0x1.5555555555555p-5,  0x1.1111111111111p-7,  0x1.6c16c16c16c17p-10,
    0x1.a01a01a01a01ap-13, 0x1.a01a01a01a01ap-16, 0x1.71de3a556c734p-19, 0x1.27e4fb7789f5cp-22,
    0x1.ae64567f544e4p-26, 0x1.1eed8eff8d898p-29, 0x1.6124613a86d09p-33, 0x1.93974a8c07c9dp-37,
    0x1.ae7f3e733b81fp-41,
};

inline f64 pow2i(int k) noexcept { // 2^k for -1022 <= k <= 1023, exact
    return std::bit_cast<f64>(static_cast<u64>(k + 1023) << 52);
}

// ln x as a dd for finite x > 0.
DD logDD(f64 x) noexcept {
    u64 bits = std::bit_cast<u64>(x);
    int e = 0;
    if ((bits >> 52) == 0) { // subnormal: scale into the normal range (exact)
        x *= 0x1p+54;
        bits = std::bit_cast<u64>(x);
        e = -54;
    }
    e += static_cast<int>(bits >> 52) - 1023;
    f64 m = std::bit_cast<f64>((bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL); // [1, 2)
    if (m > kSqrt2) {
        m *= 0.5;
        ++e;
    }
    const f64 f = m - 1.0; // exact (Sterbenz)
    // u = f / (2 + f) as a dd.
    const DD den = twoSum(2.0, f);
    const f64 uh = f / den.hi;
    const DD p = twoProd(uh, den.hi);
    const f64 ul = (((f - p.hi) - p.lo) - uh * den.lo) / den.hi;
    const DD u{uh, ul};
    // 2u^3/3 in dd, the rest of the series in double.
    DD u2 = twoProd(uh, uh);
    u2.lo += 2.0 * uh * ul;
    const DD u3 = ddMul(u2, u);
    const DD t3 = ddMul(u3, DD{kTwoThirdsHi, kTwoThirdsLo});
    const f64 z = u2.hi;
    f64 poly = kAtanhC[12];
    for (int i = 11; i >= 0; --i) poly = kAtanhC[i] + z * poly;
    const f64 tail = (u3.hi * z) * poly;
    DD s = twoSum(2.0 * uh, t3.hi);
    s.lo += 2.0 * ul + t3.lo + tail;
    s = fastTwoSum(s.hi, s.lo);
    if (e == 0) return s;
    // + e * ln 2 (e * head exact; e * mid as an exact product).
    const f64 ef = static_cast<f64>(e);
    DD q = twoProd(ef, kLn2Mid);
    q.lo += ef * kLn2Tail;
    DD r = twoSum(ef * kLn2Head, s.hi);
    r.lo += s.lo + (q.hi + q.lo);
    return fastTwoSum(r.hi, r.lo);
}

// exp(x.hi + x.lo) for |x.lo| <= ulp(x.hi); x.hi finite.
f64 expDD(DD x) noexcept {
    if (x.hi > kExpOverflow) return HUGE_VAL;
    if (x.hi < kExpUnderflow) return 0.0;
    const f64 kf = std::floor(x.hi * kInvLn2 + 0.5);
    const int k = static_cast<int>(kf);
    // r = x - k ln 2 as a dd.
    const f64 rh0 = x.hi - kf * kLn2Head; // exact: k * head is exact and cancels (Sterbenz)
    const DD km = twoProd(kf, kLn2Mid);
    DD r = twoSum(rh0, -km.hi);
    r.lo += (x.lo - km.lo) - kf * kLn2Tail;
    r = fastTwoSum(r.hi, r.lo);
    // exp(r) = 1 + r + r^2/2 + r^3 P(r).
    const f64 rh = r.hi;
    DD r2 = twoProd(rh, rh);
    r2.lo += 2.0 * rh * r.lo;
    f64 poly = kExpC[12];
    for (int i = 11; i >= 0; --i) poly = kExpC[i] + rh * poly;
    const f64 tail = (r2.hi * rh) * poly;
    DD s = twoSum(rh, 0.5 * r2.hi);
    s.lo += r.lo + 0.5 * r2.lo + tail;
    DD y = twoSum(1.0, s.hi);
    y.lo += s.lo;
    const f64 v = y.hi + y.lo; // in [~0.7, ~1.42]
    if (k > 1023) return (v * pow2i(k - 1)) * 2.0;
    if (k < -1021) return (v * pow2i(k + 1000)) * 0x1p-1000; // one rounding into the subnormals
    return v * pow2i(k);
}

bool isInteger(f64 y) noexcept { return std::floor(y) == y; }

// For an integer y: true if odd. Integers with |y| >= 2^53 are even.
bool isOddInteger(f64 y) noexcept {
    if (std::fabs(y) >= 0x1p+53) return false;
    return std::fmod(y, 2.0) != 0.0;
}

} // namespace

f64 exp(f64 x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x == HUGE_VAL) return x;
    if (x == -HUGE_VAL) return 0.0;
    return expDD(DD{x, 0.0});
}

f64 ln(f64 x) noexcept {
    if (std::isnan(x)) return x + x;
    if (x < 0.0) return std::numeric_limits<f64>::quiet_NaN();
    if (x == 0.0) return -HUGE_VAL;
    if (x == HUGE_VAL) return x;
    const DD r = logDD(x);
    return r.hi + r.lo;
}

f64 pow(f64 x, f64 y) noexcept {
    // C99 Annex F order (std::pow semantics).
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (std::isnan(x) || std::isnan(y)) return x + y;
    const bool yInt = std::isfinite(y) && isInteger(y);
    const bool yOdd = yInt && isOddInteger(y);
    if (x == 0.0) {
        if (y < 0.0) return yOdd ? std::copysign(HUGE_VAL, x) : HUGE_VAL;
        return yOdd ? x : 0.0;
    }
    if (std::isinf(y)) {
        const f64 ax = std::fabs(x);
        if (ax == 1.0) return 1.0; // pow(-1, +-inf)
        return ((ax < 1.0) == (y < 0.0)) ? HUGE_VAL : 0.0;
    }
    if (std::isinf(x)) {
        if (x > 0.0) return y < 0.0 ? 0.0 : HUGE_VAL;
        if (y < 0.0) return yOdd ? -0.0 : 0.0;
        return yOdd ? -HUGE_VAL : HUGE_VAL;
    }
    f64 sign = 1.0;
    if (x < 0.0) {
        if (!yInt) return std::numeric_limits<f64>::quiet_NaN();
        if (yOdd) sign = -1.0;
        x = -x;
    }
    // Exactly rounded shortcuts (what callers expect from pow(x, 1) and friends).
    if (y == 1.0) return sign * x;
    if (y == 2.0) return x * x;
    if (y == -1.0) return sign / x;
    if (y == 0.5) return std::sqrt(x); // x > 0 here; +-0 and -inf were handled above
    const DD l = logDD(x);
    if (std::fabs(y) > kTwo64) {
        // |y ln x| > 2^64 * 2^-53 > 745: certain overflow or underflow (x != 1).
        return sign * (((l.hi > 0.0) == (y > 0.0)) ? HUGE_VAL : 0.0);
    }
    DD w = twoProd(y, l.hi);
    w.lo += y * l.lo;
    w = fastTwoSum(w.hi, w.lo);
    return sign * expDD(w);
}

f64 asinh(f64 x) noexcept {
    if (!std::isfinite(x) || x == 0.0) return x; // NaN, +-inf and +-0 map to themselves
    const f64 a = std::fabs(x);
    f64 r;
    if (a < kTwoMinus28) {
        return x; // asinh(x) = x - x^3/6 + ..., and x^2/6 < 2^-58
    } else if (a > kTwo28) {
        const DD l = logDD(a);
        DD s = twoSum(l.hi, kLn2Hi);
        s.lo += l.lo + kLn2Lo;
        r = s.hi + s.lo;
    } else {
        // t = a + sqrt(a^2 + 1), all in dd.
        const DD a2 = twoProd(a, a);
        DD q = twoSum(1.0, a2.hi);
        q.lo += a2.lo;
        q = fastTwoSum(q.hi, q.lo);
        const f64 sq = std::sqrt(q.hi);
        const DD sq2 = twoProd(sq, sq);
        const f64 corr = (((q.hi - sq2.hi) - sq2.lo) + q.lo) / (2.0 * sq);
        DD t = twoSum(a, sq);
        t.lo += corr;
        t = fastTwoSum(t.hi, t.lo);
        const DD l = logDD(t.hi);
        r = l.hi + (l.lo + t.lo / t.hi);
    }
    return std::copysign(r, x);
}

} // namespace helios::det
