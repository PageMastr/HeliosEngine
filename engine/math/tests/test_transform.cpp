#include "test_util.h"

using namespace helios;
using helios::test::Rng;

namespace {
Transform randomTransform(Rng& rng, bool uniform) {
    const f32 s = rng.rangef(0.3f, 3.0f);
    return {rng.vec3(-50.0f, 50.0f), rng.quat(),
            uniform ? Vec3(s) : Vec3(s, rng.rangef(0.3f, 3.0f), rng.rangef(0.3f, 3.0f))};
}
}  // namespace

TEST_CASE("transform: point/vector mapping matches the matrix") {
    Rng rng(40);
    for (int i = 0; i < 500; ++i) {
        const Transform t = randomTransform(rng, false);
        const Mat4 m = toMatrix(t);
        const Vec3 p = rng.vec3(-5.0f, 5.0f);
        CHECK(approxEqual(transformPoint(t, p), transformPoint(m, p), 1e-3f));
        CHECK(approxEqual(transformVector(t, p), transformVector(m, p), 1e-4f));
        CHECK(approxEqual(length(transformDirection(t, normalize(p))), 1.0f, 1e-6f));
        // Inverse mapping is exact even with non-uniform scale.
        CHECK(approxEqual(inverseTransformPoint(t, transformPoint(t, p)), p, 1e-4f));
        CHECK(approxEqual(inverseTransformVector(t, transformVector(t, p)), p, 1e-4f));
        CHECK(approxEqual(inverseTransformDirection(t, transformDirection(t, p)), p, 1e-4f));
    }
    CHECK(Transform::identity() == Transform{});
    CHECK(toMatrix(Transform{}) == Mat4::identity());
}

TEST_CASE("transform: compose, inverse and relativeTo") {
    Rng rng(41);
    for (int i = 0; i < 500; ++i) {
        const Transform parent = randomTransform(rng, true);
        const Transform local = randomTransform(rng, false);
        const Transform world = compose(parent, local);
        // Composition equals the matrix product when the parent scale is uniform.
        CHECK(approxEqual(toMatrix(world), toMatrix(parent) * toMatrix(local), 2e-3f));
        CHECK(approxEqual(parent * local, world));
        // relativeTo recovers the local transform.
        CHECK(approxEqual(relativeTo(parent, world), local, 1e-3f, 1e-4f, 1e-4f));
        // inverse(t) undoes t for uniform scale.
        CHECK(approxEqual(compose(parent, inverse(parent)), Transform{}, 1e-3f, 1e-4f, 1e-4f));
        CHECK(approxEqual(compose(inverse(parent), parent), Transform{}, 1e-3f, 1e-4f, 1e-4f));
        const Vec3 p = rng.vec3(-5.0f, 5.0f);
        CHECK(approxEqual(transformPoint(inverse(parent), transformPoint(parent, p)), p, 1e-3f));
    }
}

TEST_CASE("transform: interpolation and view matrix") {
    const Transform a{{0.0f, 0.0f, 0.0f}, Quat{}, Vec3(1.0f)};
    const Transform b{{10.0f, 0.0f, 0.0f}, Quat::fromAxisAngle(Vec3::unitY(), 1.0f), Vec3(3.0f)};
    CHECK(approxEqual(interpolate(a, b, 0.0f), a));
    CHECK(approxEqual(interpolate(a, b, 1.0f), b));
    const Transform m = interpolate(a, b, 0.5f);
    CHECK(approxEqual(m.position, Vec3(5.0f, 0.0f, 0.0f)));
    CHECK(approxEqual(angleBetween(a.rotation, m.rotation), 0.5f, 1e-5f));
    CHECK(approxEqual(m.scale, Vec3(2.0f)));

    const Vec3 eye{3.0f, 4.0f, 5.0f}, target{-1.0f, 0.0f, 2.0f};
    const Transform cam{eye, Quat::lookRotation(target - eye), Vec3(1.0f)};
    CHECK(approxEqual(viewMatrix(cam), Mat4::lookAt(eye, target, kWorldUp), 1e-5f));
}

TEST_CASE("transform: DTransform keeps f64 precision far from the origin") {
    const DVec3 far{3.0e12, -1.0e11, 7.5e12};
    const DTransform parent{far, Quat::fromAxisAngle(Vec3::unitY(), 0.3f), Vec3(2.0f)};
    const Transform local{{1.25f, -0.5f, 3.0f}, Quat::fromAxisAngle(Vec3::unitX(), 0.2f), Vec3(1.0f)};
    const DTransform world = compose(parent, local);
    // The child's offset from the parent is exact to well under a millimetre.
    const DVec3 expected = toF64(parent.rotation) * (DVec3(2.0) * toF64(local.position));
    CHECK(approxEqual(world.position - far, expected, 1e-3));
    const Transform back = relativeTo(parent, world);
    CHECK(approxEqual(back, local, 1e-3f, 1e-5f, 1e-6f));  // world.position sits on a 0.5 mm grid
    CHECK(approxEqual(inverseTransformPoint(parent, transformPoint(parent, DVec3(4.0, 5.0, 6.0))),
                      DVec3(4.0, 5.0, 6.0), 1e-3));
    CHECK(approxEqual(transformVector(parent, DVec3(1.0, 0.0, 0.0)),
                      rotationF64(parent) * DVec3(2.0, 0.0, 0.0), 1e-12));
    CHECK(approxEqual(transformDirection(parent, DVec3(0.0, 0.0, 1.0)), rotationF64(parent) * DVec3::unitZ(),
                      1e-12));
    CHECK(approxEqual(length(rotationF64(parent)), 1.0, 1e-15));
    CHECK(approxEqual(inverseTransformVector(parent, transformVector(parent, DVec3(1.0, 2.0, 3.0))),
                      DVec3(1.0, 2.0, 3.0), 1e-12));
    const DMat4 m = toMatrix(parent);
    CHECK(approxEqual(transformPoint(m, DVec3(1.0, 1.0, 1.0)), transformPoint(parent, DVec3(1.0, 1.0, 1.0)),
                      1e-3));
    CHECK(toF64(local).position == toF64(local.position));
    CHECK(toF32(DTransform{DVec3(1.5, 2.5, 3.5), Quat{}, Vec3(1.0f)}).position == Vec3(1.5f, 2.5f, 3.5f));
    const DTransform mid = interpolate(DTransform{far, Quat{}, Vec3(1.0f)},
                                       DTransform{far + DVec3(2.0, 0.0, 0.0), Quat{}, Vec3(1.0f)}, 0.5);
    CHECK(mid.position == far + DVec3(1.0, 0.0, 0.0));
}

TEST_CASE("transform: camera-relative conversion") {
    const DVec3 cam{1.0e12, 2.0e11, -5.0e12};
    // The subtraction is exact in f64, so small offsets survive bit-exactly.
    CHECK(toCameraRelative(cam + DVec3(1.25, -2.5, 3.0), cam) == Vec3(1.25f, -2.5f, 3.0f));
    const DTransform obj{cam + DVec3(10.0, 0.0, -20.0), Quat::fromAxisAngle(Vec3::unitY(), 0.5f), Vec3(2.0f)};
    const Transform rel = toCameraRelative(obj, cam);
    CHECK(rel.position == Vec3(10.0f, 0.0f, -20.0f));
    CHECK(rel.rotation == obj.rotation);
    const Mat4 model = toMatrixCameraRelative(obj, cam);
    const Vec3 local{0.5f, 1.0f, -0.25f};
    // Reference computed relative to the camera in f64. (Going through the absolute world position
    // would itself round to the ~1 mm grid at 5e12 m and be less precise than the tested path.)
    const DVec3 relWorld = (obj.position - cam) + transformVector(obj, toF64(local));
    CHECK(approxEqual(transformPoint(model, local), toF32(relWorld), 1e-5f));

    // Full camera-relative view matches a double-precision view of the true positions.
    const Quat camRot = Quat::lookRotation(Vec3(0.3f, -0.1f, -1.0f));
    const Mat4 view = cameraRelativeView(camRot);
    const Vec3 inView = transformPoint(view * model, local);
    const DVec3 refInView = conjugate(toF64(camRot)) * relWorld;
    CHECK(approxEqual(toF64(inView), refInView, 1e-5));
}
