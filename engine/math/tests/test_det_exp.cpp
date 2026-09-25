// det::exp / ln / pow / asinh: special values, accuracy against 64-bit-mantissa references, and
// golden hashes that pin the exact bits on every compiler (09 WP-0.5: "det:: hashes equal on 5
// compilers"). If a golden hash fails on one toolchain only, fix the build flags (FP contraction,
// fast-math) — never update the constant to make one platform pass.
#include <doctest/doctest.h>

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "helios/math/scalar.h"

using namespace helios;

namespace {

constexpr f64 kInf = std::numeric_limits<f64>::infinity();
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

struct SplitMix {
    u64 s;
    u64 next() {
        u64 z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    f64 unit() { return static_cast<f64>(next() >> 11) * 0x1p-53; }
    f64 uniform(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
    // Log-uniform magnitude in [2^e0, 2^e1).
    f64 logUniform(int e0, int e1) {
        const f64 m = 1.0 + unit();
        const int e = e0 + static_cast<int>(next() % static_cast<u64>(e1 - e0));
        return std::ldexp(m, e);
    }
};

struct Hasher {
    u64 h = 0xcbf29ce484222325ULL;
    void add(f64 v) {
        const u64 bits = std::isnan(v) ? 0x7ff8000000000000ULL : std::bit_cast<u64>(v);
        for (int i = 0; i < 8; ++i) {
            h ^= (bits >> (8 * i)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    }
};

bool same(f64 a, f64 b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
}

// Deterministic input sets (shared by the accuracy and golden-hash tests).
std::vector<f64> expInputs() {
    SplitMix r{1};
    std::vector<f64> v;
    for (int i = 0; i < 20000; ++i) v.push_back(r.uniform(-750.0, 712.0));
    for (int i = 0; i < 4000; ++i) v.push_back(r.uniform(-1.0, 1.0) * r.logUniform(-60, 0));
    for (f64 s : {0.0, -0.0, 1.0, -1.0, 0.5, 709.782712893384, 709.79, -745.1332191019411, -745.14, -708.4, -720.0,
                  1e-300, -1e-300, 0x1p-1074, 0.34657359027997264, -0.34657359027997264, 88.72283905206835}) {
        v.push_back(s);
    }
    return v;
}

std::vector<f64> lnInputs() {
    SplitMix r{2};
    std::vector<f64> v;
    for (int i = 0; i < 20000; ++i) v.push_back(std::bit_cast<f64>(r.next() & 0x7fefffffffffffffULL)); // any finite
    for (int i = 0; i < 4000; ++i) v.push_back(1.0 + r.uniform(-1.0, 1.0) * r.logUniform(-52, -1));
    for (int k = -8; k <= 8; ++k) v.push_back(1.0 + k * 0x1p-52);
    for (f64 s : {1.0, 2.0, 0.5, 0.7071067811865476, 1.4142135623730951, DBL_MAX, DBL_MIN, 0x1p-1074, 0x1p-1060,
                  2.718281828459045, 10.0, 1e-300, 1e300}) {
        v.push_back(s);
    }
    return v;
}

std::vector<std::pair<f64, f64>> powInputs() {
    SplitMix r{3};
    std::vector<std::pair<f64, f64>> v;
    for (int i = 0; i < 12000; ++i) v.push_back({r.logUniform(-40, 40), r.uniform(-30.0, 30.0)});
    for (int i = 0; i < 4000; ++i) v.push_back({1.0 + r.uniform(-0.01, 0.01), r.uniform(-1e5, 1e5)});
    for (int i = 0; i < 4000; ++i) {
        v.push_back({-r.logUniform(-10, 10), static_cast<f64>(static_cast<int>(r.next() % 61) - 30)});
    }
    // HXL-style formula inputs: 0.5^(...), attribute stacking (0.87^k), signature scaling.
    for (int k = 0; k < 200; ++k) v.push_back({0.5, k * 0.137});
    for (int k = 0; k < 50; ++k) v.push_back({0.8691199808003974, static_cast<f64>(k)});
    return v;
}

std::vector<f64> asinhInputs() {
    SplitMix r{4};
    std::vector<f64> v;
    for (int i = 0; i < 20000; ++i) {
        const f64 m = r.logUniform(-40, 40);
        v.push_back((r.next() & 1) ? m : -m);
    }
    for (int i = 0; i < 2000; ++i) v.push_back(r.uniform(-3.0, 3.0));
    for (f64 s : {0x1p-28, 0x1p28, 0x1.0000000000001p28, 0x1.fffffffffffffp-29, 1e-300, 1e300, DBL_MAX, 0x1p-1074, 1.0,
                  -1.0}) {
        v.push_back(s);
    }
    return v;
}

#if LDBL_MANT_DIG >= 64
// Error in ulps of the double nearest to `ref`.
f64 ulpError(f64 got, long double ref) {
    const f64 refD = static_cast<f64>(ref);
    if (std::isinf(refD) || std::isinf(got)) return got == refD ? 0.0 : 1e9;
    const f64 mag = std::fabs(refD);
    const f64 ulp = mag < DBL_MIN ? 0x1p-1074 : std::nextafter(mag, kInf) - mag;
    return static_cast<f64>(std::fabs(static_cast<long double>(got) - ref) / static_cast<long double>(ulp));
}
#endif

} // namespace

TEST_CASE("det::exp/ln/pow/asinh: special values follow C99 Annex F") {
    CHECK(det::exp(0.0) == 1.0);
    CHECK(det::exp(-0.0) == 1.0);
    CHECK(det::exp(kInf) == kInf);
    CHECK(det::exp(-kInf) == 0.0);
    CHECK(std::isnan(det::exp(kNaN)));
    CHECK(det::exp(710.0) == kInf);
    CHECK(det::exp(-746.0) == 0.0);
    CHECK(det::exp(709.782712893384) < kInf); // largest finite result
    CHECK(det::exp(-745.1332191019411) > 0.0); // smallest subnormal region
    CHECK(det::exp(1.0) == 2.718281828459045);

    CHECK(det::ln(1.0) == 0.0);
    CHECK(!std::signbit(det::ln(1.0)));
    CHECK(det::ln(0.0) == -kInf);
    CHECK(det::ln(-0.0) == -kInf);
    CHECK(std::isnan(det::ln(-1.0)));
    CHECK(std::isnan(det::ln(-kInf)));
    CHECK(det::ln(kInf) == kInf);
    CHECK(std::isnan(det::ln(kNaN)));
    CHECK(det::ln(2.0) == 0x1.62e42fefa39efp-1);
    CHECK(det::ln(0x1p-1074) == -744.4400719213812);

    CHECK(det::pow(kNaN, 0.0) == 1.0);
    CHECK(det::pow(1.0, kNaN) == 1.0);
    CHECK(std::isnan(det::pow(kNaN, 1.0)));
    CHECK(std::isnan(det::pow(2.0, kNaN)));
    CHECK(std::isnan(det::pow(-8.0, 1.0 / 3.0)));
    CHECK(same(det::pow(-0.0, 3.0), -0.0));
    CHECK(same(det::pow(-0.0, 2.0), 0.0));
    CHECK(det::pow(-0.0, -3.0) == -kInf);
    CHECK(det::pow(0.0, -2.0) == kInf);
    CHECK(det::pow(0.0, -0.5) == kInf);
    CHECK(same(det::pow(0.0, 0.5), 0.0));
    CHECK(det::pow(-1.0, kInf) == 1.0);
    CHECK(det::pow(-1.0, -kInf) == 1.0);
    CHECK(det::pow(0.5, kInf) == 0.0);
    CHECK(det::pow(0.5, -kInf) == kInf);
    CHECK(det::pow(2.0, kInf) == kInf);
    CHECK(det::pow(2.0, -kInf) == 0.0);
    CHECK(det::pow(kInf, -1.0) == 0.0);
    CHECK(det::pow(kInf, 0.5) == kInf);
    CHECK(same(det::pow(-kInf, -3.0), -0.0));
    CHECK(same(det::pow(-kInf, -2.0), 0.0));
    CHECK(det::pow(-kInf, 3.0) == -kInf);
    CHECK(det::pow(-kInf, 2.0) == kInf);
    CHECK(det::pow(-2.0, 3.0) == -8.0);
    CHECK(det::pow(-2.0, -3.0) == -0.125);
    CHECK(det::pow(-2.0, 1e300) == kInf); // huge even integer
    CHECK(det::pow(2.0, 1e20) == kInf);
    CHECK(det::pow(2.0, -1e20) == 0.0);
    CHECK(det::pow(0.5, 1e20) == 0.0);
    // Exact shortcuts and exact powers of two.
    CHECK(det::pow(3.7, 1.0) == 3.7);
    CHECK(det::pow(3.7, 2.0) == 3.7 * 3.7);
    CHECK(det::pow(3.7, -1.0) == 1.0 / 3.7);
    CHECK(det::pow(2.25, 0.5) == 1.5);
    CHECK(det::pow(2.0, 10.0) == 1024.0);
    CHECK(det::pow(2.0, -1074.0) == 0x1p-1074);
    CHECK(det::pow(2.0, 1023.0) == 0x1p1023);
    CHECK(det::pow(10.0, 3.0) == 1000.0);

    CHECK(same(det::asinh(0.0), 0.0));
    CHECK(same(det::asinh(-0.0), -0.0));
    CHECK(det::asinh(kInf) == kInf);
    CHECK(det::asinh(-kInf) == -kInf);
    CHECK(std::isnan(det::asinh(kNaN)));
    CHECK(det::asinh(1e-300) == 1e-300);
    CHECK(det::asinh(-2.5) == -det::asinh(2.5));
    CHECK(det::asinh(DBL_MAX) == 0x1.633ce8fb9f87ep+9); // ln(2 DBL_MAX), correctly rounded

    // f32 overloads round the f64 result once.
    CHECK(det::exp(1.0f) == static_cast<f32>(det::exp(1.0)));
    CHECK(det::ln(3.0f) == static_cast<f32>(det::ln(3.0)));
    CHECK(det::pow(1.5f, 2.5f) == static_cast<f32>(det::pow(1.5, 2.5)));
    CHECK(det::asinh(0.25f) == static_cast<f32>(det::asinh(0.25)));
}

TEST_CASE("det::exp/ln/pow/asinh: identities") {
    SplitMix r{99};
    for (int i = 0; i < 2000; ++i) {
        const f64 x = r.uniform(-700.0, 700.0);
        // ln(exp(x)) round-trips to within a few ulps of x.
        // Rounding exp(x) costs up to 2^-53 relative, i.e. 2^-53 absolute after ln.
        CHECK(std::fabs(det::ln(det::exp(x)) - x) <= 4.0 * std::fabs(std::nextafter(x, kInf) - x) + 0x1p-52);
        const f64 a = r.logUniform(-20, 20);
        // a*a*a rounds twice (<= ~1 ulp); pow rounds once.
        CHECK(std::fabs(det::pow(a, 3.0) - a * a * a) <= 4e-16 * a * a * a);
    }
    // Monotonic across the reduction boundaries.
    f64 prev = det::exp(-2.0);
    for (int i = 1; i <= 4000; ++i) {
        const f64 cur = det::exp(-2.0 + i * 0.001);
        CHECK(cur >= prev);
        prev = cur;
    }
}

#if LDBL_MANT_DIG >= 64
TEST_CASE("det::exp/ln/pow/asinh: accuracy against 64-bit-mantissa references") {
    f64 worst = 0.0;
    for (f64 x : expInputs()) {
        const long double ref = expl(static_cast<long double>(x));
        if (static_cast<f64>(ref) < DBL_MIN) continue; // subnormal results: see below
        worst = std::max(worst, ulpError(det::exp(x), ref));
    }
    MESSAGE("det::exp max error " << worst << " ulp");
    CHECK(worst < 0.52);

    worst = 0.0;
    for (f64 x : lnInputs()) {
        if (x <= 0.0) continue;
        worst = std::max(worst, ulpError(det::ln(x), logl(static_cast<long double>(x))));
    }
    MESSAGE("det::ln max error " << worst << " ulp");
    CHECK(worst < 0.51);

    worst = 0.0;
    for (auto [x, y] : powInputs()) {
        const long double ref = powl(static_cast<long double>(x), static_cast<long double>(y));
        const f64 refD = static_cast<f64>(ref);
        if (std::isnan(refD) || std::isinf(refD) || std::fabs(refD) < DBL_MIN) continue;
        worst = std::max(worst, ulpError(det::pow(x, y), ref));
    }
    MESSAGE("det::pow max error " << worst << " ulp");
    CHECK(worst < 0.52);

    worst = 0.0;
    for (f64 x : asinhInputs()) worst = std::max(worst, ulpError(det::asinh(x), asinhl(static_cast<long double>(x))));
    MESSAGE("det::asinh max error " << worst << " ulp");
    CHECK(worst < 0.51);

    // Subnormal results: one extra rounding when scaling into the subnormal range.
    worst = 0.0;
    for (f64 x = -745.0; x < -708.5; x += 0.01) worst = std::max(worst, ulpError(det::exp(x), expl(x)));
    MESSAGE("det::exp subnormal max error " << worst << " ulp");
    CHECK(worst <= 1.0);
}
#endif

TEST_CASE("det::exp/ln/pow/asinh: golden hashes (bit-identical on every compiler)") {
    Hasher he, hl, hp, ha;
    for (f64 x : expInputs()) he.add(det::exp(x));
    for (f64 x : lnInputs()) hl.add(det::ln(x));
    for (auto [x, y] : powInputs()) hp.add(det::pow(x, y));
    for (f64 x : asinhInputs()) ha.add(det::asinh(x));
    // Computed once with GCC 13; verified identical with Clang 18 (x86-64) — see README.md.
    CHECK(he.h == 0x326543eaf9427a5fULL);
    CHECK(hl.h == 0x3fb4cab44bb60247ULL);
    CHECK(hp.h == 0x0a9d6178cd84387bULL);
    CHECK(ha.h == 0x8f30fd19714fe5f7ULL);
}
