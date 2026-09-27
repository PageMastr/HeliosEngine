#pragma once
// Private flecs include for engine/ecs sources. The flecs C++ addon is skipped (FLECS_NO_CPP):
// Helios wraps the C API directly, which keeps compile times down and makes the component
// registration path identical for template and runtime (reflection) descriptors.

#ifndef FLECS_NO_CPP
#define FLECS_NO_CPP
#endif
#include <flecs.h>

#include "helios/ecs/types.h"

// flecs 4.1.6 internals that flecs.h does not declare (defined in flecs.c, extern linkage). Keep
// this list short. A renamed or removed function fails to link, but a changed signature links
// silently, so a flecs update must re-check each declaration: the static_assert makes it.
static_assert(FLECS_VERSION_MAJOR == 4 && FLECS_VERSION_MINOR == 1 && FLECS_VERSION_PATCH == 6,
              "flecs changed: re-check the internals declared in flecs_internal.h (and the DontFragment "
              "record workaround in World::registerComponent), then update this version");
extern "C" {
/// Whether `entity` has a value in the sparse storage of `cr` (a Sparse or DontFragment component,
/// not a wildcard): ecs_owns_id() for such components without its table-cache lookup.
bool flecs_component_sparse_has(ecs_component_record_t* cr, ecs_entity_t entity);
}

namespace helios::ecs::detail {

static_assert(sizeof(ecs_entity_t) == sizeof(u64));

inline ecs_entity_t fe(Entity e) noexcept { return static_cast<ecs_entity_t>(e.id); }
inline Entity he(ecs_entity_t e) noexcept { return Entity(static_cast<u64>(e)); }

} // namespace helios::ecs::detail
