#pragma once
// Private flecs include for engine/ecs sources. The flecs C++ addon is skipped (FLECS_NO_CPP):
// Helios wraps the C API directly, which keeps compile times down and makes the component
// registration path identical for template and runtime (reflection) descriptors.

#ifndef FLECS_NO_CPP
#define FLECS_NO_CPP
#endif
#include <flecs.h>

#include "helios/ecs/types.h"

namespace helios::ecs::detail {

static_assert(sizeof(ecs_entity_t) == sizeof(u64));

inline ecs_entity_t fe(Entity e) noexcept { return static_cast<ecs_entity_t>(e.id); }
inline Entity he(ecs_entity_t e) noexcept { return Entity(static_cast<u64>(e)); }

} // namespace helios::ecs::detail
