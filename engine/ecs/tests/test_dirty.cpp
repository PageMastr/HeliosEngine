// Per-field dirty tracking (Mut<C>) and the per-tick change list, including parallel systems that
// write different components of the same entities.

#include <doctest/doctest.h>

#include <map>
#include <set>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

static_assert(kFieldIndex<Health, &Health::hp> == 0);
static_assert(kFieldIndex<Health, &Health::maxHp> == 1);
static_assert(kFieldIndex<Health, &Health::armor> == 2);
static_assert(kFieldIndex<Health, &Shield::value> == kNoField);
static_assert(kReplicatedFieldCount<Health> == 3);
static_assert(ReplicatedComponent<Position>);
static_assert(!ReplicatedComponent<Velocity>);

TEST_CASE("ecs dirty: Mut marks exactly the written fields") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    world.set(e, Health{});
    world.beginTick();
    {
        Mut<Health> m = world.mut<Health>(e);
        REQUIRE(m.isValid());
        m.set<&Health::hp>(90.0f);
        m.set<&Health::maxHp>(100.0f); // unchanged value: not marked
        CHECK(m.dirtyFields() == 0b001);
        m.mark<&Health::armor>();
        CHECK(m.dirtyFields() == 0b101);
    }
    const RepDirty* rep = world.get<RepDirty>(e);
    REQUIRE(rep);
    const u64 healthBit = world.replicationBit<Health>();
    CHECK(rep->componentMask == healthBit);
    CHECK(rep->changed == world.currentTick());

    ChangeList changes;
    world.gatherChanges(changes);
    CHECK(changes.tick == world.currentTick());
    CHECK(changes.entityCount == 1);
    REQUIRE(changes.changes.size() == 1);
    CHECK(changes.changes[0].entity == world.entityId(e));
    CHECK(changes.changes[0].handle == world.netHandle(e));
    CHECK(changes.changes[0].replIndex == world.componentInfo(world.id<Health>())->replIndex);
    CHECK(changes.changes[0].fields == 0b101);
    // Gather clears both levels.
    CHECK(world.get<RepDirty>(e)->componentMask == 0);
    CHECK(world.get<Health>(e)->_dirty == 0);
    world.gatherChanges(changes);
    CHECK(changes.changes.empty());
    CHECK(changes.entityCount == 0);
}

TEST_CASE("ecs dirty: raw() marks all fields; non-replicated components are untracked") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    world.set(e, Health{});
    world.set(e, Velocity{});
    world.mut<Health>(e).raw().armor = 9;
    Mut<Velocity> v = world.mut<Velocity>(e);
    v.raw().value.x = 3.0f;
    CHECK(v.dirtyFields() == 0);
    ChangeList changes;
    world.gatherChanges(changes);
    REQUIRE(changes.changes.size() == 1);
    CHECK(changes.changes[0].fields == 0b111);
    CHECK(world.get<Velocity>(e)->value.x == 3.0f);
    CHECK_FALSE(world.mut<Shield>(e).isValid()); // absent component
}

TEST_CASE("ecs dirty: change list groups entities deterministically") {
    World world;
    registerCommon(world);
    std::vector<Entity> es;
    for (int i = 0; i < 100; ++i) {
        const Entity e = world.spawn();
        world.set(e, Health{});
        world.set(e, Position{});
        es.push_back(e);
    }
    for (int i = 0; i < 100; i += 3) world.mut<Health>(es[static_cast<size_t>(i)]).set<&Health::hp>(1.0f);
    for (int i = 0; i < 100; i += 5) world.mut<Position>(es[static_cast<size_t>(i)]).raw().value.x = 1.0;
    ChangeList changes;
    world.gatherChanges(changes);
    std::set<u64> dirtyEntities;
    for (const ComponentChange& c : changes.changes) dirtyEntities.insert(c.entity.value);
    CHECK(changes.entityCount == dirtyEntities.size());
    // 34 multiples of 3, 20 of 5, 7 of 15 in [0, 100).
    CHECK(changes.entityCount == 34 + 20 - 7);
    CHECK(changes.changes.size() == 34 + 20);
    // Entries of one entity are adjacent (grouped by entity, component bits ascending).
    std::set<u64> closed;
    u64 current = 0;
    for (const ComponentChange& c : changes.changes) {
        if (c.entity.value != current) {
            CHECK(closed.count(c.entity.value) == 0);
            if (current) closed.insert(current);
            current = c.entity.value;
        }
    }
}

namespace {

/// Two systems in the same stage write *different* replicated components of the same entities, so
/// the scheduler runs them in parallel; both mark RepDirty of the same rows concurrently.
void addParallelWriters(World& world) {
    CHECK(world.system("WriteHealth")
        .stage(Stage::PostPhysics)
        .write<Health>()
        .read<Counter>()
        .grain(64)
        .each([](SystemContext& ctx, ChunkView& chunk) {
            auto health = chunk.mutColumn<Health>(0);
            const Counter* ids = chunk.read<Counter>(1);
            for (u32 r = 0; r < chunk.count(); ++r) {
                if ((ids[r].value + ctx.tick()) % 3 == 0) health[r].set<&Health::hp>(static_cast<f32>(ctx.tick()));
            }
        }).hasValue());
    CHECK(world.system("WritePosition")
        .stage(Stage::PostPhysics)
        .write<Position>()
        .read<Counter>()
        .grain(64)
        .each([](SystemContext& ctx, ChunkView& chunk) {
            auto pos = chunk.mutColumn<Position>(0);
            const Counter* ids = chunk.read<Counter>(1);
            for (u32 r = 0; r < chunk.count(); ++r) {
                if ((ids[r].value + ctx.tick()) % 4 == 0) pos[r].raw().value.y = static_cast<f64>(ctx.tick());
            }
        }).hasValue());
}

} // namespace

TEST_CASE("ecs dirty: parallel systems writing different components of one entity") {
    jobs::JobSystem js({.workerCount = 4});
    World world({.jobs = &js});
    registerCommon(world);
    constexpr u32 kCount = 20'000;
    std::vector<Entity> es;
    for (u32 i = 0; i < kCount; ++i) {
        const Entity e = world.spawn();
        world.set(e, Counter{i});
        world.set(e, Health{});
        world.set(e, Position{});
        es.push_back(e);
    }
    addParallelWriters(world);
    REQUIRE(world.buildSchedule().hasValue());
    CHECK(world.scheduleEdges(Stage::PostPhysics).empty()); // no conflict: parallel
    const u8 hBit = world.componentInfo(world.id<Health>())->replIndex;
    const u8 pBit = world.componentInfo(world.id<Position>())->replIndex;

    ChangeList changes;
    world.gatherChanges(changes); // drop the initial state
    for (int tick = 0; tick < 12; ++tick) {
        REQUIRE(world.tick(0.05f).hasValue());
        world.gatherChanges(changes);
        const Tick t = world.currentTick();
        // Expected per entity: Health if (i + t) % 3 == 0 and hp actually changed (it changes every
        // time because it is set to the tick), Position if (i + t) % 4 == 0.
        std::map<u64, std::pair<bool, bool>> seen;
        for (const ComponentChange& c : changes.changes) {
            auto& entry = seen[c.entity.value];
            if (c.replIndex == hBit) {
                CHECK_FALSE(entry.first);
                entry.first = true;
                CHECK(c.fields == 0b001);
            } else if (c.replIndex == pBit) {
                CHECK_FALSE(entry.second);
                entry.second = true;
                CHECK(c.fields == 0b1);
            } else {
                FAIL("unexpected component");
            }
        }
        u32 expectedEntities = 0;
        u32 mismatches = 0;
        for (u32 i = 0; i < kCount; ++i) {
            const bool h = (i + t) % 3 == 0;
            const bool p = (i + t) % 4 == 0;
            if (h || p) ++expectedEntities;
            auto it = seen.find(world.entityId(es[i]).value);
            const bool gotH = it != seen.end() && it->second.first;
            const bool gotP = it != seen.end() && it->second.second;
            if (gotH != h || gotP != p) ++mismatches;
        }
        CHECK(mismatches == 0);
        CHECK(changes.entityCount == expectedEntities);
    }
}

TEST_CASE("ecs dirty: random-access Mut from a single-job system") {
    World world;
    registerCommon(world);
    const Entity target = world.spawn();
    world.set(target, Health{});
    const Entity shooter = world.spawn();
    world.set(shooter, Counter{static_cast<u64>(target.id)});
    CHECK(world.system("ApplyDamage")
        .stage(Stage::PostPhysics)
        .read<Counter>()
        .alsoWrites<Health>()
        .singleJob()
        .each([](SystemContext& ctx, ChunkView& chunk) {
            for (u32 r = 0; r < chunk.count(); ++r) {
                Mut<Health> h = ctx.mut<Health>(Entity(chunk.read<Counter>(0)[r].value));
                if (h) h.set<&Health::hp>(h->hp - 10.0f);
            }
        }).hasValue());
    REQUIRE(world.tick(0.05f).hasValue());
    CHECK(world.get<Health>(target)->hp == 90.0f);
    ChangeList changes;
    world.gatherChanges(changes);
    REQUIRE(changes.changes.size() == 1);
    CHECK(changes.changes[0].entity == world.entityId(target));
}
