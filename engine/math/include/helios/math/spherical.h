// helios/math/spherical.h — cube-sphere mapping, quadtree tile addressing for planets,
// latitude/longitude and great-circle helpers.
//
// Everything is f64 and deterministic (out of line, using helios::det trigonometry), because
// planet terrain is generated identically on clients and servers (ADR-006).
//
// Cube faces: each face has an outward normal n and in-plane axes (u, v) with cross(u, v) == n,
// so a face seen from outside has u pointing right and v pointing up; the four side faces (+-X,
// +-Z) have v = +Y. Face coordinates st are in [-1, 1]^2 (cube point = n + s*u + t*v); texture
// style uv in [0, 1]^2 is uv = st * 0.5 + 0.5.
//
// Mappings (cube face -> unit sphere); max/min cell area measured on a 64x64 face grid:
//   Gnomonic     normalize(cube point). Cheapest; area distortion 5.0x (corners vs centres).
//   EquiAngular  s' = tan(s * pi/4) before normalizing (Google's EAC / "tangent-adjusted").
//                Equal angles per cell along the face axes; area distortion 1.40x. Default.
//   Nowell       P. Nowell, "Mapping a cube to a sphere" (2005): x*sqrt(1 - y²/2 - z²/2 + y²z²/3).
//                No trigonometry, area distortion 1.29x but more shape (angle) distortion near
//                the corners; exact closed-form inverse (derived in src/spherical.cpp).
//
// Latitude/longitude (planet-local frame, Y-up, right-handed): latitude in [-pi/2, pi/2] is
// positive towards +Y (north pole); longitude in [-pi, pi] is 0 at +Z and increases towards +X,
// i.e. east is the direction of positive rotation about +Y (prograde planetary rotation).
//
// Threading: all functions are pure and thread-safe.
#pragma once

#include "helios/math/quat.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

enum class CubeFace : u8 { PosX = 0, NegX = 1, PosY = 2, NegY = 3, PosZ = 4, NegZ = 5 };
inline constexpr int kCubeFaceCount = 6;

enum class CubeMapping : u8 { Gnomonic, EquiAngular, Nowell };

struct CubeFaceBasis {
    DVec3 normal;
    DVec3 u;
    DVec3 v;
};

/// Orthonormal basis of a cube face (cross(u, v) == normal).
[[nodiscard]] constexpr CubeFaceBasis cubeFaceBasis(CubeFace f) noexcept {
    switch (f) {
        case CubeFace::PosX: return {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
        case CubeFace::NegX: return {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}};
        case CubeFace::PosY: return {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
        case CubeFace::NegY: return {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}};
        case CubeFace::PosZ: return {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
        default: return {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};
    }
}

/// A point on the cube: face plus face coordinates st in [-1, 1]^2.
struct CubeCoord {
    CubeFace face = CubeFace::PosX;
    DVec2 st{};
};

/// Face whose normal is closest to `dir` (ties resolve X before Y before Z, + before -).
[[nodiscard]] CubeFace dominantFace(const DVec3& dir) noexcept;
/// Point on the [-1, 1]^3 cube surface: n + s*u + t*v.
[[nodiscard]] constexpr DVec3 cubePoint(CubeFace face, const DVec2& st) noexcept {
    const CubeFaceBasis b = cubeFaceBasis(face);
    return b.normal + b.u * st.x + b.v * st.y;
}
/// Face coordinates -> unit direction.
[[nodiscard]] DVec3 cubeToSphere(CubeFace face, const DVec2& st,
                                 CubeMapping mapping = CubeMapping::EquiAngular) noexcept;
/// Unit (or any non-zero) direction -> face coordinates. Exact inverse of cubeToSphere up to
/// rounding for all three mappings.
[[nodiscard]] CubeCoord sphereToCube(const DVec3& dir,
                                     CubeMapping mapping = CubeMapping::EquiAngular) noexcept;
[[nodiscard]] constexpr DVec2 stToUv(const DVec2& st) noexcept { return st * 0.5 + DVec2(0.5); }
[[nodiscard]] constexpr DVec2 uvToSt(const DVec2& uv) noexcept { return uv * 2.0 - DVec2(1.0); }

// ---------------------------------------------------------------------------------------------
// Quadtree tiles on the cube (one quadtree per face).
// ---------------------------------------------------------------------------------------------
enum class TileEdge : u8 { NegU = 0, PosU = 1, NegV = 2, PosV = 3 };

/// Tile (face, level, x, y): level 0 is the whole face; x counts along u, y along v, each in
/// [0, 2^level).
struct CubeTile {
    static constexpr int kMaxLevel = 28;

    CubeFace face = CubeFace::PosX;
    u8 level = 0;
    u32 x = 0;
    u32 y = 0;

    /// Unique 64-bit key: [face:3][level:5][x:28][y:28].
    [[nodiscard]] constexpr u64 key() const noexcept {
        return (static_cast<u64>(face) << 61) | (static_cast<u64>(level) << 56) |
               (static_cast<u64>(x) << 28) | static_cast<u64>(y);
    }
    [[nodiscard]] static constexpr CubeTile fromKey(u64 k) noexcept {
        return {static_cast<CubeFace>((k >> 61) & 7u), static_cast<u8>((k >> 56) & 31u),
                static_cast<u32>((k >> 28) & 0xFFFFFFFu), static_cast<u32>(k & 0xFFFFFFFu)};
    }
    /// Tile count along one axis at this level.
    [[nodiscard]] constexpr u32 resolution() const noexcept { return 1u << level; }
    /// Precondition: level > 0.
    [[nodiscard]] constexpr CubeTile parent() const noexcept {
        HELIOS_MATH_ASSERT(level > 0);
        return {face, static_cast<u8>(level - 1), x >> 1, y >> 1};
    }
    /// Child quadrant 0..3: bit 0 selects +u half, bit 1 selects +v half. Precondition: level < kMaxLevel.
    [[nodiscard]] constexpr CubeTile child(int quadrant) const noexcept {
        HELIOS_MATH_ASSERT(level < kMaxLevel && quadrant >= 0 && quadrant < 4);
        return {face, static_cast<u8>(level + 1), (x << 1) | static_cast<u32>(quadrant & 1),
                (y << 1) | static_cast<u32>(quadrant >> 1)};
    }
    /// Face-coordinate bounds (st in [-1, 1]).
    [[nodiscard]] constexpr DVec2 stMin() const noexcept {
        const f64 size = 2.0 / static_cast<f64>(resolution());
        return {-1.0 + static_cast<f64>(x) * size, -1.0 + static_cast<f64>(y) * size};
    }
    [[nodiscard]] constexpr DVec2 stMax() const noexcept {
        const f64 size = 2.0 / static_cast<f64>(resolution());
        return {-1.0 + static_cast<f64>(x + 1) * size, -1.0 + static_cast<f64>(y + 1) * size};
    }
    [[nodiscard]] constexpr DVec2 stCenter() const noexcept { return (stMin() + stMax()) * 0.5; }
    friend constexpr bool operator==(const CubeTile&, const CubeTile&) noexcept = default;
};

/// Tile at `level` containing a face coordinate (st on the face edge belongs to the last tile).
[[nodiscard]] CubeTile tileAt(const CubeCoord& c, int level) noexcept;
/// Tile at `level` containing a direction.
[[nodiscard]] CubeTile tileAt(const DVec3& dir, int level,
                              CubeMapping mapping = CubeMapping::EquiAngular) noexcept;
/// Same-level neighbour across an edge; crosses onto the adjacent cube face at face borders
/// (the neighbour's own axes may be rotated relative to this tile's).
[[nodiscard]] CubeTile tileNeighbor(const CubeTile& t, TileEdge edge) noexcept;
/// Unit direction through the tile centre.
[[nodiscard]] DVec3 tileCenterDirection(const CubeTile& t,
                                        CubeMapping mapping = CubeMapping::EquiAngular) noexcept;

// ---------------------------------------------------------------------------------------------
// Latitude / longitude and great circles.
// ---------------------------------------------------------------------------------------------
struct LatLon {
    f64 lat = 0.0;  ///< radians, + north (+Y)
    f64 lon = 0.0;  ///< radians, 0 at +Z, + towards +X (east)
};

[[nodiscard]] DVec3 latLonToDirection(const LatLon& ll) noexcept;
/// Precondition: dir non-zero. Longitude is 0 at the poles.
[[nodiscard]] LatLon directionToLatLon(const DVec3& dir) noexcept;
/// Central angle between two directions (need not be unit), accurate for tiny and antipodal
/// separations (atan2 formulation).
[[nodiscard]] f64 greatCircleAngle(const DVec3& a, const DVec3& b) noexcept;
/// Great-circle (geodesic) distance on a sphere of `radius` metres.
[[nodiscard]] f64 geodesicDistance(const LatLon& a, const LatLon& b, f64 radius) noexcept;
[[nodiscard]] f64 geodesicDistance(const DVec3& dirA, const DVec3& dirB, f64 radius) noexcept;
/// Point at fraction t along the great circle from unit direction a to unit direction b.
[[nodiscard]] DVec3 slerpDirection(const DVec3& a, const DVec3& b, f64 t) noexcept;
/// Local East-North-Up axes at a surface direction (east = +X at lon 0, lat 0). At the poles
/// east is taken as +X.
void tangentFrameENU(const DVec3& dir, DVec3& east, DVec3& north, DVec3& up) noexcept;
/// Rotation of a surface-attached Y-up frame: +X east, +Y up, -Z (forward) north.
[[nodiscard]] DQuat surfaceFrameRotation(const DVec3& dir) noexcept;

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
