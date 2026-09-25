#pragma once
// helios::rhi::Device — the backend-agnostic GPU device (03 §1.1).
//
//   auto device = rhi::Device::create({.backend = rhi::Backend::Vulkan});
//   rhi::BufferH buf = device->createBuffer({.size = 4096, .usage = BufferUsage::Storage}).value();
//   rhi::CommandList* cmd = device->acquireCommandList(rhi::Queue::Graphics, "Frame");
//   ... record ...
//   rhi::TimelinePoint done = device->submit(rhi::Queue::Graphics, {&cmd, 1}).value();
//   device->wait(done);
//
// Threading: every method is thread-safe unless its comment says otherwise. The exceptions are the
// frame-boundary operations (beginFrame, waitIdle, resizeSwapchain, destruction), which must not
// run concurrently with recording or submission on other threads.
//
// Lifetimes: destroy() is deferred until every submission of the current frame has completed on
// the GPU, so a resource may be destroyed right after the last list that uses it is submitted.
// Handles become stale immediately. Command lists recycle at beginFrame() (see command_list.h).
//
// Bindless: sampled textures and storage buffers get a slot in the global descriptor heap at
// creation (srv()); storage-image mips (uav()) and extra views get one on first request. Slots are
// released with the resource. Samplers are deduplicated and live as long as the device.

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/rhi/caps.h"
#include "helios/rhi/command_list.h"
#include "helios/rhi/handles.h"
#include "helios/rhi/types.h"

namespace helios::jobs {
class BackgroundPool;
}

namespace helios::rhi {

enum class AdapterPreference : u8 { HighPerformance, LowPower, Software };

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    std::string_view appName = "Helios";
    /// Enable Khronos validation when the layer is installed (env HELIOS_RHI_VALIDATION=1 forces
    /// on, =0 off). requireValidation fails creation when the layer is missing.
    bool validation = false;
    bool requireValidation = false;
    /// Object names and labels via VK_EXT_debug_utils when available.
    bool debugNames = true;
    AdapterPreference adapterPreference = AdapterPreference::HighPerformance;
    /// Explicit adapter (index from enumerateAdapters), -1 = pick by preference. The environment
    /// variable HELIOS_RHI_ADAPTER (index or case-insensitive name substring) overrides both.
    i32 adapterIndex = -1;
    /// Enable surface/swapchain extensions (off for purely headless tools).
    bool enableSwapchain = true;
    /// Frames the CPU may run ahead of the GPU (per-frame command pools, deferred deletion).
    u32 framesInFlight = 2;
    /// Optional capabilities to keep (CapBit mask); cleared bits are reported and treated as absent.
    CapBit capsMask = static_cast<CapBit>(~0ull);
    /// Pool for PsoPriority != Immediate compiles; null compiles synchronously.
    jobs::BackgroundPool* pipelineCompilePool = nullptr;
    /// Serialized pipeline cache from a previous run (pipelineCacheData()); ignored if stale.
    std::span<const u8> pipelineCacheData;
    /// Validation / driver / Null-backend messages (any thread). Errors are also counted.
    /// Callbacks may run while the device holds internal locks: they must not call into the Device.
    std::function<void(const ValidationMessage&)> onMessage;
    /// Called once when the GPU is lost, before the failing call returns (03 §1.5). Same rule.
    std::function<void(const DeviceLostInfo&)> onDeviceLost;
    /// Fault injection for testing device-loss handling (03 §1.5, RC-6): the Nth submit executes,
    /// then reports the device as lost. 0 = off. Also set by HELIOS_RHI_INJECT_DEVICE_LOST=N.
    u32 debugDeviceLostAfterSubmits = 0;
};

class Device {
public:
    /// Creates a device for desc.backend. Fails with Unsupported when the backend (or a device
    /// meeting Helios' requirements) is unavailable.
    static Result<std::unique_ptr<Device>> create(const DeviceDesc& desc);
    /// Adapters of a backend, including ones that fail the requirements (see AdapterInfo).
    static Result<std::vector<AdapterInfo>> enumerateAdapters(Backend backend);

    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Backend backend() const noexcept { return caps().backend; }
    /// Features, limits and adapter info (immutable after creation).
    virtual const Caps& caps() const noexcept = 0;

    // -- Resources ----------------------------------------------------------------------------
    virtual Result<BufferH> createBuffer(const BufferDesc& desc) = 0;
    virtual Result<TextureH> createTexture(const TextureDesc& desc) = 0;
    /// Deferred destruction (see header comment); null/stale handles are ignored.
    virtual void destroy(BufferH buffer) = 0;
    virtual void destroy(TextureH texture) = 0;
    virtual void destroy(PipelineH pipeline) = 0;
    virtual void destroy(SwapchainH swapchain) = 0;

    /// Creation parameters. `name` views the device's copy and stays valid until the resource is
    /// destroyed. Null/stale handle -> default-constructed desc with size/width 0.
    virtual BufferDesc bufferDesc(BufferH buffer) const = 0;
    virtual TextureDesc textureDesc(TextureH texture) const = 0;

    /// Persistently mapped pointer of an Upload/Readback buffer; nullptr for GpuOnly or stale.
    virtual void* map(BufferH buffer) = 0;
    /// Make CPU writes visible to the GPU / GPU writes visible to the CPU for non-coherent memory
    /// (no-ops on coherent heaps). Call after writing / before reading mapped memory.
    virtual void flushMapped(BufferH buffer, u64 offset = 0, u64 size = kWholeSize) = 0;
    virtual void invalidateMapped(BufferH buffer, u64 offset = 0, u64 size = kWholeSize) = 0;

    // -- Bindless -----------------------------------------------------------------------------
    /// Sampled-image slot of a Sampled texture (whole resource with {} , or a sub-view).
    virtual BindlessIndex srv(TextureH texture, const ViewDesc& view = {}) = 0;
    /// Storage-image slot of one mip of a Storage texture (all layers).
    virtual BindlessIndex uav(TextureH texture, u32 mip = 0) = 0;
    /// Storage-buffer slot of a Storage buffer (read/write RWByteAddressBuffer in shaders).
    virtual BindlessIndex srv(BufferH buffer) = 0;
    /// GPU virtual address (buffer device address); 0 for null/stale handles.
    virtual u64 deviceAddress(BufferH buffer) = 0;
    /// Deduplicated sampler slot; identical descs return the same index.
    virtual BindlessIndex sampler(const SamplerDesc& desc) = 0;

    // -- Pipelines ----------------------------------------------------------------------------
    virtual Result<PipelineH> createGraphicsPipeline(const GraphicsPipelineDesc& desc,
                                                     PsoPriority priority = PsoPriority::Immediate) = 0;
    virtual Result<PipelineH> createComputePipeline(const ComputePipelineDesc& desc,
                                                    PsoPriority priority = PsoPriority::Immediate) = 0;
    /// True once the pipeline compiled successfully. A failed async compile stays false forever and
    /// is reported through onMessage.
    virtual bool isReady(PipelineH pipeline) const = 0;
    /// Serialized pipeline cache (VkPipelineCache data keyed by device UUID + driver version).
    virtual std::vector<u8> pipelineCacheData() const = 0;

    // -- Commands and synchronization -----------------------------------------------------------
    /// A list in recording state from the calling thread's pool for the current frame. `name`
    /// labels the list in traces and tools. Never null unless the device is lost.
    virtual CommandList* acquireCommandList(Queue queue, std::string_view name = {}) = 0;
    /// Executes lists in order after `waits` complete and signals the queue's next timeline value.
    /// Lists still recording are closed first (only on their recording thread). A list from another
    /// device or queue, already submitted, recycled (acquired before the last beginFrame/waitIdle),
    /// listed twice, or still open on another thread, and a wait for a value never submitted, are
    /// validation errors: the Vulkan backend rejects the whole submit (InvalidArgument, nothing
    /// executes); the Null backend reports them and skips the offending lists so its trace still
    /// captures the rest.
    virtual Result<TimelinePoint> submit(Queue queue, std::span<CommandList* const> lists,
                                         std::span<const TimelinePoint> waits = {}) = 0;
    /// Highest completed value on `queue`.
    virtual u64 completedValue(Queue queue) const = 0;
    bool isComplete(TimelinePoint point) const { return completedValue(point.queue) >= point.value; }
    /// Blocks until `point` completes or `timeoutNs` elapses (Timeout error). Null points return
    /// immediately.
    virtual Result<void> wait(TimelinePoint point, u64 timeoutNs = ~0ull) = 0;
    /// Waits for every queue, recycles all command lists and runs deferred deletions. Not
    /// concurrent with recording/submission.
    virtual Result<void> waitIdle() = 0;

    /// Starts the next frame: waits until the frame that last used this frame slot has finished
    /// on the GPU (at most framesInFlight frames in flight), recycles its command lists and runs
    /// deferred deletions that are safe. Not concurrent with recording/submission.
    virtual Result<void> beginFrame() = 0;
    /// Frames begun so far (0 before the first beginFrame).
    virtual u64 frameIndex() const noexcept = 0;

    // -- Swapchains ---------------------------------------------------------------------------
    virtual Result<SwapchainH> createSwapchain(const SwapchainDesc& desc) = 0;
    /// Acquires the next image. status OutOfDate (no image) means resizeSwapchain() first.
    virtual Result<SwapchainImage> acquireNextImage(SwapchainH swapchain) = 0;
    /// Presents the last acquired image once `after` completes. The image must be in the Present
    /// state by then.
    virtual Result<SwapchainStatus> present(SwapchainH swapchain, TimelinePoint after) = 0;
    /// Recreates the images for a new size (0 = query the surface). Waits for the GPU; not
    /// concurrent with recording/submission.
    virtual Result<void> resizeSwapchain(SwapchainH swapchain, u32 width = 0, u32 height = 0) = 0;
    virtual SwapchainInfo swapchainInfo(SwapchainH swapchain) const = 0;

    // -- Diagnostics --------------------------------------------------------------------------
    virtual MemoryStats memoryStats() const = 0;
    /// Last breadcrumbs each queue's GPU work reached (03 §1.5).
    virtual std::array<BreadcrumbState, kQueueCount> breadcrumbs() const = 0;
    /// Validation errors reported so far (Khronos layer, driver, Null validation).
    virtual u64 validationErrorCount() const noexcept = 0;
    virtual bool isDeviceLost() const noexcept = 0;

protected:
    Device() = default;
};

} // namespace helios::rhi
