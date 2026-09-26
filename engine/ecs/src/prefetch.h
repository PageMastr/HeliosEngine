#pragma once
// Software prefetch for the structural-op paths that visit scattered memory in a known order.

#include "helios/core/platform.h"

#if defined(HELIOS_COMPILER_MSVC)
#include <intrin.h>
#endif

namespace helios::ecs::detail {

/// Hints that the cache line at `p` will be read soon. Never faults, whatever the address.
HELIOS_FORCEINLINE void prefetch(const void* p) noexcept {
#if defined(HELIOS_COMPILER_MSVC) && defined(HELIOS_ARCH_X64)
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#elif defined(HELIOS_COMPILER_MSVC)
    __prefetch(p);
#else
    __builtin_prefetch(p);
#endif
}

} // namespace helios::ecs::detail
