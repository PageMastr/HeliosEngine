// helios/math/transform.h — TRS transforms and camera-relative conversion (ADR-005).
//
// A transform maps a local point p to   position + rotation * (scale * p)   (scale first, then
// rotate, then translate; identical to Mat4::trs(position, rotation, scale) * p).
//
//   Transform   f32 position — local transforms, render data, anything near its origin.
//   DTransform  f64 position, f32 rotation/scale — poses in a (possibly huge) reference frame.
//
// Large-world rendering: the CPU keeps f64 positions; each frame the renderer subtracts the
// camera origin in f64 and only then rounds to f32 (toCameraRelative / toMatrixCameraRelative).
// The view matrix then contains rotation only (cameraRelativeView), so no large number ever
// reaches the GPU and precision is best right in front of the camera.
//
// Non-uniform scale: TRS composition is not closed under non-uniform scale combined with
// rotation (the product contains shear). compose()/inverse()/relativeTo() are exact when the
// parent's scale is uniform (or its rotation is axis-aligned with the child's); otherwise use
// matrices. inverseTransformPoint() and friends are always exact.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/mat.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

struct Transform {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] static constexpr Transform identity() noexcept { return {}; }
    friend constexpr bool operator==(const Transform&, const Transform&) noexcept = default;
};

struct DTransform {
    DVec3 position{};
    Quat rotation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    [[nodiscard]] static constexpr DTransform identity() noexcept { return {}; }
    friend constexpr bool operator==(const DTransform&, const DTransform&) noexcept = default;
};

// ---------------------------------------------------------------------------------------------
// Transform (f32)
// ---------------------------------------------------------------------------------------------
[[nodiscard]] constexpr Vec3 transformPoint(const Transform& t, const Vec3& p) noexcept {
    return t.position + t.rotation * (t.scale * p);
}
/// Rotates and scales (no translation).
[[nodiscard]] constexpr Vec3 transformVector(const Transform& t, const Vec3& v) noexcept {
    return t.rotation * (t.scale * v);
}
/// Rotates only (for unit directions that must stay unit length).
[[nodiscard]] constexpr Vec3 transformDirection(const Transform& t, const Vec3& d) noexcept {
    return t.rotation * d;
}
/// Exact inverse of transformPoint for any (non-zero) scale.
[[nodiscard]] constexpr Vec3 inverseTransformPoint(const Transform& t, const Vec3& p) noexcept {
    return (conjugate(t.rotation) * (p - t.position)) / t.scale;
}
[[nodiscard]] constexpr Vec3 inverseTransformVector(const Transform& t, const Vec3& v) noexcept {
    return (conjugate(t.rotation) * v) / t.scale;
}
[[nodiscard]] constexpr Vec3 inverseTransformDirection(const Transform& t, const Vec3& d) noexcept {
    return conjugate(t.rotation) * d;
}
/// Local-to-parent matrix.
[[nodiscard]] constexpr Mat4 toMatrix(const Transform& t) noexcept {
    return Mat4::trs(t.position, t.rotation, t.scale);
}

/// World transform of a child given its parent's world transform and its local transform
/// (parent ∘ local). Exact for uniform parent scale.
[[nodiscard]] inline Transform compose(const Transform& parent, const Transform& local) noexcept {
    return {transformPoint(parent, local.position), normalize(parent.rotation * local.rotation),
            parent.scale * local.scale};
}
[[nodiscard]] inline Transform operator*(const Transform& parent, const Transform& local) noexcept {
    return compose(parent, local);
}
/// Inverse transform (exact for uniform scale): compose(t, inverse(t)) == identity.
[[nodiscard]] inline Transform inverse(const Transform& t) noexcept {
    const Quat invRot = conjugate(t.rotation);
    const Vec3 invScale = Vec3(1.0f) / t.scale;
    return {-(invScale * (invRot * t.position)), invRot, invScale};
}
/// Local transform of `world` relative to `parent` (both in the same space), i.e. the child's
/// transform in its parent: compose(parent, relativeTo(parent, world)) == world.
[[nodiscard]] inline Transform relativeTo(const Transform& parent, const Transform& world) noexcept {
    return {inverseTransformPoint(parent, world.position),
            normalize(conjugate(parent.rotation) * world.rotation), world.scale / parent.scale};
}
/// Interpolates position/scale linearly and rotation with shortest-path slerp.
[[nodiscard]] inline Transform interpolate(const Transform& a, const Transform& b, f32 t) noexcept {
    return {lerp(a.position, b.position, t), slerp(a.rotation, b.rotation, t), lerp(a.scale, b.scale, t)};
}
/// View matrix (world -> view) of a camera placed by `camera` (scale ignored).
[[nodiscard]] inline Mat4 viewMatrix(const Transform& camera) noexcept {
    return inverseRigid(Mat4::trs(camera.position, camera.rotation, Vec3(1.0f)));
}
[[nodiscard]] inline bool approxEqual(const Transform& a, const Transform& b, f32 posTol = 1e-4f,
                                      f32 angleTol = 1e-4f, f32 scaleTol = 1e-4f) noexcept {
    return approxEqual(a.position, b.position, posTol) && sameRotation(a.rotation, b.rotation, angleTol) &&
           approxEqual(a.scale, b.scale, scaleTol);
}

// ---------------------------------------------------------------------------------------------
// DTransform (f64 position). The f32 rotation is promoted and renormalized in f64 before use, so
// large local offsets are rotated without the ~1e-7 relative scale error of an f32 quaternion
// that is unit only to f32 precision (0.1 mm per km).
// ---------------------------------------------------------------------------------------------
/// The transform's rotation as a unit f64 quaternion.
[[nodiscard]] inline DQuat rotationF64(const DTransform& t) noexcept { return normalize(toF64(t.rotation)); }
[[nodiscard]] inline DVec3 transformPoint(const DTransform& t, const DVec3& p) noexcept {
    return t.position + rotationF64(t) * (toF64(t.scale) * p);
}
[[nodiscard]] inline DVec3 transformVector(const DTransform& t, const DVec3& v) noexcept {
    return rotationF64(t) * (toF64(t.scale) * v);
}
[[nodiscard]] inline DVec3 transformDirection(const DTransform& t, const DVec3& d) noexcept {
    return rotationF64(t) * d;
}
[[nodiscard]] inline DVec3 inverseTransformPoint(const DTransform& t, const DVec3& p) noexcept {
    return (conjugate(rotationF64(t)) * (p - t.position)) / toF64(t.scale);
}
[[nodiscard]] inline DVec3 inverseTransformVector(const DTransform& t, const DVec3& v) noexcept {
    return (conjugate(rotationF64(t)) * v) / toF64(t.scale);
}
/// Local-to-frame matrix in f64 (CPU-side culling, physics hand-off).
[[nodiscard]] inline DMat4 toMatrix(const DTransform& t) noexcept {
    return DMat4::trs(t.position, rotationF64(t), toF64(t.scale));
}
/// Parent (f64) ∘ local (f32) -> child pose in the parent's frame.
[[nodiscard]] inline DTransform compose(const DTransform& parent, const Transform& local) noexcept {
    return {transformPoint(parent, toF64(local.position)), normalize(parent.rotation * local.rotation),
            parent.scale * local.scale};
}
/// Local (f32) transform of `world` relative to `parent`. The relative offset is computed in f64
/// and rounded once, so it is accurate as long as it is small (children near their parent).
[[nodiscard]] inline Transform relativeTo(const DTransform& parent, const DTransform& world) noexcept {
    return {toF32(inverseTransformPoint(parent, world.position)),
            normalize(conjugate(parent.rotation) * world.rotation), world.scale / parent.scale};
}
[[nodiscard]] constexpr DTransform toF64(const Transform& t) noexcept {
    return {toF64(t.position), t.rotation, t.scale};
}
/// Rounds the position to f32. Only meaningful near the frame origin; prefer toCameraRelative.
[[nodiscard]] constexpr Transform toF32(const DTransform& t) noexcept {
    return {toF32(t.position), t.rotation, t.scale};
}
[[nodiscard]] inline DTransform interpolate(const DTransform& a, const DTransform& b, f64 t) noexcept {
    return {lerp(a.position, b.position, t), slerp(a.rotation, b.rotation, static_cast<f32>(t)),
            lerp(a.scale, b.scale, static_cast<f32>(t))};
}

// ---------------------------------------------------------------------------------------------
// Camera-relative conversion (ADR-005). The subtraction happens in f64 (exact whenever the two
// positions are within a factor of two of each other, by Sterbenz' lemma) and the small result
// is rounded to f32 once, so precision depends only on the distance to the camera, never on the
// distance from the frame origin.
// ---------------------------------------------------------------------------------------------
/// worldPos - cameraOrigin, rounded to f32.
[[nodiscard]] constexpr Vec3 toCameraRelative(const DVec3& worldPos, const DVec3& cameraOrigin) noexcept {
    return toF32(worldPos - cameraOrigin);
}
/// The transform re-expressed relative to the camera origin (rotation/scale unchanged).
[[nodiscard]] constexpr Transform toCameraRelative(const DTransform& t, const DVec3& cameraOrigin) noexcept {
    return {toCameraRelative(t.position, cameraOrigin), t.rotation, t.scale};
}
/// Model matrix for camera-relative rendering: translation = position - cameraOrigin (f64 -> f32).
[[nodiscard]] constexpr Mat4 toMatrixCameraRelative(const DTransform& t, const DVec3& cameraOrigin) noexcept {
    return Mat4::trs(toCameraRelative(t.position, cameraOrigin), t.rotation, t.scale);
}
/// View matrix for camera-relative rendering: the camera sits at the origin, so only its
/// rotation remains. Combine with toMatrixCameraRelative() using the same camera origin.
[[nodiscard]] constexpr Mat4 cameraRelativeView(const Quat& cameraRotation) noexcept {
    return toMat4(conjugate(cameraRotation));
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
