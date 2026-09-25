#pragma once
// RHI descriptions: resources, samplers, pipelines, barriers, rendering, copies, swapchains and
// diagnostics. Everything here is a plain value type with no backend types (03 §1.6 seam rule):
// barriers are expressed as resource *states*, not API access masks.
//
// Strings (names, entry points) are std::string_view and SPIR-V is a span: the Device copies what
// it keeps, so descriptions may point at temporaries for the duration of the create call.
// Threading: plain values.

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/memory.h"
#include "helios/core/types.h"
#include "helios/rhi/format.h"
#include "helios/rhi/handles.h"

namespace helios::rhi {

inline constexpr u32 kAllMips = 0xFFFFFFFFu;
inline constexpr u32 kAllLayers = 0xFFFFFFFFu;
inline constexpr u64 kWholeSize = ~0ull;
inline constexpr u32 kMaxColorAttachments = 8;
/// Push-constant budget for every pipeline (one shared pipeline layout, 03 §1.1).
inline constexpr u32 kMaxPushConstantBytes = 128;

/// Bindings of the global bindless descriptor set (set 0); see shaders/core/bindless.slang.
enum class BindlessBinding : u32 { SampledImages = 0, StorageImages = 1, StorageBuffers = 2, Samplers = 3 };

// ---------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------
enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,   ///< Shader read/write; gets a bindless slot (srv(BufferH)).
    Indirect = 1u << 4,  ///< Indirect draw/dispatch arguments and counts.
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6, ///< Copy/fill/update destination.
};
HELIOS_ENUM_FLAGS(BufferUsage)

/// Where the memory lives. Every buffer also has a device address (BDA is baseline).
enum class MemoryUsage : u8 {
    GpuOnly,  ///< Device-local, not mappable.
    Upload,   ///< Host-visible, write-combined, persistently mapped (CPU -> GPU streaming).
    Readback, ///< Host-visible, cached, persistently mapped (GPU -> CPU).
};

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Storage;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    std::string_view name;               ///< Debug name (validation, captures, traces).
    MemoryTag tag = MemoryTag::Unknown;  ///< Budget attribution; Unknown -> the RHI's "GpuBuffers" tag.
};

// ---------------------------------------------------------------------------------------------
// Textures and views
// ---------------------------------------------------------------------------------------------
enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1u << 0,         ///< Gets a bindless sampled-image slot (srv()).
    Storage = 1u << 1,         ///< Storage image (uav(mip)).
    ColorAttachment = 1u << 2,
    DepthStencil = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
};
HELIOS_ENUM_FLAGS(TextureUsage)

/// 2D textures with arrayLayers > 1 are 2D arrays; Cube needs arrayLayers = 6 * N.
enum class TextureType : u8 { Tex2D, Tex3D, Cube };

struct TextureDesc {
    TextureType type = TextureType::Tex2D;
    Format format = Format::RGBA8Unorm;
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;       ///< Tex3D only.
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    u32 sampleCount = 1;
    TextureUsage usage = TextureUsage::Sampled;
    std::string_view name;
    MemoryTag tag = MemoryTag::Unknown;  ///< Unknown -> the RHI's "GpuTextures" tag.

    static TextureDesc tex2D(Format format, u32 width, u32 height, TextureUsage usage, std::string_view name = {},
                             u32 mipLevels = 1) noexcept {
        TextureDesc d;
        d.format = format;
        d.width = width;
        d.height = height;
        d.mipLevels = mipLevels;
        d.usage = usage;
        d.name = name;
        return d;
    }
};

enum class ViewType : u8 { Default, Tex2D, Tex2DArray, Cube, CubeArray, Tex3D };

/// Sub-view of a texture for srv(). Default = the whole resource as its natural view type.
struct ViewDesc {
    u32 baseMip = 0;
    u32 mipCount = kAllMips;
    u32 baseLayer = 0;
    u32 layerCount = kAllLayers;
    ViewType type = ViewType::Default;
    friend bool operator==(const ViewDesc&, const ViewDesc&) = default;
};

// ---------------------------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------------------------
enum class Filter : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
enum class CompareOp : u8 { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class BorderColor : u8 { TransparentBlack, OpaqueBlack, OpaqueWhite };
/// Min/Max reduction feeds the HZB downsampler (samplerFilterMinmax).
enum class SamplerReduction : u8 { WeightedAverage, Min, Max };

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    Filter mipFilter = Filter::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    f32 mipLodBias = 0.0f;
    f32 maxAnisotropy = 1.0f;  ///< <= 1 disables anisotropic filtering.
    bool compareEnable = false;
    CompareOp compareOp = CompareOp::GreaterOrEqual;
    f32 minLod = 0.0f;
    f32 maxLod = 1000.0f;
    BorderColor borderColor = BorderColor::TransparentBlack;
    SamplerReduction reduction = SamplerReduction::WeightedAverage;
    friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

// ---------------------------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------------------------
/// One entry point of a SPIR-V module (see helios_shaders() in cmake/HeliosShaders.cmake).
struct ShaderDesc {
    std::span<const u32> spirv;
    std::string_view entryPoint = "main";
};

enum class PrimitiveTopology : u8 { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class CullMode : u8 { None, Front, Back };
/// Winding as seen in the +Y-up clip space of helios::math (the backend's viewport flip is
/// accounted for), so CounterClockwise matches the math module's front faces.
enum class FrontFace : u8 { CounterClockwise, Clockwise };
enum class PolygonMode : u8 { Fill, Line };

struct RasterState {
    CullMode cullMode = CullMode::None;
    FrontFace frontFace = FrontFace::CounterClockwise;
    PolygonMode polygonMode = PolygonMode::Fill;
    bool depthClamp = false;
    f32 depthBiasConstant = 0.0f;
    f32 depthBiasSlope = 0.0f;
    f32 depthBiasClamp = 0.0f;
};

/// Defaults follow reverse-Z (03 §2.7): GreaterOrEqual, clear to 0.
struct DepthState {
    bool testEnable = false;
    bool writeEnable = false;
    CompareOp compareOp = CompareOp::GreaterOrEqual;
};

enum class BlendFactor : u8 {
    Zero, One, SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
};
enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };
enum class ColorWrite : u8 { None = 0, R = 1, G = 2, B = 4, A = 8, All = 15 };
HELIOS_ENUM_FLAGS(ColorWrite)

struct BlendState {
    bool enable = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    ColorWrite writeMask = ColorWrite::All;

    /// Premultiplied-alpha "over".
    static BlendState premultiplied() noexcept {
        BlendState b;
        b.enable = true;
        b.srcColor = BlendFactor::One;
        b.dstColor = BlendFactor::OneMinusSrcAlpha;
        b.srcAlpha = BlendFactor::One;
        b.dstAlpha = BlendFactor::OneMinusSrcAlpha;
        return b;
    }
};

/// Graphics pipeline for dynamic rendering. There is no vertex input state: geometry is pulled in
/// the vertex shader through buffer device addresses or bindless buffers (03 §0.4).
struct GraphicsPipelineDesc {
    ShaderDesc vertex;
    ShaderDesc fragment;  ///< May be empty for depth-only pipelines.
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterState raster;
    DepthState depth;
    u32 colorCount = 0;
    std::array<Format, kMaxColorAttachments> colorFormats{};
    std::array<BlendState, kMaxColorAttachments> blend{};
    Format depthFormat = Format::Unknown;
    u32 sampleCount = 1;
    std::string_view name;
};

struct ComputePipelineDesc {
    ShaderDesc compute;
    std::string_view name;
};

/// Compilation scheduling. Immediate compiles on the calling thread (ready on return); the others
/// compile on DeviceDesc::pipelineCompilePool when one is set (isReady() turns true later) and
/// behave like Immediate otherwise. Draws and dispatches with a pipeline that is not ready yet are
/// skipped and counted as PSO misses (MemoryStats::psoMisses) instead of stalling the recorder.
enum class PsoPriority : u8 { Immediate, High, Normal, Low };

// ---------------------------------------------------------------------------------------------
// Synchronization: resource states and barriers
// ---------------------------------------------------------------------------------------------
/// Logical resource states (render-graph states, 03 §1.6). A barrier moves a resource (or a
/// subresource range) from one state to another; the backend derives stages, access masks and
/// image layouts. Undefined as the source discards the previous contents.
enum class ResourceState : u8 {
    Undefined,
    General,          ///< Any access; slow path (GENERAL layout).
    CopySource,
    CopyDest,         ///< Also clearTexture / fillBuffer / updateBuffer destination.
    VertexBuffer,
    IndexBuffer,
    IndirectArgument,
    ConstantBuffer,
    ShaderResource,   ///< Sampled / read-only in any shader stage.
    UnorderedAccess,  ///< Storage read/write in any shader stage.
    RenderTarget,
    DepthWrite,
    DepthRead,        ///< Read-only depth attachment and/or sampled depth.
    Present,
    HostRead,         ///< Visible to the CPU after the submission completes (readback).
    Count
};

std::string_view resourceStateName(ResourceState state) noexcept;

struct SubresourceRange {
    u32 baseMip = 0;
    u32 mipCount = kAllMips;
    u32 baseLayer = 0;
    u32 layerCount = kAllLayers;
    friend bool operator==(const SubresourceRange&, const SubresourceRange&) = default;
};

struct Barrier {
    enum class Kind : u8 { Global, Buffer, Texture };

    Kind kind = Kind::Global;
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
    BufferH buffer;
    TextureH texture;
    u64 offset = 0;           ///< Buffer range.
    u64 size = kWholeSize;
    SubresourceRange range;   ///< Texture range.

    /// Execution + memory dependency for every resource (e.g. UAV -> UAV between dispatches).
    static Barrier global(ResourceState before, ResourceState after) noexcept {
        Barrier b;
        b.kind = Kind::Global;
        b.before = before;
        b.after = after;
        return b;
    }
    static Barrier bufferState(BufferH buffer, ResourceState before, ResourceState after, u64 offset = 0,
                               u64 size = kWholeSize) noexcept {
        Barrier b;
        b.kind = Kind::Buffer;
        b.buffer = buffer;
        b.before = before;
        b.after = after;
        b.offset = offset;
        b.size = size;
        return b;
    }
    static Barrier textureState(TextureH texture, ResourceState before, ResourceState after,
                                const SubresourceRange& range = {}) noexcept {
        Barrier b;
        b.kind = Kind::Texture;
        b.texture = texture;
        b.before = before;
        b.after = after;
        b.range = range;
        return b;
    }
};

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------
enum class LoadOp : u8 { Load, Clear, DontCare };
enum class StoreOp : u8 { Store, DontCare };

/// Color target of beginRendering(). The texture must be in ResourceState::RenderTarget.
struct ColorAttachment {
    TextureH texture;
    u32 mip = 0;
    u32 layer = 0;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    std::array<f32, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    TextureH resolve;  ///< Optional single-sample resolve target (RenderTarget state).
};

/// Depth target of beginRendering(): DepthWrite state, or DepthRead when readOnly (then `load`
/// must be Load or DontCare: clearing writes; `store` is ignored).
struct DepthAttachment {
    TextureH texture;
    u32 mip = 0;
    u32 layer = 0;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    f32 clearDepth = 0.0f;  ///< Reverse-Z: 0 is the far plane.
    u8 clearStencil = 0;
    bool readOnly = false;
};

struct Rect {
    i32 x = 0;
    i32 y = 0;
    u32 width = 0;
    u32 height = 0;
    friend bool operator==(const Rect&, const Rect&) = default;
};

/// Viewport in framebuffer pixels with the origin at the top-left. Clip-space +Y maps to the top
/// (the Vulkan backend uses a negative-height viewport, 03 §2.7 / math README).
struct Viewport {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    f32 minDepth = 0.0f;
    f32 maxDepth = 1.0f;
};

/// Dynamic-rendering pass. `area` with zero width/height covers the whole first attachment (mip
/// extent). beginRendering also sets the viewport and scissor to the area.
struct RenderingDesc {
    std::span<const ColorAttachment> colors;
    DepthAttachment depth;  ///< Unused when depth.texture is null.
    Rect area;
};

enum class IndexType : u8 { Uint16, Uint32 };

/// Indirect argument layouts (identical on Vulkan and D3D12).
struct DrawIndirectCommand {
    u32 vertexCount;
    u32 instanceCount;
    u32 firstVertex;
    u32 firstInstance;
};
struct DrawIndexedIndirectCommand {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 vertexOffset;
    u32 firstInstance;
};
struct DispatchIndirectCommand {
    u32 x;
    u32 y;
    u32 z;
};

// ---------------------------------------------------------------------------------------------
// Copies
// ---------------------------------------------------------------------------------------------
/// Texel region of one mip level. width/height/depth of 0 mean "to the end of the mip".
struct TextureRegion {
    u32 mip = 0;
    u32 baseLayer = 0;
    u32 layerCount = 1;
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
};

/// Placement of texel data in a buffer. rowPitch 0 = tightly packed rows (formatRowBytes);
/// otherwise a multiple of the format's block size. Layers/slices follow each other tightly.
struct BufferTextureLayout {
    u64 offset = 0;
    u32 rowPitch = 0;
};

// ---------------------------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------------------------
enum class PresentMode : u8 { Fifo, Mailbox, Immediate };

struct SwapchainDesc {
    void* sdlWindow = nullptr;  ///< SDL_Window* created with SDL_WINDOW_VULKAN (ignored by Null).
    u32 width = 0;              ///< Pixel size; 0 = query the surface.
    u32 height = 0;
    Format format = Format::BGRA8Srgb;  ///< Preferred; falls back to what the surface offers.
    PresentMode presentMode = PresentMode::Fifo;
    u32 imageCount = 3;
    std::string_view name;
};

enum class SwapchainStatus : u8 {
    Ok,
    Suboptimal,  ///< Usable, but resize soon (e.g. the window size changed).
    OutOfDate,   ///< Unusable until resizeSwapchain(); acquire returned no image.
};

struct SwapchainImage {
    TextureH texture;         ///< Null when status == OutOfDate.
    u32 index = 0;
    TimelinePoint ready;      ///< Wait for this in the first submit that touches `texture`.
    SwapchainStatus status = SwapchainStatus::Ok;
};

struct SwapchainInfo {
    Format format = Format::Unknown;
    u32 width = 0;
    u32 height = 0;
    u32 imageCount = 0;
    PresentMode presentMode = PresentMode::Fifo;
    TextureUsage usage = TextureUsage::None;  ///< What the images support (ColorAttachment at least).
};

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------
struct MemoryHeapStats {
    u64 budgetBytes = 0;  ///< Driver budget (VK_EXT_memory_budget) or heap size.
    u64 usageBytes = 0;
    bool deviceLocal = false;
};

struct MemoryStats {
    std::vector<MemoryHeapStats> heaps;
    u64 bufferBytes = 0;
    u64 textureBytes = 0;
    u32 bufferCount = 0;
    u32 textureCount = 0;
    u32 pipelineCount = 0;
    u64 psoMisses = 0;  ///< Draws/dispatches skipped because their pipeline was still compiling.
};

enum class BreadcrumbStage : u8 { Begin, End };

/// Last pass markers a queue reached (see CommandList::breadcrumb). Values are
/// (frameIndex & 0xFFFF) << 16 | passId; 0 = none yet.
struct BreadcrumbState {
    u32 lastBegin = 0;
    u32 lastEnd = 0;
};

struct ValidationMessage {
    enum class Severity : u8 { Info, Warning, Error };
    Severity severity = Severity::Info;
    std::string text;
};

/// Everything known when the GPU was lost (03 §1.5); passed to DeviceDesc::onDeviceLost.
struct DeviceLostInfo {
    std::string reason;
    std::string faultDescription;          ///< VK_EXT_device_fault vendor description, if any.
    std::vector<std::string> faultDetails;  ///< Address / vendor fault records, formatted.
    std::array<BreadcrumbState, kQueueCount> breadcrumbs{};
    std::array<u64, kQueueCount> submittedValues{};
    std::array<u64, kQueueCount> completedValues{};
};

} // namespace helios::rhi
