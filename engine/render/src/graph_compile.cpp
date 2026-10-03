// Render graph compile (03 §2.2): culling, queue batches and waits, aliasing, placement plan,
// barriers, store ops and command-list partition; plus entry-barrier resolution.
//
// Budget: a full compile of 200 passes ≤ 0.3 ms on REF at 60 fps (03 §8.1.5; render_tests_perf). What
// keeps it there:
//   * Storage reuse. RgCompileStorage, which the graph keeps between compiles and across
//     RenderGraph::reset(), holds the scratch arrays and the elements of the previous plan with their
//     strings and vectors, so a compile allocates only while that storage grows (for an unchanged
//     graph, in its first two compiles).
//   * Flat scratch: offsets into shared arrays instead of a vector per pass, subresource, reader or
//     batch.
//   * Happens-before folded per resource (lifetimes()): aliasing and placement test it for every pair
//     of candidates, so it is a few compares instead of a walk over pass, batch and clock records.
// The plan is identical to what a compile into fresh storage produces (render_tests check it).

#include <algorithm>
#include <charconv>

#include "graph_internal.h"
#include "helios/core/assert.h"
#include "helios/core/time.h"

namespace helios::render {

bool rgIsReadOnlyState(rhi::ResourceState state) noexcept {
    switch (state) {
    case rhi::ResourceState::ShaderResource:
    case rhi::ResourceState::CopySource:
    case rhi::ResourceState::DepthRead:
    case rhi::ResourceState::IndirectArgument:
    case rhi::ResourceState::VertexBuffer:
    case rhi::ResourceState::IndexBuffer:
    case rhi::ResourceState::ConstantBuffer:
    case rhi::ResourceState::Present:
    case rhi::ResourceState::HostRead: return true;
    default: return false;
    }
}

namespace {

constexpr u32 kQueues = rhi::kQueueCount;
constexpr u64 kPlacementAlignment = 64 * 1024;
/// RgCompileStorage::needClock of a queue no later first access constrains.
constexpr u32 kNoConstraint = 0xFFFFFFFFu;

u64 alignUp(u64 value, u64 alignment) { return (value + alignment - 1) / alignment * alignment; }

/// Where a hazard's earlier side lives: a pass on a queue, or nothing.
struct Site {
    u32 pass = kRgInvalid;
    rhi::Queue queue = rhi::Queue::Graphics;
    bool valid() const noexcept { return pass != kRgInvalid; }
};

template <class F>
void forEachSub(const rhi::SubresourceRange& range, u32 layers, F&& fn) {
    for (u32 m = range.baseMip; m < range.baseMip + range.mipCount; ++m) {
        for (u32 l = range.baseLayer; l < range.baseLayer + range.layerCount; ++l) fn(rgSub(m, l, layers), m, l);
    }
}

void appendDecimal(std::string& out, u32 value) {
    char digits[10];
    const std::to_chars_result end = std::to_chars(digits, digits + sizeof(digits), value);
    out.append(digits, end.ptr);
}

// -- storage kept between compiles ------------------------------------------------------------------

/// Moves `from` into `into` and empties it: `into` gets the storage of `from`.
template <class C>
void takeStorage(C& into, C& from) {
    into = std::move(from);
    into.clear();
}

// recycle(x): x becomes a default-valued element that keeps the storage of its strings and vectors.
void recycle(RgPassInfo& p) {
    RgPassInfo fresh;
    takeStorage(fresh.name, p.name);
    takeStorage(fresh.accesses, p.accesses);
    takeStorage(fresh.preBarriers, p.preBarriers);
    takeStorage(fresh.postBarriers, p.postBarriers);
    takeStorage(fresh.colorStore, p.colorStore);
    p = std::move(fresh);
}
void recycle(RgResourceInfo& r) {
    RgResourceInfo fresh;
    takeStorage(fresh.name, r.name);
    r = std::move(fresh);
}
void recycle(RgPhysicalInfo& p) {
    RgPhysicalInfo fresh;
    takeStorage(fresh.residents, p.residents);
    takeStorage(fresh.finalStates, p.finalStates);
    p = std::move(fresh);
}
void recycle(RgBatchInfo& b) {
    RgBatchInfo fresh;
    takeStorage(fresh.passes, b.passes);
    takeStorage(fresh.waits, b.waits);
    takeStorage(fresh.barriers, b.barriers);
    b = std::move(fresh);
}
void recycle(RgListInfo& l) {
    RgListInfo fresh;
    takeStorage(fresh.name, l.name);
    l = std::move(fresh);
}

/// Elements of earlier plans, kept with their storage; back() is reused first.
template <class T>
class Spares {
public:
    /// Moves the elements of `v` here (in reverse, so that they are reused in their order) and empties `v`.
    void reclaim(std::vector<T>& v) {
        for (usize i = v.size(); i-- > 0;) m_items.push_back(std::move(v[i]));
        v.clear();
    }
    /// Appends a default-valued element to `v`, with the storage of a kept one if there is one.
    T& append(std::vector<T>& v) {
        if (m_items.empty()) return v.emplace_back();
        T& item = v.emplace_back(std::move(m_items.back()));
        m_items.pop_back();
        recycle(item);
        return item;
    }

private:
    std::vector<T> m_items;
};

/// [begin, end) in one of RgCompileStorage's flat arrays.
struct Range {
    u32 begin = 0;
    u32 end = 0;
};

/// Hazard state of one virtual subresource (step 2).
struct VSub {
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    Site lastWrite;
    Site lastAccess;
    u32 readers = kRgInvalid;  // reads since the last write or state change (list in RgCompileStorage::readers)
};
struct Reader {
    Site site;
    u32 next = kRgInvalid;
};

/// A submission batch while it is built (step 3).
struct Build {
    rhi::Queue queue = rhi::Queue::Graphics;
    std::array<u32, kQueues> clock{};  // timeline value of each queue known complete at its start
    std::array<u32, kQueues> waits{};  // build indices, at most one per other queue
    u32 waitCount = 0;
    u32 value = 0;
    bool closed = false;
};

/// First/last accessing pass of a resource per queue (step 4).
struct Life {
    std::array<u32, kQueues> first;
    std::array<u32, kQueues> last;
    Life() {
        first.fill(kRgInvalid);
        last.fill(kRgInvalid);
    }
};

/// Barrier state of one physical subresource (step 7).
struct PSub {
    rhi::ResourceState state = kRgEntryState;
    u32 owner = kRgInvalid;
    bool writePending = false;
    rhi::Queue writeQueue = rhi::Queue::Graphics;
    u8 readQueues = 0;
    u32 lastPass = kRgInvalid;
    // Accesses since the last transition or write (all earlier ones are ordered before them).
    u8 sinceQueues = 0;
    std::array<u32, kQueues> lastOnQueue{kRgInvalid, kRgInvalid, kRgInvalid};
};

/// A barrier recorded after `pass` (step 7).
struct PostSub {
    u32 pass = kRgInvalid;
    RgSubBarrier sub;
};

} // namespace

/// What compile() keeps between compiles (see the header comment): every array is resized or cleared,
/// never freed, so it allocates only when a graph outgrows the storage.
struct RgCompileStorage {
    // Elements of the previous plan (RgPlan's vectors are emptied into these at the start of a compile).
    Spares<RgPassInfo> passes;
    Spares<RgResourceInfo> resources;
    Spares<RgPhysicalInfo> physicals;
    Spares<RgBatchInfo> batches;
    Spares<RgListInfo> lists;
    // 1. culling
    std::vector<u8> needed;
    std::vector<u32> stack;
    std::vector<u32> declOrder;  // non-culled passes, declaration order
    // 2. hazards
    std::vector<u32> crossDeps;     // per pass, crossRange: earlier passes on other queues it must wait for
    std::vector<Range> crossRange;
    std::vector<u8> hasConsumer;    // per pass: some pass on another queue waits for it
    std::vector<u32> vsubStart;     // per resource: its first entry in vsubs
    std::vector<VSub> vsubs;
    std::vector<Reader> readers;
    // 3. batches
    std::vector<Build> builds;
    std::vector<u32> closedOrder;
    std::vector<u32> buildOf;       // per pass
    std::vector<u32> buildStart;    // per build + 1: its first pass in buildPasses
    std::vector<u32> buildFill;
    std::vector<u32> buildPasses;
    std::vector<u32> submitIndex;
    std::vector<std::array<u32, kQueues>> clocks;  // per batch: queue values known complete at its start
    // 4. lifetimes, happens-before summary
    std::vector<Life> life;
    // Per resource and queue: the value of its last batch there (0: none), and the lowest clock entry for
    // that queue among its first accesses on the other queues (kNoConstraint: none).
    std::vector<std::array<u32, kQueues>> lastValue;
    std::vector<std::array<u32, kQueues>> needClock;
    // 5. aliasing, 6. placement
    std::vector<u32> firstStart;
    std::vector<u32> used;
    std::array<std::vector<u32>, kRgHeapKindCount> heaps;
    std::vector<u32> placed;
    std::vector<std::pair<u64, u64>> conflicts;  // [offset, end)
    // 7. barriers
    std::vector<u32> psubStart;  // per physical + 1
    std::vector<PSub> psubs;
    std::vector<RgSubBarrier> preSubs;
    std::vector<Range> preRange;  // per pass
    std::vector<PostSub> postSubs;
    std::vector<u32> postStart;   // per pass + 1
    std::vector<u32> postFill;
    std::vector<RgSubBarrier> postSorted;
    std::vector<RgSubBarrier> epilogue;
    std::vector<u32> epilogueWaits;
    RgMergeScratch merge;
    // 8. store ops, 9. partition
    std::vector<u32> readStart;  // per resource + 1: its first version in readLater
    std::vector<u8> readLater;
    std::vector<u32> listCounts;
};

void RgCompileStorageDeleter::operator()(RgCompileStorage* storage) const noexcept { delete storage; }

namespace {

/// Empties `plan`, moving its elements (and their storage) into `storage`.
void reclaimPlan(RgPlan& plan, RgCompileStorage& storage) {
    storage.passes.reclaim(plan.passes);
    storage.resources.reclaim(plan.resources);
    storage.physicals.reclaim(plan.physicals);
    storage.batches.reclaim(plan.batches);
    storage.lists.reclaim(plan.lists);
    plan.name.clear();
    plan.order.clear();
    plan.stats = RgCompileStats{};
}

class Compiler {
public:
    Compiler(RenderGraph::Impl& graph, const RgCompileOptions& options, RgCompileStorage& storage)
        : g(graph), opt(options), plan(graph.plan), ws(storage) {}

    void run() {
        initPlan();
        cull();
        hazards();
        buildBatches();
        lifetimes();
        aliasing();
        placement();
        barriers();
        storeOps();
        partition();
        stats();
    }

private:
    RenderGraph::Impl& g;
    const RgCompileOptions& opt;
    RgPlan& plan;          // g.plan, rebuilt in place
    RgCompileStorage& ws;  // scratch and spare plan elements
    u32 passCount = 0;
    u32 resourceCount = 0;

    rhi::Queue queueOf(u32 pass) const { return plan.passes[pass].queue; }
    u32 positionOf(u32 pass) const { return plan.passes[pass].position; }

    // -- 0. plan skeleton ----------------------------------------------------------------------
    void initPlan() {
        reclaimPlan(plan, ws);
        plan.name.assign(g.name);
        passCount = static_cast<u32>(g.passes.size());
        resourceCount = static_cast<u32>(g.resources.size());
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceDecl& d = g.resources[r];
            RgResourceInfo& info = ws.resources.append(plan.resources);
            info.name.assign(d.name);
            info.isTexture = d.isTexture;
            info.imported = d.imported;
            info.texture = d.texture;
            info.bufferSize = d.bufferSize;
            info.textureUsage = d.textureUsage;
            info.bufferUsage = d.bufferUsage;
            info.versionCount = static_cast<u32>(d.producers.size());
            info.import = d.import;
        }
        for (u32 p = 0; p < passCount; ++p) {
            const RgPassDecl& d = g.passes[p];
            RgPassInfo& info = ws.passes.append(plan.passes);
            info.name.assign(d.name);
            info.flags = d.flags;
            info.queue = hasFlag(d.flags, PassFlags::AsyncCompute) && opt.asyncCompute ? rhi::Queue::AsyncCompute
                                                                                        : rhi::Queue::Graphics;
            info.accesses.reserve(d.accesses.size());
            for (const RgAccessDecl& a : d.accesses) info.accesses.push_back(a.access);
            info.colorStore.assign(d.colors.size(), rhi::StoreOp::Store);
        }
    }

    // -- 1. culling ----------------------------------------------------------------------------
    void cull() {
        ws.needed.assign(passCount, static_cast<u8>(opt.cull ? 0 : 1));
        ws.declOrder.clear();
        if (opt.cull) {
            ws.stack.clear();
            auto mark = [&](u32 p) {
                if (p != kRgInvalid && !ws.needed[p]) {
                    ws.needed[p] = 1;
                    ws.stack.push_back(p);
                }
            };
            for (u32 p = 0; p < passCount; ++p) {
                if (hasFlag(g.passes[p].flags, PassFlags::NeverCull)) mark(p);
                for (const RgAccess& a : plan.passes[p].accesses) {
                    const RgResourceDecl& r = g.resources[a.resource];
                    if (a.isWrite() && (r.imported || r.outputs[a.writeVersion])) mark(p);
                }
            }
            while (!ws.stack.empty()) {
                const u32 p = ws.stack.back();
                ws.stack.pop_back();
                for (const RgAccess& a : plan.passes[p].accesses) {
                    if (a.readVersion != kRgInvalid) mark(g.resources[a.resource].producers[a.readVersion]);
                }
            }
        }
        for (u32 p = 0; p < passCount; ++p) {
            plan.passes[p].culled = !ws.needed[p];
            if (ws.needed[p]) ws.declOrder.push_back(p);
        }
    }

    // -- 2. cross-queue hazards on the virtual resources -------------------------------------------
    // Per subresource in declaration order: RAW, WAW and WAR hazards plus state changes (a layout
    // change is a write). A state change a queue cannot perform itself (graphics-only source state
    // on async compute) happens on the queue of the last access (a release), so later accesses
    // depend on that site. Only hazards between different queues need timeline waits.
    void hazards() {
        ws.crossDeps.clear();
        ws.crossRange.assign(passCount, Range{});
        ws.hasConsumer.assign(passCount, 0);
        ws.vsubStart.resize(resourceCount);
        u32 subCount = 0;
        for (u32 r = 0; r < resourceCount; ++r) {
            ws.vsubStart[r] = subCount;
            subCount += g.resources[r].subresourceCount();
        }
        ws.vsubs.assign(subCount, VSub{});
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceDecl& d = g.resources[r];
            if (!d.imported) continue;
            VSub* subs = ws.vsubs.data() + ws.vsubStart[r];
            for (u32 s = 0; s < d.subresourceCount(); ++s) subs[s].state = d.import.initialState;
        }
        ws.readers.clear();
        for (u32 p : ws.declOrder) {
            const rhi::Queue q = queueOf(p);
            const u32 begin = static_cast<u32>(ws.crossDeps.size());
            auto dependOn = [&](const Site& d) {
                if (d.valid() && d.pass != p && d.queue != q) ws.crossDeps.push_back(d.pass);
            };
            for (const RgAccess& a : plan.passes[p].accesses) {
                const u32 layers = g.resources[a.resource].arrayLayers();
                VSub* subs = ws.vsubs.data() + ws.vsubStart[a.resource];
                forEachSub(a.range, layers, [&](u32 s, u32, u32) {
                    VSub& v = subs[s];
                    const Site lastWrite = v.lastWrite;
                    u32 readers = kRgInvalid;  // the reads this access depends on
                    if (a.isWrite()) {
                        readers = v.readers;
                        v.lastWrite = {p, q};
                        v.readers = kRgInvalid;
                        v.state = a.state;
                    } else {
                        if (a.state != v.state) {
                            readers = v.readers;
                            Site site{p, q};
                            if (q != rhi::Queue::Graphics && rgStateLevel(v.state) > rgQueueLevel(q)) {
                                // Released by the last access (graphics); with none in this graph the
                                // transition happens in the execute-time prologue (graphics, first).
                                site = v.lastAccess;
                            }
                            v.lastWrite = site;
                            v.readers = kRgInvalid;
                            v.state = a.state;
                        }
                        ws.readers.push_back({{p, q}, v.readers});
                        v.readers = static_cast<u32>(ws.readers.size() - 1);
                    }
                    v.lastAccess = {p, q};
                    dependOn(lastWrite);
                    for (u32 n = readers; n != kRgInvalid; n = ws.readers[n].next) dependOn(ws.readers[n].site);
                });
            }
            const auto first = ws.crossDeps.begin() + begin;
            std::sort(first, ws.crossDeps.end());
            ws.crossDeps.erase(std::unique(first, ws.crossDeps.end()), ws.crossDeps.end());
            const u32 end = static_cast<u32>(ws.crossDeps.size());
            ws.crossRange[p] = {begin, end};
            for (u32 i = begin; i < end; ++i) ws.hasConsumer[ws.crossDeps[i]] = 1;
        }
    }

    // -- 3. batches, waits and vector clocks ----------------------------------------------------------
    // One open batch per queue. Every batch carries a vector clock: the timeline value of each queue
    // known complete when it starts (from its waits and, through queue order, from earlier batches
    // on its queue). A pass whose cross-queue producers are not all implied by its queue's clock
    // closes the open batch and starts a new one that waits for the latest producer batch per queue
    // (waits happen at batch starts; waits implied by another wait's clock are dropped). A pass
    // another queue waits for closes its batch right after it, so the waiter waits for no more
    // than needed. Batches are submitted in the order they close, which respects every wait.
    void buildBatches() {
        ws.builds.clear();
        ws.closedOrder.clear();
        ws.buildOf.assign(passCount, kRgInvalid);
        std::array<u32, kQueues> open;
        std::array<u32, kQueues> lastOnQueue;
        std::array<u32, kQueues> counter{};
        open.fill(kRgInvalid);
        lastOnQueue.fill(kRgInvalid);
        auto close = [&](u32 b) {
            if (b == kRgInvalid || ws.builds[b].closed) return;
            Build& build = ws.builds[b];
            const u32 qi = rhi::queueIndex(build.queue);
            build.closed = true;
            build.value = ++counter[qi];
            ws.closedOrder.push_back(b);
            if (open[qi] == b) open[qi] = kRgInvalid;
        };
        auto startBuild = [&](rhi::Queue queue, std::span<const u32> waits) {
            HELIOS_ASSERT(waits.size() <= kQueues);
            const u32 qi = rhi::queueIndex(queue);
            Build build;
            build.queue = queue;
            if (lastOnQueue[qi] != kRgInvalid) build.clock = ws.builds[lastOnQueue[qi]].clock;
            for (u32 w : waits) {
                const Build& waited = ws.builds[w];
                const u32 wq = rhi::queueIndex(waited.queue);
                for (u32 k = 0; k < kQueues; ++k) build.clock[k] = std::max(build.clock[k], waited.clock[k]);
                build.clock[wq] = std::max(build.clock[wq], waited.value);
                build.waits[build.waitCount++] = w;
            }
            const u32 index = static_cast<u32>(ws.builds.size());
            ws.builds.push_back(build);
            open[qi] = index;
            lastOnQueue[qi] = index;
        };
        for (u32 p : ws.declOrder) {
            const rhi::Queue q = queueOf(p);
            const u32 qi = rhi::queueIndex(q);
            // Latest producer batch per other queue.
            std::array<u32, kQueues> best;
            best.fill(kRgInvalid);
            const Range deps = ws.crossRange[p];
            for (u32 i = deps.begin; i < deps.end; ++i) {
                const u32 bd = ws.buildOf[ws.crossDeps[i]];
                close(bd);  // already closed: producers with consumers close right after them
                const u32 dq = rhi::queueIndex(ws.builds[bd].queue);
                if (best[dq] == kRgInvalid || ws.builds[bd].value > ws.builds[best[dq]].value) best[dq] = bd;
            }
            std::array<u32, kQueues> base{};
            if (open[qi] != kRgInvalid) {
                base = ws.builds[open[qi]].clock;
            } else if (lastOnQueue[qi] != kRgInvalid) {
                base = ws.builds[lastOnQueue[qi]].clock;
            }
            std::array<u32, kQueues> missing{};  // producer batches not implied by the queue's clock
            u32 missingCount = 0;
            for (u32 dq = 0; dq < kQueues; ++dq) {
                if (best[dq] != kRgInvalid && base[dq] < ws.builds[best[dq]].value) {
                    missing[missingCount++] = best[dq];
                }
            }
            if (missingCount != 0) {
                close(open[qi]);
                std::array<u32, kQueues> waits{};
                u32 waitCount = 0;
                for (u32 i = 0; i < missingCount; ++i) {
                    const u32 w = missing[i];
                    const u32 wq = rhi::queueIndex(ws.builds[w].queue);
                    bool implied = false;
                    for (u32 j = 0; j < missingCount; ++j) {
                        const u32 other = missing[j];
                        if (other != w && ws.builds[other].clock[wq] >= ws.builds[w].value) implied = true;
                    }
                    if (!implied) waits[waitCount++] = w;
                }
                startBuild(q, {waits.data(), waitCount});
            } else if (open[qi] == kRgInvalid) {
                startBuild(q, {});
            }
            ws.buildOf[p] = open[qi];
            if (ws.hasConsumer[p]) close(open[qi]);
        }
        const u32 buildCount = static_cast<u32>(ws.builds.size());
        for (u32 b = 0; b < buildCount; ++b) close(b);

        // The passes of each build in the order they joined it: a stable counting sort of declOrder.
        ws.buildStart.assign(buildCount + 1, 0);
        for (u32 p : ws.declOrder) ++ws.buildStart[ws.buildOf[p] + 1];
        for (u32 b = 0; b < buildCount; ++b) ws.buildStart[b + 1] += ws.buildStart[b];
        ws.buildFill.assign(ws.buildStart.begin(), ws.buildStart.end() - 1);
        ws.buildPasses.resize(ws.declOrder.size());
        for (u32 p : ws.declOrder) ws.buildPasses[ws.buildFill[ws.buildOf[p]]++] = p;

        ws.submitIndex.assign(buildCount, kRgInvalid);
        for (u32 i = 0; i < buildCount; ++i) ws.submitIndex[ws.closedOrder[i]] = i;
        ws.clocks.resize(buildCount);
        for (u32 i = 0; i < buildCount; ++i) {
            const u32 index = ws.closedOrder[i];
            const Build& b = ws.builds[index];
            RgBatchInfo& batch = ws.batches.append(plan.batches);
            batch.kind = RgBatchInfo::Kind::Passes;
            batch.queue = b.queue;
            batch.passes.assign(ws.buildPasses.begin() + ws.buildStart[index],
                                ws.buildPasses.begin() + ws.buildStart[index + 1]);
            batch.queueValue = b.value;
            for (u32 k = 0; k < b.waitCount; ++k) batch.waits.push_back(ws.submitIndex[b.waits[k]]);
            std::sort(batch.waits.begin(), batch.waits.end());
            ws.clocks[i] = b.clock;
            for (u32 p : batch.passes) {
                plan.passes[p].batch = i;
                plan.passes[p].position = static_cast<u32>(plan.order.size());
                plan.order.push_back(p);
            }
        }
    }

    // -- 4. lifetimes and the happens-before summary ----------------------------------------------------
    // Pass a (earlier in submission order) happens before pass b when they share a queue (a barrier at
    // b orders them) or b's batch waits (transitively) for a's batch: b's clock holds a's batch value
    // on a's queue. aliasing() and placement() ask whether every access of a resource x happens
    // before every access of r, which is that relation for each pair (last access of x on a queue,
    // first access of r on a queue). Folded per resource, it is: x's last access is earlier than r's
    // first, and on every queue x used, its last batch value there is at most needClock[r], the
    // lowest clock (on that queue) among r's first accesses on the other queues.
    void lifetimes() {
        ws.life.assign(resourceCount, Life{});
        for (u32 p : plan.order) {
            const u32 qi = rhi::queueIndex(queueOf(p));
            const u32 pos = positionOf(p);
            for (const RgAccess& a : plan.passes[p].accesses) {
                RgResourceInfo& r = plan.resources[a.resource];
                r.used = true;
                if (r.firstPosition == kRgInvalid) r.firstPosition = pos;
                r.lastPosition = pos;
                Life& l = ws.life[a.resource];
                if (l.first[qi] == kRgInvalid) l.first[qi] = p;
                l.last[qi] = p;
            }
        }
        ws.lastValue.resize(resourceCount);
        ws.needClock.resize(resourceCount);
        for (u32 r = 0; r < resourceCount; ++r) {
            RgResourceInfo& info = plan.resources[r];
            if (!info.imported) {
                info.bytes =
                    alignUp(info.isTexture ? rgTextureBytes(info.texture) : info.bufferSize, kPlacementAlignment);
            }
            const Life& l = ws.life[r];
            std::array<u32, kQueues>& last = ws.lastValue[r];
            std::array<u32, kQueues>& need = ws.needClock[r];
            for (u32 qa = 0; qa < kQueues; ++qa) {
                const u32 lastPass = l.last[qa];
                last[qa] = lastPass == kRgInvalid ? 0u : plan.batches[plan.passes[lastPass].batch].queueValue;
                need[qa] = kNoConstraint;
                for (u32 qb = 0; qb < kQueues; ++qb) {
                    if (qb == qa || l.first[qb] == kRgInvalid) continue;
                    need[qa] = std::min(need[qa], ws.clocks[plan.passes[l.first[qb]].batch][qa]);
                }
            }
        }
    }

    /// Every access of used resource x happens before every access of used resource r (see lifetimes()).
    bool hbAll(u32 x, u32 r) const {
        if (plan.resources[x].lastPosition >= plan.resources[r].firstPosition) return false;
        const std::array<u32, kQueues>& last = ws.lastValue[x];
        const std::array<u32, kQueues>& need = ws.needClock[r];
        for (u32 q = 0; q < kQueues; ++q) {
            if (last[q] > need[q]) return false;
        }
        return true;
    }

    // -- 5. aliasing (pooled physical resources) ------------------------------------------------------
    void aliasing() {
        // Used resources by first position, then index: a counting sort by first position.
        const u32 orderSize = static_cast<u32>(plan.order.size());
        ws.firstStart.assign(orderSize + 1, 0);
        u32 usedCount = 0;
        for (u32 r = 0; r < resourceCount; ++r) {
            if (!plan.resources[r].used) continue;
            ++ws.firstStart[plan.resources[r].firstPosition + 1];
            ++usedCount;
        }
        for (u32 i = 0; i < orderSize; ++i) ws.firstStart[i + 1] += ws.firstStart[i];
        ws.used.resize(usedCount);
        for (u32 r = 0; r < resourceCount; ++r) {
            if (plan.resources[r].used) ws.used[ws.firstStart[plan.resources[r].firstPosition]++] = r;
        }
        for (u32 r : ws.used) {
            RgResourceInfo& info = plan.resources[r];
            u32 target = kRgInvalid;
            if (!info.imported && opt.alias) {
                for (u32 i = 0; i < plan.physicals.size() && target == kRgInvalid; ++i) {
                    const RgPhysicalInfo& phys = plan.physicals[i];
                    if (phys.imported || phys.isTexture != info.isTexture) continue;
                    if (info.isTexture ? !(phys.texture == info.texture) : phys.bufferSize != info.bufferSize) continue;
                    if (hbAll(phys.residents.back(), r)) target = i;
                }
            }
            if (target == kRgInvalid) {
                target = static_cast<u32>(plan.physicals.size());
                RgPhysicalInfo& phys = ws.physicals.append(plan.physicals);
                phys.isTexture = info.isTexture;
                phys.imported = info.imported;
                phys.texture = info.texture;
                phys.bufferSize = info.bufferSize;
                phys.bytes = info.bytes;
            }
            RgPhysicalInfo& phys = plan.physicals[target];
            phys.residents.push_back(r);
            phys.textureUsage |= info.textureUsage;
            phys.bufferUsage |= info.bufferUsage;
            info.physical = target;
        }
        // Imported resources with a final state need a physical even when no pass touches them.
        for (u32 r = 0; r < resourceCount; ++r) {
            RgResourceInfo& info = plan.resources[r];
            if (!info.imported || info.used || info.import.finalState == kRgEntryState) continue;
            info.physical = static_cast<u32>(plan.physicals.size());
            RgPhysicalInfo& phys = ws.physicals.append(plan.physicals);
            phys.isTexture = info.isTexture;
            phys.imported = true;
            phys.texture = info.texture;
            phys.bufferSize = info.bufferSize;
            phys.residents.push_back(r);
        }
        for (RgPhysicalInfo& phys : plan.physicals) phys.finalStates.assign(phys.subresourceCount(), kRgEntryState);
    }

    // -- 6. placement plan (placed-resource aliasing, informational) ---------------------------------
    void placement() {
        for (std::vector<u32>& items : ws.heaps) items.clear();
        for (u32 r = 0; r < resourceCount; ++r) {
            RgResourceInfo& info = plan.resources[r];
            if (!info.used || info.imported) continue;
            RgHeapKind kind = RgHeapKind::Buffers;
            if (info.isTexture) {
                kind = hasAnyFlag(info.textureUsage, rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::DepthStencil)
                           ? RgHeapKind::RenderTargets
                           : RgHeapKind::Textures;
            }
            info.heap = kind;
            ws.heaps[static_cast<u32>(kind)].push_back(r);
        }
        for (u32 h = 0; h < kRgHeapKindCount; ++h) {
            std::vector<u32>& items = ws.heaps[h];
            std::sort(items.begin(), items.end(), [&](u32 a, u32 b) {
                const RgResourceInfo& ra = plan.resources[a];
                const RgResourceInfo& rb = plan.resources[b];
                if (ra.bytes != rb.bytes) return ra.bytes > rb.bytes;
                if (ra.firstPosition != rb.firstPosition) return ra.firstPosition < rb.firstPosition;
                return a < b;
            });
            ws.placed.clear();
            u64 heapSize = 0;
            for (u32 r : items) {
                RgResourceInfo& info = plan.resources[r];
                ws.conflicts.clear();
                for (u32 o : ws.placed) {
                    if (!(hbAll(o, r) || hbAll(r, o))) {
                        ws.conflicts.emplace_back(plan.resources[o].heapOffset,
                                                  plan.resources[o].heapOffset + plan.resources[o].bytes);
                    }
                }
                std::sort(ws.conflicts.begin(), ws.conflicts.end());
                u64 offset = 0;
                for (const auto& [begin, end] : ws.conflicts) {
                    if (offset + info.bytes <= begin) break;
                    offset = std::max(offset, end);
                }
                info.heapOffset = offset;
                heapSize = std::max(heapSize, offset + info.bytes);
                ws.placed.push_back(r);
            }
            plan.stats.heapBytes[h] = heapSize;
        }
    }

    // -- 7. barriers over physical resources ------------------------------------------------------------
    // Simulated in declaration order, like the hazard analysis that placed the cross-queue waits:
    // two accesses that submission order swaps relative to declaration order are unordered reads
    // in the same state with no transition between them, so every transition lands where the waits
    // expect it (the stress test's reference simulator checks exactly this).
    void barriers() {
        const u32 physCount = static_cast<u32>(plan.physicals.size());
        ws.psubStart.resize(physCount + 1);
        u32 subCount = 0;
        for (u32 i = 0; i < physCount; ++i) {
            ws.psubStart[i] = subCount;
            subCount += plan.physicals[i].subresourceCount();
        }
        ws.psubStart[physCount] = subCount;
        ws.psubs.assign(subCount, PSub{});
        ws.preSubs.clear();
        ws.preRange.assign(passCount, Range{});
        ws.postSubs.clear();

        for (u32 p : ws.declOrder) {
            const rhi::Queue q = queueOf(p);
            const u32 qi = rhi::queueIndex(q);
            const u8 qbit = static_cast<u8>(1u << qi);
            const u32 preBegin = static_cast<u32>(ws.preSubs.size());
            for (const RgAccess& a : plan.passes[p].accesses) {
                const u32 phys = plan.resources[a.resource].physical;
                const u32 layers = plan.physicals[phys].isTexture ? plan.physicals[phys].texture.arrayLayers : 1u;
                const bool write = a.isWrite();
                PSub* subs = ws.psubs.data() + ws.psubStart[phys];
                forEachSub(a.range, layers, [&](u32 s, u32 mip, u32 layer) {
                    PSub& st = subs[s];
                    bool transitioned = false;
                    if (st.state == kRgEntryState) {
                        ws.preSubs.push_back({phys, kRgEntryState, a.state, mip, layer});
                        transitioned = true;
                    } else {
                        const bool need = st.owner != a.resource || a.state != st.state ||
                                          (st.writePending && st.writeQueue == q) || (write && (st.readQueues & qbit));
                        if (need) {
                            if (q == rhi::Queue::Graphics || rgStateLevel(st.state) <= rgQueueLevel(q)) {
                                ws.preSubs.push_back({phys, st.state, a.state, mip, layer});
                            } else {
                                // Graphics-only source state on async compute: release on the queue
                                // of the last access (always graphics for such states).
                                const bool releasable =
                                    st.lastPass != kRgInvalid && queueOf(st.lastPass) == rhi::Queue::Graphics;
                                HELIOS_ASSERT(releasable);
                                if (releasable) {
                                    ws.postSubs.push_back({st.lastPass, {phys, st.state, a.state, mip, layer}});
                                } else {
                                    ws.preSubs.push_back({phys, st.state, a.state, mip, layer});  // unreachable
                                }
                            }
                            transitioned = true;
                        }
                    }
                    if (transitioned || write) {
                        st.state = a.state;
                        st.writePending = false;
                        st.readQueues = 0;
                        st.sinceQueues = 0;
                    }
                    st.owner = a.resource;
                    if (write) {
                        st.writePending = true;
                        st.writeQueue = q;
                    } else {
                        st.readQueues = static_cast<u8>(st.readQueues | qbit);
                    }
                    st.lastPass = p;
                    st.sinceQueues = static_cast<u8>(st.sinceQueues | qbit);
                    st.lastOnQueue[qi] = p;
                });
            }
            ws.preRange[p] = {preBegin, static_cast<u32>(ws.preSubs.size())};
        }

        // Final states of imported resources: after the last access when a single queue holds the
        // accesses since the last transition and can perform it; otherwise in a graphics epilogue
        // that waits for the last access on every other queue.
        ws.epilogue.clear();
        ws.epilogueWaits.clear();
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceInfo& info = plan.resources[r];
            if (!info.imported || info.physical == kRgInvalid || info.import.finalState == kRgEntryState) continue;
            const u32 phys = info.physical;
            const rhi::ResourceState fin = info.import.finalState;
            const RgPhysicalInfo& pi = plan.physicals[phys];
            const u32 layers = pi.isTexture ? pi.texture.arrayLayers : 1u;
            const u32 mips = pi.isTexture ? pi.texture.mipLevels : 1u;
            PSub* subs = ws.psubs.data() + ws.psubStart[phys];
            forEachSub({0, mips, 0, layers}, layers, [&](u32 s, u32 mip, u32 layer) {
                PSub& st = subs[s];
                if (st.state == kRgEntryState) {
                    ws.epilogue.push_back({phys, kRgEntryState, fin, mip, layer});
                } else if (st.state != fin) {
                    u32 single = kRgInvalid;
                    u32 queues = 0;
                    for (u32 k = 0; k < kQueues; ++k) {
                        if (st.sinceQueues & (1u << k)) {
                            single = k;
                            ++queues;
                        }
                    }
                    const rhi::Queue lq = static_cast<rhi::Queue>(single);
                    if (queues == 1 && (lq == rhi::Queue::Graphics || (rgStateLevel(fin) <= rgQueueLevel(lq) &&
                                                                       rgStateLevel(st.state) <= rgQueueLevel(lq)))) {
                        ws.postSubs.push_back({st.lastOnQueue[single], {phys, st.state, fin, mip, layer}});
                    } else {
                        ws.epilogue.push_back({phys, st.state, fin, mip, layer});
                        for (u32 k = 0; k < kQueues; ++k) {
                            if ((st.sinceQueues & (1u << k)) && k != rhi::queueIndex(rhi::Queue::Graphics)) {
                                ws.epilogueWaits.push_back(plan.passes[st.lastOnQueue[k]].batch);
                            }
                        }
                    }
                }
                st.state = fin;
            });
        }

        // Post-pass barriers per pass, in the order they were found (a stable counting sort).
        ws.postStart.assign(passCount + 1, 0);
        for (const PostSub& e : ws.postSubs) ++ws.postStart[e.pass + 1];
        for (u32 p = 0; p < passCount; ++p) ws.postStart[p + 1] += ws.postStart[p];
        ws.postFill.assign(ws.postStart.begin(), ws.postStart.end() - 1);
        ws.postSorted.resize(ws.postSubs.size());
        for (const PostSub& e : ws.postSubs) ws.postSorted[ws.postFill[e.pass]++] = e.sub;

        for (u32 p : plan.order) {
            RgPassInfo& info = plan.passes[p];
            const Range pre = ws.preRange[p];
            const std::span<const RgSubBarrier> post(ws.postSorted.data() + ws.postStart[p],
                                                     ws.postSorted.data() + ws.postStart[p + 1]);
            rgMergeBarriers(std::span(ws.preSubs).subspan(pre.begin, pre.end - pre.begin), plan.physicals,
                            info.preBarriers, ws.merge);
            rgMergeBarriers(post, plan.physicals, info.postBarriers, ws.merge);
        }
        if (!ws.epilogue.empty()) {
            // Keep only the latest batch per queue (earlier ones are implied by queue order).
            std::array<u32, kQueues> best;
            best.fill(kRgInvalid);
            for (u32 w : ws.epilogueWaits) {
                const u32 wq = rhi::queueIndex(plan.batches[w].queue);
                if (best[wq] == kRgInvalid || plan.batches[w].queueValue > plan.batches[best[wq]].queueValue) best[wq] = w;
            }
            u32 graphicsBatches = 0;
            for (const RgBatchInfo& b : plan.batches) graphicsBatches += b.queue == rhi::Queue::Graphics ? 1u : 0u;
            RgBatchInfo& batch = ws.batches.append(plan.batches);
            batch.kind = RgBatchInfo::Kind::Epilogue;
            batch.queue = rhi::Queue::Graphics;
            rgMergeBarriers(ws.epilogue, plan.physicals, batch.barriers, ws.merge);
            for (u32 w : best) {
                if (w != kRgInvalid) batch.waits.push_back(w);
            }
            std::sort(batch.waits.begin(), batch.waits.end());
            batch.queueValue = graphicsBatches + 1;
        }
        for (u32 i = 0; i < physCount; ++i) {
            std::vector<rhi::ResourceState>& states = plan.physicals[i].finalStates;
            for (u32 s = 0; s < states.size(); ++s) states[s] = ws.psubs[ws.psubStart[i] + s].state;
        }
    }

    // -- 8. store ops: DontCare when the written version is never read again ---------------------------
    void storeOps() {
        ws.readStart.resize(resourceCount + 1);
        u32 versions = 0;
        for (u32 r = 0; r < resourceCount; ++r) {
            ws.readStart[r] = versions;
            versions += static_cast<u32>(g.resources[r].producers.size());
        }
        ws.readStart[resourceCount] = versions;
        ws.readLater.assign(versions, 0);
        for (u32 p : plan.order) {
            for (const RgAccess& a : plan.passes[p].accesses) {
                if (a.readVersion != kRgInvalid) ws.readLater[ws.readStart[a.resource] + a.readVersion] = 1;
            }
        }
        auto store = [&](u32 p, const RgAttachmentDecl& att, rhi::ResourceState state) {
            for (const RgAccess& a : plan.passes[p].accesses) {
                if (a.resource != att.resource || a.state != state || a.range.baseMip != att.mip ||
                    a.range.baseLayer != att.layer || !a.isWrite()) {
                    continue;
                }
                const RgResourceDecl& r = g.resources[a.resource];
                const bool readLater = ws.readLater[ws.readStart[a.resource] + a.writeVersion] != 0;
                const bool keep = r.imported || r.outputs[a.writeVersion] || readLater;
                return keep ? rhi::StoreOp::Store : rhi::StoreOp::DontCare;
            }
            return rhi::StoreOp::Store;
        };
        for (u32 p : plan.order) {
            const RgPassDecl& d = g.passes[p];
            RgPassInfo& info = plan.passes[p];
            for (u32 i = 0; i < d.colors.size(); ++i) {
                if (d.colors[i].resource != kRgInvalid) info.colorStore[i] = store(p, d.colors[i], rhi::ResourceState::RenderTarget);
            }
            if (d.depth.resource != kRgInvalid && !d.depth.readOnly) {
                info.depthStore = store(p, d.depth, rhi::ResourceState::DepthWrite);
            }
        }
    }

    // -- 9. command lists -----------------------------------------------------------------------------
    void partition() {
        const u32 batchCount = static_cast<u32>(plan.batches.size());
        ws.listCounts.assign(batchCount, 1);
        u32 passBatches = 0;
        for (const RgBatchInfo& b : plan.batches) passBatches += b.kind == RgBatchInfo::Kind::Passes ? 1u : 0u;
        u32 remaining = opt.maxCommandLists > passBatches ? opt.maxCommandLists - passBatches : 0;
        while (remaining > 0) {
            u32 best = kRgInvalid;
            for (u32 b = 0; b < batchCount; ++b) {
                const RgBatchInfo& batch = plan.batches[b];
                const std::vector<u32>& counts = ws.listCounts;
                if (batch.kind != RgBatchInfo::Kind::Passes || counts[b] >= batch.passes.size()) continue;
                // Largest passes-per-list ratio first (cross-multiplied to stay in integers).
                if (best == kRgInvalid ||
                    batch.passes.size() * counts[best] > plan.batches[best].passes.size() * counts[b]) {
                    best = b;
                }
            }
            if (best == kRgInvalid) break;
            ++ws.listCounts[best];
            --remaining;
        }
        for (u32 b = 0; b < batchCount; ++b) {
            RgBatchInfo& batch = plan.batches[b];
            batch.firstList = static_cast<u32>(plan.lists.size());
            if (batch.kind != RgBatchInfo::Kind::Passes) {
                batch.listCount = 1;
                RgListInfo& list = ws.lists.append(plan.lists);
                list.batch = b;
                list.name.assign(g.name).append(":epilogue");
                continue;
            }
            const u32 n = static_cast<u32>(batch.passes.size());
            const u32 k = ws.listCounts[b];
            batch.listCount = k;
            for (u32 i = 0; i < k; ++i) {
                const u32 begin = i * n / k;
                const u32 end = (i + 1) * n / k;
                const u32 index = static_cast<u32>(plan.lists.size());
                RgListInfo& list = ws.lists.append(plan.lists);
                list.batch = b;
                list.firstPass = begin;
                list.passCount = end - begin;
                // "<graph>:<first pass>[+<more passes>]"
                list.name.assign(g.name).append(1, ':').append(plan.passes[batch.passes[begin]].name);
                if (end - begin > 1) {
                    list.name += '+';
                    appendDecimal(list.name, end - begin - 1);
                }
                for (u32 j = begin; j < end; ++j) plan.passes[batch.passes[j]].list = index;
            }
        }
    }

    void stats() {
        RgCompileStats& s = plan.stats;
        s.passCount = passCount;
        s.culledPassCount = passCount - static_cast<u32>(plan.order.size());
        s.batchCount = static_cast<u32>(plan.batches.size());
        s.commandListCount = static_cast<u32>(plan.lists.size());
        s.barrierCount = 0;
        for (const RgPassInfo& p : plan.passes) {
            s.barrierCount += static_cast<u32>(p.preBarriers.size() + p.postBarriers.size());
        }
        s.crossQueueWaitCount = 0;
        for (const RgBatchInfo& b : plan.batches) {
            s.barrierCount += static_cast<u32>(b.barriers.size());
            s.crossQueueWaitCount += static_cast<u32>(b.waits.size());
        }
        for (const RgResourceInfo& r : plan.resources) {
            if (r.used && !r.imported) s.transientBytes += r.bytes;
        }
        for (const RgPhysicalInfo& p : plan.physicals) {
            if (p.imported) continue;
            s.pooledBytes += p.bytes;
            (p.isTexture ? s.physicalTextureCount : s.physicalBufferCount) += 1;
        }
        s.placedBytes = 0;
        for (u64 h : s.heapBytes) s.placedBytes += h;
    }
};

} // namespace

void rgReclaimPlan(RenderGraph::Impl& graph) {
    if (graph.compileStorage) {
        reclaimPlan(graph.plan, *graph.compileStorage);
    } else {
        graph.plan = RgPlan{};
    }
}

Result<void> RenderGraph::compile(const RgCompileOptions& options) {
    Impl& g = *m_impl;
    const Stopwatch timer;
    g.compiled = false;
    if (!g.errors.empty()) {
        std::string message = std::format("render graph '{}' has {} setup error(s):", g.name, g.errors.size());
        for (const std::string& e : g.errors) message += "\n  " + e;
        return Error{ErrorCode::InvalidArgument, std::move(message)};
    }
    g.options = options;
    if (!g.compileStorage) g.compileStorage.reset(new RgCompileStorage());
    Compiler(g, options, *g.compileStorage).run();
    g.plan.stats.compileMs = timer.elapsedMillis();
    g.executed = RgPlan{};
    g.compiled = true;
    return {};
}

// ---------------------------------------------------------------------------------------------
// Entry-barrier resolution
// ---------------------------------------------------------------------------------------------
RgPlan resolveEntryBarriers(const RgPlan& plan, std::span<const std::vector<rhi::ResourceState>> physicalStates) {
    RgPlan out = plan;
    std::vector<RgSubBarrier> prologue;
    std::vector<u8> needsPrologue(out.batches.size(), 0);

    auto stateOf = [&](u32 phys, u32 sub) {
        if (phys < physicalStates.size() && sub < physicalStates[phys].size()) return physicalStates[phys][sub];
        return rhi::ResourceState::Undefined;
    };
    auto resolve = [&](std::vector<RgBarrier>& list, rhi::Queue queue, u32 batch) {
        bool hasEntry = false;
        for (const RgBarrier& b : list) hasEntry |= b.before == kRgEntryState;
        if (!hasEntry) return;
        std::vector<RgSubBarrier> subs;
        for (const RgBarrier& b : list) {
            const RgPhysicalInfo& phys = out.physicals[b.physical];
            const u32 layers = phys.isTexture ? phys.texture.arrayLayers : 1u;
            forEachSub(b.range, layers, [&](u32 s, u32 mip, u32 layer) {
                rhi::ResourceState before = b.before;
                if (before == kRgEntryState) {
                    before = stateOf(b.physical, s);
                    if (before == b.after && rgIsReadOnlyState(b.after)) return;  // already there, nothing written
                    if (queue != rhi::Queue::Graphics && rgStateLevel(before) > rgQueueLevel(queue)) {
                        prologue.push_back({b.physical, before, b.after, mip, layer});
                        if (batch != kRgInvalid) needsPrologue[batch] = 1;
                        return;
                    }
                }
                subs.push_back({b.physical, before, b.after, mip, layer});
            });
        }
        list = rgMergeBarriers(subs, out.physicals);
    };
    for (u32 p : out.order) {
        RgPassInfo& pass = out.passes[p];
        resolve(pass.preBarriers, pass.queue, pass.batch);
        HELIOS_ASSERT(std::none_of(pass.postBarriers.begin(), pass.postBarriers.end(),
                                   [](const RgBarrier& b) { return b.before == kRgEntryState; }));
    }
    for (u32 b = 0; b < out.batches.size(); ++b) {
        RgBatchInfo& batch = out.batches[b];
        if (batch.kind != RgBatchInfo::Kind::Passes) resolve(batch.barriers, batch.queue, b);
    }

    if (!prologue.empty()) {
        // Insert the prologue as batch 0 (graphics, one list) and renumber.
        RgBatchInfo pro;
        pro.kind = RgBatchInfo::Kind::Prologue;
        pro.queue = rhi::Queue::Graphics;
        pro.barriers = rgMergeBarriers(prologue, out.physicals);
        pro.queueValue = 1;
        pro.firstList = 0;
        pro.listCount = 1;
        for (RgBatchInfo& batch : out.batches) {
            for (u32& w : batch.waits) ++w;
            batch.firstList += 1;
            if (batch.queue == rhi::Queue::Graphics) ++batch.queueValue;
        }
        for (u32 b = 0; b < out.batches.size(); ++b) {
            RgBatchInfo& batch = out.batches[b];
            if (needsPrologue[b] && batch.queue != rhi::Queue::Graphics) batch.waits.insert(batch.waits.begin(), 0u);
        }
        out.batches.insert(out.batches.begin(), std::move(pro));
        for (RgPassInfo& pass : out.passes) {
            if (pass.batch != kRgInvalid) ++pass.batch;
            if (pass.list != kRgInvalid) ++pass.list;
        }
        for (RgListInfo& list : out.lists) ++list.batch;
        out.lists.insert(out.lists.begin(), RgListInfo{0, 0, 0, out.name + ":prologue"});
    }

    RgCompileStats& s = out.stats;
    s.batchCount = static_cast<u32>(out.batches.size());
    s.commandListCount = static_cast<u32>(out.lists.size());
    s.barrierCount = 0;
    s.crossQueueWaitCount = 0;
    for (const RgPassInfo& p : out.passes) s.barrierCount += static_cast<u32>(p.preBarriers.size() + p.postBarriers.size());
    for (const RgBatchInfo& b : out.batches) {
        s.barrierCount += static_cast<u32>(b.barriers.size());
        s.crossQueueWaitCount += static_cast<u32>(b.waits.size());
    }
    return out;
}

} // namespace helios::render
