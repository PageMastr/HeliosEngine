// Regression tests for defects found in review: sparse/DontFragment query fields, dirty masks
// clobbered or copied by whole-value writes, heap-pool ownership, dead relationship targets, prefab
// children fragmenting tables, staggered systems with a `once` function, the change-list entity
// count, and World integration of the block-id minter.

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "flecs_internal.h"
#include "helios/core/jobs.h"
#include "helios/core/thread.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {

struct SparseVal {
    u32 v = 0;
};
/// A replicated component in sparse storage.
struct SparseRep {
    f32 value = 0;
    FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&SparseRep::value);
};

u32 countChanges(World& world, Entity e, u64 replIndex, FieldMask* fields = nullptr) {
    ChangeList changes;
    world.gatherChanges(changes);
    u32 n = 0;
    for (const ComponentChange& c : changes.changes) {
        if (c.entity == world.entityId(e) && c.replIndex == replIndex) {
            ++n;
            if (fields) *fields = c.fields;
        }
    }
    return n;
}

} // namespace

TEST_CASE("ecs regressions: queries over Sparse and DontFragment components") {
    // Before the fix, ecs_field on a sparse field read a bogus shared source and crashed.
    for (const ComponentFlags flag : {ComponentFlags::Sparse, ComponentFlags::DontFragment}) {
        CAPTURE(static_cast<u32>(flag));
        World world;
        world.registerComponent<Counter>();
        world.registerComponent<SparseVal>(flag);
        world.registerComponent<Tagged>();
        std::vector<Entity> es;
        for (u32 i = 0; i < 300; ++i) {
            const Entity e = world.spawn();
            world.set(e, Counter{i});
            if (i % 3 != 0) world.set(e, SparseVal{i * 3});
            if (i % 2 == 0) world.add<Tagged>(e); // two tables
            es.push_back(e);
        }
        u32 rows = 0, bad = 0;
        Query q(world, {Term{world.id<Counter>(), TermAccess::Read}, Term{world.id<SparseVal>(), TermAccess::Read}});
        q.forEachChunk([&](ChunkView& c) {
            for (u32 r = 0; r < c.count(); ++r) {
                ++rows;
                const u64 i = c.read<Counter>(0)[r].value;
                if (c.at<SparseVal>(1, r).v != i * 3 || world.get<Counter>(c.entity(r))->value != i) ++bad;
            }
        });
        CHECK(rows == 200);
        CHECK(bad == 0);

        // Optional sparse term: presence per entity.
        u32 present = 0, absent = 0;
        Query opt(world, {Term{world.id<Counter>(), TermAccess::Read}, Term{world.id<SparseVal>(), TermAccess::OptionalRead}});
        opt.forEachChunk([&](ChunkView& c) {
            for (u32 r = 0; r < c.count(); ++r) {
                const u64 i = c.read<Counter>(0)[r].value;
                if (c.has(1)) {
                    ++present;
                    if (c.at<SparseVal>(1, r).v != i * 3) ++bad;
                } else {
                    ++absent;
                    if (i % 3 != 0) ++bad;
                }
            }
        });
        CHECK(present == 200);
        CHECK(absent == 100);
        CHECK(bad == 0);
        CHECK(q.count() == 200);
        CHECK(opt.count() == 300);
        // With / Without filters on the sparse component.
        Query with(world, {Term{world.id<Counter>(), TermAccess::Read}, Term{world.id<SparseVal>(), TermAccess::With}});
        Query without(world, {Term{world.id<Counter>(), TermAccess::Read}, Term{world.id<SparseVal>(), TermAccess::Without}});
        std::set<u64> withIds, withoutIds;
        with.forEachChunk([&](ChunkView& c) {
            for (u32 r = 0; r < c.count(); ++r) withIds.insert(c.read<Counter>(0)[r].value);
        });
        without.forEachChunk([&](ChunkView& c) {
            for (u32 r = 0; r < c.count(); ++r) withoutIds.insert(c.read<Counter>(0)[r].value);
        });
        CHECK(withIds.size() == 200);
        CHECK(withoutIds.size() == 100);
        for (const u64 i : withoutIds) CHECK(i % 3 == 0);

        // A scheduled system writing the sparse component on job workers.
        jobs::JobSystem js({.workerCount = 2});
        World sys({.jobs = &js});
        sys.registerComponent<Counter>();
        sys.registerComponent<SparseVal>(flag);
        for (u32 i = 0; i < 500; ++i) {
            const Entity e = sys.spawn();
            sys.set(e, Counter{i});
            sys.set(e, SparseVal{0});
        }
        CHECK(sys.system("WriteSparse").write<SparseVal>().read<Counter>().grain(64).each([](SystemContext&, ChunkView& c) {
            SparseVal* v = c.write<SparseVal>(0);
            const Counter* n = c.read<Counter>(1);
            for (u32 r = 0; r < c.count(); ++r) v[r].v += static_cast<u32>(n[r].value) + 1;
        }).hasValue());
        REQUIRE(sys.tick(0.05f).hasValue());
        REQUIRE(sys.tick(0.05f).hasValue());
        u64 sum = 0;
        Query all(sys, {Term{sys.id<Counter>(), TermAccess::Read}, Term{sys.id<SparseVal>(), TermAccess::Read}});
        all.forEachChunk([&](ChunkView& c) {
            for (u32 r = 0; r < c.count(); ++r) {
                if (c.at<SparseVal>(1, r).v != 2 * (c.read<Counter>(0)[r].value + 1)) ++bad;
                sum += c.at<SparseVal>(1, r).v;
            }
        });
        CHECK(bad == 0);
        CHECK(sum == 2 * (500 * 501 / 2));
    }
}

TEST_CASE("ecs regressions: dirty tracking of a sparse replicated component written by a system") {
    World world;
    world.registerComponent<Counter>();
    world.registerComponent<SparseRep>(ComponentFlags::Sparse);
    std::vector<Entity> es;
    for (u32 i = 0; i < 50; ++i) {
        const Entity e = world.spawn();
        world.set(e, Counter{i});
        world.set(e, SparseRep{});
        es.push_back(e);
    }
    CHECK(world.has<RepDirty>(es[0]));
    CHECK(world.system("Write").write<SparseRep>().read<Counter>().each([](SystemContext&, ChunkView& c) {
        auto col = c.mutColumn<SparseRep>(0);
        for (u32 r = 0; r < c.count(); ++r) {
            if (c.read<Counter>(1)[r].value % 5 == 0) col[r].set<&SparseRep::value>(1.0f);
        }
    }).hasValue());
    ChangeList changes;
    world.gatherChanges(changes);
    REQUIRE(world.tick(0.05f).hasValue());
    world.gatherChanges(changes);
    CHECK(changes.changes.size() == 10);
    CHECK(changes.entityCount == 10);
}

TEST_CASE("ecs regressions: whole-value writes keep pending dirty bits and mark every field") {
    World world;
    registerCommon(world);
    const u8 health = world.componentInfo(world.id<Health>())->replIndex;
    const Entity e = world.spawn();
    world.set(e, Health{});
    (void)countChanges(world, e, health); // drop the initial state

    // set() after a Mut write used to overwrite the pending _dirty bits with the value's (0).
    world.mut<Health>(e).set<&Health::hp>(5.0f);
    world.set(e, Health{7.0f, 100.0f, 0});
    FieldMask fields = 0;
    CHECK(countChanges(world, e, health, &fields) == 1);
    CHECK(fields == 0b111);
    CHECK(world.get<Health>(e)->hp == 7.0f);

    // A plain set() of an owned component is a raw write too (it used to be untracked).
    world.set(e, Health{8.0f, 100.0f, 0});
    CHECK(countChanges(world, e, health, &fields) == 1);
    CHECK(fields == 0b111);

    // Same through a command buffer.
    CommandBuffer cb(&world);
    cb.set(e, Health{9.0f, 100.0f, 1});
    world.apply(cb);
    CHECK(countChanges(world, e, health, &fields) == 1);
    CHECK(world.get<Health>(e)->armor == 1);
    CHECK(countChanges(world, e, health) == 0);
}

TEST_CASE("ecs regressions: copied _dirty bits never suppress the RepDirty summary") {
    World world;
    registerCommon(world);
    const u8 health = world.componentInfo(world.id<Health>())->replIndex;
    const Entity a = world.spawn();
    world.set(a, Health{});
    world.mut<Health>(a).set<&Health::hp>(50.0f);
    const Health copy = *world.get<Health>(a); // carries _dirty = 0b001
    CHECK(copy._dirty == 0b001);

    // Adding a component (set, spawn, command-buffer spawn) starts with a clean mask: the Add /
    // Create event already makes replication send the whole value.
    const Entity b = world.spawn();
    world.set(b, copy);
    CHECK(world.get<Health>(b)->_dirty == 0);
    CommandBuffer cb(&world);
    const TempEntity t = cb.spawn();
    cb.set(t, copy);
    world.apply(cb);
    const Entity c = cb.resolved(t);
    CHECK(world.get<Health>(c)->_dirty == 0);
    ChangeList changes;
    world.gatherChanges(changes);

    // Stale field bits (untracked getMut write) no longer stop Mut from setting the summary: the
    // write used to be lost because gatherChanges only visits entities whose summary bit is set.
    world.getMut<Health>(b)->_dirty = 0b001;
    world.mut<Health>(b).set<&Health::hp>(1.0f);
    CHECK(countChanges(world, b, health) == 1);

    // Prefab values do not leak their _dirty bits into instances or overrides.
    world.registerComponent<Shield>(ComponentFlags::Shared);
    const Entity prefab = world.createPrefab();
    world.set(prefab, Health{});
    world.set(prefab, Shield{});
    world.getMut<Health>(prefab)->_dirty = 0b111;
    world.getMut<Shield>(prefab)->_dirty = 0b1;
    const Entity inst = world.instantiate(prefab);
    CHECK(world.get<Health>(inst)->_dirty == 0);
    world.override<Shield>(inst);
    CHECK(world.get<Shield>(inst)->_dirty == 0);
    world.gatherChanges(changes);
    world.mut<Health>(inst).set<&Health::armor>(3u);
    FieldMask fields = 0;
    CHECK(countChanges(world, inst, health, &fields) == 1);
    CHECK(fields == 0b100);
}

TEST_CASE("ecs regressions: the change list counts only entities with reported changes") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    world.set(e, Health{});
    ChangeList changes;
    world.gatherChanges(changes);
    world.mut<Health>(e).set<&Health::hp>(1.0f);
    world.getMut<Health>(e)->_dirty = 0; // untracked path cleared the field bits again
    world.gatherChanges(changes);
    CHECK(changes.changes.empty());
    CHECK(changes.entityCount == 0);
}

TEST_CASE("ecs regressions: a pooled heap that another thread owned is treated as shared") {
    const MemoryTag tag = registerMemoryTag("EcsTest.Regression.Pool");
    // Take every pooled heap so the next one comes from the thread below.
    std::vector<std::unique_ptr<TaggedHeap>> drained;
    while (TaggedHeap::pooledHeapCount() > 0) drained.push_back(std::make_unique<TaggedHeap>(tag));
    void* native = nullptr;
    bool sharedInOwner = true;
    {
        Thread t("heap-owner", [&] {
            TaggedHeap h(tag);
            h.free(h.allocate(64));
            sharedInOwner = h.isShared(); // confined to its owner so far
            native = h.nativeHeap();
        });
    }
    CHECK_FALSE(sharedInOwner);
    TaggedHeap mine(tag);
    REQUIRE(mine.nativeHeap() == native); // recycled from the pool
    // The old owner's theap cache may still point at this heap: bulk destroy must be refused, or a
    // new heap at the same address would be served from the dead one (SPIKES.md §1.2).
    CHECK(mine.isShared());
    void* p = mine.allocate(32);
    CHECK_FALSE(mine.destroyAll());
    mine.free(p);
}

TEST_CASE("ecs regressions: dead relationship targets are refused without leaking identities") {
    World world;
    registerCommon(world);
    const Entity parent = world.spawn();
    const Entity frame = world.createFrame(FrameId(4));
    const Entity prefab = world.createPrefab();
    const Entity keep = world.createFrame(FrameId(5));
    const Entity e = world.spawn({.frame = keep});
    world.destroy(parent);
    world.destroy(frame);
    world.destroy(prefab);
    world.clearStructuralLog();
    const usize ids = world.registry().size();
    const u32 handles = world.registry().handles().liveCount();
    const u64 discarded = world.stats().commandsDiscarded;

    // Before the fix: a dead parent made flecs delete the child at once while its EntityId and
    // NetHandle stayed registered; a dead frame stored an (InFrame, stale id) pair.
    CHECK_FALSE(world.spawn({.parent = parent}).isValid());
    CHECK_FALSE(world.spawn({.frame = frame}).isValid());
    CHECK_FALSE(world.instantiate(prefab).isValid());
    CHECK(world.setFrame(e, frame).errorCode() == ErrorCode::InvalidArgument);
    CHECK(world.setFrame(e, e).errorCode() == ErrorCode::InvalidArgument);
    CHECK(world.frameOf(e) == keep);
    CHECK(world.registry().size() == ids);
    CHECK(world.registry().handles().liveCount() == handles);
    CHECK(world.structuralLog().empty());
    CHECK(world.stats().commandsDiscarded == discarded + 3);

    // Targets that die earlier in the same command batch.
    const Entity frame2 = world.createFrame(FrameId(6));
    const Entity parent2 = world.spawn();
    const EntityId parent2Id = world.entityId(parent2);
    CommandBuffer cb(&world);
    cb.destroy(frame2);
    cb.destroy(parent2);
    cb.reparent(e, frame2);
    const TempEntity grouped = cb.spawn({.frame = frame2}); // bulk-group path
    cb.set(grouped, Counter{1});
    const TempEntity child = cb.spawn({.parent = parent2}); // single-spawn path
    cb.set(child, Counter{2});
    world.apply(cb);
    CHECK_FALSE(cb.resolved(grouped).isValid());
    CHECK_FALSE(cb.resolved(child).isValid());
    CHECK(world.frameOf(e) == keep);
    CHECK(world.registry().size() == ids); // parent2 came and went, nothing leaked
    CHECK(world.stats().commandsDiscarded == discarded + 6);
    for (const StructuralEvent& ev : world.structuralLog()) {
        CHECK_FALSE((ev.op == StructuralOp::Create && ev.entity != parent2Id));
    }
}

TEST_CASE("ecs regressions: prefab children do not create a table per instance") {
    for (const bool nonFragmenting : {true, false}) {
        CAPTURE(nonFragmenting);
        WorldDesc desc;
        desc.relations.nonFragmentingHierarchy = nonFragmenting;
        World world(desc);
        registerCommon(world);
        const Entity ship = world.createPrefab("Corvette");
        world.set(ship, Health{});
        const Entity turret = world.createPrefab("Corvette.Turret");
        world.set(turret, Velocity{{0.0f, 1.0f, 0.0f}});
        const Entity barrel = world.createPrefab("Corvette.Turret.Barrel");
        world.set(barrel, Counter{7});
        CHECK(world.addPrefabChild(ship, turret).hasValue());
        CHECK(world.addPrefabChild(turret, barrel).hasValue());
        CHECK(world.addPrefabChild(barrel, ship).errorCode() == ErrorCode::InvalidArgument); // cycle
        CHECK(world.addPrefabChild(ship, ship).errorCode() == ErrorCode::InvalidArgument);

        const u32 before = world.stats().tableCount;
        std::vector<Entity> instances;
        for (int i = 0; i < 200; ++i) instances.push_back(world.instantiate(ship));
        const u32 added = world.stats().tableCount - before;
        if (nonFragmenting) {
            CHECK(added < 20);
        } else {
            CHECK(added >= 200); // (ChildOf, instance) pairs: one table per instance
        }
        std::vector<Entity> kids, grandkids;
        world.childrenOf(instances[17], kids);
        REQUIRE(kids.size() == 1);
        world.childrenOf(kids[0], grandkids);
        REQUIRE(grandkids.size() == 1);
        CHECK(world.parentOf(kids[0]) == instances[17]);
        CHECK(world.get<Velocity>(kids[0])->value.y == 1.0f);
        CHECK(world.get<Counter>(grandkids[0])->value == 7);
        CHECK(world.entityId(grandkids[0]).isValid());
        CHECK(world.netHandle(grandkids[0]).isValid());
        const usize ids = world.registry().size();
        CHECK(ids == 600);
        world.destroy(instances[17]);
        CHECK_FALSE(world.isAlive(grandkids[0]));
        CHECK(world.registry().size() == ids - 3);
    }
}

TEST_CASE("ecs regressions: staggered systems with a once function") {
    World world;
    registerCommon(world);
    for (u32 i = 0; i < 1000; ++i) world.set(world.spawn(), Counter{0});
    std::atomic<u32> onceRuns{0};
    SystemDesc d;
    d.name = "StaggeredOnce";
    d.policy = UpdatePolicy::everyNTicks(4, 0, true);
    d.terms = {Term{world.id<Counter>(), TermAccess::Write}};
    d.chunkGrain = 100;
    d.once = [&](SystemContext&) { ++onceRuns; };
    d.each = [](SystemContext&, ChunkView& c) {
        Counter* ctr = c.write<Counter>(0);
        for (u32 r = 0; r < c.count(); ++r) ctr[r].value += 1;
    };
    REQUIRE(world.addSystem(std::move(d)).hasValue());
    for (int t = 0; t < 12; ++t) REQUIRE(world.tick(0.05f).hasValue());
    CHECK(onceRuns.load() == 12);
    // Job 0's rows used to be processed every tick (12) instead of every 4th (3).
    std::set<u64> values;
    Query q(world, {Term{world.id<Counter>(), TermAccess::Read}});
    q.forEachChunk([&](ChunkView& c) {
        for (u32 r = 0; r < c.count(); ++r) values.insert(c.read<Counter>(0)[r].value);
    });
    CHECK(values == std::set<u64>{3});
}

namespace {
/// External block source that records calls (stands in for the orchestrator client).
class CountingSource final : public IdBlockSource {
public:
    LocalIdBlockSource inner{LocalIdBlockSource::Desc{.clock = [] { return u64(5000); }}};
    std::atomic<u32> calls{0};
    bool fail = false;
    Result<void> allocateIdBlocks(u32 n, std::vector<u64>& out) override {
        ++calls;
        if (fail) return Error{ErrorCode::Timeout, "control plane down"};
        return inner.allocateIdBlocks(n, out);
    }
};
} // namespace

TEST_CASE("ecs regressions: World mints block ids from its source and refills between ticks") {
    CountingSource src;
    World world({.shard = 7, .idBlocks = &src, .idClock = [] { return u64(5000); }});
    CHECK(world.localIdBlocks() == nullptr);
    const Entity a = world.spawn();
    const BlockIdParts pa = decodeBlockId(world.entityId(a));
    CHECK(pa.shard == 7);
    CHECK(pa.prefix == 4999); // first of the two blocks [4999, 5000]
    CHECK(pa.offset == 0);
    CHECK(src.calls.load() == 1);
    // Use half the current block, then a tick boundary refills.
    CommandBuffer cb(&world);
    for (u32 i = 0; i < BlockIdLayout::kBlockSize / 2; ++i) cb.spawn({.netHandle = false});
    world.apply(cb);
    world.beginTick();
    CHECK(src.calls.load() == 2);
    CHECK(world.idMinter().remaining() > 2 * BlockIdLayout::kBlockSize);

    // No block obtainable: the spawn is refused instead of registering an invalid id.
    CountingSource dead;
    dead.fail = true;
    World starved({.idBlocks = &dead});
    const u64 discarded = starved.stats().commandsDiscarded;
    CHECK_FALSE(starved.spawn().isValid());
    CHECK(starved.registry().size() == 0);
    CHECK(starved.stats().commandsDiscarded == discarded + 1);
    CHECK(starved.idMinter().stats().exhausted == 1);
    CHECK(starved.spawn({.id = EntityId::contentPlaced(9)}).isValid()); // explicit ids still work

    // A simulated clock starting at 0 (deterministic replays, tests) used to mint EntityId 0 for the
    // first spawn, which was then refused as invalid.
    World zero({.idClock = [] { return u64(0); }});
    const Entity first = zero.spawn();
    CHECK(first.isValid());
    CHECK(zero.entityId(first).isValid());
}

namespace {
struct Named2 {
    std::string text = "default";
};
} // namespace

TEST_CASE("ecs regressions: component names that resolve to existing flecs entities are refused") {
    World world;
    const Entity sol = world.createFrame(FrameId(1), "sol");
    ComponentDesc d;
    d.size = 8;
    d.alignment = 8;
    // A builtin flecs component of another size used to be returned as is (release builds), and a
    // named frame or relation used to be turned into a component.
    for (const char* taken : {"flecs.core.Identifier", "sol", "helios.InFrame", "helios.NetIdentity.nested"}) {
        CAPTURE(taken);
        d.name = taken;
        const Result<ComponentId> r = world.registerComponent(d);
        if (std::string_view(taken) == "helios.NetIdentity.nested") {
            CHECK(r.hasValue()); // a child scope of a component is a fresh name
        } else {
            CHECK(r.errorCode() == ErrorCode::AlreadyExists);
        }
    }
    CHECK(world.get<FrameRef>(sol)->frame == FrameId(1));
    // Registration keeps working afterwards, with hooks matched to the right component.
    world.registerComponent<Named2>();
    CommandBuffer cb(&world);
    const TempEntity t = cb.spawn();
    cb.set(t, Named2{std::string(64, 'x')});
    world.apply(cb);
    CHECK(world.get<Named2>(cb.resolved(t))->text == std::string(64, 'x'));
}

TEST_CASE("ecs regressions: commands with unregistered components or from another world are dropped") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    CommandBuffer cb(&world);
    cb.addId(e, 0);
    cb.setRaw(e, 0, &e, sizeof e);
    cb.removeId(e, 0);
    const TempEntity t = cb.spawn();
    cb.addId(t, 0); // fused into the spawn
    cb.set(t, Counter{3});
    const u64 discarded = world.stats().commandsDiscarded;
    world.apply(cb);
    CHECK(world.stats().commandsDiscarded == discarded + 4);
    CHECK(world.get<Counter>(cb.resolved(t))->value == 3);
    world.addId(e, 0); // direct calls ignore id 0 as well
    world.removeId(e, 0);
    CHECK(world.isAlive(e));
}

TEST_CASE("ecs regressions: destroying a frame or docking host logs the implicit relation changes") {
    for (const DockStorage storage : {DockStorage::Field, DockStorage::Pair, DockStorage::PairDontFragment}) {
        CAPTURE(static_cast<int>(storage));
        WorldDesc desc;
        desc.relations.docking = storage;
        World world(desc);
        const Entity frame = world.createFrame(FrameId(9));
        const Entity host = world.spawn({.frame = frame});
        const Entity a = world.spawn({.frame = frame});
        const Entity b = world.spawn();
        REQUIRE(world.dock(a, host).hasValue());
        REQUIRE(world.dock(b, host).hasValue());
        REQUIRE(world.dock(b, a).hasValue()); // re-docked: must not be reported for `host`
        world.clearStructuralLog();
        world.destroy(host);
        u32 undocks = 0;
        for (const StructuralEvent& ev : world.structuralLog()) {
            if (ev.op == StructuralOp::Undock) {
                ++undocks;
                CHECK(ev.entity == world.entityId(a));
            }
        }
        CHECK(undocks == 1);
        CHECK_FALSE(world.dockedTo(a).isValid());
        CHECK(world.dockedTo(b) == a);

        world.clearStructuralLog();
        world.destroy(frame);
        REQUIRE(world.structuralLog().size() == 1);
        CHECK(world.structuralLog()[0].op == StructuralOp::SetFrame);
        CHECK(world.structuralLog()[0].entity == world.entityId(a));
        CHECK(world.structuralLog()[0].arg == 0);
        CHECK_FALSE(world.frameOf(a).isValid());
    }
}

TEST_CASE("ecs regressions: grouped spawns write every member's values even if the first only added") {
    World world;
    registerCommon(world);
    world.registerComponent<Named2>(); // non-trivial: add<T>() carries no value
    CommandBuffer cb(&world);
    const TempEntity a = cb.spawn();
    cb.add<Named2>(a);
    cb.set(a, Counter{1});
    const TempEntity b = cb.spawn(); // same signature -> same bulk group, `a` is its representative
    cb.set(b, Named2{"second-member-value-long-enough-to-allocate"});
    cb.set(b, Counter{2});
    world.apply(cb);
    CHECK(world.get<Named2>(cb.resolved(a))->text == "default");
    CHECK(world.get<Named2>(cb.resolved(b))->text == "second-member-value-long-enough-to-allocate");
    CHECK(world.get<Counter>(cb.resolved(b))->value == 2);
}
