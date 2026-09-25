#include "test_util.h"

using namespace helios;
using helios::test::Rng;

namespace {
DMat4 randomMatrix(Rng& rng) {
    DMat4 m;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) m.at(r, c) = rng.range(-2.0, 2.0);
    }
    return m;
}
DMat4 randomTRS(Rng& rng, bool uniformScale = false) {
    const f64 s0 = rng.range(0.2, 3.0);
    const DVec3 s = uniformScale ? DVec3(s0) : DVec3(s0, rng.range(0.2, 3.0), rng.range(0.2, 3.0));
    return DMat4::trs(rng.dvec3(-100.0, 100.0), rng.quatD(), s);
}
}  // namespace

TEST_CASE("mat: layout is column-major with column vectors") {
    constexpr Mat4 id;
    static_assert(id.at(0, 0) == 1.0f && id.at(1, 0) == 0.0f && id.at(3, 3) == 1.0f);
    const Mat4 t = Mat4::translation({1.0f, 2.0f, 3.0f});
    // Translation lives in column 3, i.e. floats 12..14 in memory (GLSL mat4 layout).
    CHECK(t.data()[12] == 1.0f);
    CHECK(t.data()[13] == 2.0f);
    CHECK(t.data()[14] == 3.0f);
    CHECK(t[3] == Vec4(1.0f, 2.0f, 3.0f, 1.0f));
    CHECK(t.at(0, 3) == 1.0f);
    CHECK(t.row(0) == Vec4(1.0f, 0.0f, 0.0f, 1.0f));
    const Mat3 r = Mat3::fromRows({1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}, {7.0f, 8.0f, 9.0f});
    CHECK(r.at(0, 1) == 2.0f);
    CHECK(r[1] == Vec3(2.0f, 5.0f, 8.0f));
    CHECK(r * Vec3(1.0f, 0.0f, 0.0f) == Vec3(1.0f, 4.0f, 7.0f));
    CHECK(transformPoint(t, Vec3(1.0f, 1.0f, 1.0f)) == Vec3(2.0f, 3.0f, 4.0f));
    CHECK(transformVector(t, Vec3(1.0f, 1.0f, 1.0f)) == Vec3(1.0f, 1.0f, 1.0f));
    CHECK(DMat4(Mat4::identity()) == DMat4::identity());
    CHECK(toF32(DMat3::identity()) == Mat3{});
    CHECK(Mat4(Mat3::scaling({2.0f, 3.0f, 4.0f}), {1.0f, 0.0f, 0.0f}).linear() ==
          Mat3::scaling({2.0f, 3.0f, 4.0f}));
}

TEST_CASE("mat: multiplication") {
    Rng rng(30);
    for (int i = 0; i < 300; ++i) {
        const DMat4 a = randomMatrix(rng), b = randomMatrix(rng);
        const DVec4 v{rng.range(-1.0, 1.0), rng.range(-1.0, 1.0), rng.range(-1.0, 1.0), 1.0};
        CHECK(approxEqual((a * b) * v, a * (b * v), 1e-12));
        CHECK(approxEqual(transpose(a * b), transpose(b) * transpose(a), 1e-12));
        CHECK(a * DMat4::identity() == a);
        CHECK(DMat4::identity() * a == a);
    }
    const Mat3 m3 = Mat3::fromRows({1.0f, 2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 2.0f});
    CHECK(m3 * m3 == Mat3::fromRows({1.0f, 4.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 4.0f}));
}

TEST_CASE("mat: SIMD and scalar multiplication are bit-identical") {
#if HELIOS_MATH_SSE2
    Rng rng(31);
    for (int i = 0; i < 20000; ++i) {
        Mat4 a, b;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                a.at(r, c) = rng.rangef(-1e3f, 1e3f);
                b.at(r, c) = rng.rangef(-1e3f, 1e3f);
            }
        }
        const Mat4 s = detail::mulMat4Scalar(a, b);
        const Mat4 v = detail::mulMat4Sse(a, b);
        bool same = true;
        for (int k = 0; k < 16; ++k)
            same = same && helios::test::bits(s.data()[k]) == helios::test::bits(v.data()[k]);
        CHECK(same);
        CHECK((a * b) == s);
    }
#else
    MESSAGE("SSE2 path not compiled on this target; scalar path only");
#endif
}

TEST_CASE("mat: transpose, determinant, inverse") {
    Rng rng(32);
    CHECK(determinant(DMat4::scaling({2.0, 3.0, 4.0})) == 24.0);
    CHECK(approxEqual(determinant(DMat4::rotationAxis(normalize(DVec3(1.0, 2.0, 3.0)), 0.7)), 1.0, 1e-12));
    CHECK(determinant(Mat3::scaling({-1.0f, 1.0f, 1.0f})) == -1.0f);
    for (int i = 0; i < 300; ++i) {
        const DMat4 a = randomMatrix(rng), b = randomMatrix(rng);
        CHECK(transpose(transpose(a)) == a);
        CHECK(approxEqual(determinant(a * b), determinant(a) * determinant(b), 1e-9, 1e-9));
        CHECK(approxEqual(determinant(transpose(a)), determinant(a), 1e-12, 1e-12));
        DMat4 inv;
        if (std::fabs(determinant(a)) > 1e-3) {
            REQUIRE(tryInverse(a, inv));
            CHECK(approxEqual(a * inv, DMat4::identity(), 1e-9));
            CHECK(approxEqual(inv * a, DMat4::identity(), 1e-9));
        }
        const DMat3 m3 = a.linear();
        DMat3 inv3;
        if (std::fabs(determinant(m3)) > 1e-3) {
            REQUIRE(tryInverse(m3, inv3));
            CHECK(approxEqual(m3 * inv3, DMat3::identity(), 1e-9));
        }
    }
    // Singular matrices are rejected.
    DMat4 singular = DMat4::identity();
    singular.cols[2] = singular.cols[1];
    DMat4 out = DMat4::zero();
    CHECK_FALSE(tryInverse(singular, out));
    CHECK(out == DMat4::zero());
    DMat3 out3 = DMat3::zero();
    CHECK_FALSE(tryInverse(DMat3::zero(), out3));
}

TEST_CASE("mat: affine and rigid inverse fast paths") {
    Rng rng(33);
    for (int i = 0; i < 300; ++i) {
        const DMat4 m = randomTRS(rng);
        CHECK(approxEqual(inverseAffine(m), inverse(m), 1e-9));
        CHECK(approxEqual(m * inverseAffine(m), DMat4::identity(), 1e-9));
        const DMat4 rigid = DMat4::trs(rng.dvec3(-10.0, 10.0), rng.quatD(), DVec3(1.0));
        CHECK(approxEqual(inverseRigid(rigid), inverse(rigid), 1e-12));
    }
}

TEST_CASE("mat: builders agree with quaternions") {
    for (f32 a : {0.3f, -1.2f, 2.9f}) {
        CHECK(approxEqual(Mat4::rotationX(a), toMat4(Quat::fromAxisAngle(Vec3::unitX(), a)), 1e-6f));
        CHECK(approxEqual(Mat4::rotationY(a), toMat4(Quat::fromAxisAngle(Vec3::unitY(), a)), 1e-6f));
        CHECK(approxEqual(Mat4::rotationZ(a), toMat4(Quat::fromAxisAngle(Vec3::unitZ(), a)), 1e-6f));
        CHECK(approxEqual(Mat4::rotation(Quat::fromAxisAngle(Vec3::unitZ(), a)), Mat4::rotationZ(a), 1e-6f));
    }
    CHECK(approxEqual(transformVector(Mat4::rotationZ(kHalfPi), Vec3::unitX()), Vec3::unitY(), 1e-6f));
    CHECK(approxEqual(transformVector(Mat4::rotationAxis(Vec3::unitY(), kHalfPi), Vec3::unitZ()),
                      Vec3::unitX(), 1e-6f));
    Rng rng(34);
    for (int i = 0; i < 200; ++i) {
        const DVec3 t = rng.dvec3(-10.0, 10.0), s = rng.dvec3(0.5, 2.0);
        const DQuat q = rng.quatD();
        const DMat4 trs = DMat4::trs(t, q, s);
        CHECK(approxEqual(trs, DMat4::translation(t) * DMat4::rotation(q) * DMat4::scaling(s), 1e-12));
        const DVec3 p = rng.dvec3(-3.0, 3.0);
        CHECK(approxEqual(transformPoint(trs, p), t + q * (s * p), 1e-12));
    }
}

TEST_CASE("mat: TRS decomposition round-trips, including mirroring") {
    Rng rng(35);
    for (int i = 0; i < 500; ++i) {
        DVec3 s = rng.dvec3(0.1, 4.0);
        if (i % 3 == 0) s.y = -s.y;  // mirrored matrices are folded into -scale.x
        const DMat4 m = DMat4::trs(rng.dvec3(-50.0, 50.0), rng.quatD(), s);
        DVec3 t, s2;
        DQuat r;
        REQUIRE(decomposeTRS(m, t, r, s2));
        CHECK(approxEqual(DMat4::trs(t, r, s2), m, 1e-9));
        CHECK(isNormalized(r));
        CHECK(s2.y > 0.0);
        CHECK(s2.z > 0.0);
    }
    DVec3 t, s;
    DQuat r;
    CHECK_FALSE(decomposeTRS(DMat4::scaling({1.0, 0.0, 1.0}), t, r, s));
}

TEST_CASE("mat: normal transformation under non-uniform scale") {
    const Mat4 m =
        Mat4::trs({1.0f, 2.0f, 3.0f}, Quat::fromAxisAngle(Vec3::unitY(), 0.4f), {3.0f, 1.0f, 0.5f});
    const Vec3 tangent = normalize(Vec3(1.0f, 1.0f, 0.0f));
    const Vec3 normal = normalize(Vec3(1.0f, -1.0f, 0.0f));
    const Vec3 tt = transformVector(m, tangent);
    const Vec3 nn = transformNormal(m, normal);
    CHECK(std::fabs(dot(tt, nn)) < 1e-6f);
    CHECK(approxEqual(length(nn), 1.0f, 1e-6f));
}

TEST_CASE("mat: lookAt view matrix") {
    const DVec3 eye{10.0, 5.0, -3.0}, target{1.0, 2.0, 3.0};
    const DMat4 v = DMat4::lookAt(eye, target, DVec3::unitY());
    CHECK(approxEqual(transformPoint(v, eye), DVec3{}, 1e-12));
    // The target lies straight ahead on -Z.
    const DVec3 tv = transformPoint(v, target);
    CHECK(approxEqual(tv, DVec3(0.0, 0.0, -distance(eye, target)), 1e-12));
    // World up stays in the view's upper half-plane; view is a rigid transform.
    CHECK(transformVector(v, DVec3::unitY()).y > 0.0);
    CHECK(approxEqual(determinant(v), 1.0, 1e-12));
    // Equivalent to the inverse of the camera's world transform.
    const DQuat camRot = DQuat::lookRotation(target - eye, DVec3::unitY());
    CHECK(approxEqual(v, inverse(DMat4::trs(eye, camRot, DVec3(1.0))), 1e-12));
}

TEST_CASE("mat: reverse-Z infinite perspective (Vulkan depth 0..1)") {
    const f64 fovY = radians(70.0), aspect = 16.0 / 9.0, zn = 0.1;
    const DMat4 p = DMat4::perspectiveReverseZ(fovY, aspect, zn);
    auto ndc = [&](const DVec3& v) { return transformPointProjective(p, v); };
    CHECK(ndc({0.0, 0.0, -zn}).z == 1.0);                        // near plane -> depth 1
    CHECK(approxEqual(ndc({0.0, 0.0, -1e12}).z, 1e-13, 1e-20));  // depth = zNear / distance
    CHECK((p * DVec4(0.0, 0.0, -1.0, 0.0)).z == 0.0);            // point at infinity -> depth 0
    CHECK((p * DVec4(1.0, 2.0, -5.0, 1.0)).w == 5.0);            // w = -z_view
    // Frustum edges map to +-1; +Y is up in clip space.
    const f64 halfH = std::tan(fovY * 0.5) * 10.0, halfW = halfH * aspect;
    CHECK(approxEqual(ndc({halfW, 0.0, -10.0}).x, 1.0, 1e-12));
    CHECK(approxEqual(ndc({0.0, halfH, -10.0}).y, 1.0, 1e-12));
    CHECK(approxEqual(ndc({0.0, -halfH, -10.0}).y, -1.0, 1e-12));
    // Depth decreases monotonically with distance, and stays representable in f32 far out.
    f64 prev = 2.0;
    for (f64 d = zn; d < 1e15; d *= 3.7) {
        const f64 z = ndc({0.0, 0.0, -d}).z;
        CHECK(z < prev);
        CHECK(approxEqual(linearDepthFromReverseZ(z, zn), d, 0.0, 1e-12));
        prev = z;
    }
    const Mat4 pf = Mat4::perspectiveReverseZ(static_cast<f32>(fovY), static_cast<f32>(aspect), 0.1f);
    const f32 z1 = transformPointProjective(pf, Vec3(0.0f, 0.0f, -1.0e6f)).z;
    const f32 z2 = transformPointProjective(pf, Vec3(0.0f, 0.0f, -1.00001e6f)).z;
    CHECK(z1 > z2);  // 10 m apart at 1000 km are still distinguishable in D32F
    CHECK(linearDepthFromReverseZ(0.0f, 0.1f) == kInfinity);
}

TEST_CASE("mat: reverse-Z finite perspective and orthographic") {
    const f64 zn = 0.5, zf = 1000.0;
    const DMat4 p = DMat4::perspectiveReverseZ(radians(60.0), 1.5, zn, zf);
    CHECK(approxEqual(transformPointProjective(p, DVec3(0.0, 0.0, -zn)).z, 1.0, 1e-15));
    CHECK(approxEqual(transformPointProjective(p, DVec3(0.0, 0.0, -zf)).z, 0.0, 1e-15));
    CHECK(transformPointProjective(p, DVec3(0.0, 0.0, -2.0 * zf)).z < 0.0);  // beyond far: clipped
    const DMat4 o = DMat4::orthographicReverseZ(-4.0, 6.0, -1.0, 3.0, 1.0, 101.0);
    CHECK(approxEqual(transformPoint(o, DVec3(-4.0, -1.0, -1.0)), DVec3(-1.0, -1.0, 1.0), 1e-15));
    CHECK(approxEqual(transformPoint(o, DVec3(6.0, 3.0, -101.0)), DVec3(1.0, 1.0, 0.0), 1e-15));
    CHECK(approxEqual(transformPoint(o, DVec3(1.0, 1.0, -51.0)), DVec3(0.0, 0.0, 0.5), 1e-15));
}

TEST_CASE("mat: Vulkan Y flip") {
    const Mat4 p = Mat4::perspectiveReverseZ(1.0f, 1.0f, 0.1f);
    const Mat4 f = flipClipY(p);
    const Vec4 v{0.3f, 0.7f, -2.0f, 1.0f};
    const Vec4 a = p * v, b = f * v;
    CHECK(b.x == a.x);
    CHECK(b.y == -a.y);
    CHECK(b.z == a.z);
    CHECK(b.w == a.w);
    CHECK(flipClipY(f) == p);
}
