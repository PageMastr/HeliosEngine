// helios/physics/runtime.h — process-wide Jolt setup (02 §7.1, ADR-011 "Jolt allocators routed
// through mimalloc").
//
// A PhysicsRuntime installs, once per process:
//   * Jolt's allocation hooks, routed to helios::alignedAlloc under the "physics.jolt" memory tag
//     (mimalloc heaps behind the tagged allocators; no global operator new override);
//   * Jolt's trace hook, routed to the "Physics" log channel;
//   * Jolt's assert hook (builds with JPH_ENABLE_ASSERTS), routed to the Helios assert handler;
//   * the Jolt type factory and registered types.
// Runtimes are reference-counted: the first constructor initializes, the last destructor tears the
// factory down. The allocation hooks stay installed afterwards (memory Jolt still owns is freed
// through the same allocator).
//
// Threading: construction and destruction are thread-safe (serialized internally). Keep a runtime
// alive while any grid or shape exists.
#pragma once

#include "helios/core/memory.h"
#include "helios/core/types.h"

namespace helios::physics {

class PhysicsRuntime {
public:
    PhysicsRuntime();
    ~PhysicsRuntime();
    PhysicsRuntime(const PhysicsRuntime&) = delete;
    PhysicsRuntime& operator=(const PhysicsRuntime&) = delete;

    /// True while at least one PhysicsRuntime exists.
    static bool initialized() noexcept;
    /// The memory tag of every Jolt allocation.
    static MemoryTag memoryTag() noexcept;
};

} // namespace helios::physics
