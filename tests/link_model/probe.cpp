// link_model_probe: a game-like plugin image for the link-model tests (ADR-016, 02 §1.4, WP-0.6c part 1).
// It links helios::core, which in a modular build means importing from helios_runtime, and registers
// into the engine's registries through the engine's own functions. It owns no engine state, so it keeps
// the symbol audit's game rules (tools/lint/symbol_audit.cmake, R4-R6). Entry points are C functions,
// resolved with DynamicLibrary; every one may be called from any thread.
#include "helios/core/cvar.h"
#include "helios/core/memory.h"
#include "helios/core/platform.h"
#include "helios/runtime_api.h"

#include <string>
#include <vector>

namespace {

helios::MemoryTag probeTag() {
    // Registered by name, so a second call returns the same tag (core keeps the registry).
    return helios::registerMemoryTag("link_model.probe");
}

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
