// Deterministic scheduler: stable order independent of registration order, dependency and conflict
// edges, identical results for 0/1/2/4 workers, update policies, stats and access checks.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <string>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/jobs.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {

void addNamedSystems(World& world, const std::vector<std::string>& names, Stage stage = Stage::PrePhysics) {
    for (const std::string& n : names) {
        REQUIRE(world.system(n).stage(stage).once([](SystemContext&) {}).hasValue());
    }
}

} // namespace

TEST_CASE("ecs scheduler: order is independent of registration order") {
    auto orderFor = [](std::vector<std::string> names) {
        World world;
        addNamedSystems(world, names);
        CHECK(world.system("Early").stage(Stage::PrePhysics).order(-5).once([](SystemContext&) {}).hasValue());
        return world.executionOrder(Stage::PrePhysics);
    };
    const std::vector<std::string> a = orderFor({"Movement", "AI", "Weapons", "Cargo"});
    const std::vector<std::string> b = orderFor({"Cargo", "Weapons", "AI", "Movement"});
    const std::vector<std::string> expected{"Early", "AI", "Cargo", "Movement", "Weapons"};
    CHECK(a == expected);
    CHECK(b == expected);
}

TEST_CASE("ecs scheduler: explicit dependencies and their errors") {
    World world;
    CHECK(world.system("B").stage(Stage::PrePhysics).after("C").once([](SystemContext&) {}).hasValue());
    CHECK(world.system("A").stage(Stage::PrePhysics).once([](SystemContext&) {}).hasValue());
    CHECK(world.system("C").stage(Stage::PrePhysics).once([](SystemContext&) {}).hasValue());
    CHECK(world.executionOrder(Stage::PrePhysics) == std::vector<std::string>{"A", "C", "B"});
    const auto edges = world.scheduleEdges(Stage::PrePhysics);
    CHECK(std::find(edges.begin(), edges.end(), std::pair<std::string, std::string>{"C", "B"}) != edges.end());

    World cyclic;
    CHECK(cyclic.system("X").after("Y").once([](SystemContext&) {}).hasValue());
    CHECK(cyclic.system("Y").after("X").once([](SystemContext&) {}).hasValue());
    CHECK(cyclic.buildSchedule().errorCode() == ErrorCode::InvalidState);

    World unknown;
    CHECK(unknown.system("X").after("Nope").once([](SystemContext&) {}).hasValue());
    CHECK(unknown.buildSchedule().errorCode() == ErrorCode::NotFound);

    World later;
    CHECK(later.system("Early").stage(Stage::Input).after("Late").once([](SystemContext&) {}).hasValue());
    CHECK(later.system("Late").stage(Stage::Send).once([](SystemContext&) {}).hasValue());
    CHECK(later.buildSchedule().errorCode() == ErrorCode::InvalidArgument);

    World dup;
    CHECK(dup.system("S").once([](SystemContext&) {}).hasValue());
    CHECK(dup.system("S").once([](SystemContext&) {}).errorCode() == ErrorCode::AlreadyExists);
    CHECK(dup.system("NoFn").commit().errorCode() == ErrorCode::InvalidArgument);
    CHECK(dup.system("NoTerms").each([](SystemContext&, ChunkView&) {}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(dup.system("Unregistered").read<Health>().each([](SystemContext&, ChunkView&) {}).errorCode() ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("ecs scheduler: conflict edges come from declared access") {
    World world;
    registerCommon(world);
    auto noop = [](SystemContext&, ChunkView&) {};
    CHECK(world.system("1-WritePos").write<Position>().each(noop).hasValue());
    CHECK(world.system("2-ReadPos").read<Position>().each(noop).hasValue());
    CHECK(world.system("3-ReadPosToo").read<Position>().read<Velocity>().each(noop).hasValue());
    CHECK(world.system("4-WriteVel").write<Velocity>().each(noop).hasValue());
    CHECK(world.system("5-Health").write<Health>().without<Frozen>().each(noop).hasValue());
    CHECK(world.system("6-Exclusive").exclusive().once([](SystemContext&) {}).hasValue());
    const auto edges = world.scheduleEdges(Stage::PrePhysics);
    auto has = [&](const char* a, const char* b) {
        return std::find(edges.begin(), edges.end(), std::pair<std::string, std::string>{a, b}) != edges.end();
    };
    CHECK(has("1-WritePos", "2-ReadPos"));
    CHECK(has("1-WritePos", "3-ReadPosToo"));
    CHECK_FALSE(has("2-ReadPos", "3-ReadPosToo")); // readers run in parallel
    CHECK(has("3-ReadPosToo", "4-WriteVel"));
    CHECK_FALSE(has("1-WritePos", "4-WriteVel"));
    CHECK_FALSE(has("1-WritePos", "5-Health"));
    CHECK(has("5-Health", "6-Exclusive"));
    CHECK(has("1-WritePos", "6-Exclusive"));
}

namespace {

/// A small deterministic simulation touching every scheduler feature: parallel chunk writes,
/// conflicting systems, random-access reads, spawns/destroys through command buffers, dirty marks.
u64 simulate(u32 workers, u32 ticks, std::vector<std::string>* order = nullptr) {
    std::unique_ptr<jobs::JobSystem> js;
    if (workers > 0) js = std::make_unique<jobs::JobSystem>(jobs::JobSystemDesc{.workerCount = workers});
    u64 fakeMs = 0;
    World world({.jobs = js.get(), .idClock = [&fakeMs] { return fakeMs; }});
    registerCommon(world);
    for (u32 i = 0; i < 3000; ++i) {
        const Entity e = world.spawn();
        world.set(e, Position{{static_cast<f64>(i), 0.0, 0.0}});
        world.set(e, Velocity{{1.0f + static_cast<f32>(i % 7), 0.5f, 0.0f}});
        world.set(e, Health{});
        world.set(e, Counter{i});
        if (i % 11 == 0) world.add<Frozen>(e);
    }
    CHECK(world.system("Integrate")
        .stage(Stage::Physics)
        .write<Position>()
        .read<Velocity>()
        .without<Frozen>()
        .grain(97)
        .each([](SystemContext& ctx, ChunkView& c) {
            auto pos = c.mutColumn<Position>(0);
            const Velocity* vel = c.read<Velocity>(1);
            for (u32 r = 0; r < c.count(); ++r) {
                Position& p = pos[r].raw();
                p.value.x += static_cast<f64>(vel[r].value.x) * ctx.dt();
                p.value.y += static_cast<f64>(vel[r].value.y) * ctx.dt();
            }
        }).hasValue());
    CHECK(world.system("Aging")
        .stage(Stage::PostPhysics)
        .write<Counter>()
        .read<Position>()
        .grain(128)
        .each([](SystemContext& ctx, ChunkView& c) {
            Counter* counters = c.write<Counter>(0);
            const Position* pos = c.read<Position>(1);
            for (u32 r = 0; r < c.count(); ++r) {
                counters[r].value = counters[r].value * 31 + static_cast<u64>(pos[r].value.x * 16.0);
                // Churn: every entity with a matching hash respawns a child and dies.
                if ((counters[r].value + ctx.tick()) % 97 == 0) {
                    const TempEntity t = ctx.commands().spawn();
                    ctx.commands().set(t, Position{{pos[r].value.x, 1.0, 0.0}});
                    ctx.commands().set(t, Velocity{{2.0f, 0.0f, 0.0f}});
                    ctx.commands().set(t, Health{});
                    ctx.commands().set(t, Counter{counters[r].value / 3});
                    ctx.commands().destroy(c.entity(r));
                }
            }
        }).hasValue());
    CHECK(world.system("Damage")
        .stage(Stage::PostPhysics)
        .write<Health>()
        .read<Counter>()
        .alsoReads<Position>()
        .grain(200)
        .each([](SystemContext& ctx, ChunkView& c) {
            auto hp = c.mutColumn<Health>(0);
            for (u32 r = 0; r < c.count(); ++r) {
                const Position* self = ctx.get<Position>(c.entity(r));
                const f32 dmg = static_cast<f32>(static_cast<u64>(self->value.x) % 5);
                if (dmg > 3.0f) hp[r].set<&Health::hp>(hp[r]->hp - dmg);
            }
        }).hasValue());
    CHECK(world.system("Census").stage(Stage::ReplicationGather).singleJob().read<Health>().each(
        [](SystemContext& ctx, ChunkView& c) {
            if (c.count() > 0 && ctx.tick() % 5 == 0) ctx.commands().add<Tagged>(c.entity(0));
        }).hasValue());
    if (order) *order = world.executionOrder(Stage::PostPhysics);

    Hasher64 hasher;
    ChangeList changes;
    for (u32 t = 0; t < ticks; ++t) {
        fakeMs += 50;
        REQUIRE(world.tick(0.05f).hasValue());
        world.gatherChanges(changes);
        for (const ComponentChange& ch : changes.changes) {
            hasher.updateValue(ch.entity.value);
            hasher.updateValue(ch.fields);
        }
        for (const StructuralEvent& ev : world.structuralLog()) {
            hasher.updateValue(static_cast<u32>(ev.op));
            hasher.updateValue(ev.entity.value);
        }
        world.clearStructuralLog();
    }
    // Final state in EntityId order.
    std::vector<std::pair<u64, Entity>> ids;
    Query all(world, {Term{world.id<Position>(), TermAccess::Read}});
    all.forEachChunk([&](ChunkView& c) {
        for (u32 r = 0; r < c.count(); ++r) ids.emplace_back(world.entityId(c.entity(r)).value, c.entity(r));
    });
    std::sort(ids.begin(), ids.end());
    for (const auto& [id, e] : ids) {
        hasher.updateValue(id);
        hasher.updateValue(std::bit_cast<u64>(world.get<Position>(e)->value.x));
        hasher.updateValue(std::bit_cast<u32>(world.get<Health>(e)->hp));
        hasher.updateValue(world.get<Counter>(e)->value);
        hasher.updateValue(static_cast<u32>(world.has<Tagged>(e)));
    }
    hasher.updateValue(static_cast<u64>(ids.size()));
    return hasher.digest();
}

} // namespace

TEST_CASE("ecs scheduler: results are bit-identical for 0, 1, 2 and 4 workers") {
    std::vector<std::string> order;
    const u64 reference = simulate(0, 40, &order);
    CHECK(order == std::vector<std::string>{"Aging", "Damage"});
    for (const u32 workers : {1u, 2u, 4u}) {
        for (int run = 0; run < 2; ++run) {
            CAPTURE(workers);
            CHECK(simulate(workers, 40) == reference);
        }
    }
}

TEST_CASE("ecs scheduler: update policies") {
    World world;
    registerCommon(world);
    for (u32 i = 0; i < 1000; ++i) world.set(world.spawn(), Counter{0});
    std::atomic<u32> everyThird{0};
    CHECK(world.system("EveryThird").policy(UpdatePolicy::everyNTicks(3, 1)).once([&](SystemContext&) { ++everyThird; }).hasValue());
    CHECK(world.system("Staggered")
        .policy(UpdatePolicy::everyNTicks(4, 0, true))
        .write<Counter>()
        .grain(100)
        .each([](SystemContext&, ChunkView& c) {
            Counter* ctr = c.write<Counter>(0);
            for (u32 r = 0; r < c.count(); ++r) ctr[r].value += 1;
        }).hasValue());
    for (int t = 0; t < 12; ++t) REQUIRE(world.tick(0.05f).hasValue());
    CHECK(everyThird.load() == 4); // ticks 2, 5, 8, 11 ((tick + 1) % 3 == 0)
    u64 total = 0, minV = ~0ull, maxV = 0;
    Query q(world, {Term{world.id<Counter>(), TermAccess::Read}});
    q.forEachChunk([&](ChunkView& c) {
        for (u32 r = 0; r < c.count(); ++r) {
            const u64 v = c.read<Counter>(0)[r].value;
            total += v;
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
    });
    // Each job slice (100 rows) runs once every 4 ticks: 12 ticks -> 3 updates per entity.
    CHECK(total == 3000);
    CHECK(minV == 3);
    CHECK(maxV == 3);
    const SystemStats* st = world.systemStats("Staggered");
    REQUIRE(st);
    CHECK(st->runs == 12);
    CHECK(st->lastJobs == 3); // 10 jobs, every 4th
}

TEST_CASE("ecs scheduler: stats, stage order and chunk splitting") {
    jobs::JobSystem js({.workerCount = 2});
    World world({.jobs = &js});
    registerCommon(world);
    for (u32 i = 0; i < 5000; ++i) world.set(world.spawn(), Velocity{});
    std::vector<std::string> trace;
    CHECK(world.system("Late").stage(Stage::Send).once([&](SystemContext&) { trace.push_back("Late"); }).hasValue());
    CHECK(world.system("First").stage(Stage::Input).once([&](SystemContext&) { trace.push_back("First"); }).hasValue());
    std::atomic<u32> rows{0};
    std::atomic<u32> maxChunk{0};
    CHECK(world.system("Chunks")
        .stage(Stage::PrePhysics)
        .read<Velocity>()
        .grain(300)
        .budgetUs(0.001f)
        .each([&](SystemContext&, ChunkView& c) {
            rows += c.count();
            u32 prev = maxChunk.load();
            while (c.count() > prev && !maxChunk.compare_exchange_weak(prev, c.count())) {
            }
        }).hasValue());
    REQUIRE(world.tick(0.05f).hasValue());
    CHECK(trace == std::vector<std::string>{"First", "Late"});
    CHECK(rows.load() == 5000);
    CHECK(maxChunk.load() <= 300);
    const SystemStats* st = world.systemStats("Chunks");
    REQUIRE(st);
    CHECK(st->runs == 1);
    CHECK(st->lastRows == 5000);
    CHECK(st->lastJobs == 17); // ceil(5000 / 300)
    CHECK(st->lastNs > 0);
    CHECK(st->overBudgetRuns == 1);
    CHECK(world.allSystemStats().size() == 3);
}

#if HELIOS_ENABLE_ASSERTS
namespace {
std::atomic<int> g_asserts{0};
AssertAction countingHandler(const AssertInfo&) {
    ++g_asserts;
    return AssertAction::Continue;
}
} // namespace

TEST_CASE("ecs scheduler: undeclared random access and structural calls during a stage are caught") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    world.set(e, Velocity{});
    world.set(e, Health{});
    CHECK(world.system("Sneaky").read<Velocity>().each([](SystemContext& ctx, ChunkView& c) {
        (void)ctx.get<Health>(c.entity(0)); // Health not declared
        ctx.world().spawn();               // structural change outside a command buffer
    }).hasValue());
    const AssertHandler previous = setAssertHandler(countingHandler);
    g_asserts = 0;
    REQUIRE(world.tick(0.05f).hasValue());
    setAssertHandler(previous);
    CHECK(g_asserts.load() >= 2);
}
#endif
