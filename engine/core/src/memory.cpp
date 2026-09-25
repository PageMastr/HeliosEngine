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

// ---------------------------------------------------------------------------------------------
// Accounting layout (WP-0.5 contention fix).
//
// Every tracked allocation used to update one set of atomics per tag, so all threads allocating
// under the same tag (e.g. every job worker allocating ECS memory) bounced a single cache line
// between cores on every allocation and free. The counters are now sharded: each thread is
// assigned one of kShardCount shards on first use (round-robin), and each shard holds a cell per
// tag. The hot path only touches the calling thread's cell:
//   * liveBytes / liveAllocations / totalAllocations are exact: readers sum the cells.
//   * The global per-tag slot holds `published`, the sum of what the shards have flushed. A cell
//     flushes when its unpublished delta reaches +-kFlushBytes, so `published` lags the exact value
//     by less than kShardCount * kFlushBytes. Budget checks use published + the caller's own delta.
//   * The peak: each cell remembers the highest unpublished delta it reached since its last flush
//     (`high`). A flush raises the global peak to published-before-flush + high, and readers report
//     max(peak, published + sum of highs, live). A CAS happens only when the peak really rises.
//     This is exact while one thread uses a tag and an upper bound within kShardCount *
//     kFlushBytes when several do.
// ---------------------------------------------------------------------------------------------
constexpr u32 kShardCount = 16;
constexpr i64 kFlushBytes = 64 * 1024;

struct ShardCell {
    std::atomic<i64> bytes{0};     // net bytes allocated minus freed through this shard
    std::atomic<i64> allocs{0};    // allocations through this shard (monotonic)
    std::atomic<i64> frees{0};     // frees through this shard (monotonic)
    std::atomic<i64> published{0}; // part of `bytes` already added to TagSlot::published
    std::atomic<i64> high{0};      // max (bytes - published) since the last flush, >= 0
};

// One shard: a cell per tag. Aligned so two shards never share a cache line.
struct alignas(64) Shard {
    std::array<ShardCell, kMaxMemoryTags> cells{};
};

// Constant-initialized so tracking works from any static initializer; never destroyed.
struct TagSlot {
    std::atomic<i64> published{0};
    std::atomic<i64> peakBytes{0};
    std::atomic<u64> budget{0};
    std::atomic<u64> exceeded{0};
    std::atomic<bool> overBudget{false};
    std::atomic<bool> used{false};
    char name[kTagNameCapacity] = {};
};

constinit std::array<Shard, kShardCount> g_shards{};
constinit std::array<TagSlot, kMaxMemoryTags> g_tags{};
constinit std::atomic<u32> g_nextUserTag{static_cast<u32>(MemoryTag::FirstUser)};
constinit std::atomic<u32> g_nextShard{0};
// Constant-initialized thread_local (no dynamic TLS initializer; see 02 §1.1's pre-gate rules).
constinit thread_local u32 t_shard = ~0u;

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

u32 tagIndex(MemoryTag tag) noexcept {
    const u32 index = static_cast<u32>(tag);
    return index < kMaxMemoryTags ? index : 0;
}

TagSlot& slotFor(MemoryTag tag) noexcept { return g_tags[tagIndex(tag)]; }

u32 currentShard() noexcept {
    u32 shard = t_shard;
    if (HELIOS_UNLIKELY(shard == ~0u)) {
        shard = g_nextShard.fetch_add(1, std::memory_order_relaxed) % kShardCount;
        t_shard = shard;
    }
    return shard;
}

void raisePeak(TagSlot& slot, i64 candidate) noexcept {
    i64 peak = slot.peakBytes.load(std::memory_order_relaxed);
    while (candidate > peak &&
           !slot.peakBytes.compare_exchange_weak(peak, candidate, std::memory_order_relaxed)) {
    }
}

// Publishes a cell's unpublished delta to the tag slot. Safe against a concurrent flush of the same
// cell (threads sharing a shard): only the thread that wins the CAS on `published` publishes.
void flushCell(ShardCell& cell, TagSlot& slot) noexcept {
    i64 already = cell.published.load(std::memory_order_relaxed);
    const i64 now = cell.bytes.load(std::memory_order_relaxed);
    if (!cell.published.compare_exchange_strong(already, now, std::memory_order_relaxed)) return;
    const i64 high = cell.high.exchange(0, std::memory_order_relaxed);
    const i64 delta = now - already;
    const i64 before = slot.published.fetch_add(delta, std::memory_order_relaxed);
    raisePeak(slot, before + std::max(high, delta));
}

// Exact sums over the shards (readers only; O(kShardCount)).
struct TagTotals {
    i64 bytes = 0;
    i64 allocs = 0;
    i64 frees = 0;
    i64 highs = 0;
};

TagTotals sumShards(u32 index) noexcept {
    TagTotals t;
    for (const Shard& shard : g_shards) {
        const ShardCell& c = shard.cells[index];
        t.bytes += c.bytes.load(std::memory_order_relaxed);
        t.allocs += c.allocs.load(std::memory_order_relaxed);
        t.frees += c.frees.load(std::memory_order_relaxed);
        t.highs += c.high.load(std::memory_order_relaxed);
    }
    return t;
}

void checkBudgetRise(MemoryTag tag, TagSlot& slot, i64 estimate) noexcept {
    const u64 budget = slot.budget.load(std::memory_order_relaxed);
    if (budget == 0 || estimate <= static_cast<i64>(budget)) return;
    if (slot.overBudget.load(std::memory_order_relaxed) || slot.overBudget.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    slot.exceeded.fetch_add(1, std::memory_order_relaxed);
    HELIOS_LOG_WARN(LogMemory, "Memory tag '{}' over budget: {} bytes live > {} byte budget", memoryTagName(tag),
                    estimate, budget);
}

void checkBudgetFall(TagSlot& slot, i64 estimate) noexcept {
    // Only write when the flag is set, so frees under a tag within budget never dirty the slot.
    if (!slot.overBudget.load(std::memory_order_relaxed)) return;
    const u64 budget = slot.budget.load(std::memory_order_relaxed);
    if (budget == 0 || estimate <= static_cast<i64>(budget)) slot.overBudget.store(false, std::memory_order_relaxed);
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
    if (budgetBytes == 0 || sumShards(tagIndex(tag)).bytes <= static_cast<i64>(budgetBytes)) {
        slot.overBudget.store(false, std::memory_order_relaxed);
    }
}

MemoryTagStats memoryTagStats(MemoryTag tag) noexcept {
    const u32 index = tagIndex(tag);
    const TagSlot& slot = g_tags[index];
    const TagTotals totals = sumShards(index);
    MemoryTagStats stats;
    stats.tag = tag;
    stats.name = memoryTagName(tag);
    stats.liveBytes = totals.bytes;
    stats.peakBytes = std::max({slot.peakBytes.load(std::memory_order_relaxed),
                                slot.published.load(std::memory_order_relaxed) + totals.highs, totals.bytes});
    stats.liveAllocations = totals.allocs - totals.frees;
    stats.totalAllocations = static_cast<u64>(totals.allocs);
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

void trackAllocations(MemoryTag tag, usize bytes, u64 count) noexcept {
    const u32 index = tagIndex(tag);
    ShardCell& cell = g_shards[currentShard()].cells[index];
    TagSlot& slot = g_tags[index];
    const i64 live = cell.bytes.fetch_add(static_cast<i64>(bytes), std::memory_order_relaxed) + static_cast<i64>(bytes);
    if (count != 0) cell.allocs.fetch_add(static_cast<i64>(count), std::memory_order_relaxed);
    const i64 pending = live - cell.published.load(std::memory_order_relaxed);
    // Plain store: a thread sharing this shard can only lose an intermediate high, which a flush
    // or the reader's `live` term still bounds.
    if (pending > cell.high.load(std::memory_order_relaxed)) cell.high.store(pending, std::memory_order_relaxed);
    if (pending >= kFlushBytes) flushCell(cell, slot);
    checkBudgetRise(tag, slot, slot.published.load(std::memory_order_relaxed) +
                                   (live - cell.published.load(std::memory_order_relaxed)));
}

void trackDeallocations(MemoryTag tag, usize bytes, u64 count) noexcept {
    const u32 index = tagIndex(tag);
    ShardCell& cell = g_shards[currentShard()].cells[index];
    TagSlot& slot = g_tags[index];
    const i64 live = cell.bytes.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed) - static_cast<i64>(bytes);
    if (count != 0) cell.frees.fetch_add(static_cast<i64>(count), std::memory_order_relaxed);
    const i64 pending = live - cell.published.load(std::memory_order_relaxed);
    if (pending <= -kFlushBytes) flushCell(cell, slot);
    checkBudgetFall(slot, slot.published.load(std::memory_order_relaxed) +
                              (live - cell.published.load(std::memory_order_relaxed)));
}

void trackAllocation(MemoryTag tag, usize bytes) noexcept { trackAllocations(tag, bytes, 1); }

void trackDeallocation(MemoryTag tag, usize bytes) noexcept { trackDeallocations(tag, bytes, 1); }

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
