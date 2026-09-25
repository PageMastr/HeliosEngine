#include "scene.h"

#include <algorithm>
#include <cmath>

#include <doctest/doctest.h>

#include "helios/core/jobs.h"
#include "helios/math/scalar.h"

namespace helios::physics::test {

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

ShapeRef must(Result<ShapeRef> r) {
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().toString()));
    return *r;
}

BodyDesc body(BodyKey key, ObjectLayer layer, MotionType motion, ShapeRef shape, DVec3 pos, Quat rot = Quat::identity()) {
    BodyDesc d;
    d.key = key;
    d.layer = layer;
    d.motion = motion;
    d.shape = std::move(shape);
    d.position = pos;
    d.rotation = rot;
    return d;
}

// The platform's pose at time t (deterministic trig: det::sin/cos).
DVec3 platformPosition(f64 t) { return DVec3(18.0 + 6.0 * det::cos(0.5 * t), 0.35, 6.0 * det::sin(0.5 * t)); }

} // namespace

std::vector<BodyDesc> sceneBodies(bool independentOnly) {
    std::vector<BodyDesc> out;
    const ShapeRef ground = must(createBox(Vec3(80.0f, 1.0f, 80.0f)));
    out.push_back(body(0x1000, ObjectLayer::Terrain, MotionType::Static, ground, DVec3(0.0, -1.0, 0.0)));
    const ShapeRef sphere = must(createSphere(0.4f));
    if (independentOnly) {
        // Spheres and boxes far apart: each settles on the ground alone.
        const ShapeRef box = must(createBox(Vec3(0.4f, 0.3f, 0.5f)));
        for (u32 i = 0; i < 24; ++i) {
            const f64 x = -60.0 + 5.0 * (i % 8), z = -30.0 + 12.0 * (i / 8);
            BodyDesc d = body(0x3000 + i, ObjectLayer::Dynamic, MotionType::Dynamic, (i & 1) ? box : sphere,
                              DVec3(x, 1.0 + 0.37 * i, z), Quat::fromAxisAngle(normalize(Vec3(0.3f, 1.0f, 0.2f)), 0.1f * i));
            d.linearVelocity = Vec3(0.1f * static_cast<f32>(i % 5), 0.0f, -0.05f * static_cast<f32>(i % 3));
            d.angularVelocity = Vec3(0.2f, 0.0f, 0.1f * static_cast<f32>(i % 4));
            out.push_back(d);
        }
        return out;
    }
    // A static ramp.
    const ShapeRef ramp = must(createBox(Vec3(4.0f, 0.25f, 3.0f)));
    out.push_back(body(0x1001, ObjectLayer::Static, MotionType::Static, ramp, DVec3(-12.0, 1.0, 8.0),
                       Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), 0.3f)));
    // A mesh static (two triangles forming a tilted quad).
    const Vec3 quad[] = {{-3.0f, 0.0f, -3.0f}, {3.0f, 0.0f, -3.0f}, {3.0f, 1.5f, 3.0f}, {-3.0f, 1.5f, 3.0f}};
    const u32 quadIdx[] = {0, 2, 1, 0, 3, 2};
    out.push_back(body(0x1002, ObjectLayer::Static, MotionType::Static, must(createMesh(quad, quadIdx)), DVec3(6.0, 0.0, -14.0)));
    // Box pyramid: 4 levels, 10 boxes.
    const ShapeRef box = must(createBox(Vec3(0.5f, 0.5f, 0.5f)));
    u32 k = 0;
    for (u32 level = 0; level < 4; ++level) {
        for (u32 i = 0; i < 4 - level; ++i) {
            const f64 x = -4.0 + 1.02 * i + 0.51 * level;
            out.push_back(body(0x2000 + k++, ObjectLayer::Dynamic, MotionType::Dynamic, box, DVec3(x, 0.5 + 1.0 * level, 0.0)));
        }
    }
    // Spheres raining on the pyramid and the ramp.
    for (u32 i = 0; i < 12; ++i) {
        const f64 x = (i < 6 ? -3.0 : -13.0) + 0.37 * (i % 6), z = (i < 6 ? 0.2 : 8.0) - 0.23 * (i % 4);
        BodyDesc d = body(0x3000 + i, ObjectLayer::Dynamic, MotionType::Dynamic, sphere, DVec3(x, 6.0 + 0.9 * i, z));
        d.restitution = 0.3f;
        out.push_back(d);
    }
    // Capsules and convex hulls tumbling on the mesh ramp.
    const ShapeRef capsule = must(createCapsule(0.5f, 0.25f));
    for (u32 i = 0; i < 6; ++i) {
        BodyDesc d = body(0x4000 + i, ObjectLayer::Dynamic, MotionType::Dynamic, capsule, DVec3(5.0 + 0.6 * i, 3.0 + 0.5 * i, -14.0),
                          Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), 1.2f + 0.1f * i));
        d.friction = 0.8f;
        out.push_back(d);
    }
    const Vec3 hullPts[] = {{0.0f, 0.6f, 0.0f}, {0.5f, -0.3f, 0.4f}, {-0.5f, -0.3f, 0.4f}, {0.0f, -0.3f, -0.6f}, {0.1f, 0.1f, 0.1f}};
    const ShapeRef hull = must(createConvexHull(hullPts));
    for (u32 i = 0; i < 4; ++i) {
        BodyDesc d = body(0x5000 + i, ObjectLayer::Dynamic, MotionType::Dynamic, hull, DVec3(-1.0 + 0.8 * i, 9.0 + i, 1.5));
        d.angularVelocity = Vec3(1.0f, 0.5f * i, -0.3f);
        out.push_back(d);
    }
    // Kinematic platform (moved on a circle each step) and a crate riding it.
    out.push_back(body(0x6000, ObjectLayer::Kinematic, MotionType::Kinematic, must(createBox(Vec3(1.5f, 0.25f, 1.5f))),
                       platformPosition(0.0)));
    out.push_back(body(0x2100, ObjectLayer::Dynamic, MotionType::Dynamic, must(createBox(Vec3(0.3f, 0.3f, 0.3f))),
                       platformPosition(0.0) + DVec3(0.0, 0.6, 0.0)));
    // Debris (collides with the ground and dynamics, not with itself).
    const ShapeRef chip = must(createBox(Vec3(0.12f, 0.08f, 0.1f), 0.02f));
    for (u32 i = 0; i < 8; ++i) {
        BodyDesc d = body(0x7000 + i, ObjectLayer::Debris, MotionType::Dynamic, chip, DVec3(-3.5 + 0.2 * i, 4.5, -0.3 + 0.07 * i));
        d.linearVelocity = Vec3(0.5f - 0.1f * i, 1.0f, 0.2f);
        out.push_back(d);
    }
    // A projectile hitting the pyramid.
    BodyDesc proj = body(0x8000, ObjectLayer::Projectile, MotionType::Dynamic, must(createSphere(0.15f)), DVec3(-4.0, 1.4, 12.0));
    proj.linearVelocity = Vec3(0.3f, 2.0f, -18.0f);
    proj.mass = 2.0f;
    out.push_back(proj);
    // A compound ship hull dropped onto the ground.
    const CompoundChild parts[] = {{must(createBox(Vec3(2.0f, 0.5f, 1.0f))), Vec3(0.0f, 0.0f, 0.0f), Quat::identity()},
                                   {must(createBox(Vec3(0.6f, 0.4f, 0.6f))), Vec3(-1.2f, 0.8f, 0.0f), Quat::identity()},
                                   {must(createCylinder(0.8f, 0.3f)), Vec3(1.8f, 0.0f, 0.0f),
                                    Quat::fromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), 1.5707963f)}};
    BodyDesc hullBody = body(0x9000, ObjectLayer::ShipHull, MotionType::Dynamic, must(createStaticCompound(parts)),
                             DVec3(10.0, 4.0, 10.0), Quat::fromAxisAngle(normalize(Vec3(1.0f, 0.2f, 0.0f)), 0.4f));
    hullBody.mass = 5000.0f;
    out.push_back(hullBody);
    return out;
}

Scene::Scene(const SceneOptions& options) : m_options(options) {
    GridDesc gd;
    gd.name = "rt03";
    gd.jobs = options.jobs;
    gd.fixedDt = kDt;
    auto grid = PhysicsGrid::create(gd);
    REQUIRE_MESSAGE(grid.ok(), (grid.ok() ? std::string() : grid.error().toString()));
    m_grid = std::move(grid).value();
    std::vector<BodyDesc> descs = sceneBodies(options.independentOnly);
    if (!options.permuteBodyIds) {
        REQUIRE(m_grid->createBodies(descs).ok());
    } else {
        // Burn and free body slots so every BodyID differs from the plain build's.
        const ShapeRef dummy = must(createSphere(0.1f));
        for (u32 cycle = 0; cycle < 1000; ++cycle) {
            BodyDesc d = body(0xD000'0000 + cycle, ObjectLayer::Debris, MotionType::Dynamic, dummy, DVec3(0.0, 100.0, 0.0));
            auto h = m_grid->createBody(d);
            REQUIRE(h.ok());
            if (cycle % 3 != 0) m_grid->destroyBody(*h);
        }
        for (BodyHandle h : m_grid->bodies()) m_grid->destroyBody(h);
        std::sort(descs.begin(), descs.end(), [](const BodyDesc& a, const BodyDesc& b) {
            return std::pair(a.layer, a.key) > std::pair(b.layer, b.key);
        });
        for (const BodyDesc& d : descs) REQUIRE(m_grid->createBody(d).ok());
    }
    m_platform = m_grid->findBody(ObjectLayer::Kinematic, 0x6000);
    if (!options.independentOnly) {
        CharacterDesc cd;
        cd.key = 0xA000;
        cd.position = DVec3(20.0, 0.0, -20.0);
        auto c = m_grid->createCharacter(cd);
        REQUIRE(c.ok());
        m_character = *c;
    }
}

Scene::~Scene() = default;

void Scene::step() {
    const f64 t = static_cast<f64>(m_step + 1) * kDt;
    if (m_grid->contains(m_platform)) {
        REQUIRE(m_grid->moveKinematic(m_platform, platformPosition(t), Quat::fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), static_cast<f32>(0.5 * t)), kDt).ok());
    }
    if (m_character) {
        // Walk a square, jump every 2 s.
        const u32 leg = (m_step / 90) % 4;
        const Vec3 dirs[4] = {{3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 3.0f}, {-3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -3.0f}};
        REQUIRE(m_grid->moveCharacter(m_character, dirs[leg], (m_step % 120 == 60) ? 4.0f : 0.0f, kDt).ok());
    }
    REQUIRE(m_grid->step().ok());
    ++m_step;
}

u64 Scene::run(u32 n) {
    for (u32 i = 0; i < n; ++i) step();
    return m_grid->stateHash();
}

} // namespace helios::physics::test
