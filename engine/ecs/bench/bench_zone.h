#pragma once
// RT-01 benchmark zone (02 §8.2): 50k entities on a SERVER tick at 20 Hz — 20k replicated
// (ships + turrets, NPCs, projectiles, loot) + 30k content-placed (asteroids, station modules,
// debris, beacons), ~150 ECS archetypes, 5k physics bodies in 12 grids — and the representative
// systems the ECS decision is based on:
//
//   PrePhysics   AiScoring (NPC utility scoring, random-access target reads), ShipControl (reads
//                the prefab-shared ShipClass), WeaponFire (spawns projectiles via command buffers)
//   Physics      Integrate / IntegrateRotation (frame-local f64 positions, Mut<> dirty marks)
//   PostPhysics  ProjectileUpdate (lifetime, hits -> damage events, despawns), LootDecay (despawn
//                + respawn churn), ShieldRegen (dirty-tracked writes)
//   AuthorityFlush ApplyDamage (single job, consumes damage events in job order, loot drops)
//   ReplicationGather SpatialHash (cell keys for interest management)
//   + World::gatherChanges() (the dirty-tracking gather replication consumes)
//
// Everything is seeded and the ID blocks use a simulated clock, so every configuration (0/1/2/4
// workers) produces the same final state hash.

#include <memory>
#include <vector>

#include "helios/ecs/world.h"
#include "helios/math/quat.h"
#include "helios/math/vec.h"

namespace bench {

using namespace helios;
using helios::ecs::Entity;

struct ZoneConfig {
    u32 ships = 1600;
    u32 turretsPerCapital = 2;   ///< Capital ships (every 8th) carry turret children.
    u32 npcs = 6000;
    u32 projectiles = 8000;
    u32 loot = 4000;
    u32 placed = 30000;
    u32 grids = 12;
    u32 dockedShips = 400;       ///< Ships docked (DockedTo) at station modules.
    u64 seed = 0x5EED;
    /// The burst's 3,000 creates as spawn() + 7 set() commands each instead of 12 spawnN() batches.
    bool perCommandCreates = false;
    /// The burst exactly as the pre-WP-1.1a bench (f08cf5b) ran it, for comparison (SPIKES.md
    /// §5.3): per-command creates, the first 3,000 projectiles in query order as victims, fresh
    /// buffers every round, and a raw floor that writes no create values, reuses 250 values for
    /// every frame and deletes its own creates. ecs_bench also skips the raw parity alignment.
    bool legacyBurst = false;
};

struct ZoneInfo {
    u32 entities = 0;
    u32 replicated = 0;          ///< Entities with a NetHandle and replicated components.
    u32 placed = 0;
    u32 bodies = 0;
    u32 archetypes = 0;          ///< Distinct component sets (relationship pairs excluded).
    u32 frames = 0;
};

struct BurstResult {
    u32 commands = 0;
    f64 recordMs = 0;
    f64 createMs = 0;
    f64 destroyMs = 0;
    f64 toggleMs = 0;
    f64 toggleDontFragmentMs = 0;
    f64 totalMs() const noexcept { return createMs + destroyMs + toggleMs; }
    /// The same burst with the toggles on the DontFragment component instead of tags.
    f64 totalDontFragmentMs() const noexcept { return createMs + destroyMs + toggleDontFragmentMs; }
};

/// Per-tick damage events: one vector per job of the producing system, consumed in job order.
struct DamageEvent {
    Entity target;
    f32 amount = 0;
};

class BenchZone {
public:
    BenchZone(ecs::World& world, const ZoneConfig& config);
    ~BenchZone();

    /// Registers components, prefabs and frames, then spawns the zone.
    void build();
    /// Registers the representative systems.
    void registerSystems();
    /// One server tick: world.tick() + gatherChanges().
    void tick(f32 dt);

    const ZoneInfo& info() const noexcept { return m_info; }
    const ecs::ChangeList& lastChanges() const noexcept { return m_changes; }
    /// Hash of all replicated state in EntityId order (determinism check across configurations).
    u64 stateHash();
    /// Single-thread pass over all 50k entities touching 3 components (RT-01 "50k×3 iteration").
    /// Returns the number of entities visited.
    u32 iterate3(u64* collectNs = nullptr, u32* chunkCount = nullptr);
    /// 9,000 structural operations: 3,000 creates (projectile archetype, 7 components each; one
    /// spawnN() batch per frame, or per-command spawns with ZoneConfig::perCommandCreates),
    /// 3,000 destroys and 3,000 tag adds/removes on NPCs (a table move each), each set applied as
    /// one sync point; plus the same 3,000 toggles on a DontFragment component for comparison.
    /// Round 0 creates new tag-combination tables ("cold"); rounds >= 1 destroy the previous
    /// round's projectiles and alternately revert / re-apply the tag changes, using only existing
    /// tables ("warm").
    /// `profiled` routes the timed work through bench::timed:: functions, the ones the callgrind
    /// job collects (SPIKES.md §5.2); the timings are the same either way.
    BurstResult structuralBurst(u32 round, bool profiled = true);
    /// The same 9k operations issued directly through the flecs C API (bulk init into the final
    /// tables with values, ecs_delete, ecs_add_id/ecs_remove_id, ecs_set_id/ecs_remove_id on the
    /// DontFragment status), without World bookkeeping (ids, handles, registry, logs, dirty
    /// tracking, command buffers). This is the floor any flecs-based wrapper pays. The entities it
    /// creates are unregistered; like structuralBurst(), each call deletes the previous call's
    /// creates (the first deletes none). Continue the round numbering of structuralBurst() so the
    /// NPC tag toggles keep alternating.
    BurstResult rawFlecsBurst(u32 round, bool profiled = true);

    u32 liveProjectiles();

private:
    std::vector<ecs::Entity> burstNpcs();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    ecs::World& m_world;
    ZoneConfig m_config;
    ZoneInfo m_info;
    ecs::ChangeList m_changes;
};

} // namespace bench
