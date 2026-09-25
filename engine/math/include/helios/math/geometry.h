// helios/math/geometry.h — bounding volumes, primitives and intersection tests.
//
// All types are templated on the scalar: f32 aliases (AABB, Sphere, Plane, Ray, OBB, Frustum)
// for camera-relative/local work and f64 aliases (DAABB, DSphere, ...) for frame-space CPU
// culling (ADR-003: CPU culling in f64).
//
// Conventions:
//  * Plane: dot(normal, p) + d = signed distance (positive on the side the normal points to).
//    Frustum planes point inwards.
//  * Ray casts return the parametric interval [tEnter, tExit] of the ray (t >= 0) inside the
//    volume, in units of the ray direction's length (unit direction => metres). tEnter is 0 when
//    the origin is inside.
//  * Triangles are counter-clockwise when seen from their front (normal = cross(b-a, c-a)).
//  * Touching counts as intersecting.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/mat.h"

#include <cstddef>

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

// ---------------------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------------------
template <FloatingPoint T>
struct TAABB {
    TVec3<T> min{kInfinityT<T>};  ///< default-constructed boxes are empty (min > max)
    TVec3<T> max{-kInfinityT<T>};

    [[nodiscard]] static constexpr TAABB empty() noexcept { return {}; }
    [[nodiscard]] static constexpr TAABB fromMinMax(const TVec3<T>& lo, const TVec3<T>& hi) noexcept {
        return {lo, hi};
    }
    [[nodiscard]] static constexpr TAABB fromCenterExtents(const TVec3<T>& c,
                                                           const TVec3<T>& halfExtents) noexcept {
        return {c - halfExtents, c + halfExtents};
    }
    [[nodiscard]] static constexpr TAABB fromPoints(const TVec3<T>* points, std::size_t count) noexcept {
        TAABB b;
        for (std::size_t i = 0; i < count; ++i) b.expand(points[i]);
        return b;
    }

    [[nodiscard]] constexpr bool isEmpty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }
    [[nodiscard]] constexpr TVec3<T> center() const noexcept { return (min + max) * T(0.5); }
    /// Half size.
    [[nodiscard]] constexpr TVec3<T> extents() const noexcept { return (max - min) * T(0.5); }
    [[nodiscard]] constexpr TVec3<T> size() const noexcept { return max - min; }
    [[nodiscard]] constexpr T surfaceArea() const noexcept {
        const TVec3<T> s = size();
        return T(2) * (s.x * s.y + s.y * s.z + s.z * s.x);
    }
    [[nodiscard]] constexpr T volume() const noexcept {
        const TVec3<T> s = size();
        return s.x * s.y * s.z;
    }
    constexpr void expand(const TVec3<T>& p) noexcept {
        min = helios::min(min, p);
        max = helios::max(max, p);
    }
    constexpr void expand(const TAABB& b) noexcept {
        min = helios::min(min, b.min);
        max = helios::max(max, b.max);
    }
    /// Grows the box by `margin` on every side.
    [[nodiscard]] constexpr TAABB inflated(T margin) const noexcept {
        return {min - TVec3<T>(margin), max + TVec3<T>(margin)};
    }
    [[nodiscard]] constexpr bool contains(const TVec3<T>& p) const noexcept {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
    [[nodiscard]] constexpr bool contains(const TAABB& b) const noexcept {
        return contains(b.min) && contains(b.max);
    }
    friend constexpr bool operator==(const TAABB&, const TAABB&) noexcept = default;
};

template <FloatingPoint T>
struct TSphere {
    TVec3<T> center{};
    T radius = T(0);

    [[nodiscard]] constexpr bool contains(const TVec3<T>& p) const noexcept {
        return distanceSq(center, p) <= radius * radius;
    }
    friend constexpr bool operator==(const TSphere&, const TSphere&) noexcept = default;
};

template <FloatingPoint T>
struct TPlane {
    TVec3<T> normal{T(0), T(1), T(0)};
    T d = T(0);

    /// Plane through `point` with unit `normal`.
    [[nodiscard]] static constexpr TPlane fromPointNormal(const TVec3<T>& point, const TVec3<T>& n) noexcept {
        return {n, -dot(n, point)};
    }
    /// Plane through three points; the normal faces the side from which a, b, c appear
    /// counter-clockwise. Precondition: points are not collinear.
    [[nodiscard]] static TPlane fromPoints(const TVec3<T>& a, const TVec3<T>& b, const TVec3<T>& c) noexcept {
        return fromPointNormal(a, normalize(cross(b - a, c - a)));
    }
    /// Same plane with a unit normal. Precondition: normal is non-zero.
    [[nodiscard]] TPlane normalized() const noexcept {
        const T inv = T(1) / length(normal);
        return {normal * inv, d * inv};
    }
    /// Signed distance (exact distance when the normal is unit length).
    [[nodiscard]] constexpr T signedDistance(const TVec3<T>& p) const noexcept { return dot(normal, p) + d; }
    friend constexpr bool operator==(const TPlane&, const TPlane&) noexcept = default;
};

template <FloatingPoint T>
struct TRay {
    TVec3<T> origin{};
    TVec3<T> direction{T(0), T(0), T(-1)};

    [[nodiscard]] constexpr TVec3<T> at(T t) const noexcept { return origin + direction * t; }
};

/// Oriented box: center, half extents along its local axes and the rotation of those axes.
template <FloatingPoint T>
struct TOBB {
    TVec3<T> center{};
    TVec3<T> halfExtents{};
    TQuat<T> rotation{};

    /// World-space axis i (0..2).
    [[nodiscard]] constexpr TVec3<T> axis(int i) const noexcept {
        return i == 0 ? axisX(rotation) : (i == 1 ? axisY(rotation) : axisZ(rotation));
    }
    /// The tightest AABB enclosing this box.
    [[nodiscard]] constexpr TAABB<T> bounds() const noexcept {
        const TMat3<T> r = toMat3(rotation);
        const TVec3<T> e =
            abs(r.cols[0]) * halfExtents.x + abs(r.cols[1]) * halfExtents.y + abs(r.cols[2]) * halfExtents.z;
        return {center - e, center + e};
    }
    [[nodiscard]] constexpr bool contains(const TVec3<T>& p) const noexcept {
        const TVec3<T> l = conjugate(rotation) * (p - center);
        return abs(l.x) <= halfExtents.x && abs(l.y) <= halfExtents.y && abs(l.z) <= halfExtents.z;
    }
};

enum class Containment : u8 { Outside, Intersects, Inside };

enum class FrustumPlane : int { Left = 0, Right, Bottom, Top, Near, Far };

template <FloatingPoint T>
struct TFrustum {
    /// Inward-facing unit planes, indexed by FrustumPlane.
    TPlane<T> planes[6]{};

    /// Extracts the planes of `viewProj` (clip = viewProj * p) under the Helios conventions:
    /// clip depth in [0, 1] with reverse-Z (near: z <= w, far: z >= 0). With an infinite
    /// projection the far plane is degenerate and is stored as {0, 0, 0, 1} (never culls).
    /// Works for the Y-flipped variant as well (Bottom/Top swap roles).
    [[nodiscard]] static TFrustum fromMatrix(const TMat4<T>& viewProj) noexcept {
        const TVec4<T> r0 = viewProj.row(0), r1 = viewProj.row(1), r2 = viewProj.row(2), r3 = viewProj.row(3);
        const TVec4<T> raw[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 - r2, r2};
        TFrustum f;
        for (int i = 0; i < 6; ++i) {
            const TVec3<T> n = raw[i].xyz();
            const T len = length(n);
            // A zero normal only occurs for the far plane of an infinite projection.
            f.planes[i] = len > std::numeric_limits<T>::min() * T(1e4) ? TPlane<T>{n / len, raw[i].w / len}
                                                                       : TPlane<T>{TVec3<T>{}, T(1)};
        }
        return f;
    }
    [[nodiscard]] constexpr const TPlane<T>& plane(FrustumPlane p) const noexcept {
        return planes[static_cast<int>(p)];
    }

    [[nodiscard]] constexpr bool contains(const TVec3<T>& p) const noexcept {
        for (const TPlane<T>& pl : planes) {
            if (pl.signedDistance(p) < T(0)) return false;
        }
        return true;
    }
    /// Conservative sphere test: false only if the sphere is certainly outside.
    [[nodiscard]] constexpr bool intersects(const TSphere<T>& s) const noexcept {
        for (const TPlane<T>& pl : planes) {
            if (pl.signedDistance(s.center) < -s.radius) return false;
        }
        return true;
    }
    /// Conservative AABB test (p-vertex method): false only if the box is certainly outside.
    [[nodiscard]] constexpr bool intersects(const TAABB<T>& b) const noexcept {
        for (const TPlane<T>& pl : planes) {
            const TVec3<T> pv{pl.normal.x >= T(0) ? b.max.x : b.min.x,
                              pl.normal.y >= T(0) ? b.max.y : b.min.y,
                              pl.normal.z >= T(0) ? b.max.z : b.min.z};
            if (pl.signedDistance(pv) < T(0)) return false;
        }
        return true;
    }
    [[nodiscard]] constexpr Containment classify(const TSphere<T>& s) const noexcept {
        Containment result = Containment::Inside;
        for (const TPlane<T>& pl : planes) {
            const T dist = pl.signedDistance(s.center);
            if (dist < -s.radius) return Containment::Outside;
            if (dist < s.radius) result = Containment::Intersects;
        }
        return result;
    }
    [[nodiscard]] constexpr Containment classify(const TAABB<T>& b) const noexcept {
        Containment result = Containment::Inside;
        for (const TPlane<T>& pl : planes) {
            const bool px = pl.normal.x >= T(0), py = pl.normal.y >= T(0), pz = pl.normal.z >= T(0);
            const TVec3<T> pv{px ? b.max.x : b.min.x, py ? b.max.y : b.min.y, pz ? b.max.z : b.min.z};
            const TVec3<T> nv{px ? b.min.x : b.max.x, py ? b.min.y : b.max.y, pz ? b.min.z : b.max.z};
            if (pl.signedDistance(pv) < T(0)) return Containment::Outside;
            if (pl.signedDistance(nv) < T(0)) result = Containment::Intersects;
        }
        return result;
    }
};

using AABB = TAABB<f32>;
using DAABB = TAABB<f64>;
using Sphere = TSphere<f32>;
using DSphere = TSphere<f64>;
using Plane = TPlane<f32>;
using DPlane = TPlane<f64>;
using Ray = TRay<f32>;
using DRay = TRay<f64>;
using OBB = TOBB<f32>;
using DOBB = TOBB<f64>;
using Frustum = TFrustum<f32>;
using DFrustum = TFrustum<f64>;

// ---------------------------------------------------------------------------------------------
// Closest points and distances
// ---------------------------------------------------------------------------------------------
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPoint(const TAABB<T>& b, const TVec3<T>& p) noexcept {
    return clamp(p, b.min, b.max);
}
template <FloatingPoint T>
[[nodiscard]] constexpr T distanceSq(const TAABB<T>& b, const TVec3<T>& p) noexcept {
    return distanceSq(closestPoint(b, p), p);
}
/// Closest point on the sphere's surface (or the center itself if p == center).
template <FloatingPoint T>
[[nodiscard]] inline TVec3<T> closestPoint(const TSphere<T>& s, const TVec3<T>& p) noexcept {
    return s.center + normalizeOr(p - s.center, TVec3<T>::unitY()) * s.radius;
}
/// Orthogonal projection onto a plane with a unit normal.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPoint(const TPlane<T>& pl, const TVec3<T>& p) noexcept {
    return p - pl.normal * pl.signedDistance(p);
}
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPoint(const TOBB<T>& b, const TVec3<T>& p) noexcept {
    const TVec3<T> l = conjugate(b.rotation) * (p - b.center);
    return b.center + b.rotation * clamp(l, -b.halfExtents, b.halfExtents);
}
/// Closest point on segment [a, b]; `t` receives the parameter in [0, 1].
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPointOnSegment(const TVec3<T>& a, const TVec3<T>& b,
                                                       const TVec3<T>& p, T& t) noexcept {
    const TVec3<T> ab = b - a;
    const T len2 = dot(ab, ab);
    t = len2 > T(0) ? saturate(dot(p - a, ab) / len2) : T(0);
    return a + ab * t;
}
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPointOnSegment(const TVec3<T>& a, const TVec3<T>& b,
                                                       const TVec3<T>& p) noexcept {
    T t = T(0);
    return closestPointOnSegment(a, b, p, t);
}
/// Closest point on the ray (t >= 0).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPointOnRay(const TRay<T>& r, const TVec3<T>& p) noexcept {
    const T len2 = dot(r.direction, r.direction);
    const T t = len2 > T(0) ? max(T(0), dot(p - r.origin, r.direction) / len2) : T(0);
    return r.at(t);
}
/// Closest point on triangle (a, b, c) to p, by Voronoi-region classification (Ericson,
/// Real-Time Collision Detection, 5.1.5). Handles degenerate triangles.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> closestPointOnTriangle(const TVec3<T>& a, const TVec3<T>& b,
                                                        const TVec3<T>& c, const TVec3<T>& p) noexcept {
    const TVec3<T> ab = b - a, ac = c - a, ap = p - a;
    const T d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= T(0) && d2 <= T(0)) return a;
    const TVec3<T> bp = p - b;
    const T d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= T(0) && d4 <= d3) return b;
    const T vc = d1 * d4 - d3 * d2;
    if (vc <= T(0) && d1 >= T(0) && d3 <= T(0)) return a + ab * (d1 / (d1 - d3));
    const TVec3<T> cp = p - c;
    const T d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= T(0) && d5 <= d6) return c;
    const T vb = d5 * d2 - d1 * d6;
    if (vb <= T(0) && d2 >= T(0) && d6 <= T(0)) return a + ac * (d2 / (d2 - d6));
    const T va = d3 * d6 - d5 * d4;
    if (va <= T(0) && (d4 - d3) >= T(0) && (d5 - d6) >= T(0)) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const T sum = va + vb + vc;
    if (!(sum > T(0))) return a;  // degenerate (zero-area) triangle
    const T denom = T(1) / sum;
    return a + ab * (vb * denom) + ac * (vc * denom);
}

// ---------------------------------------------------------------------------------------------
// Triangle helpers
// ---------------------------------------------------------------------------------------------
/// Unnormalized normal cross(b - a, c - a) (length = 2 * area).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> triangleNormal(const TVec3<T>& a, const TVec3<T>& b,
                                                const TVec3<T>& c) noexcept {
    return cross(b - a, c - a);
}
template <FloatingPoint T>
[[nodiscard]] inline T triangleArea(const TVec3<T>& a, const TVec3<T>& b, const TVec3<T>& c) noexcept {
    return T(0.5) * length(triangleNormal(a, b, c));
}
/// Barycentric coordinates (u, v, w) of p's projection onto the triangle's plane:
/// p ≈ u*a + v*b + w*c, u + v + w = 1. Returns false for degenerate triangles.
template <FloatingPoint T>
[[nodiscard]] constexpr bool barycentric(const TVec3<T>& a, const TVec3<T>& b, const TVec3<T>& c,
                                         const TVec3<T>& p, TVec3<T>& uvw) noexcept {
    const TVec3<T> v0 = b - a, v1 = c - a, v2 = p - a;
    const T d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
    const T den = d00 * d11 - d01 * d01;
    if (!(abs(den) > T(0))) return false;
    const T v = (d11 * d20 - d01 * d21) / den;
    const T w = (d00 * d21 - d01 * d20) / den;
    uvw = {T(1) - v - w, v, w};
    return true;
}

// ---------------------------------------------------------------------------------------------
// Ray casts
// ---------------------------------------------------------------------------------------------
/// Slab test. Robust for zero direction components (IEEE infinities; NaN from 0*inf on a slab
/// boundary is ignored, i.e. treated as touching).
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersectRayAABB(const TRay<T>& ray, const TAABB<T>& box, T& tEnter,
                                              T& tExit) noexcept {
    T tmin = T(0);
    T tmax = kInfinityT<T>;
    for (int i = 0; i < 3; ++i) {
        const T invD = T(1) / ray.direction[i];
        T t0 = (box.min[i] - ray.origin[i]) * invD;
        T t1 = (box.max[i] - ray.origin[i]) * invD;
        if (invD < T(0)) {
            const T tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        tmin = t0 > tmin ? t0 : tmin;
        tmax = t1 < tmax ? t1 : tmax;
        if (tmax < tmin) return false;
    }
    tEnter = tmin;
    tExit = tmax;
    return true;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TRay<T>& ray, const TAABB<T>& box) noexcept {
    T a = T(0), b = T(0);
    return intersectRayAABB(ray, box, a, b);
}
/// Ray vs sphere with the cancellation-free discriminant of Haines et al. (Ray Tracing Gems,
/// ch. 7), which stays accurate for small spheres far from the ray origin.
template <FloatingPoint T>
[[nodiscard]] inline bool intersectRaySphere(const TRay<T>& ray, const TSphere<T>& s, T& tEnter,
                                             T& tExit) noexcept {
    const TVec3<T> m = ray.origin - s.center;
    const TVec3<T>& d = ray.direction;
    const T a = dot(d, d);
    const T b = dot(m, d);
    const T r2 = s.radius * s.radius;
    const T c = dot(m, m) - r2;
    if (!(a > T(0))) return false;
    if (c > T(0) && b > T(0)) return false;  // outside and pointing away
    const TVec3<T> l = m - d * (b / a);
    const T discr = a * (r2 - dot(l, l));  // == b^2 - a*c without cancellation
    if (discr < T(0)) return false;
    const T sq = std::sqrt(discr);
    const T q = b > T(0) ? -b - sq : -b + sq;
    T t0 = q / a;
    T t1 = q != T(0) ? c / q : t0;
    if (t0 > t1) {
        const T tmp = t0;
        t0 = t1;
        t1 = tmp;
    }
    if (t1 < T(0)) return false;
    tEnter = t0 > T(0) ? t0 : T(0);
    tExit = t1;
    return true;
}
/// Ray vs plane. Returns false if the ray is parallel to the plane or the hit is behind it.
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersectRayPlane(const TRay<T>& ray, const TPlane<T>& pl, T& t) noexcept {
    const T denom = dot(pl.normal, ray.direction);
    if (!(abs(denom) > std::numeric_limits<T>::min())) return false;
    const T tt = -pl.signedDistance(ray.origin) / denom;
    if (tt < T(0)) return false;
    t = tt;
    return true;
}
/// Möller–Trumbore ray/triangle test. Outputs the ray parameter t and barycentrics (u, v) of
/// the hit (hit = (1-u-v)*a + u*b + v*c). With cullBackFaces, only counter-clockwise (front)
/// faces are hit.
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersectRayTriangle(const TRay<T>& ray, const TVec3<T>& a, const TVec3<T>& b,
                                                  const TVec3<T>& c, T& t, T& u, T& v,
                                                  bool cullBackFaces = false) noexcept {
    const TVec3<T> e1 = b - a;
    const TVec3<T> e2 = c - a;
    const TVec3<T> p = cross(ray.direction, e2);
    const T det = dot(e1, p);
    // Parallel ray or degenerate triangle. A subnormal det would overflow 1/det to infinity and
    // turn 0 * inf into NaN barycentrics, so it counts as degenerate too.
    constexpr T kMinDet = std::numeric_limits<T>::min();
    if (cullBackFaces ? !(det >= kMinDet) : !(abs(det) >= kMinDet)) return false;
    const T invDet = T(1) / det;
    const TVec3<T> s = ray.origin - a;
    const T uu = dot(s, p) * invDet;
    // Written as !(inside) so that NaN (from non-finite inputs) is rejected, never reported as a hit.
    if (!(uu >= T(0) && uu <= T(1))) return false;
    const TVec3<T> q = cross(s, e1);
    const T vv = dot(ray.direction, q) * invDet;
    if (!(vv >= T(0) && uu + vv <= T(1))) return false;
    const T tt = dot(e2, q) * invDet;
    if (!(tt >= T(0))) return false;
    t = tt;
    u = uu;
    v = vv;
    return true;
}
/// Ray vs oriented box (slab test in the box's local frame).
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersectRayOBB(const TRay<T>& ray, const TOBB<T>& box, T& tEnter,
                                             T& tExit) noexcept {
    const TQuat<T> inv = conjugate(box.rotation);
    const TRay<T> local{inv * (ray.origin - box.center), inv * ray.direction};
    return intersectRayAABB(local, TAABB<T>{-box.halfExtents, box.halfExtents}, tEnter, tExit);
}

// ---------------------------------------------------------------------------------------------
// Overlap tests
// ---------------------------------------------------------------------------------------------
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TAABB<T>& a, const TAABB<T>& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TSphere<T>& a, const TSphere<T>& b) noexcept {
    const T r = a.radius + b.radius;
    return distanceSq(a.center, b.center) <= r * r;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TSphere<T>& s, const TAABB<T>& b) noexcept {
    return distanceSq(b, s.center) <= s.radius * s.radius;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TAABB<T>& b, const TSphere<T>& s) noexcept {
    return intersects(s, b);
}
/// Box vs plane (touching/straddling counts).
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TAABB<T>& b, const TPlane<T>& pl) noexcept {
    const T r = dot(b.extents(), abs(pl.normal));
    return abs(pl.signedDistance(b.center())) <= r;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool intersects(const TSphere<T>& s, const TPlane<T>& pl) noexcept {
    return abs(pl.signedDistance(s.center)) <= s.radius;
}
/// Oriented box vs oriented box: separating-axis test over the 15 candidate axes (3 + 3 face
/// normals, 9 edge cross products). A small epsilon keeps near-parallel edge axes robust.
template <FloatingPoint T>
[[nodiscard]] inline bool intersects(const TOBB<T>& a, const TOBB<T>& b) noexcept {
    const TMat3<T> ra = toMat3(a.rotation), rb = toMat3(b.rotation);
    T r[3][3], absR[3][3];
    const T eps = sizeof(T) == 4 ? T(1e-6) : T(1e-12);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = dot(ra.cols[i], rb.cols[j]);
            absR[i][j] = abs(r[i][j]) + eps;
        }
    }
    const TVec3<T> dw = b.center - a.center;
    const T t[3] = {dot(dw, ra.cols[0]), dot(dw, ra.cols[1]), dot(dw, ra.cols[2])};
    const T ea[3] = {a.halfExtents.x, a.halfExtents.y, a.halfExtents.z};
    const T eb[3] = {b.halfExtents.x, b.halfExtents.y, b.halfExtents.z};
    // Face axes of A.
    for (int i = 0; i < 3; ++i) {
        const T rbSum = eb[0] * absR[i][0] + eb[1] * absR[i][1] + eb[2] * absR[i][2];
        if (abs(t[i]) > ea[i] + rbSum) return false;
    }
    // Face axes of B.
    for (int j = 0; j < 3; ++j) {
        const T raSum = ea[0] * absR[0][j] + ea[1] * absR[1][j] + ea[2] * absR[2][j];
        if (abs(t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j]) > raSum + eb[j]) return false;
    }
    // Edge-edge axes A_i x B_j (expressed in A's frame).
    for (int i = 0; i < 3; ++i) {
        const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
        for (int j = 0; j < 3; ++j) {
            const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            const T raSum = ea[i1] * absR[i2][j] + ea[i2] * absR[i1][j];
            const T rbSum = eb[j1] * absR[i][j2] + eb[j2] * absR[i][j1];
            const T dist = t[i2] * r[i1][j] - t[i1] * r[i2][j];
            if (abs(dist) > raSum + rbSum) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Bounding-volume utilities
// ---------------------------------------------------------------------------------------------
/// AABB of an AABB transformed by an affine matrix (Arvo's method; exact for the transformed box
/// corners). Empty boxes stay empty.
template <FloatingPoint T>
[[nodiscard]] constexpr TAABB<T> transformAABB(const TAABB<T>& b, const TMat4<T>& m) noexcept {
    if (b.isEmpty()) return b;
    const TVec3<T> c = transformPoint(m, b.center());
    const TVec3<T> e = b.extents();
    const TVec3<T> ne = abs(m.cols[0].xyz()) * e.x + abs(m.cols[1].xyz()) * e.y + abs(m.cols[2].xyz()) * e.z;
    return {c - ne, c + ne};
}
/// Smallest sphere enclosing the box.
template <FloatingPoint T>
[[nodiscard]] inline TSphere<T> boundingSphere(const TAABB<T>& b) noexcept {
    return {b.center(), length(b.extents())};
}
/// AABB enclosing the sphere.
template <FloatingPoint T>
[[nodiscard]] constexpr TAABB<T> bounds(const TSphere<T>& s) noexcept {
    return {s.center - TVec3<T>(s.radius), s.center + TVec3<T>(s.radius)};
}
/// Smallest sphere enclosing both spheres.
template <FloatingPoint T>
[[nodiscard]] inline TSphere<T> merge(const TSphere<T>& a, const TSphere<T>& b) noexcept {
    const TVec3<T> d = b.center - a.center;
    const T dist = length(d);
    if (dist + b.radius <= a.radius) return a;
    if (dist + a.radius <= b.radius) return b;
    const T r = (dist + a.radius + b.radius) * T(0.5);
    return {a.center + d * ((r - a.radius) / dist), r};
}
/// Union of two boxes.
template <FloatingPoint T>
[[nodiscard]] constexpr TAABB<T> merge(const TAABB<T>& a, const TAABB<T>& b) noexcept {
    return {min(a.min, b.min), max(a.max, b.max)};
}

template <FloatingPoint T>
[[nodiscard]] constexpr TAABB<f32> toF32(const TAABB<T>& b) noexcept {
    return {toF32(b.min), toF32(b.max)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr TAABB<f64> toF64(const TAABB<T>& b) noexcept {
    return {toF64(b.min), toF64(b.max)};
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
