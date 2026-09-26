// WP-1.1a bulk structural paths (ADR-004a items 1, 3 and 4): CommandBuffer::spawnN() must leave
// exactly the state `count` spawn() + set()/add() commands leave (identities, log, tables and row
// order, values, dirty bits), EntityIdMinter::allocateN() must mint what allocate() mints, batched
// NetHandle issue must issue what one tryAllocate() per id issues, a run of destroy commands must
// end where one destroy at a time ends, the registry fast paths must behave like the checked ones,
// and Sparse/DontFragment ownership must match ecs_owns_id().

#include <doctest/doctest.h>

#include <bit>
#include <string>
#include <unordered_map>
#include <vector>

#include "flecs_internal.h"
#include "helios/core/hash.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {

struct Status { // DontFragment
    u32 flags = 0;
};
struct Mark { // Sparse
    u32 value = 0;
};
struct Label { // non-trivial: not allowed in spawnN
    std::string text = "default";
};

/// Log, identities, flecs ids, types in query (table, row) order, values and dirty bits.
u64 digestOf(World& w) {
    Hasher64 h;
    for (const StructuralEvent& ev : w.structuralLog()) {
        h.updateValue(static_cast<u32>(ev.op));
        h.updateValue(ev.entity.value);
        h.updateValue(ev.handle.value);
        h.updateValue(ev.arg);
    }
    h.updateValue(static_cast<u64>(w.structuralLog().size()));
    Query all(w, {Term{w.netIdentityId(), TermAccess::Read}});
    all.forEachChunk([&](ChunkView& ch) {
        for (u32 r = 0; r < ch.count(); ++r) {
            const Entity e = ch.entity(r);
            h.updateValue(e.id);
            h.updateValue(w.entityId(e).value);
            h.updateValue(w.netHandle(e).value);
            h.updateValue(w.authorityGroup(e));
            const ecs_type_t* type = ecs_get_type(w.flecsWorld(), e.id);
            for (i32 k = 0; k < type->count; ++k) h.updateValue(static_cast<u64>(type->array[k]));
            if (const Position* p = w.get<Position>(e)) {
                h.updateValue(std::bit_cast<u64>(p->value.x));
                h.updateValue(p->_dirty);
            }
            if (const Health* hp = w.get<Health>(e)) {
                h.updateValue(std::bit_cast<u32>(hp->hp));
                h.updateValue(hp->armor);
                h.updateValue(hp->_dirty);
            }
            if (const Counter* c = w.get<Counter>(e)) h.updateValue(c->value);
            if (const Status* s = w.get<Status>(e)) h.updateValue(s->flags);
            if (const Mark* m = w.get<Mark>(e)) h.updateValue(m->value);
            if (const RepDirty* rd = w.get<RepDirty>(e)) h.updateValue(rd->componentMask);
            h.updateValue(w.frameOf(e).id);
        }
    });
    const WorldStats s = w.stats();
    h.updateValue(s.tableCount);
    h.updateValue(s.entityCount);
    h.updateValue(s.structuralOpsApplied);
    h.updateValue(s.commandsDiscarded);
    return h.digest();
}

struct Setup {
    explicit Setup(bool inFrameDontFragment) {
        WorldDesc desc;
        desc.shard = 2;
        desc.idClock = [this] { return fakeMs; };
        desc.relations.inFrameDontFragment = inFrameDontFragment;
        world = std::make_unique<World>(desc);
        registerCommon(*world);
        world->registerComponent<Status>(ComponentFlags::DontFragment);
        world->registerComponent<Mark>(ComponentFlags::Sparse);
        world->registerComponent<Label>();
        f1 = world->createFrame(FrameId(1));
        f2 = world->createFrame(FrameId(2));
        world->spawn(); // some state before the batch
    }
    u64 fakeMs = 5'000'000;
    std::unique_ptr<World> world;
    Entity f1, f2;
};

constexpr u32 kCount = 70;

Position positionOf(u32 i) { return Position{{static_cast<f64>(i), 1.0, 2.0}, 0x5}; } // _dirty set on purpose
Health healthOf(u32 i) { return Health{static_cast<f32>(i), 200.0f, i % 3, 0xFF}; }

/// Records the same entities either as spawnN() batches or as spawn() + set()/add() commands,
/// surrounded by single spawns, a destroy and a later command on a batch entity.
void record(CommandBuffer& cb, Setup& s, bool batched, bool sparse, std::vector<TempEntity>& temps) {
    const TempEntity before = cb.spawn();
    cb.set(before, Counter{1});
    std::vector<Position> pos;
    std::vector<Health> hp;
    std::vector<Counter> ctr;
    std::vector<Status> st;
    std::vector<Mark> mk;
    for (u32 i = 0; i < kCount; ++i) {
        pos.push_back(positionOf(i));
        hp.push_back(healthOf(i));
        ctr.push_back(Counter{i * 7});
        st.push_back(Status{i});
        mk.push_back(Mark{i + 1});
    }
    for (const Entity frame : {s.f1, s.f2}) {
        const SpawnDesc desc{.frame = frame, .ag = 42};
        if (batched) {
            const TempEntity first = sparse ? cb.spawnN(desc, kCount, pos.data(), hp.data(), ctr.data(),
                                                        static_cast<const Tagged*>(nullptr), st.data(), mk.data())
                                            : cb.spawnN(desc, kCount, pos.data(), hp.data(), ctr.data(),
                                                        static_cast<const Tagged*>(nullptr));
            for (u32 i = 0; i < kCount; ++i) temps.push_back(TempEntity{first.index + i});
        } else {
            for (u32 i = 0; i < kCount; ++i) {
                const TempEntity t = cb.spawn(desc);
                cb.set(t, pos[i]);
                cb.set(t, hp[i]);
                cb.set(t, ctr[i]);
                cb.add<Tagged>(t);
                if (sparse) {
                    cb.set(t, st[i]);
                    cb.set(t, mk[i]);
                }
                temps.push_back(t);
            }
        }
    }
    const TempEntity after = cb.spawn({.frame = s.f2});
    cb.set(after, Counter{2});
    cb.setParent(temps[3], temps[0]); // later commands on batch entities apply in place
    cb.destroy(temps[5]);
}

} // namespace

TEST_CASE("ecs bulk paths: spawnN leaves the state of the same spawn() + set() commands") {
    for (const bool inFrameDf : {false, true}) {
        for (const bool sparse : {false, true}) {
            CAPTURE(inFrameDf);
            CAPTURE(sparse);
            Setup a(inFrameDf), b(inFrameDf);
            CommandBuffer ca(a.world.get()), cbuf(b.world.get());
            std::vector<TempEntity> ta, tb;
            record(ca, a, true, sparse, ta);
            record(cbuf, b, false, sparse, tb);
            CHECK(ca.spawnCount() == cbuf.spawnCount());
            CHECK(ca.size() < cbuf.size());
            a.world->apply(ca);
            b.world->apply(cbuf);
            REQUIRE(ta.size() == tb.size());
            for (usize i = 0; i < ta.size(); ++i) {
                const Entity ea = ca.resolved(ta[i]), eb = cbuf.resolved(tb[i]);
                CHECK(ea == eb);
                CHECK(a.world->entityId(ea) == b.world->entityId(eb));
                CHECK(a.world->netHandle(ea) == b.world->netHandle(eb));
            }
            const Entity e = ca.resolved(ta[10]);
            REQUIRE(a.world->isAlive(e));
            CHECK(a.world->get<Health>(e)->hp == healthOf(10).hp);
            CHECK(a.world->get<Health>(e)->_dirty == 0); // spawn values start clean
            CHECK(a.world->get<Position>(e)->_dirty == 0);
            CHECK(a.world->has<RepDirty>(e));
            CHECK(a.world->has<Tagged>(e));
            CHECK(a.world->frameOf(e) == a.f1);
            CHECK(a.world->authorityGroup(e) == 42);
            if (sparse) CHECK(a.world->get<Mark>(e)->value == 11);
            CHECK(a.world->find(a.world->entityId(e)) == e);
            CHECK_FALSE(a.world->isAlive(ca.resolved(ta[5])));
            CHECK(a.world->parentOf(ca.resolved(ta[3])) == ca.resolved(ta[0]));
            CHECK(digestOf(*a.world) == digestOf(*b.world));
        }
    }
}

TEST_CASE("ecs bulk paths: spawnN refuses what it does not support") {
    Setup s(false);
    World& w = *s.world;
    const Entity prefab = w.createPrefab();
    const Entity doomed = w.createFrame(FrameId(9));
    const std::vector<Counter> values(4, Counter{3});
    const std::vector<Label> labels(4);
    const ComponentId labelId = w.id<Label>();
    CommandBuffer cb(&w);
    const u32 entitiesBefore = w.stats().entityCount;
    const u64 discardedBefore = w.stats().commandsDiscarded;
    (void)cb.spawnN({.prefab = prefab}, 4, values.data());
    (void)cb.spawnN({.parent = s.f1}, 4, values.data());
    (void)cb.spawnN({.id = EntityId::contentPlaced(7)}, 4, values.data());
    (void)cb.spawnN({.contentHandleIndex = 1}, 4, values.data());
    const SpawnColumn nonTrivial[] = {{labelId, labels.data(), static_cast<u32>(sizeof(Label))}};
    (void)cb.spawnN({}, 4, nonTrivial);
    const SpawnColumn wrongSize[] = {{w.id<Counter>(), values.data(), 4}};
    (void)cb.spawnN({}, 4, wrongSize);
    const SpawnColumn foreign[] = {{World::pair(w.inFrameRelation(), s.f1), nullptr, 0}};
    (void)cb.spawnN({}, 4, foreign);
    (void)cb.spawnN({.frame = doomed}, 4, values.data());
    CHECK_FALSE(cb.spawnN({}, 0, values.data()).isValid());
    w.destroy(doomed); // the last batch's frame dies before the sync point
    w.apply(cb);
    CHECK(w.stats().entityCount == entitiesBefore);
    CHECK(w.stats().commandsDiscarded == discardedBefore + 8 * 4);

    // Components with hooks are fine without values (flecs constructs them).
    const SpawnColumn constructed[] = {{labelId, nullptr, 0}, {w.id<Counter>(), values.data(), 8}};
    const TempEntity t = cb.spawnN({}, 4, constructed);
    w.apply(cb);
    CHECK(w.get<Label>(cb.resolved(TempEntity{t.index + 3}))->text == "default");
    CHECK(w.get<Counter>(cb.resolved(TempEntity{t.index + 3}))->value == 3);

    // A Set or Add on a batch entity recorded after the batch applies in place (logged as Add).
    const TempEntity first = cb.spawnN({}, 3, values.data());
    cb.set(TempEntity{first.index + 1}, Shield{4.0f});
    cb.add<Tagged>(TempEntity{first.index + 2});
    w.clearStructuralLog();
    w.apply(cb);
    const auto& log = w.structuralLog();
    REQUIRE(log.size() == 5);
    CHECK(log[3].op == StructuralOp::Add);
    CHECK(log[3].arg == w.id<Shield>());
    CHECK(log[3].entity == w.entityId(cb.resolved(TempEntity{first.index + 1})));
    CHECK(log[4].op == StructuralOp::Add);
    CHECK(w.get<Shield>(cb.resolved(TempEntity{first.index + 1}))->_dirty == 0);
}

TEST_CASE("ecs bulk paths: allocateN mints what allocate() mints, across block boundaries") {
    u64 fakeMs = 1'000'000;
    LocalIdBlockSource sa({.clock = [&] { return fakeMs; }}), sb({.clock = [&] { return fakeMs; }});
    EntityIdMinter a({.shard = 3, .source = &sa, .clock = [&] { return fakeMs; }});
    EntityIdMinter b({.shard = 3, .source = &sb, .clock = [&] { return fakeMs; }});
    std::vector<EntityId> batch;
    for (const u32 n : {1u, 100u, 131'000u, 200u, 0u, 5000u}) {
        batch.assign(n, EntityId());
        a.allocateN(batch);
        for (u32 i = 0; i < n; ++i) {
            const EntityId one = b.allocate();
            if (batch[i] != one) {
                FAIL("allocateN diverged at " << i << " of " << n);
                break;
            }
        }
    }
    CHECK(a.stats().allocated == b.stats().allocated);
    CHECK(a.remaining() == b.remaining());
}

TEST_CASE("ecs bulk paths: the paged registry matches a hash map under churn") {
    // Runtime ids in runs (paged), runtime ids scattered over many blocks (one page each), and
    // content-placed ids (hashed); pages empty out and are recycled.
    EntityRegistry reg;
    std::unordered_map<u64, u64> ref;
    u64 rng = 0x1234;
    auto next = [&] { return rng = mix64(rng + 0x9E3779B97F4A7C15ull); };
    std::vector<EntityId> ids;
    for (u32 i = 0; i < 3000; ++i) ids.push_back(composeBlockId(1000 + i / 1500, 3, i % 1500 + 1));
    for (u32 i = 0; i < 300; ++i) ids.push_back(composeBlockId(5000 + next() % 100000, next() % 32, next() % 131072));
    for (u32 i = 0; i < 300; ++i) ids.push_back(EntityId::contentPlaced(next()));
    u64 serial = 1;
    for (int round = 0; round < 40; ++round) {
        for (const EntityId id : ids) {
            const bool present = ref.count(id.value) != 0;
            if (next() % 3 == 0) {
                CHECK(reg.remove(id, NetHandle()) == present);
                ref.erase(id.value);
            } else if (!present && next() % 2 == 0) {
                const Entity e(serial++);
                CHECK(reg.addNew(id, e));
                ref.emplace(id.value, e.id);
            } else if (present) {
                CHECK_FALSE(reg.addNew(id, Entity(serial++)));
            }
        }
        REQUIRE(reg.size() == ref.size());
        for (const EntityId id : ids) {
            const auto it = ref.find(id.value);
            CHECK(reg.find(id).id == (it == ref.end() ? 0 : it->second));
            CHECK(reg.contains(id) == (it != ref.end()));
        }
    }
    for (const EntityId id : ids) (void)reg.remove(id, NetHandle());
    CHECK(reg.size() == 0);
    CHECK_FALSE(reg.find(ids.front()).isValid());
    CHECK(reg.memoryBytes() > 0);
}

TEST_CASE("ecs bulk paths: registry fast paths match the checked ones") {
    EntityRegistry reg;
    const EntityId id(0x1234);
    CHECK(reg.addNew(id, Entity(55)));
    CHECK_FALSE(reg.addNew(id, Entity(56))); // taken: nothing changes
    CHECK_FALSE(reg.addNew(EntityId(), Entity(57)));
    CHECK_FALSE(reg.addNew(EntityId(0x99), Entity()));
    CHECK(reg.find(id) == Entity(55));
    const NetHandle h = reg.tryAssignHandle(id, Entity(55));
    REQUIRE(h.isValid());
    CHECK(reg.find(h) == Entity(55));
    CHECK(reg.resolve(h) == id);
    CHECK_FALSE(reg.remove(EntityId(0x777), h)); // unknown id: the handle stays live
    CHECK(reg.find(h) == Entity(55));
    CHECK(reg.remove(id, h));
    CHECK_FALSE(reg.find(h).isValid());
    CHECK(reg.handles().liveCount() == 0);

    NetHandleTable table(NetHandleTable::Desc{.maxHandles = 2});
    const NetHandle x = table.tryAllocate(EntityId(1));
    const NetHandle y = table.tryAllocate(EntityId(2));
    CHECK(x.isValid());
    CHECK(y.isValid());
    CHECK_FALSE(table.tryAllocate(EntityId(3)).isValid()); // full
    CHECK_FALSE(table.allocate(EntityId(3)).hasValue());
    CHECK_FALSE(table.releaseIssuedTo(x, EntityId(2))); // issued to another id
    CHECK(table.releaseIssuedTo(x, EntityId(1)));
    CHECK_FALSE(table.releaseIssuedTo(x, EntityId(1))); // stale now
    CHECK(table.release(y));
    CHECK(table.liveCount() == 0);

    U64Map map;
    CHECK(map.insertNew(5, 50));
    CHECK_FALSE(map.insertNew(5, 51));
    CHECK(map.find(5) == 50);
}

TEST_CASE("ecs bulk paths: Set and Add after their spawn fuse; scattered ones take the full pass") {
    // Both recordings must give the same result; the second interleaves temps.
    auto run = [](bool scattered) {
        World w({.idClock = [] { return u64{1'000'000}; }}); // the same id block in both worlds
        registerCommon(w);
        CommandBuffer cb(&w);
        std::vector<TempEntity> ts;
        for (u32 i = 0; i < 20; ++i) {
            ts.push_back(cb.spawn());
            if (!scattered) {
                cb.set(ts.back(), Counter{i});
                cb.add<Tagged>(ts.back());
            }
        }
        if (scattered) {
            for (u32 i = 0; i < 20; ++i) {
                cb.set(ts[i], Counter{i});
                cb.add<Tagged>(ts[i]);
            }
        }
        cb.setParent(ts[1], ts[0]); // closes ts[1]'s fusion
        cb.remove<Tagged>(ts[1]);   // applies in place
        w.apply(cb);
        const u64 d = digestOf(w);
        CHECK(w.get<Counter>(cb.resolved(ts[7]))->value == 7);
        CHECK(w.has<Tagged>(cb.resolved(ts[7])));
        CHECK_FALSE(w.has<Tagged>(cb.resolved(ts[1])));
        CHECK(w.parentOf(cb.resolved(ts[1])) == cb.resolved(ts[0]));
        return d;
    };
    const u64 contiguous = run(false);
    CHECK(contiguous == run(true));
}

TEST_CASE("ecs bulk paths: destroying docking hosts logs undocks across host-filter rebuilds") {
    // DockStorage::Field keeps a host filter so that destroys skip the reverse-index lookup; it is
    // rebuilt every 1,024 host removals and must never hide a host.
    World w;
    std::vector<Entity> hosts, ships;
    for (u32 i = 0; i < 1100; ++i) {
        hosts.push_back(w.spawn());
        ships.push_back(w.spawn());
        REQUIRE(w.dock(ships.back(), hosts.back()).hasValue());
    }
    for (u32 i = 0; i < 1100; ++i) {
        w.clearStructuralLog();
        w.destroy(hosts[i]);
        REQUIRE(w.structuralLog().size() == 2);
        CHECK(w.structuralLog()[0].op == StructuralOp::Undock);
        CHECK(w.structuralLog()[0].entity == w.entityId(ships[i]));
        CHECK(w.structuralLog()[1].op == StructuralOp::Destroy);
        CHECK_FALSE(w.dockedTo(ships[i]).isValid());
        if (i % 100 == 0) { // a new host after a rebuild is found too
            const Entity h = w.spawn();
            const Entity s = w.spawn();
            REQUIRE(w.dock(s, h).hasValue());
            w.clearStructuralLog();
            w.destroy(h);
            CHECK(w.structuralLog().size() == 2);
        }
    }
}

TEST_CASE("ecs bulk paths: batched handle issue matches one tryAllocate() per id") {
    // Twin tables with the same history: FIFO slots (reuse delay), fresh slots, the ring compaction
    // after thousands of pops, invalid ids and a full table.
    const NetHandleTable::Desc desc{.maxHandles = 9000, .reservedCount = 10, .reuseDelay = 16};
    NetHandleTable bulk(desc), single(desc);
    std::vector<NetHandle> live;
    u64 serial = 1, rng = 7;
    auto next = [&] { return rng = mix64(rng + 0x9E3779B97F4A7C15ull); };
    std::vector<EntityId> ids;
    std::vector<u64> owners;
    std::vector<NetHandle> out;
    for (int round = 0; round < 60; ++round) {
        const usize n = next() % 400;
        ids.clear();
        owners.clear();
        for (usize i = 0; i < n; ++i) {
            ids.push_back(next() % 97 == 0 ? EntityId() : EntityId(serial++)); // now and then invalid
            owners.push_back(next());
        }
        out.assign(n, NetHandle());
        bulk.tryAllocateN(ids, owners.data(), out.data());
        for (usize i = 0; i < n; ++i) {
            const NetHandle h = single.tryAllocate(ids[i], owners[i]);
            REQUIRE(out[i] == h);
            CHECK(bulk.resolve(h) == ids[i]);
            CHECK(bulk.ownerOf(h) == (h.isValid() ? owners[i] : 0));
            if (h.isValid()) live.push_back(h);
        }
        // Release a scattered part (issue order stays a function of the call sequence).
        for (usize i = 0; i < live.size();) {
            if (next() % 3 == 0) {
                CHECK(bulk.release(live[i]));
                CHECK(single.release(live[i]));
                live[i] = live.back();
                live.pop_back();
            } else {
                ++i;
            }
        }
        REQUIRE(bulk.liveCount() == single.liveCount());
    }
    // Fill the table: the rest of a batch gets invalid handles, as tryAllocate() gives.
    ids.assign(desc.maxHandles, EntityId());
    owners.assign(ids.size(), 1);
    for (EntityId& id : ids) id = EntityId(serial++);
    out.assign(ids.size(), NetHandle());
    bulk.tryAllocateN(ids, owners.data(), out.data());
    for (usize i = 0; i < ids.size(); ++i) REQUIRE(out[i] == single.tryAllocate(ids[i], 1));
    CHECK_FALSE(out.back().isValid());
    CHECK(bulk.liveCount() == desc.maxHandles - desc.reservedCount);
    CHECK(bulk.memoryBytes() > 0);
}

TEST_CASE("ecs bulk paths: a NetHandle slot keeps its id and owner until released") {
    NetHandleTable table({.maxHandles = 8, .reservedCount = 2, .reuseDelay = 0});
    CHECK_FALSE(table.tryAllocate(EntityId()).isValid());
    CHECK(table.allocate(EntityId()).errorCode() == ErrorCode::InvalidArgument);
    CHECK(table.allocateAt(1, EntityId()).errorCode() == ErrorCode::InvalidArgument);
    const NetHandle a = table.tryAllocate(EntityId(11), 111);
    const NetHandle c = *table.allocateAt(2, EntityId(12), 222);
    CHECK(table.ownerOf(a) == 111);
    CHECK(table.ownerOf(c) == 222);
    CHECK(table.handleAt(a.index()) == a);
    CHECK(table.release(a));
    CHECK(table.ownerOf(a) == 0);
    CHECK_FALSE(table.handleAt(a.index()).isValid());
    const NetHandle again = table.tryAllocate(EntityId(13), 333); // reuse delay 0: the same slot
    CHECK(again.index() == a.index());
    CHECK(table.ownerOf(a) == 0); // the stale handle stays stale
    CHECK(table.ownerOf(again) == 333);
    CHECK(table.resolve(again) == EntityId(13));

    EntityRegistry reg;
    CHECK(reg.addNew(EntityId(0x40), Entity(77)));
    const NetHandle h = reg.tryAssignHandle(EntityId(0x40), Entity(77));
    CHECK(reg.find(h) == Entity(77));
    CHECK(reg.remove(EntityId(0x40), h));
    CHECK_FALSE(reg.find(h).isValid());
}

TEST_CASE("ecs bulk paths: a run of destroy commands matches one destroy at a time") {
    // Plain entities, a parent with children, a DockRef host, a frame with members and an entity
    // made by flecs directly (no identity), destroyed in one buffer with repeats and dead targets.
    auto build = [](World& w, std::vector<Entity>& order) {
        registerCommon(w);
        std::vector<Entity> plain;
        for (u32 i = 0; i < 60; ++i) {
            plain.push_back(w.spawn());
            w.set(plain.back(), Counter{i});
        }
        const Entity parent = w.spawn(), child1 = w.spawn(), child2 = w.spawn();
        REQUIRE(w.setParent(child1, parent).hasValue());
        REQUIRE(w.setParent(child2, parent).hasValue());
        const Entity host = w.spawn(), ship = w.spawn();
        REQUIRE(w.dock(ship, host).hasValue());
        const Entity frame = w.createFrame(FrameId(4));
        const Entity member = w.spawn();
        REQUIRE(w.setFrame(member, frame).hasValue());
        const Entity foreign = helios::ecs::detail::he(ecs_new(w.flecsWorld()));
        const Entity dead = w.spawn();
        w.destroy(dead);
        auto range = [&](u32 a, u32 b) {
            for (u32 i = a; i < b; ++i) order.push_back(plain[i]);
        };
        range(0, 10);
        order.push_back(parent);
        range(10, 20);
        order.push_back(plain[5]); // repeat
        order.push_back(dead);
        order.push_back(host);
        range(20, 30);
        order.push_back(frame);
        order.push_back(foreign);
        range(30, 40);
        order.push_back(child1); // died with its parent
        order.push_back(ship);
        order.push_back(plain[39]); // repeat at the end of a run
        range(40, 45);
        w.clearStructuralLog();
    };
    World a, b;
    std::vector<Entity> orderA, orderB;
    build(a, orderA);
    build(b, orderB);
    REQUIRE(orderA == orderB);
    CommandBuffer cb(&a);
    for (const Entity e : orderA) cb.destroy(e);
    const u64 discardedBefore = a.stats().commandsDiscarded;
    const u64 opsBefore = a.stats().structuralOpsApplied;
    a.apply(cb);
    u64 destroyed = 0;
    for (const Entity e : orderB) {
        if (!b.isAlive(e)) continue;
        b.destroy(e);
        ++destroyed;
    }
    CHECK(a.structuralLog() == b.structuralLog());
    CHECK(a.stats().commandsDiscarded - discardedBefore == orderA.size() - destroyed); // repeats, dead
    CHECK(a.stats().structuralOpsApplied - opsBefore == destroyed);
    CHECK(a.stats().entityCount == b.stats().entityCount);
    CHECK(a.registry().handles().liveCount() == b.registry().handles().liveCount());
    for (usize i = 0; i < orderA.size(); ++i) CHECK(a.isAlive(orderA[i]) == b.isAlive(orderB[i]));
    // Handles issued next come out of the same FIFO (releases kept their order).
    for (u32 i = 0; i < 5; ++i) CHECK(a.netHandle(a.spawn()) == b.netHandle(b.spawn()));
}

TEST_CASE("ecs bulk paths: Sparse and DontFragment ownership comes from the sparse set") {
    // Every Add/Remove is logged exactly when ownership changes, as ecs_owns_id() reports it, for
    // DontFragment and Sparse components. (Not DontFragment tags: in flecs 4.1.6 a first
    // ecs_remove_id() of such a tag from an entity that lacks it caches a remove edge without the
    // tag, and later removes from that table do nothing; SPIKES.md §5.)
    World w;
    registerCommon(w);
    const ComponentId status = w.registerComponent<Status>(ComponentFlags::DontFragment);
    const ComponentId mark = w.registerComponent<Mark>(ComponentFlags::Sparse);
    const Entity e = w.spawn();
    const Entity other = w.spawn();
    w.set(other, Status{9}); // another entity owns the component too
    ecs_world_t* fw = w.flecsWorld();
    struct Step {
        CommandKind kind;
        ComponentId id;
    };
    for (const ComponentId cid : {status, mark}) {
        CAPTURE(cid);
        const std::vector<Step> steps = {{CommandKind::Set, cid},    {CommandKind::Set, cid},
                                         {CommandKind::Remove, cid}, {CommandKind::Remove, cid},
                                         {CommandKind::Add, cid},    {CommandKind::Add, cid},
                                         {CommandKind::Set, cid},    {CommandKind::Remove, cid}};
        for (usize si = 0; si < steps.size(); ++si) {
            const Step& step = steps[si];
            CAPTURE(si);
            const bool ownedBefore = ecs_owns_id(fw, e.id, cid);
            CommandBuffer cb(&w);
            const u32 v = 5;
            if (step.kind == CommandKind::Set) cb.setRaw(e, cid, &v, 4);
            if (step.kind == CommandKind::Add) cb.addId(e, cid);
            if (step.kind == CommandKind::Remove) cb.removeId(e, cid);
            w.clearStructuralLog();
            w.apply(cb);
            const bool ownedAfter = ecs_owns_id(fw, e.id, cid);
            CHECK(ownedAfter == (step.kind != CommandKind::Remove));
            if (ownedBefore == ownedAfter) {
                CHECK(w.structuralLog().empty());
            } else {
                REQUIRE(w.structuralLog().size() == 1);
                const StructuralEvent& ev = w.structuralLog().front();
                CHECK(ev.op == (ownedAfter ? StructuralOp::Add : StructuralOp::Remove));
                CHECK(ev.entity == w.entityId(e));
                CHECK(ev.handle == w.netHandle(e));
                CHECK(ev.arg == cid);
            }
            if (ownedAfter && step.kind == CommandKind::Set) {
                CHECK(*static_cast<const u32*>(w.getRaw(e, cid)) == 5);
            }
        }
    }
    CHECK(w.get<Status>(other)->flags == 9);
}
