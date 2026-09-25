// Phase 0 spikes (roadmap, 02 §2.2 / §4.2), reported in engine/ecs/SPIKES.md:
//   (a) mimalloc heap thread affinity: throughput of the candidate allocation paths for tagged
//       allocators on job-system workers, and cross-worker frees.
//   (b) flecs DontFragment for high-churn components/relationships: tables, churn cost, iteration
//       cost, lookups and memory for fragmenting vs non-fragmenting storage.

#include "bench_spikes.h"

#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/jobs.h"
#include "helios/core/memory.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/world.h"
#include "helios/math/vec.h"

namespace bench {

using namespace helios;
using namespace helios::ecs;

namespace {

void out(const std::string& s) { std::fputs(s.c_str(), stdout); }

// ---------------------------------------------------------------------------------------------
// (a) heap spike
// ---------------------------------------------------------------------------------------------

enum class AllocPath { MiMalloc, SharedHeap, ThreadHeap, TaggedHeapPath, TaggedHeapBatched, AlignedAlloc };

const char* pathName(AllocPath p) {
    switch (p) {
    case AllocPath::MiMalloc: return "mi_malloc (process heap)";
    case AllocPath::SharedHeap: return "mi_heap_malloc (shared v3 heap)";
    case AllocPath::ThreadHeap: return "mi_theap_malloc (per-thread theap)";
    case AllocPath::TaggedHeapPath: return "ecs::TaggedHeap, exact tag stats";
    case AllocPath::TaggedHeapBatched: return "ecs::TaggedHeap, batched tag stats";
    case AllocPath::AlignedAlloc: return "helios::alignedAlloc (tag + header)";
    }
    return "?";
}

struct AllocBatch {
    static constexpr u32 kBatch = 256;
    void* ptrs[kBatch];
};

/// Allocates and frees `rounds` batches of 256 blocks (16..528 bytes) on the calling thread.
void allocRounds(AllocPath path, TaggedHeap& heap, MemoryTag tag, u32 rounds, u64 seed) {
    AllocBatch b;
    mi_heap_t* shared = static_cast<mi_heap_t*>(heap.nativeHeap());
    // A theap is obtained on the thread that uses it and never kept past this job (spike rule 2).
    mi_theap_t* theap = path == AllocPath::ThreadHeap ? mi_heap_theap(shared) : nullptr;
    for (u32 round = 0; round < rounds; ++round) {
        for (u32 i = 0; i < AllocBatch::kBatch; ++i) {
            const usize size = 16 + static_cast<usize>(mix64(seed + round * 1000 + i) % 512);
            switch (path) {
            case AllocPath::MiMalloc: b.ptrs[i] = mi_malloc(size); break;
            case AllocPath::SharedHeap: b.ptrs[i] = mi_heap_malloc(shared, size); break;
            case AllocPath::ThreadHeap: b.ptrs[i] = mi_theap_malloc(theap, size); break;
            case AllocPath::TaggedHeapPath:
            case AllocPath::TaggedHeapBatched: b.ptrs[i] = heap.allocate(size); break;
            case AllocPath::AlignedAlloc: b.ptrs[i] = alignedAlloc(size, 16, tag); break;
            }
            static_cast<unsigned char*>(b.ptrs[i])[0] = static_cast<unsigned char>(i);
        }
        for (u32 i = 0; i < AllocBatch::kBatch; ++i) {
            switch (path) {
            case AllocPath::TaggedHeapPath:
            case AllocPath::TaggedHeapBatched: heap.free(b.ptrs[i]); break;
            case AllocPath::AlignedAlloc: alignedFree(b.ptrs[i]); break;
            default: mi_free(b.ptrs[i]); break;
            }
        }
    }
}

/// ns per (alloc + free) pair with `threads` concurrent job-system threads.
f64 measureAllocPath(AllocPath path, jobs::JobSystem* js, u32 threads, u32 roundsPerThread) {
    const MemoryTag tag = registerMemoryTag("Bench.HeapSpike");
    TaggedHeap heap(tag, path == AllocPath::TaggedHeapBatched ? TaggedHeap::Accounting::Batched
                                                              : TaggedHeap::Accounting::Exact);
    auto run = [&](u32 rounds) {
        if (threads <= 1 || !js) {
            allocRounds(path, heap, tag, rounds, 7);
        } else {
            js->parallelFor(0, threads, 1, [&](u64 t) { allocRounds(path, heap, tag, rounds, 7 + t * 977); });
        }
    };
    run(std::max(16u, roundsPerThread / 8)); // warm up pages and per-thread theaps on every thread
    f64 best = 1e30;
    for (int rep = 0; rep < 3; ++rep) {
        const Stopwatch sw;
        run(roundsPerThread);
        best = std::min(best, static_cast<f64>(sw.elapsedNanos()));
    }
    // Throughput per thread: wall time divided by the pairs each thread performed.
    return best / (static_cast<f64>(roundsPerThread) * AllocBatch::kBatch);
}

/// Producer jobs allocate batches that consumer jobs (usually on another worker) free.
f64 measureCrossThreadFree(bool tagged, jobs::JobSystem& js, u32 batches) {
    const MemoryTag tag = registerMemoryTag("Bench.HeapSpike.Cross");
    TaggedHeap heap(tag);
    std::vector<AllocBatch> storage(batches);
    std::atomic<u32> foreignFrees{0};
    const Stopwatch sw;
    jobs::Counter all;
    for (u32 b = 0; b < batches; ++b) {
        js.run(
            [&, b] {
                const i32 producer = js.currentWorkerIndex();
                AllocBatch& batch = storage[b];
                for (u32 i = 0; i < AllocBatch::kBatch; ++i) {
                    const usize size = 16 + static_cast<usize>(mix64(b * 1000 + i) % 512);
                    batch.ptrs[i] = tagged ? heap.allocate(size) : mi_malloc(size);
                }
                jobs::Counter freed;
                js.run(
                    [&, b, producer] {
                        if (js.currentWorkerIndex() != producer) foreignFrees.fetch_add(1, std::memory_order_relaxed);
                        for (void* p : storage[b].ptrs) {
                            if (tagged) {
                                heap.free(p);
                            } else {
                                mi_free(p);
                            }
                        }
                    },
                    &freed);
                js.wait(freed);
            },
            &all);
    }
    js.wait(all);
    const f64 ns = static_cast<f64>(sw.elapsedNanos());
    HELIOS_VERIFY(!tagged || heap.liveAllocations() == 0);
    out(std::format("    cross-thread batches freed on another worker: {}/{}\n", foreignFrees.load(), batches));
    return ns / (static_cast<f64>(batches) * AllocBatch::kBatch);
}

// ---------------------------------------------------------------------------------------------
// (b) DontFragment spike
// ---------------------------------------------------------------------------------------------

struct SPos {
    DVec3 p{};
};
struct SVel {
    Vec3 v{};
};
struct SMass {
    f32 m = 1;
};
struct SRef { // "plain EntityId field" alternative to a relationship
    Entity target;
};
struct SStatus { // high-churn component (buffs/debuffs, "in combat", ...)
    u32 flags = 0;
};

enum class LinkKind { DockFragmenting, DockDontFragment, DockField, PlainField, ChildOf, ParentComponent };

const char* linkName(LinkKind k) {
    switch (k) {
    case LinkKind::DockFragmenting: return "DockedTo (fragmenting pair)";
    case LinkKind::DockDontFragment: return "DockedTo (DontFragment pair)";
    case LinkKind::DockField: return "DockedTo (DockRef field + index)";
    case LinkKind::PlainField: return "plain Entity field";
    case LinkKind::ChildOf: return "ChildOf (fragmenting)";
    case LinkKind::ParentComponent: return "Parent (flecs 4.1 non-fragmenting)";
    }
    return "?";
}

struct LinkResult {
    u32 tables = 0;
    f64 assignMs = 0;
    f64 churnMs = 0;
    f64 iterateUs = 0;
    f64 lookupUs = 0;
    f64 memoryMb = 0;
    u32 lookedUp = 0;
};

LinkResult runLinkSpike(LinkKind kind, u32 entities, u32 linked, u32 targets, u32 churn) {
    WorldDesc desc;
    desc.relations.docking = kind == LinkKind::DockFragmenting    ? DockStorage::Pair
                             : kind == LinkKind::DockDontFragment ? DockStorage::PairDontFragment
                                                                  : DockStorage::Field;
    desc.relations.nonFragmentingHierarchy = kind != LinkKind::ChildOf;
    const i64 mem0 = memoryTagStats(ecsMemoryTag()).liveBytes;
    World w(desc);
    w.registerComponent<SPos>();
    w.registerComponent<SVel>();
    w.registerComponent<SMass>();
    w.registerComponent<SRef>();
    std::vector<Entity> es, hosts;
    CommandBuffer cb(&w);
    for (u32 i = 0; i < targets; ++i) cb.spawn();
    for (u32 i = 0; i < entities; ++i) {
        const TempEntity t = cb.spawn();
        cb.set(t, SPos{DVec3(static_cast<f64>(i), 0, 0)});
        cb.set(t, SVel{Vec3(1, 0, 0)});
        cb.set(t, SMass{});
    }
    w.apply(cb);
    for (u32 i = 0; i < targets; ++i) hosts.push_back(cb.resolved(TempEntity{i}));
    for (u32 i = 0; i < entities; ++i) es.push_back(cb.resolved(TempEntity{targets + i}));
    const u32 tables0 = w.stats().tableCount;

    auto link = [&](CommandBuffer& buf, Entity e, Entity host) {
        switch (kind) {
        case LinkKind::DockFragmenting:
        case LinkKind::DockDontFragment:
        case LinkKind::DockField: buf.dock(e, host); break;
        case LinkKind::PlainField: buf.set(e, SRef{host}); break;
        case LinkKind::ChildOf:
        case LinkKind::ParentComponent: buf.setParent(e, host); break;
        }
    };
    Stopwatch sw;
    for (u32 i = 0; i < linked; ++i) link(cb, es[i], hosts[mix64(i) % targets]);
    w.apply(cb);
    LinkResult r;
    r.assignMs = sw.elapsedMillis();
    r.tables = w.stats().tableCount - tables0;

    // Churn: re-target `churn` linked entities (docking/boarding/re-parenting at a sync point).
    for (u32 i = 0; i < churn; ++i) link(cb, es[i % linked], hosts[mix64(i + 777) % targets]);
    sw.reset();
    w.apply(cb);
    r.churnMs = sw.elapsedMillis();

    // Iteration over everything (fragmentation spreads the rows over many small tables).
    Query q(w, {Term{w.id<SPos>(), TermAccess::Write}, Term{w.id<SVel>(), TermAccess::Read}});
    std::vector<ChunkData> chunks;
    f64 best = 1e30;
    for (int rep = 0; rep < 20; ++rep) {
        sw.reset();
        chunks.clear();
        q.collect(chunks);
        for (const ChunkData& c : chunks) {
            auto* p = static_cast<SPos*>(c.fields[0]);
            const auto* v = static_cast<const SVel*>(c.fields[1]);
            for (u32 row = 0; row < c.count; ++row) p[row].p += toF64(v[row].v) * 0.05;
        }
        best = std::min(best, sw.elapsedMillis() * 1000.0);
    }
    r.iterateUs = best;

    // Lookup: everything linked to 100 hosts.
    std::vector<Entity> found;
    sw.reset();
    for (u32 h = 0; h < 100; ++h) {
        switch (kind) {
        case LinkKind::DockFragmenting:
        case LinkKind::DockDontFragment:
        case LinkKind::DockField: w.dockedAt(hosts[h], found); break;
        case LinkKind::ChildOf:
        case LinkKind::ParentComponent: w.childrenOf(hosts[h], found); break;
        case LinkKind::PlainField: {
            Query rq(w, {Term{w.id<SRef>(), TermAccess::Read}});
            rq.forEachChunk([&](ChunkView& c) {
                const SRef* refs = c.read<SRef>(0);
                for (u32 row = 0; row < c.count(); ++row) {
                    if (refs[row].target == hosts[h]) found.push_back(c.entity(row));
                }
            });
            break;
        }
        }
    }
    r.lookupUs = sw.elapsedMillis() * 1000.0;
    r.lookedUp = static_cast<u32>(found.size());
    r.memoryMb = static_cast<f64>(memoryTagStats(ecsMemoryTag()).liveBytes - mem0) / (1024.0 * 1024.0);
    return r;
}

struct StatusResult {
    u32 tables = 0;
    f64 toggleMs = 0;
    f64 iterateUs = 0;
};

/// A high-churn component (status effects) toggled on `churn` of 50k entities per sync point.
StatusResult runStatusSpike(bool dontFragment, u32 entities, u32 churn) {
    World w;
    w.registerComponent<SPos>();
    w.registerComponent<SVel>();
    // Eight independent status components: as regular components they multiply archetypes.
    std::vector<ComponentId> statuses;
    for (u32 s = 0; s < 8; ++s) {
        ComponentDesc d = componentDescOf<SStatus>("spike.Status" + std::to_string(s),
                                                   dontFragment ? ComponentFlags::DontFragment : ComponentFlags::None);
        statuses.push_back(*w.registerComponent(d));
    }
    CommandBuffer cb(&w);
    for (u32 i = 0; i < entities; ++i) {
        const TempEntity t = cb.spawn();
        cb.set(t, SPos{});
        cb.set(t, SVel{Vec3(1, 0, 0)});
    }
    w.apply(cb);
    std::vector<Entity> es;
    for (u32 i = 0; i < entities; ++i) es.push_back(cb.resolved(TempEntity{i}));
    const u32 tables0 = w.stats().tableCount;
    StatusResult r;
    const SStatus value{1};
    f64 total = 0;
    constexpr int kRounds = 10;
    for (int round = 0; round < kRounds; ++round) {
        for (u32 i = 0; i < churn; ++i) {
            const Entity e = es[mix64(static_cast<u64>(round) * 100000 + i) % entities];
            const ComponentId s = statuses[mix64(i + static_cast<u64>(round)) % statuses.size()];
            if (w.ownsId(e, s)) {
                cb.removeId(e, s);
            } else {
                cb.setRaw(e, s, &value, sizeof value);
            }
        }
        const Stopwatch sw;
        w.apply(cb);
        total += sw.elapsedMillis();
    }
    r.toggleMs = total / kRounds;
    r.tables = w.stats().tableCount - tables0;
    Query q(w, {Term{w.id<SPos>(), TermAccess::Write}, Term{w.id<SVel>(), TermAccess::Read}});
    std::vector<ChunkData> chunks;
    f64 best = 1e30;
    for (int rep = 0; rep < 20; ++rep) {
        const Stopwatch sw;
        chunks.clear();
        q.collect(chunks);
        for (const ChunkData& c : chunks) {
            auto* p = static_cast<SPos*>(c.fields[0]);
            const auto* v = static_cast<const SVel*>(c.fields[1]);
            for (u32 row = 0; row < c.count; ++row) p[row].p += toF64(v[row].v) * 0.05;
        }
        best = std::min(best, sw.elapsedMillis() * 1000.0);
    }
    r.iterateUs = best;
    return r;
}

/// Reproduces the mimalloc 3.5.3 heap-recycling hazard with the raw API (worker caches the theap of
/// a destroyed heap; the next heap lands at the same address), then runs the same pattern through
/// TaggedHeap (pooled heaps). Returns {raw wrong-heap allocations, TaggedHeap wrong-heap allocations}.
std::pair<u32, u32> heapRecycleHazard(u32 rounds) {
    u32 rawWrong = 0, sameAddress = 0, taggedWrong = 0;
    for (u32 round = 0; round < rounds; ++round) {
        mi_heap_t* h1 = mi_heap_new();
        {
            // A short-lived thread (e.g. a finished background job thread) used the heap first.
            Thread early("aba-early", [&] { mi_free(mi_heap_malloc(h1, 64)); });
        }
        std::atomic<mi_heap_t*> next{nullptr};
        std::atomic<int> phase{0};
        std::atomic<bool> wrong{false};
        {
            Thread worker("aba-worker", [&] {
                mi_free(mi_heap_malloc(h1, 64));
                phase = 1;
                while (!next.load()) yieldThread();
                void* q = mi_heap_malloc(next.load(), 64);
                wrong = mi_heap_of(q) != next.load();
                mi_free(q);
            });
            while (phase.load() == 0) yieldThread();
            mi_heap_destroy(h1);
            mi_heap_t* h2 = mi_heap_new();
            sameAddress += h2 == h1 ? 1u : 0u;
            next = h2;
        }
        rawWrong += wrong.load() ? 1u : 0u;
        mi_heap_destroy(next.load()); // the worker has exited: safe
    }
    const MemoryTag tag = registerMemoryTag("Bench.HeapSpike.Recycle");
    for (u32 round = 0; round < rounds; ++round) {
        std::atomic<TaggedHeap*> next{nullptr};
        std::atomic<int> phase{0};
        std::atomic<bool> wrong{false};
        auto first = std::make_unique<TaggedHeap>(tag);
        {
            Thread worker("aba-worker", [&] {
                first->free(first->allocate(64));
                phase = 1;
                while (!next.load()) yieldThread();
                void* q = next.load()->allocate(64);
                wrong = !next.load()->owns(q);
                next.load()->free(q);
            });
            while (phase.load() == 0) yieldThread();
            first.reset(); // returns the heap to the pool instead of freeing it
            auto second = std::make_unique<TaggedHeap>(tag);
            next = second.get();
            worker.join();
        }
        taggedWrong += wrong.load() ? 1u : 0u;
    }
    out(std::format("  heap recycling hazard ({} rounds): raw mi_heap_destroy + mi_heap_new -> same address {}x, "
                    "worker allocated from the dead heap {}x; TaggedHeap (pooled) wrong-heap {}x\n",
                    rounds, sameAddress, rawWrong, taggedWrong));
    return {rawWrong, taggedWrong};
}

} // namespace

void runHeapSpike(bool quick) {
    out("\n== Spike (a): mimalloc v3 heaps on job-system workers ==\n");
    out(std::format("  mimalloc {}.{}.{}: mi_heap_t is shareable across threads; mi_theap_t is thread-bound\n",
                    MI_MALLOC_VERSION / 10000, (MI_MALLOC_VERSION / 100) % 100, MI_MALLOC_VERSION % 100));
    jobs::JobSystem js({.workerCount = 4});
    const u32 rounds = quick ? 200 : 2000;
    out("  ns per alloc+free pair per thread (16..528 B, batches of 256)   1 thread   4 threads\n");
    for (AllocPath p : {AllocPath::MiMalloc, AllocPath::SharedHeap, AllocPath::ThreadHeap, AllocPath::TaggedHeapPath,
                        AllocPath::TaggedHeapBatched, AllocPath::AlignedAlloc}) {
        const f64 one = measureAllocPath(p, nullptr, 1, rounds);
        const f64 four = measureAllocPath(p, &js, 4, rounds);
        out(std::format("    {:<48} {:>8.1f}   {:>8.1f}\n", pathName(p), one, four));
    }
    out("  cross-worker free (producer job allocates, consumer job frees):\n");
    const u32 batches = quick ? 400 : 4000;
    const f64 crossMi = measureCrossThreadFree(false, js, batches);
    const f64 crossTagged = measureCrossThreadFree(true, js, batches);
    out(std::format("    mi_malloc/mi_free        {:>6.1f} ns/pair\n", crossMi));
    out(std::format("    TaggedHeap alloc/free    {:>6.1f} ns/pair\n", crossTagged));
    heapRecycleHazard(quick ? 50 : 200);
}

void runFragmentSpike(bool quick) {
    out("\n== Spike (b): flecs 4.1.6 DontFragment for high-churn data ==\n");
    const u32 entities = quick ? 20000 : 50000;
    const u32 linked = entities * 2 / 5;
    const u32 targets = quick ? 1000 : 2000;
    const u32 churn = 9000;
    out(std::format("  links: {} entities, {} linked to {} targets, {} re-targeted per sync point\n", entities,
                    linked, targets, churn));
    out("    storage                                 +tables  assign ms  churn ms  iterate us  lookup(100) us  ECS MB\n");
    for (LinkKind k : {LinkKind::DockFragmenting, LinkKind::DockDontFragment, LinkKind::DockField, LinkKind::PlainField,
                       LinkKind::ChildOf, LinkKind::ParentComponent}) {
        const LinkResult r = runLinkSpike(k, entities, linked, targets, churn);
        out(std::format("    {:<38} {:>8} {:>10.2f} {:>9.2f} {:>11.1f} {:>15.1f} {:>7.1f}   (found {})\n", linkName(k),
                        r.tables, r.assignMs, r.churnMs, r.iterateUs, r.lookupUs, r.memoryMb, r.lookedUp));
    }
    out(std::format("  status components: 8 kinds toggled on {} of {} entities per sync point\n", churn, entities));
    out("    storage                                 +tables  toggle ms  iterate us\n");
    for (const bool df : {false, true}) {
        const StatusResult r = runStatusSpike(df, entities, churn);
        out(std::format("    {:<38} {:>8} {:>10.2f} {:>11.1f}\n", df ? "DontFragment components" : "regular components",
                        r.tables, r.toggleMs, r.iterateUs));
    }
}

} // namespace bench
