#pragma once
// Command recording (03 §1.1).
//
// A CommandList is obtained from Device::acquireCommandList(queue) on the thread that records it,
// recorded by exactly one thread, closed with end() (or implicitly by submit() when submitted from
// the recording thread) and handed to Device::submit(). Lists come from per-thread, per-frame pools:
// they are recycled automatically once their frame slot comes around again (Device::beginFrame) and
// must not be touched after submit. Several threads (e.g. job-system jobs) may record different
// lists concurrently; submission order, not recording order, defines execution order.
//
// Resource state: the caller declares transitions with barrier() (normally the render graph does).
// Commands assume their resources are in the matching state: attachments in RenderTarget /
// DepthWrite / DepthRead, copy sources in CopySource, copy/clear/fill destinations in CopyDest.
// The Null backend validates this; the Vulkan backend trusts it (Khronos validation catches misuse).
//
// Lifetime: a list must be submitted (once) before the next beginFrame()/waitIdle() recycles it;
// both backends reject a stale, recycled or twice-submitted list at submit and report commands
// recorded into a list that is no longer recording.
//
// Validity: draw* only between beginRendering/endRendering on the Graphics queue; compute
// pipelines, dispatch* and clearTexture on Graphics or AsyncCompute; copies/fills/updates on any
// queue, outside rendering; labels must be balanced within one list. Queue rules are logical: they
// hold even where an adapter aliases AsyncCompute/Transfer onto the graphics queue. Commands that
// break these rules, or reference out-of-range buffer/texture regions, are reported as validation
// errors and dropped (never forwarded to the driver).

#include <array>
#include <span>
#include <string_view>
#include <type_traits>

#include "helios/core/types.h"
#include "helios/rhi/handles.h"
#include "helios/rhi/types.h"

namespace helios::rhi {

class CommandList {
public:
    virtual ~CommandList() = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    Queue queue() const noexcept { return m_queue; }
    /// Identity of the Device that created the list; submit() rejects lists of other devices.
    const void* deviceTag() const noexcept { return m_deviceTag; }

    /// Closes recording. Must be called on the recording thread before another thread submits it.
    virtual void end() = 0;

    // -- Synchronization ----------------------------------------------------------------------
    /// Records state transitions as one batched dependency.
    virtual void barrier(std::span<const Barrier> barriers) = 0;
    void barrier(const Barrier& b) { barrier(std::span<const Barrier>(&b, 1)); }

    // -- Rendering ----------------------------------------------------------------------------
    virtual void beginRendering(const RenderingDesc& desc) = 0;
    virtual void endRendering() = 0;
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect& scissor) = 0;

    /// Binds a graphics (Graphics queue, inside rendering) or compute pipeline.
    virtual void bindPipeline(PipelineH pipeline) = 0;
    /// Push constants shared by all stages; offset + bytes <= kMaxPushConstantBytes, both multiples
    /// of 4. Values persist across pipeline binds (one pipeline layout for everything).
    virtual void pushConstants(const void* data, u32 bytes, u32 offset = 0) = 0;
    /// Pushes a whole struct at offset 0. Single-argument only, so `pushConstants(ptr, size)` can
    /// never bind to this overload by accident.
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>)
    void pushConstants(const T& value) {
        static_assert(sizeof(T) % 4 == 0 && sizeof(T) <= kMaxPushConstantBytes, "push constants: <= 128 B, 4-byte multiple");
        pushConstants(static_cast<const void*>(&value), static_cast<u32>(sizeof(T)), 0u);
    }

    virtual void bindIndexBuffer(BufferH buffer, u64 offset, IndexType type) = 0;
    virtual void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) = 0;
    virtual void drawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0,
                             u32 firstInstance = 0) = 0;
    /// `drawCount` DrawIndirectCommand records at `offset`, `stride` bytes apart.
    virtual void drawIndirect(BufferH args, u64 offset, u32 drawCount,
                              u32 stride = sizeof(DrawIndirectCommand)) = 0;
    virtual void drawIndexedIndirect(BufferH args, u64 offset, u32 drawCount,
                                     u32 stride = sizeof(DrawIndexedIndirectCommand)) = 0;
    /// GPU-driven draws: the draw count is read from `count` (u32 at countOffset), capped at maxDraws.
    virtual void drawIndexedIndirectCount(BufferH args, u64 offset, BufferH count, u64 countOffset, u32 maxDraws,
                                          u32 stride = sizeof(DrawIndexedIndirectCommand)) = 0;

    // -- Compute ------------------------------------------------------------------------------
    virtual void dispatch(u32 groupsX, u32 groupsY = 1, u32 groupsZ = 1) = 0;
    virtual void dispatchIndirect(BufferH args, u64 offset) = 0;

    // -- Transfer -----------------------------------------------------------------------------
    virtual void copyBuffer(BufferH src, u64 srcOffset, BufferH dst, u64 dstOffset, u64 size) = 0;
    virtual void copyBufferToTexture(BufferH src, const BufferTextureLayout& layout, TextureH dst,
                                     const TextureRegion& region) = 0;
    virtual void copyTextureToBuffer(TextureH src, const TextureRegion& region, BufferH dst,
                                     const BufferTextureLayout& layout) = 0;
    /// Fills `size` bytes (multiple of 4, or kWholeSize) with a repeated u32.
    virtual void fillBuffer(BufferH buffer, u64 offset, u64 size, u32 value) = 0;
    /// Small inline update (<= 65,536 bytes, 4-byte multiple) recorded into the command stream.
    virtual void updateBuffer(BufferH buffer, u64 offset, std::span<const std::byte> data) = 0;
    /// Clears color texture subresources (CopyDest state) outside rendering.
    virtual void clearTexture(TextureH texture, const std::array<f32, 4>& color, const SubresourceRange& range = {}) = 0;

    // -- Debug --------------------------------------------------------------------------------
    /// Debug-utils label scopes (RenderDoc, Nsight, RGP). rgba = 0xRRGGBBAA, 0 = tool default.
    virtual void beginLabel(std::string_view name, u32 rgba = 0) = 0;
    virtual void endLabel() = 0;
    virtual void insertLabel(std::string_view name, u32 rgba = 0) = 0;
    /// GPU crash breadcrumb (03 §1.5): records that `passId` began/ended on this queue in the
    /// current frame. Only valid outside rendering. Read back with Device::breadcrumbs().
    virtual void breadcrumb(u16 passId, BreadcrumbStage stage) = 0;

protected:
    CommandList(Queue queue, const void* deviceTag) noexcept : m_queue(queue), m_deviceTag(deviceTag) {}
    Queue m_queue;

private:
    const void* m_deviceTag;
};

/// RAII label scope: `ScopedLabel label(cmd, "Shadows");`
class ScopedLabel {
public:
    ScopedLabel(CommandList& cmd, std::string_view name, u32 rgba = 0) : m_cmd(cmd) { m_cmd.beginLabel(name, rgba); }
    ~ScopedLabel() { m_cmd.endLabel(); }
    ScopedLabel(const ScopedLabel&) = delete;
    ScopedLabel& operator=(const ScopedLabel&) = delete;

private:
    CommandList& m_cmd;
};

} // namespace helios::rhi
