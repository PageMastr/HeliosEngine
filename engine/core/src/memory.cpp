#include "helios/core/memory.h"

#include <algorithm>
#include <array>
#include <cstring>

#include <mimalloc.h>

#include "helios/core/assert.h"
#include "helios/core/log.h"
#include "helios/core/thread.h"
#include "platform/os.h"

namespace helios {

namespace {

constexpr usize kTagNameCapacity = 48;

// Constant-initialized so tracking works from any static initializer; never destroyed.
struct TagSlot {
    std::atomic<i64> liveBytes{0};
    std::atomic<i64> peakBytes{0};
    std::atomic<i64> liveCount{0};
    std::atomic<u64> totalCount{0};
    std::atomic<u64> budget{0};
    std::atomic<u64> exceeded{0};
    std::atomic<bool> overBudget{false};
    std::atomic<bool> used{false};
    char name[kTagNameCapacity] = {};
};

constinit std::array<TagSlot, kMaxMemoryTags> g_tags{};
constinit std::atomic<u32> g_nextUserTag{static_cast<u32>(MemoryTag::FirstUser)};

std::mutex& tagRegistrationMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

constexpr std::string_view builtinTagName(u32 index) noexcept {
    switch (static_cast<MemoryTag>(index)) {
    case MemoryTag::Unknown: return "Unknown";
    case MemoryTag::Core: return "Core";
    case MemoryTag::Containers: return "Containers";
    case MemoryTag::Strings: return "Strings";
    case MemoryTag::Jobs: return "Jobs";
    case MemoryTag::FileSystem: return "FileSystem";
    case MemoryTag::Logging: return "Logging";
    case MemoryTag::Temp: return "Temp";
    case MemoryTag::Frame: return "Frame";
    case MemoryTag::Pool: return "Pool";
    case MemoryTag::Virtual: return "Virtual";
    default: return {};
    }
}

TagSlot& slotFor(MemoryTag tag) noexcept {
    const u32 index = static_cast<u32>(tag);
    return g_tags[index < kMaxMemoryTags ? index : 0];
}

// Header stored immediately before every alignedAlloc block.
struct alignas(16) AllocHeader {
    u64 size;
    u32 prefix;
    u16 tag;
    u16 magic;
};
static_assert(sizeof(AllocHeader) == 16);
constexpr u16 kHeaderMagic = 0x4E1C;

AllocHeader* headerOf(const void* ptr) noexcept {
    return reinterpret_cast<AllocHeader*>(const_cast<std::byte*>(static_cast<const std::byte*>(ptr))) - 1;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------------------------

MemoryTag registerMemoryTag(std::string_view name, u64 budgetBytes) {
    std::lock_guard lock(tagRegistrationMutex());
    for (u32 i = 0; i < kMaxMemoryTags; ++i) {
        if (memoryTagName(static_cast<MemoryTag>(i)) == name && !name.empty()) {
            if (budgetBytes) g_tags[i].budget.store(budgetBytes, std::memory_order_relaxed);
            return static_cast<MemoryTag>(i);
        }
    }
    const u32 index = g_nextUserTag.load(std::memory_order_relaxed);
    if (index >= kMaxMemoryTags) {
        HELIOS_LOG_ERROR(LogMemory, "Memory tag table full; '{}' mapped to Unknown", name);
        return MemoryTag::Unknown;
    }
    TagSlot& slot = g_tags[index];
    const usize n = std::min(name.size(), kTagNameCapacity - 1);
    std::memcpy(slot.name, name.data(), n);
    slot.name[n] = '\0';
    slot.budget.store(budgetBytes, std::memory_order_relaxed);
    slot.used.store(true, std::memory_order_release);
    g_nextUserTag.store(index + 1, std::memory_order_relaxed);
    return static_cast<MemoryTag>(index);
}

std::string_view memoryTagName(MemoryTag tag) noexcept {
    const u32 index = static_cast<u32>(tag);
    if (index >= kMaxMemoryTags) return {};
    if (index < static_cast<u32>(MemoryTag::FirstUser)) return builtinTagName(index);
    const TagSlot& slot = g_tags[index];
    return slot.used.load(std::memory_order_acquire) ? std::string_view(slot.name) : std::string_view();
}

void setMemoryBudget(MemoryTag tag, u64 budgetBytes) noexcept {
    TagSlot& slot = slotFor(tag);
    slot.budget.store(budgetBytes, std::memory_order_relaxed);
    if (budgetBytes == 0 || slot.liveBytes.load(std::memory_order_relaxed) <= static_cast<i64>(budgetBytes)) {
        slot.overBudget.store(false, std::memory_order_relaxed);
    }
}

MemoryTagStats memoryTagStats(MemoryTag tag) noexcept {
    const TagSlot& slot = slotFor(tag);
    MemoryTagStats stats;
    stats.tag = tag;
    stats.name = memoryTagName(tag);
    stats.liveBytes = slot.liveBytes.load(std::memory_order_relaxed);
    stats.peakBytes = slot.peakBytes.load(std::memory_order_relaxed);
    stats.liveAllocations = slot.liveCount.load(std::memory_order_relaxed);
    stats.totalAllocations = slot.totalCount.load(std::memory_order_relaxed);
    stats.budgetBytes = slot.budget.load(std::memory_order_relaxed);
    stats.budgetExceededCount = slot.exceeded.load(std::memory_order_relaxed);
    return stats;
}

std::vector<MemoryTagStats> allMemoryTagStats() {
    std::vector<MemoryTagStats> out;
    for (u32 i = 0; i < kMaxMemoryTags; ++i) {
        if (!memoryTagName(static_cast<MemoryTag>(i)).empty()) out.push_back(memoryTagStats(static_cast<MemoryTag>(i)));
    }
    return out;
}

void trackAllocation(MemoryTag tag, usize bytes) noexcept {
    TagSlot& slot = slotFor(tag);
    const i64 live = slot.liveBytes.fetch_add(static_cast<i64>(bytes), std::memory_order_relaxed) +
                     static_cast<i64>(bytes);
    slot.liveCount.fetch_add(1, std::memory_order_relaxed);
    slot.totalCount.fetch_add(1, std::memory_order_relaxed);
    i64 peak = slot.peakBytes.load(std::memory_order_relaxed);
    while (live > peak && !slot.peakBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    const u64 budget = slot.budget.load(std::memory_order_relaxed);
    if (budget != 0 && live > static_cast<i64>(budget) && !slot.overBudget.exchange(true, std::memory_order_relaxed)) {
        slot.exceeded.fetch_add(1, std::memory_order_relaxed);
        HELIOS_LOG_WARN(LogMemory, "Memory tag '{}' over budget: {} bytes live > {} byte budget", memoryTagName(tag),
                        live, budget);
    }
}

void trackDeallocation(MemoryTag tag, usize bytes) noexcept {
    TagSlot& slot = slotFor(tag);
    const i64 live = slot.liveBytes.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed) -
                     static_cast<i64>(bytes);
    slot.liveCount.fetch_sub(1, std::memory_order_relaxed);
    const u64 budget = slot.budget.load(std::memory_order_relaxed);
    if (budget == 0 || live <= static_cast<i64>(budget)) slot.overBudget.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Tracked aligned heap (mimalloc-backed, ADR-011)
// ---------------------------------------------------------------------------------------------

void* alignedAlloc(usize size, usize alignment, MemoryTag tag) noexcept {
    if (size == 0) return nullptr;
    if (alignment < alignof(AllocHeader)) alignment = alignof(AllocHeader);
    // The header stores the prefix (== alignment beyond 16) in 32 bits.
    if (!isPowerOfTwo(alignment) || alignment > kMaxAlignedAllocAlignment) return nullptr;
    const usize prefix = alignUp<usize>(sizeof(AllocHeader), alignment);
    if (size > ~usize(0) - prefix) return nullptr;
    void* raw = mi_malloc_aligned(size + prefix, alignment);
    if (!raw) return nullptr;
    std::byte* user = static_cast<std::byte*>(raw) + prefix;
    AllocHeader* header = headerOf(user);
    header->size = size;
    header->prefix = static_cast<u32>(prefix);
    header->tag = static_cast<u16>(tag);
    header->magic = kHeaderMagic;
    trackAllocation(tag, size);
    return user;
}

void alignedFree(void* ptr) noexcept {
    if (!ptr) return;
    AllocHeader* header = headerOf(ptr);
    HELIOS_ASSERT(header->magic == kHeaderMagic, "alignedFree: pointer not from alignedAlloc or double free");
    trackDeallocation(static_cast<MemoryTag>(header->tag), static_cast<usize>(header->size));
    header->magic = 0;
    mi_free(static_cast<std::byte*>(ptr) - header->prefix);
}

usize alignedAllocSize(const void* ptr) noexcept { return ptr ? static_cast<usize>(headerOf(ptr)->size) : 0; }

MemoryTag alignedAllocTag(const void* ptr) noexcept {
    return ptr ? static_cast<MemoryTag>(headerOf(ptr)->tag) : MemoryTag::Unknown;
}

Allocator& defaultAllocator() noexcept {
    static HeapAllocator* allocator = new HeapAllocator(MemoryTag::Unknown);
    return *allocator;
}

// ---------------------------------------------------------------------------------------------
// LinearAllocator
// ---------------------------------------------------------------------------------------------

LinearAllocator::LinearAllocator(usize capacity, MemoryTag tag) : m_capacity(capacity), m_owned(true) {
    m_buffer = static_cast<std::byte*>(alignedAlloc(capacity, 64, tag));
    if (!m_buffer) m_capacity = 0;
}

LinearAllocator::LinearAllocator(void* buffer, usize capacity) noexcept
    : m_buffer(static_cast<std::byte*>(buffer)), m_capacity(buffer ? capacity : 0), m_owned(false) {}

LinearAllocator::~LinearAllocator() {
    if (m_owned) alignedFree(m_buffer);
}

void* LinearAllocator::allocate(usize size, usize alignment) {
    if (alignment == 0) alignment = 1;
    if (!m_buffer || !isPowerOfTwo(alignment)) return nullptr;
    const uptr base = reinterpret_cast<uptr>(m_buffer);
    const uptr aligned = alignUp<uptr>(base + m_offset, alignment);
    const usize begin = static_cast<usize>(aligned - base);
    if (begin > m_capacity || size > m_capacity - begin) return nullptr;
    m_offset = begin + size;
    m_peak = std::max(m_peak, m_offset);
    return m_buffer + begin;
}

void LinearAllocator::rewind(Marker marker) noexcept {
    HELIOS_ASSERT(marker <= m_offset, "LinearAllocator::rewind past the current offset");
    m_offset = marker;
}

// ---------------------------------------------------------------------------------------------
// FrameAllocator
// ---------------------------------------------------------------------------------------------

FrameAllocator::FrameAllocator(usize bytesPerFrame, u32 frameCount, MemoryTag tag)
    : m_capacity(alignUp<usize>(bytesPerFrame, 64)), m_frameCount(frameCount == 0 ? 1 : frameCount), m_tag(tag) {
    m_memory = static_cast<std::byte*>(alignedAlloc(m_capacity * m_frameCount, 64, tag));
    if (!m_memory) m_capacity = 0;
}

FrameAllocator::~FrameAllocator() { alignedFree(m_memory); }

void* FrameAllocator::allocate(usize size, usize alignment) noexcept {
    if (alignment == 0) alignment = 1;
    if (!isPowerOfTwo(alignment)) return nullptr;
    const uptr base = reinterpret_cast<uptr>(m_memory) + static_cast<uptr>(m_current) * m_capacity;
    usize offset = m_offset.load(std::memory_order_relaxed);
    for (;;) {
        const usize begin = static_cast<usize>(alignUp<uptr>(base + offset, alignment) - base);
        if (begin > m_capacity || size > m_capacity - begin) {
            m_overflows.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (m_offset.compare_exchange_weak(offset, begin + size, std::memory_order_relaxed)) {
            return reinterpret_cast<void*>(base + begin);
        }
    }
}

void FrameAllocator::beginFrame() noexcept {
    m_current = (m_current + 1) % m_frameCount;
    m_offset.store(0, std::memory_order_relaxed);
    ++m_frameIndex;
}

usize FrameAllocator::usedThisFrame() const noexcept { return m_offset.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------------------------
// PoolAllocator
// ---------------------------------------------------------------------------------------------

PoolAllocator::PoolAllocator(const Desc& desc)
    : m_blockAlignment(std::max<usize>(desc.blockAlignment, alignof(FreeNode))),
      m_blocksPerChunk(desc.blocksPerChunk == 0 ? 1 : desc.blocksPerChunk), m_maxChunks(desc.maxChunks),
      m_threadSafe(desc.threadSafe), m_tag(desc.tag) {
    HELIOS_ASSERT(isPowerOfTwo(m_blockAlignment), "PoolAllocator alignment must be a power of two");
    m_blockSize = alignUp<usize>(std::max<usize>(desc.blockSize, sizeof(FreeNode)), m_blockAlignment);
}

PoolAllocator::~PoolAllocator() {
    if (m_live.load(std::memory_order_relaxed) != 0) {
        HELIOS_LOG_WARN(LogMemory, "PoolAllocator destroyed with {} live blocks", m_live.load());
    }
    for (std::byte* chunk : m_chunks) alignedFree(chunk);
}

void PoolAllocator::lock() noexcept {
    if (!m_threadSafe) return;
    for (;;) {
        if (!m_lock.exchange(true, std::memory_order_acquire)) return;
        u32 spins = 0;
        while (m_lock.load(std::memory_order_relaxed)) {
            if (++spins < 64) {
                cpuPause();
            } else {
                yieldThread();
            }
        }
    }
}

void PoolAllocator::unlock() noexcept {
    if (m_threadSafe) m_lock.store(false, std::memory_order_release);
}

bool PoolAllocator::growLocked() {
    if (m_maxChunks != 0 && m_chunks.size() >= m_maxChunks) return false;
    auto* chunk = static_cast<std::byte*>(alignedAlloc(m_blockSize * m_blocksPerChunk, m_blockAlignment, m_tag));
    if (!chunk) return false;
    m_chunks.push_back(chunk);
    // Link in reverse so blocks are handed out in ascending address order.
    for (usize i = m_blocksPerChunk; i-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(chunk + i * m_blockSize);
        node->next = m_freeList;
        m_freeList = node;
    }
    return true;
}

void* PoolAllocator::allocateBlock() {
    lock();
    if (!m_freeList && !growLocked()) {
        unlock();
        return nullptr;
    }
    FreeNode* node = m_freeList;
    m_freeList = node->next;
    unlock();
    m_live.fetch_add(1, std::memory_order_relaxed);
    return node;
}

void PoolAllocator::freeBlock(void* ptr) noexcept {
    if (!ptr) return;
    HELIOS_ASSERT(owns(ptr), "PoolAllocator::freeBlock: foreign pointer");
    lock();
    auto* node = static_cast<FreeNode*>(ptr);
    node->next = m_freeList;
    m_freeList = node;
    unlock();
    m_live.fetch_sub(1, std::memory_order_relaxed);
}

void* PoolAllocator::allocate(usize size, usize alignment) {
    if (size > m_blockSize || alignment > m_blockAlignment) return nullptr;
    return allocateBlock();
}

void PoolAllocator::deallocate(void* ptr, usize) { freeBlock(ptr); }

usize PoolAllocator::capacityBlocks() const noexcept {
    auto* self = const_cast<PoolAllocator*>(this);
    self->lock();
    const usize n = m_chunks.size() * m_blocksPerChunk;
    self->unlock();
    return n;
}

bool PoolAllocator::owns(const void* ptr) const noexcept {
    auto* self = const_cast<PoolAllocator*>(this);
    const auto* p = static_cast<const std::byte*>(ptr);
    self->lock();
    bool found = false;
    for (const std::byte* chunk : m_chunks) {
        if (p >= chunk && p < chunk + m_blockSize * m_blocksPerChunk) {
            found = ((p - chunk) % static_cast<isize>(m_blockSize)) == 0;
            break;
        }
    }
    self->unlock();
    return found;
}

// ---------------------------------------------------------------------------------------------
// VirtualMemory
// ---------------------------------------------------------------------------------------------

usize VirtualMemory::pageSize() noexcept { return os::pageSize(); }
usize VirtualMemory::allocationGranularity() noexcept { return os::allocationGranularity(); }

void* VirtualMemory::reserve(usize size) noexcept {
    if (size == 0) return nullptr;
    return os::vmReserve(alignUp<usize>(size, os::allocationGranularity()));
}

bool VirtualMemory::commit(void* ptr, usize size) noexcept {
    if (!ptr || size == 0) return false;
    return os::vmCommit(ptr, alignUp<usize>(size, os::pageSize()));
}

bool VirtualMemory::decommit(void* ptr, usize size) noexcept {
    if (!ptr || size == 0) return false;
    return os::vmDecommit(ptr, alignUp<usize>(size, os::pageSize()));
}

bool VirtualMemory::release(void* ptr, usize size) noexcept {
    if (!ptr) return false;
    return os::vmRelease(ptr, alignUp<usize>(size, os::allocationGranularity()));
}

} // namespace helios
