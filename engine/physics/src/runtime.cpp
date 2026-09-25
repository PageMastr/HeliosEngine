// Process-wide Jolt setup: allocation, trace and assert hooks, type registration (runtime.h).
#include "helios/physics/runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "helios/core/assert.h"
#include "helios/core/log.h"
#include "jolt.h"
#include "log_channel.h"

namespace helios::physics {

namespace {

std::mutex g_mutex;
u32 g_refs = 0;
std::atomic<bool> g_initialized{false};
bool g_hooksInstalled = false;

MemoryTag joltTag() {
    static const MemoryTag tag = registerMemoryTag("physics.jolt");
    return tag;
}

// Jolt allocates with malloc semantics (16-byte alignment on x64) and aligned variants for SIMD types.
constexpr usize kJoltDefaultAlignment = 16;

void* joltAllocate(size_t size) { return alignedAlloc(size == 0 ? 1 : size, kJoltDefaultAlignment, joltTag()); }

void joltFree(void* block) { alignedFree(block); }

void* joltReallocate(void* block, size_t oldSize, size_t newSize) {
    void* fresh = joltAllocate(newSize);
    if (fresh && block) std::memcpy(fresh, block, oldSize < newSize ? oldSize : newSize);
    joltFree(block);
    return fresh;
}

void* joltAlignedAllocate(size_t size, size_t alignment) {
    return alignedAlloc(size == 0 ? 1 : size, alignment < kJoltDefaultAlignment ? kJoltDefaultAlignment : alignment,
                        joltTag());
}

void joltAlignedFree(void* block) { alignedFree(block); }

void joltTrace(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    HELIOS_LOG_INFO(LogPhysics, "Jolt: {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    const AssertAction action = detail::reportAssertFailure("JOLT_ASSERT", expression, file, static_cast<u32>(line),
                                                            "Jolt", message ? std::string_view(message) : std::string_view());
    if (action == AssertAction::Abort) detail::assertAbort();
    return action == AssertAction::Break; // true: JPH_BREAKPOINT
}
#endif

} // namespace

PhysicsRuntime::PhysicsRuntime() {
    std::lock_guard lock(g_mutex);
    if (g_refs++ > 0) return;
    if (!g_hooksInstalled) {
        // Installed once and never removed: blocks allocated through them may outlive the runtime.
        JPH::Allocate = joltAllocate;
        JPH::Reallocate = joltReallocate;
        JPH::Free = joltFree;
        JPH::AlignedAllocate = joltAlignedAllocate;
        JPH::AlignedFree = joltAlignedFree;
        JPH::Trace = joltTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = joltAssertFailed;
#endif
        g_hooksInstalled = true;
    }
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    g_initialized.store(true, std::memory_order_release);
    HELIOS_LOG_DEBUG(LogPhysics, "Jolt {}.{}.{} initialized (double precision, cross-platform deterministic)",
                     JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH);
}

PhysicsRuntime::~PhysicsRuntime() {
    std::lock_guard lock(g_mutex);
    HELIOS_ASSERT(g_refs > 0);
    if (--g_refs > 0) return;
    g_initialized.store(false, std::memory_order_release);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

bool PhysicsRuntime::initialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

MemoryTag PhysicsRuntime::memoryTag() noexcept { return joltTag(); }

} // namespace helios::physics
