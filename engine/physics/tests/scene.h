// The scripted RT-03 Phase 0 scenes, built through the public API only:
//  - the full scene: statics, a box pyramid, falling spheres, capsules and convex hulls, a compound ship
//    hull, debris, a projectile, a kinematic platform moving on a circle and a walking, jumping
//    CharacterVirtual;
//  - the tile scene (RT-03's permuted variant, 02 §8.2): Terrain-layer tile bodies instead of one ground
//    box, with a ship hull, a vehicle chassis, a box stack and the character each touching four tiles.
#pragma once

#include <memory>
#include <vector>

#include "helios/physics/physics.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::physics::test {

struct SceneOptions {
    jobs::JobSystem* jobs = nullptr;
    /// Rebuild with different BodyIDs (RT-03's permuted variant): dummy add/remove cycles first, then
    /// one body per createBody() call in reverse (layer, key) order, so that every BodyID differs from
    /// the plain build's and every pair of bodies has the opposite BodyID order.
    bool permuteBodyIds = false;
    /// Independent bodies only: every dynamic body touches nothing but the ground (one contact
    /// constraint per island), so solver order cannot matter.
    bool independentOnly = false;
    /// The tile scene instead of the full scene (independentOnly is ignored).
    bool tiles = false;
    /// With permuteBodyIds: a non-zero seed admits the bodies in a shuffled order instead of in reverse.
    u64 shuffleSeed = 0;
};

class Scene {
public:
    explicit Scene(const SceneOptions& options);
    ~Scene();

    PhysicsGrid& grid() { return *m_grid; }
    /// Runs one scripted step (kinematic platform, character input, grid step).
    void step();
    /// Runs `n` steps and returns the state hash.
    u64 run(u32 n);
    u32 stepIndex() const noexcept { return m_step; }
    CharacterHandle character() const noexcept { return m_character; }

private:
    SceneOptions m_options;
    std::unique_ptr<PhysicsGrid> m_grid;
    BodyHandle m_platform;
    CharacterHandle m_character;
    u32 m_step = 0;
};

/// Body descriptions of the full scene (exposed so tests can permute and inspect them).
std::vector<BodyDesc> sceneBodies(bool independentOnly);
/// Body descriptions of the tile scene.
std::vector<BodyDesc> tileSceneBodies();

} // namespace helios::physics::test
