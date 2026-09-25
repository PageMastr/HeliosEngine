// Cube-sphere mapping, tile addressing and spherical helpers (see helios/math/spherical.h).
// Deterministic: arithmetic, sqrt and helios::det trigonometry only, contraction disabled.
#include "fp_control.h"

#include "helios/math/spherical.h"

#include <cmath>

namespace helios {
namespace {

constexpr f64 kFourOverPi = 1.27323954473516268615;

f64 detTan(f64 x) noexcept {
    f64 s = 0.0, c = 1.0;
    det::sinCos(x, s, c);
    return s / c;
}

f64 clampUnit(f64 x) noexcept { return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x); }

// Face coordinates -> point on the cube face plane, before normalization, per mapping.
DVec3 facePoint(const CubeFaceBasis& b, const DVec2& st, CubeMapping mapping) noexcept {
    switch (mapping) {
        case CubeMapping::Gnomonic: return b.normal + b.u * st.x + b.v * st.y;
        case CubeMapping::EquiAngular:
            return b.normal + b.u * detTan(st.x * kQuarterPiD) + b.v * detTan(st.y * kQuarterPiD);
        case CubeMapping::Nowell:
        default: {
            // In the face frame the cube point is (s, t, 1); Nowell's formula per component.
            const f64 s2 = st.x * st.x, t2 = st.y * st.y;
            const f64 cu = st.x * std::sqrt(0.5 - t2 / 6.0);
            const f64 cv = st.y * std::sqrt(0.5 - s2 / 6.0);
            const f64 cn = std::sqrt(max(0.0, 1.0 - s2 * 0.5 - t2 * 0.5 + s2 * t2 / 3.0));
            return b.normal * cn + b.u * cu + b.v * cv;  // already unit length
        }
    }
}

// Nowell inverse. On a face with cube coordinates (s, t, 1) the sphere components along u and v
// are a = s*sqrt(1/2 - t²/6) and b = t*sqrt(1/2 - s²/6). Squaring, with S = s², T = t²:
//   a² = S/2 - ST/6,  b² = T/2 - ST/6   =>   S = T + 2d  with d = a² - b²,
// and substituting gives T² + (2d - 3) T + 6b² = 0. T is the smaller root (T <= 1); by symmetry
// S solves the same equation with a and -d. Written as 12b² / (B + sqrt(B² - 24b²)), B = 3 - 2d,
// the root has no cancellation.
f64 nowellInverseSq(f64 b2, f64 bq) noexcept {
    const f64 disc = bq * bq - 24.0 * b2;
    return 12.0 * b2 / (bq + std::sqrt(disc > 0.0 ? disc : 0.0));
}

}  // namespace

CubeFace dominantFace(const DVec3& d) noexcept {
    const f64 ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) return d.x >= 0.0 ? CubeFace::PosX : CubeFace::NegX;
    if (ay >= az) return d.y >= 0.0 ? CubeFace::PosY : CubeFace::NegY;
    return d.z >= 0.0 ? CubeFace::PosZ : CubeFace::NegZ;
}

DVec3 cubeToSphere(CubeFace face, const DVec2& st, CubeMapping mapping) noexcept {
    const DVec3 p = facePoint(cubeFaceBasis(face), st, mapping);
    return mapping == CubeMapping::Nowell ? p : normalize(p);
}

CubeCoord sphereToCube(const DVec3& dirIn, CubeMapping mapping) noexcept {
    const CubeFace face = dominantFace(dirIn);
    const CubeFaceBasis b = cubeFaceBasis(face);
    const DVec3 dir = normalize(dirIn);
    const f64 dn = dot(dir, b.normal);  // >= 1/sqrt(3)
    const f64 du = dot(dir, b.u);
    const f64 dv = dot(dir, b.v);
    DVec2 st;
    switch (mapping) {
        case CubeMapping::Gnomonic: st = {du / dn, dv / dn}; break;
        case CubeMapping::EquiAngular:
            st = {det::atan(du / dn) * kFourOverPi, det::atan(dv / dn) * kFourOverPi};
            break;
        case CubeMapping::Nowell:
        default: {
            const f64 a2 = du * du, b2 = dv * dv;
            const f64 d = a2 - b2;
            const f64 s2 = nowellInverseSq(a2, 3.0 + 2.0 * d);
            const f64 t2 = nowellInverseSq(b2, 3.0 - 2.0 * d);
            st = {std::copysign(std::sqrt(s2), du), std::copysign(std::sqrt(t2), dv)};
            break;
        }
    }
    return {face, {clampUnit(st.x), clampUnit(st.y)}};
}

CubeTile tileAt(const CubeCoord& c, int level) noexcept {
    HELIOS_MATH_ASSERT(level >= 0 && level <= CubeTile::kMaxLevel);
    const u32 res = 1u << level;
    const f64 fres = static_cast<f64>(res);
    auto axis = [&](f64 s) {
        const f64 f = std::floor((clampUnit(s) + 1.0) * 0.5 * fres);
        return f < 0.0 ? 0u : (f >= fres ? res - 1u : static_cast<u32>(f));
    };
    return {c.face, static_cast<u8>(level), axis(c.st.x), axis(c.st.y)};
}

CubeTile tileAt(const DVec3& dir, int level, CubeMapping mapping) noexcept {
    return tileAt(sphereToCube(dir, mapping), level);
}

CubeTile tileNeighbor(const CubeTile& t, TileEdge edge) noexcept {
    const u32 res = t.resolution();
    switch (edge) {
        case TileEdge::NegU:
            if (t.x > 0) return {t.face, t.level, t.x - 1, t.y};
            break;
        case TileEdge::PosU:
            if (t.x + 1 < res) return {t.face, t.level, t.x + 1, t.y};
            break;
        case TileEdge::NegV:
            if (t.y > 0) return {t.face, t.level, t.x, t.y - 1};
            break;
        case TileEdge::PosV:
            if (t.y + 1 < res) return {t.face, t.level, t.x, t.y + 1};
            break;
    }
    // Crossing a face border: step the tile centre one tile over the edge, then fold that point
    // around the cube edge onto the adjacent face (unfolding preserves distances along and across
    // the edge, so it lands on the neighbour tile's centre regardless of face orientation).
    const CubeFaceBasis b = cubeFaceBasis(t.face);
    const f64 size = 2.0 / static_cast<f64>(res);
    DVec2 c = t.stCenter();
    const bool alongU = edge == TileEdge::NegU || edge == TileEdge::PosU;
    const f64 sgn = (edge == TileEdge::PosU || edge == TileEdge::PosV) ? 1.0 : -1.0;
    if (alongU)
        c.x += sgn * size;
    else
        c.y += sgn * size;
    const f64 over = (alongU ? std::fabs(c.x) : std::fabs(c.y)) - 1.0;  // distance past the edge
    const DVec3 edgeAxis = alongU ? b.u * sgn : b.v * sgn;
    const DVec3 folded = edgeAxis + b.normal * (1.0 - over) + (alongU ? b.v * c.y : b.u * c.x);
    const CubeFace nf = dominantFace(folded);
    const CubeFaceBasis nb = cubeFaceBasis(nf);
    return tileAt(CubeCoord{nf, {dot(folded, nb.u), dot(folded, nb.v)}}, t.level);
}

DVec3 tileCenterDirection(const CubeTile& t, CubeMapping mapping) noexcept {
    return cubeToSphere(t.face, t.stCenter(), mapping);
}

DVec3 latLonToDirection(const LatLon& ll) noexcept {
    f64 sLat = 0.0, cLat = 1.0, sLon = 0.0, cLon = 1.0;
    det::sinCos(ll.lat, sLat, cLat);
    det::sinCos(ll.lon, sLon, cLon);
    return {cLat * sLon, sLat, cLat * cLon};
}

LatLon directionToLatLon(const DVec3& d) noexcept {
    const f64 horiz = std::sqrt(d.x * d.x + d.z * d.z);
    return {det::atan2(d.y, horiz), horiz > 0.0 ? det::atan2(d.x, d.z) : 0.0};
}

f64 greatCircleAngle(const DVec3& a, const DVec3& b) noexcept {
    return det::atan2(length(cross(a, b)), dot(a, b));
}

f64 geodesicDistance(const LatLon& a, const LatLon& b, f64 radius) noexcept {
    return greatCircleAngle(latLonToDirection(a), latLonToDirection(b)) * radius;
}

f64 geodesicDistance(const DVec3& dirA, const DVec3& dirB, f64 radius) noexcept {
    return greatCircleAngle(dirA, dirB) * radius;
}

DVec3 slerpDirection(const DVec3& a, const DVec3& b, f64 t) noexcept {
    const f64 theta = greatCircleAngle(a, b);
    if (theta < 1e-9) return normalize(lerp(a, b, t));
    // Orthonormal direction towards b within the great circle's plane.
    const DVec3 perpDir = normalizeOr(b - a * dot(a, b), anyPerpendicular(a));
    f64 s = 0.0, c = 1.0;
    det::sinCos(theta * t, s, c);
    return normalize(a * c + perpDir * s);
}

void tangentFrameENU(const DVec3& dir, DVec3& east, DVec3& north, DVec3& up) noexcept {
    up = normalize(dir);
    east = normalizeOr(cross(DVec3::unitY(), up), DVec3::unitX());
    north = cross(up, east);
}

DQuat surfaceFrameRotation(const DVec3& dir) noexcept {
    DVec3 east, north, up;
    tangentFrameENU(dir, east, north, up);
    return DQuat::fromBasis(east, up, -north);
}

}  // namespace helios
