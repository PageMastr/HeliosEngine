// The scripted RT-03 Phase 0 scene: statics, a box pyramid, falling spheres, capsules and convex
// hulls, a compound ship hull, debris, a projectile, a kinematic platform moving on a circle and a
// walking, jumping CharacterVirtual. Built through the public API only.
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
    /// one body per createBody() call in reverse (layer, key) order.
    bool permuteBodyIds = false;
    /// Independent bodies only: every dynamic body touches nothing but the ground (one contact
    /// constraint per island), so solver order cannot matter.
    bool independentOnly = false;
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

/// Body descriptions of the scene (exposed so tests can permute and inspect them).
std::vector<BodyDesc> sceneBodies(bool independentOnly);

} // namespace helios::physics::test
