// Vulkan command recording. Misuse that would be undefined behavior in the driver — recording into
// an ended/submitted/recycled list, stale handles, draws without a pipeline or index buffer,
// rendering-scope violations, out-of-range copies/fills/updates/indirect reads, commands a queue
// cannot execute — is reported (counted as a validation error) and the command is skipped. A
// software driver such as lavapipe executes transfers with plain memcpy, so an unchecked range
// would corrupt host memory rather than fail. Resource *state* correctness is the caller's (render
// graph's) responsibility; the Null backend and the Khronos validation layer check it.

#include <algorithm>
#include <format>

#include "helios/core/containers.h"
#include "vk_device.h"

namespace helios::rhi::vk {

bool VulkanCommandList::check(bool condition, std::string_view what) {
    if (!condition) m_device.reportError(std::format("list '{}': {}", m_name, what));
    return condition;
}

bool VulkanCommandList::recording(std::string_view op) {
    if (!m_open) {
        m_device.reportError(std::format("list '{}': {} recorded into a list that is not recording (ended, submitted "
                                         "or recycled)",
                                         m_name, op));
        return false;
    }
    if (std::this_thread::get_id() != m_owner) {
        // Pools are per thread: recording from another thread races with the owner's use of the pool.
        m_device.reportError(std::format("list '{}': {} recorded from a thread other than the one that acquired it",
                                         m_name, op));
    }
    return true;
}

const BufferRes* VulkanCommandList::buffer(BufferH h, std::string_view op, BufferUsage usage) {
    const BufferRes* b = m_device.findBuffer(h);
    if (!b) {
        m_device.reportError(std::format("list '{}': {}: null or stale buffer handle", m_name, op));
        return nullptr;
    }
    if (usage != BufferUsage::None && !hasFlag(b->desc.usage, usage)) {
        m_device.reportError(std::format("list '{}': {}: buffer '{}' lacks the required usage flag", m_name, op, b->name));
        return nullptr;
    }
    return b;
}

TextureRes* VulkanCommandList::texture(TextureH h, std::string_view op, TextureUsage usage) {
    TextureRes* t = m_device.findTexture(h);
    if (!t) {
        m_device.reportError(std::format("list '{}': {}: null or stale texture handle", m_name, op));
        return nullptr;
    }
    if (usage != TextureUsage::None && !hasFlag(t->desc.usage, usage)) {
        m_device.reportError(std::format("list '{}': {}: texture '{}' lacks the required usage flag", m_name, op, t->name));
        return nullptr;
    }
    return t;
}

bool VulkanCommandList::bufferRange(const BufferRes& b, u64 offset, u64 size, std::string_view op) {
    if (detail::rangeFits(b.desc.size, offset, size)) return true;
    m_device.reportError(std::format("list '{}': {}: range {}+{} exceeds buffer '{}' ({} bytes)", m_name, op, offset,
                                     size, b.name, b.desc.size));
    return false;
}

void VulkanCommandList::begin(std::string_view name, u64 frame) {
    m_name = name.empty() ? std::string("list") : std::string(name);
    m_owner = std::this_thread::get_id();
    m_acquireFrame = frame;
    m_open = true;
    m_submitted = false;
    m_recycled = false;
    m_inRendering = false;
    m_hasIndexBuffer = false;
    m_pipelineReady = false;
    m_boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
    m_labelDepth = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult r = m_vk.vkBeginCommandBuffer(m_cmd, &bi);
    if (r != VK_SUCCESS) {
        m_open = false;  // every command will be reported and dropped
        m_device.reportError(std::format("vkBeginCommandBuffer('{}') failed: {}", m_name, resultName(r)));
        return;
    }
    m_device.setObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<u64>(m_cmd), m_name);
    // The bindless heap is bound once per list; one pipeline layout serves every pipeline, so the
    // binding survives all pipeline changes.
    const VkQueueFlags flags = m_device.queueSlot(m_queue).flags;
    const VkDescriptorSet set = m_device.bindlessSet();
    if (flags & VK_QUEUE_GRAPHICS_BIT) {
        m_vk.vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_device.pipelineLayout(), 0, 1, &set, 0,
                                     nullptr);
    }
    if (flags & VK_QUEUE_COMPUTE_BIT) {
        m_vk.vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_device.pipelineLayout(), 0, 1, &set, 0,
                                     nullptr);
    }
}

void VulkanCommandList::end() {
    if (!check(m_open, "end() on a list that is not recording")) return;
    if (!check(!m_inRendering, "list ended inside beginRendering (missing endRendering)")) endRendering();
    if (!check(m_labelDepth == 0, "list ended with unbalanced beginLabel calls")) {
        const auto endLabelFn = m_device.vki().vkCmdEndDebugUtilsLabelEXT;
        while (m_labelDepth-- > 0) {
            if (endLabelFn && m_device.debugUtils()) endLabelFn(m_cmd);
        }
        m_labelDepth = 0;
    }
    const VkResult r = m_vk.vkEndCommandBuffer(m_cmd);
    if (r != VK_SUCCESS) m_device.reportError(std::format("vkEndCommandBuffer('{}') failed: {}", m_name, resultName(r)));
    m_open = false;
}

// -------------------------------------------------------------------------------------------------
// Barriers
// -------------------------------------------------------------------------------------------------
void VulkanCommandList::barrier(std::span<const Barrier> barriers) {
    if (!recording("barrier") || !check(!m_inRendering, "barrier inside rendering")) return;
    const VkQueueFlags qflags = m_device.queueSlot(m_queue).flags;
    SmallVector<VkMemoryBarrier2, 4> memory;
    SmallVector<VkBufferMemoryBarrier2, 8> buffers;
    SmallVector<VkImageMemoryBarrier2, 8> images;
    for (const Barrier& b : barriers) {
        const StateInfo src = stateInfo(b.before);
        const StateInfo dst = stateInfo(b.after);
        // Sources with no pipeline stage of their own still order the transition after earlier work:
        // Undefined (discard, not race) and Present, whose image was released by the presentation
        // engine through the acquire semaphore — the barrier must chain with that wait (waited at
        // ALL_COMMANDS), or the layout transition could run before the engine is done reading.
        const bool noSourceStage = b.before == ResourceState::Undefined || b.before == ResourceState::Present;
        const VkPipelineStageFlags2 srcStages =
            maskStages(noSourceStage ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : src.stages, qflags);
        // Discarding contents is still a write after whatever wrote them before (e.g. memory reused
        // by a new resource): write-after-write needs those writes made available, not just ordered.
        const VkAccessFlags2 srcAccess =
            b.before == ResourceState::Undefined ? VK_ACCESS_2_MEMORY_WRITE_BIT : src.access;
        const VkPipelineStageFlags2 dstStages =
            maskStages(dst.stages == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT : dst.stages,
                       qflags);
        switch (b.kind) {
        case Barrier::Kind::Global: {
            VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m.srcStageMask = srcStages;
            m.srcAccessMask = srcAccess;
            m.dstStageMask = dstStages;
            m.dstAccessMask = dst.access;
            memory.emplace_back(m);
            break;
        }
        case Barrier::Kind::Buffer: {
            const BufferRes* res = buffer(b.buffer, "barrier", BufferUsage::None);
            if (!res) break;
            if (b.size != kWholeSize && !bufferRange(*res, b.offset, b.size, "barrier")) break;
            if (b.size == kWholeSize && !check(b.offset < res->desc.size, "barrier: buffer offset out of range")) break;
            VkBufferMemoryBarrier2 m{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            m.srcStageMask = srcStages;
            m.srcAccessMask = srcAccess;
            m.dstStageMask = dstStages;
            m.dstAccessMask = dst.access;
            m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.buffer = res->buffer;
            m.offset = b.offset;
            m.size = b.size == kWholeSize ? VK_WHOLE_SIZE : b.size;
            buffers.emplace_back(m);
            break;
        }
        case Barrier::Kind::Texture: {
            const TextureRes* res = texture(b.texture, "barrier", TextureUsage::None);
            if (!res) break;
            const SubresourceRange r = detail::resolveRange(res->desc, b.range);
            if (!check(b.range.baseMip < res->desc.mipLevels && b.range.baseLayer < res->desc.arrayLayers &&
                           r.mipCount > 0 && r.layerCount > 0,
                       "barrier: subresource range outside the texture") ||
                !check(b.after != ResourceState::Undefined, "barrier: a texture cannot transition to Undefined")) {
                break;
            }
            VkImageMemoryBarrier2 m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            m.srcStageMask = srcStages;
            m.srcAccessMask = srcAccess;
            m.dstStageMask = dstStages;
            m.dstAccessMask = dst.access;
            m.oldLayout = src.layout;
            m.newLayout = dst.layout;
            m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.image = res->image;
            m.subresourceRange = {res->aspect, r.baseMip, r.mipCount, r.baseLayer, r.layerCount};
            images.emplace_back(m);
            break;
        }
        }
    }
    if (memory.empty() && buffers.empty() && images.empty()) return;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = static_cast<u32>(memory.size());
    dep.pMemoryBarriers = memory.data();
    dep.bufferMemoryBarrierCount = static_cast<u32>(buffers.size());
    dep.pBufferMemoryBarriers = buffers.data();
    dep.imageMemoryBarrierCount = static_cast<u32>(images.size());
    dep.pImageMemoryBarriers = images.data();
    m_vk.vkCmdPipelineBarrier2(m_cmd, &dep);
}

// -------------------------------------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------------------------------------
namespace {
VkClearColorValue clearValueFor(Format format, const std::array<f32, 4>& c) {
    VkClearColorValue v{};
    const FormatKind kind = formatInfo(format).kind;
    if (kind == FormatKind::Uint || kind == FormatKind::Sint) {
        for (int i = 0; i < 4; ++i) v.uint32[i] = static_cast<u32>(std::max(c[static_cast<usize>(i)], 0.0f));
    } else {
        for (int i = 0; i < 4; ++i) v.float32[i] = c[static_cast<usize>(i)];
    }
    return v;
}
} // namespace

void VulkanCommandList::beginRendering(const RenderingDesc& desc) {
    if (!recording("beginRendering") || !check(!m_inRendering, "nested beginRendering") ||
        !check(m_queue == Queue::Graphics, "beginRendering outside the Graphics queue") ||
        !check(desc.colors.size() <= kMaxColorAttachments, "too many color attachments") ||
        !check(!desc.colors.empty() || desc.depth.texture.isValid(), "beginRendering without attachments")) {
        return;
    }
    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colors{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 width = 0;
    u32 height = 0;
    bool sizesMatch = true;
    auto extentOf = [&](const TextureRes& t, u32 mip) {
        const u32 w = mipExtent(t.desc.width, mip);
        const u32 h = mipExtent(t.desc.height, mip);
        if (width == 0) {
            width = w;
            height = h;
        } else if (w != width || h != height) {
            sizesMatch = false;
        }
    };
    for (usize i = 0; i < desc.colors.size(); ++i) {
        const ColorAttachment& c = desc.colors[i];
        TextureRes* t = texture(c.texture, "beginRendering color", TextureUsage::ColorAttachment);
        if (!t) return;
        const VkImageView view = m_device.attachmentView(*t, c.mip, c.layer);
        if (!check(view != VK_NULL_HANDLE, "beginRendering: invalid color attachment mip/layer")) return;
        extentOf(*t, c.mip);
        VkRenderingAttachmentInfo& a = colors[i];
        a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = view;
        a.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        a.loadOp = toVkLoadOp(c.load);
        a.storeOp = toVkStoreOp(c.store);
        a.clearValue.color = clearValueFor(t->desc.format, c.clearColor);
        if (c.resolve.isValid()) {
            TextureRes* r = texture(c.resolve, "beginRendering resolve", TextureUsage::ColorAttachment);
            if (!r) return;
            if (!check(t->desc.sampleCount > 1 && r->desc.sampleCount == 1 && r->desc.format == t->desc.format &&
                           mipExtent(r->desc.width, 0) == mipExtent(t->desc.width, c.mip) &&
                           mipExtent(r->desc.height, 0) == mipExtent(t->desc.height, c.mip),
                       "beginRendering: resolve needs a multisampled source and a single-sampled target of the same "
                       "format and size")) {
                return;
            }
            const FormatKind kind = formatInfo(t->desc.format).kind;
            a.resolveMode = (kind == FormatKind::Uint || kind == FormatKind::Sint) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                                                                  : VK_RESOLVE_MODE_AVERAGE_BIT;
            a.resolveImageView = m_device.attachmentView(*r, 0, 0);
            a.resolveImageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        }
    }
    bool hasStencilAttachment = false;
    if (desc.depth.texture.isValid()) {
        const DepthAttachment& d = desc.depth;
        TextureRes* t = texture(d.texture, "beginRendering depth", TextureUsage::DepthStencil);
        if (!t) return;
        const VkImageView view = m_device.attachmentView(*t, d.mip, d.layer);
        if (!check(view != VK_NULL_HANDLE, "beginRendering: invalid depth attachment mip/layer")) return;
        extentOf(*t, d.mip);
        depth.imageView = view;
        depth.imageLayout = d.readOnly ? VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        LoadOp load = d.load;
        if (d.readOnly && load == LoadOp::Clear) {
            // Clearing is a write, which a read-only depth attachment does not allow.
            m_device.reportError(std::format("list '{}': beginRendering: read-only depth attachment with LoadOp::Clear "
                                             "(loaded instead)",
                                             m_name));
            load = LoadOp::Load;
        }
        depth.loadOp = toVkLoadOp(load);
        depth.storeOp = d.readOnly ? VK_ATTACHMENT_STORE_OP_NONE : toVkStoreOp(d.store);
        depth.clearValue.depthStencil = {d.clearDepth, d.clearStencil};
        hasStencilAttachment = hasStencil(t->desc.format);
    }
    if (!check(sizesMatch, "beginRendering: attachments differ in size")) return;
    Rect area = desc.area;
    if (area.width == 0 || area.height == 0) area = Rect{0, 0, width, height};
    if (!check(area.x >= 0 && area.y >= 0 && static_cast<u64>(area.x) + area.width <= width &&
                   static_cast<u64>(area.y) + area.height <= height,
               "beginRendering: render area exceeds the attachments")) {
        return;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{area.x, area.y}, {area.width, area.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<u32>(desc.colors.size());
    ri.pColorAttachments = colors.data();
    ri.pDepthAttachment = desc.depth.texture.isValid() ? &depth : nullptr;
    ri.pStencilAttachment = hasStencilAttachment ? &depth : nullptr;
    m_vk.vkCmdBeginRendering(m_cmd, &ri);
    m_inRendering = true;
    setViewport(Viewport{static_cast<f32>(area.x), static_cast<f32>(area.y), static_cast<f32>(area.width),
                         static_cast<f32>(area.height), 0.0f, 1.0f});
    setScissor(area);
}

void VulkanCommandList::endRendering() {
    if (!recording("endRendering") || !check(m_inRendering, "endRendering without beginRendering")) return;
    m_vk.vkCmdEndRendering(m_cmd);
    m_inRendering = false;
}

void VulkanCommandList::setViewport(const Viewport& v) {
    if (!recording("setViewport") || !check(m_queue == Queue::Graphics, "setViewport outside the Graphics queue") ||
        !check(v.width > 0.0f && v.height > 0.0f, "setViewport: empty viewport")) {
        return;
    }
    // Negative height maps clip-space +Y to the top of the framebuffer (math README, 03 §2.7).
    const VkViewport vp{v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
    m_vk.vkCmdSetViewport(m_cmd, 0, 1, &vp);
}

void VulkanCommandList::setScissor(const Rect& r) {
    if (!recording("setScissor") || !check(m_queue == Queue::Graphics, "setScissor outside the Graphics queue") ||
        !check(r.x >= 0 && r.y >= 0, "setScissor: negative offset")) {
        return;
    }
    const VkRect2D rect{{r.x, r.y}, {r.width, r.height}};
    m_vk.vkCmdSetScissor(m_cmd, 0, 1, &rect);
}

void VulkanCommandList::bindPipeline(PipelineH h) {
    if (!recording("bindPipeline")) return;
    PipelineRes* p = m_device.findPipeline(h);
    if (!check(p != nullptr, "bindPipeline: null or stale pipeline")) return;
    // Logical queue rules (not the family's flags): code that happens to work where AsyncCompute or
    // Transfer alias the graphics queue must not break on hardware with dedicated queues.
    const bool graphics = p->bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (!check(graphics ? m_queue == Queue::Graphics : m_queue != Queue::Transfer,
               graphics ? "bindPipeline: graphics pipelines need the Graphics queue"
                        : "bindPipeline: compute pipelines are not supported on the Transfer queue")) {
        return;
    }
    m_boundPoint = p->bindPoint;
    const int status = p->state->status.load(std::memory_order_acquire);
    m_pipelineReady = status == 1;
    if (m_pipelineReady) {
        m_vk.vkCmdBindPipeline(m_cmd, p->bindPoint, p->state->pipeline.load(std::memory_order_acquire));
    }
}

bool VulkanCommandList::pipelineUsable(VkPipelineBindPoint point, std::string_view op) {
    if (m_boundPoint != point) {
        m_device.reportError(std::format("list '{}': {} without a bound {} pipeline", m_name, op,
                                         point == VK_PIPELINE_BIND_POINT_GRAPHICS ? "graphics" : "compute"));
        return false;
    }
    if (!m_pipelineReady) {
        m_device.countPsoMiss();  // still compiling (or failed): skip instead of stalling
        return false;
    }
    return true;
}

void VulkanCommandList::pushConstants(const void* data, u32 bytes, u32 offset) {
    if (!recording("pushConstants") ||
        !check(data != nullptr && bytes > 0 && bytes % 4 == 0 && offset % 4 == 0 && offset <= kMaxPushConstantBytes &&
                   bytes <= kMaxPushConstantBytes - offset,
               "pushConstants: size/offset must be 4-byte multiples within 128 bytes")) {
        return;
    }
    m_vk.vkCmdPushConstants(m_cmd, m_device.pipelineLayout(), VK_SHADER_STAGE_ALL, offset, bytes, data);
}

void VulkanCommandList::bindIndexBuffer(BufferH h, u64 offset, IndexType type) {
    if (!recording("bindIndexBuffer") ||
        !check(m_queue == Queue::Graphics, "bindIndexBuffer outside the Graphics queue")) {
        return;
    }
    const BufferRes* b = buffer(h, "bindIndexBuffer", BufferUsage::Index);
    const u64 align = type == IndexType::Uint16 ? 2 : 4;
    if (!b || !check(offset % align == 0 && offset < b->desc.size, "bindIndexBuffer: bad offset")) return;
    m_vk.vkCmdBindIndexBuffer(m_cmd, b->buffer, offset,
                              type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    m_hasIndexBuffer = true;
}

void VulkanCommandList::draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    if (!recording("draw") || !check(m_inRendering, "draw outside rendering") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_GRAPHICS, "draw")) {
        return;
    }
    m_vk.vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset,
                                    u32 firstInstance) {
    if (!recording("drawIndexed") || !check(m_inRendering, "drawIndexed outside rendering") ||
        !check(m_hasIndexBuffer, "drawIndexed without bindIndexBuffer") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_GRAPHICS, "drawIndexed")) {
        return;
    }
    m_vk.vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

namespace {
/// Bytes read by `count` indirect records of `recordSize` bytes, `stride` apart (0 for count 0).
u64 indirectBytes(u32 count, u32 stride, u64 recordSize) {
    return count == 0 ? 0 : u64(stride) * (count - 1) + recordSize;
}
} // namespace

void VulkanCommandList::drawIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) {
    if (!recording("drawIndirect") || !check(m_inRendering, "drawIndirect outside rendering")) return;
    const BufferRes* b = buffer(args, "drawIndirect", BufferUsage::Indirect);
    if (!b || !check(offset % 4 == 0 && stride % 4 == 0 && stride >= sizeof(DrawIndirectCommand),
                     "drawIndirect: offset/stride must be 4-byte aligned, stride >= 16") ||
        !bufferRange(*b, offset, indirectBytes(drawCount, stride, sizeof(DrawIndirectCommand)), "drawIndirect") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_GRAPHICS, "drawIndirect")) {
        return;
    }
    m_vk.vkCmdDrawIndirect(m_cmd, b->buffer, offset, drawCount, stride);
}

void VulkanCommandList::drawIndexedIndirect(BufferH args, u64 offset, u32 drawCount, u32 stride) {
    if (!recording("drawIndexedIndirect") || !check(m_inRendering, "drawIndexedIndirect outside rendering") ||
        !check(m_hasIndexBuffer, "drawIndexedIndirect without bindIndexBuffer")) {
        return;
    }
    const BufferRes* b = buffer(args, "drawIndexedIndirect", BufferUsage::Indirect);
    if (!b || !check(offset % 4 == 0 && stride % 4 == 0 && stride >= sizeof(DrawIndexedIndirectCommand),
                     "drawIndexedIndirect: offset/stride must be 4-byte aligned, stride >= 20") ||
        !bufferRange(*b, offset, indirectBytes(drawCount, stride, sizeof(DrawIndexedIndirectCommand)),
                     "drawIndexedIndirect") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_GRAPHICS, "drawIndexedIndirect")) {
        return;
    }
    m_vk.vkCmdDrawIndexedIndirect(m_cmd, b->buffer, offset, drawCount, stride);
}

void VulkanCommandList::drawIndexedIndirectCount(BufferH args, u64 offset, BufferH count, u64 countOffset,
                                                 u32 maxDraws, u32 stride) {
    if (!recording("drawIndexedIndirectCount") || !check(m_inRendering, "drawIndexedIndirectCount outside rendering") ||
        !check(m_hasIndexBuffer, "drawIndexedIndirectCount without bindIndexBuffer")) {
        return;
    }
    const BufferRes* a = buffer(args, "drawIndexedIndirectCount args", BufferUsage::Indirect);
    const BufferRes* c = buffer(count, "drawIndexedIndirectCount count", BufferUsage::Indirect);
    if (!a || !c ||
        !check(offset % 4 == 0 && countOffset % 4 == 0 && stride % 4 == 0 && stride >= sizeof(DrawIndexedIndirectCommand),
               "drawIndexedIndirectCount: offsets/stride must be 4-byte aligned, stride >= 20") ||
        !bufferRange(*a, offset, indirectBytes(maxDraws, stride, sizeof(DrawIndexedIndirectCommand)),
                     "drawIndexedIndirectCount args") ||
        !bufferRange(*c, countOffset, 4, "drawIndexedIndirectCount count") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_GRAPHICS, "drawIndexedIndirectCount")) {
        return;
    }
    m_vk.vkCmdDrawIndexedIndirectCount(m_cmd, a->buffer, offset, c->buffer, countOffset, maxDraws, stride);
}

void VulkanCommandList::dispatch(u32 x, u32 y, u32 z) {
    if (!recording("dispatch") || !check(!m_inRendering, "dispatch inside rendering")) return;
    const auto& maxCount = m_device.caps().limits.maxComputeWorkGroupCount;
    if (!check(x <= maxCount[0] && y <= maxCount[1] && z <= maxCount[2], "dispatch exceeds maxComputeWorkGroupCount") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_COMPUTE, "dispatch")) {
        return;
    }
    m_vk.vkCmdDispatch(m_cmd, x, y, z);
}

void VulkanCommandList::dispatchIndirect(BufferH args, u64 offset) {
    if (!recording("dispatchIndirect") || !check(!m_inRendering, "dispatchIndirect inside rendering")) return;
    const BufferRes* b = buffer(args, "dispatchIndirect", BufferUsage::Indirect);
    if (!b || !check(offset % 4 == 0, "dispatchIndirect: offset not 4-byte aligned") ||
        !bufferRange(*b, offset, sizeof(DispatchIndirectCommand), "dispatchIndirect") ||
        !pipelineUsable(VK_PIPELINE_BIND_POINT_COMPUTE, "dispatchIndirect")) {
        return;
    }
    m_vk.vkCmdDispatchIndirect(m_cmd, b->buffer, offset);
}

// -------------------------------------------------------------------------------------------------
// Transfer
// -------------------------------------------------------------------------------------------------
void VulkanCommandList::copyBuffer(BufferH src, u64 srcOffset, BufferH dst, u64 dstOffset, u64 size) {
    if (!recording("copyBuffer") || !check(!m_inRendering, "copyBuffer inside rendering")) return;
    const BufferRes* s = buffer(src, "copyBuffer source", BufferUsage::TransferSrc);
    const BufferRes* d = buffer(dst, "copyBuffer destination", BufferUsage::TransferDst);
    if (!s || !d || !check(size > 0, "copyBuffer: size is 0") || !bufferRange(*s, srcOffset, size, "copyBuffer source") ||
        !bufferRange(*d, dstOffset, size, "copyBuffer destination") ||
        !check(src != dst || srcOffset >= dstOffset + size || dstOffset >= srcOffset + size,
               "copyBuffer: overlapping ranges within one buffer")) {
        return;
    }
    const VkBufferCopy region{srcOffset, dstOffset, size};
    m_vk.vkCmdCopyBuffer(m_cmd, s->buffer, d->buffer, 1, &region);
}

VkBufferImageCopy2 VulkanCommandList::bufferImageCopy(const TextureRes& t, const BufferTextureLayout& layout,
                                                      const TextureRegion& region) const {
    // `region` is resolved (detail::resolveCopyRegion): no zero extents, in bounds.
    const FormatInfo& info = formatInfo(t.desc.format);
    VkBufferImageCopy2 c{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    c.bufferOffset = layout.offset;
    c.bufferRowLength = layout.rowPitch ? layout.rowPitch / info.blockBytes * info.blockWidth : 0;
    c.bufferImageHeight = 0;
    // Copies address one aspect; depth-stencil formats copy depth.
    c.imageSubresource.aspectMask =
        (t.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT) : t.aspect;
    c.imageSubresource.mipLevel = region.mip;
    c.imageSubresource.baseArrayLayer = region.baseLayer;
    c.imageSubresource.layerCount = region.layerCount;
    c.imageOffset = {static_cast<i32>(region.x), static_cast<i32>(region.y), static_cast<i32>(region.z)};
    c.imageExtent = {region.width, region.height, region.depth};
    return c;
}

void VulkanCommandList::copyBufferToTexture(BufferH src, const BufferTextureLayout& layout, TextureH dst,
                                            const TextureRegion& region) {
    if (!recording("copyBufferToTexture") || !check(!m_inRendering, "copyBufferToTexture inside rendering")) return;
    const BufferRes* s = buffer(src, "copyBufferToTexture source", BufferUsage::TransferSrc);
    const TextureRes* t = texture(dst, "copyBufferToTexture destination", TextureUsage::TransferDst);
    if (!s || !t) return;
    u64 bytes = 0;
    auto resolved = detail::resolveCopyRegion(t->desc, region, layout, &bytes);
    if (!resolved) {
        m_device.reportError(std::format("list '{}': {}", m_name, resolved.error().message));
        return;
    }
    if (!bufferRange(*s, layout.offset, bytes, "copyBufferToTexture source")) return;
    const VkBufferImageCopy2 c = bufferImageCopy(*t, layout, *resolved);
    VkCopyBufferToImageInfo2 info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    info.srcBuffer = s->buffer;
    info.dstImage = t->image;
    info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    info.regionCount = 1;
    info.pRegions = &c;
    m_vk.vkCmdCopyBufferToImage2(m_cmd, &info);
}

void VulkanCommandList::copyTextureToBuffer(TextureH src, const TextureRegion& region, BufferH dst,
                                            const BufferTextureLayout& layout) {
    if (!recording("copyTextureToBuffer") || !check(!m_inRendering, "copyTextureToBuffer inside rendering")) return;
    const TextureRes* t = texture(src, "copyTextureToBuffer source", TextureUsage::TransferSrc);
    const BufferRes* d = buffer(dst, "copyTextureToBuffer destination", BufferUsage::TransferDst);
    if (!t || !d) return;
    u64 bytes = 0;
    auto resolved = detail::resolveCopyRegion(t->desc, region, layout, &bytes);
    if (!resolved) {
        m_device.reportError(std::format("list '{}': {}", m_name, resolved.error().message));
        return;
    }
    if (!bufferRange(*d, layout.offset, bytes, "copyTextureToBuffer destination")) return;
    const VkBufferImageCopy2 c = bufferImageCopy(*t, layout, *resolved);
    VkCopyImageToBufferInfo2 info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    info.srcImage = t->image;
    info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dstBuffer = d->buffer;
    info.regionCount = 1;
    info.pRegions = &c;
    m_vk.vkCmdCopyImageToBuffer2(m_cmd, &info);
}

void VulkanCommandList::fillBuffer(BufferH h, u64 offset, u64 size, u32 value) {
    if (!recording("fillBuffer") || !check(!m_inRendering, "fillBuffer inside rendering")) return;
    const BufferRes* b = buffer(h, "fillBuffer", BufferUsage::TransferDst);
    if (!b) return;
    // VK_WHOLE_SIZE fills to the last multiple of 4 bytes, like the Null backend.
    const u64 bytes = size == kWholeSize ? (offset <= b->desc.size ? (b->desc.size - offset) & ~3ull : 0) : size;
    if (!check(offset % 4 == 0 && bytes % 4 == 0 && bytes > 0, "fillBuffer: offset and size must be non-zero "
                                                                "multiples of 4") ||
        !bufferRange(*b, offset, bytes, "fillBuffer")) {
        return;
    }
    m_vk.vkCmdFillBuffer(m_cmd, b->buffer, offset, size == kWholeSize ? VK_WHOLE_SIZE : size, value);
}

void VulkanCommandList::updateBuffer(BufferH h, u64 offset, std::span<const std::byte> data) {
    if (!recording("updateBuffer") || !check(!m_inRendering, "updateBuffer inside rendering")) return;
    const BufferRes* b = buffer(h, "updateBuffer", BufferUsage::TransferDst);
    if (!b ||
        !check(!data.empty() && data.size() <= 65536 && data.size() % 4 == 0 && offset % 4 == 0,
               "updateBuffer: 4-byte multiples, at most 65536 bytes") ||
        !bufferRange(*b, offset, data.size(), "updateBuffer")) {
        return;
    }
    m_vk.vkCmdUpdateBuffer(m_cmd, b->buffer, offset, data.size(), data.data());
}

void VulkanCommandList::clearTexture(TextureH h, const std::array<f32, 4>& color, const SubresourceRange& range) {
    if (!recording("clearTexture") || !check(!m_inRendering, "clearTexture inside rendering") ||
        !check(m_queue != Queue::Transfer, "clearTexture is not supported on the Transfer queue")) {
        return;
    }
    const TextureRes* t = texture(h, "clearTexture", TextureUsage::TransferDst);
    if (!t || !check(!isDepthFormat(t->desc.format) && !isCompressedFormat(t->desc.format),
                     "clearTexture: not a color texture")) {
        return;
    }
    const SubresourceRange r = detail::resolveRange(t->desc, range);
    if (!check(range.baseMip < t->desc.mipLevels && range.baseLayer < t->desc.arrayLayers && r.mipCount > 0 &&
                   r.layerCount > 0,
               "clearTexture: subresource range outside the texture")) {
        return;
    }
    const VkClearColorValue value = clearValueFor(t->desc.format, color);
    const VkImageSubresourceRange vr{VK_IMAGE_ASPECT_COLOR_BIT, r.baseMip, r.mipCount, r.baseLayer, r.layerCount};
    m_vk.vkCmdClearColorImage(m_cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &vr);
}

// -------------------------------------------------------------------------------------------------
// Debug
// -------------------------------------------------------------------------------------------------
namespace {
VkDebugUtilsLabelEXT makeLabel(const std::string& name, u32 rgba) {
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name.c_str();
    if (rgba != 0) {
        label.color[0] = static_cast<f32>((rgba >> 24) & 0xFF) / 255.0f;
        label.color[1] = static_cast<f32>((rgba >> 16) & 0xFF) / 255.0f;
        label.color[2] = static_cast<f32>((rgba >> 8) & 0xFF) / 255.0f;
        label.color[3] = static_cast<f32>(rgba & 0xFF) / 255.0f;
    }
    return label;
}
} // namespace

void VulkanCommandList::beginLabel(std::string_view name, u32 rgba) {
    if (!recording("beginLabel")) return;
    ++m_labelDepth;
    const auto fn = m_device.vki().vkCmdBeginDebugUtilsLabelEXT;
    if (!m_device.debugUtils() || !fn) return;
    const std::string copy(name);
    const VkDebugUtilsLabelEXT label = makeLabel(copy, rgba);
    fn(m_cmd, &label);
}

void VulkanCommandList::endLabel() {
    if (!recording("endLabel") || !check(m_labelDepth > 0, "endLabel without beginLabel")) return;
    --m_labelDepth;
    const auto fn = m_device.vki().vkCmdEndDebugUtilsLabelEXT;
    if (m_device.debugUtils() && fn) fn(m_cmd);
}

void VulkanCommandList::insertLabel(std::string_view name, u32 rgba) {
    if (!recording("insertLabel")) return;
    const auto fn = m_device.vki().vkCmdInsertDebugUtilsLabelEXT;
    if (!m_device.debugUtils() || !fn) return;
    const std::string copy(name);
    const VkDebugUtilsLabelEXT label = makeLabel(copy, rgba);
    fn(m_cmd, &label);
}

void VulkanCommandList::breadcrumb(u16 passId, BreadcrumbStage stage) {
    if (!recording("breadcrumb") || !check(!m_inRendering, "breadcrumb inside rendering")) return;
    const u32 value = static_cast<u32>((m_device.frameIndex() & 0xFFFFu) << 16) | passId;
    const u64 offset = (static_cast<u64>(queueIndex(m_queue)) * 2 + (stage == BreadcrumbStage::Begin ? 0 : 1)) * 4;
    m_vk.vkCmdFillBuffer(m_cmd, m_device.breadcrumbBuffer(), offset, 4, value);
}

} // namespace helios::rhi::vk
