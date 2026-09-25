// Queries: ray and shape casts, layer masks, the ignore filter, and tie-breaking by (layer, key,
// subShape) instead of BodyID (02 §5.4 / §7.1: results must not depend on each system's BodyIDs).
#include <doctest/doctest.h>

#include <cmath>

#include "helios/physics/physics.h"

using namespace helios;
using namespace helios::physics;

namespace {

BodyDesc staticBox(BodyKey key, ObjectLayer layer, DVec3 pos, Vec3 half = Vec3(1.0f, 1.0f, 1.0f)) {
    BodyDesc d;
    d.key = key;
    d.layer = layer;
    d.motion = MotionType::Static;
    d.shape = createBox(half).value();
    d.position = pos;
    return d;
}

/// A grid holding two coincident boxes (exactly equal hit fractions), created in the given order,
/// after `churn` add/remove cycles so the BodyIDs differ between variants.
std::unique_ptr<PhysicsGrid> twinBoxes(BodyKey first, ObjectLayer firstLayer, BodyKey second, ObjectLayer secondLayer, u32 churn) {
    auto grid = PhysicsGrid::create({}).value();
    const ShapeRef ball = createSphere(0.1f).value();
    for (u32 i = 0; i < churn; ++i) {
        BodyDesc d;
        d.key = 1000 + i;
        d.shape = ball;
        d.position = DVec3(100.0 + i, 0.0, 0.0);
        const BodyHandle h = grid->createBody(d).value();
        if (i % 2 == 0) grid->destroyBody(h);
    }
    REQUIRE(grid->createBody(staticBox(first, firstLayer, DVec3(0, 0, 0))).ok());
    REQUIRE(grid->createBody(staticBox(second, secondLayer, DVec3(0, 0, 0))).ok());
    return grid;
}

} // namespace

TEST_CASE("queries: closest ray hit, point, normal and fraction") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({}).value();
    REQUIRE(grid->createBody(staticBox(7, ObjectLayer::Static, DVec3(0, 0, 0))).ok());
    REQUIRE(grid->createBody(staticBox(8, ObjectLayer::Static, DVec3(0, -5, 0))).ok());
    const auto hit = grid->castRay({DVec3(0.25, 10, 0.25), Vec3(0, -20, 0)});
    REQUIRE(hit.has_value());
    CHECK(hit->key == 7);
    CHECK(hit->layer == ObjectLayer::Static);
    CHECK(hit->body == grid->findBody(ObjectLayer::Static, 7));
    CHECK(hit->fraction == doctest::Approx(9.0 / 20.0).epsilon(1e-4));
    CHECK(hit->point.y == doctest::Approx(1.0).epsilon(1e-4));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1e-4));
    // Too short: no hit.
    CHECK_FALSE(grid->castRay({DVec3(0.25, 10, 0.25), Vec3(0, -5, 0)}).has_value());
    // All hits, near to far.
    const auto all = grid->castRayAll({DVec3(0.25, 10, 0.25), Vec3(0, -20, 0)});
    REQUIRE(all.size() == 2);
    CHECK(all[0].key == 7);
    CHECK(all[1].key == 8);
    CHECK(all[0].fraction < all[1].fraction);
}

TEST_CASE("queries: layer masks and the ignore filter") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({}).value();
    const BodyHandle top = grid->createBody(staticBox(7, ObjectLayer::Terrain, DVec3(0, 0, 0))).value();
    REQUIRE(grid->createBody(staticBox(8, ObjectLayer::Static, DVec3(0, -5, 0))).ok());
    RayCast ray{DVec3(0, 10, 0), Vec3(0, -20, 0)};
    ray.layers = ObjectLayerMask::none().add(ObjectLayer::Static);
    auto hit = grid->castRay(ray);
    REQUIRE(hit.has_value());
    CHECK(hit->key == 8);
    ray.layers = ObjectLayerMask::all();
    ray.ignore = top;
    hit = grid->castRay(ray);
    REQUIRE(hit.has_value());
    CHECK(hit->key == 8);
    ray.layers = ObjectLayerMask::none();
    ray.ignore = {};
    CHECK_FALSE(grid->castRay(ray).has_value());
    CHECK(grid->castRayAll(ray).empty());
    // Non-finite input is rejected, not propagated into Jolt.
    CHECK_FALSE(grid->castRay({DVec3(std::nan(""), 0, 0), Vec3(0, -1, 0)}).has_value());
}

TEST_CASE("queries: equal-fraction ray hits break ties by (layer, key), not by BodyID or creation order") {
    PhysicsRuntime runtime;
    const RayCast ray{DVec3(0.3, 10, -0.2), Vec3(0, -20, 0)};
    // Same layer: the lower key wins, whatever the order and the BodyIDs.
    for (u32 churn : {0u, 7u, 64u}) {
        auto a = twinBoxes(5, ObjectLayer::Static, 3, ObjectLayer::Static, churn);
        auto b = twinBoxes(3, ObjectLayer::Static, 5, ObjectLayer::Static, churn + 1);
        const auto ha = a->castRay(ray), hb = b->castRay(ray);
        REQUIRE(ha.has_value());
        REQUIRE(hb.has_value());
        CHECK(ha->fraction == hb->fraction);
        CHECK(ha->key == 3);
        CHECK(hb->key == 3);
        const auto allA = a->castRayAll(ray), allB = b->castRayAll(ray);
        REQUIRE(allA.size() == 2);
        REQUIRE(allB.size() == 2);
        CHECK(allA[0].key == 3);
        CHECK(allA[1].key == 5);
        CHECK(allB[0].key == 3);
        CHECK(allB[1].key == 5);
    }
    // Different layers: the lower layer wins even with the larger key.
    auto c = twinBoxes(2, ObjectLayer::Terrain, 9, ObjectLayer::Static, 3);
    const auto hc = c->castRay(ray);
    REQUIRE(hc.has_value());
    CHECK(hc->layer == ObjectLayer::Static);
    CHECK(hc->key == 9);
}

TEST_CASE("queries: shape casts hit, report contact data and break ties by key") {
    PhysicsRuntime runtime;
    ShapeCast cast;
    cast.shape = createSphere(0.5f).value();
    cast.position = DVec3(0.2, 10, 0.1);
    cast.direction = Vec3(0, -20, 0);
    auto a = twinBoxes(11, ObjectLayer::Static, 4, ObjectLayer::Static, 5);
    auto b = twinBoxes(4, ObjectLayer::Static, 11, ObjectLayer::Static, 0);
    const auto ha = a->castShape(cast), hb = b->castShape(cast);
    REQUIRE(ha.has_value());
    REQUIRE(hb.has_value());
    CHECK(ha->key == 4);
    CHECK(hb->key == 4);
    CHECK(ha->fraction == hb->fraction);
    // The sphere touches the box top (y = 1) when its centre is at y = 1.5: 8.5 of 20 m.
    CHECK(ha->fraction == doctest::Approx(8.5 / 20.0).epsilon(1e-3));
    CHECK(ha->contactPoint.y == doctest::Approx(1.0).epsilon(1e-3));
    CHECK(ha->normal.y == doctest::Approx(1.0f).epsilon(1e-3));
    // Nothing below the start in this direction.
    cast.direction = Vec3(0, 20, 0);
    CHECK_FALSE(a->castShape(cast).has_value());
    cast.shape = ShapeRef{};
    CHECK_FALSE(a->castShape(cast).has_value());
}

TEST_CASE("queries: mesh hits report the triangle's sub-shape id") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({}).value();
    const Vec3 verts[] = {{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}};
    const u32 idx[] = {0, 2, 1, 0, 3, 2};
    BodyDesc d;
    d.key = 77;
    d.layer = ObjectLayer::Terrain;
    d.motion = MotionType::Static;
    d.shape = createMesh(verts, idx).value();
    REQUIRE(grid->createBody(d).ok());
    const auto h1 = grid->castRay({DVec3(1.0, 5, -0.5), Vec3(0, -10, 0)});
    const auto h2 = grid->castRay({DVec3(-1.0, 5, 0.5), Vec3(0, -10, 0)});
    REQUIRE(h1.has_value());
    REQUIRE(h2.has_value());
    CHECK(h1->key == 77);
    CHECK(h1->subShape != h2->subShape); // two different triangles
    CHECK(h1->fraction == doctest::Approx(0.5f));
    CHECK(h1->normal.y == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("queries: a shape cast with a shape Jolt cannot sweep (mesh) is refused, not dispatched") {
    // Jolt has no cast function for a MeshShape (or a compound holding one) as the swept shape; its
    // dispatch table asserts in debug builds and reports nothing in release builds.
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({}).value();
    REQUIRE(grid->createBody(staticBox(7, ObjectLayer::Static, DVec3(0, 0, 0))).ok());
    const Vec3 verts[] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}};
    const u32 idx[] = {0, 2, 1};
    ShapeCast cast;
    cast.shape = createMesh(verts, idx).value();
    cast.position = DVec3(0, 10, 0);
    cast.direction = Vec3(0, -20, 0);
    CHECK_FALSE(grid->castShape(cast).has_value());
    cast.rotation = Quat(0.0f, 0.0f, 0.0f, 0.0f);
    cast.shape = createSphere(0.5f).value();
    CHECK_FALSE(grid->castShape(cast).has_value()); // a zero rotation is invalid input
    cast.rotation = Quat::identity();
    CHECK(grid->castShape(cast).has_value());
}

TEST_CASE("queries: zero-length rays and casts return finite results or none") {
    // A zero direction reaches Jolt's ray/shape tests as a degenerate segment; the tie-break sort
    // needs a strict weak order, so no NaN fraction may reach it.
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({}).value();
    REQUIRE(grid->createBody(staticBox(1, ObjectLayer::Static, DVec3(0, 0, 0))).ok());
    BodyDesc sphere = staticBox(2, ObjectLayer::Static, DVec3(0.5, 0, 0));
    sphere.shape = createSphere(1.0f).value();
    REQUIRE(grid->createBody(sphere).ok());
    BodyDesc capsule = staticBox(3, ObjectLayer::Static, DVec3(-0.5, 0, 0));
    capsule.shape = createCapsule(1.0f, 0.5f).value();
    REQUIRE(grid->createBody(capsule).ok());
    const Vec3 verts[] = {{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}};
    const u32 idx[] = {0, 2, 1, 0, 3, 2};
    BodyDesc mesh = staticBox(4, ObjectLayer::Terrain, DVec3(0, 0.25, 0));
    mesh.shape = createMesh(verts, idx).value();
    REQUIRE(grid->createBody(mesh).ok());
    for (const DVec3 origin : {DVec3(0.1, 0.25, 0.1), DVec3(0.2, 0.1, -0.3), DVec3(5, 5, 5)}) {
        const RayCast ray{origin, Vec3{}};
        if (const auto hit = grid->castRay(ray)) CHECK(std::isfinite(hit->fraction));
        for (const RayHit& h : grid->castRayAll(ray)) CHECK(std::isfinite(h.fraction));
        ShapeCast cast;
        cast.shape = createSphere(0.2f).value();
        cast.position = origin;
        if (const auto hit = grid->castShape(cast)) {
            CHECK(std::isfinite(hit->fraction));
            CHECK(std::isfinite(hit->penetration));
        }
    }
}
