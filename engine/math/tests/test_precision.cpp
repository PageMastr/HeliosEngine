// ADR-005 precision requirement: render and simulate at 10^13 m from the origin without jitter.
//
// Facts this file proves:
//  * An f64 coordinate of magnitude <= 1e13 m lies on a 2^-9 m (1.95 mm) grid, so any position
//    is stored to within 0.98 mm: sub-millimetre.
//  * Camera-relative conversion subtracts in f64 (exact for nearby points, Sterbenz' lemma) and
//    only then rounds the small offset to f32, so an object near a camera at 1e13 m keeps
//    sub-millimetre relative precision end to end, and the full f32 GPU transform chain stays
//    within 1/100 of a pixel of an exact f64 reference.
//  * Naively using f32 world positions fails completely (the f32 grid at 1e13 m is 2^20 m).
//  * Moving together at 1e13 m, independently integrated f64 world positions jitter by up to one
//    grid step (1.95 mm, visible in a cockpit); expressing both in a shared moving frame (frame.h)
//    removes the jitter entirely.
#include "test_util.h"

using namespace helios;
using helios::test::Rng;

namespace {
constexpr f64 kFar = 1.0e13;
constexpr f64 kWidthPx = 3840.0;
constexpr f64 kHeightPx = 2160.0;
}  // namespace

TEST_CASE("precision: f64 resolves positions at 1e13 m to sub-millimetre") {
    const f64 ulp = std::nextafter(kFar, 2.0 * kFar) - kFar;
    CHECK(ulp == std::ldexp(1.0, -9));  // 1.953125 mm
    CHECK(ulp * 0.5 < 1e-3);
    // Largest representation error of any coordinate with |x| <= 1e13 m.
    CHECK(std::nextafter(1.0e13, 0.0) >= 1.0e13 - ulp);
    // Compare: the f32 grid at the same distance is 2^20 m (about 1049 km).
    const f32 farF = static_cast<f32>(kFar);
    CHECK(std::nextafter(farF, kInfinity) - farF == std::ldexp(1.0f, 20));
}

TEST_CASE("precision: camera-relative conversion at 1e13 m keeps sub-millimetre relative precision") {
    Rng rng(110);
    f64 worstConversion = 0.0, worstEndToEnd = 0.0, worstFar = 0.0;
    int naiveFailures = 0;
    constexpr int kN = 20000;
    for (int i = 0; i < kN; ++i) {
        const DVec3 cam = rng.unitD() * kFar;             // stored camera position
        const DVec3 intended = rng.dvec3(-100.0, 100.0);  // object placement relative to the camera
        const DVec3 obj = cam + intended;                 // stored object position (on the f64 grid)

        const Vec3 rel = toCameraRelative(obj, cam);
        // (1) The conversion itself only adds f32 rounding of a <= 100 m offset (<= 3.8 um).
        worstConversion = max(worstConversion, maxComponent(abs(toF64(rel) - (obj - cam))));
        // (2) End to end, including rounding the object onto the f64 grid at 1e13 m.
        worstEndToEnd = max(worstEndToEnd, maxComponent(abs(toF64(rel) - intended)));
        // (3) Objects up to 8 km away still convert to within half a millimetre.
        const DVec3 farObj = cam + rng.dvec3(-8000.0, 8000.0);
        worstFar = max(worstFar, maxComponent(abs(toF64(toCameraRelative(farObj, cam)) - (farObj - cam))));

        // Naive: round both positions to f32 first, then subtract. The offset is lost.
        const Vec3 naive = toF32(obj) - toF32(cam);
        if (maxComponent(abs(toF64(naive) - intended)) > 1.0) ++naiveFailures;
    }
    MESSAGE("camera-relative @1e13 m: conversion error "
            << worstConversion * 1e6 << " um, end-to-end " << worstEndToEnd * 1e3 << " mm, 8 km offsets "
            << worstFar * 1e3 << " mm; naive f32 failed " << naiveFailures << "/" << kN);
    CHECK(worstConversion < 4e-6);
    CHECK(worstEndToEnd < 1e-3);  // sub-millimetre
    CHECK(worstFar < 5e-4);
    CHECK(naiveFailures > kN * 99 / 100);  // naive f32 is off by metres to megametres
}

TEST_CASE("precision: full f32 GPU transform chain at 1e13 m matches an f64 reference") {
    Rng rng(111);
    const f64 fovY = radians(70.0), aspect = kWidthPx / kHeightPx, zNear = 0.05;
    const Mat4 proj =
        Mat4::perspectiveReverseZ(static_cast<f32>(fovY), static_cast<f32>(aspect), static_cast<f32>(zNear));
    const DMat4 projD = DMat4::perspectiveReverseZ(fovY, aspect, zNear);
    f64 worstPx = 0.0, worstDepthRel = 0.0, worstNaivePx = 0.0;
    int naiveBroken = 0;
    constexpr int kN = 5000;
    for (int i = 0; i < kN; ++i) {
        const DVec3 camPos = rng.unitD() * kFar;
        const Quat camRot = rng.quat();
        // An object 0.5..200 m in front of the camera, with a random pose and vertex.
        const f64 dist = rng.range(0.5, 200.0);
        const DVec3 objPos = camPos + toF64(forward(camRot)) * dist + toF64(rng.vec3(-0.2f, 0.2f)) * dist;
        const DTransform obj{objPos, rng.quat(), Vec3(rng.rangef(0.5f, 2.0f))};
        const Vec3 vertex = rng.vec3(-0.1f, 0.1f);

        // GPU path: everything f32, camera-relative model matrix, rotation-only view.
        const Mat4 mvp = proj * cameraRelativeView(camRot) * toMatrixCameraRelative(obj, camPos);
        const Vec4 clip = mvp * Vec4(vertex, 1.0f);

        // Reference: f64 math on the stored positions, subtracting the camera first. (Forming the
        // absolute world-space vertex would round it onto the 1.95 mm grid at 1e13 m.)
        const DVec3 relVertex = (obj.position - camPos) + transformVector(obj, toF64(vertex));
        const DVec3 inView = conjugate(toF64(camRot)) * relVertex;
        const DVec4 clipD = projD * DVec4(inView, 1.0);
        if (clipD.w < zNear) continue;  // vertex behind the near plane

        const f64 ndcX = clip.x / clip.w, ndcY = clip.y / clip.w, depth = clip.z / clip.w;
        const f64 refX = clipD.x / clipD.w, refY = clipD.y / clipD.w, refDepth = clipD.z / clipD.w;
        const f64 px = max(std::fabs(ndcX - refX) * 0.5 * kWidthPx, std::fabs(ndcY - refY) * 0.5 * kHeightPx);
        worstPx = max(worstPx, px);
        worstDepthRel = max(worstDepthRel, std::fabs(depth - refDepth) / refDepth);

        // Naive f32 world-space pipeline: f32 model matrix and f32 lookAt view at 1e13 m.
        const Mat4 naiveModel = toMatrix(toF32(obj));
        const Vec3 camF = toF32(camPos);
        const Mat4 naiveView = Mat4::lookAt(camF, camF + forward(camRot), up(camRot));
        const Vec4 nclip = proj * (naiveView * (naiveModel * Vec4(vertex, 1.0f)));
        const f64 nx = nclip.x / nclip.w, ny = nclip.y / nclip.w;
        if (!std::isfinite(nx) || !std::isfinite(ny) || nclip.w <= 0.0f) {
            ++naiveBroken;
        } else {
            worstNaivePx = max(worstNaivePx, max(std::fabs(nx - refX) * 0.5 * kWidthPx,
                                                 std::fabs(ny - refY) * 0.5 * kHeightPx));
        }
    }
    MESSAGE("GPU chain @1e13 m: worst " << worstPx << " px, depth rel " << worstDepthRel
                                        << "; naive f32: worst " << worstNaivePx << " px, broken "
                                        << naiveBroken);
    CHECK(worstPx < 0.01);                              // jitter-free: 1/100 pixel at 4K
    CHECK(worstDepthRel < 1e-5);                        // reverse-Z depth accurate
    CHECK((worstNaivePx > 1000.0 || naiveBroken > 0));  // naive f32 is unusable
}

TEST_CASE(
    "precision: moving together at 1e13 m - absolute f64 jitters by <= 1 ulp, shared frames are exact") {
    // A ship cruising at 30 km/s far from the origin, with the camera (pilot's eye) and a cockpit
    // display 0.6 m in front of it, simulated at 60 Hz for 10 minutes.
    const DVec3 start = normalize(DVec3(0.3, -0.2, 0.93)) * kFar;
    const DVec3 velocity{12000.0, -3000.0, 27000.0};
    const f64 dt = 1.0 / 60.0;
    const DVec3 camLocal{0.0, 1.2, 0.0}, displayLocal{0.1, 1.0, -0.6};
    const DVec3 trueRel = displayLocal - camLocal;

    DVec3 camWorld = start + camLocal, displayWorld = start + displayLocal;  // absolute f64
    FrameTransform ship{start, DQuat::fromAxisAngle(DVec3::unitY(), 0.3), velocity, {0.0, 0.001, 0.0}};
    f64 worstAbsolute = 0.0, worstFrame = 0.0;
    for (int step = 0; step < 36000; ++step) {
        // Absolute integration: each position is rounded onto the 1.95 mm grid independently.
        camWorld += velocity * dt;
        displayWorld += velocity * dt;
        worstAbsolute = max(worstAbsolute, distance(displayWorld - camWorld, trueRel));

        // Frame-based: the ship frame moves (and turns); crew positions stay ship-local, and the
        // camera-relative offset is computed in the shared frame, never through 1e13 m numbers.
        ship = advanceFrame(ship, dt);
        const DVec3 rel = vectorToParent(ship, displayLocal - camLocal);
        worstFrame = max(worstFrame, std::fabs(length(rel) - length(trueRel)));
    }
    MESSAGE("co-moving at 1e13 m for 10 min: absolute f64 relative jitter "
            << worstAbsolute * 1e3 << " mm, shared frame " << worstFrame * 1e3 << " mm");
    CHECK(worstAbsolute <=
          std::sqrt(3.0) * std::ldexp(1.0, -9) * 1.0001);  // bounded by one grid step per axis
    CHECK(worstFrame < 1e-9);                              // no jitter at all
    // The ship itself has travelled ~17800 km; its absolute pose is still on the 1.95 mm grid.
    CHECK(approxEqual(distance(ship.position, start), length(velocity) * 600.0, 1.0));
    const DVec3 camRebuilt = pointToParent(ship, camLocal);
    CHECK(std::fabs(distance(camRebuilt, ship.position) - length(camLocal)) < 2e-3);
}
