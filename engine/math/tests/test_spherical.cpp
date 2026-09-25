#include "test_util.h"

#include "helios/math/spherical.h"

#include <set>
#include <tuple>

using namespace helios;
using helios::test::Rng;

namespace {
constexpr CubeMapping kMappings[] = {CubeMapping::Gnomonic, CubeMapping::EquiAngular, CubeMapping::Nowell};
CubeFace faceOf(int i) { return static_cast<CubeFace>(i); }
}  // namespace

TEST_CASE("spherical: cube face bases") {
    std::set<std::tuple<int, int, int>> normals;
    for (int f = 0; f < kCubeFaceCount; ++f) {
        const CubeFaceBasis b = cubeFaceBasis(faceOf(f));
        CHECK(cross(b.u, b.v) == b.normal);  // right-handed, seen from outside
        CHECK(dot(b.u, b.normal) == 0.0);
        CHECK(dominantFace(b.normal) == faceOf(f));
        normals.insert(
            {static_cast<int>(b.normal.x), static_cast<int>(b.normal.y), static_cast<int>(b.normal.z)});
        if (f == 0 || f == 1 || f == 4 || f == 5) CHECK(b.v == DVec3::unitY());  // side faces: v is up
        CHECK(cubePoint(faceOf(f), {0.0, 0.0}) == b.normal);
        CHECK(maxComponent(abs(cubePoint(faceOf(f), {0.3, -1.0}))) == 1.0);
    }
    CHECK(normals.size() == 6);
    CHECK(dominantFace({0.5, 0.5, 0.2}) == CubeFace::PosX);  // ties resolve to X
    CHECK(dominantFace({0.1, -0.9, 0.5}) == CubeFace::NegY);
    CHECK(stToUv({-1.0, 1.0}) == DVec2(0.0, 1.0));
    CHECK(uvToSt({0.25, 0.5}) == DVec2(-0.5, 0.0));
}

TEST_CASE("spherical: cube <-> sphere round trips for every mapping") {
    Rng rng(100);
    for (CubeMapping m : kMappings) {
        CAPTURE(static_cast<int>(m));
        f64 worst = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const CubeFace f = faceOf(static_cast<int>(rng.nextU32() % 6));
            const DVec2 st{rng.range(-1.0, 1.0), rng.range(-1.0, 1.0)};
            const DVec3 d = cubeToSphere(f, st, m);
            CHECK(approxEqual(length(d), 1.0, 1e-14));
            const CubeCoord c = sphereToCube(d, m);
            if (maxComponent(abs(st)) < 1.0 - 1e-9) CHECK(c.face == f);  // face edges are shared
            if (c.face == f) worst = max(worst, maxComponent(abs(c.st - st)));
            CHECK(approxEqual(cubeToSphere(c.face, c.st, m), d, 1e-12));
            // Direction -> cube -> direction, starting from arbitrary (non-unit) vectors.
            const DVec3 v = rng.dvec3(-3.0, 3.0);
            if (length(v) > 1e-3) {
                const CubeCoord cc = sphereToCube(v, m);
                CHECK(approxEqual(cubeToSphere(cc.face, cc.st, m), normalize(v), 1e-12));
            }
        }
        MESSAGE("mapping " << static_cast<int>(m) << " worst st round-trip error " << worst);
        CHECK(worst < 1e-12);
        // Face centres map to the axes and corners to the cube diagonals.
        for (int f = 0; f < kCubeFaceCount; ++f) {
            const CubeFaceBasis b = cubeFaceBasis(faceOf(f));
            CHECK(approxEqual(cubeToSphere(faceOf(f), {0.0, 0.0}, m), b.normal, 1e-15));
            CHECK(
                approxEqual(cubeToSphere(faceOf(f), {1.0, 1.0}, m), normalize(b.normal + b.u + b.v), 1e-15));
            // The face edge maps onto the neighbouring face's edge (the mapping is continuous).
            for (f64 t = -1.0; t <= 1.0; t += 0.125) {
                const DVec3 onEdge = cubeToSphere(faceOf(f), {1.0, t}, m);
                const CubeCoord other = sphereToCube(onEdge + b.u * 1e-9, m);  // nudged across
                CHECK(other.face == dominantFace(b.u));
                CHECK(approxEqual(maxComponent(abs(other.st)), 1.0, 1e-6));
                CHECK(approxEqual(cubeToSphere(other.face, other.st, m), onEdge, 1e-8));
            }
        }
    }
}

TEST_CASE("spherical: area distortion of the mappings") {
    // Ratio of the largest to smallest solid angle of equal face-space cells.
    auto ratio = [](CubeMapping m) {
        const int n = 64;
        f64 lo = kInfinityD, hi = 0.0;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const f64 s0 = -1.0 + 2.0 * x / n, s1 = -1.0 + 2.0 * (x + 1) / n;
                const f64 t0 = -1.0 + 2.0 * y / n, t1 = -1.0 + 2.0 * (y + 1) / n;
                const DVec3 a = cubeToSphere(CubeFace::PosZ, {s0, t0}, m),
                            b = cubeToSphere(CubeFace::PosZ, {s1, t0}, m);
                const DVec3 c = cubeToSphere(CubeFace::PosZ, {s1, t1}, m),
                            d = cubeToSphere(CubeFace::PosZ, {s0, t1}, m);
                const f64 area = length(cross(b - a, d - a)) * 0.5 + length(cross(b - c, d - c)) * 0.5;
                lo = min(lo, area);
                hi = max(hi, area);
            }
        }
        return hi / lo;
    };
    const f64 g = ratio(CubeMapping::Gnomonic), e = ratio(CubeMapping::EquiAngular),
              nw = ratio(CubeMapping::Nowell);
    MESSAGE("max/min cell area: gnomonic " << g << ", equi-angular " << e << ", Nowell " << nw);
    CHECK(g > 4.5);
    CHECK(e < 1.5);
    CHECK(nw < 1.9);
}

TEST_CASE("spherical: quadtree tiles") {
    const CubeTile t{CubeFace::NegY, 5, 17, 3};
    CHECK(CubeTile::fromKey(t.key()) == t);
    const CubeTile deep{CubeFace::NegZ, CubeTile::kMaxLevel, (1u << 28) - 1u, 12345u};
    CHECK(CubeTile::fromKey(deep.key()) == deep);
    CHECK(t.resolution() == 32u);
    CHECK(t.parent() == CubeTile{CubeFace::NegY, 4, 8, 1});
    for (int q = 0; q < 4; ++q) {
        CHECK(t.child(q).parent() == t);
        const CubeTile c = t.child(q);
        CHECK(c.stMin().x >= t.stMin().x);
        CHECK(c.stMax().y <= t.stMax().y);
    }
    CHECK(t.child(3) == CubeTile{CubeFace::NegY, 6, 35, 7});
    CHECK(approxEqual(t.stMax() - t.stMin(), DVec2(2.0 / 32.0), 1e-15));
    // tileAt finds the tile containing a point; the tile's centre direction maps back to it.
    Rng rng(101);
    for (int i = 0; i < 5000; ++i) {
        const DVec3 d = rng.unitD();
        const int level = static_cast<int>(rng.nextU32() % 20);
        const CubeTile tile = tileAt(d, level);
        const CubeCoord c = sphereToCube(d);
        CHECK(tile.face == c.face);
        CHECK(c.st.x >= tile.stMin().x - 1e-15);
        CHECK(c.st.x <= tile.stMax().x + 1e-15);
        CHECK(tileAt(tileCenterDirection(tile), level) == tile);
        if (level > 0) CHECK(tileAt(d, level - 1) == tile.parent());
    }
    CHECK(tileAt(CubeCoord{CubeFace::PosX, {1.0, 1.0}}, 3) ==
          CubeTile{CubeFace::PosX, 3, 7, 7});  // edge -> last tile
    CHECK(tileAt(CubeCoord{CubeFace::PosX, {-1.0, -1.0}}, 3) == CubeTile{CubeFace::PosX, 3, 0, 0});
}

TEST_CASE("spherical: tile neighbours across face edges") {
    // Level 0: every face has the four faces adjacent to it as neighbours.
    for (int f = 0; f < kCubeFaceCount; ++f) {
        const CubeTile t{faceOf(f), 0, 0, 0};
        std::set<int> faces;
        for (int e = 0; e < 4; ++e) {
            const CubeTile n = tileNeighbor(t, static_cast<TileEdge>(e));
            faces.insert(static_cast<int>(n.face));
            CHECK(dot(cubeFaceBasis(n.face).normal, cubeFaceBasis(t.face).normal) == 0.0);
        }
        CHECK(faces.size() == 4);
    }
    // Every neighbour relation is symmetric and neighbours are adjacent on the sphere.
    for (int level = 0; level <= 4; ++level) {
        const u32 res = 1u << level;
        for (int f = 0; f < kCubeFaceCount; ++f) {
            for (u32 y = 0; y < res; ++y) {
                for (u32 x = 0; x < res; ++x) {
                    const CubeTile t{faceOf(f), static_cast<u8>(level), x, y};
                    for (int e = 0; e < 4; ++e) {
                        const CubeTile n = tileNeighbor(t, static_cast<TileEdge>(e));
                        CHECK(n.level == t.level);
                        CHECK_FALSE(n == t);
                        bool back = false;
                        for (int e2 = 0; e2 < 4; ++e2)
                            back = back || tileNeighbor(n, static_cast<TileEdge>(e2)) == t;
                        CHECK(back);
                        // Centres are about one tile apart (equi-angular: pi/2 / res radians).
                        const f64 ang = greatCircleAngle(tileCenterDirection(t), tileCenterDirection(n));
                        CHECK(ang > 0.6 * kHalfPiD / res);
                        CHECK(ang < 1.5 * kHalfPiD / res);
                    }
                }
            }
        }
    }
    // Within a face, neighbours are simple offsets.
    const CubeTile t{CubeFace::PosZ, 3, 4, 5};
    CHECK(tileNeighbor(t, TileEdge::PosU) == CubeTile{CubeFace::PosZ, 3, 5, 5});
    CHECK(tileNeighbor(t, TileEdge::NegV) == CubeTile{CubeFace::PosZ, 3, 4, 4});
    // Crossing +u on +Z lands on +X (u of +Z is +X) at the matching height.
    const CubeTile edge{CubeFace::PosZ, 3, 7, 2};
    const CubeTile across = tileNeighbor(edge, TileEdge::PosU);
    CHECK(across == CubeTile{CubeFace::PosX, 3, 0, 2});
}

TEST_CASE("spherical: latitude / longitude") {
    CHECK(approxEqual(latLonToDirection({0.0, 0.0}), DVec3(0.0, 0.0, 1.0), 1e-15));
    CHECK(
        approxEqual(latLonToDirection({0.0, kHalfPiD}), DVec3(1.0, 0.0, 0.0), 1e-15));  // east of lon 0 is +X
    CHECK(approxEqual(latLonToDirection({kHalfPiD, 1.0}), DVec3(0.0, 1.0, 0.0), 1e-15));
    CHECK(approxEqual(latLonToDirection({-kHalfPiD, 0.0}), DVec3(0.0, -1.0, 0.0), 1e-15));
    const LatLon pole = directionToLatLon({0.0, 5.0, 0.0});
    CHECK(pole.lat == kHalfPiD);
    CHECK(pole.lon == 0.0);
    Rng rng(102);
    for (int i = 0; i < 5000; ++i) {
        const LatLon ll{rng.range(-kHalfPiD + 1e-6, kHalfPiD - 1e-6), rng.range(-kPiD + 1e-9, kPiD)};
        const LatLon back = directionToLatLon(latLonToDirection(ll) * 7.0);
        CHECK(approxEqual(back.lat, ll.lat, 1e-12));
        CHECK(approxEqual(back.lon, ll.lon, 1e-9));
    }
    // Great circles.
    const f64 r = 6.371e6;
    CHECK(approxEqual(geodesicDistance(LatLon{0.0, 0.0}, LatLon{0.0, kHalfPiD}, r), kHalfPiD * r, 1e-6));
    CHECK(approxEqual(geodesicDistance(LatLon{0.3, 0.2}, LatLon{-0.3, 0.2 + kPiD}, r), kPiD * r,
                      1e-6));  // antipodal
    // One metre on Earth stays accurate (acos(dot) would lose it).
    const DVec3 a = latLonToDirection({0.5, 1.0});
    const DVec3 b = latLonToDirection({0.5 + 1.0 / r, 1.0});
    CHECK(approxEqual(geodesicDistance(a, b, r), 1.0, 1e-7));
    // Haversine cross-check.
    for (int i = 0; i < 1000; ++i) {
        const LatLon p{rng.range(-1.5, 1.5), rng.range(-3.1, 3.1)},
            q{rng.range(-1.5, 1.5), rng.range(-3.1, 3.1)};
        const f64 h = std::pow(std::sin((q.lat - p.lat) / 2), 2) +
                      std::cos(p.lat) * std::cos(q.lat) * std::pow(std::sin((q.lon - p.lon) / 2), 2);
        CHECK(approxEqual(geodesicDistance(p, q, r), 2.0 * r * std::asin(std::sqrt(h)), 1e-3));
    }
    // Slerp along the great circle.
    const DVec3 s0 = rng.unitD(), s1 = rng.unitD();
    CHECK(approxEqual(slerpDirection(s0, s1, 0.0), s0, 1e-12));
    CHECK(approxEqual(slerpDirection(s0, s1, 1.0), s1, 1e-12));
    const DVec3 mid = slerpDirection(s0, s1, 0.5);
    CHECK(approxEqual(greatCircleAngle(s0, mid), greatCircleAngle(mid, s1), 1e-12));
    CHECK(approxEqual(length(mid), 1.0, 1e-15));
    CHECK(slerpDirection(s0, s0, 0.3) == normalize(lerp(s0, s0, 0.3)));
}

TEST_CASE("spherical: surface frames") {
    DVec3 east, north, up;
    tangentFrameENU(latLonToDirection({0.0, 0.0}), east, north, up);
    CHECK(approxEqual(east, DVec3(1.0, 0.0, 0.0), 1e-15));
    CHECK(approxEqual(north, DVec3(0.0, 1.0, 0.0), 1e-15));
    CHECK(approxEqual(up, DVec3(0.0, 0.0, 1.0), 1e-15));
    tangentFrameENU({0.0, 1.0, 0.0}, east, north, up);  // pole: still orthonormal
    CHECK(approxEqual(cross(east, north), up, 1e-15));
    Rng rng(103);
    for (int i = 0; i < 2000; ++i) {
        const LatLon ll{rng.range(-1.5, 1.5), rng.range(-3.1, 3.1)};
        const DVec3 d = latLonToDirection(ll);
        tangentFrameENU(d, east, north, up);
        CHECK(approxEqual(cross(east, north), up, 1e-12));
        // East is the direction of increasing longitude, north of increasing latitude.
        CHECK(dot(latLonToDirection({ll.lat, ll.lon + 1e-6}) - d, east) > 0.0);
        CHECK(dot(latLonToDirection({ll.lat + 1e-6, ll.lon}) - d, north) > 0.0);
        const DQuat q = surfaceFrameRotation(d);
        CHECK(approxEqual(axisY(q), up, 1e-12));
        CHECK(approxEqual(forward(q), north, 1e-12));
        CHECK(approxEqual(axisX(q), east, 1e-12));
    }
}

TEST_CASE("spherical: golden values (deterministic mapping)") {
    // {mapping, face, st, direction bits}: planet terrain sampled on clients and servers must agree.
    struct CubeGolden {
        int mapping, face;
        DVec2 st;
        u64 x, y, z;
    };
    // clang-format off
    static const CubeGolden cube[] = {
        {0, 0, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fe975348cb4238bull, 0xbfe1d20b2f4ae5aeull, 0xbfce8ca575a4f773ull},
        {0, 0, DVec2(-0.999, 0.5), 0x3fe557c2d0ae46caull, 0x3fd557c2d0ae46caull, 0x3fe5524c17a3dcbcull},
        {0, 0, DVec2(0.123456789, 0.98765432099999995), 0x3fe6ae2140ee3b1cull, 0x3fe666730415942cull, 0xbfb6667300a8e1eeull},
        {0, 2, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fce8ca575a4f773ull, 0x3fe975348cb4238bull, 0x3fe1d20b2f4ae5aeull},
        {0, 2, DVec2(-0.999, 0.5), 0xbfe5524c17a3dcbcull, 0x3fe557c2d0ae46caull, 0xbfd557c2d0ae46caull},
        {0, 2, DVec2(0.123456789, 0.98765432099999995), 0x3fb6667300a8e1eeull, 0x3fe6ae2140ee3b1cull, 0xbfe666730415942cull},
        {0, 4, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fce8ca575a4f773ull, 0xbfe1d20b2f4ae5aeull, 0x3fe975348cb4238bull},
        {0, 4, DVec2(-0.999, 0.5), 0xbfe5524c17a3dcbcull, 0x3fd557c2d0ae46caull, 0x3fe557c2d0ae46caull},
        {0, 4, DVec2(0.123456789, 0.98765432099999995), 0x3fb6667300a8e1eeull, 0x3fe666730415942cull, 0x3fe6ae2140ee3b1cull},
        {1, 0, DVec2(0.29999999999999999, -0.69999999999999996), 0x3feabaee86b6764full, 0xbfe0615a9d78cfdcull, 0xbfc9ab5ec866861cull},
        {1, 0, DVec2(-0.999, 0.5), 0x3fe5bb18c8ec2dd1ull, 0x3fd2009ecbf82ec5ull, 0x3fe5b25d7c6c7f50ull},
        {1, 0, DVec2(0.123456789, 0.98765432099999995), 0x3fe6ca76333db28aull, 0x3fe65a6653d7920bull, 0xbfb1bc0a3de1a9b7ull},
        {1, 2, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fc9ab5ec866861cull, 0x3feabaee86b6764full, 0x3fe0615a9d78cfdcull},
        {1, 2, DVec2(-0.999, 0.5), 0xbfe5b25d7c6c7f50ull, 0x3fe5bb18c8ec2dd1ull, 0xbfd2009ecbf82ec5ull},
        {1, 2, DVec2(0.123456789, 0.98765432099999995), 0x3fb1bc0a3de1a9b7ull, 0x3fe6ca76333db28aull, 0xbfe65a6653d7920bull},
        {1, 4, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fc9ab5ec866861cull, 0xbfe0615a9d78cfdcull, 0x3feabaee86b6764full},
        {1, 4, DVec2(-0.999, 0.5), 0xbfe5b25d7c6c7f50ull, 0x3fd2009ecbf82ec5ull, 0x3fe5bb18c8ec2dd1ull},
        {1, 4, DVec2(0.123456789, 0.98765432099999995), 0x3fb1bc0a3de1a9b7ull, 0x3fe65a6653d7920bull, 0x3fe6ca76333db28aull},
        {2, 0, DVec2(0.29999999999999999, -0.69999999999999996), 0x3feb3dcb193b380cull, 0xbfdf33185031c005ull, 0xbfc8d62c9b07e129ull},
        {2, 0, DVec2(-0.999, 0.5), 0x3fe5af0c17b52c5dull, 0x3fd27c0436ab0cf7ull, 0x3fe5a476d64aef01ull},
        {2, 0, DVec2(0.123456789, 0.98765432099999995), 0x3fe6d7fd52108a57ull, 0x3fe64a8da680d45full, 0xbfb25bd5095f298full},
        {2, 2, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fc8d62c9b07e129ull, 0x3feb3dcb193b380cull, 0x3fdf33185031c005ull},
        {2, 2, DVec2(-0.999, 0.5), 0xbfe5a476d64aef01ull, 0x3fe5af0c17b52c5dull, 0xbfd27c0436ab0cf7ull},
        {2, 2, DVec2(0.123456789, 0.98765432099999995), 0x3fb25bd5095f298full, 0x3fe6d7fd52108a57ull, 0xbfe64a8da680d45full},
        {2, 4, DVec2(0.29999999999999999, -0.69999999999999996), 0x3fc8d62c9b07e129ull, 0xbfdf33185031c005ull, 0x3feb3dcb193b380cull},
        {2, 4, DVec2(-0.999, 0.5), 0xbfe5a476d64aef01ull, 0x3fd27c0436ab0cf7ull, 0x3fe5af0c17b52c5dull},
        {2, 4, DVec2(0.123456789, 0.98765432099999995), 0x3fb25bd5095f298full, 0x3fe64a8da680d45full, 0x3fe6d7fd52108a57ull},
    };
    // clang-format on
    for (const CubeGolden& g : cube) {
        const DVec3 v =
            cubeToSphere(static_cast<CubeFace>(g.face), g.st, static_cast<CubeMapping>(g.mapping));
        CAPTURE(g.mapping);
        CAPTURE(g.face);
        CHECK(helios::test::bits(v.x) == g.x);
        CHECK(helios::test::bits(v.y) == g.y);
        CHECK(helios::test::bits(v.z) == g.z);
    }
    struct LatLonGolden {
        LatLon ll;
        u64 x, y, z;
    };
    // clang-format off
    static const LatLonGolden lls[] = {
        {LatLon{0.5, 1.0}, 0x3fe7a1776aa8f842ull, 0x3fdeaee8744b05f0ull, 0x3fde58a2b0543c86ull},
        {LatLon{-1.2, -2.8999999999999999}, 0xbfb6319171333ff3ull, 0xbfedd343a21a55c4ull, 0xbfd68473ad92c88aull},
        {LatLon{0.0001, 3.1400000000000001}, 0x3f5a181209fbb508ull, 0x3f1a36e2ea609cc8ull, 0xbfeffffd546ac434ull},
    };
    // clang-format on
    for (const LatLonGolden& g : lls) {
        const DVec3 v = latLonToDirection(g.ll);
        CHECK(helios::test::bits(v.x) == g.x);
        CHECK(helios::test::bits(v.y) == g.y);
        CHECK(helios::test::bits(v.z) == g.z);
    }
}
