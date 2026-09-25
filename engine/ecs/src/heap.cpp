#include "helios/ecs/heap.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <mimalloc.h>

#include "helios/core/assert.h"
#include "helios/core/log.h"

namespace helios::ecs {

namespace {

mi_heap_t* asHeap(void* p) noexcept { return static_cast<mi_heap_t*>(p); }

/// Heaps are recycled, never freed: freeing a mi_heap_t that other threads have used lets a new
/// heap reuse its address while those threads still cache the old theap (SPIKES.md §1).
struct HeapPool {
    struct Entry {
        mi_heap_t* heap;
        bool shared;
        u64 owner; // thread that last owned it (its theap cache may still point at the heap)
    };
    std::mutex mutex;
    std::vector<Entry> free;

    static HeapPool& get() {
        static HeapPool* pool = new HeapPool(); // never destroyed (heaps outlive static destruction)
        return *pool;
    }
};

} // namespace

TaggedHeap::TaggedHeap(MemoryTag tag, Accounting accounting)
    : m_tag(tag), m_accounting(accounting), m_owner(cachedThreadId()) {
    HeapPool& pool = HeapPool::get();
    {
        std::lock_guard lock(pool.mutex);
        if (!pool.free.empty()) {
            const HeapPool::Entry& entry = pool.free.back();
            m_heap = entry.heap;
            // A heap that another thread used before it was pooled is shared for good: that
            // thread may still cache its theap, so destroyAll() (mi_heap_destroy) must refuse it.
            m_shared.store(entry.shared || entry.owner != m_owner, std::memory_order_relaxed);
            pool.free.pop_back();
        }
    }
    if (!m_heap) m_heap = mi_heap_new();
    HELIOS_VERIFY(m_heap != nullptr, "mi_heap_new failed");
}

TaggedHeap::~TaggedHeap() {
    if (!m_heap) return;
    flushAccounting();
    if (liveAllocations() != 0) {
        HELIOS_LOG_WARN(LogMemory, "TaggedHeap '{}' destroyed with {} live blocks ({} bytes)", memoryTagName(m_tag),
                        liveAllocations(), liveBytes());
        if (destroyAll()) {
            // Freed in bulk (thread-confined); the heap is empty and can be pooled below.
        } else {
            untrackAll(); // shared: quarantine the heap with its blocks (never reused, never freed)
            m_heap = nullptr;
            return;
        }
    }
    HeapPool& pool = HeapPool::get();
    std::lock_guard lock(pool.mutex);
    pool.free.push_back({asHeap(m_heap), m_shared.load(std::memory_order_relaxed), m_owner});
    m_heap = nullptr;
}

usize TaggedHeap::pooledHeapCount() noexcept {
    HeapPool& pool = HeapPool::get();
    std::lock_guard lock(pool.mutex);
    return pool.free.size();
}

namespace {
// Per-thread shard slot, assigned round-robin on first use so concurrently running threads (job
// workers are created together) get distinct cache lines.
std::atomic<u32> g_nextShard{0};
thread_local u32 t_shard = ~0u;
thread_local u64 t_threadId = 0;
} // namespace

u64 TaggedHeap::cachedThreadId() noexcept {
    u64 id = t_threadId;
    if (HELIOS_UNLIKELY(id == 0)) {
        id = currentThreadId();
        t_threadId = id;
    }
    return id;
}

TaggedHeap::Shard& TaggedHeap::shard() noexcept {
    u32 index = t_shard;
    if (HELIOS_UNLIKELY(index == ~0u)) {
        index = g_nextShard.fetch_add(1, std::memory_order_relaxed) % kShardCount;
        t_shard = index;
    }
    return m_shards[index];
}

i64 TaggedHeap::liveBytes() const noexcept {
    i64 total = 0;
    for (const Shard& s : m_shards) total += s.bytes.load(std::memory_order_relaxed);
    return total;
}

i64 TaggedHeap::liveAllocations() const noexcept {
    i64 total = 0;
    for (const Shard& s : m_shards) total += s.count.load(std::memory_order_relaxed);
    return total;
}

void TaggedHeap::track(usize bytes) noexcept {
    Shard& s = shard();
    s.bytes.fetch_add(static_cast<i64>(bytes), std::memory_order_relaxed);
    s.count.fetch_add(1, std::memory_order_relaxed);
    if (m_accounting == Accounting::Exact) {
        trackAllocation(m_tag, bytes);
        return;
    }
    const i64 pending = s.pendingBytes.fetch_add(static_cast<i64>(bytes), std::memory_order_relaxed) + static_cast<i64>(bytes);
    if (pending >= kBatchBytes) {
        const i64 flush = s.pendingBytes.exchange(0, std::memory_order_relaxed);
        if (flush > 0) trackAllocation(m_tag, static_cast<usize>(flush));
        if (flush < 0) trackDeallocation(m_tag, static_cast<usize>(-flush));
    }
}

void TaggedHeap::untrack(usize bytes) noexcept {
    Shard& s = shard();
    s.bytes.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed);
    s.count.fetch_sub(1, std::memory_order_relaxed);
    if (m_accounting == Accounting::Exact) {
        trackDeallocation(m_tag, bytes);
        return;
    }
    const i64 pending = s.pendingBytes.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed) - static_cast<i64>(bytes);
    if (pending <= -kBatchBytes) {
        const i64 flush = s.pendingBytes.exchange(0, std::memory_order_relaxed);
        if (flush > 0) trackAllocation(m_tag, static_cast<usize>(flush));
        if (flush < 0) trackDeallocation(m_tag, static_cast<usize>(-flush));
    }
}

void TaggedHeap::flushAccounting() noexcept {
    if (m_accounting != Accounting::Batched) return;
    for (Shard& s : m_shards) {
        const i64 flush = s.pendingBytes.exchange(0, std::memory_order_relaxed);
        if (flush > 0) trackAllocation(m_tag, static_cast<usize>(flush));
        if (flush < 0) trackDeallocation(m_tag, static_cast<usize>(-flush));
    }
}

void TaggedHeap::untrackAll() noexcept {
    flushAccounting();
    i64 bytes = 0, count = 0;
    for (Shard& s : m_shards) {
        bytes += s.bytes.exchange(0, std::memory_order_relaxed);
        count += s.count.exchange(0, std::memory_order_relaxed);
    }
    if (m_accounting == Accounting::Batched) {
        if (bytes > 0) trackDeallocation(m_tag, static_cast<usize>(bytes));
        return;
    }
    if (count > 0) {
        trackDeallocation(m_tag, static_cast<usize>(bytes));
        for (i64 i = 1; i < count; ++i) trackDeallocation(m_tag, 0); // one count per block
    }
}

void* TaggedHeap::allocate(usize size, usize alignment) noexcept {
    if (size == 0 || !isPowerOfTwo(alignment)) return nullptr;
    noteThread();
    void* p = alignment <= kDefaultAlignment ? mi_heap_malloc(asHeap(m_heap), size)
                                             : mi_heap_malloc_aligned(asHeap(m_heap), size, alignment);
    if (p) track(mi_usable_size(p));
    return p;
}

void* TaggedHeap::allocateZeroed(usize size, usize alignment) noexcept {
    if (size == 0 || !isPowerOfTwo(alignment)) return nullptr;
    noteThread();
    void* p = alignment <= kDefaultAlignment ? mi_heap_zalloc(asHeap(m_heap), size)
                                             : mi_heap_zalloc_aligned(asHeap(m_heap), size, alignment);
    if (p) track(mi_usable_size(p));
    return p;
}

void* TaggedHeap::reallocate(void* ptr, usize size) noexcept {
    if (!ptr) return allocate(size);
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    HELIOS_ASSERT(owns(ptr), "TaggedHeap::reallocate: block from another heap");
    noteThread();
    const usize oldSize = mi_usable_size(ptr);
    void* p = mi_heap_realloc(asHeap(m_heap), ptr, size);
    if (!p) return nullptr; // the old block is untouched
    untrack(oldSize);
    track(mi_usable_size(p));
    return p;
}

void TaggedHeap::free(void* ptr) noexcept {
    if (!ptr) return;
    HELIOS_ASSERT(owns(ptr), "TaggedHeap::free: block from another heap");
    untrack(mi_usable_size(ptr));
    mi_free(ptr);
}

usize TaggedHeap::usableSize(const void* ptr) noexcept { return ptr ? mi_usable_size(ptr) : 0; }

bool TaggedHeap::owns(const void* ptr) const noexcept {
    return ptr && m_heap && mi_heap_contains(asHeap(m_heap), ptr);
}

bool TaggedHeap::destroyAll() noexcept {
    if (!m_heap) return false;
    if (isShared() || cachedThreadId() != m_owner) return false;
    untrackAll();
    // Safe: only this thread ever cached a theap of the heap, and mi_heap_destroy clears the
    // calling thread's cache. The replacement heap may land at the same address.
    mi_heap_destroy(asHeap(m_heap));
    m_heap = mi_heap_new();
    HELIOS_VERIFY(m_heap != nullptr, "mi_heap_new failed");
    return true;
}

MemoryTag ecsMemoryTag() noexcept {
    static const MemoryTag tag = registerMemoryTag("ECS");
    return tag;
}

TaggedHeap& ecsHeap() noexcept {
    static TaggedHeap* heap = new TaggedHeap(ecsMemoryTag());
    return *heap;
}

} // namespace helios::ecs
