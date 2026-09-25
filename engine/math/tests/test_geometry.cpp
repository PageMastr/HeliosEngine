#include "test_util.h"

using namespace helios;
using helios::test::Rng;

TEST_CASE("geometry: AABB basics") {
    DAABB b;
    CHECK(b.isEmpty());
    b.expand(DVec3(1.0, 2.0, 3.0));
    CHECK_FALSE(b.isEmpty());
    CHECK(b.volume() == 0.0);
    b.expand(DVec3(-1.0, 4.0, 0.0));
    CHECK(b.min == DVec3(-1.0, 2.0, 0.0));
    CHECK(b.max == DVec3(1.0, 4.0, 3.0));
    CHECK(b.center() == DVec3(0.0, 3.0, 1.5));
    CHECK(b.extents() == DVec3(1.0, 1.0, 1.5));
    CHECK(b.size() == DVec3(2.0, 2.0, 3.0));
    CHECK(b.volume() == 12.0);
    CHECK(b.surfaceArea() == 2.0 * (4.0 + 6.0 + 6.0));
    CHECK(b.contains(DVec3(0.0, 3.0, 1.0)));
    CHECK(b.contains(DVec3(1.0, 4.0, 3.0)));  // boundary counts
    CHECK_FALSE(b.contains(DVec3(1.1, 3.0, 1.0)));
    CHECK(b.inflated(0.5).contains(DVec3(1.4, 4.4, 3.4)));
    CHECK(b.contains(DAABB::fromCenterExtents(b.center(), DVec3(0.5))));
    const DVec3 pts[] = {{0.0, 0.0, 0.0}, {5.0, -1.0, 2.0}, {1.0, 1.0, -3.0}};
    const DAABB fp = DAABB::fromPoints(pts, 3);
    CHECK(fp.min == DVec3(0.0, -1.0, -3.0));
    CHECK(fp.max == DVec3(5.0, 1.0, 2.0));
    const DAABB m = merge(b, fp);
    CHECK(m.contains(b));
    CHECK(m.contains(fp));
    DAABB e2 = DAABB::empty();
    e2.expand(fp);
    CHECK(e2 == fp);
    CHECK(toF32(fp).max == Vec3(5.0f, 1.0f, 2.0f));
    CHECK(toF64(toF32(fp)) == fp);
}

TEST_CASE("geometry: transformed AABB is the tight box of the transformed corners") {
    Rng rng(60);
    for (int i = 0; i < 300; ++i) {
        const DAABB b = DAABB::fromCenterExtents(rng.dvec3(-10.0, 10.0), rng.dvec3(0.1, 5.0));
        const DMat4 m = DMat4::trs(rng.dvec3(-50.0, 50.0), rng.quatD(), rng.dvec3(0.2, 3.0));
        DAABB ref;
        for (int c = 0; c < 8; ++c) {
            const DVec3 corner{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y,
                               (c & 4) ? b.max.z : b.min.z};
            ref.expand(transformPoint(m, corner));
        }
        const DAABB t = transformAABB(b, m);
        CHECK(approxEqual(t.min, ref.min, 1e-9));
        CHECK(approxEqual(t.max, ref.max, 1e-9));
    }
    CHECK(transformAABB(DAABB::empty(), DMat4::scaling(DVec3(2.0))).isEmpty());
}

TEST_CASE("geometry: planes") {
    const DPlane p = DPlane::fromPointNormal({0.0, 2.0, 0.0}, {0.0, 1.0, 0.0});
    CHECK(p.signedDistance({5.0, 3.0, -1.0}) == 1.0);
    CHECK(p.signedDistance({5.0, 0.0, -1.0}) == -2.0);
    CHECK(closestPoint(p, DVec3(5.0, 7.0, 1.0)) == DVec3(5.0, 2.0, 1.0));
    // Counter-clockwise points give a normal facing the viewer.
    const DPlane q = DPlane::fromPoints({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    CHECK(q.normal == DVec3(0.0, 0.0, 1.0));
    const DPlane n = DPlane{{0.0, 0.0, 4.0}, 8.0}.normalized();
    CHECK(n.normal == DVec3(0.0, 0.0, 1.0));
    CHECK(n.d == 2.0);
}

TEST_CASE("geometry: closest points match brute force") {
    Rng rng(61);
    for (int i = 0; i < 300; ++i) {
        const DVec3 p = rng.dvec3(-6.0, 6.0);
        // Triangle: compare with dense barycentric sampling.
        const DVec3 a = rng.dvec3(-3.0, 3.0), b = rng.dvec3(-3.0, 3.0), c = rng.dvec3(-3.0, 3.0);
        const DVec3 cp = closestPointOnTriangle(a, b, c, p);
        f64 best = kInfinityD;
        const int n = 60;
        for (int u = 0; u <= n; ++u) {
            for (int v = 0; v + u <= n; ++v) {
                const DVec3 s = a + (b - a) * (u / f64(n)) + (c - a) * (v / f64(n));
                best = min(best, distance(s, p));
            }
        }
        CHECK(distance(cp, p) <= best + 1e-12);
        CHECK(distance(cp, p) >= best - 0.25);  // sampling resolution
        DVec3 uvw;
        REQUIRE(barycentric(a, b, c, cp, uvw));
        CHECK(uvw.x >= -1e-9);
        CHECK(uvw.y >= -1e-9);
        CHECK(uvw.z >= -1e-9);
        CHECK(approxEqual(uvw.x + uvw.y + uvw.z, 1.0, 1e-12));

        // Segment.
        f64 t = 0.0;
        const DVec3 sp = closestPointOnSegment(a, b, p, t);
        CHECK(t >= 0.0);
        CHECK(t <= 1.0);
        for (int k = 0; k <= 50; ++k) CHECK(distance(sp, p) <= distance(lerp(a, b, k / 50.0), p) + 1e-12);

        // Box / OBB / sphere.
        const DAABB box = DAABB::fromCenterExtents(rng.dvec3(-2.0, 2.0), rng.dvec3(0.5, 2.0));
        const DVec3 bp = closestPoint(box, p);
        CHECK(box.contains(bp));
        CHECK(approxEqual(distanceSq(box, p), distanceSq(bp, p), 1e-12));
        const DOBB obb{box.center(), box.extents(), rng.quatD()};
        const DVec3 op = closestPoint(obb, p);
        CHECK(obb.contains(op + (obb.center - op) * 1e-12));
        if (obb.contains(p)) CHECK(approxEqual(op, p, 1e-12));
        const DSphere sph{rng.dvec3(-2.0, 2.0), 1.5};
        CHECK(approxEqual(distance(closestPoint(sph, p), sph.center), 1.5, 1e-12));
    }
    // Degenerate inputs.
    CHECK(closestPointOnTriangle(DVec3(1.0), DVec3(1.0), DVec3(1.0), DVec3(0.0)) == DVec3(1.0));
    CHECK(closestPointOnSegment(DVec3(2.0), DVec3(2.0), DVec3(0.0)) == DVec3(2.0));
    const DRay ray{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}};
    CHECK(closestPointOnRay(ray, DVec3(-5.0, 1.0, 0.0)) == DVec3(0.0));
    CHECK(closestPointOnRay(ray, DVec3(5.0, 1.0, 0.0)) == DVec3(5.0, 0.0, 0.0));
    DVec3 uvw;
    CHECK_FALSE(barycentric(DVec3(0.0), DVec3(1.0), DVec3(2.0), DVec3(0.5), uvw));
    CHECK(approxEqual(triangleArea(DVec3(0.0), DVec3(2.0, 0.0, 0.0), DVec3(0.0, 3.0, 0.0)), 3.0));
    CHECK(triangleNormal(DVec3(0.0), DVec3(1.0, 0.0, 0.0), DVec3(0.0, 1.0, 0.0)) == DVec3(0.0, 0.0, 1.0));
}

TEST_CASE("geometry: ray vs AABB") {
    const AABB box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    f32 t0 = 0.0f, t1 = 0.0f;
    REQUIRE(intersectRayAABB(Ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box, t0, t1));
    CHECK(t0 == 4.0f);
    CHECK(t1 == 6.0f);
    REQUIRE(intersectRayAABB(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, box, t0, t1));  // inside
    CHECK(t0 == 0.0f);
    CHECK(t1 == 1.0f);
    CHECK_FALSE(intersects(Ray{{-5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}}, box));  // pointing away
    CHECK_FALSE(intersects(Ray{{-5.0f, 2.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box));   // zero dy, outside slab
    CHECK(intersects(Ray{{-5.0f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}}, box));         // zero dy/dz, inside slabs
    CHECK(intersects(Ray{{-5.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box));         // grazing a face (0*inf)
    CHECK(intersects(Ray{{-5.0f, -0.0f, 0.0f}, {1.0f, -0.0f, 0.0f}}, box));       // negative zero direction

    // Random rays against a sampled reference.
    Rng rng(62);
    for (int i = 0; i < 2000; ++i) {
        const DAABB b = DAABB::fromCenterExtents(rng.dvec3(-3.0, 3.0), rng.dvec3(0.2, 2.0));
        const DRay r{rng.dvec3(-8.0, 8.0), rng.unitD()};
        f64 a0 = 0.0, a1 = 0.0;
        const bool hit = intersectRayAABB(r, b, a0, a1);
        if (hit) {
            CHECK(a0 <= a1);
            CHECK(b.inflated(1e-9).contains(r.at(a0)));
            CHECK(b.inflated(1e-9).contains(r.at(a1)));
            CHECK(b.inflated(1e-9).contains(r.at(0.5 * (a0 + a1))));
            if (a0 > 0.0) CHECK_FALSE(b.inflated(-1e-9).contains(r.at(a0 - 1e-6)));
        } else {
            bool anyInside = false;
            for (int k = 0; k <= 400 && !anyInside; ++k)
                anyInside = b.inflated(-1e-6).contains(r.at(k * 0.05));
            CHECK_FALSE(anyInside);
        }
        // OBB: identical to the AABB test in the box frame.
        const DOBB obb{b.center(), b.extents(), rng.quatD()};
        f64 o0 = 0.0, o1 = 0.0;
        if (intersectRayOBB(r, obb, o0, o1)) {
            CHECK(obb.contains(r.at(0.5 * (o0 + o1))));
        } else {
            bool anyInside = false;
            for (int k = 0; k <= 400 && !anyInside; ++k) {
                const DVec3 q = r.at(k * 0.05);
                const DOBB shrunk{obb.center, obb.halfExtents - DVec3(1e-6), obb.rotation};
                anyInside = shrunk.contains(q);
            }
            CHECK_FALSE(anyInside);
        }
    }
}

TEST_CASE("geometry: ray vs sphere") {
    const Sphere s{{0.0f, 0.0f, -10.0f}, 2.0f};
    f32 t0 = 0.0f, t1 = 0.0f;
    REQUIRE(intersectRaySphere(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, s, t0, t1));
    CHECK(t0 == 8.0f);
    CHECK(t1 == 12.0f);
    CHECK_FALSE(intersectRaySphere(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, s, t0, t1));
    CHECK_FALSE(intersectRaySphere(Ray{{0.0f, 2.5f, 0.0f}, {0.0f, 0.0f, -1.0f}}, s, t0, t1));
    REQUIRE(intersectRaySphere(Ray{{0.0f, 0.0f, -10.0f}, {1.0f, 0.0f, 0.0f}}, s, t0, t1));  // from inside
    CHECK(t0 == 0.0f);
    CHECK(t1 == 2.0f);
    REQUIRE(intersectRaySphere(Ray{{0.0f, 2.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, s, t0, t1));  // tangent
    CHECK(approxEqual(t0, 10.0f, 1e-4f));
    // Non-unit direction: t is in units of the direction length.
    REQUIRE(intersectRaySphere(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -2.0f}}, s, t0, t1));
    CHECK(t0 == 4.0f);
    // Small sphere far away: the robust discriminant keeps the hit.
    const DSphere far{{0.0, 0.0, -1e9}, 0.5};
    f64 d0 = 0.0, d1 = 0.0;
    REQUIRE(intersectRaySphere(DRay{{0.0, 0.3, 0.0}, {0.0, 0.0, -1.0}}, far, d0, d1));
    CHECK(approxEqual(d0, 1e9 - 0.4, 1e-6));
    CHECK(approxEqual(d1, 1e9 + 0.4, 1e-6));
    Rng rng(63);
    for (int i = 0; i < 2000; ++i) {
        const DSphere sp{rng.dvec3(-3.0, 3.0), rng.range(0.2, 2.0)};
        const DRay r{rng.dvec3(-8.0, 8.0), rng.unitD()};
        f64 a0 = 0.0, a1 = 0.0;
        if (intersectRaySphere(r, sp, a0, a1)) {
            CHECK(approxEqual(distance(r.at(a1), sp.center), sp.radius, 1e-9));
            if (a0 > 0.0) CHECK(approxEqual(distance(r.at(a0), sp.center), sp.radius, 1e-9));
        } else {
            CHECK(distance(closestPointOnRay(r, sp.center), sp.center) > sp.radius - 1e-9);
        }
    }
}

TEST_CASE("geometry: ray vs plane and triangle") {
    const Plane ground = Plane::fromPointNormal({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    f32 t = 0.0f;
    REQUIRE(intersectRayPlane(Ray{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}}, ground, t));
    CHECK(t == 5.0f);
    CHECK_FALSE(intersectRayPlane(Ray{{0.0f, 5.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, ground, t));  // parallel
    CHECK_FALSE(intersectRayPlane(Ray{{0.0f, 5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, ground, t));  // behind

    const Vec3 a{-1.0f, 0.0f, 0.0f}, b{1.0f, 0.0f, 0.0f}, c{0.0f, 2.0f, 0.0f};  // CCW seen from +Z
    f32 u = 0.0f, v = 0.0f;
    REQUIRE(intersectRayTriangle(Ray{{0.0f, 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c, t, u, v));
    CHECK(t == 5.0f);
    CHECK(approxEqual((a * (1.0f - u - v) + b * u + c * v), Vec3(0.0f, 0.5f, 0.0f)));
    // Back face: hit only when culling is off.
    const Ray fromBehind{{0.0f, 0.5f, -5.0f}, {0.0f, 0.0f, 1.0f}};
    CHECK(intersectRayTriangle(fromBehind, a, b, c, t, u, v));
    CHECK_FALSE(intersectRayTriangle(fromBehind, a, b, c, t, u, v, true));
    CHECK(intersectRayTriangle(Ray{{0.0f, 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c, t, u, v, true));
    CHECK_FALSE(
        intersectRayTriangle(Ray{{2.0f, 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c, t, u, v));  // outside
    CHECK_FALSE(
        intersectRayTriangle(Ray{{0.0f, 0.5f, 5.0f}, {1.0f, 0.0f, 0.0f}}, a, b, c, t, u, v));  // parallel
    CHECK_FALSE(
        intersectRayTriangle(Ray{{0.0f, 0.5f, -5.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c, t, u, v));  // behind
    // Regression: a (near-)degenerate triangle whose determinant is subnormal overflowed 1/det to
    // infinity and reported a hit with NaN barycentrics and t = inf.
    {
        f64 dt = 0.0, du = 0.0, dv = 0.0;
        const DRay down{{0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}};
        const DVec3 tinyB{1e-160, 0.0, 0.0}, tinyC{0.0, 1e-160, 0.0};
        CHECK_FALSE(intersectRayTriangle(down, DVec3(0.0), tinyB, tinyC, dt, du, dv));
        // Sliver: ordinary edge lengths, third vertex collinear up to a subnormal offset.
        CHECK_FALSE(intersectRayTriangle(Ray{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}, Vec3(0.0f),
                                         Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f, 1e-40f, 0.0f), t, u, v));
        CHECK_FALSE(intersectRayTriangle(Ray{{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}, Vec3(0.0f),
                                         Vec3(1e-20f, 0.0f, 0.0f), Vec3(0.0f, 1e-20f, 0.0f), t, u, v, true));
        // Non-finite ray input is a miss, not a NaN hit.
        const Ray nanRay{{std::nanf(""), 0.5f, 5.0f}, {0.0f, 0.0f, -1.0f}};
        CHECK_FALSE(intersectRayTriangle(nanRay, a, b, c, t, u, v));
    }
    // Random rays: agrees with plane intersection + barycentric containment.
    Rng rng(64);
    for (int i = 0; i < 3000; ++i) {
        const DVec3 ta = rng.dvec3(-2.0, 2.0), tb = rng.dvec3(-2.0, 2.0), tc = rng.dvec3(-2.0, 2.0);
        const DRay r{rng.dvec3(-5.0, 5.0), rng.unitD()};
        f64 tt = 0.0, uu = 0.0, vv = 0.0;
        const bool hit = intersectRayTriangle(r, ta, tb, tc, tt, uu, vv);
        f64 tp = 0.0;
        bool ref = false;
        DVec3 w;
        if (intersectRayPlane(r, DPlane::fromPoints(ta, tb, tc), tp) &&
            barycentric(ta, tb, tc, r.at(tp), w)) {
            const f64 m = min(min(w.x, w.y), w.z);
            if (std::fabs(m) < 1e-9) continue;  // too close to an edge to compare
            ref = m > 0.0;
        }
        CHECK(hit == ref);
        if (hit && ref) CHECK(approxEqual(tt, tp, 1e-9 * max(1.0, tp)));
    }
}

TEST_CASE("geometry: overlap tests") {
    const AABB a{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    CHECK(intersects(a, AABB{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}}));  // touching corner
    CHECK_FALSE(intersects(a, AABB{{1.1f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}}));
    CHECK(intersects(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, Sphere{{2.0f, 0.0f, 0.0f}, 1.0f}));
    CHECK_FALSE(intersects(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, Sphere{{2.1f, 0.0f, 0.0f}, 1.0f}));
    CHECK(intersects(Sphere{{2.0f, 0.5f, 0.5f}, 1.0f}, a));
    CHECK_FALSE(intersects(a, Sphere{{2.0f, 2.0f, 2.0f}, 1.0f}));  // near the corner but outside
    CHECK(intersects(Sphere{{0.5f, 0.5f, 0.5f}, 0.1f}, a));        // fully inside
    const Plane pl = Plane::fromPointNormal({0.0f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f});
    CHECK(intersects(a, pl));
    CHECK_FALSE(intersects(AABB{{0.0f, 0.6f, 0.0f}, {1.0f, 1.0f, 1.0f}}, pl));
    CHECK(intersects(Sphere{{0.0f, 1.0f, 0.0f}, 0.5f}, pl));
    CHECK_FALSE(intersects(Sphere{{0.0f, 1.1f, 0.0f}, 0.5f}, pl));
    const Sphere m = merge(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, Sphere{{4.0f, 0.0f, 0.0f}, 1.0f});
    CHECK(approxEqual(m.center, Vec3(2.0f, 0.0f, 0.0f)));
    CHECK(approxEqual(m.radius, 3.0f));
    CHECK(merge(Sphere{{0.0f, 0.0f, 0.0f}, 5.0f}, Sphere{{1.0f, 0.0f, 0.0f}, 1.0f}).radius == 5.0f);
    CHECK(approxEqual(boundingSphere(a).radius, std::sqrt(0.75f)));
    CHECK(bounds(Sphere{{1.0f, 1.0f, 1.0f}, 2.0f}).max == Vec3(3.0f));
    CHECK(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}.contains(Vec3(0.0f, 1.0f, 0.0f)));
}

TEST_CASE("geometry: OBB separating-axis test") {
    const DOBB a{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, DQuat{}};
    CHECK(intersects(a, DOBB{{1.9, 0.0, 0.0}, {1.0, 1.0, 1.0}, DQuat{}}));
    CHECK_FALSE(intersects(a, DOBB{{2.1, 0.0, 0.0}, {1.0, 1.0, 1.0}, DQuat{}}));
    // Rotated 45° about Z reaches sqrt(2) along X.
    const DQuat r45 = DQuat::fromAxisAngle(DVec3::unitZ(), kQuarterPiD);
    CHECK(intersects(a, DOBB{{2.3, 0.0, 0.0}, {1.0, 1.0, 1.0}, r45}));
    CHECK_FALSE(intersects(a, DOBB{{2.5, 0.0, 0.0}, {1.0, 1.0, 1.0}, r45}));
    // Edge-edge case: two boxes whose closest features are crossing edges. Only an edge x edge
    // axis separates them (face axes all overlap).
    const DQuat ra = DQuat::fromAxisAngle(DVec3::unitX(), kQuarterPiD);  // edge along X, pointing +Y
    const DQuat rb = DQuat::fromAxisAngle(DVec3::unitZ(), kQuarterPiD);  // edge along Z, pointing -Y
    const DOBB ea{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, ra};
    const f64 reach = std::sqrt(2.0);
    CHECK_FALSE(intersects(ea, DOBB{{0.0, 2.0 * reach + 0.05, 0.0}, {1.0, 1.0, 1.0}, rb}));
    CHECK(intersects(ea, DOBB{{0.0, 2.0 * reach - 0.05, 0.0}, {1.0, 1.0, 1.0}, rb}));
    // Random: if SAT reports separation no sampled point of A lies inside B; if any sample lies
    // inside B, SAT must report overlap. Also consistent with the AABB test when unrotated.
    Rng rng(65);
    for (int i = 0; i < 500; ++i) {
        const DOBB p{rng.dvec3(-2.0, 2.0), rng.dvec3(0.2, 1.5), rng.quatD()};
        const DOBB q{rng.dvec3(-2.0, 2.0), rng.dvec3(0.2, 1.5), rng.quatD()};
        const bool sat = intersects(p, q);
        CHECK(sat == intersects(q, p));
        bool sampleInside = false;
        for (int k = 0; k < 400 && !sampleInside; ++k) {
            const DVec3 local = rng.dvec3(-1.0, 1.0) * p.halfExtents;
            sampleInside = q.contains(p.center + p.rotation * local);
        }
        if (sampleInside) CHECK(sat);
        const DAABB ab = DAABB::fromCenterExtents(p.center, p.halfExtents);
        const DAABB bb = DAABB::fromCenterExtents(q.center, q.halfExtents);
        CHECK(intersects(DOBB{p.center, p.halfExtents, DQuat{}}, DOBB{q.center, q.halfExtents, DQuat{}}) ==
              intersects(ab, bb));
    }
    // bounds() of an OBB encloses its corners.
    const DOBB o{{1.0, 2.0, 3.0}, {1.0, 2.0, 0.5}, rng.quatD()};
    const DAABB ob = o.bounds();
    for (int c = 0; c < 8; ++c) {
        const DVec3 l{(c & 1) ? 1.0 : -1.0, (c & 2) ? 2.0 : -2.0, (c & 4) ? 0.5 : -0.5};
        CHECK(ob.inflated(1e-12).contains(o.center + o.rotation * l));
    }
    CHECK(approxEqual(o.axis(0), axisX(o.rotation), 1e-15));
    CHECK(approxEqual(o.axis(2), axisZ(o.rotation), 1e-15));
}

TEST_CASE("geometry: frustum extraction under reverse-Z") {
    const f32 fov = radians(60.0f), aspect = 1.5f, zn = 0.1f;
    const Vec3 eye{0.0f, 0.0f, 0.0f};
    const Mat4 view = Mat4::lookAt(eye, {0.0f, 0.0f, -1.0f}, kWorldUp);
    for (int variant = 0; variant < 3; ++variant) {
        Mat4 proj = variant == 1 ? Mat4::perspectiveReverseZ(fov, aspect, zn, 100.0f)
                                 : Mat4::perspectiveReverseZ(fov, aspect, zn);
        if (variant == 2) proj = flipClipY(proj);  // Y-flipped projections cull identically
        const Frustum f = Frustum::fromMatrix(proj * view);
        for (const Plane& p : f.planes)
            CHECK((isNormalized(p.normal) || (p.normal == Vec3{} && p.d == 1.0f)));
        CHECK(f.contains({0.0f, 0.0f, -1.0f}));
        CHECK(f.contains({0.0f, 0.0f, -50.0f}));
        CHECK_FALSE(f.contains({0.0f, 0.0f, 1.0f}));       // behind the camera
        CHECK_FALSE(f.contains({0.0f, 0.0f, -0.05f}));     // in front of the near plane
        CHECK_FALSE(f.contains({100.0f, 0.0f, -10.0f}));   // right of the frustum
        CHECK_FALSE(f.contains({0.0f, 100.0f, -10.0f}));   // above
        CHECK_FALSE(f.contains({0.0f, -100.0f, -10.0f}));  // below
        const bool finite = variant == 1;
        CHECK(f.contains({0.0f, 0.0f, -1e6f}) == !finite);  // infinite far plane never culls
        if (!finite) CHECK(f.plane(FrustumPlane::Far).normal == Vec3{});
        CHECK(f.intersects(Sphere{{0.0f, 0.0f, 1.0f}, 1.5f}));  // straddles the near plane
        CHECK_FALSE(f.intersects(Sphere{{0.0f, 0.0f, 5.0f}, 1.0f}));
        CHECK(f.classify(Sphere{{0.0f, 0.0f, -20.0f}, 1.0f}) == Containment::Inside);
        CHECK(f.classify(Sphere{{0.0f, 0.0f, 5.0f}, 1.0f}) == Containment::Outside);
        CHECK(f.classify(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}) == Containment::Intersects);
        CHECK(f.intersects(AABB{{-1.0f, -1.0f, -11.0f}, {1.0f, 1.0f, -9.0f}}));
        CHECK_FALSE(f.intersects(AABB{{50.0f, -1.0f, -11.0f}, {52.0f, 1.0f, -9.0f}}));
        CHECK(f.classify(AABB{{-1.0f, -1.0f, -11.0f}, {1.0f, 1.0f, -9.0f}}) == Containment::Inside);
        CHECK(f.classify(AABB{{-100.0f, -1.0f, -11.0f}, {1.0f, 1.0f, -9.0f}}) == Containment::Intersects);
        CHECK(f.classify(AABB{{50.0f, -1.0f, -11.0f}, {52.0f, 1.0f, -9.0f}}) == Containment::Outside);
    }
    // Plane placement: the side planes pass through the frustum edges.
    const Frustum f = Frustum::fromMatrix(Mat4::perspectiveReverseZ(fov, aspect, zn) * view);
    const f32 halfH = std::tan(fov * 0.5f) * 10.0f;
    CHECK(approxEqual(f.plane(FrustumPlane::Top).signedDistance({0.0f, halfH, -10.0f}), 0.0f, 1e-5f));
    CHECK(approxEqual(f.plane(FrustumPlane::Right).signedDistance({halfH * aspect, 0.0f, -10.0f}), 0.0f,
                      1e-5f));
    CHECK(approxEqual(f.plane(FrustumPlane::Near).signedDistance({0.0f, 0.0f, -zn}), 0.0f, 1e-6f));
}

TEST_CASE("geometry: f64 frustum culls far from the origin and matches sampling") {
    const DVec3 eye{4.0e11, -2.0e10, 1.0e11};
    const DVec3 dir = normalize(DVec3(1.0, 0.2, -0.5));
    const DMat4 view = DMat4::lookAt(eye, eye + dir, DVec3::unitY());
    const DMat4 proj = DMat4::perspectiveReverseZ(radians(75.0), 16.0 / 9.0, 0.1);
    const DFrustum f = DFrustum::fromMatrix(proj * view);
    CHECK(f.contains(eye + dir * 1000.0));
    CHECK_FALSE(f.contains(eye - dir * 1000.0));
    // Random points: frustum test == clip-space test.
    Rng rng(66);
    const DMat4 vp = proj * view;
    int inside = 0;
    for (int i = 0; i < 5000; ++i) {
        const DVec3 p = eye + rng.dvec3(-500.0, 500.0);
        const DVec4 c = vp * DVec4(p, 1.0);
        const bool clipInside =
            c.w > 0.0 && std::fabs(c.x) <= c.w && std::fabs(c.y) <= c.w && c.z >= 0.0 && c.z <= c.w;
        const f64 margin = 1e-2;  // both tests carry ~1e-4 m of cancellation error at 4e11 m
        bool nearBoundary = false;
        for (const DPlane& pl : f.planes)
            nearBoundary = nearBoundary || std::fabs(pl.signedDistance(p)) < margin;
        if (nearBoundary) continue;
        CHECK(f.contains(p) == clipInside);
        inside += clipInside ? 1 : 0;
    }
    CHECK(inside > 100);
}
