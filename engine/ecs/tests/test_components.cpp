// Component registration: template and runtime-descriptor paths, hooks, tags, traits, singletons,
// entity identity and raw access.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>

#include "flecs_internal.h"
#include "helios/core/guid.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;
using namespace ecs_test;

namespace {

std::atomic<int> g_live{0};

/// Non-trivial component: lifetime is tracked through the flecs hooks.
struct Named {
    std::string name = "default";
    Named() { ++g_live; }
    Named(const Named& o) : name(o.name) { ++g_live; }
    Named(Named&& o) noexcept : name(std::move(o.name)) { ++g_live; }
    Named& operator=(const Named&) = default;
    Named& operator=(Named&&) noexcept = default;
    ~Named() { --g_live; }
};

struct Opaque16 {
    u64 a = 0, b = 0;
};

} // namespace

TEST_CASE("ecs components: template registration binds types to ids") {
    World world;
    const ComponentId pos = world.registerComponent<Position>();
    const ComponentId vel = world.registerComponent<Velocity>();
    CHECK(pos != 0);
    CHECK(vel != 0);
    CHECK(pos != vel);
    CHECK(world.id<Position>() == pos);
    CHECK(world.registerComponent<Position>() == pos); // idempotent
    const ComponentInfo* info = world.componentInfo(pos);
    REQUIRE(info);
    CHECK(info->size == sizeof(Position));
    CHECK(info->alignment == alignof(Position));
    CHECK(info->isReplicated());
    CHECK(info->replicatedFieldCount == 1);
    CHECK(info->dirtyOffset == offsetof(Position, _dirty));
    CHECK_FALSE(world.componentInfo(vel)->isReplicated());
    CHECK(world.findComponent(info->name) == info);
    CHECK(world.replicatedComponent(info->replIndex) == info);
    CHECK(world.replicationBit<Position>() == (u64(1) << info->replIndex));
    CHECK(world.replicationBit<Velocity>() == 0);
    CHECK(world.id<Health>() == 0); // not registered in this world
}

TEST_CASE("ecs components: default values come from the C++ default constructor") {
    World world;
    registerCommon(world);
    const Entity e = world.spawn();
    world.add<Health>(e);
    REQUIRE(world.get<Health>(e));
    CHECK(world.get<Health>(e)->hp == 100.0f);
    CHECK(world.get<Health>(e)->maxHp == 100.0f);
    world.set(e, Health{25.0f, 50.0f, 3});
    CHECK(world.get<Health>(e)->hp == 25.0f);
    CHECK(world.has<Health>(e));
    CHECK(world.owns<Health>(e));
    world.remove<Health>(e);
    CHECK_FALSE(world.has<Health>(e));
}

TEST_CASE("ecs components: non-trivial hooks construct, copy and destroy") {
    const int before = g_live.load();
    {
        World world;
        world.registerComponent<Named>();
        world.registerComponent<Velocity>();
        std::vector<Entity> es;
        for (int i = 0; i < 100; ++i) {
            const Entity e = world.spawn();
            Named n;
            n.name = "ship-" + std::to_string(i) + "-with-a-long-name-to-defeat-sso";
            world.set(e, n);
            es.push_back(e);
        }
        CHECK(g_live.load() - before == 100);
        // Table moves (adding a component) must move the strings intact.
        for (const Entity e : es) world.add<Velocity>(e);
        CHECK(world.get<Named>(es[42])->name == "ship-42-with-a-long-name-to-defeat-sso");
        CHECK(g_live.load() - before == 100);
        world.destroy(es[0]);
        CHECK(g_live.load() - before == 99);
    }
    CHECK(g_live.load() == before);
}

TEST_CASE("ecs components: runtime descriptor path (reflection-driven registration)") {
    World world;
    ComponentDesc desc;
    desc.name = "game.RuntimeBlob";
    desc.size = sizeof(Opaque16);
    desc.alignment = alignof(Opaque16);
    Result<ComponentId> id = world.registerComponent(desc);
    REQUIRE(id.hasValue());
    CHECK(world.findComponent("game.RuntimeBlob")->id == *id);
    // Same name + layout is idempotent; a different layout is an error.
    CHECK(*world.registerComponent(desc) == *id);
    ComponentDesc bad = desc;
    bad.size = 8;
    CHECK(world.registerComponent(bad).errorCode() == ErrorCode::AlreadyExists);

    const Entity e = world.spawn();
    const Opaque16 v{7, 9};
    world.setRaw(e, *id, &v, sizeof v);
    const auto* got = static_cast<const Opaque16*>(world.getRaw(e, *id));
    REQUIRE(got);
    CHECK(got->a == 7);
    CHECK(got->b == 9);
    // A C++ type can be bound to the runtime component afterwards.
    CHECK(world.bindType<Opaque16>(*id).hasValue());
    CHECK(world.get<Opaque16>(e)->b == 9);
    CHECK(world.bindType<Position>(*id).errorCode() == ErrorCode::InvalidArgument);

    ComponentDesc invalid;
    invalid.name = "game.BadRepl";
    invalid.size = 8;
    invalid.alignment = 8;
    invalid.flags = ComponentFlags::Replicated; // no dirty layout
    CHECK(world.registerComponent(invalid).errorCode() == ErrorCode::InvalidArgument);
    ComponentDesc noName;
    CHECK(world.registerComponent(noName).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("ecs components: runtime hooks are invoked") {
    static std::atomic<int> constructed{0};
    static std::atomic<int> destructed{0};
    constructed = 0;
    destructed = 0;
    {
        World world;
        ComponentDesc d;
        d.name = "game.Hooked";
        d.size = 4;
        d.alignment = 4;
        d.hooks.construct = [](void* p, i32 n) {
            for (i32 i = 0; i < n; ++i) static_cast<u32*>(p)[i] = 0xC0FFEE;
            constructed += n;
        };
        d.hooks.destruct = [](void*, i32 n) { destructed += n; };
        const ComponentId id = *world.registerComponent(d);
        const Entity e = world.spawn();
        world.addId(e, id);
        CHECK(*static_cast<const u32*>(world.getRaw(e, id)) == 0xC0FFEE);
        CHECK(constructed.load() >= 1);
        world.destroy(e);
        CHECK(destructed.load() >= 1);
    }
}

TEST_CASE("ecs components: tags, sparse and non-fragmenting traits") {
    World world;
    const ComponentId tag = world.registerComponent<Tagged>();
    CHECK(world.componentInfo(tag)->isTag());
    const ComponentId sparse = world.registerComponent<Counter>(ComponentFlags::Sparse);
    struct Churn {
        u32 v = 0;
    };
    const ComponentId churn = world.registerComponent<Churn>(ComponentFlags::DontFragment);
    CHECK(ecs_has_id(world.flecsWorld(), sparse, EcsSparse));
    CHECK(ecs_has_id(world.flecsWorld(), churn, EcsDontFragment));

    const Entity e = world.spawn();
    world.add<Tagged>(e);
    CHECK(world.has<Tagged>(e));
    const u32 tablesBefore = world.stats().tableCount;
    std::vector<Entity> es;
    for (int i = 0; i < 64; ++i) es.push_back(world.spawn());
    const u32 tablesMid = world.stats().tableCount;
    for (const Entity x : es) world.set(x, Churn{7});
    CHECK(world.stats().tableCount == tablesMid); // DontFragment: no table per combination
    for (const Entity x : es) CHECK(world.get<Churn>(x)->v == 7);
    (void)tablesBefore;
}

TEST_CASE("ecs components: singletons") {
    World world;
    struct ZoneClock {
        u64 tick = 0;
    };
    world.registerComponent<ZoneClock>(ComponentFlags::Singleton);
    CHECK(world.singleton<ZoneClock>() == nullptr);
    world.setSingleton(ZoneClock{42});
    REQUIRE(world.singleton<ZoneClock>());
    CHECK(world.singleton<ZoneClock>()->tick == 42);
}

TEST_CASE("ecs components: spawn assigns EntityId + NetHandle and maps them") {
    World world({.shard = 2});
    const Entity a = world.spawn();
    const Entity b = world.spawn({.netHandle = false});
    const EntityId ida = world.entityId(a);
    CHECK(ida.kind() == EntityIdKind::Runtime);
    CHECK(decodeBlockId(ida).shard == 2);
    CHECK(world.find(ida) == a);
    CHECK(world.netHandle(a).isValid());
    CHECK(world.find(world.netHandle(a)) == a);
    CHECK_FALSE(world.netHandle(b).isValid());
    CHECK(world.entityId(b) > ida);

    const EntityId placed = EntityId::fromContentGuid(Guid(5, 6));
    const Entity c = world.spawn({.id = placed, .ag = 99});
    CHECK(world.entityId(c) == placed);
    CHECK(world.authorityGroup(c) == 99);
    CHECK(world.find(placed) == c);
    CHECK_FALSE(world.spawn({.id = placed}).isValid()); // duplicate id refused

    world.setName(c, "station-alpha");
    CHECK(world.nameOf(c) == "station-alpha");

    const NetHandle ha = world.netHandle(a);
    world.destroy(a);
    CHECK_FALSE(world.isAlive(a));
    CHECK_FALSE(world.find(ida).isValid());
    CHECK_FALSE(world.find(ha).isValid());
    CHECK(world.registry().handles().liveCount() == 1); // only c
    CHECK(world.stats().entityCount == 2);
}

TEST_CASE("ecs components: content-placed NetHandle slots") {
    World world({.handles = {.maxHandles = 1000, .reservedCount = 100}});
    const Entity e = world.spawn({.id = EntityId::contentPlaced(77), .contentHandleIndex = 42});
    CHECK(world.netHandle(e).index() == 42);
    const Entity d = world.spawn();
    CHECK(world.netHandle(d).index() == 101);
}

namespace ecs_test_names {
struct Thruster {
    float force = 0;
};
} // namespace ecs_test_names

TEST_CASE("ecs components: default names are the qualified C++ names") {
    CHECK(defaultComponentName<ecs_test_names::Thruster>() == "ecs_test_names.Thruster");
    CHECK(defaultComponentName<ecs_test::Position>() == "ecs_test.Position");
    World world;
    const ComponentId id = world.registerComponent<ecs_test_names::Thruster>();
    CHECK(world.findComponent("ecs_test_names.Thruster")->id == id);
}

TEST_CASE("ecs components: plain components keep flecs tables on the fast path") {
    World world;
    registerCommon(world);
    world.registerComponent<Named>();
    const Entity plain = world.spawn();
    world.set(plain, Position{});
    world.set(plain, Velocity{});
    world.add<Health>(plain); // default value written by the World (no flecs ctor)
    CHECK(world.get<Health>(plain)->hp == 100.0f);
    CHECK(world.has<RepDirty>(plain));
    const ecs_table_t* table = ecs_get_table(world.flecsWorld(), plain.id);
    REQUIRE(table);
    // No lifecycle hooks: flecs uses its memcpy fast paths for append/move/delete.
    CHECK_FALSE(ecs_table_has_flags(const_cast<ecs_table_t*>(table), EcsTableIsComplex));
    const Entity rich = world.spawn();
    world.set(rich, Named{});
    CHECK(ecs_table_has_flags(ecs_get_table(world.flecsWorld(), rich.id), EcsTableHasLifecycle));
    const ComponentInfo* info = world.componentInfo(world.id<Health>());
    REQUIRE(info->defaultValue);
    CHECK(reinterpret_cast<const Health*>(info->defaultValue)->maxHp == 100.0f);
    CHECK(world.componentInfo(world.id<Named>())->defaultValue == nullptr);
}
