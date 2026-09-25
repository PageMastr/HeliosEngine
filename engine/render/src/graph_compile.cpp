// Render graph compile (03 §2.2): culling, queue batches and waits, aliasing, placement plan,
// barriers, store ops and command-list partition; plus entry-barrier resolution.

#include <algorithm>

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

class Compiler {
public:
    Compiler(RenderGraph::Impl& graph, const RgCompileOptions& options) : g(graph), opt(options) {}

    RgPlan run() {
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
        return std::move(plan);
    }

private:
    RenderGraph::Impl& g;
    const RgCompileOptions& opt;
    RgPlan plan;
    u32 passCount = 0;
    u32 resourceCount = 0;

    std::vector<u8> needed;
    std::vector<u32> declOrder;                // non-culled passes, declaration order
    std::vector<std::vector<u32>> crossDeps;   // per pass: earlier passes on other queues it must wait for
    std::vector<u8> hasConsumer;               // per pass: some pass on another queue waits for it
    std::vector<std::array<u64, kQueues>> clocks;  // per batch: queue values known complete at its start

    struct Life {
        std::array<u32, kQueues> first;
        std::array<u32, kQueues> last;
        Life() {
            first.fill(kRgInvalid);
            last.fill(kRgInvalid);
        }
    };
    std::vector<Life> life;  // per resource: first/last accessing pass per queue

    rhi::Queue queueOf(u32 pass) const { return plan.passes[pass].queue; }
    u32 positionOf(u32 pass) const { return plan.passes[pass].position; }

    // -- 0. plan skeleton ----------------------------------------------------------------------
    void initPlan() {
        plan.name = g.name;
        passCount = static_cast<u32>(g.passes.size());
        resourceCount = static_cast<u32>(g.resources.size());
        plan.resources.resize(resourceCount);
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceDecl& d = g.resources[r];
            RgResourceInfo& info = plan.resources[r];
            info.name = d.name;
            info.isTexture = d.isTexture;
            info.imported = d.imported;
            info.texture = d.texture;
            info.bufferSize = d.bufferSize;
            info.textureUsage = d.textureUsage;
            info.bufferUsage = d.bufferUsage;
            info.versionCount = static_cast<u32>(d.producers.size());
            info.import = d.import;
        }
        plan.passes.resize(passCount);
        for (u32 p = 0; p < passCount; ++p) {
            const RgPassDecl& d = g.passes[p];
            RgPassInfo& info = plan.passes[p];
            info.name = d.name;
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
        needed.assign(passCount, opt.cull ? 0 : 1);
        if (opt.cull) {
            std::vector<u32> stack;
            auto mark = [&](u32 p) {
                if (p != kRgInvalid && !needed[p]) {
                    needed[p] = 1;
                    stack.push_back(p);
                }
            };
            for (u32 p = 0; p < passCount; ++p) {
                if (hasFlag(g.passes[p].flags, PassFlags::NeverCull)) mark(p);
                for (const RgAccess& a : plan.passes[p].accesses) {
                    const RgResourceDecl& r = g.resources[a.resource];
                    if (a.isWrite() && (r.imported || r.outputs[a.writeVersion])) mark(p);
                }
            }
            while (!stack.empty()) {
                const u32 p = stack.back();
                stack.pop_back();
                for (const RgAccess& a : plan.passes[p].accesses) {
                    if (a.readVersion != kRgInvalid) mark(g.resources[a.resource].producers[a.readVersion]);
                }
            }
        }
        for (u32 p = 0; p < passCount; ++p) {
            plan.passes[p].culled = !needed[p];
            if (needed[p]) declOrder.push_back(p);
        }
    }

    // -- 2. cross-queue hazards on the virtual resources -------------------------------------------
    // Per subresource in declaration order: RAW, WAW and WAR hazards plus state changes (a layout
    // change is a write). A state change a queue cannot perform itself (graphics-only source state
    // on async compute) happens on the queue of the last access (a release), so later accesses
    // depend on that site. Only hazards between different queues need timeline waits.
    void hazards() {
        crossDeps.assign(passCount, {});
        hasConsumer.assign(passCount, 0);
        struct VSub {
            rhi::ResourceState state = rhi::ResourceState::Undefined;
            Site lastWrite;
            Site lastAccess;
            std::vector<Site> readers;
        };
        std::vector<std::vector<VSub>> subs(resourceCount);
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceDecl& d = g.resources[r];
            subs[r].resize(d.subresourceCount());
            for (VSub& s : subs[r]) s.state = d.imported ? d.import.initialState : rhi::ResourceState::Undefined;
        }
        std::vector<Site> deps;
        for (u32 p : declOrder) {
            const rhi::Queue q = queueOf(p);
            for (const RgAccess& a : plan.passes[p].accesses) {
                const u32 layers = g.resources[a.resource].arrayLayers();
                forEachSub(a.range, layers, [&](u32 s, u32, u32) {
                    VSub& v = subs[a.resource][s];
                    deps.clear();
                    if (v.lastWrite.valid()) deps.push_back(v.lastWrite);
                    if (a.isWrite()) {
                        deps.insert(deps.end(), v.readers.begin(), v.readers.end());
                        v.lastWrite = {p, q};
                        v.readers.clear();
                        v.state = a.state;
                    } else {
                        if (a.state != v.state) {
                            deps.insert(deps.end(), v.readers.begin(), v.readers.end());
                            Site site{p, q};
                            if (q != rhi::Queue::Graphics && rgStateLevel(v.state) > rgQueueLevel(q)) {
                                // Released by the last access (graphics); with none in this graph the
                                // transition happens in the execute-time prologue (graphics, first).
                                site = v.lastAccess;
                            }
                            v.lastWrite = site;
                            v.readers.clear();
                            v.state = a.state;
                        }
                        v.readers.push_back({p, q});
                    }
                    v.lastAccess = {p, q};
                    for (const Site& d : deps) {
                        if (d.valid() && d.pass != p && d.queue != q) crossDeps[p].push_back(d.pass);
                    }
                });
            }
            std::vector<u32>& cd = crossDeps[p];
            std::sort(cd.begin(), cd.end());
            cd.erase(std::unique(cd.begin(), cd.end()), cd.end());
            for (u32 d : cd) hasConsumer[d] = 1;
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
        struct Build {
            rhi::Queue queue = rhi::Queue::Graphics;
            std::vector<u32> passes;
            std::vector<u32> waits;  // build indices
            std::array<u64, kQueues> clock{};
            bool closed = false;
            u32 value = 0;
        };
        std::vector<Build> builds;
        std::vector<u32> closedOrder;
        std::array<u32, kQueues> open;
        std::array<u32, kQueues> lastOnQueue;
        std::array<u32, kQueues> counter{};
        open.fill(kRgInvalid);
        lastOnQueue.fill(kRgInvalid);
        std::vector<u32> buildOf(passCount, kRgInvalid);
        auto close = [&](u32 b) {
            if (b == kRgInvalid || builds[b].closed) return;
            Build& build = builds[b];
            const u32 qi = rhi::queueIndex(build.queue);
            build.closed = true;
            build.value = ++counter[qi];
            closedOrder.push_back(b);
            if (open[qi] == b) open[qi] = kRgInvalid;
        };
        auto startBuild = [&](rhi::Queue queue, const std::vector<u32>& waits) {
            const u32 qi = rhi::queueIndex(queue);
            Build build;
            build.queue = queue;
            build.waits = waits;
            if (lastOnQueue[qi] != kRgInvalid) build.clock = builds[lastOnQueue[qi]].clock;
            for (u32 w : waits) {
                const u32 wq = rhi::queueIndex(builds[w].queue);
                for (u32 k = 0; k < kQueues; ++k) build.clock[k] = std::max(build.clock[k], builds[w].clock[k]);
                build.clock[wq] = std::max<u64>(build.clock[wq], builds[w].value);
            }
            const u32 index = static_cast<u32>(builds.size());
            builds.push_back(std::move(build));
            open[qi] = index;
            lastOnQueue[qi] = index;
        };
        for (u32 p : declOrder) {
            const rhi::Queue q = queueOf(p);
            const u32 qi = rhi::queueIndex(q);
            // Latest producer batch per other queue.
            std::array<u32, kQueues> best;
            best.fill(kRgInvalid);
            for (u32 d : crossDeps[p]) {
                const u32 bd = buildOf[d];
                close(bd);  // already closed: producers with consumers close right after them
                const u32 dq = rhi::queueIndex(builds[bd].queue);
                if (best[dq] == kRgInvalid || builds[bd].value > builds[best[dq]].value) best[dq] = bd;
            }
            std::array<u64, kQueues> base{};
            if (open[qi] != kRgInvalid) {
                base = builds[open[qi]].clock;
            } else if (lastOnQueue[qi] != kRgInvalid) {
                base = builds[lastOnQueue[qi]].clock;
            }
            std::vector<u32> missing;  // producer batches not implied by the queue's clock
            for (u32 dq = 0; dq < kQueues; ++dq) {
                if (best[dq] != kRgInvalid && base[dq] < builds[best[dq]].value) missing.push_back(best[dq]);
            }
            if (!missing.empty()) {
                close(open[qi]);
                std::vector<u32> waits;
                for (u32 w : missing) {
                    const u32 wq = rhi::queueIndex(builds[w].queue);
                    bool implied = false;
                    for (u32 other : missing) {
                        if (other != w && builds[other].clock[wq] >= builds[w].value) implied = true;
                    }
                    if (!implied) waits.push_back(w);
                }
                startBuild(q, waits);
            } else if (open[qi] == kRgInvalid) {
                startBuild(q, {});
            }
            builds[open[qi]].passes.push_back(p);
            buildOf[p] = open[qi];
            if (hasConsumer[p]) close(open[qi]);
        }
        for (u32 b = 0; b < builds.size(); ++b) close(b);

        std::vector<u32> submitIndex(builds.size(), kRgInvalid);
        for (u32 i = 0; i < closedOrder.size(); ++i) submitIndex[closedOrder[i]] = i;
        clocks.assign(closedOrder.size(), {});
        for (u32 i = 0; i < closedOrder.size(); ++i) {
            const Build& b = builds[closedOrder[i]];
            RgBatchInfo batch;
            batch.kind = RgBatchInfo::Kind::Passes;
            batch.queue = b.queue;
            batch.passes = b.passes;
            batch.queueValue = b.value;
            for (u32 w : b.waits) batch.waits.push_back(submitIndex[w]);
            std::sort(batch.waits.begin(), batch.waits.end());
            clocks[i] = b.clock;
            for (u32 p : b.passes) {
                plan.passes[p].batch = i;
                plan.passes[p].position = static_cast<u32>(plan.order.size());
                plan.order.push_back(p);
            }
            plan.batches.push_back(std::move(batch));
        }
    }

    /// Pass a (earlier in submission order) happens before pass b: same queue (a barrier at b will
    /// order them), or b's batch waits (transitively) for a's batch.
    bool hb(u32 a, u32 b) const {
        if (positionOf(a) >= positionOf(b)) return false;
        if (queueOf(a) == queueOf(b)) return true;
        const RgBatchInfo& ba = plan.batches[plan.passes[a].batch];
        return clocks[plan.passes[b].batch][rhi::queueIndex(ba.queue)] >= ba.queueValue;
    }

    /// Every access of resource x happens before every access of resource r.
    bool hbAll(u32 x, u32 r) const {
        for (u32 qa = 0; qa < kQueues; ++qa) {
            const u32 a = life[x].last[qa];
            if (a == kRgInvalid) continue;
            for (u32 qb = 0; qb < kQueues; ++qb) {
                const u32 b = life[r].first[qb];
                if (b != kRgInvalid && !hb(a, b)) return false;
            }
        }
        return true;
    }

    void lifetimes() {
        life.assign(resourceCount, Life{});
        for (u32 p : plan.order) {
            const u32 qi = rhi::queueIndex(queueOf(p));
            const u32 pos = positionOf(p);
            for (const RgAccess& a : plan.passes[p].accesses) {
                RgResourceInfo& r = plan.resources[a.resource];
                r.used = true;
                if (r.firstPosition == kRgInvalid) r.firstPosition = pos;
                r.lastPosition = pos;
                Life& l = life[a.resource];
                if (l.first[qi] == kRgInvalid) l.first[qi] = p;
                l.last[qi] = p;
            }
        }
        for (u32 r = 0; r < resourceCount; ++r) {
            RgResourceInfo& info = plan.resources[r];
            if (info.imported) continue;
            info.bytes = alignUp(info.isTexture ? rgTextureBytes(info.texture) : info.bufferSize, kPlacementAlignment);
        }
    }

    // -- 5. aliasing (pooled physical resources) ------------------------------------------------------
    void aliasing() {
        std::vector<u32> used;
        for (u32 r = 0; r < resourceCount; ++r) {
            if (plan.resources[r].used) used.push_back(r);
        }
        std::sort(used.begin(), used.end(), [&](u32 a, u32 b) {
            const u32 fa = plan.resources[a].firstPosition, fb = plan.resources[b].firstPosition;
            return fa != fb ? fa < fb : a < b;
        });
        for (u32 r : used) {
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
                RgPhysicalInfo phys;
                phys.isTexture = info.isTexture;
                phys.imported = info.imported;
                phys.texture = info.texture;
                phys.bufferSize = info.bufferSize;
                phys.bytes = info.bytes;
                target = static_cast<u32>(plan.physicals.size());
                plan.physicals.push_back(std::move(phys));
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
            RgPhysicalInfo phys;
            phys.isTexture = info.isTexture;
            phys.imported = true;
            phys.texture = info.texture;
            phys.bufferSize = info.bufferSize;
            phys.residents.push_back(r);
            info.physical = static_cast<u32>(plan.physicals.size());
            plan.physicals.push_back(std::move(phys));
        }
        for (RgPhysicalInfo& phys : plan.physicals) phys.finalStates.assign(phys.subresourceCount(), kRgEntryState);
    }

    // -- 6. placement plan (placed-resource aliasing, informational) ---------------------------------
    void placement() {
        std::array<std::vector<u32>, kRgHeapKindCount> heaps;
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
            heaps[static_cast<u32>(kind)].push_back(r);
        }
        for (u32 h = 0; h < kRgHeapKindCount; ++h) {
            std::vector<u32>& items = heaps[h];
            std::sort(items.begin(), items.end(), [&](u32 a, u32 b) {
                const RgResourceInfo& ra = plan.resources[a];
                const RgResourceInfo& rb = plan.resources[b];
                if (ra.bytes != rb.bytes) return ra.bytes > rb.bytes;
                if (ra.firstPosition != rb.firstPosition) return ra.firstPosition < rb.firstPosition;
                return a < b;
            });
            std::vector<u32> placed;
            u64 heapSize = 0;
            for (u32 r : items) {
                RgResourceInfo& info = plan.resources[r];
                std::vector<std::pair<u64, u64>> conflicts;  // [offset, end)
                for (u32 o : placed) {
                    if (!(hbAll(o, r) || hbAll(r, o))) {
                        conflicts.emplace_back(plan.resources[o].heapOffset,
                                               plan.resources[o].heapOffset + plan.resources[o].bytes);
                    }
                }
                std::sort(conflicts.begin(), conflicts.end());
                u64 offset = 0;
                for (const auto& [begin, end] : conflicts) {
                    if (offset + info.bytes <= begin) break;
                    offset = std::max(offset, end);
                }
                info.heapOffset = offset;
                heapSize = std::max(heapSize, offset + info.bytes);
                placed.push_back(r);
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
        std::vector<std::vector<PSub>> psubs(plan.physicals.size());
        for (u32 i = 0; i < plan.physicals.size(); ++i) psubs[i].resize(plan.physicals[i].subresourceCount());
        std::vector<std::vector<RgSubBarrier>> pre(passCount), post(passCount);

        for (u32 p : declOrder) {
            const rhi::Queue q = queueOf(p);
            const u32 qi = rhi::queueIndex(q);
            const u8 qbit = static_cast<u8>(1u << qi);
            for (const RgAccess& a : plan.passes[p].accesses) {
                const u32 phys = plan.resources[a.resource].physical;
                const u32 layers = plan.physicals[phys].isTexture ? plan.physicals[phys].texture.arrayLayers : 1u;
                const bool write = a.isWrite();
                forEachSub(a.range, layers, [&](u32 s, u32 mip, u32 layer) {
                    PSub& st = psubs[phys][s];
                    bool transitioned = false;
                    if (st.state == kRgEntryState) {
                        pre[p].push_back({phys, kRgEntryState, a.state, mip, layer});
                        transitioned = true;
                    } else {
                        const bool need = st.owner != a.resource || a.state != st.state ||
                                          (st.writePending && st.writeQueue == q) || (write && (st.readQueues & qbit));
                        if (need) {
                            if (q == rhi::Queue::Graphics || rgStateLevel(st.state) <= rgQueueLevel(q)) {
                                pre[p].push_back({phys, st.state, a.state, mip, layer});
                            } else {
                                // Graphics-only source state on async compute: release on the queue
                                // of the last access (always graphics for such states).
                                const bool releasable =
                                    st.lastPass != kRgInvalid && queueOf(st.lastPass) == rhi::Queue::Graphics;
                                HELIOS_ASSERT(releasable);
                                if (releasable) {
                                    post[st.lastPass].push_back({phys, st.state, a.state, mip, layer});
                                } else {
                                    pre[p].push_back({phys, st.state, a.state, mip, layer});  // unreachable
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
        }

        // Final states of imported resources: after the last access when a single queue holds the
        // accesses since the last transition and can perform it; otherwise in a graphics epilogue
        // that waits for the last access on every other queue.
        std::vector<RgSubBarrier> epilogue;
        std::vector<u32> epilogueWaits;
        for (u32 r = 0; r < resourceCount; ++r) {
            const RgResourceInfo& info = plan.resources[r];
            if (!info.imported || info.physical == kRgInvalid || info.import.finalState == kRgEntryState) continue;
            const u32 phys = info.physical;
            const rhi::ResourceState fin = info.import.finalState;
            const RgPhysicalInfo& pi = plan.physicals[phys];
            const u32 layers = pi.isTexture ? pi.texture.arrayLayers : 1u;
            const u32 mips = pi.isTexture ? pi.texture.mipLevels : 1u;
            forEachSub({0, mips, 0, layers}, layers, [&](u32 s, u32 mip, u32 layer) {
                PSub& st = psubs[phys][s];
                if (st.state == kRgEntryState) {
                    epilogue.push_back({phys, kRgEntryState, fin, mip, layer});
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
                        post[st.lastOnQueue[single]].push_back({phys, st.state, fin, mip, layer});
                    } else {
                        epilogue.push_back({phys, st.state, fin, mip, layer});
                        for (u32 k = 0; k < kQueues; ++k) {
                            if ((st.sinceQueues & (1u << k)) && k != rhi::queueIndex(rhi::Queue::Graphics)) {
                                epilogueWaits.push_back(plan.passes[st.lastOnQueue[k]].batch);
                            }
                        }
                    }
                }
                st.state = fin;
            });
        }

        for (u32 p : plan.order) {
            plan.passes[p].preBarriers = rgMergeBarriers(pre[p], plan.physicals);
            plan.passes[p].postBarriers = rgMergeBarriers(post[p], plan.physicals);
        }
        if (!epilogue.empty()) {
            RgBatchInfo batch;
            batch.kind = RgBatchInfo::Kind::Epilogue;
            batch.queue = rhi::Queue::Graphics;
            batch.barriers = rgMergeBarriers(epilogue, plan.physicals);
            // Keep only the latest batch per queue (earlier ones are implied by queue order).
            std::array<u32, kQueues> best;
            best.fill(kRgInvalid);
            for (u32 w : epilogueWaits) {
                const u32 wq = rhi::queueIndex(plan.batches[w].queue);
                if (best[wq] == kRgInvalid || plan.batches[w].queueValue > plan.batches[best[wq]].queueValue) best[wq] = w;
            }
            for (u32 w : best) {
                if (w != kRgInvalid) batch.waits.push_back(w);
            }
            std::sort(batch.waits.begin(), batch.waits.end());
            u32 graphicsBatches = 0;
            for (const RgBatchInfo& b : plan.batches) graphicsBatches += b.queue == rhi::Queue::Graphics ? 1u : 0u;
            batch.queueValue = graphicsBatches + 1;
            plan.batches.push_back(std::move(batch));
        }
        for (u32 i = 0; i < plan.physicals.size(); ++i) {
            for (u32 s = 0; s < psubs[i].size(); ++s) plan.physicals[i].finalStates[s] = psubs[i][s].state;
        }
    }

    // -- 8. store ops: DontCare when the written version is never read again ---------------------------
    void storeOps() {
        std::vector<std::vector<u8>> readLater(resourceCount);
        for (u32 r = 0; r < resourceCount; ++r) readLater[r].assign(g.resources[r].producers.size(), 0);
        for (u32 p : plan.order) {
            for (const RgAccess& a : plan.passes[p].accesses) {
                if (a.readVersion != kRgInvalid) readLater[a.resource][a.readVersion] = 1;
            }
        }
        auto store = [&](u32 p, const RgAttachmentDecl& att, rhi::ResourceState state) {
            for (const RgAccess& a : plan.passes[p].accesses) {
                if (a.resource != att.resource || a.state != state || a.range.baseMip != att.mip ||
                    a.range.baseLayer != att.layer || !a.isWrite()) {
                    continue;
                }
                const RgResourceDecl& r = g.resources[a.resource];
                const bool keep = r.imported || r.outputs[a.writeVersion] || readLater[a.resource][a.writeVersion];
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
        std::vector<u32> counts(batchCount, 1);
        u32 passBatches = 0;
        for (const RgBatchInfo& b : plan.batches) passBatches += b.kind == RgBatchInfo::Kind::Passes ? 1u : 0u;
        u32 remaining = opt.maxCommandLists > passBatches ? opt.maxCommandLists - passBatches : 0;
        while (remaining > 0) {
            u32 best = kRgInvalid;
            for (u32 b = 0; b < batchCount; ++b) {
                const RgBatchInfo& batch = plan.batches[b];
                if (batch.kind != RgBatchInfo::Kind::Passes || counts[b] >= batch.passes.size()) continue;
                // Largest passes-per-list ratio first (cross-multiplied to stay in integers).
                if (best == kRgInvalid ||
                    batch.passes.size() * counts[best] > plan.batches[best].passes.size() * counts[b]) {
                    best = b;
                }
            }
            if (best == kRgInvalid) break;
            ++counts[best];
            --remaining;
        }
        for (u32 b = 0; b < batchCount; ++b) {
            RgBatchInfo& batch = plan.batches[b];
            batch.firstList = static_cast<u32>(plan.lists.size());
            if (batch.kind != RgBatchInfo::Kind::Passes) {
                batch.listCount = 1;
                plan.lists.push_back({b, 0, 0, std::format("{}:epilogue", g.name)});
                continue;
            }
            const u32 n = static_cast<u32>(batch.passes.size());
            const u32 k = counts[b];
            batch.listCount = k;
            for (u32 i = 0; i < k; ++i) {
                const u32 begin = i * n / k;
                const u32 end = (i + 1) * n / k;
                RgListInfo list{b, begin, end - begin, {}};
                list.name = std::format("{}:{}", g.name, plan.passes[batch.passes[begin]].name);
                if (end - begin > 1) list.name += std::format("+{}", end - begin - 1);
                for (u32 j = begin; j < end; ++j) plan.passes[batch.passes[j]].list = static_cast<u32>(plan.lists.size());
                plan.lists.push_back(std::move(list));
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
    Compiler compiler(g, options);
    g.plan = compiler.run();
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
