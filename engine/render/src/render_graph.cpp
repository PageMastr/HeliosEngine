// Render graph setup: resources, passes, the builder's access declarations and their validation,
// RgContext resolution, the resource pool and shared helpers (03 §2.1).

#include <algorithm>
#include <bit>

#include "graph_internal.h"

namespace helios::render {

namespace {

struct AccessInfo {
    rhi::ResourceState state;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
};

AccessInfo textureReadInfo(TextureRead access) {
    switch (access) {
    case TextureRead::Sampled: return {rhi::ResourceState::ShaderResource, rhi::TextureUsage::Sampled};
    case TextureRead::Storage: return {rhi::ResourceState::UnorderedAccess, rhi::TextureUsage::Storage};
    case TextureRead::CopySource: return {rhi::ResourceState::CopySource, rhi::TextureUsage::TransferSrc};
    case TextureRead::DepthSampled: return {rhi::ResourceState::DepthRead, rhi::TextureUsage::Sampled};
    }
    return {rhi::ResourceState::ShaderResource, rhi::TextureUsage::Sampled};
}

AccessInfo textureWriteInfo(TextureWrite access) {
    switch (access) {
    case TextureWrite::Storage: return {rhi::ResourceState::UnorderedAccess, rhi::TextureUsage::Storage};
    case TextureWrite::CopyDest: return {rhi::ResourceState::CopyDest, rhi::TextureUsage::TransferDst};
    }
    return {rhi::ResourceState::UnorderedAccess, rhi::TextureUsage::Storage};
}

AccessInfo bufferReadInfo(BufferRead access) {
    AccessInfo i{rhi::ResourceState::ShaderResource};
    switch (access) {
    case BufferRead::Shader: i = {rhi::ResourceState::ShaderResource, {}, rhi::BufferUsage::Storage}; break;
    case BufferRead::Indirect: i = {rhi::ResourceState::IndirectArgument, {}, rhi::BufferUsage::Indirect}; break;
    case BufferRead::Vertex: i = {rhi::ResourceState::VertexBuffer, {}, rhi::BufferUsage::Vertex}; break;
    case BufferRead::Index: i = {rhi::ResourceState::IndexBuffer, {}, rhi::BufferUsage::Index}; break;
    case BufferRead::Constant: i = {rhi::ResourceState::ConstantBuffer, {}, rhi::BufferUsage::Uniform}; break;
    case BufferRead::CopySource: i = {rhi::ResourceState::CopySource, {}, rhi::BufferUsage::TransferSrc}; break;
    }
    return i;
}

AccessInfo bufferWriteInfo(BufferWrite access) {
    if (access == BufferWrite::CopyDest) return {rhi::ResourceState::CopyDest, {}, rhi::BufferUsage::TransferDst};
    return {rhi::ResourceState::UnorderedAccess, {}, rhi::BufferUsage::Storage};
}

bool rangesOverlap(const rhi::SubresourceRange& a, const rhi::SubresourceRange& b) {
    return a.baseMip < b.baseMip + b.mipCount && b.baseMip < a.baseMip + a.mipCount &&
           a.baseLayer < b.baseLayer + b.layerCount && b.baseLayer < a.baseLayer + a.layerCount;
}

std::string_view passKindName(PassFlags flags) {
    if (hasFlag(flags, PassFlags::Raster)) return "Raster";
    if (hasFlag(flags, PassFlags::AsyncCompute)) return "AsyncCompute";
    if (hasFlag(flags, PassFlags::Compute)) return "Compute";
    if (hasFlag(flags, PassFlags::Copy)) return "Copy";
    return "?";
}

u32 maxMipCount(const RgTextureDesc& d) {
    u32 largest = std::max({d.width, d.height, d.type == rhi::TextureType::Tex3D ? d.depth : 1u});
    return static_cast<u32>(std::bit_width(largest));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------
u32 rgStateLevel(rhi::ResourceState state) noexcept {
    switch (state) {
    case rhi::ResourceState::RenderTarget:
    case rhi::ResourceState::DepthWrite:
    case rhi::ResourceState::DepthRead:
    case rhi::ResourceState::VertexBuffer:
    case rhi::ResourceState::IndexBuffer:
    case rhi::ResourceState::Present: return 2;
    case rhi::ResourceState::ShaderResource:
    case rhi::ResourceState::UnorderedAccess:
    case rhi::ResourceState::ConstantBuffer:
    case rhi::ResourceState::IndirectArgument:
    case rhi::ResourceState::General: return 1;
    default: return 0;
    }
}

u32 rgQueueLevel(rhi::Queue queue) noexcept {
    switch (queue) {
    case rhi::Queue::Graphics: return 2;
    case rhi::Queue::AsyncCompute: return 1;
    case rhi::Queue::Transfer: return 0;
    }
    return 0;
}

std::string_view rgQueueName(rhi::Queue queue) noexcept { return rhi::queueName(queue); }

u64 rgTextureBytes(const RgTextureDesc& desc) noexcept {
    u64 total = 0;
    for (u32 m = 0; m < desc.mipLevels; ++m) {
        const u32 d = desc.type == rhi::TextureType::Tex3D ? rhi::mipExtent(desc.depth, m) : 1u;
        total += rhi::formatSurfaceBytes(desc.format, rhi::mipExtent(desc.width, m), rhi::mipExtent(desc.height, m), d);
    }
    return total * desc.arrayLayers * std::max(desc.sampleCount, 1u);
}

std::string rgTextureUsageName(rhi::TextureUsage usage) {
    static constexpr std::pair<rhi::TextureUsage, std::string_view> kNames[] = {
        {rhi::TextureUsage::Sampled, "Sampled"},         {rhi::TextureUsage::Storage, "Storage"},
        {rhi::TextureUsage::ColorAttachment, "Color"},   {rhi::TextureUsage::DepthStencil, "Depth"},
        {rhi::TextureUsage::TransferSrc, "TransferSrc"}, {rhi::TextureUsage::TransferDst, "TransferDst"},
    };
    std::string out;
    for (const auto& [bit, name] : kNames) {
        if (!hasFlag(usage, bit)) continue;
        if (!out.empty()) out += '|';
        out += name;
    }
    return out.empty() ? std::string("None") : out;
}

std::string rgBufferUsageName(rhi::BufferUsage usage) {
    static constexpr std::pair<rhi::BufferUsage, std::string_view> kNames[] = {
        {rhi::BufferUsage::Vertex, "Vertex"},           {rhi::BufferUsage::Index, "Index"},
        {rhi::BufferUsage::Uniform, "Uniform"},         {rhi::BufferUsage::Storage, "Storage"},
        {rhi::BufferUsage::Indirect, "Indirect"},       {rhi::BufferUsage::TransferSrc, "TransferSrc"},
        {rhi::BufferUsage::TransferDst, "TransferDst"},
    };
    std::string out;
    for (const auto& [bit, name] : kNames) {
        if (!hasFlag(usage, bit)) continue;
        if (!out.empty()) out += '|';
        out += name;
    }
    return out.empty() ? std::string("None") : out;
}

std::vector<RgBarrier> rgMergeBarriers(std::span<const RgSubBarrier> subs, std::span<const RgPhysicalInfo> physicals) {
    std::vector<RgBarrier> out;
    // Group by (physical, before, after) in order of first appearance.
    struct Group {
        u32 physical;
        rhi::ResourceState before, after;
        std::vector<u8> mask;  // per subresource
    };
    std::vector<Group> groups;
    for (const RgSubBarrier& s : subs) {
        Group* g = nullptr;
        for (Group& candidate : groups) {
            if (candidate.physical == s.physical && candidate.before == s.before && candidate.after == s.after) {
                g = &candidate;
                break;
            }
        }
        const RgPhysicalInfo& phys = physicals[s.physical];
        if (!g) {
            groups.push_back({s.physical, s.before, s.after, std::vector<u8>(phys.subresourceCount(), 0)});
            g = &groups.back();
        }
        g->mask[phys.isTexture ? rgSub(s.mip, s.layer, phys.texture.arrayLayers) : 0] = 1;
    }
    for (const Group& g : groups) {
        const RgPhysicalInfo& phys = physicals[g.physical];
        if (!phys.isTexture) {
            out.push_back({g.physical, g.before, g.after, {0, 1, 0, 1}});
            continue;
        }
        const u32 mips = phys.texture.mipLevels;
        const u32 layers = phys.texture.arrayLayers;
        // Layer runs per mip, then merge consecutive mips with identical runs.
        struct Run {
            u32 layer, count;
            bool operator==(const Run&) const = default;
        };
        std::vector<std::vector<Run>> runs(mips);
        for (u32 m = 0; m < mips; ++m) {
            u32 l = 0;
            while (l < layers) {
                if (!g.mask[rgSub(m, l, layers)]) {
                    ++l;
                    continue;
                }
                u32 e = l;
                while (e < layers && g.mask[rgSub(m, e, layers)]) ++e;
                runs[m].push_back({l, e - l});
                l = e;
            }
        }
        u32 m = 0;
        while (m < mips) {
            if (runs[m].empty()) {
                ++m;
                continue;
            }
            u32 e = m + 1;
            while (e < mips && runs[e] == runs[m]) ++e;
            for (const Run& r : runs[m]) out.push_back({g.physical, g.before, g.after, {m, e - m, r.layer, r.count}});
            m = e;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// RenderGraph setup
// ---------------------------------------------------------------------------------------------
RenderGraph::RenderGraph(std::string_view name) : m_impl(std::make_unique<Impl>()) { m_impl->name = name; }
RenderGraph::~RenderGraph() = default;
RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

std::string_view RenderGraph::name() const noexcept { return m_impl->name; }
bool RenderGraph::isCompiled() const noexcept { return m_impl->compiled; }
const RgPlan& RenderGraph::plan() const noexcept { return m_impl->plan; }
const RgPlan& RenderGraph::executedPlan() const noexcept { return m_impl->executed; }
const std::vector<std::string>& RenderGraph::errors() const noexcept { return m_impl->errors; }
u64 RenderGraph::contextErrorCount() const noexcept { return m_impl->contextErrors.load(std::memory_order_relaxed); }
u32 RenderGraph::passCount() const noexcept { return static_cast<u32>(m_impl->passes.size()); }
u32 RenderGraph::resourceCount() const noexcept { return static_cast<u32>(m_impl->resources.size()); }

RgTexture RenderGraph::importTexture(std::string_view name, rhi::TextureH texture, const rhi::TextureDesc& desc,
                                     const RgImport& state) {
    Impl& g = *m_impl;
    g.compiled = false;
    if (!texture.isValid() || desc.width == 0 || desc.height == 0) {
        g.error("importTexture('{}'): null handle or empty description", name);
        return {};
    }
    RgResourceDecl r;
    r.name = name;
    r.isTexture = true;
    r.imported = true;
    r.texture.type = desc.type;
    r.texture.format = desc.format;
    r.texture.width = desc.width;
    r.texture.height = desc.height;
    r.texture.depth = desc.depth;
    r.texture.mipLevels = std::max(desc.mipLevels, 1u);
    r.texture.arrayLayers = std::max(desc.arrayLayers, 1u);
    r.texture.sampleCount = std::max(desc.sampleCount, 1u);
    r.importTextureUsage = desc.usage;
    r.importedTexture = texture;
    r.import = state;
    if (state.finalState == rhi::ResourceState::Undefined) {
        g.error("importTexture('{}'): the final state cannot be Undefined", name);
    }
    r.producers.push_back(kRgInvalid);
    r.outputs.push_back(0);
    g.resources.push_back(std::move(r));
    return RgTexture{static_cast<u32>(g.resources.size() - 1), 0};
}

RgBuffer RenderGraph::importBuffer(std::string_view name, rhi::BufferH buffer, const rhi::BufferDesc& desc,
                                   const RgImport& state) {
    Impl& g = *m_impl;
    g.compiled = false;
    if (!buffer.isValid() || desc.size == 0) {
        g.error("importBuffer('{}'): null handle or empty description", name);
        return {};
    }
    RgResourceDecl r;
    r.name = name;
    r.isTexture = false;
    r.imported = true;
    r.bufferSize = desc.size;
    r.importBufferUsage = desc.usage;
    r.importedBuffer = buffer;
    r.import = state;
    if (state.finalState == rhi::ResourceState::Undefined) {
        g.error("importBuffer('{}'): the final state cannot be Undefined", name);
    }
    r.producers.push_back(kRgInvalid);
    r.outputs.push_back(0);
    g.resources.push_back(std::move(r));
    return RgBuffer{static_cast<u32>(g.resources.size() - 1), 0};
}

void RenderGraph::markOutput(RgTexture texture) {
    Impl& g = *m_impl;
    g.compiled = false;
    if (texture.id >= g.resources.size() || !g.resources[texture.id].isTexture ||
        texture.version > g.resources[texture.id].latestVersion()) {
        g.error("markOutput: invalid texture handle");
        return;
    }
    g.resources[texture.id].outputs[texture.version] = 1;
}

void RenderGraph::markOutput(RgBuffer buffer) {
    Impl& g = *m_impl;
    g.compiled = false;
    if (buffer.id >= g.resources.size() || g.resources[buffer.id].isTexture ||
        buffer.version > g.resources[buffer.id].latestVersion()) {
        g.error("markOutput: invalid buffer handle");
        return;
    }
    g.resources[buffer.id].outputs[buffer.version] = 1;
}

u32 RenderGraph::beginPass(std::string_view name, PassFlags flags, std::unique_ptr<PassDataBase> data) {
    Impl& g = *m_impl;
    g.compiled = false;
    const PassFlags kinds = PassFlags::Raster | PassFlags::Compute | PassFlags::AsyncCompute | PassFlags::Copy;
    const u32 kindBits = static_cast<u32>(std::popcount(toUnderlying(flags & kinds)));
    if (kindBits != 1) {
        g.error("pass '{}': exactly one of Raster, Compute, AsyncCompute or Copy must be set", name);
    }
    RgPassDecl pass;
    pass.name = name;
    pass.flags = flags;
    g.passes.push_back(std::move(pass));
    g.passData.push_back(std::move(data));
    return static_cast<u32>(g.passes.size() - 1);
}

void RenderGraph::endPass(u32 passIndex, std::function<void(RgContext&)> execute) {
    Impl& g = *m_impl;
    RgPassDecl& pass = g.passes[passIndex];
    pass.execute = std::move(execute);
    for (const RgAccessDecl& a : pass.accesses) pass.declared.push_back(a.access.resource);
    std::sort(pass.declared.begin(), pass.declared.end());
    pass.declared.erase(std::unique(pass.declared.begin(), pass.declared.end()), pass.declared.end());

    if (!hasFlag(pass.flags, PassFlags::Raster)) return;
    // Raster passes: contiguous color slots, at least one attachment, one common extent.
    bool any = pass.depth.resource != kRgInvalid;
    bool gap = false;
    for (usize i = 0; i < pass.colors.size(); ++i) {
        if (pass.colors[i].resource == kRgInvalid) {
            gap = true;
        } else {
            any = true;
            if (gap) g.error("pass '{}': color attachment slots must be contiguous from 0", pass.name);
        }
    }
    if (!any) {
        g.error("pass '{}': a Raster pass needs at least one attachment", pass.name);
        return;
    }
    u32 width = 0;
    u32 height = 0;
    auto check = [&](const RgAttachmentDecl& att) {
        if (att.resource == kRgInvalid) return;
        const RgTextureDesc& d = g.resources[att.resource].texture;
        const u32 w = rhi::mipExtent(d.width, att.mip);
        const u32 h = rhi::mipExtent(d.height, att.mip);
        if (width == 0) {
            width = w;
            height = h;
        } else if (w != width || h != height) {
            g.error("pass '{}': attachment '{}' is {}x{}, others are {}x{}", pass.name, g.resources[att.resource].name,
                    w, h, width, height);
        }
    };
    for (const RgAttachmentDecl& att : pass.colors) check(att);
    check(pass.depth);
}

void RenderGraph::addPass(std::string_view name, PassFlags flags, const std::function<void(RgBuilder&)>& setup,
                          std::function<void(RgContext&)> execute) {
    const u32 pass = beginPass(name, flags, nullptr);
    RgBuilder builder(*this, pass);
    if (setup) setup(builder);
    endPass(pass, std::move(execute));
}

// ---------------------------------------------------------------------------------------------
// RgBuilder
// ---------------------------------------------------------------------------------------------
namespace {

/// Validates `id`/`version` for use in `pass` and returns its version entry (null on error).
RgPassDecl::Versions* touchResource(RenderGraph::Impl& g, u32 passIndex, u32 id, u32 version, bool isTexture,
                                    std::string_view what) {
    RgPassDecl& pass = g.passes[passIndex];
    if (id >= g.resources.size()) {
        g.error("pass '{}': {} of an invalid {} handle", pass.name, what, isTexture ? "texture" : "buffer");
        return nullptr;
    }
    RgResourceDecl& r = g.resources[id];
    if (r.isTexture != isTexture) {
        g.error("pass '{}': {} of '{}' through a handle of the wrong type", pass.name, what, r.name);
        return nullptr;
    }
    for (RgPassDecl::Versions& v : pass.versions) {
        if (v.resource != id) continue;
        if (version != v.input && version != v.output) {
            g.error("pass '{}': {} of '{}' version {}, but the pass works on version {}", pass.name, what, r.name,
                    version, v.input);
            return nullptr;
        }
        return &v;
    }
    if (version != r.latestVersion()) {
        const u32 writer = r.producers[r.latestVersion()];
        g.error("pass '{}': {} of stale '{}' version {} (pass '{}' already produced version {})", pass.name, what, r.name,
                version, writer == kRgInvalid ? std::string_view("?") : std::string_view(g.passes[writer].name),
                r.latestVersion());
        return nullptr;
    }
    pass.versions.push_back({id, version, kRgInvalid});
    return &pass.versions.back();
}

bool normalizeRange(RenderGraph::Impl& g, const RgPassDecl& pass, const RgResourceDecl& r,
                    const rhi::SubresourceRange& in, rhi::SubresourceRange& out) {
    const u32 mips = r.mipLevels();
    const u32 layers = r.arrayLayers();
    out.baseMip = in.baseMip;
    out.baseLayer = in.baseLayer;
    out.mipCount = in.mipCount == rhi::kAllMips ? (in.baseMip < mips ? mips - in.baseMip : 0) : in.mipCount;
    out.layerCount =
        in.layerCount == rhi::kAllLayers ? (in.baseLayer < layers ? layers - in.baseLayer : 0) : in.layerCount;
    // 64-bit sums: base + count must not wrap around (e.g. {2, 0xFFFFFFFE} or a mip of 0xFFFFFFFF).
    if (out.mipCount == 0 || out.layerCount == 0 || u64{out.baseMip} + out.mipCount > mips ||
        u64{out.baseLayer} + out.layerCount > layers) {
        g.error("pass '{}': subresource range mips {}+{} layers {}+{} is outside '{}' ({} mips, {} layers)", pass.name,
                in.baseMip, in.mipCount, in.baseLayer, in.layerCount, r.name, mips, layers);
        return false;
    }
    return true;
}

/// Records one access after validating queue compatibility, usage and intra-pass conflicts.
bool addAccess(RenderGraph::Impl& g, u32 passIndex, RgAccess access, const AccessInfo& info) {
    RgPassDecl& pass = g.passes[passIndex];
    RgResourceDecl& r = g.resources[access.resource];
    access.state = info.state;
    // Queue compatibility (the pass's declared kind, independent of RgCompileOptions::asyncCompute).
    const u32 level = rgStateLevel(info.state);
    if (hasFlag(pass.flags, PassFlags::AsyncCompute) && level > 1) {
        g.error("pass '{}': async-compute passes cannot use '{}' as {}", pass.name, r.name,
                rhi::resourceStateName(info.state));
        return false;
    }
    if (hasFlag(pass.flags, PassFlags::Copy) && info.state != rhi::ResourceState::CopySource &&
        info.state != rhi::ResourceState::CopyDest) {
        g.error("pass '{}': copy passes can only use copy states ('{}' as {})", pass.name, r.name,
                rhi::resourceStateName(info.state));
        return false;
    }
    if (hasFlag(pass.flags, PassFlags::Compute) &&
        (info.state == rhi::ResourceState::RenderTarget || info.state == rhi::ResourceState::DepthWrite ||
         info.state == rhi::ResourceState::VertexBuffer || info.state == rhi::ResourceState::IndexBuffer)) {
        g.error("pass '{}': compute passes cannot use '{}' as {}", pass.name, r.name, rhi::resourceStateName(info.state));
        return false;
    }
    // Usage supported by imported resources.
    if (r.imported) {
        if (r.isTexture && !hasFlag(r.importTextureUsage, info.textureUsage)) {
            g.error("pass '{}': imported '{}' lacks usage {} (has {})", pass.name, r.name,
                    rgTextureUsageName(info.textureUsage), rgTextureUsageName(r.importTextureUsage));
            return false;
        }
        if (!r.isTexture && !hasFlag(r.importBufferUsage, info.bufferUsage)) {
            g.error("pass '{}': imported '{}' lacks usage {} (has {})", pass.name, r.name,
                    rgBufferUsageName(info.bufferUsage), rgBufferUsageName(r.importBufferUsage));
            return false;
        }
    }
    // Conflicting states on overlapping subresources within one pass.
    for (const RgAccessDecl& other : pass.accesses) {
        if (other.access.resource != access.resource || !rangesOverlap(other.access.range, access.range)) continue;
        if (other.access.state != access.state) {
            g.error("pass '{}': '{}' is used as both {} and {} on overlapping subresources", pass.name, r.name,
                    rhi::resourceStateName(other.access.state), rhi::resourceStateName(access.state));
            return false;
        }
    }
    r.textureUsage |= info.textureUsage;
    r.bufferUsage |= info.bufferUsage;
    pass.accesses.push_back({access, info.textureUsage, info.bufferUsage});
    return true;
}

/// Declares a read of (id, version) over `range`.
bool declareRead(RenderGraph::Impl& g, u32 passIndex, u32 id, u32 version, bool isTexture,
                 const rhi::SubresourceRange& range, const AccessInfo& info) {
    RgPassDecl::Versions* v = touchResource(g, passIndex, id, version, isTexture, "read");
    if (!v) return false;
    RgPassDecl& pass = g.passes[passIndex];
    RgResourceDecl& r = g.resources[id];
    if (v->output != kRgInvalid && version == v->output) {
        g.error("pass '{}': reads its own output '{}' (use readWrite)", pass.name, r.name);
        return false;
    }
    if (!r.imported && v->input == 0) {
        g.error("pass '{}': reads '{}' before any pass wrote it", pass.name, r.name);
        return false;
    }
    RgAccess a;
    a.resource = id;
    a.readVersion = v->input;
    if (!normalizeRange(g, pass, r, range, a.range)) return false;
    return addAccess(g, passIndex, a, info);
}

/// Declares a write; returns the produced version or kRgInvalid.
u32 declareWrite(RenderGraph::Impl& g, u32 passIndex, u32 id, u32 version, bool isTexture,
                 const rhi::SubresourceRange& range, const AccessInfo& info, bool preserve) {
    RgPassDecl::Versions* v = touchResource(g, passIndex, id, version, isTexture, "write");
    if (!v) return kRgInvalid;
    RgPassDecl& pass = g.passes[passIndex];
    RgResourceDecl& r = g.resources[id];
    RgAccess a;
    a.resource = id;
    if (!normalizeRange(g, pass, r, range, a.range)) return kRgInvalid;
    const bool partial = a.range.mipCount != r.mipLevels() || a.range.layerCount != r.arrayLayers();
    if (v->output == kRgInvalid) {
        v->output = static_cast<u32>(r.producers.size());
        r.producers.push_back(passIndex);
        r.outputs.push_back(0);
    }
    a.writeVersion = v->output;
    if (preserve || partial) a.readVersion = v->input;
    if (!addAccess(g, passIndex, a, info)) return kRgInvalid;
    return v->output;
}

} // namespace

RgTexture RgBuilder::create(std::string_view name, const RgTextureDesc& desc) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    const RgPassDecl& pass = g.passes[m_pass];
    const rhi::FormatInfo& fi = rhi::formatInfo(desc.format);
    // Layer and sample limits keep the per-subresource state tables small (mips * layers can no
    // longer wrap around 32 bits) and match what every supported device offers.
    if (fi.blockBytes == 0 || desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.mipLevels == 0 ||
        desc.arrayLayers == 0 || desc.arrayLayers > kRgMaxArrayLayers || desc.sampleCount == 0 ||
        desc.sampleCount > 64 || !std::has_single_bit(desc.sampleCount) || desc.mipLevels > maxMipCount(desc) ||
        (desc.type == rhi::TextureType::Cube && desc.arrayLayers % 6 != 0) ||
        (desc.type == rhi::TextureType::Tex3D && desc.arrayLayers != 1)) {
        g.error("pass '{}': invalid description for texture '{}'", pass.name, name);
        return {};
    }
    RgResourceDecl r;
    r.name = name;
    r.isTexture = true;
    r.texture = desc;
    r.producers.push_back(kRgInvalid);
    r.outputs.push_back(0);
    g.resources.push_back(std::move(r));
    return RgTexture{static_cast<u32>(g.resources.size() - 1), 0};
}

RgBuffer RgBuilder::create(std::string_view name, const RgBufferDesc& desc) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    if (desc.size == 0) {
        g.error("pass '{}': buffer '{}' has size 0", g.passes[m_pass].name, name);
        return {};
    }
    RgResourceDecl r;
    r.name = name;
    r.isTexture = false;
    r.bufferSize = desc.size;
    r.producers.push_back(kRgInvalid);
    r.outputs.push_back(0);
    g.resources.push_back(std::move(r));
    return RgBuffer{static_cast<u32>(g.resources.size() - 1), 0};
}

RgTexture RgBuilder::read(RgTexture texture, TextureRead access, const rhi::SubresourceRange& range) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    if (access == TextureRead::DepthSampled && texture.id < g.resources.size() &&
        !rhi::isDepthFormat(g.resources[texture.id].texture.format)) {
        g.error("pass '{}': DepthSampled read of non-depth texture '{}'", g.passes[m_pass].name,
                g.resources[texture.id].name);
        return {};
    }
    return declareRead(g, m_pass, texture.id, texture.version, true, range, textureReadInfo(access)) ? texture
                                                                                                     : RgTexture{};
}

RgBuffer RgBuilder::read(RgBuffer buffer, BufferRead access) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    return declareRead(g, m_pass, buffer.id, buffer.version, false, {}, bufferReadInfo(access)) ? buffer : RgBuffer{};
}

RgTexture RgBuilder::write(RgTexture texture, TextureWrite access, const rhi::SubresourceRange& range) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    const u32 v = declareWrite(g, m_pass, texture.id, texture.version, true, range, textureWriteInfo(access), false);
    return v == kRgInvalid ? RgTexture{} : RgTexture{texture.id, v};
}

RgBuffer RgBuilder::write(RgBuffer buffer, BufferWrite access) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    const u32 v = declareWrite(g, m_pass, buffer.id, buffer.version, false, {}, bufferWriteInfo(access), false);
    return v == kRgInvalid ? RgBuffer{} : RgBuffer{buffer.id, v};
}

RgTexture RgBuilder::readWrite(RgTexture texture, TextureWrite access, const rhi::SubresourceRange& range) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    const u32 v = declareWrite(g, m_pass, texture.id, texture.version, true, range, textureWriteInfo(access), true);
    return v == kRgInvalid ? RgTexture{} : RgTexture{texture.id, v};
}

RgBuffer RgBuilder::readWrite(RgBuffer buffer, BufferWrite access) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    const u32 v = declareWrite(g, m_pass, buffer.id, buffer.version, false, {}, bufferWriteInfo(access), true);
    return v == kRgInvalid ? RgBuffer{} : RgBuffer{buffer.id, v};
}

RgTexture RgBuilder::colorAttachment(RgTexture texture, u32 slot, rhi::LoadOp load,
                                     const std::array<f32, 4>& clearColor, u32 mip, u32 layer) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    RgPassDecl& pass = g.passes[m_pass];
    if (!hasFlag(pass.flags, PassFlags::Raster)) {
        g.error("pass '{}': attachments need PassFlags::Raster", pass.name);
        return {};
    }
    if (slot >= rhi::kMaxColorAttachments) {
        g.error("pass '{}': color slot {} out of range", pass.name, slot);
        return {};
    }
    if (texture.id < g.resources.size() && rhi::isDepthFormat(g.resources[texture.id].texture.format)) {
        g.error("pass '{}': depth texture '{}' used as a color attachment", pass.name, g.resources[texture.id].name);
        return {};
    }
    if (slot < pass.colors.size() && pass.colors[slot].resource != kRgInvalid) {
        g.error("pass '{}': color slot {} is used twice", pass.name, slot);
        return {};
    }
    const u32 v = declareWrite(g, m_pass, texture.id, texture.version, true, {mip, 1, layer, 1},
                               {rhi::ResourceState::RenderTarget, rhi::TextureUsage::ColorAttachment},
                               load == rhi::LoadOp::Load);
    if (v == kRgInvalid) return {};
    if (pass.colors.size() <= slot) pass.colors.resize(slot + 1);
    RgAttachmentDecl& att = pass.colors[slot];
    att.resource = texture.id;
    att.mip = mip;
    att.layer = layer;
    att.load = load;
    att.clearColor = clearColor;
    return RgTexture{texture.id, v};
}

RgTexture RgBuilder::depthAttachment(RgTexture texture, rhi::LoadOp load, f32 clearDepth, u32 mip, u32 layer) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    RgPassDecl& pass = g.passes[m_pass];
    if (!hasFlag(pass.flags, PassFlags::Raster)) {
        g.error("pass '{}': attachments need PassFlags::Raster", pass.name);
        return {};
    }
    if (texture.id < g.resources.size() && !rhi::isDepthFormat(g.resources[texture.id].texture.format)) {
        g.error("pass '{}': '{}' is not a depth format", pass.name, g.resources[texture.id].name);
        return {};
    }
    if (pass.depth.resource != kRgInvalid) {
        g.error("pass '{}': two depth attachments", pass.name);
        return {};
    }
    const u32 v = declareWrite(g, m_pass, texture.id, texture.version, true, {mip, 1, layer, 1},
                               {rhi::ResourceState::DepthWrite, rhi::TextureUsage::DepthStencil},
                               load == rhi::LoadOp::Load);
    if (v == kRgInvalid) return {};
    pass.depth.resource = texture.id;
    pass.depth.mip = mip;
    pass.depth.layer = layer;
    pass.depth.load = load;
    pass.depth.clearDepth = clearDepth;
    pass.depth.readOnly = false;
    return RgTexture{texture.id, v};
}

RgTexture RgBuilder::depthAttachmentReadOnly(RgTexture texture, u32 mip, u32 layer) {
    RenderGraph::Impl& g = *m_graph.m_impl;
    RgPassDecl& pass = g.passes[m_pass];
    if (!hasFlag(pass.flags, PassFlags::Raster)) {
        g.error("pass '{}': attachments need PassFlags::Raster", pass.name);
        return {};
    }
    if (texture.id < g.resources.size() && !rhi::isDepthFormat(g.resources[texture.id].texture.format)) {
        g.error("pass '{}': '{}' is not a depth format", pass.name, g.resources[texture.id].name);
        return {};
    }
    if (pass.depth.resource != kRgInvalid) {
        g.error("pass '{}': two depth attachments", pass.name);
        return {};
    }
    if (!declareRead(g, m_pass, texture.id, texture.version, true, {mip, 1, layer, 1},
                     {rhi::ResourceState::DepthRead, rhi::TextureUsage::DepthStencil})) {
        return {};
    }
    pass.depth.resource = texture.id;
    pass.depth.mip = mip;
    pass.depth.layer = layer;
    pass.depth.load = rhi::LoadOp::Load;
    pass.depth.readOnly = true;
    return texture;
}

void RgBuilder::neverCull() { m_graph.m_impl->passes[m_pass].flags |= PassFlags::NeverCull; }

const RgTextureDesc& RgBuilder::desc(RgTexture texture) const {
    static const RgTextureDesc kEmpty{};
    const RenderGraph::Impl& g = *m_graph.m_impl;
    return texture.id < g.resources.size() && g.resources[texture.id].isTexture ? g.resources[texture.id].texture
                                                                                : kEmpty;
}

u64 RgBuilder::size(RgBuffer buffer) const {
    const RenderGraph::Impl& g = *m_graph.m_impl;
    return buffer.id < g.resources.size() && !g.resources[buffer.id].isTexture ? g.resources[buffer.id].bufferSize : 0;
}

// ---------------------------------------------------------------------------------------------
// RgContext
// ---------------------------------------------------------------------------------------------
std::string_view RgContext::passName() const noexcept { return m_graph->m_impl->passes[m_pass].name; }

bool RgContext::declared(u32 resource) const {
    const RgPassDecl& pass = m_graph->m_impl->passes[m_pass];
    if (std::binary_search(pass.declared.begin(), pass.declared.end(), resource)) return true;
    m_graph->m_impl->contextErrors.fetch_add(1, std::memory_order_relaxed);
    const auto& resources = m_graph->m_impl->resources;
    HELIOS_LOG_ERROR(LogRenderGraph, "pass '{}' resolves resource '{}' it did not declare", pass.name,
                     resource < resources.size() ? std::string_view(resources[resource].name) : std::string_view("?"));
    return false;
}

rhi::TextureH RgContext::texture(RgTexture texture) const {
    if (!declared(texture.id)) return {};
    const RgResourceInfo& r = m_graph->m_impl->executed.resources[texture.id];
    return r.isTexture && r.physical != kRgInvalid ? (*m_textures)[r.physical] : rhi::TextureH{};
}

rhi::BufferH RgContext::buffer(RgBuffer buffer) const {
    if (!declared(buffer.id)) return {};
    const RgResourceInfo& r = m_graph->m_impl->executed.resources[buffer.id];
    return !r.isTexture && r.physical != kRgInvalid ? (*m_buffers)[r.physical] : rhi::BufferH{};
}

rhi::BindlessIndex RgContext::srv(RgTexture t, const rhi::ViewDesc& view) const {
    const rhi::TextureH h = texture(t);
    return h.isValid() ? m_device->srv(h, view) : rhi::kInvalidBindless;
}

rhi::BindlessIndex RgContext::uav(RgTexture t, u32 mip) const {
    const rhi::TextureH h = texture(t);
    return h.isValid() ? m_device->uav(h, mip) : rhi::kInvalidBindless;
}

rhi::BindlessIndex RgContext::srv(RgBuffer b) const {
    const rhi::BufferH h = buffer(b);
    return h.isValid() ? m_device->srv(h) : rhi::kInvalidBindless;
}

u64 RgContext::deviceAddress(RgBuffer b) const {
    const rhi::BufferH h = buffer(b);
    return h.isValid() ? m_device->deviceAddress(h) : 0;
}

const RgTextureDesc& RgContext::desc(RgTexture texture) const {
    static const RgTextureDesc kEmpty{};
    const auto& resources = m_graph->m_impl->resources;
    return texture.id < resources.size() && resources[texture.id].isTexture ? resources[texture.id].texture : kEmpty;
}

u64 RgContext::size(RgBuffer buffer) const {
    const auto& resources = m_graph->m_impl->resources;
    return buffer.id < resources.size() && !resources[buffer.id].isTexture ? resources[buffer.id].bufferSize : 0;
}

// ---------------------------------------------------------------------------------------------
// RgResourcePool
// ---------------------------------------------------------------------------------------------
RgResourcePool::RgResourcePool(rhi::Device& device, u32 keepFrames) : m_impl(std::make_unique<Impl>()) {
    m_impl->device = &device;
    m_impl->keepFrames = std::max(keepFrames, 1u);
}

RgResourcePool::~RgResourcePool() { clear(); }

rhi::Device& RgResourcePool::device() const noexcept { return *m_impl->device; }
u32 RgResourcePool::textureCount() const noexcept { return static_cast<u32>(m_impl->textures.size()); }
u32 RgResourcePool::bufferCount() const noexcept { return static_cast<u32>(m_impl->buffers.size()); }

u64 RgResourcePool::bytes() const noexcept {
    u64 total = 0;
    for (const RgPoolTexture& t : m_impl->textures) total += t.bytes;
    for (const RgPoolBuffer& b : m_impl->buffers) total += b.size;
    return total;
}

void RgResourcePool::clear() {
    for (RgPoolTexture& t : m_impl->textures) m_impl->device->destroy(t.handle);
    for (RgPoolBuffer& b : m_impl->buffers) m_impl->device->destroy(b.handle);
    m_impl->textures.clear();
    m_impl->buffers.clear();
}

std::string_view rgPassKindName(PassFlags flags) { return passKindName(flags); }

} // namespace helios::render
