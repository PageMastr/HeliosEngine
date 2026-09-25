#pragma once
// Tagged memory tracking, allocators and virtual memory.
//
// * MemoryTag: every tracked allocation is attributed to a tag. Per tag the registry keeps live
//   bytes, peak bytes, live/total allocation counts and an optional budget; crossing the budget
//   logs a warning (once per crossing) and bumps a counter that tests/telemetry can check.
// * alignedAlloc/alignedFree: tracked, aligned heap allocation backed by mimalloc (ADR-011).
// * Allocator: minimal polymorphic interface (plus StdAllocator adapter for std containers).
// * LinearAllocator (single-thread bump), FrameAllocator (N-buffered, lock-free bump, reset per
//   frame/tick), PoolAllocator (fixed-size blocks, optional locking).
// * VirtualMemory: reserve/commit/decommit/release address space (VirtualAlloc / mmap).
//
// Threading: tag registry, tracking and alignedAlloc/alignedFree are thread-safe and lock-free on
// the allocation path; tag counters are sharded per thread so concurrent allocation under one tag
// scales (see trackAllocation). Allocator thread-safety is documented per class.

#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "helios/core/platform.h"
#include "helios/core/types.h"

namespace helios {

/// Built-in tags; modules register their own with registerMemoryTag().
enum class MemoryTag : u16 {
    Unknown = 0,
    Core,
    Containers,
    Strings,
    Jobs,
    FileSystem,
    Logging,
    Temp,
    Frame,
    Pool,
    Virtual,
    FirstUser = 32,
};

inline constexpr u32 kMaxMemoryTags = 256;
inline constexpr usize kDefaultAlignment = alignof(std::max_align_t);
/// Largest alignment alignedAlloc() accepts.
inline constexpr usize kMaxAlignedAllocAlignment = usize(1) << 31;

/// Registers (or finds, by exact name) a tag. Returns MemoryTag::Unknown if the table is full.
MemoryTag registerMemoryTag(std::string_view name, u64 budgetBytes = 0);
std::string_view memoryTagName(MemoryTag tag) noexcept;
/// Sets a soft budget in bytes (0 = unlimited). Exceeding it logs a warning; allocation proceeds.
void setMemoryBudget(MemoryTag tag, u64 budgetBytes) noexcept;

struct MemoryTagStats {
    MemoryTag tag = MemoryTag::Unknown;
    std::string_view name;
    i64 liveBytes = 0;
    i64 peakBytes = 0;
    i64 liveAllocations = 0;
    u64 totalAllocations = 0;
    u64 budgetBytes = 0;
    u64 budgetExceededCount = 0; ///< Number of times live bytes crossed the budget.
};

MemoryTagStats memoryTagStats(MemoryTag tag) noexcept;
/// Stats for every built-in and registered tag.
std::vector<MemoryTagStats> allMemoryTagStats();

/// Manual accounting for memory obtained elsewhere (GPU heaps, third-party allocators, ...).
/// One allocation (or free) of `bytes`. Thread-safe and lock-free.
///
/// Scaling: counters are sharded per thread, so threads allocating under the same tag do not
/// contend on one cache line (≤ 25 ns per allocate+free pair per thread, flat from 1 to 8
/// threads; `core_memory_bench`). Live bytes and allocation counts read by memoryTagStats() are
/// exact. The peak and budget crossings are exact while one thread uses a tag; when several do,
/// they are evaluated on a per-tag estimate that lags by less than 16 × 64 KiB.
void trackAllocation(MemoryTag tag, usize bytes) noexcept;
void trackDeallocation(MemoryTag tag, usize bytes) noexcept;
/// Batched accounting: `count` allocations (or frees) totalling `bytes` in one call. Allocators
/// that hand out many blocks at once (pools, arenas, a heap flushing a thread-local tally) use it
/// so live/total allocation counts stay exact without one call per block. `count` may be 0 to
/// adjust bytes only (e.g. a block that grew in place).
void trackAllocations(MemoryTag tag, usize bytes, u64 count) noexcept;
void trackDeallocations(MemoryTag tag, usize bytes, u64 count) noexcept;

/// Tracked aligned allocation (alignment: power of two <= kMaxAlignedAllocAlignment). Returns
/// nullptr on failure, size 0 or an invalid alignment.
void* alignedAlloc(usize size, usize alignment = kDefaultAlignment, MemoryTag tag = MemoryTag::Unknown) noexcept;
/// Frees memory from alignedAlloc (nullptr is ignored). Tag and size are recovered from a header.
void alignedFree(void* ptr) noexcept;
/// Requested size of a live alignedAlloc block.
usize alignedAllocSize(const void* ptr) noexcept;
/// Tag of a live alignedAlloc block.
MemoryTag alignedAllocTag(const void* ptr) noexcept;

/// Polymorphic allocator interface. `size` passed to deallocate must match the allocation.
class Allocator {
public:
    virtual ~Allocator() = default;
    virtual void* allocate(usize size, usize alignment = kDefaultAlignment) = 0;
    virtual void deallocate(void* ptr, usize size) = 0;

    template <class T, class... Args>
    T* create(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? ::new (p) T(std::forward<Args>(args)...) : nullptr;
    }
    template <class T>
    void destroy(T* obj) {
        if (!obj) return;
        obj->~T();
        deallocate(obj, sizeof(T));
    }
};

/// Heap allocator (alignedAlloc) attributing memory to one tag. Thread-safe.
class HeapAllocator final : public Allocator {
public:
    explicit HeapAllocator(MemoryTag tag = MemoryTag::Unknown) noexcept : m_tag(tag) {}
    void* allocate(usize size, usize alignment = kDefaultAlignment) override {
        return alignedAlloc(size, alignment, m_tag);
    }
    void deallocate(void* ptr, usize) override { alignedFree(ptr); }
    MemoryTag tag() const noexcept { return m_tag; }

private:
    MemoryTag m_tag;
};

/// Process-wide heap allocator (MemoryTag::Unknown).
Allocator& defaultAllocator() noexcept;

/// std-compatible allocator adapter over an Allocator (e.g. std::vector<int, StdAllocator<int>>).
template <class T>
class StdAllocator {
public:
    using value_type = T;
    StdAllocator() noexcept : m_alloc(&defaultAllocator()) {}
    explicit StdAllocator(Allocator& alloc) noexcept : m_alloc(&alloc) {}
    template <class U>
    StdAllocator(const StdAllocator<U>& o) noexcept : m_alloc(o.allocator()) {}

    T* allocate(usize n) {
        void* p = m_alloc->allocate(n * sizeof(T), alignof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, usize n) noexcept { m_alloc->deallocate(p, n * sizeof(T)); }
    Allocator* allocator() const noexcept { return m_alloc; }

    template <class U>
    bool operator==(const StdAllocator<U>& o) const noexcept {
        return m_alloc == o.allocator();
    }

private:
    Allocator* m_alloc;
};

/// Bump allocator over one contiguous buffer. deallocate() is a no-op; memory is reclaimed with
/// reset() or rewind(marker). NOT thread-safe.
class LinearAllocator final : public Allocator {
public:
    using Marker = usize;

    /// Owns a heap buffer of `capacity` bytes attributed to `tag`.
    explicit LinearAllocator(usize capacity, MemoryTag tag = MemoryTag::Temp);
    /// Uses caller-provided memory (not owned).
    LinearAllocator(void* buffer, usize capacity) noexcept;
    ~LinearAllocator() override;
    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    /// Returns nullptr when the buffer is exhausted or `alignment` is not a power of two (0 = 1).
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void*, usize) override {}

    template <class T>
        requires std::is_trivially_destructible_v<T>
    T* allocArray(usize count) {
        T* p = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        if (p) {
            for (usize i = 0; i < count; ++i) ::new (static_cast<void*>(p + i)) T();
        }
        return p;
    }

    Marker marker() const noexcept { return m_offset; }
    void rewind(Marker marker) noexcept;
    void reset() noexcept { m_offset = 0; }

    usize used() const noexcept { return m_offset; }
    usize capacity() const noexcept { return m_capacity; }
    usize remaining() const noexcept { return m_capacity - m_offset; }
    usize peak() const noexcept { return m_peak; }

private:
    std::byte* m_buffer = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
    usize m_peak = 0;
    bool m_owned = false;
};

/// N-buffered per-frame (or per-tick) bump allocator. Memory allocated during frame F stays valid
/// until beginFrame() has been called `frameCount` more times, so with frameCount = 2 data may be
/// handed from simulation frame N to rendering of frame N.
///
/// Threading: allocate() is lock-free and may be called concurrently from any thread (jobs).
/// beginFrame() must be called by one thread while no allocate() is in flight (between frames).
/// Only trivially destructible objects may live here; nothing is destroyed on reset.
class FrameAllocator final : public Allocator {
public:
    explicit FrameAllocator(usize bytesPerFrame, u32 frameCount = 2, MemoryTag tag = MemoryTag::Frame);
    ~FrameAllocator() override;
    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;

    /// Returns nullptr (and counts an overflow) when the current frame's buffer is exhausted;
    /// nullptr for an `alignment` that is not a power of two (0 = 1).
    void* allocate(usize size, usize alignment = kDefaultAlignment) noexcept override;
    void deallocate(void*, usize) noexcept override {}

    template <class T>
        requires std::is_trivially_destructible_v<T>
    T* allocArray(usize count) noexcept {
        T* p = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        if (p) {
            for (usize i = 0; i < count; ++i) ::new (static_cast<void*>(p + i)) T();
        }
        return p;
    }
    template <class T, class... Args>
        requires std::is_trivially_destructible_v<T>
    T* make(Args&&... args) noexcept {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? ::new (p) T(std::forward<Args>(args)...) : nullptr;
    }

    /// Advances to the next buffer and resets it.
    void beginFrame() noexcept;

    u64 frameIndex() const noexcept { return m_frameIndex; }
    u32 frameCount() const noexcept { return m_frameCount; }
    usize capacityPerFrame() const noexcept { return m_capacity; }
    usize usedThisFrame() const noexcept;
    u64 overflowCount() const noexcept { return m_overflows.load(std::memory_order_relaxed); }

private:
    std::byte* m_memory = nullptr;
    usize m_capacity = 0;
    u32 m_frameCount = 0;
    u32 m_current = 0;
    u64 m_frameIndex = 0;
    MemoryTag m_tag;
    std::atomic<usize> m_offset{0};
    std::atomic<u64> m_overflows{0};
};

/// Fixed-size block allocator. Grows by chunks of `blocksPerChunk` (up to `maxChunks`, 0 = no
/// limit). Blocks are recycled through an intrusive free list. Thread-safe only if constructed
/// with threadSafe = true (a SpinLock-style lock around the free list).
class PoolAllocator final : public Allocator {
public:
    struct Desc {
        usize blockSize = 64;
        usize blockAlignment = kDefaultAlignment;
        usize blocksPerChunk = 256;
        usize maxChunks = 0;
        bool threadSafe = false;
        MemoryTag tag = MemoryTag::Pool;
    };

    explicit PoolAllocator(const Desc& desc);
    ~PoolAllocator() override;
    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    /// `size` must be <= blockSize and `alignment` <= blockAlignment; returns nullptr otherwise or
    /// when maxChunks is reached.
    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* ptr, usize size) override;

    void* allocateBlock();
    void freeBlock(void* ptr) noexcept;

    usize blockSize() const noexcept { return m_blockSize; }
    usize liveBlocks() const noexcept { return m_live.load(std::memory_order_relaxed); }
    usize capacityBlocks() const noexcept;
    /// True if `ptr` lies inside one of this pool's chunks.
    bool owns(const void* ptr) const noexcept;

private:
    struct FreeNode {
        FreeNode* next;
    };
    bool growLocked();
    void lock() noexcept;
    void unlock() noexcept;

    usize m_blockSize;
    usize m_blockAlignment;
    usize m_blocksPerChunk;
    usize m_maxChunks;
    bool m_threadSafe;
    MemoryTag m_tag;
    std::atomic<bool> m_lock{false};
    FreeNode* m_freeList = nullptr;
    std::vector<std::byte*> m_chunks;
    std::atomic<usize> m_live{0};
};

/// Direct control over address space (VirtualAlloc / mmap). All sizes are rounded up to pages.
/// Reserved-but-uncommitted memory costs no physical memory. Thread-safe (OS calls). Not tracked
/// automatically: arenas built on it report committed bytes with trackAllocation(MemoryTag::Virtual
/// or their own tag, ...).
struct VirtualMemory {
    static usize pageSize() noexcept;
    /// Granularity of reservation base addresses (64 KiB on Windows, the page size elsewhere).
    static usize allocationGranularity() noexcept;
    /// Reserves address space without backing it. Returns nullptr on failure.
    static void* reserve(usize size) noexcept;
    /// Backs [ptr, ptr+size) with zeroed read/write pages (page-aligned range inside a reservation).
    static bool commit(void* ptr, usize size) noexcept;
    /// Releases the physical pages of the range but keeps the reservation.
    static bool decommit(void* ptr, usize size) noexcept;
    /// Releases a whole reservation (`size` must be the reserved size).
    static bool release(void* ptr, usize size) noexcept;
};

} // namespace helios
