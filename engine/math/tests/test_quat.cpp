#include "test_util.h"

using namespace helios;
using helios::test::Rng;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 tol = 1e-5f) { return approxEqual(a, b, tol); }
bool near(const DVec3& a, const DVec3& b, f64 tol = 1e-12) { return approxEqual(a, b, tol); }
}  // namespace

TEST_CASE("quat: identity and axis-angle follow the right-hand rule") {
    constexpr Quat id;
    static_assert(id.w == 1.0f && id.x == 0.0f);
    CHECK(id * Vec3(1.0f, 2.0f, 3.0f) == Vec3(1.0f, 2.0f, 3.0f));
    // +90° about Z takes X to Y; about X takes Y to Z; about Y takes Z to X.
    CHECK(near(Quat::fromAxisAngle(Vec3::unitZ(), kHalfPi) * Vec3::unitX(), Vec3::unitY()));
    CHECK(near(Quat::fromAxisAngle(Vec3::unitX(), kHalfPi) * Vec3::unitY(), Vec3::unitZ()));
    CHECK(near(Quat::fromAxisAngle(Vec3::unitY(), kHalfPi) * Vec3::unitZ(), Vec3::unitX()));
    const DQuat q = DQuat::fromAxisAngle(normalize(DVec3(1.0, 1.0, 1.0)), kTwoPiD / 3.0);
    CHECK(near(q * DVec3::unitX(), DVec3::unitY()));  // cyclic permutation of the axes
    CHECK(isNormalized(q));
}

TEST_CASE("quat: multiplication order matches matrices") {
    Rng rng(20);
    for (int i = 0; i < 500; ++i) {
        const DQuat a = rng.quatD(), b = rng.quatD();
        const DVec3 v = rng.dvec3(-5.0, 5.0);
        CHECK(near((a * b) * v, a * (b * v), 1e-12));
        CHECK(near(toMat3(a * b) * v, toMat3(a) * (toMat3(b) * v), 1e-12));
        CHECK(near(toMat3(a) * v, a * v, 1e-12));
        CHECK(near(rotate(a, v), a * v));
        CHECK(near(inverseRotate(a, a * v), v, 1e-12));
    }
}

TEST_CASE("quat: conjugate, inverse, normalize") {
    Rng rng(21);
    for (int i = 0; i < 200; ++i) {
        const DQuat q = rng.quatD();
        CHECK(sameRotation(q * conjugate(q), DQuat{}, 1e-7));
        const DQuat s = q * 3.0;  // non-unit
        const DQuat r = s * inverse(s);
        CHECK(approxEqual(r.w, 1.0, 1e-12));
        CHECK(approxEqual(length(normalize(s)), 1.0, 1e-12));
    }
    CHECK(normalize(Quat(0.0f, 0.0f, 0.0f, 0.0f)) == Quat{});
    CHECK(length(Quat(1.0f, 1.0f, 1.0f, 1.0f)) == 2.0f);
}

TEST_CASE("quat: Euler angles use the documented Ry * Rx * Rz order") {
    const f64 pitch = 0.3, yaw = -1.1, roll = 2.0;
    const DQuat e = DQuat::fromEuler({pitch, yaw, roll});
    const DQuat composed = DQuat::fromAxisAngle(DVec3::unitY(), yaw) *
                           DQuat::fromAxisAngle(DVec3::unitX(), pitch) *
                           DQuat::fromAxisAngle(DVec3::unitZ(), roll);
    CHECK(sameRotation(e, composed, 1e-12));
    // Positive pitch looks up; positive yaw turns left (counter-clockwise from above).
    CHECK(forward(Quat::fromEuler({0.3f, 0.0f, 0.0f})).y > 0.0f);
    CHECK(forward(Quat::fromEuler({0.0f, 0.3f, 0.0f})).x < 0.0f);
    CHECK(near(forward(Quat{}), kWorldForward));
    CHECK(near(up(Quat{}), kWorldUp));
    CHECK(near(right(Quat{}), kWorldRight));

    Rng rng(22);
    for (int i = 0; i < 2000; ++i) {
        const DVec3 angles{rng.range(-kHalfPiD + 0.01, kHalfPiD - 0.01), rng.range(-kPiD, kPiD),
                           rng.range(-kPiD, kPiD)};
        const DVec3 back = toEuler(DQuat::fromEuler(angles));
        CHECK(near(back, angles, 1e-9));
        // Any rotation survives a round trip through Euler angles.
        const DQuat q = rng.quatD();
        CHECK(sameRotation(DQuat::fromEuler(toEuler(q)), q, 1e-9));
    }
    // Gimbal lock: pitch = +-90°. Roll folds into yaw; the rotation itself is preserved.
    for (f64 p : {kHalfPiD, -kHalfPiD}) {
        const DQuat q = DQuat::fromEuler({p, 0.4, 0.3});
        const DVec3 e2 = toEuler(q);
        CHECK(approxEqual(e2.x, p, 1e-6));
        CHECK(e2.z == 0.0);
        CHECK(sameRotation(DQuat::fromEuler(e2), q, 1e-6));
    }
}

TEST_CASE("quat: slerp and nlerp") {
    Rng rng(23);
    for (int i = 0; i < 500; ++i) {
        const DQuat a = rng.quatD(), b = rng.quatD();
        CHECK(sameRotation(slerp(a, b, 0.0), a, 1e-7));
        CHECK(sameRotation(slerp(a, b, 1.0), b, 1e-7));
        const f64 total = angleBetween(a, b);
        // Constant angular velocity and shortest path (total <= pi).
        CHECK(total <= kPiD + 1e-12);
        for (f64 t : {0.25, 0.5, 0.8}) {
            const DQuat m = slerp(a, b, t);
            CHECK(approxEqual(angleBetween(a, m), t * total, 1e-7));
            CHECK(isNormalized(m));
        }
        // q and -q are the same rotation: slerp must not take the long way.
        CHECK(sameRotation(slerp(a, -b, 0.5), slerp(a, b, 0.5), 1e-7));
        CHECK(sameRotation(nlerp(a, -b, 0.5), slerp(a, b, 0.5), 1e-7));
        CHECK(approxEqual(angleBetween(a, nlerp(a, b, 0.5)), 0.5 * total, 1e-7));  // symmetric at t=0.5
    }
    // Nearly identical inputs use the nlerp fallback and stay unit length.
    const Quat q = Quat::fromAxisAngle(Vec3::unitY(), 1.0f);
    const Quat r = Quat::fromAxisAngle(Vec3::unitY(), 1.0001f);
    CHECK(isNormalized(slerp(q, r, 0.5f)));
    CHECK(approxEqual(angleBetween(q, slerp(q, r, 0.5f)), 0.00005f, 1e-5f));
}

TEST_CASE("quat: lookRotation and fromTo") {
    CHECK(sameRotation(Quat::lookRotation(kWorldForward), Quat{}, 1e-6f));
    Rng rng(24);
    for (int i = 0; i < 1000; ++i) {
        const DVec3 f = rng.unitD();
        const DQuat q = DQuat::lookRotation(f * 3.0, DVec3::unitY());
        CHECK(near(forward(q), f, 1e-12));
        // Up stays in the plane spanned by world up and forward, on the upper side.
        CHECK(std::fabs(dot(up(q), cross(DVec3::unitY(), f))) < 1e-12);
        if (std::fabs(f.y) < 0.999) CHECK(up(q).y > 0.0);

        const DVec3 a = rng.unitD(), b = rng.unitD();
        CHECK(near(DQuat::fromTo(a, b) * a, b, 1e-12));
        // Shortest arc: rotation angle equals the angle between the vectors.
        CHECK(approxEqual(rotationAngle(DQuat::fromTo(a, b)), angleBetween(a, b), 1e-9));
    }
    // Degenerate: forward parallel to up still gives an orthonormal frame.
    const Quat v = Quat::lookRotation(Vec3(0.0f, 5.0f, 0.0f));
    CHECK(near(forward(v), Vec3::unitY()));
    CHECK(isNormalized(v));
    // Opposite and identical vectors.
    CHECK(near(DQuat::fromTo(DVec3::unitX(), -DVec3::unitX()) * DVec3::unitX(), -DVec3::unitX()));
    CHECK(near(Quat::fromTo(Vec3::unitZ(), -Vec3::unitZ()) * Vec3::unitZ(), -Vec3::unitZ()));
    CHECK(sameRotation(DQuat::fromTo(DVec3::unitY(), DVec3::unitY()), DQuat{}, 1e-12));
}

TEST_CASE("quat: fromBasis covers every Shepperd branch") {
    Rng rng(25);
    const DQuat cases[] = {DQuat{},
                           DQuat::fromAxisAngle(DVec3::unitX(), kPiD),
                           DQuat::fromAxisAngle(DVec3::unitY(), kPiD),
                           DQuat::fromAxisAngle(DVec3::unitZ(), kPiD),
                           DQuat::fromAxisAngle(normalize(DVec3(1.0, 1.0, 0.0)), 3.1),
                           rng.quatD(),
                           rng.quatD()};
    for (const DQuat& q : cases) {
        const DQuat r = DQuat::fromBasis(axisX(q), axisY(q), axisZ(q));
        CHECK(sameRotation(r, q, 1e-9));
        CHECK(r.w >= 0.0);
        CHECK(sameRotation(toQuat(toMat3(q)), q, 1e-9));
    }
}

TEST_CASE("quat: angle, axis-angle and rotation vectors") {
    const DQuat q = DQuat::fromAxisAngle(DVec3::unitZ(), 0.75);
    CHECK(approxEqual(rotationAngle(q), 0.75, 1e-12));
    CHECK(approxEqual(rotationAngle(-q), 0.75, 1e-12));
    CHECK(sameRotation(q, -q));
    CHECK_FALSE(sameRotation(q, DQuat{}, 1e-3));
    DVec3 axis;
    f64 angle = 0.0;
    toAxisAngle(-q, axis, angle);
    CHECK(near(axis, DVec3::unitZ()));
    CHECK(approxEqual(angle, 0.75, 1e-12));
    toAxisAngle(DQuat{}, axis, angle);
    CHECK(angle == 0.0);
    CHECK(axis == DVec3::unitX());

    Rng rng(26);
    for (int i = 0; i < 500; ++i) {
        const DVec3 rv = rng.unitD() * rng.range(0.0, 3.0);
        CHECK(near(toRotationVector(DQuat::fromRotationVector(rv)), rv, 1e-10));
    }
    // Tiny rotation vectors keep full relative precision.
    const DVec3 tiny{1e-12, -2e-12, 3e-12};
    CHECK(near(toRotationVector(DQuat::fromRotationVector(tiny)), tiny, 1e-24));
    CHECK(approxEqual(angleBetween(DQuat{}, DQuat::fromRotationVector(tiny)), length(tiny), 0.0, 1e-9));
}

TEST_CASE("quat: integrate constant angular velocity") {
    const DVec3 omega{0.1, 0.5, -0.2};
    DQuat q{};
    for (int i = 0; i < 1000; ++i) q = integrate(q, omega, 0.01);
    CHECK(sameRotation(q, DQuat::fromRotationVector(omega * 10.0), 1e-9));
    // Rotating body: 24 h at Earth's rate is (almost) one full turn.
    const f64 earthRate = kTwoPiD / 86164.0905;
    DQuat e{};
    for (int i = 0; i < 86164; ++i) e = integrate(e, DVec3(0.0, earthRate, 0.0), 1.0);
    e = integrate(e, DVec3(0.0, earthRate, 0.0), 0.0905);
    CHECK(sameRotation(e, DQuat{}, 1e-9));
}

TEST_CASE("quat: precision conversion") {
    const DQuat d = DQuat::fromAxisAngle(normalize(DVec3(1.0, 2.0, 3.0)), 1.0);
    const Quat f = toF32(d);
    CHECK(sameRotation(toF64(f), d, 1e-6));
    CHECK(Quat(d) == f);
}
