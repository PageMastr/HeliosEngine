// WP-1.1a (ADR-004a option A) regression tests for the optimized structural paths: grouped and
// spawnN creates with batched identity bookkeeping, destroys without a hierarchy walk, and adds and
// removes without ecs_owns_id. ADR-004a §5: "A WP-1.1a change that alters [the structural log,
// identity, dirty bits and apply order] is a defect, not an optimization". So the workload below is
// digested (log, EntityIds, NetHandles, flecs ids, tables and row order, values, dirty bits, change
// lists, stats) and compared with the digest the World produced before WP-1.1a.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "flecs_internal.h"
#include "helios/core/hash.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {

struct Status { // high-churn flags: DontFragment (SPIKES.md §2.5)
    u32 flags = 0;
};
struct Mark { // Sparse storage
    u32 value = 0;
};
struct Label { // non-trivial: flecs copy/move/dtor hooks
    std::string text = "default";
};

class Digest {
public:
    explicit Digest(World& world) : m_world(world) {}

    /// Structural log (then cleared) and the change list gathered now.
    void events() {
        for (const StructuralEvent& ev : m_world.structuralLog()) {
            m_h.updateValue(static_cast<u32>(ev.op));
            m_h.updateValue(ev.entity.value);
            m_h.updateValue(ev.handle.value);
            m_h.updateValue(ev.arg);
        }
        m_h.updateValue(static_cast<u64>(m_world.structuralLog().size()));
        m_world.clearStructuralLog();
        ChangeList changes;
        m_world.gatherChanges(changes);
        m_h.updateValue(changes.entityCount);
        for (const ComponentChange& c : changes.changes) {
            m_h.updateValue(c.entity.value);
            m_h.updateValue(c.handle.value);
            m_h.updateValue(c.replIndex);
            m_h.updateValue(c.fields);
        }
    }

    /// Every World entity in query order (tables, then rows), with its flecs id, identity, type and
    /// component values (dirty masks included), plus the world's counters.
    void state() {
        ecs_world_t* fw = m_world.flecsWorld();
        collectDebugOnlyEntities();
        u64 entities = 0;
        Query all(m_world, {Term{m_world.netIdentityId(), TermAccess::Read}});
        all.forEachChunk([&](ChunkView& ch) {
            for (u32 r = 0; r < ch.count(); ++r) {
                const Entity e = ch.entity(r);
                ++entities;
                m_h.updateValue(flecsId(e.id));
                m_h.updateValue(m_world.entityId(e).value);
                m_h.updateValue(m_world.netHandle(e).value);
                m_h.updateValue(m_world.authorityGroup(e));
                const ecs_type_t* type = ecs_get_type(fw, e.id);
                for (i32 k = 0; k < type->count; ++k) m_h.updateValue(flecsId(type->array[k]));
                if (const Position* p = m_world.get<Position>(e)) {
                    m_h.updateValue(std::bit_cast<u64>(p->value.x));
                    m_h.updateValue(p->_dirty);
                }
                if (const Health* hp = m_world.get<Health>(e)) {
                    m_h.updateValue(std::bit_cast<u32>(hp->hp));
                    m_h.updateValue(hp->armor);
                    m_h.updateValue(hp->_dirty);
                }
                if (const Shield* s = m_world.get<Shield>(e)) {
                    m_h.updateValue(std::bit_cast<u32>(s->value));
                    m_h.updateValue(s->_dirty);
                }
                if (const Counter* c = m_world.get<Counter>(e)) m_h.updateValue(c->value);
                if (const Status* s = m_world.get<Status>(e)) m_h.updateValue(s->flags);
                if (const Mark* m = m_world.get<Mark>(e)) m_h.updateValue(m->value);
                if (const Label* l = m_world.get<Label>(e)) m_h.update(l->text);
                if (const RepDirty* rd = m_world.get<RepDirty>(e)) {
                    m_h.updateValue(rd->componentMask);
                    m_h.updateValue(rd->changed);
                }
                m_h.updateValue(flecsId(m_world.parentOf(e).id));
                m_h.updateValue(flecsId(m_world.frameOf(e).id));
                m_h.updateValue(flecsId(m_world.dockedTo(e).id));
            }
        });
        const WorldStats s = m_world.stats();
        m_h.updateValue(entities);
        m_h.updateValue(s.tableCount - debugOnlyTables());
        m_h.updateValue(s.entityCount);
        m_h.updateValue(s.structuralOpsApplied);
        m_h.updateValue(s.commandsDiscarded);
        m_h.updateValue(m_world.registry().handles().liveCount());
    }

    u64 value() const noexcept { return m_h.digest(); }

private:
    // Flecs built without NDEBUG (FLECS_DEBUG) gives every singleton component two
    // `debug_only_*InvariantCheck` observers while the World bootstraps: 14 entities and the 13
    // tables they were built in. So in Debug and sanitizer builds every later flecs id was 14 higher,
    // and the table count 13 higher, than in the NDEBUG builds the goldens were recorded in (the
    // WP-1.1a state, log, EntityIds and values were identical). The digest hashes flecs ids as they
    // are numbered without those entities, and the table count without those tables. There are none
    // in NDEBUG builds, so both are the identity there.
    void collectDebugOnlyEntities() {
        m_debugOnly.clear();
        m_debugOnlyParents.clear();
        ecs_world_t* fw = m_world.flecsWorld();
        const ecs_entities_t all = ecs_get_entities(fw);
        for (i32 i = 0; i < all.alive_count; ++i) {
            const char* name = ecs_get_name(fw, all.ids[i]);
            if (name && std::string_view(name).starts_with("debug_only_")) {
                m_debugOnly.push_back(static_cast<u32>(all.ids[i]));
                m_debugOnlyParents.push_back(ecs_get_parent(fw, all.ids[i]));
            }
        }
        std::sort(m_debugOnly.begin(), m_debugOnly.end());
    }

    bool isDebugOnly(ecs_entity_t e) const {
        return std::binary_search(m_debugOnly.begin(), m_debugOnly.end(), static_cast<u32>(e));
    }

    /// Tables that exist only for the debug-only observers: observer tables under one of their
    /// parents that hold nothing else (or nothing: the archetypes flecs passed them through).
    u32 debugOnlyTables() const {
        if (m_debugOnly.empty()) return 0;
        ecs_world_t* fw = m_world.flecsWorld();
        ecs_query_desc_t qd = {};
        qd.terms[0].id = EcsAny;
        qd.cache_kind = EcsQueryCacheNone; // an uncached query creates no entity
        qd.flags = EcsQueryMatchEmptyTables | EcsQueryMatchDisabled | EcsQueryMatchPrefab;
        ecs_query_t* q = ecs_query_init(fw, &qd);
        if (!q) {
            FAIL("could not create the table query");
            return 0;
        }
        u32 count = 0;
        ecs_iter_t it = ecs_query_iter(fw, q);
        while (ecs_query_next(&it)) {
            if (!ecs_table_has_id(fw, it.table, ecs_pair(ecs_id(EcsPoly), EcsObserver))) continue;
            bool underDebugParent = false;
            for (const ecs_entity_t parent : m_debugOnlyParents) {
                underDebugParent = underDebugParent || ecs_table_has_id(fw, it.table, ecs_childof(parent));
            }
            bool onlyDebugOnly = true;
            for (i32 i = 0; i < it.count; ++i) onlyDebugOnly = onlyDebugOnly && isDebugOnly(it.entities[i]);
            if (underDebugParent && onlyDebugOnly) ++count;
        }
        ecs_query_fini(q);
        return count;
    }

    u32 flecsIndex(u32 index) const {
        const auto below = std::lower_bound(m_debugOnly.begin(), m_debugOnly.end(), index);
        return index - static_cast<u32>(below - m_debugOnly.begin());
    }

    /// An entity id (generation kept), component id or pair, with its entity indices renumbered.
    u64 flecsId(u64 id) const {
        const u64 flags = id & ECS_ID_FLAGS_MASK;
        if (flags == ECS_PAIR) {
            return flags | (u64{flecsIndex(static_cast<u32>(ECS_PAIR_FIRST(id)))} << 32) |
                   flecsIndex(static_cast<u32>(ECS_PAIR_SECOND(id)));
        }
        if (flags == ECS_VALUE_PAIR) { // (relationship, value): only the relationship is an entity
            return flags | (u64{flecsIndex(static_cast<u32>(ECS_PAIR_FIRST(id)))} << 32) |
                   static_cast<u32>(ECS_PAIR_SECOND(id));
        }
        return (id & ~u64{0xFFFFFFFF}) | flecsIndex(static_cast<u32>(id));
    }

    World& m_world;
    Hasher64 m_h;
    std::vector<u32> m_debugOnly;                 ///< sorted indices of flecs' debug-only entities
    std::vector<ecs_entity_t> m_debugOnlyParents; ///< their parents
};

struct Variant {
    const char* name;
    RelationConfig relations;
    u64 golden; ///< runWorkload() digest of the pre-WP-1.1a World
};

/// A scripted workload over every structural command kind the optimizations touch. Returns the
/// digest after each step, and the final one.
u64 runWorkload(const RelationConfig& relations) {
    u64 fakeMs = 1'000'000;
    WorldDesc desc;
    desc.shard = 5;
    desc.idClock = [&fakeMs] { return fakeMs; };
    desc.handles.reservedCount = 64;
    desc.relations = relations;
    World w(desc);
    registerCommon(w);
    w.registerComponent<Status>(ComponentFlags::DontFragment);
    w.registerComponent<Mark>(ComponentFlags::Sparse);
    w.registerComponent<Label>();
    std::vector<ComponentId> tags;
    for (u32 t = 0; t < 6; ++t) {
        ComponentDesc d;
        d.name = "wp11a.tag" + std::to_string(t);
        tags.push_back(*w.registerComponent(d));
    }
    const Entity f1 = w.createFrame(FrameId(1)), f2 = w.createFrame(FrameId(2)), f3 = w.createFrame(FrameId(3));
    const Entity prefab = w.createPrefab("wp11a.Ship");
    w.set(prefab, HullSpec{900.0f, 450.0f});
    w.set(prefab, Health{450.0f, 450.0f, 3});
    const Entity turret = w.createPrefab("wp11a.Turret");
    w.set(turret, Counter{77});
    REQUIRE(w.addPrefabChild(prefab, turret).hasValue());
    const Entity host = w.spawn({.frame = f1});
    w.set(host, Counter{1});
    Digest d(w);

    // 1. Creates: groups by (frame, signature), prefab instances with children, hierarchy,
    //    content-placed ids and handle slots, sparse values. Every Set/Add follows its spawn, so
    //    the World fuses runs (CommandBuffer::trackFusion); the next buffer interleaves temps.
    CommandBuffer cb(&w);
    std::vector<TempEntity> grouped;
    for (u32 i = 0; i < 240; ++i) {
        const TempEntity t = cb.spawn({.frame = i % 2 ? f1 : f2, .ag = 100 + i});
        cb.set(t, Position{{static_cast<f64>(i), 0.0, 0.0}});
        cb.set(t, Health{static_cast<f32>(i), 500.0f, i % 4});
        cb.set(t, Counter{i * 3});
        if (i % 3 == 0) cb.add<Tagged>(t);
        if (i % 5 == 0) cb.addId(t, tags[i % 4]);
        if (i % 11 == 0) cb.set(t, Label{"label-" + std::to_string(i) + "-long-enough-to-allocate"});
        grouped.push_back(t);
    }
    std::vector<TempEntity> ships;
    for (u32 i = 0; i < 12; ++i) {
        ships.push_back(cb.spawn({.prefab = prefab, .frame = i % 2 ? f1 : f3, .ag = 7}));
        if (i % 3 == 0) cb.set(ships.back(), Health{10.0f + static_cast<f32>(i), 450.0f, 1});
    }
    for (u32 i = 0; i < 8; ++i) {
        const TempEntity child = cb.spawn({.ag = 9});
        cb.set(child, Counter{1000 + i});
        cb.setParent(child, grouped[i * 7]);
    }
    for (u32 i = 0; i < 10; ++i) {
        const TempEntity t = cb.spawn({.id = EntityId::contentPlaced(0xC0FFEE00 + i), .frame = f2,
                                       .contentHandleIndex = 1 + i * 2});
        cb.set(t, Position{{-static_cast<f64>(i), 0.0, 0.0}});
        cb.addId(t, tags[5]);
    }
    const TempEntity sparse = cb.spawn();
    cb.set(sparse, Status{3});
    cb.set(sparse, Mark{4});
    cb.set(sparse, Health{});
    const TempEntity quiet = cb.spawn({.netHandle = false});
    cb.set(quiet, Counter{3});
    w.apply(cb);
    d.events();
    d.state();

    // 1b. Interleaved temps (fused through the per-temp lists), a closed fusion, and groups that
    //     continue across other spawns.
    CommandBuffer ib(&w);
    const TempEntity a = ib.spawn(), b = ib.spawn({.frame = f3});
    ib.set(a, Counter{5});
    ib.set(b, Counter{6});
    ib.set(a, Position{{1.0, 2.0, 3.0}});
    ib.add<Frozen>(b);
    const TempEntity closed = ib.spawn();
    ib.set(closed, Counter{1});
    ib.remove<Counter>(closed);
    ib.set(closed, Counter{2});
    std::vector<TempEntity> late;
    for (u32 i = 0; i < 30; ++i) late.push_back(ib.spawn({.frame = i % 3 ? f1 : f2}));
    for (u32 i = 0; i < 30; ++i) {
        ib.set(late[i], Position{{static_cast<f64>(i) * 0.5, 1.0, 0.0}});
        ib.set(late[i], Counter{i});
        if (i % 4 == 0) ib.add<Tagged>(late[i]);
    }
    w.apply(ib);
    d.events();
    d.state();

    std::vector<Entity> es;
    for (const TempEntity t : grouped) es.push_back(cb.resolved(t));
    std::vector<Entity> shipEs;
    for (const TempEntity t : ships) shipEs.push_back(cb.resolved(t));
    const Entity sparseE = cb.resolved(sparse);
    const Entity dead = cb.resolved(quiet);
    w.destroy(dead);
    for (u32 i = 0; i < 6; ++i) REQUIRE(w.dock(shipEs[i], host).hasValue());
    d.events();

    // 2. Toggles: tags added and removed (some redundant, some repeated on one entity), a fresh
    //    replicated component (RepDirty + clean mask), a whole-value write of an owned one, sparse
    //    and DontFragment sets and removes, pairs, and a dead target.
    for (int round = 0; round < 3; ++round) {
        CommandBuffer tb(&w);
        for (usize i = 0; i < es.size(); ++i) {
            const Entity e = es[i];
            const bool even = (static_cast<usize>(round) + i) % 2 == 0;
            if (even) {
                tb.add<Frozen>(e);
            } else {
                tb.remove<Frozen>(e);
            }
            if (i % 3 == 0) tb.remove<Tagged>(e);
            if (i % 7 == 0) tb.add<Tagged>(e);
            tb.addId(e, tags[(i + static_cast<usize>(round)) % 4]);
            if (i % 4 == 1) tb.removeId(e, tags[(i + 2) % 4]);
            if (i % 9 == 0) {
                tb.add<Tagged>(e);
                tb.remove<Tagged>(e);
                tb.add<Tagged>(e);
            }
            if (i % 5 == 2) tb.set(e, Shield{static_cast<f32>(i)});
            if (i % 5 == 3 && round > 0) tb.remove<Shield>(e);
            if (i % 6 == 0) tb.set(e, Health{1.0f, 2.0f, static_cast<u32>(round)});
            if (i % 2 == 0) {
                tb.set(e, Status{static_cast<u32>(i)});
            } else if (round > 0) {
                tb.remove<Status>(e);
            }
            if (i % 8 == 0) tb.set(e, Mark{static_cast<u32>(round)});
            if (i % 8 == 4) tb.remove<Mark>(e);
            if (i % 10 == 0) tb.addId(e, World::pair(w.inFrameRelation(), f3));
        }
        if (round == 0) {
            // Shared (prefab-inherited) components: removing one that is only inherited changes
            // nothing, adding or setting one overrides it; Health is already an own copy.
            tb.remove<HullSpec>(shipEs[1]);
            tb.add<HullSpec>(shipEs[2]);
            tb.set(shipEs[3], HullSpec{1.0f, 2.0f});
            tb.add<Health>(shipEs[4]);
            tb.remove<Health>(shipEs[5]);
        }
        tb.add<Tagged>(dead);
        tb.remove<Counter>(dead);
        tb.remove<Status>(sparseE);
        tb.remove<Mark>(sparseE);
        w.apply(tb);
        d.events();
        d.state();
    }

    // 3. Destroys: leaves, parents with children, prefab instances (children cascade), docked
    //    ships, the docking host, a frame with members, and stale targets.
    CommandBuffer db(&w);
    for (usize i = 0; i < es.size(); i += 4) db.destroy(es[i]);
    for (usize i = 0; i < 4; ++i) db.destroy(shipEs[i * 2 + 1]);
    db.destroy(host);
    db.destroy(f3);
    db.destroy(dead);
    db.destroy(es[0]); // destroyed earlier in this batch
    w.apply(db);
    d.events();
    d.state();

    // 4. The immediate-mode API.
    for (usize i = 1; i < es.size(); i += 9) {
        if (!w.isAlive(es[i])) continue;
        w.add<Frozen>(es[i]);
        w.remove<Tagged>(es[i]);
        w.addId(es[i], tags[4]);
        w.removeId(es[i], tags[4]);
        w.set(es[i], Shield{3.0f});
        w.remove<Health>(es[i]);
    }
    for (usize i = 2; i < es.size(); i += 13) w.destroy(es[i]);
    w.destroy(shipEs[0]);
    d.events();
    d.state();
    return d.value();
}

TEST_CASE("ecs structural ops: the WP-1.1a paths leave the pre-WP-1.1a state, log and dirty bits") {
    // Digests of runWorkload() recorded with the World before WP-1.1a (commit f08cf5b, GCC 13 and
    // Clang 18 alike); they must not change on any toolchain, in optimized, Debug and sanitizer
    // builds alike (Digest discounts flecs' debug-only entities, which only Debug builds create).
    RelationConfig defaults;
    RelationConfig pairs;
    pairs.docking = DockStorage::Pair;
    RelationConfig fragmenting;
    fragmenting.nonFragmentingHierarchy = false;
    fragmenting.docking = DockStorage::PairDontFragment;
    RelationConfig inFrameDf;
    inFrameDf.inFrameDontFragment = true;
    const Variant variants[] = {{"defaults", defaults, 0x1f7d7fd7395e78f4ull},
                                {"docking pairs", pairs, 0xbcb2a747c872444full},
                                {"ChildOf + DontFragment docking", fragmenting, 0x2b7768115671757aull},
                                {"InFrame DontFragment", inFrameDf, 0xdec015dc027efe97ull}};
    for (const Variant& v : variants) {
        CAPTURE(std::string(v.name));
        const u64 digest = runWorkload(v.relations);
        CAPTURE(std::format("{:#018x}", digest));
        CHECK(digest == v.golden);
        CHECK(digest == runWorkload(v.relations)); // deterministic
    }
}

} // namespace
