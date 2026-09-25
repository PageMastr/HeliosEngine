#include "test_util.h"

#include <cstring>

using namespace helios;
using helios::test::Rng;

TEST_CASE("vec: construction, layout and conversion") {
    constexpr Vec3 zero;
    static_assert(zero.x == 0.0f && zero.y == 0.0f && zero.z == 0.0f);
    static_assert(Vec4(2.0f).w == 2.0f);
    static_assert(Vec3(Vec2(1.0f, 2.0f), 3.0f).z == 3.0f);
    static_assert(Vec4(Vec3(1.0f, 2.0f, 3.0f), 4.0f).xyz() == Vec3(1.0f, 2.0f, 3.0f));
    static_assert(Vec4(Vec2(1.0f, 2.0f), Vec2(3.0f, 4.0f)).zw() == Vec2(3.0f, 4.0f));
    static_assert(IVec3::unitY() == IVec3(0, 1, 0));
    static_assert(Vec2::one() == Vec2(1.0f, 1.0f));
    static_assert(offsetof(Vec4, w) == 12);

    const DVec3 d{1.0 + 1e-12, 2.0, 3.0};
    const Vec3 f(d);  // explicit narrowing
    CHECK(f == Vec3(1.0f, 2.0f, 3.0f));
    CHECK(toF64(f) == DVec3(1.0, 2.0, 3.0));
    CHECK(toF32(DVec2(0.5, 0.25)) == Vec2(0.5f, 0.25f));
    CHECK(IVec2(Vec2(1.9f, -1.9f)) == IVec2(1, -1));  // truncating conversion
    static_assert(!std::is_convertible_v<DVec3, Vec3>, "f64 -> f32 must be explicit");
    static_assert(!std::is_convertible_v<Vec3, DVec3>, "conversions are always explicit");

    Vec4 v{1.0f, 2.0f, 3.0f, 4.0f};
    f32 raw[4];
    std::memcpy(raw, v.data(), sizeof(raw));
    CHECK(raw[3] == 4.0f);
    v[2] = 9.0f;
    CHECK(v.z == 9.0f);
    CHECK(v[3] == 4.0f);
}

TEST_CASE("vec: arithmetic") {
    constexpr Vec3 a{1.0f, 2.0f, 3.0f};
    constexpr Vec3 b{4.0f, 5.0f, 6.0f};
    static_assert(a + b == Vec3(5.0f, 7.0f, 9.0f));
    static_assert(b - a == Vec3(3.0f, 3.0f, 3.0f));
    static_assert(a * b == Vec3(4.0f, 10.0f, 18.0f));
    static_assert(b / a == Vec3(4.0f, 2.5f, 2.0f));
    static_assert(a * 2.0f == 2.0f * a);
    static_assert(-a == Vec3(-1.0f, -2.0f, -3.0f));
    static_assert(a / 2.0f == Vec3(0.5f, 1.0f, 1.5f));
    Vec3 c = a;
    c += b;
    c -= Vec3(1.0f);
    c *= 2.0f;
    c /= Vec3(2.0f, 4.0f, 8.0f);
    CHECK(c == Vec3(4.0f, 3.0f, 2.0f));
    CHECK(a * 2 == Vec3(2.0f, 4.0f, 6.0f));  // int scalar converts

    constexpr IVec3 i{7, -3, 4};
    static_assert(i + IVec3(1) == IVec3(8, -2, 5));
    static_assert(i / 2 == IVec3(3, -1, 2));
    static_assert(i * IVec3(2, 2, -1) == IVec3(14, -6, -4));
    constexpr UVec2 u{5u, 9u};
    static_assert(u * 2u == UVec2(10u, 18u));
    static_assert(DVec4(1.0) + DVec4(1.0, 2.0, 3.0, 4.0) == DVec4(2.0, 3.0, 4.0, 5.0));
}

TEST_CASE("vec: swizzles") {
    constexpr Vec3 v{1.0f, 2.0f, 3.0f};
    static_assert(v.xy() == Vec2(1.0f, 2.0f));
    static_assert(v.xz() == Vec2(1.0f, 3.0f));
    static_assert(v.yz() == Vec2(2.0f, 3.0f));
    static_assert(v.zyx() == Vec3(3.0f, 2.0f, 1.0f));
    static_assert(Vec2(1.0f, 2.0f).yx() == Vec2(2.0f, 1.0f));
    static_assert(swizzle<2, 0>(v) == Vec2(3.0f, 1.0f));
    static_assert(swizzle<0, 2, 1>(v) == Vec3(1.0f, 3.0f, 2.0f));
    static_assert(swizzle<3, 2, 1, 0>(Vec4(1.0f, 2.0f, 3.0f, 4.0f)) == Vec4(4.0f, 3.0f, 2.0f, 1.0f));
    static_assert(swizzle<0, 0, 0>(IVec2(5, 6)) == IVec3(5, 5, 5));
}

TEST_CASE("vec: dot, cross, length, normalize") {
    static_assert(dot(Vec3(1.0f, 2.0f, 3.0f), Vec3(4.0f, 5.0f, 6.0f)) == 32.0f);
    static_assert(dot(IVec2(1, 2), IVec2(3, 4)) == 11);
    // Right-handed: X x Y = Z, Y x Z = X, Z x X = Y.
    static_assert(cross(Vec3::unitX(), Vec3::unitY()) == Vec3::unitZ());
    static_assert(cross(Vec3::unitY(), Vec3::unitZ()) == Vec3::unitX());
    static_assert(cross(Vec3::unitZ(), Vec3::unitX()) == Vec3::unitY());
    static_assert(cross(Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f)) == 1.0f);
    static_assert(perp(Vec2(1.0f, 0.0f)) == Vec2(0.0f, 1.0f));
    static_assert(lengthSq(Vec3(2.0f, 3.0f, 6.0f)) == 49.0f);
    CHECK(length(Vec3(2.0f, 3.0f, 6.0f)) == 7.0f);
    CHECK(distance(DVec3(1.0, 1.0, 1.0), DVec3(4.0, 5.0, 1.0)) == 5.0);
    CHECK(distanceSq(IVec2(0, 0), IVec2(3, 4)) == 25);
    CHECK(approxEqual(normalize(Vec3(3.0f, 0.0f, 4.0f)), Vec3(0.6f, 0.0f, 0.8f)));
    CHECK(normalizeOrZero(Vec3{}) == Vec3{});
    CHECK(normalizeOr(Vec3{}, Vec3::unitY()) == Vec3::unitY());
    CHECK(normalizeOr(Vec3(std::nanf(""), 0.0f, 0.0f), Vec3::unitX()) == Vec3::unitX());
    CHECK(normalizeOr(Vec3(kInfinity, 0.0f, 0.0f), Vec3::unitZ()) == Vec3::unitZ());
    CHECK(isNormalized(normalize(DVec3(1.0, 2.0, 3.0))));
    CHECK_FALSE(isNormalized(Vec3(1.0f, 1.0f, 0.0f)));
    CHECK(isFinite(Vec3(1.0f)));
    CHECK_FALSE(isFinite(DVec2(1.0, kInfinityD)));
    Rng rng(10);
    for (int i = 0; i < 1000; ++i) {
        const DVec3 a = rng.dvec3(-10.0, 10.0), b = rng.dvec3(-10.0, 10.0);
        const DVec3 c = cross(a, b);
        CHECK(std::fabs(dot(c, a)) < 1e-9);
        CHECK(std::fabs(dot(c, b)) < 1e-9);
        CHECK(approxEqual(cross(b, a), -c, 1e-12));
    }
}

TEST_CASE("vec: component-wise helpers") {
    constexpr Vec3 a{1.0f, -2.0f, 3.5f};
    constexpr Vec3 b{0.0f, 5.0f, 2.0f};
    static_assert(min(a, b) == Vec3(0.0f, -2.0f, 2.0f));
    static_assert(max(a, b) == Vec3(1.0f, 5.0f, 3.5f));
    static_assert(abs(a) == Vec3(1.0f, 2.0f, 3.5f));
    static_assert(abs(IVec2(-3, 4)) == IVec2(3, 4));
    static_assert(sign(a) == Vec3(1.0f, -1.0f, 1.0f));
    static_assert(clamp(a, 0.0f, 2.0f) == Vec3(1.0f, 0.0f, 2.0f));
    static_assert(clamp(a, Vec3(0.0f), Vec3(0.5f, 1.0f, 4.0f)) == Vec3(0.5f, 0.0f, 3.5f));
    static_assert(saturate(a) == Vec3(1.0f, 0.0f, 1.0f));
    static_assert(lerp(Vec2(0.0f), Vec2(2.0f, 4.0f), 0.5f) == Vec2(1.0f, 2.0f));
    static_assert(lerp(Vec2(0.0f), Vec2(2.0f, 4.0f), Vec2(0.0f, 1.0f)) == Vec2(0.0f, 4.0f));
    static_assert(step(Vec2(0.5f), Vec2(0.4f, 0.6f)) == Vec2(0.0f, 1.0f));
    static_assert(minComponent(a) == -2.0f && maxComponent(a) == 3.5f);
    static_assert(minComponent(Vec4(4.0f, 3.0f, 2.0f, 1.0f)) == 1.0f &&
                  maxComponent(Vec2(4.0f, 3.0f)) == 4.0f);
    static_assert(maxAbsAxis(Vec3(1.0f, -7.0f, 3.0f)) == 1 && maxAbsAxis(Vec3(1.0f, 1.0f, 1.0f)) == 0);
    static_assert(sum(Vec4(1.0f, 2.0f, 3.0f, 4.0f)) == 10.0f);
    CHECK(floor(Vec3(1.5f, -1.5f, 2.0f)) == Vec3(1.0f, -2.0f, 2.0f));
    CHECK(ceil(Vec3(1.5f, -1.5f, 2.0f)) == Vec3(2.0f, -1.0f, 2.0f));
    CHECK(round(Vec2(2.5f, -2.5f)) == Vec2(3.0f, -3.0f));
    CHECK(trunc(Vec2(2.7f, -2.7f)) == Vec2(2.0f, -2.0f));
    CHECK(fract(Vec2(1.25f, -1.25f)) == Vec2(0.25f, 0.75f));
    CHECK(smoothstep(0.0f, 1.0f, Vec2(0.5f, 2.0f)) == Vec2(0.5f, 1.0f));
    CHECK(floorToI32(Vec3(-0.5f, 1.5f, -2.0f)) == IVec3(-1, 1, -2));
    CHECK(floorToI32(DVec2(-1e-9, 3.999)) == IVec2(-1, 3));
}

TEST_CASE("vec: reflection, refraction and projection") {
    const Vec3 n{0.0f, 1.0f, 0.0f};
    CHECK(reflectVector(Vec3(1.0f, -1.0f, 0.0f), n) == Vec3(1.0f, 1.0f, 0.0f));
    CHECK(reflectVector(DVec3(0.0, -2.0, 3.0), DVec3(0.0, 1.0, 0.0)) == DVec3(0.0, 2.0, 3.0));
    const Vec3 i = normalize(Vec3(1.0f, -1.0f, 0.0f));
    CHECK(approxEqual(refract(i, n, 1.0f), i));
    // Snell: n1 sin1 = n2 sin2.
    const Vec3 t = refract(i, n, 1.0f / 1.5f);
    CHECK(approxEqual(t.x, std::sin(kQuarterPi) / 1.5f, 1e-6f));
    CHECK(approxEqual(length(t), 1.0f, 1e-6f));
    // Total internal reflection.
    CHECK(refract(i, n, 1.5f) == Vec3{});
    CHECK(project(Vec3(2.0f, 3.0f, 0.0f), Vec3(2.0f, 0.0f, 0.0f)) == Vec3(2.0f, 0.0f, 0.0f));
    CHECK(reject(Vec3(2.0f, 3.0f, 0.0f), Vec3(2.0f, 0.0f, 0.0f)) == Vec3(0.0f, 3.0f, 0.0f));
}

TEST_CASE("vec: angles") {
    CHECK(angleBetween(Vec3::unitX(), Vec3::unitX()) == 0.0f);
    CHECK(approxEqual(angleBetween(Vec3::unitX(), -Vec3::unitX()), kPi));
    CHECK(approxEqual(angleBetween(Vec3::unitX(), Vec3(0.0f, 0.0f, 5.0f)), kHalfPi));
    // atan2 form keeps precision for tiny angles where acos(dot) rounds to 0.
    const f64 tiny = 1e-9;
    const DVec3 a{1.0, 0.0, 0.0}, b{std::cos(tiny), std::sin(tiny), 0.0};
    CHECK(std::acos(dot(a, b)) == 0.0);
    CHECK(approxEqual(angleBetween(a, b), tiny, 0.0, 1e-9));
    CHECK(approxEqual(angleBetween(Vec2(1.0f, 0.0f), Vec2(0.0f, -1.0f)), kHalfPi));
    CHECK(approxEqual(signedAngle(Vec2(1.0f, 0.0f), Vec2(0.0f, -1.0f)), -kHalfPi));
    CHECK(approxEqual(rotate(Vec2(1.0f, 0.0f), kHalfPi), Vec2(0.0f, 1.0f)));
}

TEST_CASE("vec: orthonormal basis is right-handed and continuous") {
    Rng rng(11);
    auto checkBasis = [](const DVec3& n) {
        DVec3 t, b;
        orthonormalBasis(n, t, b);
        CHECK(approxEqual(length(t), 1.0, 1e-12));
        CHECK(approxEqual(length(b), 1.0, 1e-12));
        CHECK(std::fabs(dot(t, n)) < 1e-12);
        CHECK(std::fabs(dot(b, n)) < 1e-12);
        CHECK(std::fabs(dot(t, b)) < 1e-12);
        CHECK(approxEqual(cross(t, b), n, 1e-12));
    };
    for (int i = 0; i < 1000; ++i) checkBasis(rng.unitD());
    checkBasis({0.0, 0.0, 1.0});
    checkBasis({0.0, 0.0, -1.0});
    checkBasis(normalize(DVec3(1e-9, 0.0, -1.0)));
    checkBasis({1.0, 0.0, 0.0});
    const Vec3 p = anyPerpendicular(Vec3(0.0f, 1.0f, 0.0f));
    CHECK(std::fabs(p.y) < 1e-7f);
    CHECK(approxEqual(length(p), 1.0f, 1e-6f));
}
