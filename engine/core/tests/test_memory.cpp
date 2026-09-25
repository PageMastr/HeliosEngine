#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/memory.h"

using namespace helios;

TEST_CASE("memory: tag registry") {
    const MemoryTag t = registerMemoryTag("TestTagRegistry");
    CHECK(static_cast<u32>(t) >= static_cast<u32>(MemoryTag::FirstUser));
    CHECK(registerMemoryTag("TestTagRegistry") == t);
    CHECK(memoryTagName(t) == "TestTagRegistry");
    CHECK(memoryTagName(MemoryTag::Jobs) == "Jobs");
    const auto all = allMemoryTagStats();
    CHECK(std::any_of(all.begin(), all.end(), [&](const MemoryTagStats& s) { return s.tag == t; }));
}

TEST_CASE("memory: tracked aligned allocation") {
    const MemoryTag tag = registerMemoryTag("TestAlignedAlloc");
    const MemoryTagStats before = memoryTagStats(tag);
    for (usize alignment : {usize(1), usize(8), usize(16), usize(64), usize(256), usize(4096)}) {
        void* p = alignedAlloc(100, alignment, tag);
        REQUIRE(p != nullptr);
        CHECK(isAligned(p, std::max<usize>(alignment, 16)));
        CHECK(alignedAllocSize(p) == 100);
        CHECK(alignedAllocTag(p) == tag);
        std::memset(p, 0xAB, 100);
        alignedFree(p);
    }
    void* a = alignedAlloc(1000, 32, tag);
    void* b = alignedAlloc(24, 32, tag);
    const MemoryTagStats mid = memoryTagStats(tag);
    CHECK(mid.liveBytes - before.liveBytes == 1024);
    CHECK(mid.liveAllocations - before.liveAllocations == 2);
    CHECK(mid.totalAllocations - before.totalAllocations == 8);
    CHECK(mid.peakBytes >= 1024);
    alignedFree(a);
    alignedFree(b);
    alignedFree(nullptr);
    const MemoryTagStats after = memoryTagStats(tag);
    CHECK(after.liveBytes == before.liveBytes);
    CHECK(after.liveAllocations == before.liveAllocations);
    CHECK(alignedAlloc(0, 16, tag) == nullptr);
    CHECK(alignedAlloc(16, 24, tag) == nullptr); // non power-of-two alignment
    // Regression: the header keeps the alignment prefix in 32 bits; larger alignments used to be
    // truncated there (alignedFree would then free the wrong address).
    CHECK(alignedAlloc(16, kMaxAlignedAllocAlignment << 1, tag) == nullptr);
    CHECK(memoryTagStats(tag).liveAllocations == before.liveAllocations);
}

TEST_CASE("memory: budgets warn once per crossing") {
    const auto saved = log::level();
    log::setLevel(log::Level::Error); // the expected warning is not interesting in test output
    const MemoryTag tag = registerMemoryTag("TestBudget", 1000);
    CHECK(memoryTagStats(tag).budgetBytes == 1000);
    void* a = alignedAlloc(600, 16, tag);
    CHECK(memoryTagStats(tag).budgetExceededCount == 0);
    void* b = alignedAlloc(600, 16, tag);
    CHECK(memoryTagStats(tag).budgetExceededCount == 1);
    void* c = alignedAlloc(10, 16, tag); // still over: no new warning
    CHECK(memoryTagStats(tag).budgetExceededCount == 1);
    alignedFree(b);
    alignedFree(c);
    void* d = alignedAlloc(600, 16, tag); // crosses again
    CHECK(memoryTagStats(tag).budgetExceededCount == 2);
    alignedFree(a);
    alignedFree(d);
    setMemoryBudget(tag, 0);
    log::setLevel(saved);
}

TEST_CASE("memory: heap allocator and std adapter") {
    HeapAllocator heap(MemoryTag::Containers);
    int* value = heap.create<int>(42);
    CHECK(*value == 42);
    heap.destroy(value);
    std::vector<int, StdAllocator<int>> v{StdAllocator<int>(heap)};
    for (int i = 0; i < 1000; ++i) v.push_back(i);
    CHECK(v[999] == 999);
    CHECK((StdAllocator<int>(heap) == StdAllocator<long>(heap)));
    CHECK(&defaultAllocator() == &defaultAllocator());
}

TEST_CASE("memory: linear allocator") {
    LinearAllocator linear(1024);
    CHECK(linear.capacity() == 1024);
    void* a = linear.allocate(10, 1);
    void* b = linear.allocate(16, 64);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(isAligned(b, 64));
    CHECK(static_cast<std::byte*>(b) > static_cast<std::byte*>(a));
    const auto marker = linear.marker();
    u32* arr = linear.allocArray<u32>(8);
    REQUIRE(arr);
    CHECK(arr[7] == 0);
    linear.rewind(marker);
    CHECK(linear.allocArray<u32>(8) == arr);
    CHECK(linear.allocate(2000) == nullptr);
    CHECK(linear.peak() >= linear.used());
    linear.reset();
    CHECK(linear.used() == 0);

    alignas(16) std::byte external[64];
    LinearAllocator ext(external, sizeof(external));
    CHECK(ext.allocate(64, 16) == external);
    CHECK(ext.allocate(1, 1) == nullptr);

    // Invalid alignments are rejected instead of producing garbage offsets; 0 means 1.
    LinearAllocator checked(256);
    CHECK(checked.allocate(8, 24) == nullptr);
    CHECK(checked.allocate(8, 0) != nullptr);
    FrameAllocator frame(256);
    CHECK(frame.allocate(8, 12) == nullptr);
    CHECK(frame.allocate(8, 0) != nullptr);
}

TEST_CASE("memory: frame allocator is N-buffered") {
    FrameAllocator frames(4096, 2);
    void* f0 = frames.allocate(128);
    REQUIRE(f0);
    frames.beginFrame();
    void* f1 = frames.allocate(128);
    REQUIRE(f1);
    CHECK(f0 != f1); // frame 0 data is still valid during frame 1
    frames.beginFrame();
    CHECK(frames.allocate(128) == f0); // buffer 0 recycled two frames later
    CHECK(frames.frameIndex() == 2);
    CHECK(frames.allocate(8192) == nullptr);
    CHECK(frames.overflowCount() == 1);
    auto* v = frames.make<u64>(u64{77});
    CHECK(*v == 77);
}

TEST_CASE("memory: frame allocator is safe for concurrent allocation") {
    constexpr int kThreads = 8;
    constexpr int kAllocs = 2000;
    FrameAllocator frames(kThreads * kAllocs * 64, 2);
    std::vector<std::vector<u8*>> ptrs(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kAllocs; ++i) {
                auto* p = static_cast<u8*>(frames.allocate(48, 16));
                if (!p) continue;
                std::memset(p, t + 1, 48);
                ptrs[t].push_back(p);
            }
        });
    }
    for (auto& th : threads) th.join();
    std::set<u8*> unique;
    for (int t = 0; t < kThreads; ++t) {
        CHECK(ptrs[t].size() == kAllocs);
        for (u8* p : ptrs[t]) {
            unique.insert(p);
            // Nobody else wrote into this block.
            CHECK(std::all_of(p, p + 48, [t](u8 b) { return b == t + 1; }));
        }
    }
    CHECK(unique.size() == static_cast<usize>(kThreads * kAllocs));
    CHECK(frames.overflowCount() == 0);
}

TEST_CASE("memory: pool allocator") {
    PoolAllocator::Desc desc;
    desc.blockSize = 24;
    desc.blockAlignment = 16;
    desc.blocksPerChunk = 4;
    desc.maxChunks = 2;
    PoolAllocator pool(desc);
    CHECK(pool.blockSize() == 32);
    std::vector<void*> blocks;
    for (int i = 0; i < 8; ++i) {
        void* p = pool.allocateBlock();
        REQUIRE(p);
        CHECK(isAligned(p, 16));
        CHECK(pool.owns(p));
        blocks.push_back(p);
    }
    CHECK(pool.allocateBlock() == nullptr); // maxChunks reached
    CHECK(pool.liveBlocks() == 8);
    CHECK(pool.capacityBlocks() == 8);
    CHECK(std::set<void*>(blocks.begin(), blocks.end()).size() == 8);
    pool.freeBlock(blocks[3]);
    CHECK(pool.allocateBlock() == blocks[3]); // recycled
    CHECK(pool.allocate(64) == nullptr);      // too big for the block size
    int local = 0;
    CHECK(!pool.owns(&local));
    for (void* p : blocks) pool.freeBlock(p);
    CHECK(pool.liveBlocks() == 0);
}

TEST_CASE("memory: thread-safe pool allocator under contention") {
    PoolAllocator::Desc desc;
    desc.blockSize = 64;
    desc.blocksPerChunk = 128;
    desc.threadSafe = true;
    PoolAllocator pool(desc);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::vector<u8*> mine;
            for (int round = 0; round < 50; ++round) {
                for (int i = 0; i < 64; ++i) {
                    auto* p = static_cast<u8*>(pool.allocateBlock());
                    if (!p) {
                        failures.fetch_add(1);
                        continue;
                    }
                    std::memset(p, t, 64);
                    mine.push_back(p);
                }
                for (u8* p : mine) {
                    if (p[0] != t || p[63] != t) failures.fetch_add(1);
                    pool.freeBlock(p);
                }
                mine.clear();
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(failures.load() == 0);
    CHECK(pool.liveBlocks() == 0);
}

TEST_CASE("memory: virtual memory reserve/commit/decommit/release") {
    const usize page = VirtualMemory::pageSize();
    CHECK(isPowerOfTwo(page));
    CHECK(VirtualMemory::allocationGranularity() >= page);
    const usize size = 64 * page;
    auto* base = static_cast<u8*>(VirtualMemory::reserve(size));
    REQUIRE(base != nullptr);
    CHECK(VirtualMemory::commit(base, 2 * page));
    base[0] = 1;
    base[2 * page - 1] = 2;
    CHECK(base[0] == 1);
    CHECK(VirtualMemory::commit(base + 10 * page, page));
    base[10 * page] = 3;
    CHECK(VirtualMemory::decommit(base, 2 * page));
    CHECK(VirtualMemory::commit(base, page));
    CHECK(base[0] == 0); // recommitted pages are zeroed
    CHECK(VirtualMemory::release(base, size));
}

// ---------------------------------------------------------------------------------------------
// Sharded tag accounting (WP-0.5 contention fix)
// ---------------------------------------------------------------------------------------------

TEST_CASE("memory: peak is exact for a short spike on one thread") {
    const MemoryTag tag = registerMemoryTag("TestPeakSpike");
    const MemoryTagStats before = memoryTagStats(tag);
    // Below the per-shard flush threshold: the spike is never published, yet the peak sees it.
    trackAllocation(tag, 4096);
    trackDeallocation(tag, 4096);
    MemoryTagStats after = memoryTagStats(tag);
    CHECK(after.liveBytes == before.liveBytes);
    CHECK(after.peakBytes == std::max<i64>(before.peakBytes, before.liveBytes + 4096));
    // Above it: the flush path publishes and raises the global peak exactly.
    trackAllocation(tag, 300 * 1024);
    trackAllocation(tag, 5);
    trackDeallocation(tag, 300 * 1024 + 5);
    after = memoryTagStats(tag);
    CHECK(after.liveBytes == before.liveBytes);
    CHECK(after.peakBytes == before.liveBytes + 300 * 1024 + 5);
    CHECK(after.liveAllocations == before.liveAllocations + 1); // two allocations, one (combined) free
    trackDeallocations(tag, 0, 1);                              // rebalance the count
    CHECK(memoryTagStats(tag).liveAllocations == before.liveAllocations);
}

TEST_CASE("memory: batched accounting keeps counts exact") {
    const MemoryTag tag = registerMemoryTag("TestBatched");
    const MemoryTagStats before = memoryTagStats(tag);
    trackAllocations(tag, 6400, 100);
    MemoryTagStats mid = memoryTagStats(tag);
    CHECK(mid.liveBytes - before.liveBytes == 6400);
    CHECK(mid.liveAllocations - before.liveAllocations == 100);
    CHECK(mid.totalAllocations - before.totalAllocations == 100);
    CHECK(mid.peakBytes >= before.liveBytes + 6400);
    trackDeallocations(tag, 6400, 100);
    const MemoryTagStats after = memoryTagStats(tag);
    CHECK(after.liveBytes == before.liveBytes);
    CHECK(after.liveAllocations == before.liveAllocations);
    CHECK(after.totalAllocations - before.totalAllocations == 100);
    // count = 0 adjusts bytes only (a block that grew in place).
    trackAllocations(tag, 10, 0);
    CHECK(memoryTagStats(tag).liveAllocations == before.liveAllocations);
    CHECK(memoryTagStats(tag).liveBytes == before.liveBytes + 10);
    trackDeallocations(tag, 10, 0);
}

TEST_CASE("memory: batched accounting honours budgets") {
    const auto saved = log::level();
    log::setLevel(log::Level::Error);
    const MemoryTag tag = registerMemoryTag("TestBatchedBudget", 1000);
    trackAllocations(tag, 900, 9);
    CHECK(memoryTagStats(tag).budgetExceededCount == 0);
    trackAllocations(tag, 200, 2);
    CHECK(memoryTagStats(tag).budgetExceededCount == 1);
    trackDeallocations(tag, 1100, 11);
    trackAllocations(tag, 1001, 1);
    CHECK(memoryTagStats(tag).budgetExceededCount == 2);
    trackDeallocations(tag, 1001, 1);
    setMemoryBudget(tag, 0);
    log::setLevel(saved);
}

TEST_CASE("memory: concurrent tracking under one tag balances exactly") {
    const MemoryTag tag = registerMemoryTag("TestConcurrentTracking");
    const MemoryTagStats before = memoryTagStats(tag);
    constexpr int kThreads = 8;
    constexpr int kOps = 20000;
    constexpr usize kWindow = 8;
    constexpr usize kSize = 96;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // Frees are done on a different thread than the allocations half of the time, so
            // shard cells go negative and must still sum up exactly.
            void* window[kWindow] = {};
            for (int i = 0; i < kOps; ++i) {
                void*& slot = window[static_cast<usize>(i) % kWindow];
                alignedFree(slot);
                slot = alignedAlloc(kSize + static_cast<usize>(t), 16, tag);
            }
            for (void* p : window) alignedFree(p);
        });
    }
    for (auto& th : threads) th.join();
    // Cross-thread frees: allocate here, free on other threads.
    std::vector<void*> handoff;
    for (int i = 0; i < 64; ++i) handoff.push_back(alignedAlloc(1000, 16, tag));
    std::vector<std::thread> freers;
    for (int t = 0; t < 4; ++t) {
        freers.emplace_back([&, t] {
            for (int i = t; i < 64; i += 4) alignedFree(handoff[static_cast<usize>(i)]);
        });
    }
    for (auto& th : freers) th.join();

    const MemoryTagStats after = memoryTagStats(tag);
    CHECK(after.liveBytes == before.liveBytes);
    CHECK(after.liveAllocations == before.liveAllocations);
    CHECK(after.totalAllocations - before.totalAllocations == static_cast<u64>(kThreads * kOps + 64));
    // Peak: at least what one thread held, at most everything plus the documented slack.
    const i64 oneThread = static_cast<i64>(kWindow * kSize);
    i64 everything = 64 * 1000;
    for (int t = 0; t < kThreads; ++t) everything += static_cast<i64>(kWindow * (kSize + static_cast<usize>(t)));
    CHECK(after.peakBytes >= before.liveBytes + oneThread);
    CHECK(after.peakBytes <= std::max<i64>(before.peakBytes, before.liveBytes + everything + 16 * 64 * 1024));
}

TEST_CASE("memory: a budget crossing on one thread is detected immediately under load") {
    // Other threads churn the same tag while this thread crosses the budget: its own delta is
    // part of the estimate, so the crossing is seen on the allocation that causes it.
    const auto saved = log::level();
    log::setLevel(log::Level::Error);
    const MemoryTag tag = registerMemoryTag("TestBudgetUnderLoad", 1 << 20);
    std::atomic<bool> stop{false};
    std::vector<std::thread> churn;
    for (int t = 0; t < 3; ++t) {
        churn.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                void* p = alignedAlloc(64, 16, tag);
                alignedFree(p);
            }
        });
    }
    void* big = alignedAlloc((1 << 20) + 4096, 16, tag);
    CHECK(memoryTagStats(tag).budgetExceededCount == 1);
    stop.store(true);
    for (auto& th : churn) th.join();
    alignedFree(big);
    setMemoryBudget(tag, 0);
    log::setLevel(saved);
}
