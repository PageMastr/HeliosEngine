#pragma once
// Component types that link_model_tests and link_model_probe both use (WP-0.6c). Each image computes a
// type's ECS key on its own (helios::ecs::kTypeKey<T>), so typed World access from one image to a type
// another image registered works only if both images derive the same key (02 §1.4, "No per-image caches
// of global state"). Plain value types; no threading rules.
#include "helios/core/types.h"

namespace helios::link_model {

/// Registered by the executable; the plugin finds it by its type key.
struct HostCounter {
    u32 value = 0;
};

/// Registered by the plugin; the executable reads it back with its own key.
struct ProbeCounter {
    u32 value = 0;
};

} // namespace helios::link_model
