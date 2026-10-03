// The vendored Jolt stable-order patch's NoCrossUpdateCache rule (third_party/jolt/patches/
// 0001-stable-order.patch; 02 §7.1), checked through the public API: ShipHull and Vehicle bodies keep no
// cached manifold or warm-start impulse from one step() to the next, so a step of such a body is a
// function of the bodies' states alone. That is what lets a client predict a hull or a vehicle and
// roll it back to the server's state: a rollback restores bodies, never the cell's contact cache.
//
// The check: grid A runs a body resting and sliding on two tiles for a while (its contacts are cached);
// grid B is built fresh (nothing cached) with the same bodies, and both receive A's body state through
// setPose()/setVelocity(), so the states are bit-identical. Every later step must then match bit for
// bit. The control case shows the check can fail: a Dynamic-layer body warm-starts from A's cache, so
// A and B part ways.
#include <doctest/doctest.h>

#include <cstring>
#include <memory>

#include "helios/physics/physics.h"

using namespace helios;
using namespace helios::physics;

namespace {

ShapeRef must(Result<ShapeRef> r) {
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    return *r;
}

/// Two tiles side by side and one box over their seam: the box rests on both, so it has two contact
/// constraints (one per tile) whose manifolds and impulses Jolt caches between steps.
struct SeamGrid {
    std::unique_ptr<PhysicsGrid> grid;
    BodyHandle box;

    SeamGrid(ObjectLayer layer, u32 collisionSteps) {
        GridDesc gd;
        gd.name = "seam";
        gd.collisionSteps = collisionSteps;
        auto g = PhysicsGrid::create(gd);
        REQUIRE(g.ok());
        grid = std::move(g).value();
        const ShapeRef tile = must(createBox(Vec3(2.0f, 0.5f, 2.0f)));
        for (u32 i = 0; i < 2; ++i) {
            BodyDesc t;
            t.key = 0x7100'0000 + i;
            t.layer = ObjectLayer::Terrain;
            t.motion = MotionType::Static;
            t.shape = tile;
            t.position = DVec3(-2.0 + 4.0 * i, -0.5, 1.0);
            REQUIRE(grid->createBody(t).ok());
        }
        BodyDesc d;
        d.key = 0x9000;
        d.layer = layer;
        d.shape = must(createBox(Vec3(1.5f, 0.4f, 0.8f))); // centre of mass at the origin (setPose exact)
        d.position = DVec3(0.3, 0.41, 1.2);
        d.rotation = Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), 0.2f);
        d.linearVelocity = Vec3(0.6f, 0.0f, 0.2f);
        d.mass = 2000.0f;
        d.allowSleeping = false; // keep the contact active (a sleeping pair is not stepped at all)
        auto h = grid->createBody(d);
        REQUIRE(h.ok());
        box = *h;
    }

    BodyState state() const {
        auto s = grid->bodyState(box);
        REQUIRE(s.ok());
        return *s;
    }
    void set(const BodyState& s) {
        REQUIRE(grid->setPose(box, s.position, s.rotation).ok());
        REQUIRE(grid->setVelocity(box, s.linearVelocity, s.angularVelocity).ok());
    }
};

bool sameBits(const BodyState& a, const BodyState& b) {
    // Bitwise, so -0.0 and 0.0 differ and the comparison has no tolerance.
    return std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 &&
           std::memcmp(&a.rotation, &b.rotation, sizeof(a.rotation)) == 0 &&
           std::memcmp(&a.linearVelocity, &b.linearVelocity, sizeof(a.linearVelocity)) == 0 &&
           std::memcmp(&a.angularVelocity, &b.angularVelocity, sizeof(a.angularVelocity)) == 0;
}

/// Runs grid A for `warmup` steps, gives A's box state to A and to a fresh grid B, then steps both `n`
/// times; returns the first step after which the boxes differ (0 if they never do).
u32 firstDivergence(ObjectLayer layer, u32 collisionSteps, u32 warmup, u32 n) {
    SeamGrid a(layer, collisionSteps);
    for (u32 i = 0; i < warmup; ++i) REQUIRE(a.grid->step().ok());
    const BodyState s = a.state();
    SeamGrid b(layer, collisionSteps);
    a.set(s);
    b.set(s);
    REQUIRE(sameBits(a.state(), b.state()));
    for (u32 i = 1; i <= n; ++i) {
        REQUIRE(a.grid->step().ok());
        REQUIRE(b.grid->step().ok());
        if (!sameBits(a.state(), b.state())) return i;
    }
    return 0;
}

TEST_CASE("stable-order: ShipHull and Vehicle bodies keep no contact cache across steps (NoCrossUpdateCache)") {
    PhysicsRuntime runtime;
    for (ObjectLayer layer : {ObjectLayer::ShipHull, ObjectLayer::Vehicle}) {
        // 3 collision steps per step: the cache is dropped at the start of every step(), not only at the
        // start of every collision step, and the substeps of one step() may still use it.
        for (u32 collisionSteps : {1u, 3u}) {
            CAPTURE(objectLayerName(layer));
            CAPTURE(collisionSteps);
            CHECK(firstDivergence(layer, collisionSteps, 90, 120) == 0);
        }
    }
}

TEST_CASE("stable-order: control: a Dynamic-layer body warm-starts from contacts cached by earlier steps") {
    // Without NoCrossUpdateCache, grid A reuses its cached manifolds and impulses and grid B starts cold,
    // so the same body state steps to different bits: the property above is not vacuous.
    PhysicsRuntime runtime;
    CHECK(firstDivergence(ObjectLayer::Dynamic, 1, 90, 120) != 0);
}

} // namespace
