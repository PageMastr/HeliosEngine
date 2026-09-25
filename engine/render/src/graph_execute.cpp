// Render graph execution (03 §2.4): physical resources from the pool, entry-barrier resolution,
// parallel command recording on the job system and ordered submission with timeline waits.

#include <algorithm>

#include "graph_internal.h"
#include "helios/core/jobs.h"
#include "helios/core/memory.h"
#include "helios/core/time.h"

namespace helios::render {

namespace {

MemoryTag transientTag() {
    static const MemoryTag tag = registerMemoryTag("RenderTransients");
    return tag;
}

struct Recorder {
    const RenderGraph::Impl& g;
    const RgPlan& plan;
    rhi::Device& device;
    const std::vector<rhi::TextureH>& textures;
    const std::vector<rhi::BufferH>& buffers;

    void barriers(rhi::CommandList& cmd, std::span<const RgBarrier> list) const {
        if (list.empty()) return;
        std::vector<rhi::Barrier> out;
        out.reserve(list.size());
        for (const RgBarrier& b : list) {
            if (plan.physicals[b.physical].isTexture) {
                out.push_back(rhi::Barrier::textureState(textures[b.physical], b.before, b.after, b.range));
            } else {
                out.push_back(rhi::Barrier::bufferState(buffers[b.physical], b.before, b.after));
            }
        }
        cmd.barrier(out);
    }

    /// Full extent of a Raster pass's attachments (zero otherwise).
    rhi::Rect renderArea(u32 p) const {
        const RgPassDecl& decl = g.passes[p];
        if (!hasFlag(decl.flags, PassFlags::Raster)) return {};
        const RgAttachmentDecl& att = decl.depth.resource != kRgInvalid ? decl.depth : decl.colors.front();
        const RgTextureDesc& d = plan.resources[att.resource].texture;
        return {0, 0, rhi::mipExtent(d.width, att.mip), rhi::mipExtent(d.height, att.mip)};
    }

    void pass(rhi::CommandList& cmd, u32 p, RgContext& ctx) const {
        const RgPassInfo& info = plan.passes[p];
        const RgPassDecl& decl = g.passes[p];
        if (g.options.labels) cmd.beginLabel(info.name);
        if (g.options.breadcrumbs) cmd.breadcrumb(static_cast<u16>(info.position + 1), rhi::BreadcrumbStage::Begin);
        barriers(cmd, info.preBarriers);
        const bool raster = hasFlag(decl.flags, PassFlags::Raster);
        if (raster) {
            std::array<rhi::ColorAttachment, rhi::kMaxColorAttachments> colors{};
            const u32 colorCount = static_cast<u32>(decl.colors.size());
            for (u32 i = 0; i < colorCount; ++i) {
                const RgAttachmentDecl& att = decl.colors[i];
                const u32 phys = plan.resources[att.resource].physical;
                colors[i].texture = textures[phys];
                colors[i].mip = att.mip;
                colors[i].layer = att.layer;
                colors[i].load = att.load;
                colors[i].store = info.colorStore[i];
                colors[i].clearColor = att.clearColor;
            }
            rhi::RenderingDesc rd;
            rd.colors = {colors.data(), colorCount};
            if (decl.depth.resource != kRgInvalid) {
                const RgAttachmentDecl& att = decl.depth;
                rd.depth.texture = textures[plan.resources[att.resource].physical];
                rd.depth.mip = att.mip;
                rd.depth.layer = att.layer;
                rd.depth.load = att.load;
                rd.depth.store = info.depthStore;
                rd.depth.clearDepth = att.clearDepth;
                rd.depth.readOnly = att.readOnly;
            }
            rd.area = renderArea(p);
            cmd.beginRendering(rd);
        }
        if (decl.execute) decl.execute(ctx);
        if (raster) cmd.endRendering();
        barriers(cmd, info.postBarriers);
        if (g.options.breadcrumbs) cmd.breadcrumb(static_cast<u16>(info.position + 1), rhi::BreadcrumbStage::End);
        if (g.options.labels) cmd.endLabel();
    }
};

// Creates the bindless slots of the views passes typically ask for — uav(mip) of Storage textures
// and the one-mip views {.baseMip = m, .mipCount = 1} of mipmapped Sampled textures — when the
// physical texture is created, on this thread and in plan order. Lazily created on the recording
// jobs, their slot numbers would depend on how the jobs interleave (non-reproducible traces and
// captures). Other sub-views are still created on first request.
void createViews(rhi::Device& device, rhi::TextureH texture, const rhi::TextureDesc& desc) {
    for (u32 mip = 0; mip < desc.mipLevels; ++mip) {
        if (hasFlag(desc.usage, rhi::TextureUsage::Storage)) (void)device.uav(texture, mip);
        if (desc.mipLevels > 1 && hasFlag(desc.usage, rhi::TextureUsage::Sampled)) {
            (void)device.srv(texture, rhi::ViewDesc{.baseMip = mip, .mipCount = 1});
        }
    }
}

/// Creates, in plan order, the sub-views of imported textures that the passes declare: uav(mip) for
/// Storage accesses and one-mip SRVs for sampled reads of mipmapped textures (the same views
/// createViews() makes for transients). Existing views are only looked up, so this is cheap after the
/// first frame; without it, lazily created slots would be numbered in job-interleaving order.
void createImportedViews(rhi::Device& device, const RenderGraph::Impl& g, const RgPlan& plan,
                         const std::vector<rhi::TextureH>& textures) {
    for (u32 p : plan.order) {
        for (const RgAccess& a : plan.passes[p].accesses) {
            const RgResourceInfo& r = plan.resources[a.resource];
            if (!r.imported || !r.isTexture || r.physical == kRgInvalid) continue;
            const rhi::TextureUsage usage = g.resources[a.resource].importTextureUsage;
            const rhi::TextureH texture = textures[r.physical];
            for (u32 mip = a.range.baseMip; mip < a.range.baseMip + a.range.mipCount; ++mip) {
                if (a.state == rhi::ResourceState::UnorderedAccess && hasFlag(usage, rhi::TextureUsage::Storage)) {
                    (void)device.uav(texture, mip);
                } else if ((a.state == rhi::ResourceState::ShaderResource || a.state == rhi::ResourceState::DepthRead) &&
                           r.texture.mipLevels > 1 && hasFlag(usage, rhi::TextureUsage::Sampled)) {
                    (void)device.srv(texture, rhi::ViewDesc{.baseMip = mip, .mipCount = 1});
                }
            }
        }
    }
}

void addExternalWait(std::vector<rhi::TimelinePoint>& waits, rhi::TimelinePoint point) {
    if (point.isNull()) return;
    for (rhi::TimelinePoint& w : waits) {
        if (w.queue == point.queue) {
            w.value = std::max(w.value, point.value);
            return;
        }
    }
    waits.push_back(point);
}

} // namespace

Result<RgExecuteResult> RenderGraph::execute(rhi::Device& device, RgResourcePool& pool, const RgExecuteOptions& options) {
    Impl& g = *m_impl;
    if (!g.compiled) return Error{ErrorCode::InvalidState, "RenderGraph::execute() before a successful compile()"};
    if (&pool.device() != &device) return Error{ErrorCode::InvalidArgument, "the resource pool belongs to another device"};
    RgResourcePool::Impl& pi = *pool.m_impl;
    const u64 frame = device.frameIndex();
    const RgPlan& plan = g.plan;
    const u32 physCount = static_cast<u32>(plan.physicals.size());

    // -- Physical resources and their current states ---------------------------------------------
    std::vector<rhi::TextureH> textures(physCount);
    std::vector<rhi::BufferH> buffers(physCount);
    std::vector<std::vector<rhi::ResourceState>> states(physCount);
    std::vector<u32> poolIndex(physCount, kRgInvalid);
    auto releaseAll = [&] {
        for (RgPoolTexture& t : pi.textures) t.inUse = false;
        for (RgPoolBuffer& b : pi.buffers) b.inUse = false;
    };
    for (u32 i = 0; i < physCount; ++i) {
        const RgPhysicalInfo& phys = plan.physicals[i];
        if (phys.imported) {
            const RgResourceDecl& r = g.resources[phys.residents.front()];
            if (phys.isTexture) {
                textures[i] = r.importedTexture;
            } else {
                buffers[i] = r.importedBuffer;
            }
            states[i].assign(phys.subresourceCount(), r.import.initialState);
            continue;
        }
        const std::string& name = plan.resources[phys.residents.front()].name;
        if (phys.isTexture) {
            u32 found = kRgInvalid;
            for (u32 j = 0; j < pi.textures.size() && found == kRgInvalid; ++j) {
                const RgPoolTexture& t = pi.textures[j];
                if (!t.inUse && t.desc == phys.texture && hasFlag(t.usage, phys.textureUsage)) found = j;
            }
            if (found == kRgInvalid) {
                rhi::TextureDesc d;
                d.type = phys.texture.type;
                d.format = phys.texture.format;
                d.width = phys.texture.width;
                d.height = phys.texture.height;
                d.depth = phys.texture.depth;
                d.mipLevels = phys.texture.mipLevels;
                d.arrayLayers = phys.texture.arrayLayers;
                d.sampleCount = phys.texture.sampleCount;
                d.usage = phys.textureUsage;
                d.name = name;
                d.tag = transientTag();
                auto created = device.createTexture(d);
                if (!created) {
                    releaseAll();
                    return Error{created.error().code,
                                 std::format("render graph '{}': transient '{}': {}", g.name, name, created.error().message)};
                }
                createViews(device, created.value(), d);
                RgPoolTexture t;
                t.handle = created.value();
                t.desc = phys.texture;
                t.usage = phys.textureUsage;
                t.states.assign(phys.subresourceCount(), rhi::ResourceState::Undefined);
                t.bytes = rgTextureBytes(phys.texture);
                found = static_cast<u32>(pi.textures.size());
                pi.textures.push_back(std::move(t));
            }
            RgPoolTexture& t = pi.textures[found];
            t.inUse = true;
            textures[i] = t.handle;
            states[i] = t.states;
            poolIndex[i] = found;
        } else {
            u32 found = kRgInvalid;
            for (u32 j = 0; j < pi.buffers.size() && found == kRgInvalid; ++j) {
                const RgPoolBuffer& b = pi.buffers[j];
                if (!b.inUse && b.size == phys.bufferSize && hasFlag(b.usage, phys.bufferUsage)) found = j;
            }
            if (found == kRgInvalid) {
                rhi::BufferDesc d;
                d.size = phys.bufferSize;
                d.usage = phys.bufferUsage;
                d.memory = rhi::MemoryUsage::GpuOnly;
                d.name = name;
                d.tag = transientTag();
                auto created = device.createBuffer(d);
                if (!created) {
                    releaseAll();
                    return Error{created.error().code,
                                 std::format("render graph '{}': transient '{}': {}", g.name, name, created.error().message)};
                }
                RgPoolBuffer b;
                b.handle = created.value();
                b.size = phys.bufferSize;
                b.usage = phys.bufferUsage;
                found = static_cast<u32>(pi.buffers.size());
                pi.buffers.push_back(b);
            }
            RgPoolBuffer& b = pi.buffers[found];
            b.inUse = true;
            buffers[i] = b.handle;
            states[i].assign(1, b.state);
            poolIndex[i] = found;
        }
    }

    // -- Resolve entry barriers against those states -----------------------------------------------
    g.executed = resolveEntryBarriers(plan, states);
    const RgPlan& s = g.executed;
    const u32 batchCount = static_cast<u32>(s.batches.size());

    // Batches touching each physical resource (for cross-frame and import waits, and pool updates).
    std::vector<std::vector<u32>> touching(physCount);
    for (u32 b = 0; b < batchCount; ++b) {
        const RgBatchInfo& batch = s.batches[b];
        for (const RgBarrier& bar : batch.barriers) touching[bar.physical].push_back(b);
        for (u32 p : batch.passes) {
            const RgPassInfo& pass = s.passes[p];
            for (const RgAccess& a : pass.accesses) touching[s.resources[a.resource].physical].push_back(b);
            for (const RgBarrier& bar : pass.preBarriers) touching[bar.physical].push_back(b);
            for (const RgBarrier& bar : pass.postBarriers) touching[bar.physical].push_back(b);
        }
    }
    std::vector<std::vector<rhi::TimelinePoint>> external(batchCount);
    for (u32 i = 0; i < physCount; ++i) {
        std::vector<u32>& t = touching[i];
        std::sort(t.begin(), t.end());
        t.erase(std::unique(t.begin(), t.end()), t.end());
        if (t.empty()) continue;
        const RgPhysicalInfo& phys = s.physicals[i];
        if (phys.imported) {
            // The first submission on *every* queue that touches the import waits for its ready
            // point: batches on different queues need not be ordered by the graph (e.g. two reads).
            const rhi::TimelinePoint ready = g.resources[phys.residents.front()].import.waitFor;
            std::array<bool, rhi::kQueueCount> seen{};
            for (u32 b : t) {
                const u32 q = rhi::queueIndex(s.batches[b].queue);
                if (seen[q]) continue;
                seen[q] = true;
                addExternalWait(external[b], ready);
            }
            continue;
        }
        const std::array<u64, rhi::kQueueCount>& last =
            phys.isTexture ? pi.textures[poolIndex[i]].lastValues : pi.buffers[poolIndex[i]].lastValues;
        std::array<bool, rhi::kQueueCount> seen{};
        for (u32 b : t) {
            const u32 q = rhi::queueIndex(s.batches[b].queue);
            if (seen[q]) continue;
            seen[q] = true;
            for (u32 other = 0; other < rhi::kQueueCount; ++other) {
                if (other != q && last[other] != 0) {
                    addExternalWait(external[b], {static_cast<rhi::Queue>(other), last[other]});
                }
            }
        }
    }
    if (!options.waits.empty()) {
        std::array<bool, rhi::kQueueCount> seen{};
        for (u32 b = 0; b < batchCount; ++b) {
            const u32 q = rhi::queueIndex(s.batches[b].queue);
            if (seen[q]) continue;
            seen[q] = true;
            for (const rhi::TimelinePoint& w : options.waits) addExternalWait(external[b], w);
        }
    }

    createImportedViews(device, g, s, textures);

    // -- Record (in parallel) ----------------------------------------------------------------------
    const u32 listCount = static_cast<u32>(s.lists.size());
    std::vector<rhi::CommandList*> cmds(listCount, nullptr);
    std::atomic<bool> acquireFailed{false};
    const Recorder recorder{g, s, device, textures, buffers};
    auto recordList = [&](u32 li) {
        const RgListInfo& list = s.lists[li];
        const RgBatchInfo& batch = s.batches[list.batch];
        rhi::CommandList* cmd = device.acquireCommandList(batch.queue, list.name);
        if (!cmd) {
            acquireFailed.store(true, std::memory_order_relaxed);
            return;
        }
        if (batch.kind != RgBatchInfo::Kind::Passes) {
            recorder.barriers(*cmd, batch.barriers);
        } else {
            RgContext ctx;
            ctx.m_graph = this;
            ctx.m_device = &device;
            ctx.m_cmd = cmd;
            ctx.m_textures = &textures;
            ctx.m_buffers = &buffers;
            for (u32 i = 0; i < list.passCount; ++i) {
                ctx.m_pass = batch.passes[list.firstPass + i];
                ctx.m_area = recorder.renderArea(ctx.m_pass);
                recorder.pass(*cmd, ctx.m_pass, ctx);
            }
        }
        cmd->end();
        cmds[li] = cmd;
    };
    const Stopwatch recordTimer;
    if (options.jobs && listCount > 1) {
        jobs::Counter counter;
        for (u32 li = 0; li < listCount; ++li) options.jobs->run([&recordList, li] { recordList(li); }, &counter);
        options.jobs->wait(counter);
    } else {
        for (u32 li = 0; li < listCount; ++li) recordList(li);
    }
    RgExecuteResult result;
    result.recordMs = recordTimer.elapsedMillis();
    result.commandListCount = listCount;
    if (acquireFailed.load()) {
        releaseAll();
        return Error{ErrorCode::InvalidState, std::format("render graph '{}': no command list (device lost?)", g.name)};
    }

    // -- Submit in order --------------------------------------------------------------------------------
    std::vector<rhi::TimelinePoint> points(batchCount);
    std::vector<rhi::TimelinePoint> waits;
    for (u32 b = 0; b < batchCount; ++b) {
        const RgBatchInfo& batch = s.batches[b];
        waits.clear();
        for (u32 w : batch.waits) waits.push_back(points[w]);
        for (const rhi::TimelinePoint& w : external[b]) waits.push_back(w);
        const std::span<rhi::CommandList* const> lists(cmds.data() + batch.firstList, batch.listCount);
        auto submitted = device.submit(batch.queue, lists, waits);
        if (!submitted) {
            releaseAll();
            return Error{submitted.error().code,
                         std::format("render graph '{}': submit of batch {} failed: {}", g.name, b, submitted.error().message)};
        }
        points[b] = submitted.value();
        result.lastPoints[rhi::queueIndex(batch.queue)] = points[b];
        ++result.submitCount;
    }

    // -- Carry states and last uses over to the next frame -------------------------------------------
    for (u32 i = 0; i < physCount; ++i) {
        const RgPhysicalInfo& phys = s.physicals[i];
        if (phys.imported) continue;
        std::vector<rhi::ResourceState>* poolStates = nullptr;
        std::array<u64, rhi::kQueueCount>* lastValues = nullptr;
        if (phys.isTexture) {
            RgPoolTexture& t = pi.textures[poolIndex[i]];
            poolStates = &t.states;
            lastValues = &t.lastValues;
            t.lastUsedFrame = frame;
        } else {
            RgPoolBuffer& b = pi.buffers[poolIndex[i]];
            lastValues = &b.lastValues;
            b.lastUsedFrame = frame;
            if (phys.finalStates[0] != kRgEntryState) b.state = phys.finalStates[0];
        }
        if (poolStates) {
            for (u32 sub = 0; sub < phys.finalStates.size(); ++sub) {
                if (phys.finalStates[sub] != kRgEntryState) (*poolStates)[sub] = phys.finalStates[sub];
            }
        }
        for (u32 b : touching[i]) (*lastValues)[rhi::queueIndex(s.batches[b].queue)] = points[b].value;
    }
    releaseAll();
    // Trim resources no graph used during the last keepFrames *completed* frames (destroy is
    // deferred by the RHI). The current frame does not count: another graph sharing the pool may
    // still use a resource later in it.
    auto stale = [&](u64 lastUsed) { return frame > lastUsed && frame - lastUsed > pi.keepFrames; };
    for (usize j = pi.textures.size(); j-- > 0;) {
        if (stale(pi.textures[j].lastUsedFrame)) {
            device.destroy(pi.textures[j].handle);
            pi.textures.erase(pi.textures.begin() + static_cast<std::ptrdiff_t>(j));
        }
    }
    for (usize j = pi.buffers.size(); j-- > 0;) {
        if (stale(pi.buffers[j].lastUsedFrame)) {
            device.destroy(pi.buffers[j].handle);
            pi.buffers.erase(pi.buffers.begin() + static_cast<std::ptrdiff_t>(j));
        }
    }
    return result;
}

} // namespace helios::render
