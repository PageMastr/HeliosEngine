// helios/math/frame.h — reference-frame math for nested moving/rotating frames (ADR-005).
//
// The universe is a tree of reference frames (galaxy/sector -> star system -> rotating body ->
// ship/station grid -> interior). Every position is stored in the coordinates of the frame it
// lives in (FramePos), so magnitudes stay small and f64 keeps sub-micrometre precision where
// gameplay happens. This header holds only the pure math; the frame graph (parenting, lookup,
// Reparent() bookkeeping) belongs to the world module.
//
// A FrameTransform describes a child frame C relative to its parent P at one instant:
//   p_P = position + rotation * p_C
// plus the rate of change of that relation: the velocity of C's origin and C's angular velocity,
// both measured relative to P and expressed in P's axes.
//
// Frame orientation is f64 (DQuat) rather than f32: a body frame rotates positions that can be
// 10^6..10^9 m from its origin, and an f32 quaternion (~6e-8 rad resolution) would displace a
// point on a 6000 km planet by ~0.4 m. Body orientations inside a frame stay f32 (Quat), like
// DTransform. Velocity conversions implement the transport theorem, so re-parenting an object
// preserves both its world position and its world (inertial) velocity.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/transform.h"

#include <compare>
#include <cstddef>
#include <functional>

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

/// Strongly typed reference-frame handle (assigned by the world module).
struct FrameId {
    static constexpr u32 kInvalidValue = 0xFFFFFFFFu;
    u32 value = kInvalidValue;

    constexpr FrameId() noexcept = default;
    constexpr explicit FrameId(u32 v) noexcept : value(v) {}
    [[nodiscard]] static constexpr FrameId invalid() noexcept { return {}; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return value != kInvalidValue; }
    friend constexpr auto operator<=>(const FrameId&, const FrameId&) noexcept = default;
};
/// By convention the root (galaxy) frame has id 0.
inline constexpr FrameId kRootFrame{0u};

/// A position expressed in a specific frame's coordinates (metres).
struct FramePos {
    FrameId frame{};
    DVec3 local{};
    friend constexpr bool operator==(const FramePos&, const FramePos&) noexcept = default;
};

/// Child frame relative to its parent, including its motion (see file comment).
struct FrameTransform {
    DVec3 position{};         ///< child origin in parent coordinates [m]
    DQuat rotation{};         ///< child axes expressed in parent axes
    DVec3 linearVelocity{};   ///< d(position)/dt, parent axes [m/s]
    DVec3 angularVelocity{};  ///< angular velocity of child relative to parent, parent axes [rad/s]

    [[nodiscard]] static constexpr FrameTransform identity() noexcept { return {}; }
};

/// Pose plus velocities of a body in some frame. Velocities are relative to that frame and
/// expressed in its axes.
struct KinematicState {
    DVec3 position{};
    Quat rotation{};
    DVec3 linearVelocity{};   ///< [m/s]
    DVec3 angularVelocity{};  ///< [rad/s]
};

// ---------------------------------------------------------------------------------------------
// Points and directions.
// ---------------------------------------------------------------------------------------------
/// Child-frame point -> parent-frame point.
[[nodiscard]] constexpr DVec3 pointToParent(const FrameTransform& f, const DVec3& pChild) noexcept {
    return f.position + f.rotation * pChild;
}
/// Parent-frame point -> child-frame point.
[[nodiscard]] constexpr DVec3 pointToChild(const FrameTransform& f, const DVec3& pParent) noexcept {
    return conjugate(f.rotation) * (pParent - f.position);
}
/// Child-frame direction/free vector -> parent axes (rotation only).
[[nodiscard]] constexpr DVec3 vectorToParent(const FrameTransform& f, const DVec3& vChild) noexcept {
    return f.rotation * vChild;
}
[[nodiscard]] constexpr DVec3 vectorToChild(const FrameTransform& f, const DVec3& vParent) noexcept {
    return conjugate(f.rotation) * vParent;
}
/// Velocity (relative to the parent, parent axes) of a point that is fixed in the child frame,
/// e.g. a spot on a rotating planet's surface: v_frame + omega x (R p).
[[nodiscard]] constexpr DVec3 velocityOfFixedPoint(const FrameTransform& f, const DVec3& pChild) noexcept {
    return f.linearVelocity + cross(f.angularVelocity, f.rotation * pChild);
}

// ---------------------------------------------------------------------------------------------
// Poses (position + orientation; scale is frame-invariant and carried through).
// ---------------------------------------------------------------------------------------------
[[nodiscard]] inline DTransform poseToParent(const FrameTransform& f, const DTransform& inChild) noexcept {
    return {pointToParent(f, inChild.position), normalize(toF32(f.rotation * toF64(inChild.rotation))),
            inChild.scale};
}
[[nodiscard]] inline DTransform poseToChild(const FrameTransform& f, const DTransform& inParent) noexcept {
    return {pointToChild(f, inParent.position),
            normalize(toF32(conjugate(f.rotation) * toF64(inParent.rotation))), inParent.scale};
}

// ---------------------------------------------------------------------------------------------
// Full kinematic state (transport theorem).
//   p_P = x + R p_C
//   v_P = v_f + w_f x (R p_C) + R v_C
//   q_P = R q_C
//   w_P = w_f + R w_C
// ---------------------------------------------------------------------------------------------
[[nodiscard]] inline KinematicState stateToParent(const FrameTransform& f, const KinematicState& s) noexcept {
    const DVec3 rp = f.rotation * s.position;
    return {f.position + rp, normalize(toF32(f.rotation * toF64(s.rotation))),
            f.linearVelocity + cross(f.angularVelocity, rp) + f.rotation * s.linearVelocity,
            f.angularVelocity + f.rotation * s.angularVelocity};
}
[[nodiscard]] inline KinematicState stateToChild(const FrameTransform& f, const KinematicState& s) noexcept {
    const DQuat inv = conjugate(f.rotation);
    const DVec3 rel = s.position - f.position;
    return {inv * rel, normalize(toF32(inv * toF64(s.rotation))),
            inv * (s.linearVelocity - f.linearVelocity - cross(f.angularVelocity, rel)),
            inv * (s.angularVelocity - f.angularVelocity)};
}

// ---------------------------------------------------------------------------------------------
// Frame algebra.
// ---------------------------------------------------------------------------------------------
/// C relative to A from B-in-A (`parentInGrand`) and C-in-B (`childInParent`).
[[nodiscard]] inline FrameTransform composeFrames(const FrameTransform& parentInGrand,
                                                  const FrameTransform& childInParent) noexcept {
    const FrameTransform& a = parentInGrand;
    const FrameTransform& b = childInParent;
    const DVec3 rx = a.rotation * b.position;
    return {a.position + rx, normalize(a.rotation * b.rotation),
            a.linearVelocity + cross(a.angularVelocity, rx) + a.rotation * b.linearVelocity,
            a.angularVelocity + a.rotation * b.angularVelocity};
}
/// Parent relative to child (P-in-C) from C-in-P. composeFrames(f, inverseFrame(f)) == identity.
[[nodiscard]] inline FrameTransform inverseFrame(const FrameTransform& f) noexcept {
    const DQuat inv = conjugate(f.rotation);
    // d/dt(-R^T x) = R^T (w x x - v).
    return {-(inv * f.position), inv, inv * (cross(f.angularVelocity, f.position) - f.linearVelocity),
            -(inv * f.angularVelocity)};
}
/// Frame B expressed in frame A, where both are given relative to the same frame P.
[[nodiscard]] inline FrameTransform relativeFrame(const FrameTransform& aInP,
                                                  const FrameTransform& bInP) noexcept {
    return composeFrames(inverseFrame(aInP), bInP);
}
/// Re-expresses a body's state from frame A into frame B (both given relative to a common
/// frame P) such that its position and velocity relative to P — and therefore relative to every
/// ancestor, including the inertial root — are unchanged. This is the math behind Reparent().
[[nodiscard]] inline KinematicState reparent(const KinematicState& inA, const FrameTransform& aInP,
                                             const FrameTransform& bInP) noexcept {
    return stateToChild(bInP, stateToParent(aInP, inA));
}
/// Advances a frame's pose by dt seconds assuming constant linear and angular velocity
/// (exact for that motion model; orbits should be re-evaluated from their own model instead).
[[nodiscard]] inline FrameTransform advanceFrame(const FrameTransform& f, f64 dt) noexcept {
    return {f.position + f.linearVelocity * dt, integrate(f.rotation, f.angularVelocity, dt),
            f.linearVelocity, f.angularVelocity};
}
/// Frame of a body rotating about its local +Y axis (the planet's north pole) at `angularSpeed`
/// rad/s, `angle` radians into its rotation, with its centre at `position`.
[[nodiscard]] inline FrameTransform rotatingBodyFrame(const DVec3& position, const DQuat& axialTilt,
                                                      f64 angle, f64 angularSpeed,
                                                      const DVec3& linearVelocity = {}) noexcept {
    const DVec3 axis = axisY(axialTilt);
    return {position, normalize(axialTilt * DQuat::fromAxisAngle(DVec3::unitY(), angle)), linearVelocity,
            axis * angularSpeed};
}

/// Converts a FramePos from a child frame into its parent frame.
[[nodiscard]] constexpr FramePos toParentFrame(const FramePos& p, FrameId parent,
                                               const FrameTransform& childInParent) noexcept {
    return {parent, pointToParent(childInParent, p.local)};
}
/// Converts a FramePos from a parent frame into one of its child frames.
[[nodiscard]] constexpr FramePos toChildFrame(const FramePos& p, FrameId child,
                                              const FrameTransform& childInParent) noexcept {
    return {child, pointToChild(childInParent, p.local)};
}

}  // namespace helios

namespace std {
/// FrameId is usable as an unordered container key.
template <>
struct hash<helios::FrameId> {
    size_t operator()(const helios::FrameId& id) const noexcept { return hash<helios::u32>{}(id.value); }
};
}  // namespace std

#pragma pop_macro("max")
#pragma pop_macro("min")
