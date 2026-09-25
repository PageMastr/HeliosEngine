// Command buffers: deferred spawn/destroy/add/remove/set/relationships, temp entities, ordering,
// payload lifetimes, discarding stale targets, and the structural log.

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <vector>

#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {
std::atomic<int> g_payloads{0};
struct Label {
    std::string text;
    Label() { ++g_payloads; }
    explicit Label(std::string t) : text(std::move(t)) { ++g_payloads; }
    Label(const Label& o) : text(o.text) { ++g_payloads; }
    Label& operator=(const Label&) = default;
    ~Label() { --g_payloads; }
};
} // namespace

TEST_CASE("ecs commands: temp entities, sets and relationships resolve at apply") {
    World world;
    registerCommon(world);
    world.registerComponent<Label>();
    const Entity frame = world.createFrame(FrameId(3));
    CommandBuffer cb(&world);
    const TempEntity ship = cb.spawn();
    const TempEntity pilot = cb.spawn();
    cb.set(ship, Health{80.0f, 100.0f, 2});
    cb.set(ship, Label("Kestrel"));
    cb.add<Tagged>(ship);
    cb.setParent(pilot, ship);
    cb.reparent(ship, frame);
    CHECK(cb.size() == 7);
    CHECK(cb.spawnCount() == 2);
    CHECK_FALSE(cb.resolved(ship).isValid());

    world.clearStructuralLog();
    world.apply(cb);
    CHECK(cb.empty());
    const Entity s = cb.resolved(ship);
    const Entity p = cb.resolved(pilot);
    REQUIRE(world.isAlive(s));
    REQUIRE(world.isAlive(p));
    CHECK(world.get<Health>(s)->hp == 80.0f);
    CHECK(world.get<Label>(s)->text == "Kestrel");
    CHECK(world.has<Tagged>(s));
    CHECK(world.parentOf(p) == s);
    CHECK(world.frameOf(s) == frame);
    CHECK(world.entityId(s).isValid());
    CHECK(world.entityId(s) < world.entityId(p)); // creation order == command order

    // Sets/adds recorded with a spawn are part of its Create (replication sends full state for
    // creates), so the log holds: Create ship, Create pilot, SetParent, SetFrame.
    const auto& log = world.structuralLog();
    REQUIRE(log.size() == 4);
    CHECK(log[0].op == StructuralOp::Create);
    CHECK(log[0].entity == world.entityId(s));
    CHECK(log[1].op == StructuralOp::Create);
    CHECK(log[3].op == StructuralOp::SetFrame);
    CHECK(log[3].arg == 3);
    for (const StructuralEvent& ev : log) CHECK(ev.entity.isValid());
    bool sawParent = false;
    for (const StructuralEvent& ev : log) {
        if (ev.op == StructuralOp::SetParent) {
            sawParent = true;
            CHECK(ev.entity == world.entityId(p));
            CHECK(ev.arg == world.entityId(s).value);
        }
    }
    CHECK(sawParent);
}

TEST_CASE("ecs commands: fusion stops at the first other command on a temp entity") {
    World world;
    registerCommon(world);
    CommandBuffer cb(&world);
    const TempEntity t = cb.spawn();
    cb.set(t, Counter{1});
    cb.remove<Counter>(t); // closes fusion: the following set applies after the remove
    cb.set(t, Counter{2});
    cb.add<Tagged>(t);
    world.clearStructuralLog();
    world.apply(cb);
    const Entity e = cb.resolved(t);
    REQUIRE(world.has<Counter>(e));
    CHECK(world.get<Counter>(e)->value == 2);
    CHECK(world.has<Tagged>(e));
    // Create, Remove(Counter), Add(Counter), Add(Tagged)
    REQUIRE(world.structuralLog().size() == 4);
    CHECK(world.structuralLog()[1].op == StructuralOp::Remove);
    CHECK(world.structuralLog()[2].op == StructuralOp::Add);
}

TEST_CASE("ecs commands: fused spawns get RepDirty, sparse components and prefab values") {
    World world;
    registerCommon(world);
    struct Sparse {
        u32 v = 0;
    };
    world.registerComponent<Sparse>(ComponentFlags::DontFragment);
    const Entity prefab = world.createPrefab();
    world.set(prefab, Health{10.0f, 10.0f, 1});
    CommandBuffer cb(&world);
    const TempEntity t = cb.spawn({.prefab = prefab});
    cb.set(t, Health{5.0f, 10.0f, 1}); // wins over the prefab copy
    cb.set(t, Sparse{9});
    cb.set(t, Position{{1.0, 2.0, 3.0}});
    world.apply(cb);
    const Entity e = cb.resolved(t);
    CHECK(world.get<Health>(e)->hp == 5.0f);
    CHECK(world.get<Sparse>(e)->v == 9);
    CHECK(world.get<Position>(e)->value.z == 3.0);
    CHECK(world.has<RepDirty>(e)); // (With, RepDirty) of replicated components applies to fused spawns
    CHECK(world.isA(e, prefab));
}

TEST_CASE("ecs commands: grouped (bulk) spawns keep command-order ids, values and hooks") {
    World world;
    registerCommon(world);
    world.registerComponent<Label>();
    const Entity f1 = world.createFrame(FrameId(1));
    const Entity f2 = world.createFrame(FrameId(2));
    const int payloadsBefore = g_payloads.load();
    CommandBuffer cb(&world);
    std::vector<TempEntity> ts;
    for (int i = 0; i < 120; ++i) {
        const TempEntity t = cb.spawn({.frame = i % 2 ? f1 : f2, .ag = static_cast<AgId>(i)});
        cb.set(t, Position{{static_cast<f64>(i), 0.0, 0.0}});
        cb.set(t, Label("label-" + std::to_string(i) + "-long-enough-to-allocate"));
        if (i % 3 == 0) cb.add<Tagged>(t); // a third signature per frame
        ts.push_back(t);
    }
    world.clearStructuralLog();
    world.apply(cb);
    EntityId prev;
    for (int i = 0; i < 120; ++i) {
        const Entity e = cb.resolved(ts[static_cast<size_t>(i)]);
        REQUIRE(world.isAlive(e));
        CHECK(world.get<Position>(e)->value.x == static_cast<f64>(i));
        CHECK(world.get<Label>(e)->text == "label-" + std::to_string(i) + "-long-enough-to-allocate");
        CHECK(world.has<Tagged>(e) == (i % 3 == 0));
        CHECK(world.has<RepDirty>(e));
        CHECK(world.frameOf(e) == (i % 2 ? f1 : f2));
        CHECK(world.authorityGroup(e) == static_cast<AgId>(i));
        CHECK(world.netHandle(e).isValid());
        CHECK(world.find(world.entityId(e)) == e);
        CHECK(world.entityId(e) > prev); // EntityIds follow command order across groups
        prev = world.entityId(e);
    }
    CHECK(world.structuralLog().size() == 120);
    CHECK(g_payloads.load() - payloadsBefore == 120); // buffer copies destroyed, components live
}

TEST_CASE("ecs commands: duplicate explicit ids in one batch keep the first spawn") {
    World world;
    registerCommon(world);
    CommandBuffer cb(&world);
    const EntityId placed = EntityId::contentPlaced(4242);
    std::vector<TempEntity> ts;
    for (int i = 0; i < 6; ++i) {
        const TempEntity t = cb.spawn({.id = i == 3 ? placed : EntityId(), .netHandle = false});
        cb.set(t, Counter{static_cast<u64>(i)});
        ts.push_back(t);
    }
    const TempEntity dup = cb.spawn({.id = placed, .netHandle = false});
    cb.set(dup, Counter{99});
    const u64 discarded = world.stats().commandsDiscarded;
    world.apply(cb);
    CHECK(world.stats().commandsDiscarded == discarded + 1);
    CHECK(world.get<Counter>(world.find(placed))->value == 3);
    for (int i = 0; i < 6; ++i) CHECK(world.get<Counter>(cb.resolved(ts[static_cast<size_t>(i)]))->value == static_cast<u64>(i));
    CHECK(world.stats().entityCount == 6);
}

TEST_CASE("ecs commands: destroy, remove and stale targets") {
    World world;
    registerCommon(world);
    const Entity a = world.spawn();
    const Entity b = world.spawn();
    world.set(a, Health{});
    world.add<Tagged>(a);
    CommandBuffer cb(&world);
    cb.remove<Tagged>(a);
    cb.destroy(b);
    cb.set(b, Health{}); // same batch: b is destroyed first, so this is dropped by flecs
    world.apply(cb);
    CHECK_FALSE(world.has<Tagged>(a));
    CHECK_FALSE(world.isAlive(b));

    // Commands recorded against an entity that died before the sync point are discarded.
    CommandBuffer late(&world);
    late.set(b, Health{});
    late.add<Tagged>(b);
    const u64 discardedBefore = world.stats().commandsDiscarded;
    world.apply(late);
    CHECK(world.stats().commandsDiscarded == discardedBefore + 2);
    // A recycled flecs id with a new generation is a different entity.
    const Entity c = world.spawn();
    CHECK(c != b);
}

TEST_CASE("ecs commands: multiple buffers apply in order as one batch") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    CommandBuffer first(&world), second(&world);
    first.set(e, Counter{1});
    second.set(e, Counter{2});
    first.set(e, Counter{3});
    CommandBuffer* order[] = {&second, &first};
    world.apply(std::span<CommandBuffer* const>(order, 2));
    CHECK(world.get<Counter>(e)->value == 3); // second (2), then first (1, 3)

    CommandBuffer* reversed[] = {&first, &second};
    first.set(e, Counter{10});
    second.set(e, Counter{20});
    world.apply(std::span<CommandBuffer* const>(reversed, 2));
    CHECK(world.get<Counter>(e)->value == 20);
}

TEST_CASE("ecs commands: payloads are destroyed after apply and on clear") {
    const int before = g_payloads.load();
    {
        World world;
        world.registerComponent<Label>();
        const Entity e = world.spawn();
        CommandBuffer cb(&world);
        for (int i = 0; i < 1000; ++i) cb.set(e, Label(std::string(40, static_cast<char>('a' + i % 26))));
        CHECK(g_payloads.load() - before == 1000);
        world.apply(cb);
        CHECK(g_payloads.load() - before == 1); // only the component itself
        CHECK(world.get<Label>(e)->text == std::string(40, static_cast<char>('a' + 999 % 26)));
        cb.set(e, Label("x"));
        cb.clear();
        CHECK(g_payloads.load() - before == 1);
        // Large payloads get their own block; memory is reused across rounds.
        struct Big {
            unsigned char bytes[40000] = {};
        };
        world.registerComponent<Big>();
        Big big;
        big.bytes[39999] = 9;
        cb.set(e, big);
        world.apply(cb);
        CHECK(world.get<Big>(e)->bytes[39999] == 9);
    }
    CHECK(g_payloads.load() == before);
}

TEST_CASE("ecs commands: cycles created within one batch are refused") {
    World world;
    const Entity a = world.spawn();
    const Entity b = world.spawn();
    CommandBuffer cb(&world);
    cb.setParent(a, b);
    cb.setParent(b, a); // would close a cycle with the pending a->b edge
    const u64 discarded = world.stats().commandsDiscarded;
    world.apply(cb);
    CHECK(world.parentOf(a) == b);
    CHECK_FALSE(world.parentOf(b).isValid());
    CHECK(world.stats().commandsDiscarded == discarded + 1);
}

TEST_CASE("ecs commands: docking and undocking through buffers") {
    World world;
    const Entity host = world.spawn();
    CommandBuffer cb(&world);
    const TempEntity shuttle = cb.spawn();
    cb.dock(shuttle, host);
    world.apply(cb);
    const Entity s = cb.resolved(shuttle);
    CHECK(world.dockedTo(s) == host);
    cb.undock(s);
    world.apply(cb);
    CHECK_FALSE(world.dockedTo(s).isValid());
}

TEST_CASE("ecs commands: prefab instances spawned in a batch") {
    World world;
    registerCommon(world);
    const Entity prefab = world.createPrefab();
    world.set(prefab, HullSpec{5.0f, 6.0f});
    world.set(prefab, Health{7.0f, 7.0f, 0});
    CommandBuffer cb(&world);
    std::vector<TempEntity> temps;
    for (int i = 0; i < 100; ++i) temps.push_back(cb.spawn({.prefab = prefab}));
    world.apply(cb);
    for (const TempEntity t : temps) {
        const Entity e = cb.resolved(t);
        CHECK(world.isA(e, prefab));
        CHECK(world.get<HullSpec>(e)->mass == 5.0f);
        CHECK(world.get<Health>(e)->hp == 7.0f);
    }
}
