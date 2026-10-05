#pragma once
// test_type_keys_other.cpp's half of the type-key tests in test_type_keys.cpp. That file's types in an unnamed
// namespace print the same names as test_type_keys.cpp's (Cooldown, KeyClash) but are other types, so they
// must never reach each other's components. Same thread as the World.

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/type_key.h"
#include "helios/ecs/world.h"

/// In the global namespace on purpose: keyed per image (helios/ecs/type_key.h), one key in every translation
/// unit of the image.
struct GlobalKeyed {
    helios::u32 value = 0;
};

namespace ecs_test {

/// World::id<Cooldown>() of test_type_keys_other.cpp's Cooldown {u64 started; u64 length;}.
helios::ecs::ComponentId otherCooldownId(const helios::ecs::World& world);
/// World::get<Cooldown>(e) of that Cooldown (nullptr if `e` has none).
const void* otherCooldownGet(const helios::ecs::World& world, helios::ecs::Entity e);
/// World::has<Cooldown>(e) of that Cooldown.
bool otherCooldownHas(const helios::ecs::World& world, helios::ecs::Entity e);
/// Registers that Cooldown as "ecs_test.OtherCooldown", sets {started, length} on `e` and returns its id.
helios::ecs::ComponentId otherCooldownRegisterAndSet(helios::ecs::World& world, helios::ecs::Entity e,
                                                     helios::u64 started, helios::u64 length);
/// `started` of that Cooldown on `e` (~0 if `e` has none).
helios::u64 otherCooldownStarted(const helios::ecs::World& world, helios::ecs::Entity e);

/// Binds test_type_keys_other.cpp's KeyClash to `id` in `world` (World::bindType).
helios::Result<void> bindOtherKeyClash(helios::ecs::World& world, helios::ecs::ComponentId id);
/// World::id<KeyClash>() of that KeyClash.
helios::ecs::ComponentId otherKeyClashId(const helios::ecs::World& world);
/// helios::ecs::typeKey<KeyClash>() of that KeyClash.
helios::ecs::TypeKey otherKeyClashKey() noexcept;
/// helios::ecs::typeKey<GlobalKeyed>() as test_type_keys_other.cpp computes it.
helios::ecs::TypeKey otherGlobalKeyedKey() noexcept;

} // namespace ecs_test
