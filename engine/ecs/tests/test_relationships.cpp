// Relationship helpers: hierarchy (non-fragmenting Parent and fragmenting ChildOf), reparenting
// and cycle checks, cascade deletes, InFrame with the reparent hook, DockedTo exclusivity and the
// fragmentation guard, plus prefabs with shared/copied components and overrides.

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {
WorldDesc hierarchyDesc(bool nonFragmenting) {
    WorldDesc d;
    d.relations.nonFragmentingHierarchy = nonFragmenting;
    return d;
}
} // namespace

TEST_CASE("ecs relationships: hierarchy, reparenting and cycle checks") {
    for (const bool nonFragmenting : {true, false}) {
        CAPTURE(nonFragmenting);
        World world(hierarchyDesc(nonFragmenting));
        const Entity ship = world.spawn();
        const Entity turret = world.spawn({.parent = ship});
        const Entity barrel = world.spawn({.parent = turret});
        const Entity other = world.spawn();
        CHECK(world.parentOf(turret) == ship);
        CHECK(world.parentOf(barrel) == turret);
        CHECK_FALSE(world.parentOf(ship).isValid());

        std::vector<Entity> kids;
        world.childrenOf(ship, kids);
        CHECK(kids == std::vector<Entity>{turret});

        // Reparent: turret moves to `other`, keeping its own subtree.
        CHECK(world.setParent(turret, other).hasValue());
        CHECK(world.parentOf(turret) == other);
        CHECK(world.parentOf(barrel) == turret);
        kids.clear();
        world.childrenOf(ship, kids);
        CHECK(kids.empty());

        // Cycles are refused: other -> turret -> barrel -> other.
        CHECK(world.setParent(other, barrel).errorCode() == ErrorCode::InvalidArgument);
        CHECK(world.setParent(other, other).errorCode() == ErrorCode::InvalidArgument);
        CHECK(world.parentOf(other) == Entity());

        world.clearParent(turret);
        CHECK_FALSE(world.parentOf(turret).isValid());
        CHECK(world.setParent(turret, ship).hasValue());

        // Cascade: deleting the ship deletes its subtree and unregisters their ids.
        const EntityId barrelId = world.entityId(barrel);
        const NetHandle barrelHandle = world.netHandle(barrel);
        world.clearStructuralLog();
        world.destroy(ship);
        CHECK_FALSE(world.isAlive(turret));
        CHECK_FALSE(world.isAlive(barrel));
        CHECK(world.isAlive(other));
        CHECK_FALSE(world.find(barrelId).isValid());
        CHECK_FALSE(world.find(barrelHandle).isValid());
        u32 destroys = 0;
        for (const StructuralEvent& ev : world.structuralLog()) destroys += ev.op == StructuralOp::Destroy ? 1u : 0u;
        CHECK(destroys == 3);
        CHECK(world.stats().entityCount == 1);
    }
}

TEST_CASE("ecs relationships: non-fragmenting hierarchy keeps the table count flat") {
    auto tablesFor = [](bool nonFragmenting) {
        World world(hierarchyDesc(nonFragmenting));
        world.registerComponent<Velocity>();
        const u32 base = world.stats().tableCount;
        for (int p = 0; p < 200; ++p) {
            const Entity parent = world.spawn();
            for (int c = 0; c < 3; ++c) world.set(world.spawn({.parent = parent}), Velocity{});
        }
        return world.stats().tableCount - base;
    };
    const u32 flat = tablesFor(true);
    const u32 fragmented = tablesFor(false);
    CHECK(flat < 10);
    CHECK(fragmented >= 200); // one table per parent
}

TEST_CASE("ecs relationships: frames and the reparent hook") {
    World world;
    const Entity system = world.createFrame(FrameId(1), "sol");
    const Entity shipGrid = world.createFrame(FrameId(7), "ship-grid");
    std::vector<std::pair<Entity, Entity>> calls;
    world.setReparentHook([&](World&, Entity, Entity from, Entity to) { calls.emplace_back(from, to); });

    const Entity crew = world.spawn({.frame = system});
    CHECK(world.frameOf(crew) == system);
    CHECK(world.get<FrameRef>(system)->frame == FrameId(1));
    world.clearStructuralLog();
    CHECK(world.setFrame(crew, shipGrid).hasValue()); // boarding
    CHECK(world.frameOf(crew) == shipGrid);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == system);
    CHECK(calls[0].second == shipGrid);
    REQUIRE(world.structuralLog().size() == 1);
    CHECK(world.structuralLog()[0].op == StructuralOp::SetFrame);
    CHECK(world.structuralLog()[0].arg == 7);
    CHECK(world.structuralLog()[0].entity == world.entityId(crew));

    CHECK(world.setFrame(crew, shipGrid).hasValue()); // no-op, no hook
    CHECK(calls.size() == 1);
    // Exclusive: an entity is in exactly one frame.
    CHECK_FALSE(world.hasId(crew, World::pair(world.inFrameRelation(), system)));
    CHECK(world.setFrame(crew, Entity()).hasValue());
    CHECK_FALSE(world.frameOf(crew).isValid());
    CHECK(calls.size() == 2);
}

TEST_CASE("ecs relationships: docking is exclusive, non-fragmenting and cleaned up with the host") {
  for (const DockStorage storage : {DockStorage::Field, DockStorage::PairDontFragment}) {
    CAPTURE(static_cast<int>(storage));
    WorldDesc desc;
    desc.relations.docking = storage;
    World world(desc);
    world.registerComponent<Velocity>();
    const Entity station = world.spawn();
    const Entity carrier = world.spawn();
    std::vector<Entity> fighters;
    for (int i = 0; i < 50; ++i) {
        fighters.push_back(world.spawn());
        world.set(fighters.back(), Velocity{});
    }
    const u32 tables = world.stats().tableCount;
    for (int i = 0; i < 50; ++i) CHECK(world.dock(fighters[static_cast<size_t>(i)], i % 2 ? station : carrier).hasValue());
    CHECK(world.stats().tableCount <= tables + 1); // DontFragment: no table per host

    CHECK(world.dockedTo(fighters[1]) == station);
    CHECK(world.dock(fighters[1], carrier).hasValue()); // re-dock replaces
    CHECK(world.dockedTo(fighters[1]) == carrier);
    CHECK(world.dock(station, station).errorCode() == ErrorCode::InvalidArgument);

    std::vector<Entity> atStation, atCarrier;
    world.dockedAt(station, atStation);
    world.dockedAt(carrier, atCarrier);
    CHECK(atStation.size() == 24);
    CHECK(atCarrier.size() == 26);
    CHECK(std::is_sorted(atStation.begin(), atStation.end()));

    world.undock(fighters[1]);
    CHECK_FALSE(world.dockedTo(fighters[1]).isValid());

    world.destroy(station); // docked ships survive; their dock pair is removed
    CHECK(world.isAlive(fighters[3]));
    CHECK_FALSE(world.dockedTo(fighters[3]).isValid());
    // Docked entities that die disappear from the host's list.
    world.destroy(fighters[0]);
    std::vector<Entity> left;
    world.dockedAt(carrier, left);
    CHECK(left.size() == 24); // 26 docked, fighters[1] undocked, fighters[0] destroyed
    // Heavy re-docking keeps the reverse index consistent.
    for (int round = 0; round < 20; ++round) {
        for (int i = 2; i < 50; ++i) {
            CHECK(world.dock(fighters[static_cast<size_t>(i)], (i + round) % 2 ? carrier : fighters[1]).hasValue());
        }
    }
    std::vector<Entity> atCarrier2, atFighter;
    world.dockedAt(carrier, atCarrier2);
    world.dockedAt(fighters[1], atFighter);
    CHECK(atCarrier2.size() + atFighter.size() == 48);
  }
}

TEST_CASE("ecs relationships: fragmenting DockedTo when configured") {
    WorldDesc d;
    d.relations.docking = DockStorage::Pair;
    World world(d);
    const u32 base = world.stats().tableCount;
    std::vector<Entity> hosts;
    for (int i = 0; i < 20; ++i) hosts.push_back(world.spawn());
    for (int i = 0; i < 20; ++i) CHECK(world.dock(world.spawn(), hosts[static_cast<size_t>(i)]).hasValue());
    CHECK(world.stats().tableCount - base >= 20);
}

TEST_CASE("ecs prefabs: shared components are inherited, others copied, overrides stick") {
    World world;
    registerCommon(world);
    const Entity fighter = world.createPrefab("Fighter");
    world.set(fighter, HullSpec{800.0f, 350.0f}); // Shared
    world.set(fighter, Health{350.0f, 350.0f, 5}); // copied into every instance
    world.add<Tagged>(fighter);

    const Entity a = world.instantiate(fighter);
    const Entity b = world.instantiate(fighter);
    CHECK(world.isA(a, fighter));
    CHECK(world.has<HullSpec>(a));
    CHECK_FALSE(world.owns<HullSpec>(a));
    CHECK(world.get<HullSpec>(a) == world.get<HullSpec>(fighter)); // same storage
    CHECK(world.owns<Health>(a));
    CHECK(world.get<Health>(a)->armor == 5);
    CHECK(world.has<Tagged>(a));
    // Identity is never inherited: each instance has its own EntityId.
    CHECK(world.entityId(a).isValid());
    CHECK(world.entityId(a) != world.entityId(b));
    CHECK(world.entityId(fighter) == EntityId());

    // Override on a: a gets its own copy (initialized from the prefab), b keeps sharing.
    world.override<HullSpec>(a);
    CHECK(world.owns<HullSpec>(a));
    CHECK(world.get<HullSpec>(a)->mass == 800.0f);
    world.set(a, HullSpec{1200.0f, 350.0f});
    world.set(fighter, HullSpec{900.0f, 400.0f}); // prefab edit (hot reload)
    CHECK(world.get<HullSpec>(a)->mass == 1200.0f);
    CHECK(world.get<HullSpec>(b)->mass == 900.0f);
    // Copied components do not follow later prefab edits.
    world.set(fighter, Health{1.0f, 1.0f, 0});
    CHECK(world.get<Health>(b)->hp == 350.0f);

    // Queries see shared values through the instance (ChunkView::isShared).
    u32 sharedRows = 0, ownedRows = 0;
    Query q(world, {Term{world.id<HullSpec>(), TermAccess::Read}});
    q.forEachChunk([&](ChunkView& c) {
        for (u32 r = 0; r < c.count(); ++r) {
            (c.isShared(0) ? sharedRows : ownedRows) += 1;
            CHECK(c.at<HullSpec>(0, r).maxHp > 0.0f);
        }
    });
    CHECK(sharedRows == 1); // b
    CHECK(ownedRows == 1);  // a (the prefab itself is not matched)
}

TEST_CASE("ecs prefabs: dirty-tracked write overrides an inherited replicated component") {
    World world;
    world.registerComponent<Shield>(ComponentFlags::Shared);
    const Entity prefab = world.createPrefab();
    world.set(prefab, Shield{75.0f});
    const Entity e = world.instantiate(prefab);
    CHECK_FALSE(world.owns<Shield>(e));
    Mut<Shield> m = world.mut<Shield>(e);
    REQUIRE(m.isValid());
    m.set<&Shield::value>(10.0f);
    CHECK(world.owns<Shield>(e));
    CHECK(world.get<Shield>(prefab)->value == 75.0f);
    CHECK(world.get<Shield>(e)->value == 10.0f);
    ChangeList changes;
    world.gatherChanges(changes);
    REQUIRE(changes.changes.size() == 1);
    CHECK(changes.changes[0].entity == world.entityId(e));
}

TEST_CASE("ecs prefabs: prefab children are instantiated with the prefab") {
    World world;
    registerCommon(world);
    const Entity ship = world.createPrefab("Corvette");
    world.set(ship, Health{});
    const Entity turret = world.createPrefab("Corvette.Turret");
    world.set(turret, Velocity{{0.0f, 1.0f, 0.0f}});
    CHECK(world.addPrefabChild(ship, turret).hasValue());
    CHECK(world.addPrefabChild(ship, world.spawn()).errorCode() == ErrorCode::InvalidArgument);

    const Entity inst = world.instantiate(ship);
    std::vector<Entity> kids;
    world.childrenOf(inst, kids);
    REQUIRE(kids.size() == 1);
    CHECK(world.get<Velocity>(kids[0])->value.y == 1.0f);
    // Instantiated children get their own identities (and are unregistered with the root).
    const EntityId childId = world.entityId(kids[0]);
    CHECK(childId.isValid());
    CHECK(world.find(childId) == kids[0]);
    CHECK(world.netHandle(kids[0]).isValid());
    world.destroy(inst);
    CHECK_FALSE(world.find(childId).isValid());
}
