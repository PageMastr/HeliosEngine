// Null backend (03 §1.4): the whole Device API without a GPU. Commands are recorded into
// per-list vectors, validated against usage flags at record time and "executed" in submission
// order at submit time: resource states are checked and updated, buffer/texture copies are
// emulated on host memory, and a canonical text trace is appended. Everything completes
// immediately, so timelines advance at submit.
//
// Threading: recording is lock-free per list (handle lookups use the pools' shared locks); device
// state (timelines, trace, lists, swapchains) is guarded by m_mutex.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <thread>

#include "helios/core/jobs.h"
#include "helios/rhi/null_device.h"
#include "rhi_internal.h"

namespace helios::rhi::detail {
namespace {

constexpr u64 kNullAddressBase = 0x100000ull;

std::string hexBytes(const void* data, usize size, usize maxBytes = 64) {
    const auto* p = static_cast<const u8*>(data);
    std::string out;
    const usize n = std::min(size, maxBytes);
    out.reserve(n * 2 + 3);
    for (usize i = 0; i < n; ++i) out += std::format("{:02x}", p[i]);
    if (size > maxBytes) out += "...";
    return out;
}

std::string_view loadOpName(LoadOp op) {
    switch (op) {
    case LoadOp::Load: return "Load";
    case LoadOp::Clear: return "Clear";
    case LoadOp::DontCare: return "DontCare";
    }
    return "?";
}
std::string_view storeOpName(StoreOp op) { return op == StoreOp::Store ? "Store" : "DontCare"; }

struct NullBuffer {
    BufferDesc desc;
    std::string name;   // as given (desc.name views it)
    std::string label;  // name, or "buffer#<index>" when unnamed (traces, messages)
    std::vector<u8> data;  // host backing: sized at creation when mappable, lazily otherwise
    u64 address = 0;
    BindlessIndex storageIndex = kInvalidBindless;
    ResourceState state = ResourceState::Undefined;

    u8* bytes(u64 minSize) {
        if (data.size() < minSize) data.resize(static_cast<usize>(desc.size), 0);
        return data.data();
    }
};

struct NullTexture {
    TextureDesc desc;
    std::string name;
    std::string label;
    std::vector<ResourceState> states;       // [mip * arrayLayers + layer]
    std::vector<std::vector<u8>> data;       // same indexing; empty until written
    BindlessIndex srvIndex = kInvalidBindless;
    std::vector<std::pair<ViewDesc, BindlessIndex>> views;
    std::vector<BindlessIndex> uavs;         // per mip, lazily
    bool swapchainImage = false;

    usize sub(u32 mip, u32 layer) const { return static_cast<usize>(mip) * desc.arrayLayers + layer; }
    u32 mipDepth(u32 mip) const { return desc.type == TextureType::Tex3D ? mipExtent(desc.depth, mip) : 1; }
    u64 subresourceBytes(u32 mip) const {
        return formatSurfaceBytes(desc.format, mipExtent(desc.width, mip), mipExtent(desc.height, mip), mipDepth(mip));
    }
    std::vector<u8>& subData(u32 mip, u32 layer) {
        std::vector<u8>& d = data[sub(mip, layer)];
        if (d.empty()) d.resize(static_cast<usize>(subresourceBytes(mip)), 0);
        return d;
    }
};

struct NullPipelineState {
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
};

struct NullPipeline {
    std::string name;
    bool compute = false;
    u32 colorCount = 0;
    std::array<Format, kMaxColorAttachments> colorFormats{};
    Format depthFormat = Format::Unknown;
    u32 sampleCount = 1;
    std::shared_ptr<NullPipelineState> state = std::make_shared<NullPipelineState>();
};

struct NullSwapchain {
    std::string name;
    SwapchainInfo info;
    std::vector<TextureH> images;
    u32 next = 0;
    u32 current = ~0u;
};

enum class UseKind : u8 { Buffer, Texture, Pipeline };

/// A resource reference of a recorded command, checked in submission order.
struct Use {
    UseKind kind = UseKind::Buffer;
    u64 handle = 0;
    ResourceState state = ResourceState::Count;  // required state (Count = none), or 'before'
    ResourceState after = ResourceState::Count;  // != Count: this use is a transition
    SubresourceRange range{};                     // textures: resolved range
    std::string_view what;
};

class NullDeviceImpl;

struct Command {
    std::string text;
    std::vector<Use> uses;
    std::function<void(NullDeviceImpl&)> exec;  // memory emulation, run at submit
};

class NullCommandList final : public CommandList {
public:
    NullCommandList(NullDeviceImpl& device, Queue queue);

    void reset(Queue queue, std::string_view name, u64 frame) {
        m_queue = queue;
        m_name = name.empty() ? std::string("list") : std::string(name);
        m_owner = std::this_thread::get_id();
        m_acquireFrame = frame;
        m_open = true;
        m_submitted = false;
        m_recycled = false;
        m_inRendering = false;
        m_pipeline = {};
        m_pipelineCompute = false;
        m_hasIndexBuffer = false;
        m_labelDepth = 0;
        m_renderColorCount = 0;
        m_renderColorFormats = {};
        m_renderDepthFormat = Format::Unknown;
        m_commands.clear();
    }

    void end() override;
    void barrier(std::span<const Barrier> barriers) override;
    void beginRendering(const RenderingDesc& desc) override;
    void endRendering() override;
    void setViewport(const Viewport& v) override;
    void setScissor(const Rect& r) override;
    void bindPipeline(PipelineH pipeline) override;
    void pushConstants(const void* data, u32 bytes, u32 offset) override;
    void bindIndexBuffer(BufferH buffer, u64 offset, IndexType type) override;
    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) override;
    void drawIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) override;
    void drawIndexedIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) override;
    void drawIndexedIndirectCount(BufferH args, u64 offset, BufferH count, u64 countOffset, u32 maxDraws,
                                  u32 stride) override;
    void dispatch(u32 x, u32 y, u32 z) override;
    void dispatchIndirect(BufferH args, u64 offset) override;
    void copyBuffer(BufferH src, u64 srcOffset, BufferH dst, u64 dstOffset, u64 size) override;
    void copyBufferToTexture(BufferH src, const BufferTextureLayout& layout, TextureH dst,
                             const TextureRegion& region) override;
    void copyTextureToBuffer(TextureH src, const TextureRegion& region, BufferH dst,
                             const BufferTextureLayout& layout) override;
    void fillBuffer(BufferH buffer, u64 offset, u64 size, u32 value) override;
    void updateBuffer(BufferH buffer, u64 offset, std::span<const std::byte> data) override;
    void clearTexture(TextureH texture, const std::array<f32, 4>& color, const SubresourceRange& range) override;
    void beginLabel(std::string_view name, u32 rgba) override;
    void endLabel() override;
    void insertLabel(std::string_view name, u32 rgba) override;
    void breadcrumb(u16 passId, BreadcrumbStage stage) override;

    const std::string& name() const { return m_name; }
    bool isOpen() const { return m_open; }
    bool isSubmitted() const { return m_submitted; }
    void markSubmitted() { m_submitted = true; }
    /// Returned to the free list by beginFrame/waitIdle: any further use is a stale-pointer bug.
    bool isRecycled() const { return m_recycled; }
    void markRecycled() {
        m_recycled = true;
        m_open = false;
    }
    u64 acquireFrame() const { return m_acquireFrame; }
    std::thread::id owner() const { return m_owner; }
    const std::vector<Command>& commands() const { return m_commands; }

private:
    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args);
    bool recording(std::string_view op);
    bool outsideRendering(std::string_view op);
    bool requireQueue(std::string_view op, bool graphicsOnly);
    NullBuffer* buffer(BufferH h, std::string_view op, BufferUsage usage);
    NullTexture* texture(TextureH h, std::string_view op, TextureUsage usage);
    bool checkRange(const NullBuffer& b, u64 offset, u64 size, std::string_view op);
    std::optional<TextureRegion> resolveRegion(const NullTexture& t, const TextureRegion& region,
                                               const BufferTextureLayout& layout, u64& bufferBytes, std::string_view op);
    bool validateDrawState(std::string_view op);
    void add(std::string text, std::vector<Use> uses = {}, std::function<void(NullDeviceImpl&)> exec = {}) {
        m_commands.push_back(Command{std::move(text), std::move(uses), std::move(exec)});
    }

    NullDeviceImpl& m_device;
    std::string m_name;
    std::thread::id m_owner;
    u64 m_acquireFrame = 0;
    bool m_open = false;
    bool m_submitted = false;
    bool m_recycled = false;
    bool m_inRendering = false;
    PipelineH m_pipeline;
    bool m_pipelineCompute = false;
    bool m_hasIndexBuffer = false;
    u32 m_labelDepth = 0;
    u32 m_renderColorCount = 0;
    std::array<Format, kMaxColorAttachments> m_renderColorFormats{};
    Format m_renderDepthFormat = Format::Unknown;
    std::vector<Command> m_commands;
};

class NullDeviceImpl final : public NullDevice {
public:
    explicit NullDeviceImpl(const DeviceDesc& desc) : m_desc(desc) {
        m_caps.backend = Backend::Null;
        m_caps.adapter = nullAdapterInfo();
        m_caps.bits = (CapBit::DebugUtils | CapBit::Swapchain | CapBit::AsyncComputeQueue | CapBit::TransferQueue |
                       CapBit::StorageImageWithoutFormat | CapBit::SamplerAnisotropy | CapBit::ShaderInt64 |
                       CapBit::ShaderFloat16 | CapBit::FillModeNonSolid | CapBit::TimestampQueries) &
                      desc.capsMask;
        Limits& l = m_caps.limits;
        l.maxBindlessSampledImages = 131072;
        l.maxBindlessStorageImages = 16384;
        l.maxBindlessStorageBuffers = 65536;
        l.maxBindlessSamplers = 128;
        l.maxPushConstantBytes = kMaxPushConstantBytes;
        l.maxTextureDimension2D = 16384;
        l.maxTextureDimension3D = 2048;
        l.maxTextureArrayLayers = 2048;
        l.maxComputeWorkGroupCount = {65535, 65535, 65535};
        l.maxComputeWorkGroupSize = {1024, 1024, 64};
        l.maxComputeWorkGroupInvocations = 1024;
        l.minStorageBufferOffsetAlignment = 16;
        l.minUniformBufferOffsetAlignment = 64;
        l.optimalBufferCopyRowPitchAlignment = 1;
        l.timestampPeriodNs = 1.0;
        l.subgroupSize = 32;
        l.maxSamplerAnisotropy = 16.0f;
        m_sampledSlots.reset(l.maxBindlessSampledImages);
        m_storageImageSlots.reset(l.maxBindlessStorageImages);
        m_storageBufferSlots.reset(l.maxBindlessStorageBuffers);
        m_samplers.push_back(SamplerDesc{});  // slot 0: default linear/repeat sampler
    }

    ~NullDeviceImpl() override {
        // Async "compiles" hold the pipeline state by shared_ptr; wait for them so the pool (which
        // may be destroyed after us) never runs a job that touches a dead device.
        while (m_pendingCompiles.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    }

    const Caps& caps() const noexcept override { return m_caps; }

    // -- Resources ----------------------------------------------------------------------------
    Result<BufferH> createBuffer(const BufferDesc& desc) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        HELIOS_TRY(validateBufferDesc(desc));
        NullBuffer b;
        b.desc = desc;
        b.name = std::string(desc.name);
        b.desc.tag = desc.tag == MemoryTag::Unknown ? defaultBufferTag() : desc.tag;
        if (desc.memory != MemoryUsage::GpuOnly) b.data.resize(static_cast<usize>(desc.size), 0);
        {
            std::lock_guard lock(m_mutex);
            b.address = m_nextAddress;
            m_nextAddress += alignUp<u64>(desc.size, 256);
        }
        if (hasFlag(desc.usage, BufferUsage::Storage)) {
            b.storageIndex = m_storageBufferSlots.allocate();
            if (b.storageIndex == kInvalidBindless) {
                return Error{ErrorCode::LimitExceeded, "bindless storage-buffer slots exhausted"};
            }
        }
        trackAllocation(b.desc.tag, static_cast<usize>(desc.size));
        const u64 size = desc.size;
        const BufferH h = m_buffers.create(std::move(b));
        NullBuffer* stored = m_buffers.get(h);
        stored->desc.name = stored->name;
        stored->label = resourceLabel("buffer", stored->name, h.index());
        m_bufferBytes.fetch_add(size, std::memory_order_relaxed);
        return h;
    }

    Result<TextureH> createTexture(const TextureDesc& desc) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        HELIOS_TRY(validateTextureDesc(desc, m_caps.limits));
        return createTextureInternal(desc, false);
    }

    Result<TextureH> createTextureInternal(const TextureDesc& desc, bool swapchainImage) {
        NullTexture t;
        t.desc = desc;
        t.name = std::string(desc.name);
        t.desc.tag = desc.tag == MemoryTag::Unknown ? defaultTextureTag() : desc.tag;
        t.swapchainImage = swapchainImage;
        const usize subs = static_cast<usize>(desc.mipLevels) * desc.arrayLayers;
        t.states.assign(subs, ResourceState::Undefined);
        t.data.resize(subs);
        t.uavs.assign(desc.mipLevels, kInvalidBindless);
        if (hasFlag(desc.usage, TextureUsage::Sampled)) {
            t.srvIndex = m_sampledSlots.allocate();
            if (t.srvIndex == kInvalidBindless) return Error{ErrorCode::LimitExceeded, "bindless texture slots exhausted"};
        }
        u64 bytes = 0;
        for (u32 m = 0; m < desc.mipLevels; ++m) bytes += t.subresourceBytes(m) * desc.arrayLayers * desc.sampleCount;
        if (!swapchainImage) trackAllocation(t.desc.tag, static_cast<usize>(bytes));
        const TextureH h = m_textures.create(std::move(t));
        NullTexture* stored = m_textures.get(h);
        stored->desc.name = stored->name;
        stored->label = resourceLabel("texture", stored->name, h.index());
        m_textureBytes.fetch_add(bytes, std::memory_order_relaxed);
        return h;
    }

    void destroy(BufferH h) override {
        NullBuffer* b = m_buffers.get(h);
        if (!b) return;
        m_storageBufferSlots.release(b->storageIndex);
        trackDeallocation(b->desc.tag, static_cast<usize>(b->desc.size));
        m_bufferBytes.fetch_sub(b->desc.size, std::memory_order_relaxed);
        m_buffers.destroy(h);
    }

    void destroy(TextureH h) override {
        NullTexture* t = m_textures.get(h);
        if (!t) return;
        if (t->swapchainImage) {
            report(std::format("destroy: texture '{}' is a swapchain image; destroy the swapchain instead", t->label));
            return;
        }
        destroyTextureInternal(h);
    }

    void destroyTextureInternal(TextureH h) {
        NullTexture* t = m_textures.get(h);
        if (!t) return;
        m_sampledSlots.release(t->srvIndex);
        for (auto& [view, index] : t->views) m_sampledSlots.release(index);
        for (BindlessIndex index : t->uavs) m_storageImageSlots.release(index);
        u64 bytes = 0;
        for (u32 m = 0; m < t->desc.mipLevels; ++m) {
            bytes += t->subresourceBytes(m) * t->desc.arrayLayers * t->desc.sampleCount;
        }
        if (!t->swapchainImage) trackDeallocation(t->desc.tag, static_cast<usize>(bytes));
        m_textureBytes.fetch_sub(bytes, std::memory_order_relaxed);
        m_textures.destroy(h);
    }

    void destroy(PipelineH h) override {
        // Dropping the entry is enough: an in-flight async compile holds the state by shared_ptr.
        if (m_pipelines.destroy(h)) m_pipelineCount.fetch_sub(1, std::memory_order_relaxed);
    }

    void destroy(SwapchainH h) override {
        std::vector<TextureH> images;
        {
            std::lock_guard lock(m_mutex);
            NullSwapchain* sc = m_swapchains.get(h);
            if (!sc) return;
            images = sc->images;
            m_swapchains.destroy(h);
        }
        for (TextureH image : images) destroyTextureInternal(image);
    }

    BufferDesc bufferDesc(BufferH h) const override {
        const NullBuffer* b = m_buffers.get(h);
        return b ? b->desc : BufferDesc{};
    }
    TextureDesc textureDesc(TextureH h) const override {
        const NullTexture* t = m_textures.get(h);
        if (!t) {
            TextureDesc d;
            d.width = 0;
            return d;
        }
        return t->desc;
    }

    void* map(BufferH h) override {
        NullBuffer* b = m_buffers.get(h);
        if (!b || b->desc.memory == MemoryUsage::GpuOnly) return nullptr;
        return b->data.data();
    }
    void flushMapped(BufferH, u64, u64) override {}
    void invalidateMapped(BufferH, u64, u64) override {}

    // -- Bindless -----------------------------------------------------------------------------
    BindlessIndex srv(TextureH h, const ViewDesc& view) override {
        NullTexture* t = m_textures.get(h);
        if (!t) return kInvalidBindless;
        if (!hasFlag(t->desc.usage, TextureUsage::Sampled)) {
            report(std::format("srv: texture '{}' lacks TextureUsage::Sampled", t->label));
            return kInvalidBindless;
        }
        if (view == ViewDesc{}) return t->srvIndex;
        auto resolved = resolveView(t->desc, view);
        if (!resolved) {
            report(resolved.error().message);
            return kInvalidBindless;
        }
        std::lock_guard lock(m_mutex);
        for (const auto& [v, index] : t->views) {
            if (v == *resolved) return index;
        }
        const BindlessIndex index = m_sampledSlots.allocate();
        if (index != kInvalidBindless) t->views.emplace_back(*resolved, index);
        return index;
    }

    BindlessIndex uav(TextureH h, u32 mip) override {
        NullTexture* t = m_textures.get(h);
        if (!t) return kInvalidBindless;
        if (!hasFlag(t->desc.usage, TextureUsage::Storage) || mip >= t->desc.mipLevels) {
            report(std::format("uav: texture '{}' lacks TextureUsage::Storage or mip {} is out of range", t->label, mip));
            return kInvalidBindless;
        }
        std::lock_guard lock(m_mutex);
        if (t->uavs[mip] == kInvalidBindless) t->uavs[mip] = m_storageImageSlots.allocate();
        return t->uavs[mip];
    }

    BindlessIndex srv(BufferH h) override {
        NullBuffer* b = m_buffers.get(h);
        if (!b) return kInvalidBindless;
        if (b->storageIndex == kInvalidBindless) {
            report(std::format("srv: buffer '{}' lacks BufferUsage::Storage", b->label));
        }
        return b->storageIndex;
    }

    u64 deviceAddress(BufferH h) override {
        const NullBuffer* b = m_buffers.get(h);
        return b ? b->address : 0;
    }

    BindlessIndex sampler(const SamplerDesc& desc) override {
        std::lock_guard lock(m_mutex);
        for (usize i = 0; i < m_samplers.size(); ++i) {
            if (m_samplers[i] == desc) return static_cast<BindlessIndex>(i);
        }
        if (m_samplers.size() >= m_caps.limits.maxBindlessSamplers) {
            reportLocked("sampler: bindless sampler slots exhausted");
            return kInvalidBindless;
        }
        m_samplers.push_back(desc);
        return static_cast<BindlessIndex>(m_samplers.size() - 1);
    }

    // -- Pipelines ----------------------------------------------------------------------------
    Result<PipelineH> createGraphicsPipeline(const GraphicsPipelineDesc& desc, PsoPriority priority) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        NullPipeline p;
        p.name = std::string(desc.name);
        p.compute = false;
        p.colorCount = desc.colorCount;
        p.colorFormats = desc.colorFormats;
        p.depthFormat = desc.depthFormat;
        p.sampleCount = desc.sampleCount;
        if (priority == PsoPriority::Immediate || !m_desc.pipelineCompilePool) {
            HELIOS_TRY(validateGraphicsPipelineDesc(desc));
            p.state->ready.store(true, std::memory_order_release);
            return addPipeline(std::move(p));
        }
        // Async: validate on the pool, like a real compile would. The job owns copies of the SPIR-V.
        auto job = [desc, vs = copyWords(desc.vertex.spirv), fs = copyWords(desc.fragment.spirv),
                    ve = std::string(desc.vertex.entryPoint), fe = std::string(desc.fragment.entryPoint),
                    name = p.name]() mutable {
            GraphicsPipelineDesc d = desc;
            d.vertex = ShaderDesc{vs, ve};
            d.fragment = ShaderDesc{fs, fe};
            d.name = name;
            return validateGraphicsPipelineDesc(d);
        };
        return addPipelineAsync(std::move(p), std::move(job), priority);
    }

    Result<PipelineH> createComputePipeline(const ComputePipelineDesc& desc, PsoPriority priority) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        NullPipeline p;
        p.name = std::string(desc.name);
        p.compute = true;
        if (priority == PsoPriority::Immediate || !m_desc.pipelineCompilePool) {
            HELIOS_TRY(validateComputePipelineDesc(desc));
            p.state->ready.store(true, std::memory_order_release);
            return addPipeline(std::move(p));
        }
        auto job = [cs = copyWords(desc.compute.spirv), ce = std::string(desc.compute.entryPoint), name = p.name] {
            ComputePipelineDesc d;
            d.compute = ShaderDesc{cs, ce};
            d.name = name;
            return validateComputePipelineDesc(d);
        };
        return addPipelineAsync(std::move(p), std::move(job), priority);
    }

    bool isReady(PipelineH h) const override {
        const NullPipeline* p = m_pipelines.get(h);
        return p && p->state->ready.load(std::memory_order_acquire);
    }

    std::vector<u8> pipelineCacheData() const override { return {}; }

    // -- Commands -----------------------------------------------------------------------------
    CommandList* acquireCommandList(Queue queue, std::string_view name) override {
        if (isDeviceLost()) return nullptr;
        std::lock_guard lock(m_mutex);
        NullCommandList* list = nullptr;
        if (!m_freeLists.empty()) {
            list = m_freeLists.back();
            m_freeLists.pop_back();
        } else {
            m_allLists.push_back(std::make_unique<NullCommandList>(*this, queue));
            list = m_allLists.back().get();
        }
        list->reset(queue, name, m_frameIndex.load(std::memory_order_relaxed));
        m_activeLists.push_back(list);
        return list;
    }

    Result<TimelinePoint> submit(Queue queue, std::span<CommandList* const> lists,
                                 std::span<const TimelinePoint> waits) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        std::unique_lock lock(m_mutex);
        const u32 q = queueIndex(queue);
        std::string header = std::format("submit {} #{} waits=[", queueName(queue), m_signaled[q] + 1);
        bool first = true;
        for (const TimelinePoint& w : waits) {
            if (w.isNull()) continue;
            if (w.value > m_signaled[queueIndex(w.queue)]) {
                reportLocked(std::format("submit {}: waits for {}:{} which was never submitted (would deadlock)",
                                         queueName(queue), queueName(w.queue), w.value));
            }
            header += std::format("{}{}:{}", first ? "" : ",", queueName(w.queue), w.value);
            first = false;
        }
        header += "]";
        m_trace += header;
        m_trace += '\n';

        for (CommandList* base : lists) {
            if (!ownsList(base)) {
                reportLocked("submit: command list was not acquired from this device");
                continue;
            }
            auto* list = static_cast<NullCommandList*>(base);
            if (list->isSubmitted()) {
                reportLocked(std::format("submit: list '{}' was already submitted", list->name()));
                continue;
            }
            if (list->isRecycled() || list->acquireFrame() != m_frameIndex.load(std::memory_order_relaxed)) {
                reportLocked(std::format("submit: list '{}' was acquired in frame {} and recycled by beginFrame/"
                                         "waitIdle before it was submitted",
                                         list->name(), list->acquireFrame()));
                continue;
            }
            if (list->queue() != queue) {
                reportLocked(std::format("submit: list '{}' was acquired for {} but submitted to {}", list->name(),
                                         queueName(list->queue()), queueName(queue)));
            }
            if (list->isOpen()) {
                if (list->owner() != std::this_thread::get_id()) {
                    reportLocked(std::format("submit: list '{}' is still recording on another thread; call end() "
                                             "on the recording thread first", list->name()));
                }
                list->end();  // reports through m_messageMutex only; m_mutex stays held
            }
            list->markSubmitted();
            m_trace += std::format("  list \"{}\"\n", list->name());
            execute(*list);
        }
        const u64 value = ++m_signaled[q];
        if (m_desc.debugDeviceLostAfterSubmits != 0 && ++m_submitCount == m_desc.debugDeviceLostAfterSubmits) {
            lock.unlock();
            return loseDevice("submit (injected)");
        }
        return TimelinePoint{queue, value};
    }

    u64 completedValue(Queue queue) const override {
        std::lock_guard lock(m_mutex);
        return m_signaled[queueIndex(queue)];
    }

    Result<void> wait(TimelinePoint point, u64) override {
        if (point.isNull()) return {};
        std::lock_guard lock(m_mutex);
        if (point.value > m_signaled[queueIndex(point.queue)]) {
            // Nothing can ever signal it on the Null device: report instead of hanging forever.
            reportLocked(std::format("wait: {}:{} was never submitted", queueName(point.queue), point.value));
            return makeError(ErrorCode::Timeout, "wait for {}:{} which was never submitted", queueName(point.queue),
                             point.value);
        }
        return {};
    }

    Result<void> waitIdle() override {
        std::lock_guard lock(m_mutex);
        recycleLists();
        return {};
    }

    Result<void> beginFrame() override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        std::lock_guard lock(m_mutex);
        recycleLists();
        m_frameIndex.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    u64 frameIndex() const noexcept override { return m_frameIndex.load(std::memory_order_relaxed); }

    // -- Swapchains ---------------------------------------------------------------------------
    Result<SwapchainH> createSwapchain(const SwapchainDesc& desc) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        NullSwapchain sc;
        sc.name = desc.name.empty() ? std::string("swapchain") : std::string(desc.name);
        sc.info.format = desc.format == Format::Unknown ? Format::BGRA8Srgb : desc.format;
        sc.info.presentMode = desc.presentMode;
        sc.info.imageCount = std::clamp<u32>(desc.imageCount, 2, 8);
        sc.info.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc | TextureUsage::TransferDst;
        HELIOS_TRY(createSwapchainImages(sc, desc.width ? desc.width : 1280, desc.height ? desc.height : 720));
        std::lock_guard lock(m_mutex);
        return m_swapchains.create(std::move(sc));
    }

    Result<SwapchainImage> acquireNextImage(SwapchainH h) override {
        if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
        std::lock_guard lock(m_mutex);
        NullSwapchain* sc = m_swapchains.get(h);
        if (!sc) return Error{ErrorCode::InvalidArgument, "acquireNextImage: stale swapchain handle"};
        if (sc->current != ~0u) {
            reportLocked(std::format("acquireNextImage: '{}' image {} was acquired but never presented", sc->name,
                                     sc->current));
        }
        sc->current = sc->next;
        sc->next = (sc->next + 1) % sc->info.imageCount;
        m_trace += std::format("acquire \"{}\" image={}\n", sc->name, sc->current);
        SwapchainImage image;
        image.texture = sc->images[sc->current];
        image.index = sc->current;
        image.ready = TimelinePoint{Queue::Graphics, m_signaled[0]};
        return image;
    }

    Result<SwapchainStatus> present(SwapchainH h, TimelinePoint after) override {
        std::lock_guard lock(m_mutex);
        NullSwapchain* sc = m_swapchains.get(h);
        if (!sc) return Error{ErrorCode::InvalidArgument, "present: stale swapchain handle"};
        if (sc->current == ~0u) {
            reportLocked(std::format("present: '{}' has no acquired image", sc->name));
            return SwapchainStatus::Ok;
        }
        if (!after.isNull() && after.value > m_signaled[queueIndex(after.queue)]) {
            reportLocked(std::format("present: waits for {}:{} which was never submitted", queueName(after.queue),
                                     after.value));
        }
        const NullTexture* image = m_textures.get(sc->images[sc->current]);
        if (image && image->states[0] != ResourceState::Present) {
            reportLocked(std::format("present: '{}' image {} is in state {}, expected Present", sc->name, sc->current,
                                     resourceStateName(image->states[0])));
        }
        m_trace += std::format("present \"{}\" image={} after={}:{}\n", sc->name, sc->current, queueName(after.queue),
                               after.value);
        sc->current = ~0u;
        return SwapchainStatus::Ok;
    }

    Result<void> resizeSwapchain(SwapchainH h, u32 width, u32 height) override {
        std::vector<TextureH> old;
        NullSwapchain copy;
        {
            std::lock_guard lock(m_mutex);
            NullSwapchain* sc = m_swapchains.get(h);
            if (!sc) return Error{ErrorCode::InvalidArgument, "resizeSwapchain: stale swapchain handle"};
            old = sc->images;
            copy.name = sc->name;
            copy.info = sc->info;
        }
        for (TextureH image : old) destroyTextureInternal(image);
        HELIOS_TRY(createSwapchainImages(copy, width ? width : copy.info.width, height ? height : copy.info.height));
        std::lock_guard lock(m_mutex);
        NullSwapchain* sc = m_swapchains.get(h);
        if (!sc) return Error{ErrorCode::InvalidArgument, "resizeSwapchain: swapchain destroyed concurrently"};
        sc->images = copy.images;
        sc->info = copy.info;
        sc->next = 0;
        sc->current = ~0u;
        m_trace += std::format("resize \"{}\" {}x{}\n", sc->name, sc->info.width, sc->info.height);
        return {};
    }

    SwapchainInfo swapchainInfo(SwapchainH h) const override {
        std::lock_guard lock(m_mutex);
        const NullSwapchain* sc = m_swapchains.get(h);
        return sc ? sc->info : SwapchainInfo{};
    }

    // -- Diagnostics --------------------------------------------------------------------------
    MemoryStats memoryStats() const override {
        MemoryStats s;
        s.bufferBytes = m_bufferBytes.load(std::memory_order_relaxed);
        s.textureBytes = m_textureBytes.load(std::memory_order_relaxed);
        s.bufferCount = m_buffers.size();
        s.textureCount = m_textures.size();
        s.pipelineCount = m_pipelineCount.load(std::memory_order_relaxed);
        s.psoMisses = m_psoMisses.load(std::memory_order_relaxed);
        s.heaps.push_back(MemoryHeapStats{8ull << 30, s.bufferBytes + s.textureBytes, true});
        return s;
    }

    std::array<BreadcrumbState, kQueueCount> breadcrumbs() const override {
        std::lock_guard lock(m_mutex);
        return m_breadcrumbs;
    }

    u64 validationErrorCount() const noexcept override { return m_errorCount.load(std::memory_order_relaxed); }
    bool isDeviceLost() const noexcept override { return m_lost.load(std::memory_order_acquire); }

    // -- NullDevice ---------------------------------------------------------------------------
    std::string trace() const override {
        std::lock_guard lock(m_mutex);
        return m_trace;
    }
    void clearTrace() override {
        std::lock_guard lock(m_mutex);
        m_trace.clear();
    }
    std::vector<std::string> validationErrors() const override {
        std::lock_guard lock(m_messageMutex);
        return m_errors;
    }
    void clearValidationErrors() override {
        std::lock_guard lock(m_messageMutex);
        m_errors.clear();
    }
    ResourceState textureState(TextureH h, u32 mip, u32 layer) const override {
        std::lock_guard lock(m_mutex);
        const NullTexture* t = m_textures.get(h);
        if (!t || mip >= t->desc.mipLevels || layer >= t->desc.arrayLayers) return ResourceState::Undefined;
        return t->states[t->sub(mip, layer)];
    }

    // -- Internal (used by NullCommandList) ----------------------------------------------------
    void report(std::string message) {
        {
            std::lock_guard lock(m_messageMutex);
            m_errors.push_back(message);
        }
        m_errorCount.fetch_add(1, std::memory_order_relaxed);
        HELIOS_LOG_WARN(LogRhi, "[Null RHI] {}", message);
        if (m_desc.onMessage) m_desc.onMessage(ValidationMessage{ValidationMessage::Severity::Error, std::move(message)});
    }
    void reportLocked(std::string message) { report(std::move(message)); }  // m_messageMutex is separate

    NullBuffer* findBuffer(BufferH h) { return m_buffers.get(h); }
    NullTexture* findTexture(TextureH h) { return m_textures.get(h); }
    NullPipeline* findPipeline(PipelineH h) { return m_pipelines.get(h); }
    std::string bufferLabel(BufferH h) {
        const NullBuffer* b = m_buffers.get(h);
        return b ? resourceLabel("buffer", b->label, h.index()) : std::format("buffer#{}(stale)", h.index());
    }
    std::string textureLabel(TextureH h) {
        const NullTexture* t = m_textures.get(h);
        return t ? resourceLabel("texture", t->label, h.index()) : std::format("texture#{}(stale)", h.index());
    }
    std::string pipelineLabel(PipelineH h) {
        const NullPipeline* p = m_pipelines.get(h);
        return p ? resourceLabel("pipeline", p->name, h.index()) : std::format("pipeline#{}(stale)", h.index());
    }
    void countPsoMiss() { m_psoMisses.fetch_add(1, std::memory_order_relaxed); }

    /// Marks the device lost (once), reports DeviceLostInfo like the Vulkan backend would.
    Error loseDevice(std::string_view where) {
        bool expected = false;
        if (m_lost.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            DeviceLostInfo info;
            info.reason = std::format("device lost in {}", where);
            {
                std::lock_guard lock(m_mutex);
                info.breadcrumbs = m_breadcrumbs;
                info.submittedValues = m_signaled;
                info.completedValues = m_signaled;
                m_trace += std::format("deviceLost {}\n", where);
            }
            HELIOS_LOG_ERROR(LogRhi, "[Null RHI] {}", info.reason);
            if (m_desc.onDeviceLost) m_desc.onDeviceLost(info);
        }
        return makeError(ErrorCode::InvalidState, "GPU device lost ({})", where);
    }
    u64 currentFrame() const { return m_frameIndex.load(std::memory_order_relaxed); }
    void setBreadcrumb(Queue queue, u32 value, BreadcrumbStage stage) {
        BreadcrumbState& b = m_breadcrumbs[queueIndex(queue)];
        (stage == BreadcrumbStage::Begin ? b.lastBegin : b.lastEnd) = value;
    }

private:
    static std::vector<u32> copyWords(std::span<const u32> words) { return {words.begin(), words.end()}; }

    Result<PipelineH> addPipeline(NullPipeline&& p) {
        m_pipelineCount.fetch_add(1, std::memory_order_relaxed);
        return m_pipelines.create(std::move(p));
    }

    template <class Job>
    Result<PipelineH> addPipelineAsync(NullPipeline&& p, Job&& job, PsoPriority priority) {
        std::shared_ptr<NullPipelineState> state = p.state;
        const std::string name = p.name;
        HELIOS_TRY_ASSIGN(PipelineH h, addPipeline(std::move(p)));
        m_pendingCompiles.fetch_add(1, std::memory_order_acq_rel);
        const jobs::Priority jobPriority = priority == PsoPriority::High  ? jobs::Priority::High
                                           : priority == PsoPriority::Low ? jobs::Priority::Low
                                                                          : jobs::Priority::Normal;
        m_desc.pipelineCompilePool->run(
            [this, state, name, job = std::forward<Job>(job)]() mutable {
                Result<void> r = job();
                if (r) {
                    state->ready.store(true, std::memory_order_release);
                } else {
                    state->failed.store(true, std::memory_order_release);
                    report(std::format("async pipeline '{}' failed: {}", name, r.error().message));
                }
                m_pendingCompiles.fetch_sub(1, std::memory_order_acq_rel);
            },
            nullptr, jobPriority);
        return h;
    }

    Result<void> createSwapchainImages(NullSwapchain& sc, u32 width, u32 height) {
        sc.images.clear();
        sc.info.width = width;
        sc.info.height = height;
        for (u32 i = 0; i < sc.info.imageCount; ++i) {
            TextureDesc d = TextureDesc::tex2D(sc.info.format, width, height, sc.info.usage);
            const std::string name = std::format("{}[{}]", sc.name, i);
            d.name = name;
            HELIOS_TRY_ASSIGN(TextureH image, createTextureInternal(d, true));
            sc.images.push_back(image);
        }
        return {};
    }

    bool ownsList(const CommandList* list) const {
        return list && list->deviceTag() == static_cast<const Device*>(this);
    }

    void recycleLists() {
        for (NullCommandList* list : m_activeLists) {
            if (!list->isSubmitted()) {
                reportLocked(std::format("list '{}' was acquired but never submitted", list->name()));
            }
            list->markRecycled();
            m_freeLists.push_back(list);
        }
        m_activeLists.clear();
    }

    bool stateMatches(ResourceState tracked, ResourceState required, bool isBuffer) const {
        if (tracked == required || tracked == ResourceState::General) return true;
        // Buffers have no layouts; a never-transitioned buffer may be used in any state.
        return isBuffer && tracked == ResourceState::Undefined;
    }

    /// Checks/applies one command's resource uses in submission order (m_mutex held).
    void applyUses(const NullCommandList& list, const Command& cmd) {
        for (const Use& use : cmd.uses) {
            switch (use.kind) {
            case UseKind::Pipeline:
                if (!m_pipelines.get(PipelineH::fromBits(use.handle))) {
                    reportLocked(std::format("list '{}': '{}' uses a pipeline destroyed before submit", list.name(),
                                             cmd.text));
                }
                break;
            case UseKind::Buffer: {
                NullBuffer* b = m_buffers.get(BufferH::fromBits(use.handle));
                if (!b) {
                    reportLocked(std::format("list '{}': '{}' uses a buffer destroyed before submit", list.name(),
                                             cmd.text));
                    break;
                }
                if (use.state == ResourceState::Count) break;
                if (use.after != ResourceState::Count) {
                    if (use.state != ResourceState::Undefined && b->state != ResourceState::Undefined &&
                        b->state != use.state) {
                        reportLocked(std::format("list '{}': barrier on buffer '{}' expects {} but it is in {}",
                                                 list.name(), b->label, resourceStateName(use.state),
                                                 resourceStateName(b->state)));
                    }
                    b->state = use.after;
                } else if (!stateMatches(b->state, use.state, true)) {
                    reportLocked(std::format("list '{}': {} needs buffer '{}' in {} but it is in {}", list.name(),
                                             use.what, b->label, resourceStateName(use.state),
                                             resourceStateName(b->state)));
                }
                break;
            }
            case UseKind::Texture: {
                NullTexture* t = m_textures.get(TextureH::fromBits(use.handle));
                if (!t) {
                    reportLocked(std::format("list '{}': '{}' uses a texture destroyed before submit", list.name(),
                                             cmd.text));
                    break;
                }
                if (use.state == ResourceState::Count) break;
                bool reported = false;
                for (u32 m = use.range.baseMip; m < use.range.baseMip + use.range.mipCount; ++m) {
                    for (u32 l = use.range.baseLayer; l < use.range.baseLayer + use.range.layerCount; ++l) {
                        ResourceState& s = t->states[t->sub(m, l)];
                        if (use.after != ResourceState::Count) {
                            if (use.state != ResourceState::Undefined && s != use.state && !reported) {
                                reportLocked(std::format(
                                    "list '{}': barrier on texture '{}' (mip {}, layer {}) expects {} but it is in {}",
                                    list.name(), t->label, m, l, resourceStateName(use.state), resourceStateName(s)));
                                reported = true;
                            }
                            s = use.after;
                        } else if (!stateMatches(s, use.state, false) && !reported) {
                            reportLocked(std::format("list '{}': {} needs texture '{}' (mip {}, layer {}) in {} but "
                                                     "it is in {}",
                                                     list.name(), use.what, t->label, m, l,
                                                     resourceStateName(use.state), resourceStateName(s)));
                            reported = true;
                        }
                    }
                }
                break;
            }
            }
        }
    }

    void execute(const NullCommandList& list) {
        for (const Command& cmd : list.commands()) {
            applyUses(list, cmd);
            if (cmd.exec) cmd.exec(*this);
            m_trace += "    ";
            m_trace += cmd.text;
            m_trace += '\n';
        }
    }

    DeviceDesc m_desc;
    Caps m_caps;

    LockedPool<NullBuffer, BufferTag> m_buffers;
    LockedPool<NullTexture, TextureTag> m_textures;
    LockedPool<NullPipeline, PipelineTag> m_pipelines;
    HandlePool<NullSwapchain, SwapchainTag> m_swapchains;  // guarded by m_mutex

    SlotAllocator m_sampledSlots;
    SlotAllocator m_storageImageSlots;
    SlotAllocator m_storageBufferSlots;

    mutable std::mutex m_mutex;
    std::vector<SamplerDesc> m_samplers;
    u64 m_nextAddress = kNullAddressBase;
    std::array<u64, kQueueCount> m_signaled{};
    std::array<BreadcrumbState, kQueueCount> m_breadcrumbs{};
    std::string m_trace;
    std::vector<std::unique_ptr<NullCommandList>> m_allLists;
    std::vector<NullCommandList*> m_freeLists;
    std::vector<NullCommandList*> m_activeLists;

    mutable std::mutex m_messageMutex;
    std::vector<std::string> m_errors;
    std::atomic<u64> m_errorCount{0};

    std::atomic<u64> m_frameIndex{0};
    std::atomic<u64> m_bufferBytes{0};
    std::atomic<u64> m_textureBytes{0};
    std::atomic<u32> m_pipelineCount{0};
    std::atomic<u64> m_psoMisses{0};
    std::atomic<u32> m_pendingCompiles{0};
    std::atomic<bool> m_lost{false};
    u32 m_submitCount = 0;  // guarded by m_mutex
};

// -------------------------------------------------------------------------------------------------
// NullCommandList
// -------------------------------------------------------------------------------------------------
NullCommandList::NullCommandList(NullDeviceImpl& device, Queue queue)
    : CommandList(queue, static_cast<const Device*>(&device)), m_device(device) {}

template <class... Args>
void NullCommandList::error(std::format_string<Args...> fmt, Args&&... args) {
    m_device.report(std::format("list '{}' cmd {}: {}", m_name, m_commands.size(),
                                std::format(fmt, std::forward<Args>(args)...)));
}

bool NullCommandList::recording(std::string_view op) {
    if (!m_open) {
        error("{} recorded into a list that is not recording (ended or submitted)", op);
        return false;
    }
    if (std::this_thread::get_id() != m_owner) {
        error("{} recorded from a thread other than the one that acquired the list", op);
    }
    return true;
}

bool NullCommandList::outsideRendering(std::string_view op) {
    if (m_inRendering) {
        error("{} is not allowed inside beginRendering/endRendering", op);
        return false;
    }
    return true;
}

bool NullCommandList::requireQueue(std::string_view op, bool graphicsOnly) {
    if (graphicsOnly ? m_queue != Queue::Graphics : m_queue == Queue::Transfer) {
        error("{} is not supported on the {} queue", op, queueName(m_queue));
        return false;
    }
    return true;
}

NullBuffer* NullCommandList::buffer(BufferH h, std::string_view op, BufferUsage usage) {
    NullBuffer* b = m_device.findBuffer(h);
    if (!b) {
        error("{}: null or stale buffer handle", op);
        return nullptr;
    }
    if (usage != BufferUsage::None && !hasFlag(b->desc.usage, usage)) {
        error("{}: buffer '{}' lacks the required usage flag", op, b->label);
    }
    return b;
}

NullTexture* NullCommandList::texture(TextureH h, std::string_view op, TextureUsage usage) {
    NullTexture* t = m_device.findTexture(h);
    if (!t) {
        error("{}: null or stale texture handle", op);
        return nullptr;
    }
    if (usage != TextureUsage::None && !hasFlag(t->desc.usage, usage)) {
        error("{}: texture '{}' lacks the required usage flag", op, t->label);
    }
    return t;
}

bool NullCommandList::checkRange(const NullBuffer& b, u64 offset, u64 size, std::string_view op) {
    if (offset > b.desc.size || size > b.desc.size - offset) {
        error("{}: range {}+{} exceeds buffer '{}' ({} bytes)", op, offset, size, b.name, b.desc.size);
        return false;
    }
    return true;
}

std::optional<TextureRegion> NullCommandList::resolveRegion(const NullTexture& t, const TextureRegion& region,
                                                             const BufferTextureLayout& layout, u64& bufferBytes,
                                                             std::string_view op) {
    auto r = resolveCopyRegion(t.desc, region, layout, &bufferBytes);
    if (!r) {
        error("{}: {}", op, r.error().message);
        return std::nullopt;
    }
    return *r;
}

bool NullCommandList::validateDrawState(std::string_view op) {
    if (!requireQueue(op, true)) return false;
    if (!m_inRendering) {
        error("{} outside beginRendering/endRendering", op);
        return false;
    }
    if (!m_pipeline.isValid() || m_pipelineCompute) {
        error("{} without a bound graphics pipeline", op);
        return false;
    }
    NullPipeline* p = m_device.findPipeline(m_pipeline);
    if (!p) {
        error("{}: bound pipeline was destroyed", op);
        return false;
    }
    if (!p->state->ready.load(std::memory_order_acquire)) {
        m_device.countPsoMiss();
        add(std::format("{} skipped (pipeline \"{}\" not ready)", op, p->name));
        return false;
    }
    return true;
}

void NullCommandList::end() {
    if (!m_open) {
        error("end() on a list that is not recording");
        return;
    }
    if (m_inRendering) error("list ended inside beginRendering (missing endRendering)");
    if (m_labelDepth != 0) error("list ended with {} unbalanced beginLabel call(s)", m_labelDepth);
    m_open = false;
}

void NullCommandList::barrier(std::span<const Barrier> barriers) {
    if (!recording("barrier") || !outsideRendering("barrier")) return;
    for (const Barrier& b : barriers) {
        switch (b.kind) {
        case Barrier::Kind::Global:
            add(std::format("barrier global {}->{}", resourceStateName(b.before), resourceStateName(b.after)));
            break;
        case Barrier::Kind::Buffer: {
            NullBuffer* buf = buffer(b.buffer, "barrier", BufferUsage::None);
            if (!buf) break;
            std::string text = std::format("barrier buffer \"{}\" {}->{}", buf->label, resourceStateName(b.before),
                                           resourceStateName(b.after));
            if (b.offset != 0 || b.size != kWholeSize) text += std::format(" range={}+{}", b.offset, b.size);
            add(std::move(text), {Use{UseKind::Buffer, b.buffer.toBits(), b.before, b.after, {}, "barrier"}});
            break;
        }
        case Barrier::Kind::Texture: {
            NullTexture* t = texture(b.texture, "barrier", TextureUsage::None);
            if (!t) break;
            if (b.range.baseMip >= t->desc.mipLevels || b.range.baseLayer >= t->desc.arrayLayers) {
                error("barrier: subresource range outside texture '{}'", t->label);
                break;
            }
            if (b.after == ResourceState::Undefined) {
                error("barrier: texture '{}' cannot transition to Undefined", t->label);
            }
            const SubresourceRange r = resolveRange(t->desc, b.range);
            add(std::format("barrier texture \"{}\" {}->{} mips={}+{} layers={}+{}", t->label,
                            resourceStateName(b.before), resourceStateName(b.after), r.baseMip, r.mipCount,
                            r.baseLayer, r.layerCount),
                {Use{UseKind::Texture, b.texture.toBits(), b.before, b.after, r, "barrier"}});
            break;
        }
        }
    }
}

void NullCommandList::beginRendering(const RenderingDesc& desc) {
    if (!recording("beginRendering") || !requireQueue("beginRendering", true)) return;
    if (m_inRendering) {
        error("beginRendering while already rendering");
        return;
    }
    if (desc.colors.size() > kMaxColorAttachments) {
        error("beginRendering: {} color attachments (max {})", desc.colors.size(), kMaxColorAttachments);
        return;
    }
    if (desc.colors.empty() && !desc.depth.texture.isValid()) {
        error("beginRendering without attachments");
        return;
    }
    std::vector<Use> uses;
    std::string text = "beginRendering";
    u32 width = 0;
    u32 height = 0;
    m_renderColorCount = static_cast<u32>(desc.colors.size());
    m_renderColorFormats = {};
    m_renderDepthFormat = Format::Unknown;
    auto sizeCheck = [&](const NullTexture& t, u32 mip, std::string_view which) {
        const u32 w = mipExtent(t.desc.width, mip);
        const u32 h = mipExtent(t.desc.height, mip);
        if (width == 0) {
            width = w;
            height = h;
        } else if (w != width || h != height) {
            error("beginRendering: {} '{}' is {}x{}, other attachments are {}x{}", which, t.label, w, h, width, height);
        }
    };
    std::string attachments;
    for (usize i = 0; i < desc.colors.size(); ++i) {
        const ColorAttachment& c = desc.colors[i];
        NullTexture* t = texture(c.texture, "beginRendering color", TextureUsage::ColorAttachment);
        if (!t) return;
        if (c.mip >= t->desc.mipLevels || c.layer >= t->desc.arrayLayers) {
            error("beginRendering: color {} mip/layer out of range", i);
            return;
        }
        sizeCheck(*t, c.mip, "color attachment");
        m_renderColorFormats[i] = t->desc.format;
        attachments += std::format(" color{}=\"{}\" mip={} layer={} load={}", i, t->label, c.mip, c.layer,
                                   loadOpName(c.load));
        if (c.load == LoadOp::Clear) {
            attachments += std::format("({},{},{},{})", c.clearColor[0], c.clearColor[1], c.clearColor[2],
                                       c.clearColor[3]);
        }
        attachments += std::format(" store={}", storeOpName(c.store));
        uses.push_back(Use{UseKind::Texture, c.texture.toBits(), ResourceState::RenderTarget, ResourceState::Count,
                           SubresourceRange{c.mip, 1, c.layer, 1}, "color attachment"});
        if (c.resolve.isValid()) {
            NullTexture* r = texture(c.resolve, "beginRendering resolve", TextureUsage::ColorAttachment);
            if (!r) return;
            if (t->desc.sampleCount == 1 || r->desc.sampleCount != 1) {
                error("beginRendering: resolve needs a multisampled source and a single-sampled target");
            }
            attachments += std::format(" resolve=\"{}\"", r->label);
            uses.push_back(Use{UseKind::Texture, c.resolve.toBits(), ResourceState::RenderTarget, ResourceState::Count,
                               SubresourceRange{0, 1, 0, 1}, "resolve attachment"});
        }
    }
    if (desc.depth.texture.isValid()) {
        const DepthAttachment& d = desc.depth;
        NullTexture* t = texture(d.texture, "beginRendering depth", TextureUsage::DepthStencil);
        if (!t) return;
        if (d.mip >= t->desc.mipLevels || d.layer >= t->desc.arrayLayers) {
            error("beginRendering: depth mip/layer out of range");
            return;
        }
        if (d.readOnly && d.load == LoadOp::Clear) {
            error("beginRendering: read-only depth attachment '{}' with LoadOp::Clear (clearing writes)", t->label);
        }
        sizeCheck(*t, d.mip, "depth attachment");
        m_renderDepthFormat = t->desc.format;
        attachments += std::format(" depth=\"{}\" load={}", t->label, loadOpName(d.load));
        if (d.load == LoadOp::Clear) attachments += std::format("({})", d.clearDepth);
        attachments += std::format(" store={}{}", storeOpName(d.store), d.readOnly ? " readOnly" : "");
        uses.push_back(Use{UseKind::Texture, d.texture.toBits(),
                           d.readOnly ? ResourceState::DepthRead : ResourceState::DepthWrite, ResourceState::Count,
                           SubresourceRange{d.mip, 1, d.layer, 1}, "depth attachment"});
    }
    Rect area = desc.area;
    if (area.width == 0 || area.height == 0) area = Rect{0, 0, width, height};
    if (area.x < 0 || area.y < 0 || static_cast<u64>(area.x) + area.width > width ||
        static_cast<u64>(area.y) + area.height > height) {
        error("beginRendering: area {},{} {}x{} exceeds the attachments ({}x{})", area.x, area.y, area.width,
              area.height, width, height);
    }
    text += std::format(" area={},{} {}x{}", area.x, area.y, area.width, area.height);
    text += attachments;
    m_inRendering = true;
    add(std::move(text), std::move(uses));
}

void NullCommandList::endRendering() {
    if (!recording("endRendering")) return;
    if (!m_inRendering) {
        error("endRendering without beginRendering");
        return;
    }
    m_inRendering = false;
    add("endRendering");
}

void NullCommandList::setViewport(const Viewport& v) {
    if (!recording("setViewport") || !requireQueue("setViewport", true)) return;
    if (v.width <= 0.0f || v.height <= 0.0f) error("setViewport: empty viewport {}x{}", v.width, v.height);
    add(std::format("viewport {},{} {}x{} depth={}..{}", v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth));
}

void NullCommandList::setScissor(const Rect& r) {
    if (!recording("setScissor") || !requireQueue("setScissor", true)) return;
    add(std::format("scissor {},{} {}x{}", r.x, r.y, r.width, r.height));
}

void NullCommandList::bindPipeline(PipelineH h) {
    if (!recording("bindPipeline")) return;
    NullPipeline* p = m_device.findPipeline(h);
    if (!p) {
        error("bindPipeline: null or stale pipeline handle");
        return;
    }
    if (p->compute) {
        if (!requireQueue("bindPipeline (compute)", false)) return;
    } else {
        if (!requireQueue("bindPipeline (graphics)", true)) return;
        if (!m_inRendering) {
            error("bindPipeline: graphics pipeline '{}' bound outside rendering", p->name);
        } else {
            bool match = p->colorCount == m_renderColorCount && p->depthFormat == m_renderDepthFormat;
            for (u32 i = 0; match && i < p->colorCount; ++i) match = p->colorFormats[i] == m_renderColorFormats[i];
            if (!match) error("bindPipeline: attachment formats of '{}' do not match the current rendering", p->name);
        }
    }
    m_pipeline = h;
    m_pipelineCompute = p->compute;
    add(std::format("bindPipeline \"{}\"", p->name), {Use{UseKind::Pipeline, h.toBits(), ResourceState::Count}});
}

void NullCommandList::pushConstants(const void* data, u32 bytes, u32 offset) {
    if (!recording("pushConstants")) return;
    if (bytes == 0 || bytes % 4 != 0 || offset % 4 != 0 || offset + bytes > kMaxPushConstantBytes) {
        error("pushConstants: offset {} + size {} must be 4-byte multiples within {} bytes", offset, bytes,
              kMaxPushConstantBytes);
        return;
    }
    add(std::format("pushConstants offset={} size={} data={}", offset, bytes, hexBytes(data, bytes, 128)));
}

void NullCommandList::bindIndexBuffer(BufferH h, u64 offset, IndexType type) {
    if (!recording("bindIndexBuffer") || !requireQueue("bindIndexBuffer", true)) return;
    NullBuffer* b = buffer(h, "bindIndexBuffer", BufferUsage::Index);
    if (!b) return;
    const u64 align = type == IndexType::Uint16 ? 2 : 4;
    if (offset % align != 0 || offset >= b->desc.size) error("bindIndexBuffer: bad offset {}", offset);
    m_hasIndexBuffer = true;
    add(std::format("bindIndexBuffer \"{}\" offset={} type={}", b->label, offset,
                    type == IndexType::Uint16 ? "Uint16" : "Uint32"),
        {Use{UseKind::Buffer, h.toBits(), ResourceState::IndexBuffer, ResourceState::Count, {}, "bindIndexBuffer"}});
}

void NullCommandList::draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    if (!recording("draw") || !validateDrawState("draw")) return;
    add(std::format("draw vertices={} instances={} firstVertex={} firstInstance={}", vertexCount, instanceCount,
                    firstVertex, firstInstance));
}

void NullCommandList::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                                  u32 firstInstance) {
    if (!recording("drawIndexed") || !validateDrawState("drawIndexed")) return;
    if (!m_hasIndexBuffer) error("drawIndexed without bindIndexBuffer");
    add(std::format("drawIndexed indices={} instances={} firstIndex={} vertexOffset={} firstInstance={}", indexCount,
                    instanceCount, firstIndex, vertexOffset, firstInstance));
}

void NullCommandList::drawIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) {
    if (!recording("drawIndirect") || !validateDrawState("drawIndirect")) return;
    NullBuffer* b = buffer(args, "drawIndirect", BufferUsage::Indirect);
    if (!b) return;
    if (offset % 4 || stride % 4 || stride < sizeof(DrawIndirectCommand)) error("drawIndirect: bad offset/stride");
    if (drawCount > 0) checkRange(*b, offset, u64(stride) * (drawCount - 1) + sizeof(DrawIndirectCommand), "drawIndirect");
    add(std::format("drawIndirect \"{}\" offset={} count={} stride={}", b->label, offset, drawCount, stride),
        {Use{UseKind::Buffer, args.toBits(), ResourceState::IndirectArgument, ResourceState::Count, {}, "drawIndirect"}});
}

void NullCommandList::drawIndexedIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) {
    if (!recording("drawIndexedIndirect") || !validateDrawState("drawIndexedIndirect")) return;
    if (!m_hasIndexBuffer) error("drawIndexedIndirect without bindIndexBuffer");
    NullBuffer* b = buffer(args, "drawIndexedIndirect", BufferUsage::Indirect);
    if (!b) return;
    if (offset % 4 || stride % 4 || stride < sizeof(DrawIndexedIndirectCommand)) {
        error("drawIndexedIndirect: bad offset/stride");
    }
    if (drawCount > 0) {
        checkRange(*b, offset, u64(stride) * (drawCount - 1) + sizeof(DrawIndexedIndirectCommand), "drawIndexedIndirect");
    }
    add(std::format("drawIndexedIndirect \"{}\" offset={} count={} stride={}", b->label, offset, drawCount, stride),
        {Use{UseKind::Buffer, args.toBits(), ResourceState::IndirectArgument, ResourceState::Count, {},
             "drawIndexedIndirect"}});
}

void NullCommandList::drawIndexedIndirectCount(BufferH args, u64 offset, BufferH count, u64 countOffset,
                                               u32 maxDraws, u32 stride) {
    if (!recording("drawIndexedIndirectCount") || !validateDrawState("drawIndexedIndirectCount")) return;
    if (!m_hasIndexBuffer) error("drawIndexedIndirectCount without bindIndexBuffer");
    NullBuffer* a = buffer(args, "drawIndexedIndirectCount args", BufferUsage::Indirect);
    NullBuffer* c = buffer(count, "drawIndexedIndirectCount count", BufferUsage::Indirect);
    if (!a || !c) return;
    if (offset % 4 || countOffset % 4 || stride % 4 || stride < sizeof(DrawIndexedIndirectCommand)) {
        error("drawIndexedIndirectCount: bad offset/stride");
    }
    if (maxDraws > 0) {
        checkRange(*a, offset, u64(stride) * (maxDraws - 1) + sizeof(DrawIndexedIndirectCommand),
                   "drawIndexedIndirectCount args");
    }
    checkRange(*c, countOffset, 4, "drawIndexedIndirectCount count");
    add(std::format("drawIndexedIndirectCount \"{}\" offset={} count=\"{}\" countOffset={} maxDraws={} stride={}",
                    a->label, offset, c->label, countOffset, maxDraws, stride),
        {Use{UseKind::Buffer, args.toBits(), ResourceState::IndirectArgument, ResourceState::Count, {}, "indirect args"},
         Use{UseKind::Buffer, count.toBits(), ResourceState::IndirectArgument, ResourceState::Count, {},
             "indirect count"}});
}

void NullCommandList::dispatch(u32 x, u32 y, u32 z) {
    if (!recording("dispatch") || !requireQueue("dispatch", false) || !outsideRendering("dispatch")) return;
    if (!m_pipeline.isValid() || !m_pipelineCompute) {
        error("dispatch without a bound compute pipeline");
        return;
    }
    NullPipeline* p = m_device.findPipeline(m_pipeline);
    if (!p) {
        error("dispatch: bound pipeline was destroyed");
        return;
    }
    if (!p->state->ready.load(std::memory_order_acquire)) {
        m_device.countPsoMiss();
        add(std::format("dispatch skipped (pipeline \"{}\" not ready)", p->name));
        return;
    }
    const Limits& l = m_device.caps().limits;
    if (x > l.maxComputeWorkGroupCount[0] || y > l.maxComputeWorkGroupCount[1] || z > l.maxComputeWorkGroupCount[2]) {
        error("dispatch {}x{}x{} exceeds maxComputeWorkGroupCount", x, y, z);
    }
    add(std::format("dispatch {}x{}x{}", x, y, z));
}

void NullCommandList::dispatchIndirect(BufferH args, u64 offset) {
    if (!recording("dispatchIndirect") || !requireQueue("dispatchIndirect", false) ||
        !outsideRendering("dispatchIndirect")) {
        return;
    }
    if (!m_pipeline.isValid() || !m_pipelineCompute) {
        error("dispatchIndirect without a bound compute pipeline");
        return;
    }
    NullPipeline* p = m_device.findPipeline(m_pipeline);
    if (!p) {
        error("dispatchIndirect: bound pipeline was destroyed");
        return;
    }
    if (!p->state->ready.load(std::memory_order_acquire)) {
        m_device.countPsoMiss();
        add(std::format("dispatchIndirect skipped (pipeline \"{}\" not ready)", p->name));
        return;
    }
    NullBuffer* b = buffer(args, "dispatchIndirect", BufferUsage::Indirect);
    if (!b) return;
    if (offset % 4) error("dispatchIndirect: offset {} not 4-byte aligned", offset);
    checkRange(*b, offset, sizeof(DispatchIndirectCommand), "dispatchIndirect");
    add(std::format("dispatchIndirect \"{}\" offset={}", b->label, offset),
        {Use{UseKind::Buffer, args.toBits(), ResourceState::IndirectArgument, ResourceState::Count, {},
             "dispatchIndirect"}});
}

void NullCommandList::copyBuffer(BufferH src, u64 srcOffset, BufferH dst, u64 dstOffset, u64 size) {
    if (!recording("copyBuffer") || !outsideRendering("copyBuffer")) return;
    NullBuffer* s = buffer(src, "copyBuffer source", BufferUsage::TransferSrc);
    NullBuffer* d = buffer(dst, "copyBuffer destination", BufferUsage::TransferDst);
    if (!s || !d) return;
    if (size == 0) {
        error("copyBuffer: size is 0");
        return;
    }
    if (!checkRange(*s, srcOffset, size, "copyBuffer source") || !checkRange(*d, dstOffset, size, "copyBuffer dest")) {
        return;
    }
    if (src == dst && srcOffset < dstOffset + size && dstOffset < srcOffset + size) {
        error("copyBuffer: overlapping ranges within buffer '{}'", s->label);
    }
    add(std::format("copyBuffer \"{}\"+{} -> \"{}\"+{} size={}", s->label, srcOffset, d->label, dstOffset, size),
        {Use{UseKind::Buffer, src.toBits(), ResourceState::CopySource, ResourceState::Count, {}, "copyBuffer source"},
         Use{UseKind::Buffer, dst.toBits(), ResourceState::CopyDest, ResourceState::Count, {}, "copyBuffer destination"}},
        [src, dst, srcOffset, dstOffset, size](NullDeviceImpl& dev) {
            NullBuffer* sb = dev.findBuffer(src);
            NullBuffer* db = dev.findBuffer(dst);
            if (!sb || !db) return;
            const u8* from = sb->bytes(sb->desc.size);
            u8* to = db->bytes(db->desc.size);
            std::memmove(to + dstOffset, from + srcOffset, static_cast<usize>(size));
        });
}

namespace {
/// Copies texel rows between a buffer image (layout) and a texture subresource, in either direction.
void copyTexels(NullBuffer& buf, const BufferTextureLayout& layout, NullTexture& tex, const TextureRegion& r,
                bool toTexture) {
    const FormatInfo& info = formatInfo(tex.desc.format);
    const u32 mipW = mipExtent(tex.desc.width, r.mip);
    const u32 mipH = mipExtent(tex.desc.height, r.mip);
    const u64 texRowBytes = formatRowBytes(tex.desc.format, mipW);
    const u64 texRows = (mipH + info.blockHeight - 1) / info.blockHeight;
    const u64 rowBytes = formatRowBytes(tex.desc.format, r.width);
    const u64 rows = (r.height + info.blockHeight - 1) / info.blockHeight;
    const u64 bufRowPitch = layout.rowPitch ? layout.rowPitch : rowBytes;
    const u64 bufSlice = bufRowPitch * rows;
    const u64 xBytes = static_cast<u64>(r.x / info.blockWidth) * info.blockBytes;
    const u64 yRow = r.y / info.blockHeight;
    u8* bufBytes = buf.bytes(buf.desc.size);
    for (u32 l = 0; l < r.layerCount; ++l) {
        std::vector<u8>& sub = tex.subData(r.mip, r.baseLayer + l);
        for (u32 z = 0; z < r.depth; ++z) {
            for (u64 row = 0; row < rows; ++row) {
                const u64 bufOff = layout.offset + (static_cast<u64>(l) * r.depth + z) * bufSlice + row * bufRowPitch;
                const u64 texOff = ((r.z + z) * texRows + yRow + row) * texRowBytes + xBytes;
                if (bufOff + rowBytes > buf.desc.size || texOff + rowBytes > sub.size()) return;
                if (toTexture) {
                    std::memcpy(sub.data() + texOff, bufBytes + bufOff, static_cast<usize>(rowBytes));
                } else {
                    std::memcpy(bufBytes + bufOff, sub.data() + texOff, static_cast<usize>(rowBytes));
                }
            }
        }
    }
}

} // namespace

void NullCommandList::copyBufferToTexture(BufferH src, const BufferTextureLayout& layout, TextureH dst,
                                          const TextureRegion& region) {
    if (!recording("copyBufferToTexture") || !outsideRendering("copyBufferToTexture")) return;
    NullBuffer* s = buffer(src, "copyBufferToTexture source", BufferUsage::TransferSrc);
    NullTexture* t = texture(dst, "copyBufferToTexture destination", TextureUsage::TransferDst);
    if (!s || !t) return;
    u64 bytes = 0;
    const auto r = resolveRegion(*t, region, layout, bytes, "copyBufferToTexture");
    if (!r) return;
    if (!checkRange(*s, layout.offset, bytes, "copyBufferToTexture source")) return;
    add(std::format("copyBufferToTexture \"{}\" offset={} rowPitch={} -> \"{}\" mip={} layers={}+{} region={},{},{} "
                    "{}x{}x{}",
                    s->label, layout.offset, layout.rowPitch, t->label, r->mip, r->baseLayer, r->layerCount, r->x, r->y,
                    r->z, r->width, r->height, r->depth),
        {Use{UseKind::Buffer, src.toBits(), ResourceState::CopySource, ResourceState::Count, {}, "copy source"},
         Use{UseKind::Texture, dst.toBits(), ResourceState::CopyDest, ResourceState::Count,
             SubresourceRange{r->mip, 1, r->baseLayer, r->layerCount}, "copy destination"}},
        [src, dst, layout, reg = *r](NullDeviceImpl& dev) {
            NullBuffer* b = dev.findBuffer(src);
            NullTexture* tex = dev.findTexture(dst);
            if (b && tex) copyTexels(*b, layout, *tex, reg, true);
        });
}

void NullCommandList::copyTextureToBuffer(TextureH src, const TextureRegion& region, BufferH dst,
                                          const BufferTextureLayout& layout) {
    if (!recording("copyTextureToBuffer") || !outsideRendering("copyTextureToBuffer")) return;
    NullTexture* t = texture(src, "copyTextureToBuffer source", TextureUsage::TransferSrc);
    NullBuffer* d = buffer(dst, "copyTextureToBuffer destination", BufferUsage::TransferDst);
    if (!t || !d) return;
    u64 bytes = 0;
    const auto r = resolveRegion(*t, region, layout, bytes, "copyTextureToBuffer");
    if (!r) return;
    if (!checkRange(*d, layout.offset, bytes, "copyTextureToBuffer destination")) return;
    add(std::format("copyTextureToBuffer \"{}\" mip={} layers={}+{} region={},{},{} {}x{}x{} -> \"{}\" offset={} "
                    "rowPitch={}",
                    t->label, r->mip, r->baseLayer, r->layerCount, r->x, r->y, r->z, r->width, r->height, r->depth,
                    d->label, layout.offset, layout.rowPitch),
        {Use{UseKind::Texture, src.toBits(), ResourceState::CopySource, ResourceState::Count,
             SubresourceRange{r->mip, 1, r->baseLayer, r->layerCount}, "copy source"},
         Use{UseKind::Buffer, dst.toBits(), ResourceState::CopyDest, ResourceState::Count, {}, "copy destination"}},
        [src, dst, layout, reg = *r](NullDeviceImpl& dev) {
            NullBuffer* b = dev.findBuffer(dst);
            NullTexture* tex = dev.findTexture(src);
            if (b && tex) copyTexels(*b, layout, *tex, reg, false);
        });
}

void NullCommandList::fillBuffer(BufferH h, u64 offset, u64 size, u32 value) {
    if (!recording("fillBuffer") || !outsideRendering("fillBuffer")) return;
    NullBuffer* b = buffer(h, "fillBuffer", BufferUsage::TransferDst);
    if (!b) return;
    if (size == kWholeSize) size = offset <= b->desc.size ? ((b->desc.size - offset) & ~3ull) : 0;
    if (offset % 4 || size % 4 || size == 0) {
        error("fillBuffer: offset {} and size {} must be non-zero multiples of 4", offset, size);
        return;
    }
    if (!checkRange(*b, offset, size, "fillBuffer")) return;
    add(std::format("fillBuffer \"{}\" offset={} size={} value=0x{:08x}", b->label, offset, size, value),
        {Use{UseKind::Buffer, h.toBits(), ResourceState::CopyDest, ResourceState::Count, {}, "fillBuffer"}},
        [h, offset, size, value](NullDeviceImpl& dev) {
            NullBuffer* buf = dev.findBuffer(h);
            if (!buf) return;
            u8* bytes = buf->bytes(buf->desc.size);
            for (u64 i = 0; i < size; i += 4) std::memcpy(bytes + offset + i, &value, 4);
        });
}

void NullCommandList::updateBuffer(BufferH h, u64 offset, std::span<const std::byte> data) {
    if (!recording("updateBuffer") || !outsideRendering("updateBuffer")) return;
    NullBuffer* b = buffer(h, "updateBuffer", BufferUsage::TransferDst);
    if (!b) return;
    if (data.empty() || data.size() > 65536 || data.size() % 4 || offset % 4) {
        error("updateBuffer: {} bytes at offset {} (need 4-byte multiples, <= 65536 bytes)", data.size(), offset);
        return;
    }
    if (!checkRange(*b, offset, data.size(), "updateBuffer")) return;
    add(std::format("updateBuffer \"{}\" offset={} size={} data={}", b->label, offset, data.size(),
                    hexBytes(data.data(), data.size())),
        {Use{UseKind::Buffer, h.toBits(), ResourceState::CopyDest, ResourceState::Count, {}, "updateBuffer"}},
        [h, offset, bytes = std::vector<std::byte>(data.begin(), data.end())](NullDeviceImpl& dev) {
            NullBuffer* buf = dev.findBuffer(h);
            if (!buf) return;
            std::memcpy(buf->bytes(buf->desc.size) + offset, bytes.data(), bytes.size());
        });
}

void NullCommandList::clearTexture(TextureH h, const std::array<f32, 4>& color, const SubresourceRange& range) {
    if (!recording("clearTexture") || !requireQueue("clearTexture", false) || !outsideRendering("clearTexture")) return;
    NullTexture* t = texture(h, "clearTexture", TextureUsage::TransferDst);
    if (!t) return;
    if (isDepthFormat(t->desc.format) || isCompressedFormat(t->desc.format)) {
        error("clearTexture: '{}' is not a color texture", t->label);
        return;
    }
    if (range.baseMip >= t->desc.mipLevels || range.baseLayer >= t->desc.arrayLayers) {
        error("clearTexture: subresource range outside texture '{}'", t->label);
        return;
    }
    const SubresourceRange r = resolveRange(t->desc, range);
    add(std::format("clearTexture \"{}\" color=({},{},{},{}) mips={}+{} layers={}+{}", t->label, color[0], color[1],
                    color[2], color[3], r.baseMip, r.mipCount, r.baseLayer, r.layerCount),
        {Use{UseKind::Texture, h.toBits(), ResourceState::CopyDest, ResourceState::Count, r, "clearTexture"}});
}

void NullCommandList::beginLabel(std::string_view name, u32) {
    if (!recording("beginLabel")) return;
    ++m_labelDepth;
    add(std::format("beginLabel \"{}\"", name));
}

void NullCommandList::endLabel() {
    if (!recording("endLabel")) return;
    if (m_labelDepth == 0) {
        error("endLabel without beginLabel");
        return;
    }
    --m_labelDepth;
    add("endLabel");
}

void NullCommandList::insertLabel(std::string_view name, u32) {
    if (!recording("insertLabel")) return;
    add(std::format("insertLabel \"{}\"", name));
}

void NullCommandList::breadcrumb(u16 passId, BreadcrumbStage stage) {
    if (!recording("breadcrumb") || !outsideRendering("breadcrumb")) return;
    const u32 value = static_cast<u32>((m_device.currentFrame() & 0xFFFFu) << 16) | passId;
    const Queue queue = m_queue;
    add(std::format("breadcrumb pass={} {}", passId, stage == BreadcrumbStage::Begin ? "Begin" : "End"), {},
        [queue, value, stage](NullDeviceImpl& dev) { dev.setBreadcrumb(queue, value, stage); });
}

} // namespace

AdapterInfo nullAdapterInfo() {
    AdapterInfo info;
    info.index = 0;
    info.name = "Helios Null Device";
    info.type = AdapterType::Other;
    info.driverName = "null";
    info.driverInfo = "Helios Null RHI";
    info.apiVersion = (1u << 22) | (3u << 12);
    info.meetsRequirements = true;
    return info;
}

Result<std::unique_ptr<Device>> createNullDevice(const DeviceDesc& desc) {
    if (desc.framesInFlight == 0) return Error{ErrorCode::InvalidArgument, "framesInFlight must be >= 1"};
    return std::unique_ptr<Device>(std::make_unique<NullDeviceImpl>(desc));
}

} // namespace helios::rhi::detail
