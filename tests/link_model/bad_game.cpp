// link_model_bad_game: a symbol-audit fixture (tools/lint/symbol_audit.cmake, WP-0.6c part 1). A game-like
// image that breaks each game rule of 02 §1.4 on purpose. It is built (ELF modular builds only) and
// audited, never loaded.
#include "helios/core/memory.h"
#include "helios/core/platform.h"

#include <mimalloc.h>

namespace helios::probe {
int g_ticks = 0; // R5: engine-namespace state owned by a game image
} // namespace helios::probe

namespace helios {
// R6: a strong copy of a function helios_runtime exports.
std::string_view memoryTagName(MemoryTag) noexcept { return "bad-game"; }
} // namespace helios

// R4: a second, static mimalloc.
HELIOS_PLUGIN_EXPORT void* helios_link_model_bad_game_alloc(unsigned bytes) {
    ++helios::probe::g_ticks;
    return mi_malloc(bytes);
}
