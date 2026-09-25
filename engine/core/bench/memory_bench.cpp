// core_memory_bench — contention benchmark for memory-tag accounting (WP-0.5).
//
// Every tracked allocation updates its tag's counters. Before WP-0.5 those counters were one set of
// atomics per tag, so threads allocating under the same tag (every job worker allocating under
// "ECS", say) bounced one cache line between cores on every allocation and free. The counters are
// now sharded per thread (see memory.cpp), and the peak is only written when it rises.
//
// This benchmark measures, for 1..N threads hammering ONE tag:
//   legacy  - a verbatim replica of the pre-WP-0.5 single-slot accounting (the A/B baseline),
//   track   - helios::trackAllocation/trackDeallocation (sharded),
//   batch   - helios::trackAllocations/trackDeallocations with 64 allocations per call,
//   alloc   - helios::alignedAlloc/alignedFree of 64 bytes (includes mimalloc's own cost).
// It prints ns per allocate+free pair and the speed-up of `track` over `legacy`.
//
// Usage: core_memory_bench [--ops=N] [--threads=N] [--gate]
//   --gate  exits 1 unless `track` beats `legacy` by >= 2x at the highest thread count (>= 4
//           threads). Timing-dependent, so it is not part of the default CTest run.
// Budget (docs: engine/core/README.md): <= 25 ns per tracked allocate+free pair per thread, flat
// from 1 to 8 threads on REF hardware.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helios/core/memory.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "helios/core/types.h"

using namespace helios;

namespace {

// ---- Replica of the pre-WP-0.5 accounting (one set of atomics per tag). ---------------------
struct LegacySlot {
    std::atomic<i64> liveBytes{0};
    std::atomic<i64> peakBytes{0};
    std::atomic<i64> liveCount{0};
    std::atomic<u64> totalCount{0};
    std::atomic<u64> budget{0};
    std::atomic<u64> exceeded{0};
    std::atomic<bool> overBudget{false};
};

LegacySlot g_legacy;

void legacyTrack(LegacySlot& slot, usize bytes) noexcept {
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
    }
}

void legacyUntrack(LegacySlot& slot, usize bytes) noexcept {
    const i64 live = slot.liveBytes.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed) -
                     static_cast<i64>(bytes);
    slot.liveCount.fetch_sub(1, std::memory_order_relaxed);
    const u64 budget = slot.budget.load(std::memory_order_relaxed);
    if (budget == 0 || live <= static_cast<i64>(budget)) slot.overBudget.store(false, std::memory_order_relaxed);
}

// ---- Harness ----------------------------------------------------------------------------------
enum class Mode { Legacy, Track, Batch, Alloc };

constexpr std::string_view modeName(Mode m) {
    switch (m) {
    case Mode::Legacy: return "legacy";
    case Mode::Track: return "track";
    case Mode::Batch: return "batch";
    case Mode::Alloc: return "alloc";
    }
    return "?";
}

// Each thread keeps a small window of live allocations so live bytes and the peak move the way
// they do in real code (not strictly alternating +64/-64).
constexpr usize kWindow = 16;
constexpr usize kBytes = 64;
constexpr u64 kBatch = 64;

void worker(Mode mode, MemoryTag tag, u64 ops, std::atomic<u32>& ready, std::atomic<bool>& go) {
    ready.fetch_add(1, std::memory_order_acq_rel);
    while (!go.load(std::memory_order_acquire)) cpuPause();
    switch (mode) {
    case Mode::Legacy:
        for (u64 i = 0; i < ops; ++i) {
            legacyTrack(g_legacy, kBytes);
            if (i >= kWindow) legacyUntrack(g_legacy, kBytes);
        }
        for (usize i = 0; i < std::min<u64>(ops, kWindow); ++i) legacyUntrack(g_legacy, kBytes);
        break;
    case Mode::Track:
        for (u64 i = 0; i < ops; ++i) {
            trackAllocation(tag, kBytes);
            if (i >= kWindow) trackDeallocation(tag, kBytes);
        }
        for (usize i = 0; i < std::min<u64>(ops, kWindow); ++i) trackDeallocation(tag, kBytes);
        break;
    case Mode::Batch:
        for (u64 i = 0; i < ops; i += kBatch) {
            trackAllocations(tag, kBytes * kBatch, kBatch);
            trackDeallocations(tag, kBytes * kBatch, kBatch);
        }
        break;
    case Mode::Alloc: {
        void* window[kWindow] = {};
        for (u64 i = 0; i < ops; ++i) {
            void*& slot = window[i % kWindow];
            alignedFree(slot);
            slot = alignedAlloc(kBytes, 16, tag);
        }
        for (void* p : window) alignedFree(p);
        break;
    }
    }
}

// Returns ns per allocate+free pair (per thread).
f64 run(Mode mode, MemoryTag tag, u32 threads, u64 opsPerThread) {
    std::atomic<u32> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (u32 t = 0; t < threads; ++t) {
        pool.emplace_back([&] { worker(mode, tag, opsPerThread, ready, go); });
    }
    while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
    Stopwatch sw;
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const f64 ns = static_cast<f64>(sw.elapsedNanos());
    return ns / static_cast<f64>(opsPerThread);
}

bool parseU64(std::string_view arg, std::string_view key, u64& out) {
    if (arg.substr(0, key.size()) != key) return false;
    out = std::strtoull(std::string(arg.substr(key.size())).c_str(), nullptr, 10);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    u64 ops = 2'000'000;
    u64 maxThreads = std::max<u32>(4, std::min<u32>(8, hardwareThreadCount()));
    bool gate = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--gate") {
            gate = true;
        } else if (!parseU64(a, "--ops=", ops) && !parseU64(a, "--threads=", maxThreads)) {
            std::fprintf(stderr, "usage: core_memory_bench [--ops=N] [--threads=N] [--gate]\n");
            return 2;
        }
    }
    ops = std::max<u64>(ops, kBatch);
    const MemoryTag tag = registerMemoryTag("BenchContention");

    std::printf("memory-tag accounting, one shared tag, %llu allocate+free pairs per thread\n",
                static_cast<unsigned long long>(ops));
    std::printf("%8s %10s %10s %10s %10s %9s\n", "threads", "legacy", "track", "batch", "alloc", "speedup");
    f64 lastSpeedup = 0.0;
    u32 lastThreads = 0;
    for (u32 threads = 1; threads <= maxThreads; threads *= 2) {
        f64 best[4] = {1e30, 1e30, 1e30, 1e30};
        for (int rep = 0; rep < 3; ++rep) { // best of 3 filters scheduler noise on shared machines
            const Mode modes[4] = {Mode::Legacy, Mode::Track, Mode::Batch, Mode::Alloc};
            for (int m = 0; m < 4; ++m) best[m] = std::min(best[m], run(modes[m], tag, threads, ops));
        }
        lastSpeedup = best[0] / best[1];
        lastThreads = threads;
        std::printf("%8u %8.1fns %8.1fns %8.1fns %8.1fns %8.2fx\n", threads, best[0], best[1], best[2], best[3],
                    lastSpeedup);
        if (threads * 2 > maxThreads && threads != maxThreads) {
            threads = static_cast<u32>(maxThreads) / 2; // make the last row exactly maxThreads
        }
    }
    (void)modeName;

    // Accounting must balance exactly after every run, whatever the interleaving.
    const MemoryTagStats stats = memoryTagStats(tag);
    if (stats.liveBytes != 0 || stats.liveAllocations != 0 || g_legacy.liveBytes.load() != 0) {
        std::fprintf(stderr, "FAIL: accounting did not balance (live %lld bytes, %lld allocations)\n",
                     static_cast<long long>(stats.liveBytes), static_cast<long long>(stats.liveAllocations));
        return 1;
    }
    if (gate) {
        if (lastThreads < 4) {
            std::printf("gate skipped: fewer than 4 threads\n");
            return 0;
        }
        if (lastSpeedup < 2.0) {
            std::fprintf(stderr, "FAIL: sharded tracking only %.2fx faster than legacy at %u threads (need 2x)\n",
                         lastSpeedup, lastThreads);
            return 1;
        }
        std::printf("gate passed: %.2fx at %u threads\n", lastSpeedup, lastThreads);
    }
    return 0;
}
