#include "test_util.h"

#include <unordered_set>

using namespace helios;
using helios::test::Rng;

namespace {
FrameTransform randomFrame(Rng& rng, f64 posRange = 1e7) {
    return {rng.dvec3(-posRange, posRange), rng.quatD(), rng.dvec3(-3e4, 3e4), rng.dvec3(-1e-3, 1e-3)};
}
KinematicState randomState(Rng& rng) {
    return {rng.dvec3(-1e4, 1e4), rng.quat(), rng.dvec3(-300.0, 300.0), rng.dvec3(-0.5, 0.5)};
}
bool sameState(const KinematicState& a, const KinematicState& b, f64 posTol, f64 velTol) {
    return approxEqual(a.position, b.position, posTol) &&
           approxEqual(a.linearVelocity, b.linearVelocity, velTol) &&
           approxEqual(a.angularVelocity, b.angularVelocity, 1e-12) &&
           sameRotation(a.rotation, b.rotation, 1e-3f);
}
bool sameFrame(const FrameTransform& a, const FrameTransform& b, f64 posTol, f64 velTol) {
    return approxEqual(a.position, b.position, posTol) && sameRotation(a.rotation, b.rotation, 1e-12) &&
           approxEqual(a.linearVelocity, b.linearVelocity, velTol) &&
           approxEqual(a.angularVelocity, b.angularVelocity, 1e-15);
}
}  // namespace

TEST_CASE("frame: ids and positions") {
    constexpr FrameId none;
    static_assert(!none.isValid());
    static_assert(kRootFrame.isValid() && kRootFrame.value == 0u);
    static_assert(FrameId(3u) < FrameId(4u));
    static_assert(FrameId::invalid() == FrameId{});
    std::unordered_set<FrameId> set{FrameId(1u), FrameId(2u), FrameId(1u)};
    CHECK(set.size() == 2);
    const FramePos a{FrameId(7u), {1.0, 2.0, 3.0}};
    CHECK(a == FramePos{FrameId(7u), {1.0, 2.0, 3.0}});
    CHECK_FALSE(a == FramePos{FrameId(8u), {1.0, 2.0, 3.0}});
}

TEST_CASE("frame: point and direction conversion round-trips") {
    Rng rng(50);
    for (int i = 0; i < 1000; ++i) {
        const FrameTransform f = randomFrame(rng);
        const DVec3 p = rng.dvec3(-1e6, 1e6);
        CHECK(approxEqual(pointToChild(f, pointToParent(f, p)), p, 1e-8));
        CHECK(approxEqual(vectorToChild(f, vectorToParent(f, p)), p, 1e-8));
        CHECK(approxEqual(length(vectorToParent(f, p)), length(p), 1e-8));
        const FramePos inChild{FrameId(5u), p};
        const FramePos inParent = toParentFrame(inChild, FrameId(2u), f);
        CHECK(inParent.frame == FrameId(2u));
        const FramePos back = toChildFrame(inParent, FrameId(5u), f);
        CHECK(back.frame == FrameId(5u));
        CHECK(approxEqual(back.local, p, 1e-8));
    }
}

TEST_CASE("frame: velocity of a point fixed on a rotating planet") {
    const f64 radius = 6.371e6;
    const f64 omega = kTwoPiD / 86164.0905;  // sidereal day
    const FrameTransform planet = rotatingBodyFrame(DVec3{}, DQuat{}, 0.0, omega);
    // A point on the equator at longitude 0 (+Z) moves east (+X) at omega * R ~ 464.6 m/s.
    const DVec3 v = velocityOfFixedPoint(planet, {0.0, 0.0, radius});
    CHECK(approxEqual(v, DVec3(omega * radius, 0.0, 0.0), 1e-9));
    CHECK(approxEqual(length(v), 464.6, 0.1));
    // Poles do not move.
    CHECK(length(velocityOfFixedPoint(planet, {0.0, radius, 0.0})) < 1e-9);
    // Axial tilt: the angular velocity follows the tilted pole.
    const DQuat tilt = DQuat::fromAxisAngle(DVec3::unitX(), radians(23.44));
    const FrameTransform tilted = rotatingBodyFrame({1e11, 0.0, 0.0}, tilt, 1.0, omega, {0.0, 0.0, 29780.0});
    CHECK(approxEqual(normalize(tilted.angularVelocity), axisY(tilt), 1e-12));
    CHECK(approxEqual(length(tilted.angularVelocity), omega, 1e-18));
    CHECK(approxEqual(axisY(tilted.rotation), axisY(tilt), 1e-12));
}

TEST_CASE("frame: state conversion obeys the transport theorem") {
    Rng rng(51);
    for (int i = 0; i < 200; ++i) {
        const FrameTransform f = randomFrame(rng, 1e5);
        const KinematicState s = randomState(rng);
        const KinematicState p = stateToParent(f, s);
        CHECK(sameState(stateToChild(f, p), s, 1e-7, 1e-9));
        // Finite-difference check: move the body in the child frame and the frame in the parent,
        // then differentiate the parent-space position.
        const f64 dt = 1e-3;
        const FrameTransform f2 = advanceFrame(f, dt);
        const FrameTransform f0 = advanceFrame(f, -dt);
        const DVec3 p2 = pointToParent(f2, s.position + s.linearVelocity * dt);
        const DVec3 p0 = pointToParent(f0, s.position - s.linearVelocity * dt);
        const DVec3 numeric = (p2 - p0) / (2.0 * dt);
        CHECK(approxEqual(numeric, p.linearVelocity, 1e-4 * max(1.0, length(p.linearVelocity))));
    }
}

TEST_CASE("frame: composition and inversion") {
    Rng rng(52);
    for (int i = 0; i < 300; ++i) {
        const FrameTransform a = randomFrame(rng), b = randomFrame(rng), c = randomFrame(rng);
        // f ∘ f^-1 = identity, including velocities.
        const FrameTransform id = composeFrames(a, inverseFrame(a));
        CHECK(sameFrame(id, FrameTransform{}, 1e-8, 1e-9));
        CHECK(sameFrame(composeFrames(inverseFrame(a), a), FrameTransform{}, 1e-8, 1e-9));
        // Associativity.
        CHECK(sameFrame(composeFrames(composeFrames(a, b), c), composeFrames(a, composeFrames(b, c)), 1e-6,
                        1e-6));
        // Converting a state through two levels equals converting through the composed frame.
        const KinematicState s = randomState(rng);
        const KinematicState twoStep = stateToParent(a, stateToParent(b, s));
        const KinematicState oneStep = stateToParent(composeFrames(a, b), s);
        CHECK(sameState(twoStep, oneStep, 1e-6, 1e-6));
        // relativeFrame(a, b): b expressed in a.
        const FrameTransform ba = relativeFrame(a, b);
        CHECK(sameFrame(composeFrames(a, ba), b, 1e-6, 1e-6));
    }
}

TEST_CASE("frame: reparenting preserves world position and velocity") {
    // System (inertial) -> planet (orbiting, rotating) and system -> ship (moving, spinning).
    const f64 omega = kTwoPiD / 86164.0905;
    const FrameTransform planetInSystem = rotatingBodyFrame(
        {1.496e11, 0.0, 0.0}, DQuat::fromAxisAngle(DVec3::unitX(), 0.4), 2.0, omega, {0.0, 0.0, 29780.0});
    const FrameTransform shipInSystem{{1.496e11 + 7.0e6, 1.0e5, -2.0e5},
                                      DQuat::fromAxisAngle(normalize(DVec3(1.0, 2.0, 3.0)), 0.8),
                                      {150.0, 7500.0, 29780.0},
                                      {0.0, 0.01, 0.002}};
    // A crew member walking inside the ship.
    const KinematicState inShip{
        {3.0, 1.0, -12.0}, Quat::fromAxisAngle(Vec3::unitY(), 0.3f), {1.2, 0.0, -0.4}, {0.0, 0.1, 0.0}};
    const KinematicState world = stateToParent(shipInSystem, inShip);

    const KinematicState inPlanet = reparent(inShip, shipInSystem, planetInSystem);
    const KinematicState worldAfter = stateToParent(planetInSystem, inPlanet);
    CHECK(approxEqual(worldAfter.position, world.position, 1e-4));  // ~7000 km from the planet
    CHECK(approxEqual(worldAfter.linearVelocity, world.linearVelocity, 1e-7));
    CHECK(approxEqual(worldAfter.angularVelocity, world.angularVelocity, 1e-15));
    CHECK(sameRotation(worldAfter.rotation, world.rotation, 1e-6f));
    // In the rotating planet frame the co-rotation shows up as extra relative velocity.
    const DVec3 rel = inPlanet.position;
    const DVec3 expectedRelVel =
        vectorToChild(planetInSystem, world.linearVelocity - velocityOfFixedPoint(planetInSystem, rel));
    CHECK(approxEqual(inPlanet.linearVelocity, expectedRelVel, 1e-7));
    // And back into the ship.
    const KinematicState back = reparent(inPlanet, planetInSystem, shipInSystem);
    CHECK(sameState(back, inShip, 1e-4, 1e-6));  // omega_ship x (1 ulp at 1.5e11 m) ~ 3e-7 m/s
}

TEST_CASE("frame: advancing a rotating frame") {
    const FrameTransform f{{10.0, 0.0, 0.0}, DQuat{}, {1.0, 2.0, 3.0}, {0.0, 0.5, 0.0}};
    const FrameTransform g = advanceFrame(f, 2.0);
    CHECK(approxEqual(g.position, DVec3(12.0, 4.0, 6.0), 1e-12));
    CHECK(sameRotation(g.rotation, DQuat::fromAxisAngle(DVec3::unitY(), 1.0), 1e-12));
    CHECK(g.linearVelocity == f.linearVelocity);
    CHECK(g.angularVelocity == f.angularVelocity);
}

TEST_CASE("frame: poses convert between frames") {
    Rng rng(53);
    for (int i = 0; i < 300; ++i) {
        const FrameTransform f = randomFrame(rng);
        const DTransform pose{rng.dvec3(-1e3, 1e3), rng.quat(), Vec3(1.5f)};
        const DTransform up = poseToParent(f, pose);
        const DTransform down = poseToChild(f, up);
        CHECK(approxEqual(down.position, pose.position, 1e-7));
        CHECK(sameRotation(down.rotation, pose.rotation, 1e-3f));
        CHECK(down.scale == pose.scale);
        CHECK(approxEqual(up.position, pointToParent(f, pose.position), 1e-9));
    }
}

TEST_CASE("frame: f64 frame rotation is required for planet-sized frames") {
    // A point on a 6371 km planet converted with the planet's orientation after ~3 h of spin.
    const DQuat exact = DQuat::fromAxisAngle(normalize(DVec3(0.1, 1.0, 0.05)), 0.8136);
    const DVec3 surface{0.0, 0.0, 6.371e6};
    const DVec3 viaF64 = exact * surface;
    const DVec3 viaF32 = toF64(toF32(exact)) * surface;  // what an f32 frame rotation would give
    const f64 errF32 = distance(viaF64, viaF32);
    MESSAGE("f32 frame rotation error on a planet surface: " << errF32 << " m");
    CHECK(errF32 > 0.01);  // centimetres to decimetres: visible jitter
    const FrameTransform f{{}, exact, {}, {}};
    CHECK(distance(pointToParent(f, surface), viaF64) < 1e-8);
}

TEST_CASE("frame: frame motion is bit-identical across platforms (golden)") {
    // Planet and ship frames are advanced independently on Windows clients and Linux servers; the
    // quaternion constructors behind advanceFrame()/rotatingBodyFrame() use det:: trigonometry
    // (std::sin/cos differ in the last bit between the MSVC CRT and glibc). If this fails on one
    // platform only, fix the build flags or the code, never the numbers.
    const DQuat tilt = DQuat::fromAxisAngle(normalize(DVec3(1.0, 0.0, 0.1)), 0.409);
    FrameTransform planet = rotatingBodyFrame({1.496e11, 0.0, 0.0}, tilt, 1.25, 7.2921159e-5, {0.0, 0.0, 29780.0});
    FrameTransform ship{{3.0e5, -2.0e4, 7.0e3},
                        DQuat::fromEuler({0.1, -0.7, 0.25}),
                        {120.0, -3.0, 45.0},
                        {0.002, -0.01, 0.0005}};
    for (int i = 0; i < 3600; ++i) {
        planet = advanceFrame(planet, 1.0 / 60.0);
        ship = advanceFrame(ship, 1.0 / 60.0);
    }
    using helios::test::bits;
    CHECK(bits(planet.rotation.x) == 0x3fc36ca6514f6078ull);
    CHECK(bits(planet.rotation.y) == 0x3fe26375d3b8fb10ull);
    CHECK(bits(planet.rotation.z) == 0x3fc14609b311241aull);
    CHECK(bits(planet.rotation.w) == 0x3fe95eb235ea6bc1ull);
    CHECK(bits(ship.rotation.x) == 0x3f98ad7fd39f615aull);
    CHECK(bits(ship.rotation.y) == 0xbfe393136785c033ull);
    CHECK(bits(ship.rotation.z) == 0x3fbf476637d6de6bull);
    CHECK(bits(ship.rotation.w) == 0x3fe8ffc725010fa3ull);
    CHECK(bits(ship.position.x) == 0x4112c00000000000ull);
    const DVec3 surface = pointToParent(planet, {0.0, 0.0, 6.371e6});
    CHECK(bits(surface.x) == 0x42416a9bb392d479ull);
    CHECK(bits(surface.y) == 0xc120a0fcf68c051bull);
    CHECK(bits(surface.z) == 0x414be62e7c4be978ull);
    const Quat q = Quat::fromEuler({0.3f, -1.2f, 2.0f});
    CHECK(bits(q.x) == 0xbece6a76u);
    CHECK(bits(q.y) == 0xbecf9552u);
    CHECK(bits(q.z) == 0x3f3b7736u);
    CHECK(bits(q.w) == 0x3ebd6643u);
    const Quat r = Quat::fromAxisAngle(normalize(Vec3(1.0f, 2.0f, 3.0f)), 2.5f);
    CHECK(bits(r.x) == 0x3e81db5fu);
    CHECK(bits(r.w) == 0x3ea171efu);
}
