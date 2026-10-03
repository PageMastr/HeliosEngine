#include "helios/editorui/imgui_renderer.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "editor_pipelines.h"
#include "helios/core/log.h"
#include "helios_editorui_shaders.h"
#include "imgui.h"

namespace helios::edui {

namespace {

/// Frame slots of the vertex/index upload buffers. The RHI keeps at most framesInFlight (2 by
/// default, ≤ 3) frames on the GPU, so a slot is never rewritten while the GPU reads it.
constexpr u32 kFrameSlots = 3;

struct Push {
    u64 vertices;
    f32 scale[2];
    f32 translate[2];
    u32 vertexOffset;
    u32 texture;
    u32 samplerIndex;
    u32 pad;
};
static_assert(sizeof(Push) == 40);
static_assert(sizeof(ImDrawVert) == 20, "imgui.slang reads 20-byte vertices");
static_assert(sizeof(ImDrawIdx) == 2, "the renderer binds 16-bit indices");

struct TextureSlot {
    rhi::TextureH texture;
};

} // namespace

Result<std::unique_ptr<ImGuiRenderer>> ImGuiRenderer::create(rhi::Device& device, rhi::Format colorFormat) {
    std::unique_ptr<ImGuiRenderer> r(new ImGuiRenderer(device));
    rhi::GraphicsPipelineDesc desc;
    desc.vertex = rhi::ShaderDesc{helios_editorui_shaders::imgui(), "vsMain"};
    desc.fragment = rhi::ShaderDesc{helios_editorui_shaders::imgui(), "psMain"};
    desc.colorCount = 1;
    desc.colorFormats[0] = colorFormat;
    rhi::BlendState blend;
    blend.enable = true;
    blend.srcColor = rhi::BlendFactor::SrcAlpha;
    blend.dstColor = rhi::BlendFactor::OneMinusSrcAlpha;
    blend.srcAlpha = rhi::BlendFactor::One;
    blend.dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha;
    desc.blend[0] = blend;
    desc.name = "ImGui";
    HELIOS_TRY_ASSIGN(r->m_pipeline, detail::editorPipeline(device, desc));
    rhi::SamplerDesc s;
    s.addressU = s.addressV = s.addressW = rhi::AddressMode::ClampToEdge;
    s.mipFilter = rhi::Filter::Nearest;
    r->m_sampler = device.sampler(s);
    r->m_frames.resize(kFrameSlots);
    return r;
}

ImGuiRenderer::~ImGuiRenderer() {
    for (FrameBuffers& f : m_frames) {
        m_device->destroy(f.vertices);
        m_device->destroy(f.indices);
    }
    m_device->destroy(m_pipeline);
}

void ImGuiRenderer::installBackend(ImGuiIO& io) {
    io.BackendRendererName = "helios_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
}

Result<void> ImGuiRenderer::updateTextures(ImDrawData* drawData) {
    if (!drawData || !drawData->Textures) return {};
    rhi::CommandList* cmd = nullptr;
    std::vector<rhi::BufferH> staging;
    Result<void> result;
    for (ImTextureData* tex : *drawData->Textures) {
        if (tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_Destroyed) continue;
        if (tex->Status == ImTextureStatus_WantDestroy) {
            // Keep it while a frame in flight may still sample it.
            if (tex->UnusedFrames < static_cast<int>(kFrameSlots)) continue;
            if (auto* slot = static_cast<TextureSlot*>(tex->BackendUserData)) {
                m_device->destroy(slot->texture);
                delete slot;
                --m_liveTextures;
            }
            tex->BackendUserData = nullptr;
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
            continue;
        }
        if (tex->Format != ImTextureFormat_RGBA32) {
            result = Error{ErrorCode::Unsupported, "ImGui textures must be RGBA32"};
            continue;
        }
        const bool create = tex->Status == ImTextureStatus_WantCreate;
        auto* slot = static_cast<TextureSlot*>(tex->BackendUserData);
        if (create) {
            if (slot) {
                m_device->destroy(slot->texture);
                delete slot;
                --m_liveTextures;
            }
            auto t = m_device->createTexture(rhi::TextureDesc::tex2D(rhi::Format::RGBA8Unorm, static_cast<u32>(tex->Width),
                                                                     static_cast<u32>(tex->Height),
                                                                     rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                                                     "ImGuiTexture"));
            if (!t) {
                result = std::move(t).error();
                continue;
            }
            slot = new TextureSlot{*t};
            ++m_liveTextures;
            tex->BackendUserData = slot;
            tex->SetTexID(static_cast<ImTextureID>(m_device->srv(*t)));
        }
        if (!slot) continue;
        // Upload the whole texture on creation, else the queued update rectangles.
        std::vector<ImTextureRect> rects;
        if (create) {
            rects.push_back({0, 0, static_cast<unsigned short>(tex->Width), static_cast<unsigned short>(tex->Height)});
        } else {
            for (const ImTextureRect& r : tex->Updates) rects.push_back(r);
            if (rects.empty()) rects.push_back(tex->UpdateRect);
        }
        if (!cmd) cmd = m_device->acquireCommandList(rhi::Queue::Graphics, "ImGuiTextures");
        if (!cmd) return Error{ErrorCode::InvalidState, "device lost"};
        cmd->barrier(rhi::Barrier::textureState(slot->texture, create ? rhi::ResourceState::Undefined : rhi::ResourceState::ShaderResource,
                                                rhi::ResourceState::CopyDest));
        for (const ImTextureRect& r : rects) {
            if (r.w == 0 || r.h == 0) continue;
            const u64 rowBytes = static_cast<u64>(r.w) * 4;
            auto buf = m_device->createBuffer({.size = rowBytes * r.h,
                                               .usage = rhi::BufferUsage::TransferSrc,
                                               .memory = rhi::MemoryUsage::Upload,
                                               .name = "ImGuiTextureUpload"});
            if (!buf) {
                result = std::move(buf).error();
                break;
            }
            auto* dst = static_cast<u8*>(m_device->map(*buf));
            for (u32 y = 0; y < r.h; ++y) {
                std::memcpy(dst + y * rowBytes, tex->GetPixelsAt(r.x, r.y + static_cast<int>(y)), rowBytes);
            }
            m_device->flushMapped(*buf);
            rhi::TextureRegion region;
            region.x = r.x;
            region.y = r.y;
            region.width = r.w;
            region.height = r.h;
            cmd->copyBufferToTexture(*buf, {}, slot->texture, region);
            staging.push_back(*buf);
        }
        cmd->barrier(rhi::Barrier::textureState(slot->texture, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource));
        tex->SetStatus(ImTextureStatus_OK);
    }
    if (cmd) {
        auto submitted = m_device->submit(rhi::Queue::Graphics, {&cmd, 1});
        if (!submitted && result) result = std::move(submitted).error();
    }
    // Deferred by the RHI until the submission completes.
    for (rhi::BufferH b : staging) m_device->destroy(b);
    return result;
}

Result<void> ImGuiRenderer::prepare(const ImDrawData* drawData) {
    if (!drawData || drawData->TotalVtxCount == 0) return {};
    FrameBuffers& f = m_frames[m_device->frameIndex() % kFrameSlots];
    const u64 vtxBytes = static_cast<u64>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const u64 idxBytes = static_cast<u64>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);
    if (vtxBytes > f.vertexCapacity) {
        m_device->destroy(f.vertices);
        f.vertexCapacity = std::max<u64>(vtxBytes + vtxBytes / 2, 64 * 1024);
        HELIOS_TRY_ASSIGN(f.vertices, m_device->createBuffer({.size = f.vertexCapacity,
                                                              .usage = rhi::BufferUsage::Storage,
                                                              .memory = rhi::MemoryUsage::Upload,
                                                              .name = "ImGuiVertices"}));
    }
    if (idxBytes > f.indexCapacity) {
        m_device->destroy(f.indices);
        f.indexCapacity = std::max<u64>(idxBytes + idxBytes / 2, 32 * 1024);
        HELIOS_TRY_ASSIGN(f.indices, m_device->createBuffer({.size = f.indexCapacity,
                                                             .usage = rhi::BufferUsage::Index,
                                                             .memory = rhi::MemoryUsage::Upload,
                                                             .name = "ImGuiIndices"}));
    }
    auto* vtx = static_cast<u8*>(m_device->map(f.vertices));
    auto* idx = static_cast<u8*>(m_device->map(f.indices));
    if (!vtx || !idx) return Error{ErrorCode::InvalidState, "ImGui upload buffers are not mapped"};
    for (const ImDrawList* list : drawData->CmdLists) {
        const usize vb = static_cast<usize>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const usize ib = static_cast<usize>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        std::memcpy(vtx, list->VtxBuffer.Data, vb);
        std::memcpy(idx, list->IdxBuffer.Data, ib);
        vtx += vb;
        idx += ib;
    }
    m_device->flushMapped(f.vertices, 0, vtxBytes);
    m_device->flushMapped(f.indices, 0, idxBytes);
    f.preparedFrame = m_device->frameIndex();
    return {};
}

void ImGuiRenderer::record(rhi::CommandList& cmd, const ImDrawData* drawData) const {
    if (!drawData || drawData->TotalVtxCount == 0) return;
    const FrameBuffers& f = m_frames[m_device->frameIndex() % kFrameSlots];
    if (f.preparedFrame != m_device->frameIndex()) return;
    const f32 fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
    const f32 fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
    if (fbW <= 0 || fbH <= 0) return;
    cmd.bindPipeline(m_pipeline);
    cmd.bindIndexBuffer(f.indices, 0, rhi::IndexType::Uint16);
    Push push{};
    push.vertices = m_device->deviceAddress(f.vertices);
    // Pixels (y down, origin DisplayPos) -> clip space with +Y up (the RHI flips the viewport).
    push.scale[0] = 2.0f / drawData->DisplaySize.x;
    push.scale[1] = -2.0f / drawData->DisplaySize.y;
    push.translate[0] = -1.0f - drawData->DisplayPos.x * push.scale[0];
    push.translate[1] = 1.0f - drawData->DisplayPos.y * push.scale[1];
    push.samplerIndex = m_sampler;
    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;
    u32 globalVtx = 0;
    u32 globalIdx = 0;
    for (const ImDrawList* list : drawData->CmdLists) {
        for (const ImDrawCmd& dc : list->CmdBuffer) {
            if (dc.UserCallback) {
                if (dc.UserCallback != ImDrawCallback_ResetRenderState) dc.UserCallback(list, &dc);
                continue;
            }
            const f32 x0 = std::max((dc.ClipRect.x - clipOff.x) * clipScale.x, 0.0f);
            const f32 y0 = std::max((dc.ClipRect.y - clipOff.y) * clipScale.y, 0.0f);
            const f32 x1 = std::min((dc.ClipRect.z - clipOff.x) * clipScale.x, fbW);
            const f32 y1 = std::min((dc.ClipRect.w - clipOff.y) * clipScale.y, fbH);
            if (x1 <= x0 || y1 <= y0) continue;
            cmd.setScissor(rhi::Rect{static_cast<i32>(x0), static_cast<i32>(y0), static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)});
            push.vertexOffset = globalVtx + dc.VtxOffset;
            push.texture = static_cast<u32>(dc.GetTexID());
            cmd.pushConstants(push);
            cmd.drawIndexed(dc.ElemCount, 1, globalIdx + dc.IdxOffset, 0, 0);
        }
        globalVtx += static_cast<u32>(list->VtxBuffer.Size);
        globalIdx += static_cast<u32>(list->IdxBuffer.Size);
    }
}

void ImGuiRenderer::shutdown(ImGuiIO& io) {
    (void)io;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (auto* slot = static_cast<TextureSlot*>(tex->BackendUserData)) {
            m_device->destroy(slot->texture);
            delete slot;
            --m_liveTextures;
        }
        tex->BackendUserData = nullptr;
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

} // namespace helios::edui
