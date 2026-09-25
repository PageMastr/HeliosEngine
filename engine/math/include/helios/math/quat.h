// helios/math/quat.h — unit quaternions for rotations (Quat = f32, DQuat = f64).
//
// Conventions (see README.md):
//  * Hamilton product; q1 * q2 applies q2 first, then q1 (same order as matrices, M * v).
//  * Rotations follow the right-hand rule about their axis (positive angle = counter-clockwise
//    when looking down the axis towards the origin).
//  * Storage order x, y, z, w (vector part first), matching glTF and GPU layouts.
//  * Euler angles are a Vec3 (pitch, yaw, roll) = rotations about (X, Y, Z), composed as
//    R = Ry(yaw) * Rx(pitch) * Rz(roll): roll is applied first, then pitch, then yaw
//    (intrinsic Y-X'-Z''). With the -Z forward convention positive pitch looks up, positive yaw
//    turns left (counter-clockwise seen from above).
//  * Matrix conversions (toMat3/toMat4/toQuat) live in mat.h.
//  * The constructors from angles (fromAxisAngle, fromEuler, fromRotationVector and therefore
//    integrate()) use helios::det trigonometry, so orientations built or integrated from the
//    same inputs are bit-identical on Windows clients and Linux servers (frame.h relies on this).
//    Analysis helpers (slerp, toEuler, rotationAngle, ...) use the CRT and may differ by an ulp.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/vec.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

template <FloatingPoint T>
struct TQuat {
    using ValueType = T;

    T x = T(0);
    T y = T(0);
    T z = T(0);
    T w = T(1);

    /// Identity rotation.
    constexpr TQuat() noexcept = default;
    /// Raw components (not normalized).
    constexpr TQuat(T ax, T ay, T az, T aw) noexcept : x(ax), y(ay), z(az), w(aw) {}
    constexpr TQuat(const TVec3<T>& v, T aw) noexcept : x(v.x), y(v.y), z(v.z), w(aw) {}
    template <FloatingPoint U>
    constexpr explicit TQuat(const TQuat<U>& o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)), w(static_cast<T>(o.w)) {}

    [[nodiscard]] static constexpr TQuat identity() noexcept { return {}; }

    /// Rotation of `angle` radians about unit vector `axis`. Deterministic (det::sinCos).
    [[nodiscard]] static TQuat fromAxisAngle(const TVec3<T>& axis, T angle) noexcept {
        T s = T(0), c = T(1);
        sinCosT(angle * T(0.5), s, c);
        return {axis.x * s, axis.y * s, axis.z * s, c};
    }
    /// Rotation from Euler angles (pitch, yaw, roll) = about (X, Y, Z); R = Ry * Rx * Rz.
    /// Deterministic (det::sinCos).
    [[nodiscard]] static TQuat fromEuler(const TVec3<T>& pitchYawRoll) noexcept {
        const T hp = pitchYawRoll.x * T(0.5), hy = pitchYawRoll.y * T(0.5), hr = pitchYawRoll.z * T(0.5);
        T sp = T(0), cp = T(1), sy = T(0), cy = T(1), sr = T(0), cr = T(1);
        sinCosT(hp, sp, cp);
        sinCosT(hy, sy, cy);
        sinCosT(hr, sr, cr);
        // qYaw * qPitch * qRoll expanded.
        return {cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                cy * cp * cr + sy * sp * sr};
    }
    /// Exponential map: rotation by |v| radians about v / |v| (v = angular velocity * dt).
    /// Accurate for tiny |v| (uses a Taylor expansion of sin(h)/|v| near zero). Deterministic.
    [[nodiscard]] static TQuat fromRotationVector(const TVec3<T>& v) noexcept {
        const T theta2 = dot(v, v);
        const T theta = std::sqrt(theta2);
        T s = T(0), c = T(1);
        sinCosT(theta * T(0.5), s, c);
        // sin(theta/2)/theta, series near zero: 1/2 - theta^2/48.
        const T k = theta2 < T(1e-8) ? T(0.5) - theta2 * (T(1) / T(48)) : s / theta;
        return {v.x * k, v.y * k, v.z * k, c};
    }
    /// Rotation whose X/Y/Z axes are the given orthonormal, right-handed basis vectors (the
    /// columns of the rotation matrix). Shepperd's method; robust for all rotations.
    [[nodiscard]] static TQuat fromBasis(const TVec3<T>& bx, const TVec3<T>& by,
                                         const TVec3<T>& bz) noexcept {
        // m[row][col]: column c is basis vector c.
        const T m00 = bx.x, m10 = bx.y, m20 = bx.z;
        const T m01 = by.x, m11 = by.y, m21 = by.z;
        const T m02 = bz.x, m12 = bz.y, m22 = bz.z;
        const T trace = m00 + m11 + m22;
        TQuat q;
        if (trace > T(0)) {
            const T s = std::sqrt(trace + T(1)) * T(2);
            q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, T(0.25) * s};
        } else if (m00 > m11 && m00 > m22) {
            const T s = std::sqrt(T(1) + m00 - m11 - m22) * T(2);
            q = {T(0.25) * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
        } else if (m11 > m22) {
            const T s = std::sqrt(T(1) + m11 - m00 - m22) * T(2);
            q = {(m01 + m10) / s, T(0.25) * s, (m12 + m21) / s, (m02 - m20) / s};
        } else {
            const T s = std::sqrt(T(1) + m22 - m00 - m11) * T(2);
            q = {(m02 + m20) / s, (m12 + m21) / s, T(0.25) * s, (m10 - m01) / s};
        }
        return q.w < T(0) ? TQuat{-q.x, -q.y, -q.z, -q.w} : q;  // canonical w >= 0
    }
    /// Shortest-arc rotation taking unit vector `from` onto unit vector `to`. For opposite
    /// vectors the result is a half turn about an arbitrary perpendicular axis.
    [[nodiscard]] static TQuat fromTo(const TVec3<T>& from, const TVec3<T>& to) noexcept {
        const T d = dot(from, to);
        if (d < T(-1) + T(sizeof(T) == 4 ? 1e-6 : 1e-14)) {
            const TVec3<T> axis = anyPerpendicular(from);
            return {axis.x, axis.y, axis.z, T(0)};
        }
        const TVec3<T> c = cross(from, to);
        const TQuat q{c.x, c.y, c.z, T(1) + d};
        const T inv = T(1) / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    }
    /// Orientation whose forward (-Z) points along `forward` and whose up (+Y) is as close to
    /// `up` as possible. Falls back to another up axis when forward is parallel to up.
    /// Neither argument needs to be normalized; forward must be non-zero.
    [[nodiscard]] static TQuat lookRotation(const TVec3<T>& forward,
                                            const TVec3<T>& up = TVec3<T>(kWorldUp)) noexcept {
        const TVec3<T> back = -normalize(forward);  // local +Z
        TVec3<T> right = cross(up, back);
        if (lengthSq(right) < T(1e-12) * lengthSq(up)) {
            // forward is parallel to up: pick the world axis least aligned with forward.
            const TVec3<T> alt = abs(back.y) < T(0.9) ? TVec3<T>::unitY() : TVec3<T>::unitZ();
            right = cross(alt, back);
        }
        right = normalize(right);
        const TVec3<T> realUp = cross(back, right);
        return fromBasis(right, realUp, back);
    }

    [[nodiscard]] constexpr TVec3<T> xyz() const noexcept { return {x, y, z}; }

private:
    // det::sinCos in f64, rounded once to T (bit-identical across CRTs and compilers).
    static void sinCosT(T angle, T& s, T& c) noexcept {
        f64 sd = 0.0, cd = 1.0;
        det::sinCos(static_cast<f64>(angle), sd, cd);
        s = static_cast<T>(sd);
        c = static_cast<T>(cd);
    }

public:
    /// Hamilton product: (a * b) rotates by b first, then by a.
    friend constexpr TQuat operator*(const TQuat& a, const TQuat& b) noexcept {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    constexpr TQuat& operator*=(const TQuat& b) noexcept { return *this = *this * b; }
    /// Rotates v by q (q must be unit length).
    friend constexpr TVec3<T> operator*(const TQuat& q, const TVec3<T>& v) noexcept {
        // v' = v + w*t + u x t with t = 2 (u x v); 15 mul + 15 add.
        const TVec3<T> u{q.x, q.y, q.z};
        const TVec3<T> t = cross(u, v) * T(2);
        return v + t * q.w + cross(u, t);
    }
    // Linear-space operations (used for blending/nlerp; results are generally not unit length).
    friend constexpr TQuat operator+(const TQuat& a, const TQuat& b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr TQuat operator-(const TQuat& a, const TQuat& b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr TQuat operator*(const TQuat& a, T s) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    friend constexpr TQuat operator*(T s, const TQuat& a) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    /// Component negation. -q represents the same rotation as q.
    friend constexpr TQuat operator-(const TQuat& a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
    /// Exact component equality (q and -q compare unequal; use sameRotation for that).
    friend constexpr bool operator==(const TQuat& a, const TQuat& b) noexcept = default;
};

using Quat = TQuat<f32>;
using DQuat = TQuat<f64>;
static_assert(sizeof(Quat) == 16 && sizeof(DQuat) == 32);
static_assert(std::is_trivially_copyable_v<Quat> && std::is_standard_layout_v<Quat>);

template <FloatingPoint T>
[[nodiscard]] constexpr TQuat<f32> toF32(const TQuat<T>& q) noexcept {
    return TQuat<f32>(q);
}
template <FloatingPoint T>
[[nodiscard]] constexpr TQuat<f64> toF64(const TQuat<T>& q) noexcept {
    return TQuat<f64>(q);
}

/// 4D dot product (cosine of half the angle between unit quaternions, up to sign).
template <FloatingPoint T>
[[nodiscard]] constexpr T dot(const TQuat<T>& a, const TQuat<T>& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
template <FloatingPoint T>
[[nodiscard]] constexpr T lengthSq(const TQuat<T>& q) noexcept {
    return dot(q, q);
}
template <FloatingPoint T>
[[nodiscard]] inline T length(const TQuat<T>& q) noexcept {
    return std::sqrt(dot(q, q));
}
/// Unit quaternion; returns identity for a (near) zero input.
template <FloatingPoint T>
[[nodiscard]] inline TQuat<T> normalize(const TQuat<T>& q) noexcept {
    const T inv = safeRsqrt(dot(q, q));
    return inv > T(0) ? q * inv : TQuat<T>{};
}
/// Conjugate (x, y, z negated). Equals the inverse for unit quaternions.
template <FloatingPoint T>
[[nodiscard]] constexpr TQuat<T> conjugate(const TQuat<T>& q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}
/// Inverse of an arbitrary non-zero quaternion.
template <FloatingPoint T>
[[nodiscard]] constexpr TQuat<T> inverse(const TQuat<T>& q) noexcept {
    return conjugate(q) * (T(1) / dot(q, q));
}
/// Rotates v by unit quaternion q (same as q * v).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> rotate(const TQuat<T>& q, const TVec3<T>& v) noexcept {
    return q * v;
}
/// Rotates v by the inverse of unit quaternion q.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> inverseRotate(const TQuat<T>& q, const TVec3<T>& v) noexcept {
    return conjugate(q) * v;
}
/// The rotated local axes of q (columns of its rotation matrix).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> axisX(const TQuat<T>& q) noexcept {
    return {T(1) - T(2) * (q.y * q.y + q.z * q.z), T(2) * (q.x * q.y + q.w * q.z),
            T(2) * (q.x * q.z - q.w * q.y)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> axisY(const TQuat<T>& q) noexcept {
    return {T(2) * (q.x * q.y - q.w * q.z), T(1) - T(2) * (q.x * q.x + q.z * q.z),
            T(2) * (q.y * q.z + q.w * q.x)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> axisZ(const TQuat<T>& q) noexcept {
    return {T(2) * (q.x * q.z + q.w * q.y), T(2) * (q.y * q.z - q.w * q.x),
            T(1) - T(2) * (q.x * q.x + q.y * q.y)};
}
/// Local forward (-Z) of an orientation, in the parent space.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> forward(const TQuat<T>& q) noexcept {
    return -axisZ(q);
}
/// Local up (+Y) of an orientation, in the parent space.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> up(const TQuat<T>& q) noexcept {
    return axisY(q);
}
/// Local right (+X) of an orientation, in the parent space.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> right(const TQuat<T>& q) noexcept {
    return axisX(q);
}

/// Normalized linear interpolation along the shortest path. Fast; angular velocity is not
/// constant (use slerp when that matters).
template <FloatingPoint T>
[[nodiscard]] inline TQuat<T> nlerp(const TQuat<T>& a, const TQuat<T>& b, T t) noexcept {
    const TQuat<T> bb = dot(a, b) < T(0) ? -b : b;
    return normalize(a + (bb - a) * t);
}
/// Spherical linear interpolation along the shortest path (constant angular velocity). Inputs
/// must be unit quaternions. Falls back to nlerp when the quaternions are nearly identical.
template <FloatingPoint T>
[[nodiscard]] inline TQuat<T> slerp(const TQuat<T>& a, const TQuat<T>& b, T t) noexcept {
    T d = dot(a, b);
    TQuat<T> bb = b;
    if (d < T(0)) {
        d = -d;
        bb = -b;
    }
    if (d > T(0.9995)) return normalize(a + (bb - a) * t);
    // theta via atan2 for accuracy; sin(theta) >= ~0.03 here so the division is safe.
    const T sinTheta = std::sqrt(max(T(0), T(1) - d * d));
    const T theta = std::atan2(sinTheta, d);
    const T wa = std::sin((T(1) - t) * theta) / sinTheta;
    const T wb = std::sin(t * theta) / sinTheta;
    return normalize(a * wa + bb * wb);
}
/// Rotation angle of q in [0, pi] (the shortest equivalent rotation).
template <FloatingPoint T>
[[nodiscard]] inline T rotationAngle(const TQuat<T>& q) noexcept {
    const T s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return T(2) * std::atan2(s, abs(q.w));
}
/// Angle in [0, pi] of the rotation taking orientation a to orientation b.
template <FloatingPoint T>
[[nodiscard]] inline T angleBetween(const TQuat<T>& a, const TQuat<T>& b) noexcept {
    return rotationAngle(conjugate(a) * b);
}
/// True if a and b represent the same rotation within `angleTol` radians (q and -q are equal).
template <FloatingPoint T>
[[nodiscard]] inline bool sameRotation(const TQuat<T>& a, const TQuat<T>& b, T angleTol = T(1e-4)) noexcept {
    return angleBetween(a, b) <= angleTol;
}
/// Axis and angle of a unit quaternion; angle in [0, pi]. Identity yields axis +X, angle 0.
template <FloatingPoint T>
inline void toAxisAngle(const TQuat<T>& q, TVec3<T>& axis, T& angle) noexcept {
    const TQuat<T> c = q.w < T(0) ? -q : q;
    const T s = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
    angle = T(2) * std::atan2(s, c.w);
    axis = s > T(0) ? TVec3<T>{c.x / s, c.y / s, c.z / s} : TVec3<T>::unitX();
}
/// Logarithmic map: rotation vector (axis * angle, angle in [0, pi]). Inverse of fromRotationVector.
template <FloatingPoint T>
[[nodiscard]] inline TVec3<T> toRotationVector(const TQuat<T>& q) noexcept {
    TVec3<T> axis;
    T angle;
    toAxisAngle(q, axis, angle);
    return axis * angle;
}
/// Euler angles (pitch, yaw, roll) with pitch in [-pi/2, pi/2]; inverse of TQuat::fromEuler.
/// At gimbal lock (|pitch| == pi/2) roll is set to 0 and yaw absorbs the combined rotation.
template <FloatingPoint T>
[[nodiscard]] inline TVec3<T> toEuler(const TQuat<T>& q) noexcept {
    // Elements of R = Ry * Rx * Rz: m12 = -sin(pitch), m02/m22 give yaw, m10/m11 give roll.
    const T m12 = T(2) * (q.y * q.z - q.w * q.x);
    const T sp = clamp(-m12, T(-1), T(1));
    const T pitch = std::asin(sp);
    if (abs(sp) < T(1) - T(sizeof(T) == 4 ? 1e-6 : 1e-12)) {
        const T m02 = T(2) * (q.x * q.z + q.w * q.y);
        const T m22 = T(1) - T(2) * (q.x * q.x + q.y * q.y);
        const T m10 = T(2) * (q.x * q.y + q.w * q.z);
        const T m11 = T(1) - T(2) * (q.x * q.x + q.z * q.z);
        return {pitch, std::atan2(m02, m22), std::atan2(m10, m11)};
    }
    const T m00 = T(1) - T(2) * (q.y * q.y + q.z * q.z);
    const T m20 = T(2) * (q.x * q.z - q.w * q.y);
    return {pitch, std::atan2(-m20, m00), T(0)};
}
/// Integrates an orientation by a constant angular velocity (radians/s, expressed in the same
/// space as q, i.e. world/parent space) over dt seconds. Exact for constant omega.
template <FloatingPoint T>
[[nodiscard]] inline TQuat<T> integrate(const TQuat<T>& q, const TVec3<T>& angularVelocity, T dt) noexcept {
    return normalize(TQuat<T>::fromRotationVector(angularVelocity * dt) * q);
}
/// True if |length(q) - 1| <= tol.
template <FloatingPoint T>
[[nodiscard]] inline bool isNormalized(const TQuat<T>& q, T tol = T(1e-4)) noexcept {
    return abs(lengthSq(q) - T(1)) <= T(2) * tol;
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
