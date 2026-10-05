// Type keys (kTypeKey; 02 §1.4, WP-0.6c): the World's C++ type -> component table is keyed by a
// compile-time hash of the type's name, so an image finds a type that any other image bound; nothing per
// image caches it. Two distinct types with one qualified name share a key, and a World binds only the
// first.

#include <doctest/doctest.h>

#include <utility>

#include "helios/core/platform.h"
#include "helios/ecs/world.h"
#include "test_types.h"
#include "type_key_clash.h"

namespace {

using namespace helios;
using namespace helios::ecs;

struct KeyClash {
    u32 value = 0;
};

template <u32 N>
struct Numbered {
    u32 value = N;
};

static_assert(kTypeKey<ecs_test::Position> != 0);
static_assert(kTypeKey<const ecs_test::Position> == kTypeKey<ecs_test::Position>);
static_assert(kTypeKey<ecs_test::Position> != kTypeKey<ecs_test::Velocity>);
static_assert(kTypeKey<NetIdentity> != kTypeKey<RepDirty>);
static_assert(kTypeKey<Numbered<1>> != kTypeKey<Numbered<2>>);

TEST_CASE("ecs type keys: the built-ins are bound by key") {
    World world;
    CHECK(world.id<NetIdentity>() == world.netIdentityId());
    CHECK(world.idForKey(kTypeKey<NetIdentity>) == world.netIdentityId());
    CHECK(world.idForKey(kTypeKey<RepDirty>) == world.repDirtyId());
    CHECK(world.id<FrameRef>() != 0);
    CHECK(world.id<DockRef>() != 0);
    CHECK(world.id<KeyClash>() == 0);
    CHECK(world.replicationBit<KeyClash>() == 0);
}

template <u32... N>
void registerAndCheck(World& world, std::integer_sequence<u32, N...>) {
    const ComponentId ids[] = {world.registerComponent<Numbered<N>>()...};
    const ComponentId found[] = {world.id<Numbered<N>>()...};
    for (usize i = 0; i < sizeof...(N); ++i) {
        CAPTURE(i);
        CHECK(ids[i] != 0);
        CHECK(found[i] == ids[i]);
    }
    // Every id is distinct: the table kept each key apart through its growth.
    for (usize i = 0; i < sizeof...(N); ++i) {
        for (usize j = i + 1; j < sizeof...(N); ++j) CHECK(ids[i] != ids[j]);
    }
}

TEST_CASE("ecs type keys: many types in one world keep their ids as the table grows") {
    World world;
    registerAndCheck(world, std::make_integer_sequence<u32, 70>{});
    CHECK(world.id<NetIdentity>() == world.netIdentityId());
    // Registering a type again is idempotent and keeps its binding.
    const ComponentId again = world.registerComponent<Numbered<3>>();
    CHECK(again == world.id<Numbered<3>>());
}

TEST_CASE("ecs type keys: two types with one qualified name, and a world binds only the first") {
    World world;
    const Result<ComponentId> a = world.registerComponent(componentDescOf<KeyClash>("ecs_test.KeyClashA"));
    const Result<ComponentId> b = world.registerComponent(componentDescOf<KeyClash>("ecs_test.KeyClashB"));
    REQUIRE(a.hasValue());
    REQUIRE(b.hasValue());
    REQUIRE(world.bindType<KeyClash>(*a).hasValue());
    CHECK(world.bindType<KeyClash>(*a).hasValue()); // the same binding again is fine
    const Result<void> other = ecs_test::bindOtherKeyClash(world, *b);
#if !defined(HELIOS_COMPILER_MSVC)
    // GCC and Clang name both types "(anonymous namespace)::KeyClash"; MSVC's spelling is checked by the
    // branch below either way.
    CHECK(ecs_test::otherKeyClashKey() == kTypeKey<KeyClash>);
#endif
    if (ecs_test::otherKeyClashKey() == kTypeKey<KeyClash>) {
        REQUIRE_FALSE(other.hasValue());
        CHECK(other.errorCode() == ErrorCode::AlreadyExists);
        CHECK(world.id<KeyClash>() == *a); // the first binding stands
    } else {
        CHECK(other.hasValue());
    }
    // A component bound to one type cannot be bound to another.
    const Result<void> twice = world.bindType<Numbered<7>>(*a);
    REQUIRE_FALSE(twice.hasValue());
    CHECK(twice.errorCode() == ErrorCode::AlreadyExists);
    CHECK(world.id<Numbered<7>>() == 0);
}

} // namespace
