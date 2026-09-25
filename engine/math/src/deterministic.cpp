// Deterministic trigonometry (see helios::det in scalar.h).
//
// Method: Cody-Waite reduction by pi/2 (a three-part constant for |n| < 2^20, a five-part constant
// with double-double accumulation for 2^20 <= |n| < 2^30), then Taylor kernels on [-pi/4, pi/4]
// (sin to degree 17, cos to degree 16; truncation error < 1e-19). atan reduces to
// |t| <= tan(pi/12) with atan(a) = pi/6 + atan((sqrt3*a - 1) / (a + sqrt3)) and a degree-29 Taylor
// kernel. Only +, -, *, /, sqrt, floor, fmod, copysign are used; all are exactly specified by
// IEEE 754, so with contraction disabled the results are bit-identical on every platform.
#include "fp_control.h"

#include "helios/math/scalar.h"

#include <cmath>

namespace helios::det {
namespace {

// pi/2 split so that n * kPio2Hi (31 significant bits) and n * kPio2Mid (32 bits) are exact for
// |n| < 2^20.
constexpr f64 kTwoOverPi = 0x1.45f306dc9c883p-1;
constexpr f64 kPio2Hi = 0x1.921fb54400000p+0;
constexpr f64 kPio2Mid = 0x1.0b4611a600000p-34;
constexpr f64 kPio2Lo = 0x1.3198a2e037073p-69;
// pi/2 = kPio2A + kPio2B + kPio2C + kPio2D + kPio2E to ~148 bits. A..D have <= 23 significant bits,
// so n * A..D are exact for |n| < 2^30 (every n the reduction sees after the 2^30 fold).
constexpr f64 kPio2A = 0x1.921fb40000000p+0;
constexpr f64 kPio2B = 0x1.4442d00000000p-24;
constexpr f64 kPio2C = 0x1.8469880000000p-48;
constexpr f64 kPio2D = 0x1.8cc5140000000p-72;
constexpr f64 kPio2E = 0x1.80dc1cd129025p-95;

// Correctly rounded constants with their residuals (value = hi + lo to ~107 bits).
constexpr f64 kPiHi = 0x1.921fb54442d18p+1;
constexpr f64 kPiLo = 0x1.1a62633145c07p-53;
constexpr f64 kHalfPiHi = 0x1.921fb54442d18p+0;
constexpr f64 kHalfPiLo = 0x1.1a62633145c07p-54;
constexpr f64 kSixthPiHi = 0x1.0c152382d7366p-1;
constexpr f64 kSixthPiLo = -0x1.ee6913347c2a6p-55;
constexpr f64 kQuarterPi = 0x1.921fb54442d18p-1;
constexpr f64 kTwoPiHi = 0x1.921fb54442d18p+2;
constexpr f64 kThreeQuarterPi = 0x1.2d97c7f3321d2p+1;
constexpr f64 kSqrt3 = 0x1.bb67ae8584caap+0;
constexpr f64 kTanPiOver12 = 0.2679491924311227;  // 2 - sqrt(3)

// Taylor coefficients: exact rationals rounded once at compile time.
constexpr f64 kS3 = -1.0 / 6.0;
constexpr f64 kS5 = 1.0 / 120.0;
constexpr f64 kS7 = -1.0 / 5040.0;
constexpr f64 kS9 = 1.0 / 362880.0;
constexpr f64 kS11 = -1.0 / 39916800.0;
constexpr f64 kS13 = 1.0 / 6227020800.0;
constexpr f64 kS15 = -1.0 / 1307674368000.0;
constexpr f64 kS17 = 1.0 / 355687428096000.0;

constexpr f64 kC4 = 1.0 / 24.0;
constexpr f64 kC6 = -1.0 / 720.0;
constexpr f64 kC8 = 1.0 / 40320.0;
constexpr f64 kC10 = -1.0 / 3628800.0;
constexpr f64 kC12 = 1.0 / 479001600.0;
constexpr f64 kC14 = -1.0 / 87178291200.0;
constexpr f64 kC16 = 1.0 / 20922789888000.0;

// sin(r) for |r| <= ~pi/4.
f64 sinKernel(f64 r) noexcept {
    const f64 z = r * r;
    const f64 p = kS3 + z * (kS5 + z * (kS7 + z * (kS9 + z * (kS11 + z * (kS13 + z * (kS15 + z * kS17))))));
    return r + (r * z) * p;
}

// cos(r) for |r| <= ~pi/4. The (1 - w) - hz term recovers the rounding error of 1 - r^2/2.
f64 cosKernel(f64 r) noexcept {
    const f64 z = r * r;
    const f64 hz = 0.5 * z;
    const f64 w = 1.0 - hz;
    const f64 p = (z * z) * (kC4 + z * (kC6 + z * (kC8 + z * (kC10 + z * (kC12 + z * (kC14 + z * kC16))))));
    return w + (((1.0 - w) - hz) + p);
}

// atan(t) = t - t^3/3 + t^5/5 - ... for |t| <= tan(pi/12) (terms up to t^29).
f64 atanKernel(f64 t) noexcept {
    const f64 z = t * t;
    f64 p = 1.0 / 29.0;
    p = -1.0 / 27.0 + z * p;
    p = 1.0 / 25.0 + z * p;
    p = -1.0 / 23.0 + z * p;
    p = 1.0 / 21.0 + z * p;
    p = -1.0 / 19.0 + z * p;
    p = 1.0 / 17.0 + z * p;
    p = -1.0 / 15.0 + z * p;
    p = 1.0 / 13.0 + z * p;
    p = -1.0 / 11.0 + z * p;
    p = 1.0 / 9.0 + z * p;
    p = -1.0 / 7.0 + z * p;
    p = 1.0 / 5.0 + z * p;
    p = -1.0 / 3.0 + z * p;
    return t + (t * z) * p;
}

struct Reduced {
    f64 r;
    int quadrant;  // x = r + quadrant * pi/2 (mod 2*pi)
};

// hi + lo -= t, keeping the rounding error of the subtraction in lo (Knuth's TwoSum; exact under
// round-to-nearest without contraction).
void subtractTwoSum(f64& hi, f64& lo, f64 t) noexcept {
    const f64 s = hi - t;
    const f64 bb = s - hi;
    const f64 err = (hi - (s - bb)) + (-t - bb);
    hi = s;
    lo += err;
}

Reduced reduce(f64 x) noexcept {
    if (std::fabs(x) <= kQuarterPi) return {x, 0};  // exact; preserves -0 and denormals
    // Beyond 2^30 even the 23-bit products below are no longer exact. Fold huge arguments with
    // fmod by the rounded 2*pi first: exact (so still deterministic) and bounded, though the phase
    // of such arguments is not meaningful anyway.
    if (std::fabs(x) >= 1073741824.0) x = std::fmod(x, kTwoPiHi);
    const f64 n = std::floor(x * kTwoOverPi + 0.5);
    f64 r;
    if (std::fabs(n) < 1048576.0) {
        r = x - n * kPio2Hi;  // exact (Sterbenz; the product is exact for |n| < 2^20)
        r -= n * kPio2Mid;
        r -= n * kPio2Lo;
    } else {
        // 2^20 <= |n| < 2^30 (|x| >= ~1.6e6): the products with the 31/32-bit pieces above round
        // from |n| >= 2^21 on (that used to cost up to ~3e11 ulp at 1e9). Use the 23-bit pieces
        // (exact products) and accumulate the remainder as a double-double, so cancellation near
        // multiples of pi/2 loses nothing.
        f64 hi = x - n * kPio2A;  // exact (Sterbenz)
        f64 lo = 0.0;
        subtractTwoSum(hi, lo, n * kPio2B);
        subtractTwoSum(hi, lo, n * kPio2C);
        subtractTwoSum(hi, lo, n * kPio2D);
        subtractTwoSum(hi, lo, n * kPio2E);
        r = hi + lo;
    }
    f64 q = std::fmod(n, 4.0);  // exact; avoids an out-of-range cast for huge n
    if (q < 0.0) q += 4.0;
    return {r, static_cast<int>(q)};
}

// atan for a >= 0 (finite or +inf).
f64 atanPositive(f64 a) noexcept {
    bool inverted = false;
    if (a > 1.0) {
        a = 1.0 / a;
        inverted = true;
    }
    f64 res;
    if (a > kTanPiOver12) {
        const f64 t = (a * kSqrt3 - 1.0) / (a + kSqrt3);
        res = kSixthPiHi + (atanKernel(t) + kSixthPiLo);
    } else {
        res = atanKernel(a);
    }
    if (inverted) res = (kHalfPiHi - res) + kHalfPiLo;
    return res;
}

}  // namespace

f64 sin(f64 x) noexcept {
    if (!std::isfinite(x)) return x - x;  // NaN for +-inf and NaN
    if (x == 0.0) return x;               // keeps the sign of zero
    const Reduced red = reduce(x);
    switch (red.quadrant) {
        case 0: return sinKernel(red.r);
        case 1: return cosKernel(red.r);
        case 2: return -sinKernel(red.r);
        default: return -cosKernel(red.r);
    }
}

f64 cos(f64 x) noexcept {
    if (!std::isfinite(x)) return x - x;
    const Reduced red = reduce(x);
    switch (red.quadrant) {
        case 0: return cosKernel(red.r);
        case 1: return -sinKernel(red.r);
        case 2: return -cosKernel(red.r);
        default: return sinKernel(red.r);
    }
}

void sinCos(f64 x, f64& outSin, f64& outCos) noexcept {
    if (!std::isfinite(x)) {
        outSin = outCos = x - x;
        return;
    }
    if (x == 0.0) {
        outSin = x;
        outCos = 1.0;
        return;
    }
    const Reduced red = reduce(x);
    const f64 s = sinKernel(red.r);
    const f64 c = cosKernel(red.r);
    switch (red.quadrant) {
        case 0:
            outSin = s;
            outCos = c;
            break;
        case 1:
            outSin = c;
            outCos = -s;
            break;
        case 2:
            outSin = -s;
            outCos = -c;
            break;
        default:
            outSin = -c;
            outCos = s;
            break;
    }
}

f64 atan(f64 x) noexcept {
    if (std::isnan(x)) return x;
    const f64 res = atanPositive(std::fabs(x));
    return std::signbit(x) ? -res : res;
}

f64 atan2(f64 y, f64 x) noexcept {
    if (std::isnan(x) || std::isnan(y)) return x + y;
    // Special cases follow C99 Annex F so callers can rely on std::atan2 semantics.
    if (y == 0.0) return std::signbit(x) ? std::copysign(kPiHi, y) : y;
    if (x == 0.0) return std::copysign(kHalfPiHi, y);
    if (std::isinf(x)) {
        if (std::isinf(y)) return std::copysign(x > 0.0 ? kQuarterPi : kThreeQuarterPi, y);
        return x > 0.0 ? std::copysign(0.0, y) : std::copysign(kPiHi, y);
    }
    if (std::isinf(y)) return std::copysign(kHalfPiHi, y);

    const f64 ax = std::fabs(x);
    const f64 ay = std::fabs(y);
    f64 a;
    if (ay > ax) {
        a = (kHalfPiHi - atanPositive(ax / ay)) + kHalfPiLo;
    } else {
        a = atanPositive(ay / ax);
    }
    if (x < 0.0) a = (kPiHi - a) + kPiLo;
    return std::copysign(a, y);
}

f64 asin(f64 x) noexcept { return atan2(x, std::sqrt((1.0 - x) * (1.0 + x))); }

f64 acos(f64 x) noexcept { return atan2(std::sqrt((1.0 - x) * (1.0 + x)), x); }

}  // namespace helios::det
