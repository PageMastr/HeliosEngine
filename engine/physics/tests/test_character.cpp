// CharacterVirtual mover skeleton (02 §7.1 "Characters"): support, walking, jumping, slopes, the
// ground body's stable key.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "helios/physics/physics.h"

using namespace helios;
using namespace helios::physics;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

std::unique_ptr<PhysicsGrid> flatWorld() {
    auto grid = PhysicsGrid::create({}).value();
    BodyDesc ground;
    ground.key = 0x5E;
    ground.layer = ObjectLayer::Terrain;
    ground.motion = MotionType::Static;
    ground.shape = createBox(Vec3(50.0f, 1.0f, 50.0f)).value();
    ground.position = DVec3(0.0, -1.0, 0.0);
    REQUIRE(grid->createBody(ground).ok());
    return grid;
}

} // namespace

TEST_CASE("character: lands, stands on the ground and reports the ground's key") {
    PhysicsRuntime runtime;
    auto grid = flatWorld();
    CharacterDesc cd;
    cd.key = 0xC1;
    cd.position = DVec3(0.0, 1.0, 0.0);
    const CharacterHandle c = grid->createCharacter(cd).value();
    for (int i = 0; i < 90; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    const CharacterState st = grid->characterState(c).value();
    CHECK(st.ground == GroundState::OnGround);
    CHECK(st.groundKey == 0x5E);
    CHECK(st.position.y == doctest::Approx(0.0).epsilon(0.05));
    CHECK(std::abs(st.linearVelocity.y) < 0.5f);
}

TEST_CASE("character: walks at the requested horizontal speed and jumps") {
    PhysicsRuntime runtime;
    auto grid = flatWorld();
    CharacterDesc cd;
    cd.key = 0xC2;
    const CharacterHandle c = grid->createCharacter(cd).value();
    for (int i = 0; i < 30; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    const DVec3 start = grid->characterState(c).value().position;
    for (int i = 0; i < 60; ++i) REQUIRE(grid->moveCharacter(c, Vec3(4.0f, 0.0f, 0.0f), 0.0f, kDt).ok());
    const CharacterState walked = grid->characterState(c).value();
    CHECK(walked.position.x - start.x == doctest::Approx(4.0).epsilon(0.05)); // 1 s at 4 m/s
    CHECK(walked.ground == GroundState::OnGround);
    REQUIRE(grid->moveCharacter(c, Vec3{}, 5.0f, kDt).ok());
    for (int i = 0; i < 10; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    const CharacterState air = grid->characterState(c).value();
    CHECK(air.ground == GroundState::InAir);
    CHECK(air.position.y > 0.5);
    for (int i = 0; i < 120; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    CHECK(grid->characterState(c).value().ground == GroundState::OnGround);
}

TEST_CASE("character: climbs a step and is blocked by a wall") {
    PhysicsRuntime runtime;
    auto grid = flatWorld();
    BodyDesc stepBox;
    stepBox.key = 0x57E9;
    stepBox.layer = ObjectLayer::Static;
    stepBox.motion = MotionType::Static;
    stepBox.shape = createBox(Vec3(2.0f, 0.1f, 2.0f), 0.0f).value();
    stepBox.position = DVec3(3.0, 0.1, 0.0); // a 20 cm step (below stepHeight 0.4)
    REQUIRE(grid->createBody(stepBox).ok());
    BodyDesc wall = stepBox;
    wall.key = 0x3A11;
    wall.shape = createBox(Vec3(0.2f, 2.0f, 5.0f)).value();
    wall.position = DVec3(-3.0, 2.0, 0.0);
    REQUIRE(grid->createBody(wall).ok());
    CharacterDesc cd;
    cd.key = 0xC3;
    const CharacterHandle c = grid->createCharacter(cd).value();
    for (int i = 0; i < 20; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    for (int i = 0; i < 90; ++i) REQUIRE(grid->moveCharacter(c, Vec3(3.0f, 0.0f, 0.0f), 0.0f, kDt).ok());
    CharacterState st = grid->characterState(c).value();
    CHECK(st.position.x > 2.5);
    CHECK(st.position.y == doctest::Approx(0.2).epsilon(0.05)); // on top of the step
    CHECK(st.groundKey == 0x57E9);
    for (int i = 0; i < 240; ++i) REQUIRE(grid->moveCharacter(c, Vec3(-3.0f, 0.0f, 0.0f), 0.0f, kDt).ok());
    st = grid->characterState(c).value();
    CHECK(st.position.x > -3.0 + 0.2); // stopped by the wall (its face is at x = -2.8)
    CHECK(st.position.x < -2.0);
}

TEST_CASE("character: validation and handles") {
    PhysicsRuntime runtime;
    auto grid = flatWorld();
    CharacterDesc cd;
    cd.key = 0;
    CHECK(grid->createCharacter(cd).errorCode() == ErrorCode::InvalidArgument);
    cd.key = 5;
    cd.radius = 0.0f;
    CHECK(grid->createCharacter(cd).errorCode() == ErrorCode::InvalidArgument);
    cd.radius = 0.3f;
    const CharacterHandle c = grid->createCharacter(cd).value();
    CHECK(grid->createCharacter(cd).errorCode() == ErrorCode::AlreadyExists);
    CHECK(grid->moveCharacter(c, Vec3{}, 0.0f, 0.0f).errorCode() == ErrorCode::InvalidArgument);
    const f32 inf = std::numeric_limits<f32>::infinity();
    CHECK(grid->moveCharacter(c, Vec3{}, 0.0f, inf).errorCode() == ErrorCode::InvalidArgument);
    CHECK(grid->moveCharacter(c, Vec3{}, inf, kDt).errorCode() == ErrorCode::InvalidArgument);
    CHECK(std::isfinite(grid->characterState(c).value().position.y));
    CHECK(grid->setCharacterUp(c, Vec3{}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(grid->setCharacterUp(c, Vec3(0, 0, 2)).ok());
    grid->destroyCharacter(c);
    CHECK(grid->characterState(c).errorCode() == ErrorCode::NotFound);
    CHECK(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).errorCode() == ErrorCode::NotFound);
}

TEST_CASE("character: a non-vertical up stands the capsule along up (radial gravity on planets, 02 §7.1)") {
    // The capsule is built along the character's local Y. With up = +X (a wall used as a floor,
    // gravity along -X) the capsule must stand on its end along +X, not lie on its side with its
    // origin a radius above the floor. Both createCharacter and setCharacterUp keep local Y on up.
    PhysicsRuntime runtime;
    GridDesc gd;
    auto grid = PhysicsGrid::create(gd).value();
    BodyDesc floor;
    floor.key = 0xF1;
    floor.layer = ObjectLayer::Static;
    floor.motion = MotionType::Static;
    floor.shape = createBox(Vec3(1.0f, 50.0f, 50.0f)).value();
    floor.position = DVec3(-1.0, 0.0, 0.0); // its +X face is the plane x = 0
    REQUIRE(grid->createBody(floor).ok());
    const auto localY = [](const Quat& q) { return rotate(q, Vec3(0.0f, 1.0f, 0.0f)); };

    CharacterDesc cd;
    cd.key = 0xC7;
    cd.up = Vec3(1.0f, 0.0f, 0.0f);
    cd.position = DVec3(0.5, 0.0, 0.0);
    const CharacterHandle c = grid->createCharacter(cd).value();
    CHECK(localY(grid->characterState(c).value().rotation).x == doctest::Approx(1.0f).epsilon(1e-5));
    for (int i = 0; i < 90; ++i) REQUIRE(grid->moveCharacter(c, Vec3{}, 0.0f, kDt).ok());
    CharacterState st = grid->characterState(c).value();
    CHECK(st.ground == GroundState::OnGround);
    CHECK(st.groundKey == 0xF1);
    CHECK(st.position.x == doctest::Approx(0.0).epsilon(0.05)); // standing on its end, not lying down

    // Re-orienting an existing character: start upright on +Y ground, then turn up to +X.
    CharacterDesc upright;
    upright.key = 0xC8;
    upright.position = DVec3(0.5, 5.0, 10.0);
    const CharacterHandle d = grid->createCharacter(upright).value();
    REQUIRE(grid->setCharacterUp(d, Vec3(2.0f, 0.0f, 0.0f)).ok());
    CHECK(localY(grid->characterState(d).value().rotation).x == doctest::Approx(1.0f).epsilon(1e-5));
    for (int i = 0; i < 120; ++i) REQUIRE(grid->moveCharacter(d, Vec3{}, 0.0f, kDt).ok());
    st = grid->characterState(d).value();
    CHECK(st.ground == GroundState::OnGround);
    CHECK(st.position.x == doctest::Approx(0.0).epsilon(0.05));
}

TEST_CASE("character: non-finite or negative mover parameters are rejected") {
    PhysicsRuntime runtime;
    auto grid = flatWorld();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    for (int field = 0; field < 6; ++field) {
        CharacterDesc cd;
        cd.key = 0xD0 + static_cast<BodyKey>(field);
        switch (field) {
            case 0: cd.maxSlopeRadians = nan; break;
            case 1: cd.mass = -1.0f; break;
            case 2: cd.maxStrength = nan; break;
            case 3: cd.stepHeight = -0.1f; break;
            case 4: cd.stickToFloorDistance = nan; break;
            default: cd.up = Vec3{}; break;
        }
        CHECK_MESSAGE(grid->createCharacter(cd).errorCode() == ErrorCode::InvalidArgument, "field " << field);
    }
    CharacterDesc cd;
    cd.key = 0xDF;
    cd.rotation = Quat(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(grid->createCharacter(cd).errorCode() == ErrorCode::InvalidArgument);
}
