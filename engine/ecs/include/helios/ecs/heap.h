#pragma once
// TaggedHeap: a mimalloc heap attributed to a Helios MemoryTag (ADR-011; Phase 0 spike, see
// engine/ecs/SPIKES.md §1).
//
// mimalloc v3 (vendored 3.5.3) split the old thread-bound `mi_heap_t` into:
//   * mi_heap_t  — first-class heap: allocate from ANY thread, free from ANY thread. Internally
//                  each thread lazily gets its own "theap" for the heap (versioned dynamic TLS
//                  slots, so the number of heaps is not limited by OS TLS keys).
//   * mi_theap_t — thread-local view; must only be used by the thread that obtained it.
// So one heap per tag group can serve every job-system worker, jobs that are stolen or migrate
// between workers, and threads that later exit (their pages are abandoned and reclaimed; live
// blocks stay valid).
//
// The spike also found a hazard in 3.5.3: every thread caches its last-used theap and validates the
// cache only by comparing heap *pointers*. Destroying/deleting a heap that another live thread has
// used, then creating a heap that lands at the same address (the usual case), makes that thread
// allocate from the dead heap's zombie theap (reproduced 200/200 by ecs_bench --spikes-only). Rules:
//   1. Heaps are per *tag group* (ECS, physics, script, ...), not per object.
//   2. Never free a mi_heap_t that other threads have used: TaggedHeap recycles its heap through a
//      process-wide pool instead of mi_heap_destroy/mi_heap_delete (pooled heaps are never freed,
//      so their addresses are never reused).
//   3. Bulk free (destroyAll) is only allowed for heaps confined to one thread.
//   4. Never cache a mi_theap_t* (mi_heap_theap) across a job boundary or a JobSystem::wait(): with
//      Phase-2 fibers the job may resume on another thread.
//
// Accounting uses mi_usable_size() so free() needs no size and no header (the usable size may
// round the request up to mimalloc's size class, which is what the process actually pays for).
// The heap's own counters are sharded per thread (no shared cache line on the hot path). The
// MemoryTag counters in core are shared atomics, which the spike measured to collapse under 4
// concurrent workers, so the tag can be fed two ways:
//   * Accounting::Exact   — every allocation updates the tag (exact counts; contended);
//   * Accounting::Batched — each thread shard forwards its byte delta to the tag once it exceeds
//                           kBatchBytes (tag bytes lag by < kBatchBytes per thread; the tag's
//                           allocation *counts* count flushes, not blocks). flushAccounting()
//                           makes the tag exact again at a quiescent point (e.g. end of tick).
//
// Threading: allocate/reallocate/free/usableSize/owns are thread-safe; construction, destroyAll()
// and destruction require that no allocation from this heap is in flight.

#include <atomic>

#include "helios/core/memory.h"
#include "helios/core/thread.h"
#include "helios/core/types.h"

#if defined(HELIOS_COMPILER_MSVC)
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignment specifier (intended: false sharing)
#endif

namespace helios::ecs {

class TaggedHeap {
public:
    enum class Accounting : u8 { Exact, Batched };
    static constexpr i64 kBatchBytes = 256 * 1024;

    /// Takes a heap from the process-wide pool (or creates one). `tag` receives the accounting.
    explicit TaggedHeap(MemoryTag tag, Accounting accounting = Accounting::Exact);
    /// Returns the heap to the pool. Blocks still live are reported; a thread-confined heap frees
    /// them in bulk, a shared heap is quarantined with them (they stay valid, never reused).
    ~TaggedHeap();
    TaggedHeap(const TaggedHeap&) = delete;
    TaggedHeap& operator=(const TaggedHeap&) = delete;

    /// nullptr for size 0 or on failure. `alignment` must be a power of two.
    void* allocate(usize size, usize alignment = kDefaultAlignment) noexcept;
    void* allocateZeroed(usize size, usize alignment = kDefaultAlignment) noexcept;
    /// realloc semantics (nullptr ptr = allocate, size 0 = free and return nullptr).
    void* reallocate(void* ptr, usize size) noexcept;
    /// Frees a block from this heap (nullptr is ignored). Any thread.
    void free(void* ptr) noexcept;

    /// Usable size of a block from any mimalloc heap.
    static usize usableSize(const void* ptr) noexcept;
    /// True if `ptr` is a block inside this heap's pages.
    bool owns(const void* ptr) const noexcept;

    /// Frees every block at once (arena-style teardown). Only possible while the heap has been
    /// used by a single thread (the caller); returns false (and frees nothing) otherwise.
    bool destroyAll() noexcept;
    /// True once a thread other than the creator allocated from this heap (or the pooled heap was
    /// shared before).
    bool isShared() const noexcept { return m_shared.load(std::memory_order_relaxed); }

    MemoryTag tag() const noexcept { return m_tag; }
    Accounting accounting() const noexcept { return m_accounting; }
    /// Live bytes allocated through this heap (usable sizes; sums the thread shards).
    i64 liveBytes() const noexcept;
    i64 liveAllocations() const noexcept;
    /// Batched mode: forwards every shard's pending delta to the tag. Call while no allocation from
    /// this heap is in flight for exact tag totals. No-op in Exact mode.
    void flushAccounting() noexcept;
    /// The underlying mi_heap_t* (as void* to keep mimalloc.h out of this header).
    void* nativeHeap() const noexcept { return m_heap; }

    /// Heaps currently waiting in the process-wide pool (diagnostics/tests).
    static usize pooledHeapCount() noexcept;

private:
    void track(usize bytes) noexcept;
    void untrack(usize bytes) noexcept;
    void noteThread() noexcept {
        // The thread id is only consulted until the heap is known to be shared.
        if (!m_shared.load(std::memory_order_relaxed) && cachedThreadId() != m_owner) {
            m_shared.store(true, std::memory_order_relaxed);
        }
    }
    /// helios::currentThreadId() cached per thread (it is a gettid syscall on Linux).
    static u64 cachedThreadId() noexcept;
    void untrackAll() noexcept;

    struct alignas(HELIOS_CACHE_LINE_SIZE) Shard {
        std::atomic<i64> bytes{0};
        std::atomic<i64> count{0};
        std::atomic<i64> pendingBytes{0}; // Batched: not yet forwarded to the tag
    };
    static constexpr u32 kShardCount = 32;
    Shard& shard() noexcept;

    void* m_heap = nullptr;
    MemoryTag m_tag;
    Accounting m_accounting;
    ThreadId m_owner;
    std::atomic<bool> m_shared{false};
    Shard m_shards[kShardCount];
};

/// Allocator adapter (helios::Allocator) over a TaggedHeap, e.g. for StdAllocator<T>.
class TaggedHeapAllocator final : public Allocator {
public:
    explicit TaggedHeapAllocator(TaggedHeap& heap) noexcept : m_heap(&heap) {}
    void* allocate(usize size, usize alignment = kDefaultAlignment) override {
        return m_heap->allocate(size, alignment);
    }
    void deallocate(void* ptr, usize) override { m_heap->free(ptr); }

private:
    TaggedHeap* m_heap;
};

/// Memory tag "ECS" (registered on first use).
MemoryTag ecsMemoryTag() noexcept;
/// Process-wide heap behind the flecs OS API and helios::ecs containers (tag "ECS"). Created on
/// first use and intentionally never destroyed (flecs may free during static destruction).
TaggedHeap& ecsHeap() noexcept;

} // namespace helios::ecs

#if defined(HELIOS_COMPILER_MSVC)
#pragma warning(pop)
#endif
