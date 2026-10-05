// link_model_plugin: a plugin image that is not a game module (an editor or tool plugin, say) for the
// link-model tests (ADR-016, 02 §1.4, WP-0.6c part 1). The game rules do not cover it, so it may use a type
// outside a named namespace through the typed ECS API: that type gets a per-image key
// (helios/ecs/type_key.h), which link_model_probe, a game image, may not hold (symbol audit R5). Entry points
// are C functions, resolved with DynamicLibrary.
#include "helios/core/platform.h"
#include "helios/ecs/world.h"

#include <cstdint>

namespace {

/// Prints the same name as test_link_model.cpp's Cooldown and has the same layout, but is another type,
/// private to this translation unit: only its per-image key keeps it from that type's component.
struct Cooldown {
    std::uint32_t ticks = 0;
};

} // namespace

/// Typed ECS access through a type in an unnamed namespace, on a World where the caller registered and set
/// its own type of that name and layout on `entity`: this image's Cooldown must resolve to no component.
/// Returns 1, or a negative step number at the first check that fails. Same thread as the World (World's
/// threading rules).
HELIOS_PLUGIN_EXPORT int helios_link_model_plugin_private_type(const helios::ecs::World* world,
                                                               std::uint64_t entity) {
    const helios::ecs::Entity e{entity};
    if (world->id<Cooldown>() != 0) return -1;
    if (world->has<Cooldown>(e)) return -2;
    if (world->get<Cooldown>(e) != nullptr) return -3;
    return 1;
}
