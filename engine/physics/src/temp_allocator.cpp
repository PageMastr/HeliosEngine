#include "temp_allocator.h"

#include "log_channel.h"

namespace helios::physics::jolt {

TempAllocator::TempAllocator(usize bytes, std::string gridName)
    : m_impl(static_cast<size_t>(bytes)), m_gridName(std::move(gridName)) {}

TempAllocator::~TempAllocator() = default;

void* TempAllocator::Allocate(JPH::uint size) {
    if (size == 0) return nullptr;
    if (m_impl.CanAllocate(size)) return m_impl.Allocate(size);
    if (m_fallbacks++ == 0) {
        HELIOS_LOG_WARN(LogPhysics, "grid '{}': temp allocator ({} KiB) too small for a {} KiB request; using the heap "
                        "(raise GridDesc::tempAllocatorBytes)", m_gridName, m_impl.GetSize() >> 10, size >> 10);
    }
    return JPH::AlignedAllocate(size, JPH_RVECTOR_ALIGNMENT);
}

void TempAllocator::Free(void* address, JPH::uint size) {
    if (!address) return;
    if (m_impl.OwnsMemory(address)) {
        m_impl.Free(address, size);
    } else {
        JPH::AlignedFree(address);
    }
}

} // namespace helios::physics::jolt
