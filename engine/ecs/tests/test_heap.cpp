// TaggedHeap (mimalloc v3 heaps behind memory tags) and the Phase 0 heap/thread-affinity spike:
// blocks allocated by one job-system worker and freed by another, jobs that migrate between
// workers (stealing, helping waits), and workers that exit while their blocks are still live.

#include <doctest/doctest.h>

#include <mimalloc.h>

#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/core/thread.h"
#include "helios/ecs/heap.h"

using namespace helios;
using namespace helios::ecs;

namespace {
MemoryTag testTag(const char* name) { return registerMemoryTag(name); }
} // namespace

TEST_CASE("ecs heap: allocation, accounting and ownership") {
    const MemoryTag tag = testTag("EcsTest.Heap.Basic");
    const MemoryTagStats before = memoryTagStats(tag);
    TaggedHeap heap(tag);
    void* a = heap.allocate(100);
    void* b = heap.allocate(64, 64);
    void* z = heap.allocateZeroed(256);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(z);
    CHECK(reinterpret_cast<uptr>(b) % 64 == 0);
    for (int i = 0; i < 256; ++i) CHECK(static_cast<unsigned char*>(z)[i] == 0);
    CHECK(heap.owns(a));
    CHECK(mi_heap_of(a) == static_cast<mi_heap_t*>(heap.nativeHeap()));
    CHECK(heap.liveAllocations() == 3);
    const i64 live = heap.liveBytes();
    CHECK(live >= 100 + 64 + 256);
    CHECK(memoryTagStats(tag).liveBytes - before.liveBytes == live);
    CHECK(heap.allocate(0) == nullptr);
    CHECK(heap.allocate(8, 3) == nullptr); // not a power of two

    std::memset(a, 0x5A, 100);
    void* grown = heap.reallocate(a, 10'000);
    REQUIRE(grown);
    CHECK(static_cast<unsigned char*>(grown)[99] == 0x5A);
    CHECK(heap.liveAllocations() == 3);
    CHECK(heap.reallocate(grown, 0) == nullptr);
    heap.free(b);
    heap.free(z);
    heap.free(nullptr);
    CHECK(heap.liveBytes() == 0);
    CHECK(heap.liveAllocations() == 0);
    CHECK(memoryTagStats(tag).liveBytes == before.liveBytes);
    CHECK(memoryTagStats(tag).liveAllocations == before.liveAllocations);
}

TEST_CASE("ecs heap: destroyAll frees a thread-confined heap at once and the heap stays usable") {
    const MemoryTag tag = testTag("EcsTest.Heap.Destroy");
    const i64 before = memoryTagStats(tag).liveBytes;
    TaggedHeap heap(tag);
    for (int i = 0; i < 1000; ++i) CHECK(heap.allocate(static_cast<usize>(16 + i % 300)) != nullptr);
    CHECK(memoryTagStats(tag).liveBytes > before);
    if (heap.isShared()) return; // a pooled heap that other threads used earlier (tested below)
    CHECK(heap.destroyAll());
    CHECK(heap.liveBytes() == 0);
    CHECK(memoryTagStats(tag).liveBytes == before);
    CHECK(memoryTagStats(tag).liveAllocations == 0);
    void* p = heap.allocate(32);
    CHECK(heap.owns(p));
    heap.free(p);
}

TEST_CASE("ecs heap spike: heaps used by several workers are recycled, never freed") {
    // Regression for the mimalloc 3.5.3 hazard (SPIKES.md §1): freeing a heap that live workers
    // cached, then creating a heap at the same address, made the workers allocate from a dead
    // theap. TaggedHeap recycles heaps through a pool, so every round below must stay correct.
    const MemoryTag tag = testTag("EcsTest.Heap.Recycle");
    jobs::JobSystem js({.workerCount = 4});
    std::atomic<int> foreign{0};
    for (int round = 0; round < 40; ++round) {
        TaggedHeap heap(tag);
        {
            // A thread that used the heap and exited is what made 3.5.3 hand out the dead theap.
            Thread early("heap-early", [&] { heap.free(heap.allocate(64)); });
        }
        std::vector<void*> blocks(512, nullptr);
        js.parallelFor(0, blocks.size(), 16, [&](u64 i) {
            blocks[i] = heap.allocate(64 + i % 256);
            if (!heap.owns(blocks[i])) foreign.fetch_add(1);
        });
        for (void* b : blocks) heap.free(b);
        CHECK_FALSE(heap.destroyAll()); // shared: bulk free refused
    }
    CHECK(foreign.load() == 0);
    CHECK(TaggedHeap::pooledHeapCount() >= 1);
}

TEST_CASE("ecs heap: a shared heap destroyed with live blocks is quarantined") {
    const MemoryTag tag = testTag("EcsTest.Heap.Quarantine");
    const i64 before = memoryTagStats(tag).liveBytes;
    void* survivor = nullptr;
    {
        TaggedHeap heap(tag);
        Thread t("heap-other", [&] { survivor = heap.allocate(100); });
        t.join();
        CHECK(heap.isShared());
    } // warns; the block stays valid and the tag accounting is released
    REQUIRE(survivor);
    static_cast<unsigned char*>(survivor)[99] = 1; // still writable memory
    CHECK(memoryTagStats(tag).liveBytes == before);
}

TEST_CASE("ecs heap spike: cross-worker alloc/free with job migration is safe") {
    // One shared heap; 4 workers + the main thread. Every job allocates blocks, hands them to a
    // job that another worker is likely to steal, and that job frees them. Waiting jobs help, so
    // allocation and free sites interleave arbitrarily across threads.
    const MemoryTag tag = testTag("EcsTest.Heap.Spike");
    const i64 before = memoryTagStats(tag).liveBytes;
    TaggedHeap heap(tag);
    jobs::JobSystem js({.workerCount = 4});
    constexpr int kProducers = 256;
    constexpr int kBlocks = 64;
    std::atomic<u64> threadMask{0};
    std::atomic<int> mismatches{0};
    jobs::Counter all;
    for (int p = 0; p < kProducers; ++p) {
        js.run(
            [&, p] {
                const i32 w = js.currentWorkerIndex();
                threadMask.fetch_or(1ull << (w < 0 ? 63 : w), std::memory_order_relaxed);
                auto blocks = std::make_shared<std::vector<void*>>();
                for (int i = 0; i < kBlocks; ++i) {
                    const usize size = static_cast<usize>(8 + ((p * 131 + i * 17) % 2048));
                    auto* mem = static_cast<unsigned char*>(heap.allocate(size));
                    std::memset(mem, p & 0xFF, size);
                    blocks->push_back(mem);
                }
                jobs::Counter freed;
                js.run(
                    [&, blocks, p] {
                        for (void* b : *blocks) {
                            if (*static_cast<unsigned char*>(b) != (p & 0xFF)) mismatches.fetch_add(1);
                            if (mi_heap_of(b) != static_cast<mi_heap_t*>(heap.nativeHeap())) mismatches.fetch_add(1);
                            heap.free(b);
                        }
                    },
                    &freed);
                js.wait(freed); // helping wait: this thread may run unrelated jobs meanwhile
            },
            &all);
    }
    js.wait(all);
    CHECK(mismatches.load() == 0);
    CHECK(std::popcount(threadMask.load()) >= 2);
    CHECK(heap.liveAllocations() == 0);
    CHECK(memoryTagStats(tag).liveBytes == before);
}

TEST_CASE("ecs heap spike: blocks outlive the worker threads that allocated them") {
    const MemoryTag tag = testTag("EcsTest.Heap.ThreadExit");
    TaggedHeap heap(tag);
    std::vector<void*> blocks(4096, nullptr);
    {
        jobs::JobSystem js({.workerCount = 3});
        js.parallelFor(0, blocks.size(), 64, [&](u64 i) {
            blocks[i] = heap.allocate(static_cast<usize>(24 + i % 500));
            std::memset(blocks[i], static_cast<int>(i & 0xFF), 24);
        });
    } // workers exit: their per-thread theaps are abandoned, the pages stay owned by the heap
    for (usize i = 0; i < blocks.size(); ++i) {
        CHECK(static_cast<unsigned char*>(blocks[i])[23] == static_cast<unsigned char>(i & 0xFF));
        CHECK(heap.owns(blocks[i]));
    }
    // New threads can keep allocating from (and freeing into) the same heap.
    {
        Thread t("heap-spike", [&] {
            for (usize i = 0; i < blocks.size(); i += 2) heap.free(blocks[i]);
        });
    }
    for (usize i = 1; i < blocks.size(); i += 2) heap.free(blocks[i]);
    CHECK(heap.liveAllocations() == 0);
    CHECK(heap.liveBytes() == 0);
}

TEST_CASE("ecs heap: batched tag accounting converges on flush") {
    const MemoryTag tag = testTag("EcsTest.Heap.Batched");
    const i64 before = memoryTagStats(tag).liveBytes;
    TaggedHeap heap(tag, TaggedHeap::Accounting::Batched);
    jobs::JobSystem js({.workerCount = 4});
    std::vector<void*> blocks(20000, nullptr);
    js.parallelFor(0, blocks.size(), 256, [&](u64 i) { blocks[i] = heap.allocate(32 + i % 200); });
    const i64 exact = heap.liveBytes();
    CHECK(heap.liveAllocations() == 20000);
    // Before the flush the tag lags by less than one batch per shard.
    CHECK(memoryTagStats(tag).liveBytes - before <= exact);
    CHECK(exact - (memoryTagStats(tag).liveBytes - before) < 32 * TaggedHeap::kBatchBytes);
    heap.flushAccounting();
    CHECK(memoryTagStats(tag).liveBytes - before == exact);
    js.parallelFor(0, blocks.size(), 256, [&](u64 i) { heap.free(blocks[i]); });
    heap.flushAccounting();
    CHECK(heap.liveBytes() == 0);
    CHECK(memoryTagStats(tag).liveBytes == before);
}

TEST_CASE("ecs heap: the process ECS heap and allocator adapter") {
    TaggedHeap& heap = ecsHeap();
    CHECK(&heap == &ecsHeap());
    CHECK(heap.tag() == ecsMemoryTag());
    CHECK(memoryTagName(ecsMemoryTag()) == "ECS");
    TaggedHeapAllocator alloc(heap);
    std::vector<int, StdAllocator<int>> v{StdAllocator<int>(alloc)};
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    CHECK(heap.owns(v.data()));
}
