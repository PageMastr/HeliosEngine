#pragma once
// Vulkan 1.3 device (volk device table + VMA). See helios/rhi/device.h for the contract.
//
// Structure:
//   * vk_device.cpp        instance/adapter/device creation, resources, bindless heap, pipelines,
//                          submission, frames, deferred deletion, diagnostics
//   * vk_command_list.cpp  command recording
//   * vk_swapchain.cpp     SDL3 surfaces, swapchains, acquire/present
//
// Synchronization model: one timeline semaphore per *logical* queue (Graphics, AsyncCompute,
// Transfer). Logical queues without a dedicated hardware queue alias the graphics VkQueue and share
// its submit mutex (VkQueue is externally synchronized). Swapchain binary semaphores are bridged to
// the graphics timeline with empty submits, so users only ever deal with TimelinePoints.
//
// Function pointers: every device owns its own VkInstance, so instance-level commands go through a
// per-device VolkInstanceTable (m_vki) and device commands through a VolkDeviceTable (m_vk). volk's
// process-global instance pointers are never loaded: they can only describe one instance, and a
// second device (or enumerateAdapters) would silently rebind them for everyone else — with
// different extensions enabled that leaves NULL surface/debug-utils entry points behind.

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../rhi_internal.h"
#include "vk_common.h"

namespace helios::rhi::vk {

using detail::LockedPool;
using detail::SlotAllocator;

class VulkanDevice;
class VulkanCommandList;

struct BufferRes {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    u64 address = 0;
    u64 allocationSize = 0;
    BindlessIndex storageIndex = kInvalidBindless;
    BufferDesc desc;
    std::string name;
};

struct TextureRes {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;  // null for swapchain images
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    u64 allocationSize = 0;
    TextureDesc desc;
    std::string name;
    bool swapchainImage = false;
    VkImageView srvView = VK_NULL_HANDLE;
    BindlessIndex srvIndex = kInvalidBindless;
    // Lazily created views (guarded by VulkanDevice::m_viewMutex).
    std::vector<std::pair<ViewDesc, std::pair<VkImageView, BindlessIndex>>> views;
    std::vector<std::pair<VkImageView, BindlessIndex>> uavs;  // per mip
    std::vector<VkImageView> attachmentViews;                  // per mip * layers
};

/// Shared with async compile jobs so a pipeline can be destroyed while its compile is in flight.
struct PipelineState {
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
    std::atomic<int> status{0};  // 0 pending, 1 ready, 2 failed
};

struct PipelineRes {
    std::shared_ptr<PipelineState> state;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::string name;
};

struct SwapchainRes {
    std::string name;
    void* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    SwapchainInfo info;
    Format requestedFormat = Format::BGRA8Srgb;
    PresentMode requestedMode = PresentMode::Fifo;
    u32 requestedImageCount = 3;
    std::vector<TextureH> images;
    std::vector<VkSemaphore> acquireSemaphores;    // ring, imageCount + 1
    std::vector<u64> acquireSemaphoreValues;        // graphics value of the bridge that consumed it
    std::vector<VkSemaphore> presentSemaphores;    // per image
    u32 acquireCursor = 0;
    u32 currentImage = ~0u;
    bool zeroExtent = false;  // minimized: acquire reports OutOfDate until resized
};

struct QueueSlot {
    VkQueue queue = VK_NULL_HANDLE;
    u32 family = 0;
    u32 indexInFamily = 0;
    VkQueueFlags flags = 0;
    std::mutex* submitMutex = nullptr;  // shared by aliased logical queues
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::atomic<u64> submitted{0};  // last value handed to vkQueueSubmit2
};

struct CommandPoolSet {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<VulkanCommandList>> lists;
    u32 used = 0;

    /// Resets the pool (its command buffers must not be pending) and marks the lists recycled.
    void reset(const VolkDeviceTable& vk, VkDevice device);
};

struct ThreadContext {
    std::array<CommandPoolSet, kQueueCount> perQueue;
};

struct FrameSlot {
    std::vector<std::unique_ptr<ThreadContext>> threads;
    std::array<u64, kQueueCount> lastSubmitted{};
};

struct GarbageBatch {
    std::array<u64, kQueueCount> values{};
    std::vector<std::function<void()>> items;
};

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceDesc& desc);
    ~VulkanDevice() override;

    Result<void> init();

    // Device ------------------------------------------------------------------------------------
    const Caps& caps() const noexcept override { return m_caps; }
    Result<BufferH> createBuffer(const BufferDesc& desc) override;
    Result<TextureH> createTexture(const TextureDesc& desc) override;
    void destroy(BufferH buffer) override;
    void destroy(TextureH texture) override;
    void destroy(PipelineH pipeline) override;
    void destroy(SwapchainH swapchain) override;
    BufferDesc bufferDesc(BufferH buffer) const override;
    TextureDesc textureDesc(TextureH texture) const override;
    void* map(BufferH buffer) override;
    void flushMapped(BufferH buffer, u64 offset, u64 size) override;
    void invalidateMapped(BufferH buffer, u64 offset, u64 size) override;
    BindlessIndex srv(TextureH texture, const ViewDesc& view) override;
    BindlessIndex uav(TextureH texture, u32 mip) override;
    BindlessIndex srv(BufferH buffer) override;
    u64 deviceAddress(BufferH buffer) override;
    BindlessIndex sampler(const SamplerDesc& desc) override;
    Result<PipelineH> createGraphicsPipeline(const GraphicsPipelineDesc& desc, PsoPriority priority) override;
    Result<PipelineH> createComputePipeline(const ComputePipelineDesc& desc, PsoPriority priority) override;
    bool isReady(PipelineH pipeline) const override;
    std::vector<u8> pipelineCacheData() const override;
    CommandList* acquireCommandList(Queue queue, std::string_view name) override;
    Result<TimelinePoint> submit(Queue queue, std::span<CommandList* const> lists,
                                 std::span<const TimelinePoint> waits) override;
    u64 completedValue(Queue queue) const override;
    Result<void> wait(TimelinePoint point, u64 timeoutNs) override;
    Result<void> waitIdle() override;
    Result<void> beginFrame() override;
    u64 frameIndex() const noexcept override { return m_frameIndex.load(std::memory_order_relaxed); }
    Result<SwapchainH> createSwapchain(const SwapchainDesc& desc) override;
    Result<SwapchainImage> acquireNextImage(SwapchainH swapchain) override;
    Result<SwapchainStatus> present(SwapchainH swapchain, TimelinePoint after) override;
    Result<void> resizeSwapchain(SwapchainH swapchain, u32 width, u32 height) override;
    SwapchainInfo swapchainInfo(SwapchainH swapchain) const override;
    MemoryStats memoryStats() const override;
    std::array<BreadcrumbState, kQueueCount> breadcrumbs() const override;
    u64 validationErrorCount() const noexcept override { return m_validationErrors.load(std::memory_order_relaxed); }
    bool isDeviceLost() const noexcept override { return m_deviceLost.load(std::memory_order_acquire); }

    // Internal (command lists, swapchains) ------------------------------------------------------
    const VolkDeviceTable& vkt() const noexcept { return m_vk; }
    const VolkInstanceTable& vki() const noexcept { return m_vki; }
    VkDevice device() const noexcept { return m_device; }
    VkPipelineLayout pipelineLayout() const noexcept { return m_pipelineLayout; }
    VkDescriptorSet bindlessSet() const noexcept { return m_bindlessSet; }
    const QueueSlot& queueSlot(Queue q) const noexcept { return m_queues[queueIndex(q)]; }
    bool debugUtils() const noexcept { return m_debugUtils; }
    BufferRes* findBuffer(BufferH h) const { return m_buffers.get(h); }
    TextureRes* findTexture(TextureH h) const { return m_textures.get(h); }
    PipelineRes* findPipeline(PipelineH h) const { return m_pipelines.get(h); }
    /// View for rendering into (mip, layer) of a texture; created on first use.
    VkImageView attachmentView(TextureRes& texture, u32 mip, u32 layer);
    VkBuffer breadcrumbBuffer() const noexcept { return m_breadcrumbBuffer; }
    void countPsoMiss() noexcept { m_psoMisses.fetch_add(1, std::memory_order_relaxed); }
    void setObjectName(VkObjectType type, u64 handle, std::string_view name) const;
    /// Reports an RHI usage error (counted as a validation error).
    void reportError(std::string message);
    /// Handles VK_ERROR_DEVICE_LOST (once) and builds the error returned to the caller.
    Error deviceLost(std::string_view where);
    /// Error for any other failing VkResult (device loss is detected and routed to deviceLost()).
    Error vkError(VkResult result, std::string_view where);

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                        void* userData);

private:
    friend class VulkanCommandList;

    Result<void> createInstance();
    Result<void> selectPhysicalDevice();
    Result<void> createLogicalDevice();
    Result<void> createAllocator();
    Result<void> createBindlessHeap();
    Result<void> createDefaultResources();
    void fillCaps();

    Result<VkImageView> createView(const TextureRes& texture, VkImageViewType type, u32 baseMip, u32 mipCount,
                                   u32 baseLayer, u32 layerCount, bool storage);
    void writeSampledImage(BindlessIndex index, VkImageView view);
    void writeStorageImage(BindlessIndex index, VkImageView view);
    void writeStorageBuffer(BindlessIndex index, VkBuffer buffer, u64 size);
    /// Locks every distinct hardware queue (fixed order): vkDeviceWaitIdle and fault injection
    /// need all VkQueues externally synchronized.
    std::vector<std::unique_lock<std::mutex>> lockAllQueues();
    Result<void> deviceWaitIdle(std::string_view where);
    void writeSampler(BindlessIndex index, VkSampler sampler);
    Result<VkPipeline> buildGraphicsPipeline(const GraphicsPipelineDesc& desc);
    Result<VkPipeline> buildComputePipeline(const ComputePipelineDesc& desc);
    Result<VkShaderModule> createShaderModule(std::span<const u32> spirv, std::string_view name);
    Result<void> immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

    void defer(std::function<void()> item);
    void sealGarbage();
    void collectGarbage(bool all);
    void destroyTextureNow(TextureRes& texture);
    std::array<u64, kQueueCount> submittedValues() const;
    std::array<u64, kQueueCount> completedValues() const;
    Result<void> waitValues(const std::array<u64, kQueueCount>& values, u64 timeoutNs);
    u32 threadIndex();

    // Swapchain helpers (vk_swapchain.cpp)
    Result<void> buildSwapchain(SwapchainRes& sc, u32 width, u32 height);
    void releaseSwapchainImages(SwapchainRes& sc);

    DeviceDesc m_desc;
    Caps m_caps;

    VkInstance m_instance = VK_NULL_HANDLE;
    VolkInstanceTable m_vki{};
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VolkDeviceTable m_vk{};
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    bool m_debugUtils = false;
    bool m_validationLayer = false;
    bool m_surfaceExtensions = false;
    bool m_swapchainExtension = false;
    bool m_memoryBudget = false;
    bool m_deviceFault = false;
    bool m_deviceFaultVendorBinary = false;
    std::vector<std::string> m_enabledDeviceExtensions;
    VkPhysicalDeviceProperties m_properties{};
    std::vector<VkQueueFamilyProperties> m_queueFamilies;

    std::array<QueueSlot, kQueueCount> m_queues;
    std::array<std::mutex, kQueueCount> m_queueMutexes;

    // Bindless heap
    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::mutex m_descriptorMutex;
    SlotAllocator m_sampledSlots;
    SlotAllocator m_storageImageSlots;
    SlotAllocator m_storageBufferSlots;
    std::mutex m_samplerMutex;
    std::vector<std::pair<SamplerDesc, VkSampler>> m_samplers;  // index == bindless slot
    std::mutex m_viewMutex;

    // Default resources behind slot 0 of every array. Released slots are pointed back at them, so a
    // stale index reads the default instead of a destroyed view/buffer.
    TextureH m_defaultTexture;
    TextureH m_defaultStorageImage;
    BufferH m_defaultBuffer;
    VkImageView m_defaultSampledView = VK_NULL_HANDLE;
    VkImageView m_defaultStorageView = VK_NULL_HANDLE;
    VkBuffer m_defaultVkBuffer = VK_NULL_HANDLE;
    u64 m_maxStorageBufferRange = 0;
    bool m_tearingDown = false;

    // Breadcrumbs: [queue][Begin, End] u32 values in host-visible memory.
    VkBuffer m_breadcrumbBuffer = VK_NULL_HANDLE;
    VmaAllocation m_breadcrumbAllocation = VK_NULL_HANDLE;
    const volatile u32* m_breadcrumbData = nullptr;

    LockedPool<BufferRes, BufferTag> m_buffers;
    LockedPool<TextureRes, TextureTag> m_textures;
    LockedPool<PipelineRes, PipelineTag> m_pipelines;
    mutable std::mutex m_swapchainMutex;
    HandlePool<SwapchainRes, SwapchainTag> m_swapchains;

    // Frames and command pools
    std::mutex m_contextMutex;
    std::unordered_map<std::thread::id, u32> m_threadIndices;
    std::vector<FrameSlot> m_frames;
    std::atomic<u64> m_frameIndex{0};

    // Deferred deletion
    std::mutex m_garbageMutex;
    std::vector<std::function<void()>> m_pendingGarbage;
    std::deque<GarbageBatch> m_sealedGarbage;

    // Stats and diagnostics
    std::atomic<u64> m_bufferBytes{0};
    std::atomic<u64> m_textureBytes{0};
    std::atomic<u32> m_pipelineCount{0};
    std::atomic<u64> m_psoMisses{0};
    std::atomic<u64> m_validationErrors{0};
    std::atomic<bool> m_deviceLost{false};
    std::atomic<u32> m_pendingCompiles{0};
    std::atomic<u32> m_submitCount{0};  // fault injection (DeviceDesc::debugDeviceLostAfterSubmits)
};

class VulkanCommandList final : public CommandList {
public:
    VulkanCommandList(VulkanDevice& device, Queue queue, VkCommandBuffer cmd) noexcept
        : CommandList(queue, static_cast<const Device*>(&device)), m_device(device), m_vk(device.vkt()), m_cmd(cmd) {}

    void begin(std::string_view name, u64 frame);
    bool isOpen() const noexcept { return m_open; }
    bool isSubmitted() const noexcept { return m_submitted; }
    void markSubmitted() noexcept { m_submitted = true; }
    /// Its pool was reset: the command buffer is back in the initial state and must not be used.
    bool isRecycled() const noexcept { return m_recycled; }
    void markRecycled() noexcept {
        m_recycled = true;
        m_open = false;
        m_inRendering = false;
    }
    u64 acquireFrame() const noexcept { return m_acquireFrame; }
    std::thread::id owner() const noexcept { return m_owner; }
    VkCommandBuffer handle() const noexcept { return m_cmd; }
    const std::string& name() const noexcept { return m_name; }

    void end() override;
    void barrier(std::span<const Barrier> barriers) override;
    void beginRendering(const RenderingDesc& desc) override;
    void endRendering() override;
    void setViewport(const Viewport& viewport) override;
    void setScissor(const Rect& scissor) override;
    void bindPipeline(PipelineH pipeline) override;
    void pushConstants(const void* data, u32 bytes, u32 offset) override;
    void bindIndexBuffer(BufferH buffer, u64 offset, IndexType type) override;
    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) override;
    void drawIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) override;
    void drawIndexedIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) override;
    void drawIndexedIndirectCount(BufferH args, u64 offset, BufferH count, u64 countOffset, u32 maxDraws,
                                  u32 stride) override;
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
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

private:
    bool check(bool condition, std::string_view what);
    /// Every command starts with this: recording into an ended/submitted/recycled command buffer is
    /// undefined behavior in Vulkan, so such commands are reported and dropped.
    bool recording(std::string_view op);
    bool pipelineUsable(VkPipelineBindPoint point, std::string_view op);
    const BufferRes* buffer(BufferH h, std::string_view op, BufferUsage usage);
    TextureRes* texture(TextureH h, std::string_view op, TextureUsage usage);
    bool bufferRange(const BufferRes& b, u64 offset, u64 size, std::string_view op);
    VkBufferImageCopy2 bufferImageCopy(const TextureRes& texture, const BufferTextureLayout& layout,
                                       const TextureRegion& region) const;

    VulkanDevice& m_device;
    const VolkDeviceTable& m_vk;
    VkCommandBuffer m_cmd;
    std::string m_name;
    std::thread::id m_owner;
    u64 m_acquireFrame = 0;
    bool m_open = false;
    bool m_submitted = false;
    bool m_recycled = false;
    bool m_inRendering = false;
    bool m_hasIndexBuffer = false;
    bool m_pipelineReady = false;
    VkPipelineBindPoint m_boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
    i32 m_labelDepth = 0;
};

} // namespace helios::rhi::vk
