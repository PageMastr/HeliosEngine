// Grid and body lifecycle: stable keys, handles, validation, capacity, fixed stepping.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helios/physics/physics.h"

using namespace helios;
using namespace helios::physics;

namespace {

BodyDesc sphereAt(BodyKey key, DVec3 pos, ShapeRef shape, ObjectLayer layer = ObjectLayer::Dynamic) {
    BodyDesc d;
    d.key = key;
    d.layer = layer;
    d.motion = MotionType::Dynamic;
    d.shape = std::move(shape);
    d.position = pos;
    return d;
}

BodyDesc groundDesc() {
    BodyDesc d;
    d.key = 1;
    d.layer = ObjectLayer::Terrain;
    d.motion = MotionType::Static;
    d.shape = createBox(Vec3(50.0f, 1.0f, 50.0f)).value();
    d.position = DVec3(0.0, -1.0, 0.0);
    return d;
}

} // namespace

TEST_CASE("grid: bodies are created, found by (layer, key) and destroyed") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    const ShapeRef ball = createSphere(0.5f).value();
    auto a = g.createBody(sphereAt(42, DVec3(0, 5, 0), ball));
    REQUIRE(a.ok());
    CHECK(g.contains(*a));
    CHECK(g.bodyCount() == 1);
    CHECK(g.findBody(ObjectLayer::Dynamic, 42) == *a);
    CHECK_FALSE(g.findBody(ObjectLayer::Debris, 42).isValid());
    CHECK(g.bodyKey(*a).value() == 42);
    CHECK(g.bodyLayer(*a).value() == ObjectLayer::Dynamic);
    const BodyState st = g.bodyState(*a).value();
    CHECK(st.position.y == 5.0);
    CHECK(st.motion == MotionType::Dynamic);
    CHECK(st.active);
    // The same key on another layer is a different body.
    auto b = g.createBody(sphereAt(42, DVec3(3, 5, 0), ball, ObjectLayer::Debris));
    REQUIRE(b.ok());
    CHECK(g.bodyCount() == 2);
    g.destroyBody(*a);
    CHECK_FALSE(g.contains(*a));
    CHECK(g.bodyState(*a).errorCode() == ErrorCode::NotFound);
    CHECK(g.setVelocity(*a, Vec3{}, Vec3{}).errorCode() == ErrorCode::NotFound);
    CHECK_FALSE(g.findBody(ObjectLayer::Dynamic, 42).isValid());
    g.destroyBody(*a); // stale: ignored
    CHECK(g.bodyCount() == 1);
    // A recreated body gets a fresh handle; the stale one stays stale.
    auto c = g.createBody(sphereAt(42, DVec3(0, 5, 0), ball));
    REQUIRE(c.ok());
    CHECK(*c != *a);
    CHECK_FALSE(g.contains(*a));
    CHECK(ball.useCount() >= 3); // bodies hold shape references
}

TEST_CASE("grid: validation rejects key 0, duplicates, wrong motion types and bad values") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    const ShapeRef ball = createSphere(0.5f).value();
    CHECK(g.createBody(sphereAt(0, DVec3{}, ball)).errorCode() == ErrorCode::InvalidArgument);
    REQUIRE(g.createBody(sphereAt(7, DVec3{}, ball)).ok());
    CHECK(g.createBody(sphereAt(7, DVec3(1, 0, 0), ball)).errorCode() == ErrorCode::AlreadyExists);
    CHECK(g.createBody(sphereAt(8, DVec3{}, ShapeRef{})).errorCode() == ErrorCode::InvalidArgument);
    BodyDesc dynamicTerrain = groundDesc();
    dynamicTerrain.motion = MotionType::Dynamic;
    CHECK(g.createBody(dynamicTerrain).errorCode() == ErrorCode::InvalidArgument);
    BodyDesc kin = sphereAt(9, DVec3{}, ball, ObjectLayer::Kinematic);
    CHECK(g.createBody(kin).errorCode() == ErrorCode::InvalidArgument); // Kinematic layer, Dynamic motion
    kin.motion = MotionType::Kinematic;
    CHECK(g.createBody(kin).ok());
    BodyDesc bad = sphereAt(10, DVec3(std::nan(""), 0, 0), ball);
    CHECK(g.createBody(bad).errorCode() == ErrorCode::InvalidArgument);
    CHECK(g.bodyCount() == 2);
    // Batches are all-or-nothing, and duplicates inside a batch are caught.
    const BodyDesc batch[] = {sphereAt(20, DVec3{}, ball), sphereAt(21, DVec3{}, ball), sphereAt(20, DVec3{}, ball)};
    CHECK(g.createBodies(batch).errorCode() == ErrorCode::AlreadyExists);
    CHECK(g.bodyCount() == 2);
    CHECK_FALSE(g.findBody(ObjectLayer::Dynamic, 21).isValid());
}

TEST_CASE("grid: capacity is enforced with an error") {
    PhysicsRuntime runtime;
    GridDesc gd;
    gd.maxBodies = 8;
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    const ShapeRef ball = createSphere(0.5f).value();
    for (BodyKey k = 1; k <= 8; ++k) REQUIRE((*grid)->createBody(sphereAt(k, DVec3(2.0 * k, 0, 0), ball)).ok());
    CHECK((*grid)->createBody(sphereAt(99, DVec3{}, ball)).errorCode() == ErrorCode::LimitExceeded);
    std::vector<BodyDesc> more = {sphereAt(100, DVec3{}, ball)};
    CHECK((*grid)->createBodies(more).errorCode() == ErrorCode::LimitExceeded);
    CHECK(PhysicsGrid::create({.maxBodies = 0}).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("grid: batches are added in (layer, key) order and handles follow the input order") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    const ShapeRef ball = createSphere(0.5f).value();
    const BodyDesc descs[] = {sphereAt(30, DVec3(0, 1, 0), ball), sphereAt(10, DVec3(0, 2, 0), ball),
                              sphereAt(20, DVec3(0, 3, 0), ball, ObjectLayer::Debris), groundDesc()};
    auto handles = (*grid)->createBodies(descs);
    REQUIRE(handles.ok());
    REQUIRE(handles->size() == 4);
    for (usize i = 0; i < 4; ++i) CHECK((*grid)->bodyKey((*handles)[i]).value() == descs[i].key);
    // bodies() is (layer, key) order: Terrain(1), Dynamic(10), Dynamic(30), Debris(20).
    const std::vector<BodyHandle> ordered = (*grid)->bodies();
    REQUIRE(ordered.size() == 4);
    CHECK((*grid)->bodyKey(ordered[0]).value() == 1);
    CHECK((*grid)->bodyKey(ordered[1]).value() == 10);
    CHECK((*grid)->bodyKey(ordered[2]).value() == 30);
    CHECK((*grid)->bodyKey(ordered[3]).value() == 20);
}

TEST_CASE("grid: fixed-step simulation (gravity, the accumulator, kinematic motion, sleeping)") {
    PhysicsRuntime runtime;
    GridDesc gd;
    gd.fixedDt = 1.0f / 50.0f;
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    REQUIRE(g.createBody(groundDesc()).ok());
    const ShapeRef ball = createSphere(0.5f).value();
    auto ballBody = g.createBody(sphereAt(2, DVec3(0, 10, 0), ball));
    REQUIRE(ballBody.ok());
    BodyDesc platform;
    platform.key = 3;
    platform.layer = ObjectLayer::Kinematic;
    platform.motion = MotionType::Kinematic;
    platform.shape = createBox(Vec3(1, 0.2f, 1)).value();
    platform.position = DVec3(10, 1, 0);
    auto plat = g.createBody(platform);
    REQUIRE(plat.ok());

    // Free fall: after 0.5 s (25 steps) the ball fell ~1.2 m (semi-implicit Euler, some damping).
    CHECK(g.advance(0.25).value() == 12);
    CHECK(g.advance(0.25).value() == 13); // 0.01 s carried over
    CHECK(g.stepCount() == 25);
    const BodyState falling = g.bodyState(*ballBody).value();
    CHECK(falling.position.y < 9.0);
    CHECK(falling.position.y > 8.5);
    CHECK(falling.linearVelocity.y < -4.5f);

    // Kinematic target reached exactly after one step.
    REQUIRE(g.moveKinematic(*plat, DVec3(11, 1, 0), Quat::identity(), gd.fixedDt).ok());
    REQUIRE(g.step().ok());
    CHECK(g.bodyState(*plat).value().position.x == doctest::Approx(11.0).epsilon(1e-9));
    CHECK(g.moveKinematic(*ballBody, DVec3{}, Quat::identity(), gd.fixedDt).errorCode() == ErrorCode::InvalidState);

    // The ball lands and goes to sleep.
    for (int i = 0; i < 600; ++i) REQUIRE(g.step().ok());
    const BodyState rest = g.bodyState(*ballBody).value();
    CHECK(rest.position.y == doctest::Approx(0.5).epsilon(0.02));
    CHECK_FALSE(rest.active);
    // Impulses wake it.
    REQUIRE(g.addImpulse(*ballBody, Vec3(0, 3000, 0)).ok());
    REQUIRE(g.step().ok());
    CHECK(g.bodyState(*ballBody).value().active);
    CHECK(g.bodyState(*ballBody).value().linearVelocity.y > 1.0f);
    CHECK(g.advance(-1.0).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("grid: collision matrix decides which layers collide") {
    PhysicsRuntime runtime;
    const CollisionMatrix m = CollisionMatrix::defaults();
    CHECK(m.collides(ObjectLayer::Dynamic, ObjectLayer::Terrain));
    CHECK(m.collides(ObjectLayer::Terrain, ObjectLayer::Dynamic));
    CHECK_FALSE(m.collides(ObjectLayer::Terrain, ObjectLayer::Static));
    CHECK_FALSE(m.collides(ObjectLayer::Kinematic, ObjectLayer::Kinematic));
    CHECK_FALSE(m.collides(ObjectLayer::Debris, ObjectLayer::Debris));
    CHECK_FALSE(m.collides(ObjectLayer::Debris, ObjectLayer::Character));
    CHECK(m.collides(ObjectLayer::ShipHull, ObjectLayer::ShipHull));
    CHECK(broadPhaseLayerOf(ObjectLayer::Terrain) == BroadPhaseLayer::Static);
    CHECK(broadPhaseLayerOf(ObjectLayer::ShipHull) == BroadPhaseLayer::Moving);
    CHECK(objectLayerName(ObjectLayer::ShipHull) == "ShipHull");

    // With Dynamic-vs-Terrain switched off, a ball falls through the ground.
    GridDesc gd;
    gd.collision.set(ObjectLayer::Dynamic, ObjectLayer::Terrain, false);
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    REQUIRE((*grid)->createBody(groundDesc()).ok());
    auto ball = (*grid)->createBody(sphereAt(2, DVec3(0, 2, 0), createSphere(0.5f).value()));
    REQUIRE(ball.ok());
    for (int i = 0; i < 120; ++i) REQUIRE((*grid)->step().ok());
    CHECK((*grid)->bodyState(*ball).value().position.y < -5.0);
}

TEST_CASE("grid: a stack of boxes settles and sleeps (solver sanity)") {
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    REQUIRE((*grid)->createBody(groundDesc()).ok());
    const ShapeRef box = createBox(Vec3(0.5f, 0.5f, 0.5f)).value();
    std::vector<BodyDesc> stack;
    for (u32 i = 0; i < 6; ++i) {
        BodyDesc d;
        d.key = 100 + i;
        d.shape = box;
        d.position = DVec3(0.0, 0.5 + 1.0 * i, 0.0);
        stack.push_back(d);
    }
    auto handles = (*grid)->createBodies(stack);
    REQUIRE(handles.ok());
    for (int i = 0; i < 300; ++i) REQUIRE((*grid)->step().ok());
    for (usize i = 0; i < handles->size(); ++i) {
        const BodyState st = (*grid)->bodyState((*handles)[i]).value();
        CHECK(st.position.y == doctest::Approx(0.5 + 1.0 * i).epsilon(0.01));
        CHECK(std::abs(st.position.x) < 0.02); // a 6-box stack drifts ~1 cm in 5 s with Jolt's default solver
    }
}

// -- Review regressions (WP-0.9 adversarial review) --------------------------------------------------

TEST_CASE("grid: degenerate rotations and non-finite material values are rejected, not normalized to NaN") {
    // A zero quaternion passes a finiteness check but Normalized() turns it into NaN, which then
    // poisons the broadphase and every body the NaN body touches.
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    const ShapeRef ball = createSphere(0.5f).value();
    BodyDesc zeroRot = sphereAt(5, DVec3(0, 2, 0), ball);
    zeroRot.rotation = Quat(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(g.createBody(zeroRot).errorCode() == ErrorCode::InvalidArgument);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    for (int field = 0; field < 6; ++field) {
        BodyDesc d = sphereAt(6, DVec3(0, 2, 0), ball);
        switch (field) {
            case 0: d.friction = nan; break;
            case 1: d.restitution = inf; break;
            case 2: d.linearDamping = -1.0f; break;
            case 3: d.angularDamping = nan; break;
            case 4: d.gravityFactor = inf; break;
            default: d.maxLinearVelocity = 0.0f; break;
        }
        CHECK_MESSAGE(g.createBody(d).errorCode() == ErrorCode::InvalidArgument, "field " << field);
    }
    CHECK(g.bodyCount() == 0);
    const BodyHandle h = g.createBody(sphereAt(7, DVec3(0, 2, 0), ball)).value();
    CHECK(g.setPose(h, DVec3(0, 3, 0), Quat(0.0f, 0.0f, 0.0f, 0.0f)).errorCode() == ErrorCode::InvalidArgument);
    BodyDesc kin = sphereAt(8, DVec3(4, 2, 0), ball, ObjectLayer::Kinematic);
    kin.motion = MotionType::Kinematic;
    const BodyHandle k = g.createBody(kin).value();
    CHECK(g.moveKinematic(k, DVec3(4, 3, 0), Quat(0.0f, 0.0f, 0.0f, 0.0f), 1.0f / 60.0f).errorCode() ==
          ErrorCode::InvalidArgument);
    REQUIRE(g.step().ok());
    const BodyState st = g.bodyState(h).value();
    CHECK(std::isfinite(st.position.y));
    CHECK(std::isfinite(st.rotation.w));
}

TEST_CASE("grid: shapes that must be static (meshes, compounds holding meshes) cannot move") {
    // Jolt cannot compute mass properties for a MeshShape: a dynamic mesh body gets an infinite
    // inverse mass (NaN state in release builds, a JPH_ASSERT abort in debug builds).
    PhysicsRuntime runtime;
    auto grid = PhysicsGrid::create({});
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    const Vec3 verts[] = {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}};
    const u32 idx[] = {0, 2, 1, 0, 3, 2};
    const ShapeRef mesh = createMesh(verts, idx).value();
    BodyDesc d;
    d.key = 1;
    d.shape = mesh;
    d.position = DVec3(0, 5, 0);
    d.motion = MotionType::Dynamic;
    CHECK(g.createBody(d).errorCode() == ErrorCode::InvalidArgument);
    d.mass = 10.0f; // an explicit mass does not help: the inertia is still undefined
    CHECK(g.createBody(d).errorCode() == ErrorCode::InvalidArgument);
    d.layer = ObjectLayer::Kinematic;
    d.motion = MotionType::Kinematic;
    CHECK(g.createBody(d).errorCode() == ErrorCode::InvalidArgument);
    const CompoundChild kids[] = {{mesh, Vec3{}, Quat::identity()}, {createSphere(0.5f).value(), Vec3(0, 1, 0), Quat::identity()}};
    d.shape = createStaticCompound(kids).value();
    d.layer = ObjectLayer::ShipHull;
    d.motion = MotionType::Dynamic;
    CHECK(g.createBody(d).errorCode() == ErrorCode::InvalidArgument);
    // As a static body the same shapes are fine.
    d.layer = ObjectLayer::Static;
    d.motion = MotionType::Static;
    d.mass = 0.0f;
    CHECK(g.createBody(d).ok());
}

TEST_CASE("grid: GridDesc limits that Jolt cannot honour are rejected") {
    PhysicsRuntime runtime;
    // Jolt's BodyID holds a 23-bit index: more bodies silently alias IDs in release builds.
    CHECK(PhysicsGrid::create({.maxBodies = 1u << 24}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(PhysicsGrid::create({.fixedDt = std::numeric_limits<f32>::infinity()}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(PhysicsGrid::create({.collisionSteps = 0x80000000u}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(PhysicsGrid::create({.maxStepsPerAdvance = 0}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(PhysicsGrid::create({.maxBodies = 100000, .collisionSteps = 64, .maxStepsPerAdvance = 1}).ok());
}

TEST_CASE("grid: advance() runs at most maxStepsPerAdvance steps and never spins on a huge interval") {
    // accumulator -= dt does not change 1e20 in f64, so an uncapped loop never ends; a long hitch
    // (a debugger pause, a stalled frame) must not turn into thousands of catch-up steps either.
    PhysicsRuntime runtime;
    GridDesc gd;
    gd.fixedDt = 1.0f / 60.0f;
    gd.maxStepsPerAdvance = 8;
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    CHECK(g.advance(1e20).value() == 8);
    CHECK(g.stepCount() == 8);
    // The excess whole steps were dropped; the next small interval behaves normally again.
    CHECK(g.advance(1.0 / 60.0 + 1e-9).value() == 1);
    CHECK(g.advance(0.5).value() == 8); // 30 steps due, 8 run
    CHECK(g.advance(0.0).value() == 0);
    CHECK(g.advance(std::numeric_limits<f64>::infinity()).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("grid: bodies may exceed Jolt's default 500 m/s (06 NAV mode 1 km/s, BENCH-2 1.5 km/s)") {
    PhysicsRuntime runtime;
    GridDesc gd;
    gd.gravity = Vec3{};
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    BodyDesc d = sphereAt(9, DVec3{}, createSphere(1.0f).value(), ObjectLayer::ShipHull);
    d.linearVelocity = Vec3(1500.0f, 0.0f, 0.0f);
    d.linearDamping = 0.0f;
    const BodyHandle h = (*grid)->createBody(d).value();
    for (int i = 0; i < 60; ++i) REQUIRE((*grid)->step().ok());
    const BodyState st = (*grid)->bodyState(h).value();
    CHECK(st.linearVelocity.x == doctest::Approx(1500.0f));
    CHECK(st.position.x == doctest::Approx(1500.0).epsilon(1e-3));
    // The cap is per body and still applies.
    d.key = 10;
    d.maxLinearVelocity = 100.0f;
    const BodyHandle capped = (*grid)->createBody(d).value();
    REQUIRE((*grid)->step().ok());
    CHECK((*grid)->bodyState(capped).value().linearVelocity.x == doctest::Approx(100.0f));
}

TEST_CASE("grid: a step that outgrows the temp allocator falls back to the heap instead of aborting") {
    // Jolt's own TempAllocatorImpl JPH_CRASHes when a step needs more than it holds.
    PhysicsRuntime runtime;
    GridDesc gd;
    gd.tempAllocatorBytes = 64u << 10;
    auto grid = PhysicsGrid::create(gd);
    REQUIRE(grid.ok());
    PhysicsGrid& g = **grid;
    REQUIRE(g.createBody(groundDesc()).ok());
    const ShapeRef box = createBox(Vec3(0.5f, 0.5f, 0.5f)).value();
    std::vector<BodyDesc> pile;
    for (u32 i = 0; i < 400; ++i) {
        BodyDesc d;
        d.key = 1000 + i;
        d.shape = box;
        d.position = DVec3(-10.0 + 1.01 * (i % 20), 0.5 + 1.01 * (i / 20), 0.0);
        pile.push_back(d);
    }
    REQUIRE(g.createBodies(pile).ok());
    for (int i = 0; i < 30; ++i) REQUIRE(g.step().ok());
    CHECK(g.stats().tempAllocatorFallbacks > 0);
    CHECK(g.stats().jobsInFlight == 0);
}
