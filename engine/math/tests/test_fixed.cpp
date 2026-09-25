// Fixed point (Q16, Q32, Fixed64), 128-bit helpers and the integer square roots.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "helios/math/fixed.h"

using namespace helios;

namespace {

struct Rng {
    u64 s;
    u64 next() {
        u64 z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};

// Independent reference: schoolbook multiplication with 16-bit limbs.
U128 refMul(u64 a, u64 b) {
    u32 r[8] = {};
    for (int i = 0; i < 4; ++i) {
        u64 carry = 0;
        const u64 ai = (a >> (16 * i)) & 0xffff;
        for (int j = 0; j < 4; ++j) {
            const u64 bj = (b >> (16 * j)) & 0xffff;
            const u64 t = r[i + j] + ai * bj + carry;
            r[i + j] = static_cast<u32>(t & 0xffff);
            carry = t >> 16;
        }
        int k = i + 4;
        while (carry) {
            const u64 t = r[k] + carry;
            r[k] = static_cast<u32>(t & 0xffff);
            carry = t >> 16;
            ++k;
        }
    }
    U128 out;
    for (int i = 0; i < 4; ++i) out.lo |= static_cast<u64>(r[i]) << (16 * i);
    for (int i = 0; i < 4; ++i) out.hi |= static_cast<u64>(r[i + 4]) << (16 * i);
    return out;
}

// Independent reference: restoring binary long division.
U128 refDiv(U128 n, u64 d, u64& rem) {
    U128 q{};
    U128 r{};
    for (int i = 127; i >= 0; --i) {
        r = shl(r, 1);
        const u64 bit = i >= 64 ? (n.hi >> (i - 64)) & 1 : (n.lo >> i) & 1;
        r.lo |= bit;
        if (r >= U128{0, d}) {
            r = sub(r, U128{0, d});
            if (i >= 64) {
                q.hi |= u64(1) << (i - 64);
            } else {
                q.lo |= u64(1) << i;
            }
        }
    }
    rem = r.lo;
    return q;
}

// a * b for a 128-bit a and 64-bit b; `overflow` set if the product needs more than 128 bits.
U128 mul128x64(U128 a, u64 b, bool& overflow) {
    const U128 lo = mulU64(a.lo, b);
    const U128 hi = mulU64(a.hi, b);
    overflow = hi.hi != 0;
    const U128 shifted{hi.lo, 0};
    const U128 sum = add(lo, shifted);
    if (sum < lo) overflow = true;
    return sum;
}

} // namespace

TEST_CASE("fixed: 128-bit helpers match independent references") {
    Rng r{7};
    for (int i = 0; i < 20000; ++i) {
        const u64 a = r.next() >> (r.next() % 64);
        const u64 b = r.next() >> (r.next() % 64);
        CHECK(mulU64(a, b) == refMul(a, b));
        const u64 d = (r.next() >> (r.next() % 64)) | 1;
        const U128 n{r.next() >> (r.next() % 64), r.next()};
        u64 rem = 0, refRem = 0;
        const U128 q = divU128(n, d, &rem);
        CHECK(q == refDiv(n, d, refRem));
        CHECK(rem == refRem);
    }
    CHECK(mulU64(~u64(0), ~u64(0)) == U128{~u64(0) - 1, 1});
    // Signed products in two's complement.
    const U128 m = mulI64(-3, 5);
    CHECK(m == sub(U128{}, U128{0, 15}));
    const U128 mm = mulI64(std::numeric_limits<i64>::min(), -1);
    CHECK(mm == U128{0, u64(1) << 63});
    CHECK(shl(U128{0, 1}, 127) == U128{u64(1) << 63, 0});
    CHECK(shr(U128{u64(1) << 63, 0}, 127) == U128{0, 1});
}

TEST_CASE("fixed: exact integer square roots") {
    CHECK(isqrt(u64(0)) == 0);
    CHECK(isqrt(u64(1)) == 1);
    CHECK(isqrt(u64(15)) == 3);
    CHECK(isqrt(u64(16)) == 4);
    CHECK(isqrt(~u64(0)) == 0xffffffffu);
    static_assert(isqrt(u64(1) << 62) == (u32(1) << 31));
    Rng r{11};
    for (int i = 0; i < 20000; ++i) {
        const u64 x = r.next() >> (r.next() % 64);
        const u64 s = isqrt(x);
        CHECK(s * s <= x);
        CHECK(((s + 1) * (s + 1) > x || s == 0xffffffffu));
        const U128 big{r.next() >> (r.next() % 64), r.next()};
        const u64 bs = isqrt(big);
        CHECK(mulU64(bs, bs) <= big);
        if (bs != ~u64(0)) CHECK(mulU64(bs + 1, bs + 1) > big);
    }
    CHECK(isqrt(U128{~u64(0), ~u64(0)}) == ~u64(0));
}

TEST_CASE("fixed: integer rsqrt is the exact floor") {
    CHECK(rsqrtFixed(0, 16, 16) == ~u64(0));
    CHECK(rsqrtFixed(1, 100, 20) == ~u64(0)); // precision beyond 127 bits
    CHECK(rsqrtQ30(u64(1) << 60) == (u32(1) << 30)); // 1/|(0,0,1)| = 1
    CHECK(rsqrtQ30(u64(3) << 60) == 619925131u);     // floor(2^30 / sqrt(3))
    CHECK(rsqrtQ30(u64(2) << 60) == 759250124u);     // floor(2^30 / sqrt(2))
    Rng r{13};
    for (int i = 0; i < 20000; ++i) {
        const u32 fracIn = static_cast<u32>(r.next() % 61);
        const u32 fracOut = static_cast<u32>(r.next() % 31);
        const u64 x = (r.next() >> (r.next() % 64)) | 1;
        const u64 y = rsqrtFixed(x, fracIn, fracOut);
        const U128 target = shl(U128{0, 1}, fracIn + 2 * fracOut);
        bool ovf = false;
        const U128 lo = mul128x64(mulU64(y, y), x, ovf);
        REQUIRE(!ovf);
        CHECK(lo <= target);
        const U128 hi = mul128x64(mulU64(y + 1, y + 1), x, ovf);
        CHECK((ovf || hi > target));
    }
}

TEST_CASE("fixed: Q16 golden values and rounding rules") {
    CHECK(Q16::fromDouble(1.5).raw() == 98304);
    CHECK(Q16::fromInt(-3).raw() == -196608);
    CHECK(Q16::one().raw() == 65536);
    CHECK(Q16::half().raw() == 32768);
    CHECK(Q16::epsilon().raw() == 1);
    CHECK(Q16::fromDouble(1.5).toDouble() == 1.5);
    // fromDouble: half away from zero, saturation, NaN.
    CHECK(Q16::fromDouble(0.5 / 65536).raw() == 1);
    CHECK(Q16::fromDouble(-0.5 / 65536).raw() == -1);
    CHECK(Q16::fromDouble(1e10) == Q16::max());
    CHECK(Q16::fromDouble(-1e10) == Q16::min());
    CHECK(Q16::fromDouble(std::numeric_limits<f64>::quiet_NaN()).raw() == 0);
    // *: exact products and ties toward +infinity.
    CHECK((Q16::fromDouble(1.5) * Q16::fromDouble(2.25)).raw() == 221184);
    CHECK((Q16::fromDouble(-1.5) * Q16::fromDouble(2.25)).raw() == -221184);
    CHECK((Q16::epsilon() * Q16::half()).raw() == 1);
    CHECK((-Q16::epsilon() * Q16::half()).raw() == 0);
    // /: nearest, ties away from zero; saturation and division by zero.
    CHECK((Q16::one() / Q16::fromInt(3)).raw() == 21845);
    CHECK((Q16::fromInt(2) / Q16::fromInt(3)).raw() == 43691);
    CHECK((Q16::fromInt(-2) / Q16::fromInt(3)).raw() == -43691);
    CHECK((Q16::epsilon() / Q16::fromInt(2)).raw() == 1);  // 0.5 ulp -> away from zero
    CHECK((-Q16::epsilon() / Q16::fromInt(2)).raw() == -1);
    CHECK(Q16::fromInt(5) / Q16() == Q16::max());
    CHECK(Q16::fromInt(-5) / Q16() == Q16::min());
    CHECK((Q16() / Q16()).raw() == 0);
    CHECK(Q16::max() / Q16::epsilon() == Q16::max());
    // + and - wrap.
    CHECK(Q16::max() + Q16::epsilon() == Q16::min());
    CHECK(Q16::min() - Q16::epsilon() == Q16::max());
    CHECK(-Q16::min() == Q16::min());
    // Rounding helpers.
    const Q16 v = Q16::fromDouble(-2.25);
    CHECK(v.floorToInt() == -3);
    CHECK(v.floor().toDouble() == -3.0);
    CHECK(v.frac().toDouble() == 0.75);
    CHECK(Q16::fromDouble(2.5).roundToInt() == 3);
    CHECK(Q16::fromDouble(-2.5).roundToInt() == -2);
    CHECK(abs(v).toDouble() == 2.25);
    CHECK(Q16::fromDouble(1.25) < Q16::fromDouble(1.5));
    Q16 acc = Q16::fromInt(1);
    acc += Q16::half();
    acc *= Q16::fromInt(2);
    acc -= Q16::one();
    acc /= Q16::fromInt(4);
    CHECK(acc.toDouble() == 0.5);
    // Roots.
    CHECK(sqrt(Q16::fromInt(2)).raw() == 92681);  // floor(sqrt(2) * 65536)
    CHECK(sqrt(Q16::fromInt(4)).raw() == 131072);
    CHECK(sqrt(Q16::fromInt(-4)).raw() == 0);
    CHECK(rsqrt(Q16::fromInt(2)).raw() == 46340); // floor(65536 / sqrt(2))
    CHECK(rsqrt(Q16::fromInt(4)) == Q16::half());
    CHECK(rsqrt(Q16::one()) == Q16::one());
    CHECK(rsqrt(Q16::epsilon()).raw() == 1 << 24);
    CHECK(rsqrt(Q16()) == Q16::max());
}

TEST_CASE("fixed: Q32 and Fixed64 golden values (128-bit products)") {
    CHECK(Q32::fromDouble(1.5).raw() == 6442450944LL);
    CHECK((Q32::fromDouble(1.5) * Q32::fromDouble(2.25)).toDouble() == 3.375);
    CHECK((Q32::fromInt(100000) * Q32::fromInt(-3)).toDouble() == -300000.0);
    // (2^10 + 0.5) * (2^10 + 0.25): raw operands ~2^42, so the product needs 128-bit intermediates.
    const Q32 a = Q32::fromDouble(1024.5), b = Q32::fromDouble(1024.25);
    CHECK((a * b).toDouble() == 1049344.125);
    CHECK((Q32::fromDouble(-1024.5) * b).toDouble() == -1049344.125);
    CHECK((Q32::fromDouble(46341.5) * Q32::fromDouble(46341.5)).toDouble() == 2147534622.25 - 4294967296.0); // wraps
    CHECK((Q32::epsilon() * Q32::half()).raw() == 1);
    CHECK((Q32::fromInt(1) / Q32::fromInt(3)).raw() == 1431655765LL); // round(2^32 / 3)
    CHECK((Q32::fromInt(-7) / Q32::fromInt(2)).toDouble() == -3.5);
    CHECK(Q32::fromInt(1 << 30) / Q32::epsilon() == Q32::max());
    CHECK(sqrt(Q32::fromInt(2)).raw() == 6074000999LL); // floor(sqrt(2) * 2^32 = 6074000999.95)
    CHECK(rsqrt(Q32::fromInt(2)).raw() == 3037000499LL); // floor(2^32 / sqrt(2))
    CHECK(sqrt(Q32::fromInt(1 << 20)).toDouble() == 1024.0);

    // Fixed64: 2^-10 m units, exact in f64 to 8.8e12 m (04 §5.4).
    CHECK(Fixed64::one().raw() == 1024);
    CHECK(Fixed64::fromDouble(8.8e12).toDouble() == 8.8e12);
    CHECK(Fixed64::fromDouble(1.0 / 1024.0).raw() == 1);
    CHECK(Fixed64::fromDouble(0.0004).raw() == 0); // < half a unit
    CHECK(Fixed64::fromDouble(0.0005).raw() == 1);
    const Fixed64 pos = Fixed64::fromDouble(1.5e12), vel = Fixed64::fromDouble(1234.5);
    const Fixed64 dt = Fixed64::fromDouble(0.0625);
    CHECK((pos + vel * dt).toDouble() == 1500000000077.1562);
    CHECK((Fixed64::fromDouble(10.0) / Fixed64::fromDouble(4.0)).toDouble() == 2.5);
    CHECK(sqrt(Fixed64::fromDouble(16.0)).toDouble() == 4.0);

    // Format conversions.
    CHECK(fixedCast<Q32>(Q16::fromDouble(-1.25)).toDouble() == -1.25);
    CHECK(fixedCast<Q16>(Q32::fromDouble(1.0 + 0x1p-17)).raw() == 65537); // tie rounds up
    CHECK(fixedCast<Q16>(Q32::fromDouble(1.0 + 0x1p-18)).raw() == 65536);
    CHECK(fixedCast<Fixed64>(Q32::fromDouble(3.0)).raw() == 3 * 1024);
    // Rounding at the limits must not overflow.
    CHECK(Q32::max().roundToInt() == (i64(1) << 31));
    CHECK(Q32::min().roundToInt() == -(i64(1) << 31));
    CHECK(fixedCast<Fixed64>(Q32::max()).raw() == (i64(1) << 41));
    CHECK(fixedCast<Fixed64>(Q32::min()).raw() == -(i64(1) << 41));
}

TEST_CASE("fixed: operations agree with exact rational arithmetic") {
    Rng r{17};
    for (int i = 0; i < 20000; ++i) {
        const i32 a = static_cast<i32>(r.next());
        const i32 b = static_cast<i32>(r.next() >> 40) + 1;
        // Q16 *: exact product rounded half up, wrapped.
        const i64 prod = static_cast<i64>(a) * b;
        CHECK((Q16::fromRaw(a) * Q16::fromRaw(b)).raw() == static_cast<i32>((prod + 32768) >> 16));
        // Q16 /: |q - exact| <= 1/2 ulp.
        const Q16 q = Q16::fromRaw(a) / Q16::fromRaw(b);
        const f64 exact = static_cast<f64>(a) * 65536.0 / static_cast<f64>(b);
        if (std::fabs(exact) < 2147483647.0) CHECK(std::fabs(static_cast<f64>(q.raw()) - exact) <= 0.5);
        // Q32 * against the 128-bit reference product.
        const i64 x = static_cast<i64>(r.next()) >> (r.next() % 40);
        const i64 y = static_cast<i64>(r.next()) >> (r.next() % 40);
        const U128 p = add(mulI64(x, y), U128{0, u64(1) << 31});
        CHECK(static_cast<u64>((Q32::fromRaw(x) * Q32::fromRaw(y)).raw()) == ((p.lo >> 32) | (p.hi << 32)));
    }
}

TEST_CASE("fixed: golden hash over mixed operations") {
    Rng r{23};
    u64 h = 0xcbf29ce484222325ULL;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    };
    for (int i = 0; i < 5000; ++i) {
        const Q16 a = Q16::fromRaw(static_cast<i32>(r.next()));
        const Q16 b = Q16::fromRaw(static_cast<i32>(r.next() >> 33) + 1);
        mix(static_cast<u32>((a * b).raw()));
        mix(static_cast<u32>((a / b).raw()));
        mix(static_cast<u32>(sqrt(b).raw()));
        mix(static_cast<u32>(rsqrt(b).raw()));
        const Q32 c = Q32::fromRaw(static_cast<i64>(r.next()) >> 8);
        const Q32 d = Q32::fromRaw(static_cast<i64>(r.next() >> 20) + 1);
        mix(static_cast<u64>((c * d).raw()));
        mix(static_cast<u64>((c / d).raw()));
        mix(static_cast<u64>(rsqrt(d).raw()));
        mix(rsqrtQ30((u64(1) << 60) + (r.next() >> 3)));
    }
    CHECK(h == 0x12f8a5d2d681ac08ULL);
}
