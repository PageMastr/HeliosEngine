#pragma once
// test_type_keys_other.cpp's half of the key-clash test in test_type_keys.cpp: a type in an unnamed
// namespace there has the same qualified name as test_type_keys.cpp's KeyClash. Same thread as the World.

#include "helios/core/result.h"
#include "helios/ecs/world.h"

namespace ecs_test {

/// Binds test_type_keys_other.cpp's KeyClash to `id` in `world` (World::bindType).
helios::Result<void> bindOtherKeyClash(helios::ecs::World& world, helios::ecs::ComponentId id);
/// helios::ecs::kTypeKey of that KeyClash.
helios::ecs::TypeKey otherKeyClashKey() noexcept;

} // namespace ecs_test
