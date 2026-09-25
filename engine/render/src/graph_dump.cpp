// Render graph dumps: deterministic text (trace goldens, logs) and Graphviz (visualizer, 03 §2.4).

#include <algorithm>

#include "graph_internal.h"

namespace helios::render {

namespace {

std::string rangeText(const RgPlan& plan, u32 physical, const rhi::SubresourceRange& r) {
    if (physical != kRgInvalid && !plan.physicals[physical].isTexture) return {};
    return std::format(" mips={}+{} layers={}+{}", r.baseMip, r.mipCount, r.baseLayer, r.layerCount);
}

std::string stateText(rhi::ResourceState s) {
    return s == kRgEntryState ? std::string("entry") : std::string(rhi::resourceStateName(s));
}

std::string physicalName(const RgPlan& plan, u32 physical) {
    const RgPhysicalInfo& p = plan.physicals[physical];
    return std::format("p{}:{}", physical, plan.resources[p.residents.front()].name);
}

std::string barrierText(const RgPlan& plan, const RgBarrier& b) {
    return std::format("{} {}->{}{}", physicalName(plan, b.physical), stateText(b.before), stateText(b.after),
                       rangeText(plan, b.physical, b.range));
}

std::string descText(const RgResourceInfo& r) {
    if (!r.isTexture) return std::format("buffer {} bytes", r.bufferSize);
    const RgTextureDesc& d = r.texture;
    std::string text = std::format("texture {} {}x{}", rhi::formatName(d.format), d.width, d.height);
    if (d.type == rhi::TextureType::Tex3D) text += std::format("x{}", d.depth);
    if (d.type == rhi::TextureType::Cube) text += " cube";
    text += std::format(" mips={} layers={}", d.mipLevels, d.arrayLayers);
    if (d.sampleCount > 1) text += std::format(" samples={}", d.sampleCount);
    return text;
}

std::string_view heapName(RgHeapKind kind) {
    switch (kind) {
    case RgHeapKind::Buffers: return "Buffers";
    case RgHeapKind::RenderTargets: return "RenderTargets";
    case RgHeapKind::Textures: return "Textures";
    }
    return "?";
}

std::string_view batchKindName(RgBatchInfo::Kind kind) {
    switch (kind) {
    case RgBatchInfo::Kind::Passes: return "";
    case RgBatchInfo::Kind::Prologue: return " prologue";
    case RgBatchInfo::Kind::Epilogue: return " epilogue";
    }
    return "";
}

std::string dotEscape(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

} // namespace

std::string rgDumpPlan(const RgPlan& plan) {
    std::string out;
    const RgCompileStats& s = plan.stats;
    out += std::format("RenderGraph \"{}\": passes={} culled={} batches={} lists={} barriers={} waits={}\n", plan.name,
                       s.passCount, s.culledPassCount, s.batchCount, s.commandListCount, s.barrierCount,
                       s.crossQueueWaitCount);
    out += "resources:\n";
    for (u32 i = 0; i < plan.resources.size(); ++i) {
        const RgResourceInfo& r = plan.resources[i];
        out += std::format("  r{} \"{}\" {}", i, r.name, descText(r));
        if (r.imported) {
            out += std::format(" imported initial={} final={}", stateText(r.import.initialState),
                               r.import.finalState == kRgEntryState ? std::string("last")
                                                                    : stateText(r.import.finalState));
        }
        out += std::format(" usage={} versions={}",
                           r.isTexture ? rgTextureUsageName(r.textureUsage) : rgBufferUsageName(r.bufferUsage),
                           r.versionCount);
        if (!r.used) {
            out += " unused\n";
            continue;
        }
        out += std::format(" life={}..{} physical=p{}", r.firstPosition, r.lastPosition, r.physical);
        if (!r.imported) out += std::format(" bytes={} heap={}@{}", r.bytes, heapName(r.heap), r.heapOffset);
        out += '\n';
    }
    out += "physicals:\n";
    for (u32 i = 0; i < plan.physicals.size(); ++i) {
        const RgPhysicalInfo& p = plan.physicals[i];
        out += std::format("  p{}{} {}", i, p.imported ? " imported" : "", descText(plan.resources[p.residents.front()]));
        if (!p.imported) {
            out += std::format(" usage={}",
                               p.isTexture ? rgTextureUsageName(p.textureUsage) : rgBufferUsageName(p.bufferUsage));
        }
        out += " residents=[";
        for (usize k = 0; k < p.residents.size(); ++k) {
            out += std::format("{}{}", k ? ", " : "", plan.resources[p.residents[k]].name);
        }
        out += "]\n";
    }
    out += "batches:\n";
    for (u32 b = 0; b < plan.batches.size(); ++b) {
        const RgBatchInfo& batch = plan.batches[b];
        out += std::format("  b{} {}#{}{} passes=[", b, rgQueueName(batch.queue), batch.queueValue,
                           batchKindName(batch.kind));
        for (usize k = 0; k < batch.passes.size(); ++k) {
            out += std::format("{}{}", k ? ", " : "", plan.passes[batch.passes[k]].name);
        }
        out += "] waits=[";
        for (usize k = 0; k < batch.waits.size(); ++k) out += std::format("{}b{}", k ? ", " : "", batch.waits[k]);
        out += std::format("] lists={}", batch.listCount);
        out += '\n';
        for (const RgBarrier& bar : batch.barriers) out += "    barrier " + barrierText(plan, bar) + "\n";
    }
    out += "passes:\n";
    for (u32 p : plan.order) {
        const RgPassInfo& pass = plan.passes[p];
        out += std::format("  #{} \"{}\" {} {} b{} L{}{}\n", pass.position, pass.name, rgPassKindName(pass.flags),
                           rgQueueName(pass.queue), pass.batch, pass.list,
                           hasFlag(pass.flags, PassFlags::NeverCull) ? " never-cull" : "");
        for (const RgAccess& a : pass.accesses) {
            const RgResourceInfo& r = plan.resources[a.resource];
            if (a.isWrite()) {
                out += std::format("    write {}@{} {}{}", r.name, a.writeVersion, rhi::resourceStateName(a.state),
                                   rangeText(plan, r.physical, a.range));
                if (a.readVersion != kRgInvalid) out += std::format(" keeps@{}", a.readVersion);
            } else {
                out += std::format("    read  {}@{} {}{}", r.name, a.readVersion, rhi::resourceStateName(a.state),
                                   rangeText(plan, r.physical, a.range));
            }
            out += '\n';
        }
        for (usize i = 0; i < pass.colorStore.size(); ++i) {
            if (pass.colorStore[i] == rhi::StoreOp::DontCare) out += std::format("    color{} store=DontCare\n", i);
        }
        if (pass.depthStore == rhi::StoreOp::DontCare) out += "    depth store=DontCare\n";
        for (const RgBarrier& b : pass.preBarriers) out += "    pre  " + barrierText(plan, b) + "\n";
        for (const RgBarrier& b : pass.postBarriers) out += "    post " + barrierText(plan, b) + "\n";
    }
    bool anyCulled = false;
    for (const RgPassInfo& pass : plan.passes) {
        if (!pass.culled) continue;
        if (!anyCulled) out += "culled:\n";
        anyCulled = true;
        out += std::format("  \"{}\" {}\n", pass.name, rgPassKindName(pass.flags));
    }
    out += std::format("memory: transient={} pooled={} placed={} (buffers={} rendertargets={} textures={})\n",
                       s.transientBytes, s.pooledBytes, s.placedBytes, s.heapBytes[0], s.heapBytes[1], s.heapBytes[2]);
    return out;
}

std::string RenderGraph::dumpText() const { return rgDumpPlan(m_impl->plan); }

std::string RenderGraph::dumpGraphviz() const {
    const RgPlan& plan = m_impl->plan;
    static constexpr std::string_view kQueueColor[] = {"#d6e6ff", "#ffe2c2", "#e0f0d0"};
    std::string out = std::format("digraph \"{}\" {{\n", dotEscape(plan.name));
    out += "  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n  edge [fontname=\"Helvetica\", fontsize=9];\n";
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        const RgPassInfo& pass = plan.passes[p];
        if (pass.culled) {
            out += std::format("  pass{} [shape=box, style=dashed, color=\"#999999\", fontcolor=\"#999999\", "
                               "label=\"{}\\n(culled)\"];\n",
                               p, dotEscape(pass.name));
        } else {
            out += std::format("  pass{} [shape=box, style=filled, fillcolor=\"{}\", label=\"#{} {}\\n{} b{}\\n{} "
                               "barriers\"];\n",
                               p, kQueueColor[rhi::queueIndex(pass.queue)], pass.position, dotEscape(pass.name),
                               rgQueueName(pass.queue), pass.batch, pass.preBarriers.size() + pass.postBarriers.size());
        }
    }
    // Resource versions: one node per (resource, version) that some pass touches.
    std::vector<std::vector<u8>> emitted(plan.resources.size());
    for (u32 r = 0; r < plan.resources.size(); ++r) emitted[r].assign(plan.resources[r].versionCount, 0);
    auto versionNode = [&](u32 r, u32 v) {
        if (!emitted[r][v]) {
            emitted[r][v] = 1;
            const RgResourceInfo& info = plan.resources[r];
            out += std::format("  r{}v{} [shape=ellipse, style={}, label=\"{}@{}{}\"];\n", r, v,
                               info.imported ? "bold" : "solid", dotEscape(info.name), v,
                               info.physical != kRgInvalid ? std::format("\\np{}", info.physical) : std::string());
        }
        return std::format("r{}v{}", r, v);
    };
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        const RgPassInfo& pass = plan.passes[p];
        const std::string style = pass.culled ? ", style=dashed, color=\"#999999\"" : "";
        for (const RgAccess& a : pass.accesses) {
            if (a.readVersion != kRgInvalid) {
                out += std::format("  {} -> pass{} [label=\"{}\"{}];\n", versionNode(a.resource, a.readVersion), p,
                                   rhi::resourceStateName(a.state), style);
            }
            if (a.isWrite()) {
                out += std::format("  pass{} -> {} [color=\"#c0392b\", label=\"{}\"{}];\n", p,
                                   versionNode(a.resource, a.writeVersion), rhi::resourceStateName(a.state), style);
            }
        }
    }
    for (u32 b = 0; b < plan.batches.size(); ++b) {
        const RgBatchInfo& batch = plan.batches[b];
        if (batch.passes.empty()) continue;
        for (u32 w : batch.waits) {
            const RgBatchInfo& from = plan.batches[w];
            if (from.passes.empty()) continue;
            out += std::format("  pass{} -> pass{} [style=dashed, color=\"#2255cc\", label=\"wait\"];\n",
                               from.passes.back(), batch.passes.front());
        }
    }
    out += "}\n";
    return out;
}

} // namespace helios::render
