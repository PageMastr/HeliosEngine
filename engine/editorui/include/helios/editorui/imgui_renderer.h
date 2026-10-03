#pragma once
// Dear ImGui renderer backend on the Helios RHI (07 §1.3: "a Helios renderer backend on our RHI
// makes viewports and thumbnails bindless textures in the render graph"). Replaces
// imgui_impl_vulkan: vertices are pulled through buffer device addresses, textures are bindless
// sampled-image slots (ImTextureID = BindlessIndex), and ImGui 1.92's dynamic textures (font atlas
// growth at 200 % DPI, re-rasterized glyphs) are honoured through ImDrawData::Textures.
//
//   renderer->updateTextures(drawData);     // every frame, after ImGui::Render()
//   renderer->prepare(drawData);            // frames that are drawn: upload vertices/indices
//   ... inside a render-graph raster pass over the target: renderer->record(ctx.cmd(), drawData);
//
// Threading: owner thread only, except record(), which only reads prepared state and may run on a
// render-graph recording job.

#include <array>
#include <memory>
#include <vector>

#include "helios/core/result.h"
#include "helios/rhi/device.h"

struct ImDrawData;
struct ImGuiIO;

namespace helios::edui {

class ImGuiRenderer {
public:
    /// Creates the pipeline for render targets of `colorFormat`.
    static Result<std::unique_ptr<ImGuiRenderer>> create(rhi::Device& device, rhi::Format colorFormat);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    /// Declares the backend capabilities (RendererHasTextures, RendererHasVtxOffset) in `io`.
    void installBackend(ImGuiIO& io);
    /// Creates, updates and destroys the textures ImGui asks for (one upload submission).
    Result<void> updateTextures(ImDrawData* drawData);
    /// Copies this frame's vertices and indices into the current frame slot's upload buffers.
    Result<void> prepare(const ImDrawData* drawData);
    /// Records the draw commands; the target is `drawData->DisplaySize * FramebufferScale` pixels
    /// and must be bound by the caller (beginRendering). Skips when prepare() was not called.
    void record(rhi::CommandList& cmd, const ImDrawData* drawData) const;
    /// Destroys every texture created for ImGui (call before destroying the ImGui context).
    void shutdown(ImGuiIO& io);

    /// Sampler used for every ImGui texture (linear, clamp).
    rhi::BindlessIndex sampler() const noexcept { return m_sampler; }
    u32 liveTextures() const noexcept { return m_liveTextures; }

private:
    explicit ImGuiRenderer(rhi::Device& device) noexcept : m_device(&device) {}

    struct FrameBuffers {
        rhi::BufferH vertices;
        rhi::BufferH indices;
        u64 vertexCapacity = 0;
        u64 indexCapacity = 0;
        u64 preparedFrame = ~0ull;
    };

    rhi::Device* m_device;
    rhi::PipelineH m_pipeline;
    rhi::BindlessIndex m_sampler = rhi::kInvalidBindless;
    std::vector<FrameBuffers> m_frames;
    u32 m_liveTextures = 0;
};

} // namespace helios::edui
