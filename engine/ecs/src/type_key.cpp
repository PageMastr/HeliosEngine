#include "helios/ecs/type_key.h"

#include <atomic>

namespace helios::ecs {

namespace {
// One counter per process: helios_runtime holds it in a modular build, so per-image keys drawn in two images
// never meet in one World.
std::atomic<TypeKey> g_perImageKeys{0};
} // namespace

TypeKey detail::nextPerImageTypeKey() noexcept {
    // 2, 4, 6, ...: even and never 0, so a per-image key never equals a name key (odd).
    return (g_perImageKeys.fetch_add(1, std::memory_order_relaxed) + 1) * 2;
}

} // namespace helios::ecs
