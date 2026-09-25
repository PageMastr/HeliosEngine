// Render graph stress test: random DAGs (raster, compute, async compute and copy passes over
// transient and imported textures with mips/layers and buffers, random options) are compiled,
// executed on the Null backend (with and without parallel recording, two frames sharing a resource
// pool) and validated against an independent reference simulator:
//   * culling equals a reference flood over versions;
//   * the schedule covers every live pass once, keeps per-queue declaration order and valid waits;
//   * every pair of conflicting operations on a physical subresource (writes, barriers, state
//     changes) is ordered by happens-before (queue order + a barrier on that subresource, or
//     timeline waits through vector clocks) — catches missing barriers, missing waits and unsafe
//     aliasing of memory used concurrently on two queues;
//   * barrier source states match the simulated states, accesses happen in their declared state;
//   * every read sees the contents written by the version it names (aliasing never clobbers live
//     data);
//   * the placement plan never overlaps memory of resources whose lifetimes are not ordered;
//   * the Null backend reports no validation error and imported resources end in their final state.

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <set>

#include "graph_test_util.h"
#include "helios/core/jobs.h"
#include "helios/core/random.h"

using namespace graphtest;
using rhi::Format;
using rhi::ResourceState;

namespace {

constexpr u32 kQueues = rhi::kQueueCount;

// ---------------------------------------------------------------------------------------------
// Random program generator
// ---------------------------------------------------------------------------------------------
struct TexDescChoice {
    Format format;
    u32 mips;
    u32 layers;
};
constexpr TexDescChoice kTexDescs[] = {
    {Format::RGBA8Unorm, 1, 1},   // color-attachable
    {Format::RGBA16Float, 3, 1},  // mip chain
    {Format::RGBA8Unorm, 1, 2},   // array
    {Format::D32Float, 1, 1},     // depth
};
constexpr u64 kBufSizes[] = {256, 1024};
constexpr u32 kSize = 32;

struct GenResource {
    bool isTexture = true;
    u32 desc = 0;  // index into kTexDescs / kBufSizes
    bool imported = false;
    bool created = false;
    RgTexture tex;
    RgBuffer buf;
    std::vector<u8> defined;  // per subresource: holds data written by some version (or imported)
    rhi::TextureH handle;
    rhi::BufferH bufferHandle;
    ResourceState initial = ResourceState::Undefined;
    ResourceState finalState = kRgEntryState;

    bool isDepth() const { return isTexture && kTexDescs[desc].format == Format::D32Float; }
    u32 mips() const { return isTexture ? kTexDescs[desc].mips : 1u; }
    u32 layers() const { return isTexture ? kTexDescs[desc].layers : 1u; }
    u32 id() const { return isTexture ? tex.id : buf.id; }
};

/// Imported resources live in the device across frames (created once per seed).
struct Imports {
    std::vector<GenResource> resources;
};

struct Program {
    std::vector<GenResource> resources;
    std::set<std::pair<u32, u32>> outputs;  // (resource id, version) marked as outputs
    rhi::BufferH scratch;
};

rhi::SubresourceRange randomRange(Pcg32& rng, u32 mips, u32 layers) {
    const u32 baseMip = uniformU32Below(rng, mips);
    const u32 mipCount = 1 + uniformU32Below(rng, mips - baseMip);
    const u32 baseLayer = uniformU32Below(rng, layers);
    const u32 layerCount = 1 + uniformU32Below(rng, layers - baseLayer);
    return {baseMip, mipCount, baseLayer, layerCount};
}

bool rangeDefined(const GenResource& r, const rhi::SubresourceRange& range) {
    for (u32 m = range.baseMip; m < range.baseMip + range.mipCount; ++m) {
        for (u32 l = range.baseLayer; l < range.baseLayer + range.layerCount; ++l) {
            if (!r.defined[m * r.layers() + l]) return false;
        }
    }
    return true;
}

void defineRange(GenResource& r, const rhi::SubresourceRange& range) {
    for (u32 m = range.baseMip; m < range.baseMip + range.mipCount; ++m) {
        for (u32 l = range.baseLayer; l < range.baseLayer + range.layerCount; ++l) r.defined[m * r.layers() + l] = 1;
    }
}

Imports makeImports(rhi::Device& device, u64 seed) {
    Pcg32 rng(seed, 7);
    Imports imports;
    const u32 count = uniformU32Below(rng, 4);
    for (u32 i = 0; i < count; ++i) {
        GenResource r;
        r.imported = true;
        r.created = true;
        r.isTexture = uniformU32Below(rng, 3) != 0;
        const std::string name = std::format("Imp{}", i);
        if (r.isTexture) {
            r.desc = uniformU32Below(rng, 3);  // no imported depth
            const TexDescChoice& d = kTexDescs[r.desc];
            rhi::TextureDesc td = rhi::TextureDesc::tex2D(d.format, kSize, kSize, kAllColorUsage, name, d.mips);
            td.arrayLayers = d.layers;
            r.handle = device.createTexture(td).value();
            constexpr ResourceState kInit[] = {ResourceState::ShaderResource, ResourceState::UnorderedAccess,
                                               ResourceState::CopySource, ResourceState::RenderTarget};
            r.initial = kInit[uniformU32Below(rng, r.desc == 0 ? 4 : 3)];
            constexpr ResourceState kFinal[] = {kRgEntryState, ResourceState::ShaderResource, ResourceState::CopySource,
                                                ResourceState::Present};
            r.finalState = kFinal[uniformU32Below(rng, 4)];
        } else {
            r.desc = uniformU32Below(rng, 2);
            r.bufferHandle = device
                                 .createBuffer({.size = kBufSizes[r.desc],
                                                .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect |
                                                         rhi::BufferUsage::Vertex | rhi::BufferUsage::Index |
                                                         rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferSrc |
                                                         rhi::BufferUsage::TransferDst,
                                                .name = name})
                                 .value();
            constexpr ResourceState kInit[] = {ResourceState::ShaderResource, ResourceState::UnorderedAccess,
                                               ResourceState::CopyDest};
            r.initial = kInit[uniformU32Below(rng, 3)];
            constexpr ResourceState kFinal[] = {kRgEntryState, ResourceState::ShaderResource, ResourceState::CopySource};
            r.finalState = kFinal[uniformU32Below(rng, 3)];
        }
        r.defined.assign(r.mips() * r.layers(), 1);
        imports.resources.push_back(std::move(r));
    }
    return imports;
}

/// Records commands that make the Null backend check the graph's states (clears into CopyDest
/// subresources, copies out of CopySource ones, fills of CopyDest buffers) and resolves bindless
/// slots (usage flags of the physical resources).
struct ExecAction {
    enum class Kind : u8 { None, ClearTexture, CopyTexture, FillBuffer, CopyBuffer, SrvTexture, UavTexture, SrvBuffer };
    Kind kind = Kind::None;
    RgTexture tex;
    RgBuffer buf;
    rhi::SubresourceRange range;
};

void buildProgram(RenderGraph& graph, Program& prog, const Imports& imports, u64 seed) {
    Pcg32 rng(seed, 11);
    prog.resources.clear();
    prog.outputs.clear();
    for (const GenResource& imp : imports.resources) {
        GenResource r = imp;
        const std::string name = std::format("Imp{}", prog.resources.size());
        if (r.isTexture) {
            rhi::TextureDesc td = rhi::TextureDesc::tex2D(kTexDescs[r.desc].format, kSize, kSize, kAllColorUsage, {},
                                                          kTexDescs[r.desc].mips);
            td.arrayLayers = kTexDescs[r.desc].layers;
            r.tex = graph.importTexture(name, r.handle, td, {.initialState = r.initial, .finalState = r.finalState});
        } else {
            r.buf = graph.importBuffer(name, r.bufferHandle,
                                       {.size = kBufSizes[r.desc],
                                        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect |
                                                 rhi::BufferUsage::Vertex | rhi::BufferUsage::Index |
                                                 rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferSrc |
                                                 rhi::BufferUsage::TransferDst},
                                       {.initialState = r.initial, .finalState = r.finalState});
        }
        prog.resources.push_back(std::move(r));
    }
    const u32 passCount = 4 + uniformU32Below(rng, 37);
    const rhi::BufferH scratch = prog.scratch;
    for (u32 p = 0; p < passCount; ++p) {
        const u32 roll = uniformU32Below(rng, 100);
        const PassFlags kind = roll < 30 ? PassFlags::Raster
                               : roll < 55 ? PassFlags::Compute
                               : roll < 85 ? PassFlags::AsyncCompute
                                           : PassFlags::Copy;
        const PassFlags flags = kind | (uniformU32Below(rng, 10) == 0 ? PassFlags::NeverCull : PassFlags::None);
        auto actions = std::make_shared<std::vector<ExecAction>>();
        graph.addPass(
            std::format("P{}", p), flags,
            [&](RgBuilder& b) {
                std::set<usize> touched;
                auto newResource = [&](bool isTexture, u32 desc) {
                    GenResource r;
                    r.isTexture = isTexture;
                    r.desc = desc;
                    r.created = true;
                    r.defined.assign(r.mips() * r.layers(), 0);
                    const std::string name = std::format("R{}", prog.resources.size());
                    if (isTexture) {
                        const TexDescChoice& d = kTexDescs[desc];
                        RgTextureDesc td = RgTextureDesc::tex2D(d.format, kSize, kSize, d.mips);
                        td.arrayLayers = d.layers;
                        r.tex = b.create(name, td);
                    } else {
                        r.buf = b.create(name, RgBufferDesc{kBufSizes[desc]});
                    }
                    prog.resources.push_back(std::move(r));
                    return prog.resources.size() - 1;
                };
                auto maybeOutput = [&](const GenResource& r) {
                    if (uniformU32Below(rng, 20) != 0) return;
                    if (r.isTexture) {
                        graph.markOutput(r.tex);
                        prog.outputs.insert({r.tex.id, r.tex.version});
                    } else {
                        graph.markOutput(r.buf);
                        prog.outputs.insert({r.buf.id, r.buf.version});
                    }
                };
                // Raster passes: one color attachment (and maybe depth), all 32x32.
                if (kind == PassFlags::Raster) {
                    std::vector<usize> colors;
                    for (usize i = 0; i < prog.resources.size(); ++i) {
                        if (prog.resources[i].isTexture && prog.resources[i].desc == 0) colors.push_back(i);
                    }
                    usize ci = colors.empty() || uniformU32Below(rng, 3) == 0
                                   ? newResource(true, 0)
                                   : colors[uniformU32Below(rng, static_cast<u32>(colors.size()))];
                    GenResource& c = prog.resources[ci];
                    const bool load = c.defined[0] && uniformU32Below(rng, 2) == 0;
                    c.tex = b.colorAttachment(c.tex, 0, load ? rhi::LoadOp::Load : rhi::LoadOp::Clear);
                    c.defined[0] = 1;
                    touched.insert(ci);
                    maybeOutput(c);
                    if (uniformU32Below(rng, 2) == 0) {
                        std::vector<usize> depths;
                        for (usize i = 0; i < prog.resources.size(); ++i) {
                            if (prog.resources[i].isDepth()) depths.push_back(i);
                        }
                        usize di = depths.empty() || uniformU32Below(rng, 3) == 0
                                       ? newResource(true, 3)
                                       : depths[uniformU32Below(rng, static_cast<u32>(depths.size()))];
                        GenResource& d = prog.resources[di];
                        if (d.defined[0] && uniformU32Below(rng, 3) == 0) {
                            b.depthAttachmentReadOnly(d.tex);
                        } else {
                            const bool dload = d.defined[0] && uniformU32Below(rng, 2) == 0;
                            d.tex = b.depthAttachment(d.tex, dload ? rhi::LoadOp::Load : rhi::LoadOp::Clear);
                            d.defined[0] = 1;
                        }
                        touched.insert(di);
                    }
                }
                const u32 accessCount = 1 + uniformU32Below(rng, 3);
                for (u32 a = 0; a < accessCount; ++a) {
                    const bool write = uniformU32Below(rng, 2) == 0;
                    usize ri = kRgInvalid;
                    if (write && (prog.resources.empty() || uniformU32Below(rng, 3) == 0)) {
                        const bool isTexture = uniformU32Below(rng, 3) != 0;
                        ri = newResource(isTexture, isTexture ? uniformU32Below(rng, 3) : uniformU32Below(rng, 2));
                    } else if (!prog.resources.empty()) {
                        ri = uniformU32Below(rng, static_cast<u32>(prog.resources.size()));
                    }
                    if (ri == kRgInvalid || touched.count(ri)) continue;
                    GenResource& r = prog.resources[ri];
                    if (r.isTexture) {
                        const rhi::SubresourceRange range = randomRange(rng, r.mips(), r.layers());
                        if (r.isDepth()) {
                            // Depth: sampled by raster/compute passes only.
                            if (write || !rangeDefined(r, range) ||
                                !(kind == PassFlags::Raster || kind == PassFlags::Compute)) {
                                continue;
                            }
                            b.read(r.tex, TextureRead::DepthSampled, range);
                            touched.insert(ri);
                            continue;
                        }
                        if (write) {
                            const bool copy = kind == PassFlags::Copy || uniformU32Below(rng, 3) == 0;
                            const bool rmw = !copy && rangeDefined(r, range) && uniformU32Below(rng, 3) == 0;
                            const RgTexture before = r.tex;
                            r.tex = rmw ? b.readWrite(r.tex, TextureWrite::Storage, range)
                                        : b.write(r.tex, copy ? TextureWrite::CopyDest : TextureWrite::Storage, range);
                            (void)before;
                            defineRange(r, range);
                            actions->push_back({copy ? ExecAction::Kind::ClearTexture : ExecAction::Kind::UavTexture,
                                                r.tex, {}, range});
                            maybeOutput(r);
                        } else {
                            if (!rangeDefined(r, range)) continue;
                            u32 how = kind == PassFlags::Copy ? 2 : uniformU32Below(rng, 3);
                            constexpr TextureRead kReads[] = {TextureRead::Sampled, TextureRead::Storage,
                                                              TextureRead::CopySource};
                            b.read(r.tex, kReads[how], range);
                            actions->push_back({how == 2 ? ExecAction::Kind::CopyTexture
                                                         : (how == 0 ? ExecAction::Kind::SrvTexture
                                                                     : ExecAction::Kind::UavTexture),
                                                r.tex, {}, range});
                        }
                    } else {
                        if (write) {
                            const bool copy = kind == PassFlags::Copy || uniformU32Below(rng, 3) == 0;
                            const bool rmw = r.defined[0] && uniformU32Below(rng, 3) == 0;
                            r.buf = rmw ? b.readWrite(r.buf, copy ? BufferWrite::CopyDest : BufferWrite::Shader)
                                        : b.write(r.buf, copy ? BufferWrite::CopyDest : BufferWrite::Shader);
                            r.defined[0] = 1;
                            actions->push_back({copy ? ExecAction::Kind::FillBuffer : ExecAction::Kind::SrvBuffer, {},
                                                r.buf, {}});
                            maybeOutput(r);
                        } else {
                            if (!r.defined[0]) continue;
                            std::vector<BufferRead> options{BufferRead::CopySource};
                            if (kind != PassFlags::Copy) {
                                options.insert(options.end(), {BufferRead::Shader, BufferRead::Indirect, BufferRead::Constant});
                            }
                            if (kind == PassFlags::Raster) options.insert(options.end(), {BufferRead::Vertex, BufferRead::Index});
                            const BufferRead how = options[uniformU32Below(rng, static_cast<u32>(options.size()))];
                            b.read(r.buf, how);
                            actions->push_back({how == BufferRead::CopySource ? ExecAction::Kind::CopyBuffer
                                                                              : ExecAction::Kind::None,
                                                {}, r.buf, {}});
                        }
                    }
                    touched.insert(ri);
                }
            },
            [actions, scratch](RgContext& ctx) {
                rhi::CommandList& cmd = ctx.cmd();
                const bool inRendering = ctx.renderArea().width != 0;
                for (const ExecAction& a : *actions) {
                    switch (a.kind) {
                    case ExecAction::Kind::ClearTexture:
                        if (!inRendering) cmd.clearTexture(ctx.texture(a.tex), {0.5f, 0.25f, 0.0f, 1.0f}, a.range);
                        break;
                    case ExecAction::Kind::CopyTexture:
                        if (!inRendering) {
                            rhi::TextureRegion region;
                            region.mip = a.range.baseMip;
                            region.baseLayer = a.range.baseLayer;
                            cmd.copyTextureToBuffer(ctx.texture(a.tex), region, scratch, {});
                        }
                        break;
                    case ExecAction::Kind::FillBuffer:
                        if (!inRendering) cmd.fillBuffer(ctx.buffer(a.buf), 0, rhi::kWholeSize, 0x1234u);
                        break;
                    case ExecAction::Kind::CopyBuffer:
                        if (!inRendering) cmd.copyBuffer(ctx.buffer(a.buf), 0, scratch, 0, ctx.size(a.buf));
                        break;
                    case ExecAction::Kind::SrvTexture: (void)ctx.srv(a.tex); break;
                    case ExecAction::Kind::UavTexture: (void)ctx.uav(a.tex, a.range.baseMip); break;
                    case ExecAction::Kind::SrvBuffer: (void)ctx.deviceAddress(a.buf); break;
                    case ExecAction::Kind::None: break;
                    }
                }
            });
    }
}

// ---------------------------------------------------------------------------------------------
// Reference checks
// ---------------------------------------------------------------------------------------------
struct Failures {
    std::vector<std::string> list;
    template <class... Args>
    void add(std::format_string<Args...> fmt, Args&&... args) {
        if (list.size() < 20) list.push_back(std::format(fmt, std::forward<Args>(args)...));
    }
};

/// Independent culling reference (versions, side effects, outputs).
void checkCulling(const RgPlan& plan, const std::set<std::pair<u32, u32>>& outputs, Failures& f) {
    std::map<std::pair<u32, u32>, u32> producer;
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        for (const RgAccess& a : plan.passes[p].accesses) {
            if (a.isWrite()) producer[{a.resource, a.writeVersion}] = p;
        }
    }
    std::vector<u8> needed(plan.passes.size(), 0);
    std::vector<u32> stack;
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        bool seed = hasFlag(plan.passes[p].flags, PassFlags::NeverCull);
        for (const RgAccess& a : plan.passes[p].accesses) {
            if (a.isWrite() && (plan.resources[a.resource].imported || outputs.count({a.resource, a.writeVersion}))) seed = true;
        }
        if (seed) {
            needed[p] = 1;
            stack.push_back(p);
        }
    }
    while (!stack.empty()) {
        const u32 p = stack.back();
        stack.pop_back();
        for (const RgAccess& a : plan.passes[p].accesses) {
            if (a.readVersion == kRgInvalid) continue;
            auto it = producer.find({a.resource, a.readVersion});
            if (it != producer.end() && !needed[it->second]) {
                needed[it->second] = 1;
                stack.push_back(it->second);
            }
        }
    }
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        if (plan.passes[p].culled == static_cast<bool>(needed[p])) {
            f.add("pass {} culled={} but reference says needed={}", plan.passes[p].name, plan.passes[p].culled, needed[p]);
        }
    }
}

struct Simulator {
    const RgPlan& plan;
    Failures& f;
    std::vector<u64> value;                      // per batch: timeline value on its queue
    std::vector<std::array<u64, kQueues>> clock;  // per batch: values known complete at its start

    struct Event {
        u32 batch = 0;
        rhi::Queue queue = rhi::Queue::Graphics;
        bool barrier = false;
        bool layoutChange = false;  // barrier that rewrites an image layout (a memory write)
        ResourceState before = ResourceState::Undefined;
        ResourceState state = ResourceState::Undefined;  // access state or barrier 'after'
        bool write = false;
        u32 pass = kRgInvalid;
        u32 resource = kRgInvalid;
        u32 readVersion = kRgInvalid;
        u32 writeVersion = kRgInvalid;
    };
    std::vector<Event> events;
    std::vector<std::vector<std::vector<u32>>> subEvents;  // [physical][subresource] -> events

    bool allowEntries = false;  // compile plans still hold unresolved entry barriers

    Simulator(const RgPlan& p, Failures& failures, bool entries = false) : plan(p), f(failures), allowEntries(entries) {}

    u32 layersOf(u32 phys) const { return plan.physicals[phys].isTexture ? plan.physicals[phys].texture.arrayLayers : 1u; }

    template <class F>
    void forEach(u32 phys, const rhi::SubresourceRange& r, F&& fn) {
        const u32 layers = layersOf(phys);
        for (u32 m = r.baseMip; m < r.baseMip + r.mipCount; ++m) {
            for (u32 l = r.baseLayer; l < r.baseLayer + r.layerCount; ++l) fn(m * layers + l);
        }
    }

    /// Image layout class of a state, as the Vulkan backend maps it (buffers have no layouts).
    static int layoutOf(ResourceState s) {
        switch (s) {
        case ResourceState::Undefined: return 0;
        case ResourceState::CopySource: return 2;
        case ResourceState::CopyDest: return 3;
        case ResourceState::ShaderResource:
        case ResourceState::DepthRead: return 4;
        case ResourceState::RenderTarget:
        case ResourceState::DepthWrite: return 5;
        case ResourceState::Present: return 6;
        default: return 1;  // GENERAL: UnorderedAccess, General, buffer-only states, HostRead
        }
    }

    /// Queue capability a state needs (graphics-only states: attachments, vertex/index, present).
    static u32 levelOf(ResourceState s) {
        switch (s) {
        case ResourceState::RenderTarget: case ResourceState::DepthWrite: case ResourceState::DepthRead:
        case ResourceState::VertexBuffer: case ResourceState::IndexBuffer: case ResourceState::Present: return 2;
        case ResourceState::ShaderResource: case ResourceState::UnorderedAccess: case ResourceState::ConstantBuffer:
        case ResourceState::IndirectArgument: case ResourceState::General: return 1;
        default: return 0;
        }
    }

    void addBarrier(u32 batch, rhi::Queue q, const RgBarrier& b) {
        if (b.before == kRgEntryState && !allowEntries) f.add("unresolved entry barrier in an executed plan");
        const u32 queueLevel = q == rhi::Queue::Graphics ? 2u : (q == rhi::Queue::AsyncCompute ? 1u : 0u);
        if ((b.before != kRgEntryState && levelOf(b.before) > queueLevel) || levelOf(b.after) > queueLevel) {
            f.add("barrier {}->{} recorded on {}", rhi::resourceStateName(b.before), rhi::resourceStateName(b.after),
                  rgQueueName(q));
        }
        forEach(b.physical, b.range, [&](u32 s) {
            Event e;
            e.batch = batch;
            e.queue = q;
            e.barrier = true;
            e.layoutChange = plan.physicals[b.physical].isTexture &&
                             (b.before == kRgEntryState || layoutOf(b.before) != layoutOf(b.after));
            e.before = b.before;
            e.state = b.after;
            subEvents[b.physical][s].push_back(static_cast<u32>(events.size()));
            events.push_back(e);
        });
    }

    void build() {
        // Batch values and vector clocks, recomputed from the waits alone.
        std::array<u64, kQueues> counter{};
        std::array<u32, kQueues> last;
        last.fill(kRgInvalid);
        value.resize(plan.batches.size());
        clock.resize(plan.batches.size());
        for (u32 b = 0; b < plan.batches.size(); ++b) {
            const RgBatchInfo& batch = plan.batches[b];
            const u32 qi = rhi::queueIndex(batch.queue);
            value[b] = ++counter[qi];
            if (batch.queueValue != value[b]) f.add("batch {} queueValue {} expected {}", b, batch.queueValue, value[b]);
            std::array<u64, kQueues> c{};
            if (last[qi] != kRgInvalid) c = clock[last[qi]];
            for (u32 w : batch.waits) {
                if (w >= b) {
                    f.add("batch {} waits for later batch {}", b, w);
                    continue;
                }
                if (plan.batches[w].queue == batch.queue) f.add("batch {} waits for its own queue", b);
                for (u32 k = 0; k < kQueues; ++k) c[k] = std::max(c[k], clock[w][k]);
                c[rhi::queueIndex(plan.batches[w].queue)] = std::max(c[rhi::queueIndex(plan.batches[w].queue)], value[w]);
            }
            clock[b] = c;
            last[qi] = b;
        }
        subEvents.resize(plan.physicals.size());
        for (u32 i = 0; i < plan.physicals.size(); ++i) subEvents[i].resize(plan.physicals[i].subresourceCount());
        for (u32 b = 0; b < plan.batches.size(); ++b) {
            const RgBatchInfo& batch = plan.batches[b];
            for (const RgBarrier& bar : batch.barriers) addBarrier(b, batch.queue, bar);
            for (u32 p : batch.passes) {
                const RgPassInfo& pass = plan.passes[p];
                if (pass.queue != batch.queue) f.add("pass {} on {} in a {} batch", pass.name, rgQueueName(pass.queue),
                                                      rgQueueName(batch.queue));
                for (const RgBarrier& bar : pass.preBarriers) addBarrier(b, batch.queue, bar);
                for (const RgAccess& a : pass.accesses) {
                    const u32 phys = plan.resources[a.resource].physical;
                    forEach(phys, a.range, [&](u32 s) {
                        Event e;
                        e.batch = b;
                        e.queue = batch.queue;
                        e.state = a.state;
                        e.write = a.isWrite();
                        e.pass = p;
                        e.resource = a.resource;
                        e.readVersion = a.readVersion;
                        e.writeVersion = a.writeVersion;
                        subEvents[phys][s].push_back(static_cast<u32>(events.size()));
                        events.push_back(e);
                    });
                }
                for (const RgBarrier& bar : pass.postBarriers) addBarrier(b, batch.queue, bar);
            }
        }
    }

    bool crossHb(const Event& a, const Event& b) const {
        return clock[b.batch][rhi::queueIndex(a.queue)] >= value[a.batch];
    }

    void checkSchedule() {
        std::vector<u32> seen(plan.passes.size(), 0);
        std::array<u32, kQueues> lastDecl;
        lastDecl.fill(kRgInvalid);
        for (u32 b = 0; b < plan.batches.size(); ++b) {
            const RgBatchInfo& batch = plan.batches[b];
            u32 covered = 0;
            for (u32 li = batch.firstList; li < batch.firstList + batch.listCount; ++li) {
                const RgListInfo& list = plan.lists.at(li);
                if (list.batch != b || list.firstPass != covered) f.add("list {} does not tile batch {}", li, b);
                covered += list.passCount;
                for (u32 k = 0; k < list.passCount; ++k) {
                    if (plan.passes[batch.passes[list.firstPass + k]].list != li) f.add("pass list index mismatch");
                }
            }
            if (covered != batch.passes.size()) f.add("lists of batch {} cover {} of {} passes", b, covered, batch.passes.size());
            for (u32 p : batch.passes) {
                ++seen[p];
                if (plan.passes[p].batch != b) f.add("pass {} batch index mismatch", plan.passes[p].name);
                u32& ld = lastDecl[rhi::queueIndex(batch.queue)];
                if (ld != kRgInvalid && ld > p) f.add("pass {} out of declaration order on its queue", plan.passes[p].name);
                ld = p;
            }
        }
        for (u32 p = 0; p < plan.passes.size(); ++p) {
            if (seen[p] != (plan.passes[p].culled ? 0u : 1u)) f.add("pass {} scheduled {} times", plan.passes[p].name, seen[p]);
        }
    }

    /// Conflicting operations on one physical subresource must be ordered by happens-before.
    void checkHazards() {
        for (u32 phys = 0; phys < subEvents.size(); ++phys) {
            for (u32 s = 0; s < subEvents[phys].size(); ++s) {
                const std::vector<u32>& list = subEvents[phys][s];
                for (usize i = 0; i < list.size(); ++i) {
                    const Event& a = events[list[i]];
                    for (usize j = i + 1; j < list.size(); ++j) {
                        const Event& b = events[list[j]];
                        // Layout transitions rewrite memory; other barriers only order their queue.
                        const bool conflict = (a.barrier && a.layoutChange) || (b.barrier && b.layoutChange) ||
                                              (!a.barrier && !b.barrier && (a.write || b.write || a.state != b.state));
                        if (!conflict) continue;
                        bool ordered = false;
                        if (a.queue != b.queue) {
                            ordered = crossHb(a, b);
                        } else if (a.barrier || b.barrier) {
                            ordered = true;
                        } else {
                            for (usize k = i + 1; k < j && !ordered; ++k) {
                                const Event& m = events[list[k]];
                                ordered = m.barrier && m.queue == a.queue;
                            }
                            ordered = ordered || crossHb(a, b);
                        }
                        if (!ordered) {
                            f.add("race on physical {} sub {}: {} ({}) and {} ({}) on {}/{}", phys, s,
                                  a.barrier ? std::string("barrier") : plan.passes[a.pass].name,
                                  rhi::resourceStateName(a.state), b.barrier ? std::string("barrier") : plan.passes[b.pass].name,
                                  rhi::resourceStateName(b.state), rgQueueName(a.queue), rgQueueName(b.queue));
                        }
                    }
                }
            }
        }
    }

    /// Barrier sources match the simulated state; accesses happen in their declared state.
    void checkStates() {
        for (u32 phys = 0; phys < subEvents.size(); ++phys) {
            for (u32 s = 0; s < subEvents[phys].size(); ++s) {
                std::optional<ResourceState> cur;
                for (u32 ei : subEvents[phys][s]) {
                    const Event& e = events[ei];
                    if (e.barrier) {
                        if (cur && e.before != *cur) {
                            f.add("physical {} sub {}: barrier from {} but state is {}", phys, s,
                                  rhi::resourceStateName(e.before), rhi::resourceStateName(*cur));
                        }
                        // A read-only X->X barrier as first event can only be an entry the resolver should drop.
                        if (e.before == e.state && rgIsReadOnlyStateForTest(e.before) && !cur) {
                            f.add("physical {} sub {}: useless read-only entry barrier", phys, s);
                        }
                        cur = e.state;
                    } else {
                        if (cur && e.state != *cur) {
                            f.add("physical {} sub {}: pass {} uses it as {} but it is in {}", phys, s,
                                  plan.passes[e.pass].name, rhi::resourceStateName(e.state), rhi::resourceStateName(*cur));
                        }
                        cur = e.state;
                    }
                }
                if (cur && plan.physicals[phys].finalStates[s] != *cur) {
                    f.add("physical {} sub {}: final state {} but plan says {}", phys, s, rhi::resourceStateName(*cur),
                          rhi::resourceStateName(plan.physicals[phys].finalStates[s]));
                }
            }
        }
    }

    static bool rgIsReadOnlyStateForTest(ResourceState s) {
        return s == ResourceState::ShaderResource || s == ResourceState::CopySource || s == ResourceState::DepthRead ||
               s == ResourceState::IndirectArgument || s == ResourceState::VertexBuffer ||
               s == ResourceState::IndexBuffer || s == ResourceState::ConstantBuffer || s == ResourceState::Present;
    }

    /// Every read sees the version it names: content tags per physical subresource.
    void checkContents() {
        // Expected writer version of (resource, version, subresource) from the declarations.
        std::map<std::pair<u32, u32>, const RgAccess*> writerOf;  // (resource, version) -> write access
        for (u32 p : plan.order) {
            for (const RgAccess& a : plan.passes[p].accesses) {
                if (a.isWrite()) writerOf[{a.resource, a.writeVersion}] = &a;
            }
        }
        auto expected = [&](u32 r, u32 v, u32 mip, u32 layer) -> std::optional<u32> {
            for (u32 k = v; k >= 1; --k) {
                auto it = writerOf.find({r, k});
                if (it == writerOf.end()) return std::nullopt;  // produced by a culled pass: not checkable
                const rhi::SubresourceRange& w = it->second->range;
                if (mip >= w.baseMip && mip < w.baseMip + w.mipCount && layer >= w.baseLayer &&
                    layer < w.baseLayer + w.layerCount) {
                    return k;
                }
            }
            if (plan.resources[r].imported) return 0u;
            return std::nullopt;
        };
        for (u32 phys = 0; phys < subEvents.size(); ++phys) {
            const u32 layers = layersOf(phys);
            for (u32 s = 0; s < subEvents[phys].size(); ++s) {
                const u32 mip = s / layers;
                const u32 layer = s % layers;
                std::optional<std::pair<u32, u32>> tag;
                const RgPhysicalInfo& pi = plan.physicals[phys];
                if (pi.imported) tag = std::pair<u32, u32>{pi.residents.front(), 0u};
                for (u32 ei : subEvents[phys][s]) {
                    const Event& e = events[ei];
                    if (e.barrier) continue;
                    if (e.readVersion != kRgInvalid) {
                        const std::optional<u32> want = expected(e.resource, e.readVersion, mip, layer);
                        if (want && (!tag || tag->first != e.resource || tag->second != *want)) {
                            f.add("pass {} reads {}@{} (sub {}) but memory holds {}", plan.passes[e.pass].name,
                                  plan.resources[e.resource].name, *want, s,
                                  tag ? std::format("{}@{}", plan.resources[tag->first].name, tag->second)
                                      : std::string("nothing"));
                        }
                    }
                    if (e.write) tag = std::pair<u32, u32>{e.resource, e.writeVersion};
                }
            }
        }
    }
};

/// Placement plan: memory overlap only between resources whose accesses are ordered.
void checkPlacement(const RgPlan& plan, Failures& f) {
    std::vector<u64> value(plan.batches.size());
    std::vector<std::array<u64, kQueues>> clock(plan.batches.size());
    std::array<u64, kQueues> counter{};
    std::array<u32, kQueues> last;
    last.fill(kRgInvalid);
    for (u32 b = 0; b < plan.batches.size(); ++b) {
        const u32 qi = rhi::queueIndex(plan.batches[b].queue);
        value[b] = ++counter[qi];
        std::array<u64, kQueues> c{};
        if (last[qi] != kRgInvalid) c = clock[last[qi]];
        for (u32 w : plan.batches[b].waits) {
            for (u32 k = 0; k < kQueues; ++k) c[k] = std::max(c[k], clock[w][k]);
            c[rhi::queueIndex(plan.batches[w].queue)] = std::max(c[rhi::queueIndex(plan.batches[w].queue)], value[w]);
        }
        clock[b] = c;
        last[qi] = b;
    }
    std::vector<std::vector<u32>> users(plan.resources.size());
    for (u32 p : plan.order) {
        for (const RgAccess& a : plan.passes[p].accesses) users[a.resource].push_back(p);
    }
    auto hbPass = [&](u32 a, u32 b) {
        const RgPassInfo& pa = plan.passes[a];
        const RgPassInfo& pb = plan.passes[b];
        if (pa.position >= pb.position) return false;
        if (pa.queue == pb.queue) return true;
        return clock[pb.batch][rhi::queueIndex(pa.queue)] >= value[pa.batch];
    };
    auto allHb = [&](u32 x, u32 y) {
        for (u32 a : users[x]) {
            for (u32 b : users[y]) {
                if (!hbPass(a, b)) return false;
            }
        }
        return true;
    };
    for (u32 x = 0; x < plan.resources.size(); ++x) {
        const RgResourceInfo& rx = plan.resources[x];
        if (!rx.used || rx.imported) continue;
        for (u32 y = x + 1; y < plan.resources.size(); ++y) {
            const RgResourceInfo& ry = plan.resources[y];
            if (!ry.used || ry.imported) continue;
            const bool orderedLives = allHb(x, y) || allHb(y, x);
            if (rx.heap == ry.heap && rx.heapOffset < ry.heapOffset + ry.bytes && ry.heapOffset < rx.heapOffset + rx.bytes &&
                !orderedLives) {
                f.add("placement overlaps {} and {} with concurrent lifetimes", rx.name, ry.name);
            }
            if (rx.physical == ry.physical && !orderedLives) f.add("pooled {} and {} share memory concurrently", rx.name, ry.name);
        }
    }
}

std::string joined(const Failures& f) {
    std::string out;
    for (const std::string& s : f.list) out += "\n  " + s;
    return out;
}

/// Puts every imported resource into its program initial state (Null tracks states per subresource).
void resetImports(rhi::Device& device, rhi::NullDevice& null, const Imports& imports) {
    rhi::CommandList* cmd = device.acquireCommandList(rhi::Queue::Graphics, "ResetImports");
    for (const GenResource& r : imports.resources) {
        if (r.isTexture) {
            for (u32 m = 0; m < r.mips(); ++m) {
                for (u32 l = 0; l < r.layers(); ++l) {
                    const ResourceState cur = null.textureState(r.handle, m, l);
                    if (cur != r.initial) cmd->barrier(rhi::Barrier::textureState(r.handle, cur, r.initial, {m, 1, l, 1}));
                }
            }
        } else {
            cmd->barrier(rhi::Barrier::bufferState(r.bufferHandle, ResourceState::Undefined, r.initial));
        }
    }
    REQUIRE(device.submit(rhi::Queue::Graphics, {&cmd, 1}).ok());
}

} // namespace

TEST_CASE("graph stress: random DAGs match the reference simulator and run clean on Null") {
    NullDeviceFixture fx;
    rhi::Device& device = *fx.device;
    jobs::JobSystem jobSystem(jobs::JobSystemDesc{.workerCount = 3});
    const u64 kSeeds = 300;
    u64 aliasedPhysicals = 0;
    u64 prologues = 0;
    u64 epilogues = 0;
    u64 waits = 0;
    u64 culled = 0;
    for (u64 seed = 1; seed <= kSeeds; ++seed) {
        CAPTURE(seed);
        Pcg32 optRng(seed, 3);
        const RgCompileOptions options{.cull = uniformU32Below(optRng, 4) != 0,
                                       .alias = uniformU32Below(optRng, 4) != 0,
                                       .asyncCompute = uniformU32Below(optRng, 4) != 0,
                                       .maxCommandLists = 1 + uniformU32Below(optRng, 12)};
        const bool parallel = uniformU32Below(optRng, 2) == 0;
        Imports imports = makeImports(device, seed);
        Program prog;
        prog.scratch = device.createBuffer({.size = 16384, .usage = rhi::BufferUsage::TransferDst,
                                            .memory = rhi::MemoryUsage::Readback, .name = "Scratch"})
                           .value();
        RgResourcePool pool(device);
        for (int frame = 0; frame < 2; ++frame) {
            CAPTURE(frame);
            REQUIRE(device.beginFrame().ok());
            resetImports(device, *fx.null, imports);
            RenderGraph graph(std::format("Seed{}", seed));
            buildProgram(graph, prog, imports, seed);
            INFO((graph.errors().empty() ? std::string() : graph.errors().front()));
            REQUIRE(graph.errors().empty());
            REQUIRE(graph.compile(options).ok());
            const RgPlan& plan = graph.plan();

            Failures failures;
            if (options.cull) {
                checkCulling(plan, prog.outputs, failures);
            } else {
                CHECK(plan.stats.culledPassCount == 0);
            }
            checkPlacement(plan, failures);
            {
                Simulator compiled(plan, failures, true);
                compiled.build();
                compiled.checkSchedule();
            }
            auto result = graph.execute(device, pool, {.jobs = parallel ? &jobSystem : nullptr});
            REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().toString()));
            const RgPlan& executed = graph.executedPlan();
            Simulator sim(executed, failures);
            sim.build();
            sim.checkSchedule();
            sim.checkHazards();
            sim.checkStates();
            sim.checkContents();
            INFO("failures:" << joined(failures) << "\nplan:\n" << rgDumpPlan(executed));
            CHECK(failures.list.empty());
            {
                INFO(fx.errors() << "\nplan:\n" << rgDumpPlan(executed));
                CHECK(fx.errorCount() == 0);
            }
            fx.null->clearValidationErrors();
            CHECK(graph.contextErrorCount() == 0);
            // Imported resources end in their final states.
            for (const GenResource& r : imports.resources) {
                if (!r.isTexture || r.finalState == kRgEntryState) continue;
                for (u32 m = 0; m < r.mips(); ++m) {
                    for (u32 l = 0; l < r.layers(); ++l) CHECK(fx.null->textureState(r.handle, m, l) == r.finalState);
                }
            }
            for (const RgPhysicalInfo& p : plan.physicals) aliasedPhysicals += p.residents.size() > 1 ? 1 : 0;
            for (const RgBatchInfo& b : executed.batches) {
                prologues += b.kind == RgBatchInfo::Kind::Prologue ? 1 : 0;
                epilogues += b.kind == RgBatchInfo::Kind::Epilogue ? 1 : 0;
                waits += b.waits.size();
            }
            culled += plan.stats.culledPassCount;
            fx.null->clearTrace();
        }
        REQUIRE(device.waitIdle().ok());
        for (const GenResource& r : imports.resources) {
            if (r.isTexture) {
                device.destroy(r.handle);
            } else {
                device.destroy(r.bufferHandle);
            }
        }
        device.destroy(prog.scratch);
    }
    MESSAGE("stress: " << kSeeds << " seeds x 2 frames; aliased physicals " << aliasedPhysicals << ", prologues "
                       << prologues << ", epilogues " << epilogues << ", cross-queue waits " << waits << ", culled passes "
                       << culled);
    // The generator must actually exercise the interesting paths.
    CHECK(aliasedPhysicals > 50);
    CHECK(prologues > 0);
    CHECK(epilogues > 0);
    CHECK(waits > 100);
    CHECK(culled > 50);
}
