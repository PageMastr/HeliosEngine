#include "bench_zone.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <string>
#include <tuple>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/time.h"

#include "flecs_internal.h"

namespace bench {

using namespace helios::ecs;

// ---------------------------------------------------------------------------------------------
// Components (hand-written stand-ins for schemac output; replicated ones carry _dirty)
// ---------------------------------------------------------------------------------------------
namespace c {
struct Position {
    DVec3 p{};
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Position::p);
};
struct Rotation {
    Quat q{};
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Rotation::q);
};
struct Velocity {
    Vec3 lin{};
    Vec3 ang{};
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Velocity::lin, &Velocity::ang);
};
struct Health {
    f32 hp = 100;
    f32 maxHp = 100;
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Health::hp, &Health::maxHp);
};
struct Shield {
    f32 value = 50;
    f32 regen = 2;
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Shield::value);
};
struct Faction {
    u16 id = 0;
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Faction::id);
};
struct Loot {
    u32 item = 0;
    u16 count = 1;
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Loot::item, &Loot::count);
};
struct ShipClass { // shared through prefabs (@shared)
    f32 mass = 1000;
    f32 maxHp = 500;
    f32 maxSpeed = 200;
    f32 accel = 40;
};
struct Thrusters {
    f32 throttle = 0;
    f32 maxForce = 1e5f;
};
struct Weapon {
    f32 cooldown = 0;
    f32 rate = 1;
    f32 damage = 10;
    u32 ammo = 1000;
};
struct AiState {
    Entity target;
    f32 scores[4] = {};
    u32 state = 0;
    u32 retargetTick = 0;
};
struct Projectile {
    Entity target;
    f32 damage = 5;
};
struct Lifetime {
    f32 remaining = 1;
};
struct Bounds {
    f32 radius = 1;
};
struct SpatialCell {
    u64 key = 0;
};
struct PhysicsBody {
    u32 body = 0;
    f32 mass = 1;
};
struct StaticProp {
    u32 mesh = 0;
};
struct Collider {
    Vec3 halfExtents{1, 1, 1};
    u32 shape = 0;
};
struct Resource {
    u32 kind = 0;
    f32 amount = 100;
};
struct Interactable {
    u32 action = 0;
};
struct Waypoint {
    DVec3 target{};
};
struct Status { // high-churn status effects: DontFragment (SPIKES.md §2)
    u32 flags = 0;
};
} // namespace c

namespace {

constexpr u32 kTagCount = 48;
constexpr f64 kCellSize = 256.0;
constexpr f64 kGridExtent = 20'000.0; // metres, per grid

/// Deterministic per-entity hash (no shared RNG state between jobs).
constexpr u64 h64(u64 a, u64 b = 0) noexcept { return mix64(a * 0x9E3779B97F4A7C15ull + b); }
constexpr f32 unit(u64 h) noexcept { return static_cast<f32>(h >> 40) / static_cast<f32>(1ull << 24); }

u64 cellKey(const DVec3& p) noexcept {
    const auto cx = static_cast<i64>(std::floor(p.x / kCellSize));
    const auto cy = static_cast<i64>(std::floor(p.y / kCellSize));
    const auto cz = static_cast<i64>(std::floor(p.z / kCellSize));
    return mix64(static_cast<u64>(cx) * 73856093ull ^ static_cast<u64>(cy) * 19349663ull ^ static_cast<u64>(cz) * 83492791ull);
}

} // namespace

// The timed parts of the 9k-op burst, out of line so that callgrind counts exactly the measured
// work (SPIKES.md §5.2):  valgrind --tool=callgrind --toggle-collect='bench::timed::*' ecs_bench ...
// Each writes its own marker so identical-code folding cannot merge the World phases, and
// writes it again after the call so that it is not a tail call.
namespace timed {
namespace {
volatile u32 g_phase = 0;
} // namespace
HELIOS_NOINLINE void worldCreates(World& w, CommandBuffer& cb) {
    g_phase = 1;
    w.apply(cb);
    g_phase = 0; // not a tail call, so callgrind sees the function return
}
HELIOS_NOINLINE void worldDestroys(World& w, CommandBuffer& cb) {
    g_phase = 2;
    w.apply(cb);
    g_phase = 0; // not a tail call, so callgrind sees the function return
}
HELIOS_NOINLINE void worldToggles(World& w, CommandBuffer& cb) {
    g_phase = 3;
    w.apply(cb);
    g_phase = 0; // not a tail call, so callgrind sees the function return
}
HELIOS_NOINLINE void worldStatuses(World& w, CommandBuffer& cb) {
    g_phase = 4;
    w.apply(cb);
    g_phase = 0; // not a tail call, so callgrind sees the function return
}
} // namespace timed

struct BenchZone::Impl {
    std::vector<ComponentId> tags; // runtime-registered variant tags
    std::vector<Entity> frames;
    std::vector<Entity> shipPrefabs;
    std::vector<Entity> targets;  // ships + NPCs (never destroyed): AI/projectile targets
    std::vector<Entity> stations; // docking hosts
    std::vector<std::vector<DamageEvent>> damageEvents; // per ProjectileUpdate job
    std::set<std::vector<ComponentId>> archetypes;
    u64 spawnSerial = 0;
    u64 clockMs = 0;
    std::unique_ptr<Query> iterQuery;
    std::vector<ChunkData> iterChunks;
    std::vector<Entity> burstCreated; // projectiles spawned by the last structuralBurst()
    std::vector<ecs_entity_t> rawCreated; // unregistered projectiles of the last rawFlecsBurst()
    // The burst's buffers are kept across rounds, as the scheduler keeps its per-job buffers.
    CommandBuffer creates, destroys, toggles, statuses;
};

BenchZone::BenchZone(World& world, const ZoneConfig& config)
    : m_impl(std::make_unique<Impl>()), m_world(world), m_config(config) {}

BenchZone::~BenchZone() = default; // Impl (and its Query) goes before the World

void BenchZone::build() {
    World& w = m_world;
    Impl& im = *m_impl;
    w.registerComponent<c::Position>();
    w.registerComponent<c::Rotation>();
    w.registerComponent<c::Velocity>();
    w.registerComponent<c::Health>();
    w.registerComponent<c::Shield>();
    w.registerComponent<c::Faction>();
    w.registerComponent<c::Loot>();
    w.registerComponent<c::ShipClass>(ComponentFlags::Shared);
    w.registerComponent<c::Thrusters>();
    w.registerComponent<c::Weapon>();
    w.registerComponent<c::AiState>();
    w.registerComponent<c::Projectile>();
    w.registerComponent<c::Lifetime>();
    w.registerComponent<c::Bounds>();
    w.registerComponent<c::SpatialCell>();
    w.registerComponent<c::PhysicsBody>();
    w.registerComponent<c::StaticProp>();
    w.registerComponent<c::Collider>();
    w.registerComponent<c::Resource>();
    w.registerComponent<c::Interactable>();
    w.registerComponent<c::Waypoint>();
    w.registerComponent<c::Status>(ComponentFlags::DontFragment);
    // Variant tags go through the runtime descriptor path (what reflection-driven content will use).
    for (u32 t = 0; t < kTagCount; ++t) {
        ComponentDesc d;
        d.name = "bench.tag" + std::to_string(t);
        im.tags.push_back(*w.registerComponent(d));
    }
    for (u32 g = 0; g < m_config.grids; ++g) im.frames.push_back(w.createFrame(FrameId(100 + g)));

    // Five ship classes: prefab-shared ShipClass + copied Health/Shield/Thrusters defaults.
    const c::ShipClass classes[5] = {{800, 300, 320, 90}, {5000, 900, 180, 30}, {40000, 4000, 90, 8},
                                     {2500, 600, 220, 50}, {12000, 1500, 140, 15}};
    for (u32 k = 0; k < 5; ++k) {
        const Entity p = w.createPrefab("bench.ShipClass" + std::to_string(k));
        w.set(p, classes[k]);
        w.set(p, c::Health{classes[k].maxHp, classes[k].maxHp});
        w.set(p, c::Shield{classes[k].maxHp * 0.5f, 3.0f});
        w.set(p, c::Thrusters{0.0f, classes[k].mass * classes[k].accel});
        im.shipPrefabs.push_back(p);
    }

    const u64 seed = m_config.seed;
    u32 bodies = 0;
    u32 bodySerial = 1;
    // All spawning goes through one command buffer per kind: exactly how zone boot works.
    CommandBuffer cb(&w);
    auto addTag = [&](TempEntity t, u32 tag, std::vector<ComponentId>& key) {
        cb.addId(t, im.tags[tag]);
        key.push_back(im.tags[tag]);
    };
    auto commit = [&](std::vector<ComponentId>& key) {
        std::sort(key.begin(), key.end());
        im.archetypes.insert(key);
    };
    auto gridPos = [&](u64 h) {
        return DVec3((unit(h64(h, 1)) - 0.5f) * kGridExtent, (unit(h64(h, 2)) - 0.5f) * kGridExtent * 0.2,
                     (unit(h64(h, 3)) - 0.5f) * kGridExtent);
    };
    const ComponentId idPos = w.id<c::Position>(), idRot = w.id<c::Rotation>(), idVel = w.id<c::Velocity>(),
                      idHp = w.id<c::Health>(), idSh = w.id<c::Shield>(), idFac = w.id<c::Faction>(),
                      idThr = w.id<c::Thrusters>(), idWpn = w.id<c::Weapon>(), idAi = w.id<c::AiState>(),
                      idBnd = w.id<c::Bounds>(), idCell = w.id<c::SpatialCell>(), idBody = w.id<c::PhysicsBody>(),
                      idProj = w.id<c::Projectile>(), idLife = w.id<c::Lifetime>(), idLoot = w.id<c::Loot>(),
                      idProp = w.id<c::StaticProp>(), idCol = w.id<c::Collider>(), idRes = w.id<c::Resource>(),
                      idInt = w.id<c::Interactable>(), idWay = w.id<c::Waypoint>(), idClass = w.id<c::ShipClass>();

    // ---- ships (+ turret children on capitals) ------------------------------------------------
    std::vector<TempEntity> shipTemps;
    std::vector<TempEntity> capitalTemps;
    for (u32 i = 0; i < m_config.ships; ++i) {
        const u64 h = h64(seed, 1000 + i);
        const u32 cls = static_cast<u32>(h % 5);
        const Entity frame = im.frames[i % m_config.grids];
        const TempEntity t = cb.spawn({.prefab = im.shipPrefabs[cls], .frame = frame, .ag = 1 + i});
        std::vector<ComponentId> key{idClass, idHp, idSh, idThr};
        cb.set(t, c::Position{gridPos(h)});
        cb.set(t, c::Rotation{});
        cb.set(t, c::Velocity{{unit(h64(h, 4)) * 50 - 25, 0, unit(h64(h, 5)) * 50 - 25}, {0, 0.1f, 0}});
        cb.set(t, c::Faction{static_cast<u16>(h % 7)});
        cb.set(t, c::Bounds{10.0f + static_cast<f32>(cls) * 20.0f});
        cb.set(t, c::SpatialCell{});
        cb.set(t, c::PhysicsBody{bodySerial++, 1000.0f});
        cb.set(t, c::Waypoint{gridPos(h64(h, 6))});
        ++bodies;
        key.insert(key.end(), {idPos, idRot, idVel, idFac, idBnd, idCell, idBody, idWay});
        addTag(t, cls, key); // class tag
        if (h64(h, 7) % 10 < 7) {
            cb.set(t, c::Weapon{unit(h64(h, 8)), 0.5f + static_cast<f32>(cls % 3) * 0.25f, 12, 1000});
            key.push_back(idWpn);
        }
        if (h64(h, 9) % 4 == 0) addTag(t, 5, key); // cloaking device
        commit(key);
        shipTemps.push_back(t);
        if (i % 8 == 0) capitalTemps.push_back(t);
    }
    u32 turrets = 0;
    for (usize ci = 0; ci < capitalTemps.size(); ++ci) {
        for (u32 k = 0; k < m_config.turretsPerCapital; ++k) {
            const TempEntity t = cb.spawn({.ag = 1 + static_cast<u64>(ci)});
            cb.setParent(t, capitalTemps[ci]);
            cb.set(t, c::Position{DVec3(static_cast<f64>(k) * 5.0, 3.0, 0.0)});
            cb.set(t, c::Rotation{});
            cb.set(t, c::Health{200, 200});
            cb.set(t, c::Weapon{0, 2.0f, 6, 5000});
            cb.set(t, c::Faction{static_cast<u16>(ci % 7)});
            cb.set(t, c::Bounds{2});
            cb.set(t, c::SpatialCell{});
            std::vector<ComponentId> key{idPos, idRot, idHp, idWpn, idFac, idBnd, idCell};
            addTag(t, 6, key); // turret
            commit(key);
            ++turrets;
        }
    }
    w.apply(cb);
    for (const TempEntity t : shipTemps) im.targets.push_back(cb.resolved(t));

    // ---- NPCs ------------------------------------------------------------------------------------
    std::vector<TempEntity> npcTemps;
    const u32 npcBodies = 5000 > m_config.ships + 2000 ? 5000 - m_config.ships - 2000 : 0;
    for (u32 i = 0; i < m_config.npcs; ++i) {
        const u64 h = h64(seed, 100'000 + i);
        const TempEntity t = cb.spawn({.frame = im.frames[(i * 7) % m_config.grids], .ag = 10'000 + i});
        cb.set(t, c::Position{gridPos(h)});
        cb.set(t, c::Rotation{});
        cb.set(t, c::Velocity{{unit(h64(h, 4)) * 4 - 2, 0, unit(h64(h, 5)) * 4 - 2}, {0, 0.5f, 0}});
        cb.set(t, c::Health{120, 120});
        cb.set(t, c::AiState{Entity(), {}, 0, static_cast<u32>(h % 20)});
        cb.set(t, c::Faction{static_cast<u16>(h % 7)});
        cb.set(t, c::Bounds{1});
        cb.set(t, c::SpatialCell{});
        std::vector<ComponentId> key{idPos, idRot, idVel, idHp, idAi, idFac, idBnd, idCell};
        addTag(t, 8 + static_cast<u32>(h % 3), key);        // species
        addTag(t, 11 + static_cast<u32>(h64(h, 6) % 3), key); // AI profile
        if (h64(h, 7) % 2 == 0) {
            cb.set(t, c::Weapon{unit(h64(h, 8)), 1.0f, 4, 200});
            key.push_back(idWpn);
        }
        if (i < npcBodies) {
            cb.set(t, c::PhysicsBody{bodySerial++, 90.0f});
            key.push_back(idBody);
            ++bodies;
        }
        commit(key);
        npcTemps.push_back(t);
    }
    w.apply(cb);
    for (const TempEntity t : npcTemps) im.targets.push_back(cb.resolved(t));

    // ---- placed content (content-placed ids and handle slots) -------------------------------------
    u32 placedBodies = 0;
    const u32 placedBodyBudget = 5000 > bodies ? 5000 - bodies : 0;
    std::vector<TempEntity> stationTemps;
    for (u32 i = 0; i < m_config.placed; ++i) {
        const u64 h = h64(seed, 1'000'000 + i);
        const TempEntity t = cb.spawn({.id = EntityId::contentPlaced(h64(seed ^ 0xC0DE, i)),
                                       .frame = im.frames[(i * 5) % m_config.grids],
                                       .contentHandleIndex = i + 1});
        cb.set(t, c::Position{gridPos(h)});
        cb.set(t, c::Rotation{});
        cb.set(t, c::Bounds{5.0f + unit(h64(h, 4)) * 50.0f});
        cb.set(t, c::SpatialCell{cellKey(gridPos(h))});
        cb.set(t, c::StaticProp{static_cast<u32>(h64(h, 5) % 500)});
        cb.set(t, c::Collider{{2, 2, 2}, static_cast<u32>(h % 3)});
        std::vector<ComponentId> key{idPos, idRot, idBnd, idCell, idProp, idCol};
        const u32 kind = static_cast<u32>(h64(h, 6) % 100);
        if (kind < 50) { // asteroid: resource kind x depleted
            cb.set(t, c::Resource{static_cast<u32>(h % 4), 100});
            key.push_back(idRes);
            addTag(t, 14 + static_cast<u32>(h % 4), key);
            if (h64(h, 7) % 3 == 0) addTag(t, 18, key);
        } else if (kind < 80) { // station module: 10 types x interactable x powered
            addTag(t, 20 + static_cast<u32>(h64(h, 8) % 10), key);
            if (h64(h, 9) % 2 == 0) {
                cb.set(t, c::Interactable{static_cast<u32>(h % 16)});
                key.push_back(idInt);
            }
            if (h64(h, 10) % 2 == 0) addTag(t, 30, key);
            if (stationTemps.size() < 100 && h64(h, 11) % 4 == 0) stationTemps.push_back(t);
        } else if (kind < 95) { // debris: 3 sizes
            addTag(t, 31 + static_cast<u32>(h % 3), key);
        } else { // beacons: 2 kinds x active
            addTag(t, 34 + static_cast<u32>(h % 2), key);
            if (h64(h, 12) % 2 == 0) addTag(t, 36, key);
        }
        if (placedBodies < placedBodyBudget && (kind < 50 || kind >= 80) && h64(h, 13) % 4 == 0) {
            cb.set(t, c::PhysicsBody{bodySerial++, 5000.0f});
            key.push_back(idBody);
            ++placedBodies;
        }
        commit(key);
    }
    w.apply(cb);
    for (const TempEntity t : stationTemps) im.stations.push_back(cb.resolved(t));
    bodies += placedBodies;

    // ---- projectiles and loot (the churning part of the replicated set) ---------------------------
    for (u32 i = 0; i < m_config.projectiles; ++i) {
        const Entity shooter = im.targets[static_cast<usize>(h64(seed, 2'000'000 + i) % im.targets.size())];
        const Entity target = im.targets[static_cast<usize>(h64(seed, 3'000'000 + i) % im.targets.size())];
        const u64 h = h64(seed, 4'000'000 + i);
        const TempEntity t = cb.spawn({.frame = w.frameOf(shooter)});
        cb.set(t, *w.get<c::Position>(shooter));
        cb.set(t, c::Velocity{{unit(h64(h, 1)) * 800 - 400, 0, unit(h64(h, 2)) * 800 - 400}, {}});
        cb.set(t, c::Projectile{target, 5.0f + static_cast<f32>(h % 10)});
        cb.set(t, c::Lifetime{0.5f + unit(h64(h, 3)) * 3.0f});
        cb.set(t, c::Faction{static_cast<u16>(h % 7)});
        cb.set(t, c::Bounds{0.2f});
        cb.set(t, c::SpatialCell{});
        std::vector<ComponentId> key{idPos, idVel, idProj, idLife, idFac, idBnd, idCell};
        addTag(t, 37 + static_cast<u32>(h % 3), key); // bolt / missile / rail
        if (h64(h, 4) % 2 == 0) addTag(t, 40, key);    // homing
        if (h64(h, 5) % 2 == 0) addTag(t, 41, key);    // tracer
        commit(key);
    }
    for (u32 i = 0; i < m_config.loot; ++i) {
        const u64 h = h64(seed, 5'000'000 + i);
        const TempEntity t = cb.spawn({.frame = im.frames[i % m_config.grids]});
        cb.set(t, c::Position{gridPos(h)});
        cb.set(t, c::Rotation{});
        cb.set(t, c::Loot{static_cast<u32>(h % 1000), static_cast<u16>(1 + h % 5)});
        cb.set(t, c::Lifetime{10.0f + unit(h64(h, 1)) * 50.0f});
        cb.set(t, c::Bounds{0.5f});
        cb.set(t, c::SpatialCell{});
        std::vector<ComponentId> key{idPos, idRot, idLoot, idLife, idBnd, idCell};
        addTag(t, 42 + static_cast<u32>(h % 4), key); // rarity
        if (h64(h, 2) % 2 == 0) addTag(t, 46, key);    // quest
        if (h64(h, 3) % 2 == 0) addTag(t, 47, key);    // soulbound
        commit(key);
    }
    w.apply(cb);

    // ---- docking: some ships sit in station hangars (DockedTo, non-fragmenting) --------------------
    for (u32 i = 0; i < m_config.dockedShips && !im.stations.empty(); ++i) {
        (void)w.dock(im.targets[i * 3 % m_config.ships], im.stations[i % im.stations.size()]);
    }

    m_info.entities = static_cast<u32>(w.stats().entityCount);
    m_info.replicated = m_config.ships + turrets + m_config.npcs + m_config.projectiles + m_config.loot;
    m_info.placed = m_config.placed;
    m_info.bodies = bodies;
    m_info.archetypes = static_cast<u32>(im.archetypes.size());
    m_info.frames = m_config.grids;
    w.clearStructuralLog();
    ChangeList drop;
    w.gatherChanges(drop);
}

void BenchZone::registerSystems() {
    World& w = m_world;
    Impl& im = *m_impl;
    const std::vector<Entity>* targets = &im.targets;

    // AI utility scoring: 4 considerations per NPC; the target's position is a random-access read.
    (void)w.system("AiScoring")
        .stage(ecs::Stage::PrePhysics)
        .write<c::AiState>()
        .read<c::Position>()
        .read<c::Faction>()
        .read<c::Health>()
        .alsoReads<c::Position>()
        .grain(512)
        .each([targets](SystemContext& ctx, ChunkView& ch) {
            c::AiState* ai = ch.write<c::AiState>(0);
            const c::Position* pos = ch.read<c::Position>(1);
            const c::Faction* fac = ch.read<c::Faction>(2);
            const c::Health* hp = ch.read<c::Health>(3);
            const u64 tick = ctx.tick();
            for (u32 r = 0; r < ch.count(); ++r) {
                c::AiState& s = ai[r];
                if (!s.target || tick >= s.retargetTick) {
                    s.target = (*targets)[static_cast<usize>(h64(ch.entity(r).id, tick) % targets->size())];
                    s.retargetTick = static_cast<u32>(tick + 40 + h64(tick, ch.entity(r).id) % 40);
                }
                const c::Position* tp = ctx.get<c::Position>(s.target);
                const f64 dist = tp ? length(tp->p - pos[r].p) : 1e9;
                s.scores[0] = static_cast<f32>(1.0 / (1.0 + dist * 0.001));            // proximity
                s.scores[1] = 1.0f - hp[r].hp / std::max(1.0f, hp[r].maxHp);           // self-preservation
                s.scores[2] = (fac[r].id % 3 == 0) ? 0.8f : 0.2f;                      // aggression
                s.scores[3] = unit(h64(ch.entity(r).id, tick / 20));                   // boredom
                u32 best = 0;
                for (u32 k = 1; k < 4; ++k) best = s.scores[k] > s.scores[best] ? k : best;
                s.state = best;
            }
        });

    // Ship flight control toward a waypoint, limited by the prefab-shared ShipClass.
    (void)w.system("ShipControl")
        .stage(ecs::Stage::PrePhysics)
        .write<c::Velocity>()
        .write<c::Thrusters>()
        .read<c::Position>()
        .read<c::ShipClass>()
        .read<c::Waypoint>()
        .grain(512)
        .each([](SystemContext& ctx, ChunkView& ch) {
            auto vel = ch.mutColumn<c::Velocity>(0);
            c::Thrusters* thr = ch.write<c::Thrusters>(1);
            const c::Position* pos = ch.read<c::Position>(2);
            const c::Waypoint* way = ch.read<c::Waypoint>(4);
            const f32 dt = ctx.dt();
            for (u32 r = 0; r < ch.count(); ++r) {
                const c::ShipClass& cls = ch.at<c::ShipClass>(3, r);
                const DVec3 to = way[r].target - pos[r].p;
                const f64 d = length(to);
                const Vec3 dir = d > 1.0 ? toF32(to / d) : Vec3{};
                const Vec3 cur = vel[r]->lin;
                Vec3 next = cur + dir * (cls.accel * dt);
                const f32 speed = length(next);
                if (speed > cls.maxSpeed) next = next * (cls.maxSpeed / speed);
                thr[r].throttle = d > 1.0 ? 1.0f : 0.0f;
                if (next != cur) vel[r].set<&c::Velocity::lin>(next);
            }
        });

    // Weapons fire projectiles through the job's command buffer (structural churn).
    (void)w.system("WeaponFire")
        .stage(ecs::Stage::PrePhysics)
        .write<c::Weapon>()
        .read<c::Position>()
        .read<c::Faction>()
        .with<c::Velocity>()
        .grain(512)
        .each([this, targets](SystemContext& ctx, ChunkView& ch) {
            c::Weapon* wpn = ch.write<c::Weapon>(0);
            const c::Position* pos = ch.read<c::Position>(1);
            const c::Faction* fac = ch.read<c::Faction>(2);
            const f32 dt = ctx.dt();
            for (u32 r = 0; r < ch.count(); ++r) {
                c::Weapon& wp = wpn[r];
                wp.cooldown -= dt;
                if (wp.cooldown > 0 || wp.ammo == 0) continue;
                wp.cooldown = 1.0f / wp.rate + 2.0f; // bursts every few seconds
                --wp.ammo;
                const u64 h = h64(ch.entity(r).id, ctx.tick());
                if (h % 3 != 0) continue; // only some weapons have a target in range
                CommandBuffer& cmd = ctx.commands();
                const TempEntity t = cmd.spawn({.frame = m_world.frameOf(ch.entity(r))});
                cmd.set(t, c::Position{pos[r].p});
                cmd.set(t, c::Velocity{{unit(h64(h, 1)) * 800 - 400, 0, unit(h64(h, 2)) * 800 - 400}, {}});
                cmd.set(t, c::Projectile{(*targets)[static_cast<usize>(h % targets->size())], wp.damage});
                cmd.set(t, c::Lifetime{0.5f + unit(h64(h, 3)) * 3.0f});
                cmd.set(t, c::Faction{fac[r].id});
                cmd.set(t, c::Bounds{0.2f});
                cmd.set(t, c::SpatialCell{});
                cmd.addId(t, m_impl->tags[37 + static_cast<u32>(h % 3)]);
            }
        });

    // Frame-local integration (f64 positions, first-order quaternion integration).
    (void)w.system("Integrate")
        .stage(ecs::Stage::Physics)
        .write<c::Position>()
        .read<c::Velocity>()
        .grain(1024)
        .each([](SystemContext& ctx, ChunkView& ch) {
            auto pos = ch.mutColumn<c::Position>(0);
            const c::Velocity* vel = ch.read<c::Velocity>(1);
            const f64 dt = ctx.dt();
            for (u32 r = 0; r < ch.count(); ++r) {
                const Vec3 v = vel[r].lin;
                if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) continue;
                pos[r].raw().p += toF64(v) * dt;
            }
        });
    (void)w.system("IntegrateRotation")
        .stage(ecs::Stage::Physics)
        .write<c::Rotation>()
        .read<c::Velocity>()
        .grain(1024)
        .each([](SystemContext& ctx, ChunkView& ch) {
            auto rot = ch.mutColumn<c::Rotation>(0);
            const c::Velocity* vel = ch.read<c::Velocity>(1);
            const f32 half = 0.5f * ctx.dt();
            for (u32 r = 0; r < ch.count(); ++r) {
                const Vec3 w3 = vel[r].ang;
                if (w3.x == 0.0f && w3.y == 0.0f && w3.z == 0.0f) continue;
                Quat& q = rot[r].raw().q;
                const Quat dq = Quat(w3.x, w3.y, w3.z, 0.0f) * q;
                q = normalize(Quat(q.x + dq.x * half, q.y + dq.y * half, q.z + dq.z * half, q.w + dq.w * half));
            }
        });

    // Projectiles: age, hit (damage event), despawn.
    (void)w.system("ProjectileUpdate")
        .stage(ecs::Stage::PostPhysics)
        .write<c::Lifetime>()
        .read<c::Projectile>()
        .grain(512)
        .each([this](SystemContext& ctx, ChunkView& ch) {
            c::Lifetime* life = ch.write<c::Lifetime>(0);
            const c::Projectile* proj = ch.read<c::Projectile>(1);
            std::vector<DamageEvent>& events = m_impl->damageEvents[ctx.jobIndex()];
            for (u32 r = 0; r < ch.count(); ++r) {
                life[r].remaining -= ctx.dt();
                const u64 h = h64(ch.entity(r).id, ctx.tick());
                const bool hit = h % 64 == 0;
                if (hit) events.push_back({proj[r].target, proj[r].damage});
                if (hit || life[r].remaining <= 0) ctx.commands().destroy(ch.entity(r));
            }
        });
    // Loot decays and respawns elsewhere (keeps the population stable).
    (void)w.system("LootDecay")
        .stage(ecs::Stage::PostPhysics)
        .write<c::Lifetime>()
        .read<c::Loot>()
        .read<c::Position>()
        .grain(512)
        .each([this](SystemContext& ctx, ChunkView& ch) {
            c::Lifetime* life = ch.write<c::Lifetime>(0);
            const c::Loot* loot = ch.read<c::Loot>(1);
            const c::Position* pos = ch.read<c::Position>(2);
            for (u32 r = 0; r < ch.count(); ++r) {
                life[r].remaining -= ctx.dt();
                if (life[r].remaining > 0) continue;
                CommandBuffer& cmd = ctx.commands();
                cmd.destroy(ch.entity(r));
                const u64 h = h64(ch.entity(r).id, ctx.tick());
                const TempEntity t = cmd.spawn({.frame = m_world.frameOf(ch.entity(r))});
                cmd.set(t, c::Position{pos[r].p + DVec3(unit(h) * 100.0, 0.0, 0.0)});
                cmd.set(t, c::Rotation{});
                cmd.set(t, c::Loot{loot[r].item, loot[r].count});
                cmd.set(t, c::Lifetime{10.0f + unit(h64(h, 1)) * 50.0f});
                cmd.set(t, c::Bounds{0.5f});
                cmd.set(t, c::SpatialCell{});
                cmd.addId(t, m_impl->tags[42 + static_cast<u32>(h % 4)]);
            }
        });
    (void)w.system("ShieldRegen")
        .stage(ecs::Stage::PostPhysics)
        .write<c::Shield>()
        .read<c::ShipClass>()
        .grain(512)
        .each([](SystemContext& ctx, ChunkView& ch) {
            auto sh = ch.mutColumn<c::Shield>(0);
            for (u32 r = 0; r < ch.count(); ++r) {
                const f32 cap = ch.at<c::ShipClass>(1, r).maxHp * 0.5f;
                const f32 v = sh[r]->value;
                if (v < cap) sh[r].set<&c::Shield::value>(std::min(cap, v + sh[r]->regen * ctx.dt()));
            }
        });

    // Damage application: one job, events consumed in producer job order (deterministic).
    (void)w.system("ApplyDamage")
        .stage(ecs::Stage::AuthorityFlush)
        .alsoWrites<c::Health>()
        .alsoReads<c::Position>()
        .singleJob()
        .once([this](SystemContext& ctx) {
            for (std::vector<DamageEvent>& list : m_impl->damageEvents) {
                for (const DamageEvent& ev : list) {
                    Mut<c::Health> hp = ctx.mut<c::Health>(ev.target);
                    if (!hp) continue;
                    const f32 next = hp->hp - ev.amount;
                    if (next > 0) {
                        hp.set<&c::Health::hp>(next);
                        continue;
                    }
                    hp.set<&c::Health::hp>(hp->maxHp); // "respawn" in place: population stays stable
                    // Loot drop.
                    const c::Position* p = ctx.get<c::Position>(ev.target);
                    const TempEntity t = ctx.commands().spawn({.frame = m_world.frameOf(ev.target)});
                    ctx.commands().set(t, c::Position{p ? p->p : DVec3{}});
                    ctx.commands().set(t, c::Rotation{});
                    ctx.commands().set(t, c::Loot{7, 1});
                    ctx.commands().set(t, c::Lifetime{30.0f});
                    ctx.commands().set(t, c::Bounds{0.5f});
                    ctx.commands().set(t, c::SpatialCell{});
                }
                list.clear();
            }
        });

    // Interest-management cell keys for everything that moves.
    (void)w.system("SpatialHash")
        .stage(ecs::Stage::ReplicationGather)
        .write<c::SpatialCell>()
        .read<c::Position>()
        .read<c::Bounds>()
        .with<c::Velocity>()
        .grain(2048)
        .each([](SystemContext&, ChunkView& ch) {
            c::SpatialCell* cell = ch.write<c::SpatialCell>(0);
            const c::Position* pos = ch.read<c::Position>(1);
            for (u32 r = 0; r < ch.count(); ++r) cell[r].key = cellKey(pos[r].p);
        });

    HELIOS_VERIFY(w.buildSchedule().hasValue());
}

void BenchZone::tick(f32 dt) {
    Impl& im = *m_impl;
    im.clockMs += 50;
    // Enough event lists for the largest job count ProjectileUpdate can produce.
    const usize maxJobs = (m_info.entities / 512) + 64;
    if (im.damageEvents.size() < maxJobs) im.damageEvents.resize(maxJobs);
    HELIOS_VERIFY(m_world.tick(dt).hasValue());
    m_world.gatherChanges(m_changes);
    m_world.clearStructuralLog();
}

u64 BenchZone::stateHash() {
    World& w = m_world;
    std::vector<std::pair<u64, Entity>> ids;
    Query all(w, {Term{w.id<c::Position>(), TermAccess::Read}});
    all.forEachChunk([&](ChunkView& ch) {
        for (u32 r = 0; r < ch.count(); ++r) ids.emplace_back(w.entityId(ch.entity(r)).value, ch.entity(r));
    });
    std::sort(ids.begin(), ids.end());
    Hasher64 hasher;
    for (const auto& [id, e] : ids) {
        hasher.updateValue(id);
        const c::Position* p = w.get<c::Position>(e);
        hasher.updateValue(std::bit_cast<u64>(p->p.x));
        hasher.updateValue(std::bit_cast<u64>(p->p.z));
        if (const c::Health* hp = w.get<c::Health>(e)) hasher.updateValue(std::bit_cast<u32>(hp->hp));
        if (const c::Shield* sh = w.get<c::Shield>(e)) hasher.updateValue(std::bit_cast<u32>(sh->value));
    }
    hasher.updateValue(static_cast<u64>(ids.size()));
    return hasher.digest();
}

u32 BenchZone::iterate3(u64* collectNs, u32* chunkCount) {
    World& w = m_world;
    if (!m_impl->iterQuery) {
        m_impl->iterQuery = std::make_unique<Query>(
            w, std::vector<Term>{Term{w.id<c::Position>(), TermAccess::Read}, Term{w.id<c::Bounds>(), TermAccess::Read},
                                 Term{w.id<c::SpatialCell>(), TermAccess::Write}});
    }
    std::vector<ChunkData>& chunks = m_impl->iterChunks;
    chunks.clear();
    const Stopwatch sw;
    m_impl->iterQuery->collect(chunks);
    if (collectNs) *collectNs = sw.elapsedNanos();
    if (chunkCount) *chunkCount = static_cast<u32>(chunks.size());
    u32 visited = 0;
    for (const ChunkData& chunk : chunks) {
        const auto* pos = static_cast<const c::Position*>(chunk.fields[0]);
        const auto* bnd = static_cast<const c::Bounds*>(chunk.fields[1]);
        auto* cell = static_cast<c::SpatialCell*>(chunk.fields[2]);
        constexpr f64 inv = 1.0 / kCellSize;
        for (u32 r = 0; r < chunk.count; ++r) {
            // Packed 21-bit cell coordinates of the bounding sphere's min corner.
            const f64 rad = bnd[r].radius;
            const auto cx = static_cast<i64>((pos[r].p.x - rad) * inv);
            const auto cy = static_cast<i64>((pos[r].p.y - rad) * inv);
            const auto cz = static_cast<i64>((pos[r].p.z - rad) * inv);
            cell[r].key = (static_cast<u64>(cx) & 0x1FFFFF) | ((static_cast<u64>(cy) & 0x1FFFFF) << 21) |
                          ((static_cast<u64>(cz) & 0x1FFFFF) << 42);
        }
        visited += chunk.count;
    }
    return visited;
}

BurstResult BenchZone::structuralBurst(u32 round, bool profiled) {
    World& w = m_world;
    Impl& im = *m_impl;
    BurstResult res;
    // Victims chosen deterministically: round 0 destroys loot and adds/removes NPC tags (creating
    // the new tag-combination tables: "cold"); later rounds destroy the projectiles the previous
    // round created and alternately revert / re-apply the tag changes (all tables exist: "warm",
    // the steady state of a running zone).
    std::vector<Entity> victims, npcs;
    if (round == 0 || im.burstCreated.empty()) {
        Query vq(w, {Term{w.id<c::Loot>(), TermAccess::Read}});
        vq.forEachChunk([&](ChunkView& ch) {
            for (u32 r = 0; r < ch.count() && victims.size() < 3000; ++r) victims.push_back(ch.entity(r));
        });
    } else {
        // The previous round's projectiles, as the raw-flecs burst deletes its own creates. (Taking
        // the first 3,000 projectiles in query order instead picked the zone's older ones and let
        // the burst tables grow every round, a table-growth cost the raw burst never pays.)
        for (const Entity e : im.burstCreated) {
            if (w.isAlive(e)) victims.push_back(e);
        }
    }
    npcs = burstNpcs();

    CommandBuffer& creates = im.creates;
    CommandBuffer& destroys = im.destroys;
    CommandBuffer& toggles = im.toggles;
    CommandBuffer& statuses = im.statuses;
    for (CommandBuffer* b : {&creates, &destroys, &toggles, &statuses}) b->setWorld(&w);
    const Stopwatch record;
    const u32 frameCount = static_cast<u32>(im.frames.size());
    std::vector<TempEntity> createdTemps(3000);
    if (m_config.perCommandCreates) {
        // One spawn() and seven set() commands per projectile (fused at apply time).
        for (u32 i = 0; i < 3000; ++i) {
            const u64 h = h64(0xB0057 + round, i);
            const TempEntity t = creates.spawn({.frame = im.frames[i % frameCount]});
            creates.set(t, c::Position{DVec3(unit(h) * 1000.0, 0.0, 0.0)});
            creates.set(t, c::Velocity{{100, 0, 0}, {}});
            creates.set(t, c::Projectile{im.targets[i % im.targets.size()], 5});
            creates.set(t, c::Lifetime{2});
            creates.set(t, c::Faction{1});
            creates.set(t, c::Bounds{0.2f});
            creates.set(t, c::SpatialCell{});
            createdTemps[i] = t;
        }
    } else {
        // The same projectiles as one spawnN() batch per frame, values in column arrays: how a
        // system records a volley (ADR-004a item 4).
        std::vector<c::Position> pos;
        std::vector<c::Projectile> proj;
        const std::vector<c::Velocity> vel(3000 / frameCount + 1, c::Velocity{{100, 0, 0}, {}});
        const std::vector<c::Lifetime> life(vel.size(), c::Lifetime{2});
        const std::vector<c::Faction> fac(vel.size(), c::Faction{1});
        const std::vector<c::Bounds> bounds(vel.size(), c::Bounds{0.2f});
        const std::vector<c::SpatialCell> cells(vel.size(), c::SpatialCell{});
        for (u32 f = 0; f < frameCount; ++f) {
            pos.clear();
            proj.clear();
            for (u32 i = f; i < 3000; i += frameCount) {
                pos.push_back(c::Position{DVec3(unit(h64(0xB0057 + round, i)) * 1000.0, 0.0, 0.0)});
                proj.push_back(c::Projectile{im.targets[i % im.targets.size()], 5});
            }
            const u32 n = static_cast<u32>(pos.size());
            const TempEntity first = creates.spawnN({.frame = im.frames[f]}, n, pos.data(), vel.data(), proj.data(),
                                                    life.data(), fac.data(), bounds.data(), cells.data());
            for (u32 k = 0; k < n; ++k) createdTemps[f + k * frameCount] = TempEntity{first.index + k};
        }
    }
    for (const Entity e : victims) destroys.destroy(e);
    for (usize i = 0; i < npcs.size(); ++i) {
        const ComponentId cloak = im.tags[5];
        const bool apply = round % 2 == 0;
        if (i % 2 == 0) {
            if (apply) {
                toggles.addId(npcs[i], cloak);
            } else {
                toggles.removeId(npcs[i], cloak);
            }
        } else if (apply) {
            // Remove the species tag the NPC has (8..10); the next round adds one back.
            for (u32 sp = 8; sp <= 10; ++sp) {
                if (w.hasId(npcs[i], im.tags[sp])) {
                    toggles.removeId(npcs[i], im.tags[sp]);
                    break;
                }
            }
        } else {
            toggles.addId(npcs[i], im.tags[8 + static_cast<u32>(mix64(i) % 3)]);
        }
        // The same churn on a DontFragment component (status effect): no table move.
        if (apply) {
            statuses.set(npcs[i], c::Status{static_cast<u32>(i)});
        } else {
            statuses.remove<c::Status>(npcs[i]);
        }
    }
    res.recordMs = record.elapsedMillis();
    res.commands = static_cast<u32>(creates.size() + destroys.size() + toggles.size());
    Stopwatch sw;
    profiled ? timed::worldCreates(w, creates) : w.apply(creates);
    res.createMs = sw.elapsedMillis();
    im.burstCreated.clear();
    for (const TempEntity t : createdTemps) im.burstCreated.push_back(creates.resolved(t));
    sw.reset();
    profiled ? timed::worldDestroys(w, destroys) : w.apply(destroys);
    res.destroyMs = sw.elapsedMillis();
    sw.reset();
    profiled ? timed::worldToggles(w, toggles) : w.apply(toggles);
    res.toggleMs = sw.elapsedMillis();
    sw.reset();
    profiled ? timed::worldStatuses(w, statuses) : w.apply(statuses);
    res.toggleDontFragmentMs = sw.elapsedMillis();
    w.clearStructuralLog();
    return res;
}

std::vector<Entity> BenchZone::burstNpcs() {
    std::vector<Entity> npcs;
    Query nq(m_world, {Term{m_world.id<c::AiState>(), TermAccess::Read}});
    nq.forEachChunk([&](ChunkView& ch) {
        for (u32 r = 0; r < ch.count(); ++r) npcs.push_back(ch.entity(r));
    });
    std::sort(npcs.begin(), npcs.end());
    if (npcs.size() > 3000) npcs.resize(3000);
    return npcs;
}

namespace ops {

/// Column arrays of all 3,000 creates (frame f takes elements [f * perFrame, ...), as the World's
/// burst has 3,000 distinct values), plus zeros for NetIdentity and RepDirty.
struct RawColumns {
    struct Column {
        ecs_id_t id;
        const std::byte* data;
        usize size;
    };
    Column columns[7];
    void* zeros;
};

// 3,000 creates: one ecs_bulk_init with values per frame table.
void rawCreates(ecs_world_t* fw, const std::vector<ecs_table_t*>& tables, u32 perFrame,
                                const RawColumns& c, std::vector<ecs_entity_t>& created) {
    u32 remaining = 3000;
    for (usize f = 0; f < tables.size() && remaining > 0; ++f) {
        const u32 count = std::min(perFrame, remaining);
        remaining -= count;
        const ecs_type_t* type = ecs_table_get_type(tables[f]);
        std::vector<void*> data(static_cast<usize>(type->count), nullptr);
        for (i32 k = 0; k < type->count; ++k) {
            const ecs_id_t id = type->array[k];
            if (ecs_get_typeid(fw, id) != 0) data[k] = c.zeros; // NetIdentity, RepDirty
            for (const RawColumns::Column& col : c.columns) {
                if (col.id == id) data[k] = const_cast<std::byte*>(col.data + f * perFrame * col.size);
            }
        }
        ecs_bulk_desc_t bd{};
        bd.count = static_cast<i32>(count);
        bd.table = tables[f];
        bd.data = data.data();
        const ecs_entity_t* es = ecs_bulk_init(fw, &bd);
        created.insert(created.end(), es, es + count);
    }
}

void rawDestroys(ecs_world_t* fw, const std::vector<ecs_entity_t>& created) {
    for (const ecs_entity_t e : created) ecs_delete(fw, e);
}

// The NPC tag toggles of structuralBurst(): even NPCs toggle the cloak tag, odd ones lose or regain
// a species tag.
void rawToggles(ecs_world_t* fw, const std::vector<Entity>& npcs, bool apply, ecs_id_t cloak,
                                const std::vector<ecs_id_t>& species) {
    for (usize i = 0; i < npcs.size(); ++i) {
        const ecs_entity_t e = npcs[i].id;
        if (i % 2 == 0) {
            if (apply) {
                ecs_add_id(fw, e, cloak);
            } else {
                ecs_remove_id(fw, e, cloak);
            }
        } else if (apply) {
            for (const ecs_id_t sp : species) {
                if (ecs_has_id(fw, e, sp)) {
                    ecs_remove_id(fw, e, sp);
                    break;
                }
            }
        } else {
            ecs_add_id(fw, e, species[static_cast<usize>(mix64(i) % species.size())]);
        }
    }
}

void rawStatuses(ecs_world_t* fw, const std::vector<Entity>& npcs, bool apply, ecs_id_t status) {
    for (usize i = 0; i < npcs.size(); ++i) {
        if (apply) {
            const c::Status value{static_cast<u32>(i)};
            ecs_set_id(fw, npcs[i].id, status, sizeof value, &value);
        } else {
            ecs_remove_id(fw, npcs[i].id, status);
        }
    }
}

} // namespace ops

namespace timed {
HELIOS_NOINLINE void rawCreates(ecs_world_t* fw, const std::vector<ecs_table_t*>& tables, u32 perFrame,
                                const ops::RawColumns& c, std::vector<ecs_entity_t>& created) {
    ops::rawCreates(fw, tables, perFrame, c, created);
}
HELIOS_NOINLINE void rawDestroys(ecs_world_t* fw, const std::vector<ecs_entity_t>& created) {
    ops::rawDestroys(fw, created);
}
HELIOS_NOINLINE void rawToggles(ecs_world_t* fw, const std::vector<Entity>& npcs, bool apply, ecs_id_t cloak,
                                const std::vector<ecs_id_t>& species) {
    ops::rawToggles(fw, npcs, apply, cloak, species);
}
HELIOS_NOINLINE void rawStatuses(ecs_world_t* fw, const std::vector<Entity>& npcs, bool apply, ecs_id_t status) {
    ops::rawStatuses(fw, npcs, apply, status);
}
} // namespace timed

BurstResult BenchZone::rawFlecsBurst(u32 round, bool profiled) {
    World& w = m_world;
    Impl& im = *m_impl;
    ecs_world_t* fw = w.flecsWorld();
    BurstResult res;
    const std::vector<Entity> npcs = burstNpcs();

    // Final table per frame: the type of a projectile spawned by the World (7 components +
    // NetIdentity + RepDirty + (InFrame, frame)) with the frame pair swapped.
    HELIOS_VERIFY(!im.burstCreated.empty() && w.isAlive(im.burstCreated.front()));
    const Entity sample = im.burstCreated.front();
    const ecs_type_t* sampleType = ecs_get_type(fw, sample.id);
    const ecs_id_t inFrame = w.inFrameRelation().id;
    const u32 frameCount = static_cast<u32>(im.frames.size());
    std::vector<ecs_table_t*> tables(frameCount);
    for (u32 f = 0; f < frameCount; ++f) {
        std::vector<ecs_id_t> ids(sampleType->array, sampleType->array + sampleType->count);
        for (ecs_id_t& id : ids) {
            if (ECS_IS_PAIR(id) && ECS_PAIR_FIRST(id) == static_cast<u32>(inFrame)) id = ecs_pair(inFrame, im.frames[f].id);
        }
        std::sort(ids.begin(), ids.end());
        tables[f] = ecs_table_find(fw, ids.data(), static_cast<i32>(ids.size()));
        HELIOS_VERIFY(tables[f] != nullptr);
    }

    // Values in column order (per frame group), as the World writes them.
    const u32 perFrame = (3000 + frameCount - 1) / frameCount;
    const ecs_id_t idPos = w.id<c::Position>(), idVel = w.id<c::Velocity>(), idProj = w.id<c::Projectile>(),
                   idLife = w.id<c::Lifetime>(), idFac = w.id<c::Faction>(), idBounds = w.id<c::Bounds>(),
                   idCell = w.id<c::SpatialCell>();
    const usize total = static_cast<usize>(perFrame) * frameCount;
    std::vector<c::Position> pos(total);
    std::vector<c::Velocity> vel(total, c::Velocity{{100, 0, 0}, {}});
    std::vector<c::Projectile> proj(total);
    std::vector<c::Lifetime> life(total, c::Lifetime{2});
    std::vector<c::Faction> fac(total, c::Faction{1});
    std::vector<c::Bounds> bounds(total, c::Bounds{0.2f});
    std::vector<c::SpatialCell> cells(total);
    std::vector<std::byte> zeros(perFrame * 64);
    for (u32 i = 0; i < total; ++i) {
        pos[i].p = DVec3(unit(h64(0xFA57 + round, i)) * 1000.0, 0.0, 0.0);
        proj[i] = c::Projectile{im.targets[i % im.targets.size()], 5};
    }
    std::vector<ecs_entity_t> created;
    created.reserve(3000);
    auto col = [](ecs_id_t id, const auto& v) {
        return ops::RawColumns::Column{id, reinterpret_cast<const std::byte*>(v.data()), sizeof(v[0])};
    };
    const ops::RawColumns columns{{col(idPos, pos), col(idVel, vel), col(idProj, proj), col(idLife, life),
                                   col(idFac, fac), col(idBounds, bounds), col(idCell, cells)},
                                  zeros.data()};
    Stopwatch sw;
    profiled ? timed::rawCreates(fw, tables, perFrame, columns, created) : ops::rawCreates(fw, tables, perFrame, columns, created);
    res.createMs = sw.elapsedMillis();

    // Like the World burst, delete the previous burst's creates while this burst's sit at the ends
    // of the tables (so every delete moves a row into the hole, as it does for the World).
    sw.reset();
    profiled ? timed::rawDestroys(fw, im.rawCreated) : ops::rawDestroys(fw, im.rawCreated);
    res.destroyMs = sw.elapsedMillis();
    im.rawCreated = std::move(created);

    const bool apply = round % 2 == 0;
    std::vector<ecs_id_t> species{im.tags[8], im.tags[9], im.tags[10]};
    sw.reset();
    profiled ? timed::rawToggles(fw, npcs, apply, im.tags[5], species) : ops::rawToggles(fw, npcs, apply, im.tags[5], species);
    res.toggleMs = sw.elapsedMillis();

    sw.reset();
    const ecs_id_t status = w.id<c::Status>();
    profiled ? timed::rawStatuses(fw, npcs, apply, status) : ops::rawStatuses(fw, npcs, apply, status);
    res.toggleDontFragmentMs = sw.elapsedMillis();
    res.commands = 9000;
    return res;
}

u32 BenchZone::liveProjectiles() {
    Query q(m_world, {Term{m_world.id<c::Projectile>(), TermAccess::Read}});
    return q.count();
}

} // namespace bench
