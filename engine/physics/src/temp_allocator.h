// Internal: the per-grid temp allocator (02 §7.1). A bump allocator like Jolt's TempAllocatorImpl,
// but a request that does not fit falls back to the heap and is logged once per grid, where Jolt's
// own allocator aborts the process (JPH_CRASH) when a step outgrows it.
//
// Threading: Jolt uses a temp allocator from one thread at a time (the stepping thread and, during a
// step, its jobs in strict LIFO order through the job system's barrier); not thread-safe itself.
#pragma once

#include <string>

#include "jolt.h"

namespace helios::physics::jolt {

class TempAllocator final : public JPH::TempAllocator {
public:
    TempAllocator(usize bytes, std::string gridName);
    ~TempAllocator() override;

    void* Allocate(JPH::uint size) override;
    void Free(void* address, JPH::uint size) override;

    usize fallbackCount() const noexcept { return m_fallbacks; }

private:
    JPH::TempAllocatorImpl m_impl;
    std::string m_gridName;
    usize m_fallbacks = 0;
};

} // namespace helios::physics::jolt
