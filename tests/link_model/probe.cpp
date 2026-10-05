// link_model_probe: a game-like plugin image for the link-model tests (ADR-016, 02 §1.4, WP-0.6c part 1).
// It links helios::core and helios::ecs, which in a modular build means importing from helios_runtime, and
// registers into the engine's registries through the engine's own functions. It owns no engine state, so
// it keeps the symbol audit's game rules (tools/lint/symbol_audit.cmake, R4-R6). Entry points are C
// functions, resolved with DynamicLibrary; each may be called from any thread unless its comment says
// otherwise.
#include "helios/core/cvar.h"
#include "helios/core/memory.h"
#include "helios/core/platform.h"
#include "helios/ecs/command_buffer.h"
#include "helios/ecs/world.h"
#include "helios/runtime_api.h"

#include <cstdint>
#include <string>
#include <vector>

#include "probe_types.h"

namespace {

helios::MemoryTag probeTag() {
    // Registered by name, so a second call returns the same tag (core keeps the registry).
    return helios::registerMemoryTag("link_model.probe");
}

/// Prints the same name as test_link_model.cpp's Cooldown but is another, larger type, private to this
/// translation unit: it must never reach that type's component (helios/ecs/type_key.h).
struct Cooldown {
    std::uint64_t started = 0;
    std::uint64_t length = 0;
};

} // namespace

/// Registers the console command "link_model.probe" (code in this image, registry in helios_runtime).
HELIOS_PLUGIN_EXPORT int helios_link_model_probe_register() {
    auto r = helios::CVarRegistry::instance().registerCommand(
        "link_model.probe", "link-model test command from a plugin image",
        [](std::span<const std::string> args) -> helios::Result<std::string> {
            return std::string("probe:") + std::to_string(args.size());
        });
    return r ? 1 : 0;
}

/// Removes the command again; must run before the image is unloaded.
HELIOS_PLUGIN_EXPORT int helios_link_model_probe_unregister() {
    return helios::CVarRegistry::instance().unregisterCommand("link_model.probe") ? 1 : 0;
}

/// Fills STL containers with heap memory allocated in this image; the caller frees it (one CRT heap).
HELIOS_PLUGIN_EXPORT void helios_link_model_probe_fill(std::string* text, std::vector<unsigned>* values) {
    text->assign(4096, 'p');
    values->assign(1000, 7u);
}

/// Allocates engine memory under the probe's tag; the caller frees it with helios::alignedFree.
HELIOS_PLUGIN_EXPORT void* helios_link_model_probe_alloc(unsigned bytes) {
    return helios::alignedAlloc(bytes, 64, probeTag());
}

/// The address of helios_runtime's module list as this image sees it (one runtime image: one address).
HELIOS_PLUGIN_EXPORT const char* helios_link_model_probe_runtime_modules() {
    return helios_runtime_link_group_modules();
}

/// Typed ECS access across images, on a World the caller created (its constructor ran in helios_runtime):
/// the built-in components, a type the caller registered (HostCounter) and one this image registers
/// (ProbeCounter), written with World::set<T> and with a typed CommandBuffer. Returns 1, or a negative
/// step number at the first check that fails. Same thread as the World (World's threading rules).
HELIOS_PLUGIN_EXPORT int helios_link_model_probe_ecs(helios::ecs::World* world, std::uint64_t entity,
                                                     unsigned value) {
    using namespace helios::ecs;
    using helios::link_model::HostCounter;
    using helios::link_model::ProbeCounter;
    if (world->id<NetIdentity>() == 0 || world->id<NetIdentity>() != world->netIdentityId()) return -1;
    if (world->id<RepDirty>() == 0 || world->id<RepDirty>() != world->repDirtyId()) return -2;
    if (world->id<FrameRef>() == 0 || world->id<DockRef>() == 0) return -3;
    const Entity e{entity};
    const HostCounter* host = world->get<HostCounter>(e);
    if (world->id<HostCounter>() == 0 || host == nullptr || host->value != value) return -4;
    if (world->id<ProbeCounter>() == 0) world->registerComponent<ProbeCounter>();
    world->set(e, ProbeCounter{value});
    const ProbeCounter* back = world->get<ProbeCounter>(e);
    if (back == nullptr || back->value != value) return -5;
    CommandBuffer commands(world);
    commands.set(e, HostCounter{value + 1});
    world->apply(commands);
    host = world->get<HostCounter>(e);
    if (host == nullptr || host->value != value + 1) return -6;
    return 1;
}

/// Typed ECS access through a type in an unnamed namespace, on a World where the caller registered and set its
/// own type of that name on `entity`: this image's Cooldown must resolve to no component. Returns 1, or a
/// negative step number at the first check that fails. Same thread as the World.
HELIOS_PLUGIN_EXPORT int helios_link_model_probe_private_type(const helios::ecs::World* world,
                                                              std::uint64_t entity) {
    const helios::ecs::Entity e{entity};
    if (world->id<Cooldown>() != 0) return -1;
    if (world->has<Cooldown>(e)) return -2;
    if (world->get<Cooldown>(e) != nullptr) return -3;
    return 1;
}
