#pragma once
// Render graph v0 (docs/plan/03-rendering.md §2, ADR-003): Frostbite FrameGraph shape — *setup*
// declares passes and the resources they read, write and create; *compile* plans the frame; *execute*
// records and submits it.
//
//   RenderGraph graph("Frame");
//   RgTexture backbuffer = graph.importTexture("Backbuffer", image.texture, device.textureDesc(image.texture),
//                                              {.finalState = rhi::ResourceState::Present, .waitFor = image.ready});
//   struct GeoData { RgTexture color, depth; };
//   const GeoData& geo = graph.addPass<GeoData>("Geometry", PassFlags::Raster,
//       [&](RgBuilder& b, GeoData& d) {
//           d.color = b.colorAttachment(b.create("SceneColor", RgTextureDesc::tex2D(Format::RGBA16Float, w, h)), 0);
//           d.depth = b.depthAttachment(b.create("SceneDepth", RgTextureDesc::tex2D(Format::D32Float, w, h)));
//       },
//       [=](const GeoData&, RgContext& ctx) { ctx.cmd().draw(3); });
//   ... more passes reading geo.color ...
//   graph.compile().value();
//   RgExecuteResult done = graph.execute(device, pool, {.jobs = &jobSystem}).value();
//   device.present(swapchain, done.graphics());
//
// Compile (03 §2.2):
//   1. **Culling** — reference flood from side-effect passes (NeverCull, writes to imported or
//      output-marked resources) through the *versions* they read; everything else costs nothing.
//   2. **Ordering / queues** — declaration order; AsyncCompute passes go to the async-compute
//      queue (serialized on graphics with RgCompileOptions::asyncCompute = false — the graph is
//      correct either way). Passes are grouped into submission *batches*; a batch is split only
//      where another queue has to wait for part of it, and every cross-queue hazard (RAW, WAR, WAW,
//      layout changes) becomes one timeline wait, with waits already implied by others dropped
//      (vector clocks).
//   3. **Aliasing** — transient resources whose lifetimes are ordered by the schedule's
//      happens-before relation (queue order + waits, so concurrent async work never shares memory)
//      share one pooled RHI resource when their descriptions match; a placement plan (greedy
//      interval packing per heap class and 64 KiB alignment) reports what placed-resource aliasing
//      would save (the RHI has no placed resources yet).
//   4. **Barriers** — per-subresource state tracking over physical resources in submission order:
//      layout/state transitions, RAW/WAW/WAR barriers on the same queue, discard-free reuse of
//      aliased memory, cross-queue transitions on the queue that supports both states (a
//      graphics-only state handed to async compute is released on the graphics queue), and final
//      transitions of imported resources. One barrier batch per pass boundary.
//   5. **Partition** — each batch is split into command lists (RgCompileOptions::maxCommandLists
//      in total, 4–12 per frame on REF) that are recorded in parallel on the job system.
//
// The first touch of each physical resource is an *entry* barrier (before = kRgEntryState): its
// source state is only known at execute time (the pool's state from the previous frame, or the
// import's initial state) and is resolved by resolveEntryBarriers(), which may move it into a
// graphics *prologue* batch when the consuming queue cannot perform the transition.
//
// Threading: a RenderGraph is built, compiled and executed by one thread. During execute() the
// pass callbacks run concurrently on job-system workers (one command list per job), so they must
// only touch their own data and the RgContext they are given. RgResourcePool is single-threaded
// (used by the thread that calls execute()).
//
// Budget: compile of a 200-pass graph ≤ 2 ms on REF (graph cache of Phase 2 brings the unchanged
// case to ≤ 0.3 ms, 03 §2.2); measured by render_tests ("graph: compile budget").

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/rhi/device.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::render {

inline constexpr u32 kRgInvalid = 0xFFFFFFFFu;
/// Most array layers a transient texture may have (the D3D12 limit; Vulkan guarantees 256).
inline constexpr u32 kRgMaxArrayLayers = 2048;
/// RgBarrier::before of an entry barrier (first touch of a physical resource; resolved at execute).
inline constexpr rhi::ResourceState kRgEntryState = rhi::ResourceState::Count;

/// Virtual texture: a graph resource id plus the *version* written by one pass. Every write
/// produces a new version, so a read names exactly the data it consumes.
struct RgTexture {
    u32 id = kRgInvalid;
    u32 version = 0;
    constexpr bool isValid() const noexcept { return id != kRgInvalid; }
    friend constexpr bool operator==(const RgTexture&, const RgTexture&) = default;
};

/// Virtual buffer (see RgTexture).
struct RgBuffer {
    u32 id = kRgInvalid;
    u32 version = 0;
    constexpr bool isValid() const noexcept { return id != kRgInvalid; }
    friend constexpr bool operator==(const RgBuffer&, const RgBuffer&) = default;
};

/// Transient texture description. Usage flags are derived from the declared accesses. create()
/// rejects empty extents, unknown formats, more mips than the extent allows, more than
/// kRgMaxArrayLayers layers and sample counts that are not a power of two <= 64.
struct RgTextureDesc {
    rhi::TextureType type = rhi::TextureType::Tex2D;
    rhi::Format format = rhi::Format::RGBA8Unorm;
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    u32 sampleCount = 1;

    static RgTextureDesc tex2D(rhi::Format format, u32 width, u32 height, u32 mipLevels = 1) noexcept {
        RgTextureDesc d;
        d.format = format;
        d.width = width;
        d.height = height;
        d.mipLevels = mipLevels;
        return d;
    }
    friend bool operator==(const RgTextureDesc&, const RgTextureDesc&) = default;
};

/// Transient buffer description (GPU-only memory; usage derived from the declared accesses).
struct RgBufferDesc {
    u64 size = 0;
    friend bool operator==(const RgBufferDesc&, const RgBufferDesc&) = default;
};

enum class PassFlags : u32 {
    None = 0,
    /// Graphics queue. The graph opens a rendering scope over the declared attachments around the
    /// execute callback (beginRendering / endRendering).
    Raster = 1u << 0,
    /// Compute on the graphics queue.
    Compute = 1u << 1,
    /// Compute on the async-compute queue (graphics when RgCompileOptions::asyncCompute is false).
    AsyncCompute = 1u << 2,
    /// Copies, fills and clears (graphics queue).
    Copy = 1u << 3,
    /// Never culled: the pass has effects the graph cannot see (CPU readback, queries, ...).
    NeverCull = 1u << 4,
};
HELIOS_ENUM_FLAGS(PassFlags)

/// How a pass reads a texture.
enum class TextureRead : u8 {
    Sampled,      ///< ShaderResource (bindless srv).
    Storage,      ///< Storage-image load (UnorderedAccess state, no write).
    CopySource,   ///< Copy source.
    DepthSampled, ///< Depth texture sampled in a shader (DepthRead).
};
/// How a pass writes a texture (attachments have their own builder calls).
enum class TextureWrite : u8 {
    Storage,  ///< Storage image (UnorderedAccess).
    CopyDest, ///< Copy / clearTexture destination.
};
/// How a pass reads a buffer.
enum class BufferRead : u8 { Shader, Indirect, Vertex, Index, Constant, CopySource };
/// How a pass writes a buffer.
enum class BufferWrite : u8 { Shader, CopyDest };

/// State of an imported (external) resource: histories, the GPU scene, swapchain images.
struct RgImport {
    /// State of every subresource when the graph starts.
    rhi::ResourceState initialState = rhi::ResourceState::Undefined;
    /// State to leave the resource in (e.g. Present); kRgEntryState (Count) = its last used state.
    rhi::ResourceState finalState = kRgEntryState;
    /// Waited for by the first submission on each queue that touches the resource (e.g.
    /// SwapchainImage::ready, an upload).
    rhi::TimelinePoint waitFor;
};

struct RgCompileOptions {
    bool cull = true;
    /// Share pooled resources between transients whose lifetimes do not overlap.
    bool alias = true;
    /// false: AsyncCompute passes run on the graphics queue.
    bool asyncCompute = true;
    /// Command lists per frame for parallel recording (at least one per batch).
    u32 maxCommandLists = 8;
    /// Breadcrumb (pass index, Begin/End) around every pass for GPU-crash reports (03 §1.5).
    bool breadcrumbs = false;
    /// Debug label per pass (RenderDoc, Nsight, Null traces).
    bool labels = true;
};

// ---------------------------------------------------------------------------------------------
// Compiled plan (read-only; exposed for tests, dumps and the visualizer)
// ---------------------------------------------------------------------------------------------

/// State transition of one physical resource (textures: an explicit subresource range; buffers:
/// {0, 1, 0, 1}). before == kRgEntryState marks an unresolved entry barrier.
struct RgBarrier {
    u32 physical = kRgInvalid;
    rhi::ResourceState before = rhi::ResourceState::Undefined;
    rhi::ResourceState after = rhi::ResourceState::Undefined;
    rhi::SubresourceRange range{0, 1, 0, 1};
    friend bool operator==(const RgBarrier&, const RgBarrier&) = default;
};

/// One declared access of a pass (attachments included).
struct RgAccess {
    u32 resource = kRgInvalid;
    /// Version whose contents the access depends on (reads, read-modify-writes, partial writes and
    /// attachments with LoadOp::Load); kRgInvalid for plain overwrites.
    u32 readVersion = kRgInvalid;
    /// Version the access produces; kRgInvalid for reads.
    u32 writeVersion = kRgInvalid;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    rhi::SubresourceRange range{0, 1, 0, 1};  ///< Explicit counts (never kAllMips/kAllLayers).
    bool isWrite() const noexcept { return writeVersion != kRgInvalid; }
};

struct RgPassInfo {
    std::string name;
    PassFlags flags = PassFlags::None;
    rhi::Queue queue = rhi::Queue::Graphics;
    bool culled = false;
    u32 position = kRgInvalid;  ///< Index in RgPlan::order (submission order); kRgInvalid if culled.
    u32 batch = kRgInvalid;
    u32 list = kRgInvalid;
    std::vector<RgAccess> accesses;
    std::vector<RgBarrier> preBarriers;   ///< Recorded before the pass (one barrier batch).
    std::vector<RgBarrier> postBarriers;  ///< Recorded after the pass (releases to another queue, final states).
    /// Store ops of the attachments (DontCare when the written version is never read again).
    std::vector<rhi::StoreOp> colorStore;
    rhi::StoreOp depthStore = rhi::StoreOp::Store;
};

/// D3D12 resource-heap tier-1 classes used by the placement plan.
enum class RgHeapKind : u8 { Buffers, RenderTargets, Textures };
inline constexpr u32 kRgHeapKindCount = 3;

struct RgResourceInfo {
    std::string name;
    bool isTexture = true;
    bool imported = false;
    RgTextureDesc texture;                                  ///< Textures.
    u64 bufferSize = 0;                                     ///< Buffers.
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;  ///< Union of the declared accesses.
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
    u32 versionCount = 1;
    bool used = false;               ///< Accessed by a non-culled pass.
    u32 firstPosition = kRgInvalid;  ///< First/last access in submission order (non-culled passes).
    u32 lastPosition = kRgInvalid;
    u32 physical = kRgInvalid;       ///< Backing physical resource (used resources only).
    u64 bytes = 0;                   ///< Estimated memory (transients: aligned to 64 KiB).
    RgHeapKind heap = RgHeapKind::Buffers;  ///< Placement plan (used transients).
    u64 heapOffset = 0;
    RgImport import;
};

struct RgPhysicalInfo {
    bool isTexture = true;
    bool imported = false;
    std::vector<u32> residents;  ///< Virtual resources backed by this resource, in first-use order.
    RgTextureDesc texture;
    u64 bufferSize = 0;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;  ///< Creation usage (union of residents).
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
    u64 bytes = 0;
    /// State of every subresource (index mip * arrayLayers + layer) after the graph;
    /// kRgEntryState = never touched.
    std::vector<rhi::ResourceState> finalStates;
    u32 subresourceCount() const noexcept { return isTexture ? texture.mipLevels * texture.arrayLayers : 1u; }
};

struct RgBatchInfo {
    enum class Kind : u8 { Passes, Prologue, Epilogue };
    Kind kind = Kind::Passes;
    rhi::Queue queue = rhi::Queue::Graphics;
    std::vector<u32> passes;          ///< Pass indices in recording order.
    std::vector<u32> waits;           ///< Earlier batches (other queues) this batch waits for.
    std::vector<RgBarrier> barriers;  ///< Prologue/Epilogue batches only.
    u32 firstList = 0;
    u32 listCount = 0;
    /// Timeline value of this batch among the graph's submissions on its queue (1-based).
    u32 queueValue = 0;
};

struct RgListInfo {
    u32 batch = 0;
    u32 firstPass = 0;  ///< Index into RgBatchInfo::passes.
    u32 passCount = 0;
    std::string name;
};

struct RgCompileStats {
    u32 passCount = 0;
    u32 culledPassCount = 0;
    u32 batchCount = 0;
    u32 commandListCount = 0;
    u32 barrierCount = 0;
    u32 crossQueueWaitCount = 0;
    u32 physicalTextureCount = 0;  ///< Transient textures actually allocated (after aliasing).
    u32 physicalBufferCount = 0;
    u64 transientBytes = 0;  ///< Sum over used transients (no aliasing).
    u64 pooledBytes = 0;     ///< Pooled physical resources (same-description aliasing, realized).
    u64 placedBytes = 0;     ///< Placement plan: sum of heap sizes.
    std::array<u64, kRgHeapKindCount> heapBytes{};
    f64 compileMs = 0.0;     ///< Not part of dumps (non-deterministic).
};

struct RgPlan {
    std::string name;                       ///< RenderGraph::name().
    std::vector<RgPassInfo> passes;         ///< Declaration order.
    std::vector<RgResourceInfo> resources;  ///< Declaration order.
    std::vector<RgPhysicalInfo> physicals;
    std::vector<RgBatchInfo> batches;       ///< Submission order.
    std::vector<RgListInfo> lists;          ///< Submission order.
    std::vector<u32> order;                 ///< Non-culled passes in submission order.
    RgCompileStats stats;
};

/// Resolves the entry barriers of `plan` (see the header comment) for the given current states of
/// the physical resources (one vector per physical, one state per subresource). Entry barriers
/// whose source equals their read-only destination are dropped; transitions the consuming queue
/// cannot perform (a graphics-only source state on async compute) move into a graphics prologue
/// batch inserted at index 0, which the consumers then wait for. Pure function (tests use it
/// directly; RenderGraph::execute uses it with the pool/import states).
RgPlan resolveEntryBarriers(const RgPlan& plan, std::span<const std::vector<rhi::ResourceState>> physicalStates);

/// Queue capability level a state needs: 0 = transfer, 1 = compute, 2 = graphics only.
u32 rgStateLevel(rhi::ResourceState state) noexcept;
/// Capability level of a queue (Transfer 0, AsyncCompute 1, Graphics 2).
u32 rgQueueLevel(rhi::Queue queue) noexcept;

// ---------------------------------------------------------------------------------------------
// Setup / execute interfaces
// ---------------------------------------------------------------------------------------------
class RenderGraph;
class RgResourcePool;

/// Declares a pass's resources during setup. Misuse (stale versions, conflicting accesses, wrong
/// queue, missing usage on an imported resource) is recorded and fails compile(); the builder then
/// returns invalid handles.
class RgBuilder {
public:
    RgTexture create(std::string_view name, const RgTextureDesc& desc);
    RgBuffer create(std::string_view name, const RgBufferDesc& desc);

    /// Reads `texture` (the version named by the handle). Returns the same handle.
    RgTexture read(RgTexture texture, TextureRead access = TextureRead::Sampled, const rhi::SubresourceRange& range = {});
    RgBuffer read(RgBuffer buffer, BufferRead access = BufferRead::Shader);
    /// Overwrites the (sub)resource; the previous contents are not needed (a partial range keeps
    /// the other subresources, so it depends on the previous version). Returns the new version.
    RgTexture write(RgTexture texture, TextureWrite access = TextureWrite::Storage,
                    const rhi::SubresourceRange& range = {});
    RgBuffer write(RgBuffer buffer, BufferWrite access = BufferWrite::Shader);
    /// Read-modify-write: depends on the previous version. Returns the new version.
    RgTexture readWrite(RgTexture texture, TextureWrite access = TextureWrite::Storage,
                        const rhi::SubresourceRange& range = {});
    RgBuffer readWrite(RgBuffer buffer, BufferWrite access = BufferWrite::Shader);

    /// Color attachment `slot` (0..7, contiguous) of a Raster pass. LoadOp::Load depends on the
    /// previous version. Returns the new version.
    RgTexture colorAttachment(RgTexture texture, u32 slot, rhi::LoadOp load = rhi::LoadOp::Clear,
                              const std::array<f32, 4>& clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, u32 mip = 0,
                              u32 layer = 0);
    /// Depth attachment (DepthWrite; reverse-Z clears to 0). Returns the new version.
    RgTexture depthAttachment(RgTexture texture, rhi::LoadOp load = rhi::LoadOp::Clear, f32 clearDepth = 0.0f,
                              u32 mip = 0, u32 layer = 0);
    /// Read-only depth attachment (DepthRead: depth test, no writes). Returns the same handle.
    RgTexture depthAttachmentReadOnly(RgTexture texture, u32 mip = 0, u32 layer = 0);

    /// Same as PassFlags::NeverCull.
    void neverCull();

    /// Description of a declared resource (for sizing derived resources).
    const RgTextureDesc& desc(RgTexture texture) const;
    u64 size(RgBuffer buffer) const;

private:
    friend class RenderGraph;
    RgBuilder(RenderGraph& graph, u32 pass) noexcept : m_graph(graph), m_pass(pass) {}
    RenderGraph& m_graph;
    u32 m_pass;
};

/// What a pass callback records with. Valid only during the callback, on the recording thread.
/// Resolving a resource the pass did not declare is a graph validation error (counted by
/// RenderGraph::contextErrorCount and logged) — the "declared accesses" check of 03 §1.7.
class RgContext {
public:
    rhi::CommandList& cmd() const noexcept { return *m_cmd; }
    rhi::Device& device() const noexcept { return *m_device; }
    std::string_view passName() const noexcept;
    u32 passIndex() const noexcept { return m_pass; }

    rhi::TextureH texture(RgTexture texture) const;
    rhi::BufferH buffer(RgBuffer buffer) const;
    /// Bindless slots / addresses of the physical resource behind a declared resource. For
    /// transients, the whole-resource srv(), every uav(mip) and the one-mip views
    /// {.baseMip = m, .mipCount = 1} get their slots when the graph creates the texture; for
    /// imported textures, execute() creates the uav(mip) of declared Storage accesses and the
    /// one-mip views of declared sampled reads before recording, in plan order. Slot numbers
    /// therefore do not depend on parallel recording; other sub-views are allocated on first
    /// request, in recording order.
    rhi::BindlessIndex srv(RgTexture texture, const rhi::ViewDesc& view = {}) const;
    rhi::BindlessIndex uav(RgTexture texture, u32 mip = 0) const;
    rhi::BindlessIndex srv(RgBuffer buffer) const;
    u64 deviceAddress(RgBuffer buffer) const;
    const RgTextureDesc& desc(RgTexture texture) const;
    u64 size(RgBuffer buffer) const;
    /// Full extent of a Raster pass's attachments (zero for other passes).
    rhi::Rect renderArea() const noexcept { return m_area; }

private:
    friend class RenderGraph;
    RgContext() = default;
    bool declared(u32 resource) const;
    const RenderGraph* m_graph = nullptr;
    rhi::Device* m_device = nullptr;
    rhi::CommandList* m_cmd = nullptr;
    const std::vector<rhi::TextureH>* m_textures = nullptr;  // per physical
    const std::vector<rhi::BufferH>* m_buffers = nullptr;
    u32 m_pass = 0;
    rhi::Rect m_area;
};

struct RgExecuteOptions {
    /// Records command lists in parallel on this job system (null: on the calling thread).
    jobs::JobSystem* jobs = nullptr;
    /// Extra points the graph's first submission on each queue waits for (uploads, ...).
    std::span<const rhi::TimelinePoint> waits;
};

struct RgExecuteResult {
    /// Last submission per queue (null point when the queue was not used).
    std::array<rhi::TimelinePoint, rhi::kQueueCount> lastPoints{};
    u32 submitCount = 0;
    u32 commandListCount = 0;
    f64 recordMs = 0.0;  ///< Wall time of (parallel) command recording.
    /// Graphics queue's last point (present after it).
    rhi::TimelinePoint graphics() const noexcept { return lastPoints[rhi::queueIndex(rhi::Queue::Graphics)]; }
};

/// The frame graph. See the header comment.
class RenderGraph {
public:
    explicit RenderGraph(std::string_view name = "Frame");
    ~RenderGraph();
    RenderGraph(RenderGraph&&) noexcept;
    RenderGraph& operator=(RenderGraph&&) noexcept;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    std::string_view name() const noexcept;

    /// External resources (histories, GPU scene, swapchain images). `desc` supplies the format,
    /// extent and usage (Device::textureDesc / bufferDesc of the handle).
    RgTexture importTexture(std::string_view name, rhi::TextureH texture, const rhi::TextureDesc& desc,
                            const RgImport& state = {});
    RgBuffer importBuffer(std::string_view name, rhi::BufferH buffer, const rhi::BufferDesc& desc,
                          const RgImport& state = {});
    /// Keeps the producer of this version alive (debug capture, visualizer). Writes to imported
    /// resources are outputs already.
    void markOutput(RgTexture texture);
    void markOutput(RgBuffer buffer);

    /// Adds a pass with typed data: setup(RgBuilder&, Data&) runs now; execute(const Data&,
    /// RgContext&) runs during execute() on a recording job. Returns the data (stable address).
    template <class Data, class Setup, class Execute>
    const Data& addPass(std::string_view name, PassFlags flags, Setup&& setup, Execute&& execute);
    /// Adds a pass without typed data.
    void addPass(std::string_view name, PassFlags flags, const std::function<void(RgBuilder&)>& setup,
                 std::function<void(RgContext&)> execute);

    /// Plans the frame (see the header comment). Fails with InvalidArgument listing every setup
    /// error. May be called again with other options.
    Result<void> compile(const RgCompileOptions& options = {});
    bool isCompiled() const noexcept;
    /// The compiled plan (valid after a successful compile()).
    const RgPlan& plan() const noexcept;

    /// Resolves entry barriers against the pool (and import states), records every command list
    /// (in parallel with options.jobs) and submits the batches in order with their waits. The
    /// pool's resources and states carry over to the next frame's graph.
    Result<RgExecuteResult> execute(rhi::Device& device, RgResourcePool& pool, const RgExecuteOptions& options = {});
    /// The schedule of the last execute(): the plan with resolved entry barriers and the prologue.
    const RgPlan& executedPlan() const noexcept;

    /// Deterministic text dump of the plan (trace goldens, logs) and a Graphviz DAG (visualizer).
    std::string dumpText() const;
    std::string dumpGraphviz() const;

    /// Setup errors so far (compile() fails while this is non-empty).
    const std::vector<std::string>& errors() const noexcept;
    /// RgContext misuse during execute() (undeclared resources).
    u64 contextErrorCount() const noexcept;

    u32 passCount() const noexcept;
    u32 resourceCount() const noexcept;

    struct Impl;

private:
    friend class RgBuilder;
    friend class RgContext;

    struct PassDataBase {
        virtual ~PassDataBase() = default;
    };
    template <class Data>
    struct PassData final : PassDataBase {
        Data value{};
    };
    u32 beginPass(std::string_view name, PassFlags flags, std::unique_ptr<PassDataBase> data);
    void endPass(u32 pass, std::function<void(RgContext&)> execute);

    std::unique_ptr<Impl> m_impl;
};

template <class Data, class Setup, class Execute>
const Data& RenderGraph::addPass(std::string_view name, PassFlags flags, Setup&& setup, Execute&& execute) {
    static_assert(std::is_invocable_v<Setup&, RgBuilder&, Data&>, "setup must be callable as (RgBuilder&, Data&)");
    static_assert(std::is_invocable_v<Execute&, const Data&, RgContext&>,
                  "execute must be callable as (const Data&, RgContext&)");
    auto holder = std::make_unique<PassData<Data>>();
    Data* data = &holder->value;
    const u32 pass = beginPass(name, flags, std::move(holder));
    RgBuilder builder(*this, pass);
    setup(builder, *data);
    endPass(pass, [data, fn = std::forward<Execute>(execute)](RgContext& ctx) { fn(static_cast<const Data&>(*data), ctx); });
    return *data;
}

/// Pooled physical resources behind the transients, reused across frames with their tracked
/// states and the timeline points of their last use (cross-frame synchronization). Several graphs
/// per frame may share one pool. Resources that no graph used during the last `keepFrames`
/// completed frames (Device::frameIndex(), advanced by beginFrame) are destroyed at the end of an
/// execute (deferred by the RHI); without beginFrame calls nothing is trimmed (use clear()).
/// Threading: single-threaded (the thread that calls RenderGraph::execute).
class RgResourcePool {
public:
    explicit RgResourcePool(rhi::Device& device, u32 keepFrames = 3);
    ~RgResourcePool();
    RgResourcePool(const RgResourcePool&) = delete;
    RgResourcePool& operator=(const RgResourcePool&) = delete;

    rhi::Device& device() const noexcept;
    u32 textureCount() const noexcept;
    u32 bufferCount() const noexcept;
    /// Estimated bytes of all pooled resources.
    u64 bytes() const noexcept;
    /// Destroys every pooled resource (e.g. after a resolution change). Not during execute().
    void clear();

    struct Impl;

private:
    friend class RenderGraph;
    std::unique_ptr<Impl> m_impl;
};

/// Deterministic text dump of a plan (RenderGraph::dumpText() for plan(); use it for executedPlan()).
std::string rgDumpPlan(const RgPlan& plan);

/// Estimated memory of a texture (all mips, layers and samples; tightly packed).
u64 rgTextureBytes(const RgTextureDesc& desc) noexcept;
/// "Sampled|ColorAttachment" style names (dumps).
std::string rgTextureUsageName(rhi::TextureUsage usage);
std::string rgBufferUsageName(rhi::BufferUsage usage);
std::string_view rgQueueName(rhi::Queue queue) noexcept;

} // namespace helios::render
