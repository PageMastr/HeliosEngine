#pragma once
// Internal state of RenderGraph / RgResourcePool shared by the setup, compile, execute and dump
// translation units.

#include <atomic>
#include <format>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/render/render_graph.h"

namespace helios::render {

HELIOS_LOG_CHANNEL(LogRenderGraph, "RenderGraph");

/// One color/depth attachment of a Raster pass.
struct RgAttachmentDecl {
    u32 resource = kRgInvalid;
    u32 mip = 0;
    u32 layer = 0;
    rhi::LoadOp load = rhi::LoadOp::Clear;
    std::array<f32, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    f32 clearDepth = 0.0f;
    bool readOnly = false;
};

/// Access declaration plus the RHI usage it requires.
struct RgAccessDecl {
    RgAccess access;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
};

struct RgPassDecl {
    std::string name;
    PassFlags flags = PassFlags::None;
    std::vector<RgAccessDecl> accesses;
    std::vector<RgAttachmentDecl> colors;  ///< Indexed by slot (resource == kRgInvalid: unused slot).
    RgAttachmentDecl depth;                ///< resource == kRgInvalid: no depth attachment.
    std::function<void(RgContext&)> execute;
    /// Versions of each resource this pass touches: the one it started from and the one it
    /// produces (kRgInvalid if it only reads).
    struct Versions {
        u32 resource = kRgInvalid;
        u32 input = 0;
        u32 output = kRgInvalid;
    };
    std::vector<Versions> versions;
    /// Resources the pass may resolve through RgContext (sorted, unique; built at endPass()).
    std::vector<u32> declared;
};

struct RgResourceDecl {
    std::string name;
    bool isTexture = true;
    bool imported = false;
    RgTextureDesc texture;
    u64 bufferSize = 0;
    rhi::TextureUsage textureUsage = rhi::TextureUsage::None;  ///< Declared (union).
    rhi::BufferUsage bufferUsage = rhi::BufferUsage::None;
    rhi::TextureUsage importTextureUsage = rhi::TextureUsage::None;  ///< What the imported resource supports.
    rhi::BufferUsage importBufferUsage = rhi::BufferUsage::None;
    rhi::TextureH importedTexture;
    rhi::BufferH importedBuffer;
    RgImport import;
    std::vector<u32> producers;  ///< producers[v] = pass producing version v (v = 0: kRgInvalid).
    std::vector<u8> outputs;     ///< outputs[v] != 0: version v was markOutput()-ed.

    u32 latestVersion() const noexcept { return static_cast<u32>(producers.size() - 1); }
    u32 mipLevels() const noexcept { return isTexture ? texture.mipLevels : 1u; }
    u32 arrayLayers() const noexcept { return isTexture ? texture.arrayLayers : 1u; }
    u32 subresourceCount() const noexcept { return mipLevels() * arrayLayers(); }
};

struct RenderGraph::Impl {
    std::string name;
    std::vector<RgPassDecl> passes;
    std::vector<std::unique_ptr<PassDataBase>> passData;
    std::vector<RgResourceDecl> resources;
    std::vector<std::string> errors;

    bool compiled = false;
    RgCompileOptions options;
    RgPlan plan;
    RgPlan executed;
    std::atomic<u64> contextErrors{0};

    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        errors.push_back(std::format(fmt, std::forward<Args>(args)...));
    }
};

/// Physical resource slots of RgResourcePool.
struct RgPoolTexture {
    rhi::TextureH handle;
    RgTextureDesc desc;
    rhi::TextureUsage usage = rhi::TextureUsage::None;
    std::vector<rhi::ResourceState> states;  ///< Per subresource (mip * layers + layer).
    std::array<u64, rhi::kQueueCount> lastValues{};  ///< Last timeline value per queue that used it.
    u64 lastUsedFrame = 0;  ///< Device::frameIndex() of the last execute that used it.
    bool inUse = false;
    u64 bytes = 0;
};
struct RgPoolBuffer {
    rhi::BufferH handle;
    u64 size = 0;
    rhi::BufferUsage usage = rhi::BufferUsage::None;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    std::array<u64, rhi::kQueueCount> lastValues{};
    u64 lastUsedFrame = 0;
    bool inUse = false;
};

struct RgResourcePool::Impl {
    rhi::Device* device = nullptr;
    u32 keepFrames = 3;
    std::vector<RgPoolTexture> textures;
    std::vector<RgPoolBuffer> buffers;
};

/// Subresource index of (mip, layer).
inline u32 rgSub(u32 mip, u32 layer, u32 layers) noexcept { return mip * layers + layer; }

/// Merges per-subresource transitions into ranged barriers (grouped by physical/before/after in
/// first-appearance order; per mip, runs of layers; consecutive mips with identical runs merged).
struct RgSubBarrier {
    u32 physical = kRgInvalid;
    rhi::ResourceState before = rhi::ResourceState::Undefined;
    rhi::ResourceState after = rhi::ResourceState::Undefined;
    u32 mip = 0;
    u32 layer = 0;
};
std::vector<RgBarrier> rgMergeBarriers(std::span<const RgSubBarrier> subs, std::span<const RgPhysicalInfo> physicals);

/// "Raster", "Compute", "AsyncCompute", "Copy".
std::string_view rgPassKindName(PassFlags flags);
/// True for states in which nothing can be written (entry barriers into them may be dropped).
bool rgIsReadOnlyState(rhi::ResourceState state) noexcept;

} // namespace helios::render
