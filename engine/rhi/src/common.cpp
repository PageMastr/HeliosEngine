// Backend-independent parts of the RHI: names, format tables, environment overrides, descriptor
// validation, SPIR-V inspection and Device::create dispatch.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "helios/rhi/caps.h"
#include "helios/rhi/format.h"
#include "rhi_internal.h"

namespace helios::rhi {

std::string_view backendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::Null: return "Null";
    case Backend::Vulkan: return "Vulkan";
    case Backend::D3D12: return "D3D12";
    }
    return "?";
}

std::string_view queueName(Queue queue) noexcept {
    switch (queue) {
    case Queue::Graphics: return "Graphics";
    case Queue::AsyncCompute: return "AsyncCompute";
    case Queue::Transfer: return "Transfer";
    }
    return "?";
}

std::string_view adapterTypeName(AdapterType type) noexcept {
    switch (type) {
    case AdapterType::Other: return "Other";
    case AdapterType::Integrated: return "Integrated";
    case AdapterType::Discrete: return "Discrete";
    case AdapterType::Virtual: return "Virtual";
    case AdapterType::Cpu: return "Cpu";
    }
    return "?";
}

std::string_view capBitName(CapBit bit) noexcept {
    switch (bit) {
    case CapBit::None: return "None";
    case CapBit::MeshShader: return "MeshShader";
    case CapBit::RayQuery: return "RayQuery";
    case CapBit::AccelerationStructure: return "AccelerationStructure";
    case CapBit::DescriptorBuffer: return "DescriptorBuffer";
    case CapBit::GraphicsPipelineLibrary: return "GraphicsPipelineLibrary";
    case CapBit::PipelineBinary: return "PipelineBinary";
    case CapBit::MemoryBudget: return "MemoryBudget";
    case CapBit::MemoryPriority: return "MemoryPriority";
    case CapBit::DeviceFault: return "DeviceFault";
    case CapBit::DebugUtils: return "DebugUtils";
    case CapBit::AsyncComputeQueue: return "AsyncComputeQueue";
    case CapBit::TransferQueue: return "TransferQueue";
    case CapBit::TimestampQueries: return "TimestampQueries";
    case CapBit::ImageInt64Atomics: return "ImageInt64Atomics";
    case CapBit::StorageImageWithoutFormat: return "StorageImageWithoutFormat";
    case CapBit::PresentWait: return "PresentWait";
    case CapBit::CalibratedTimestamps: return "CalibratedTimestamps";
    case CapBit::FragmentShadingRate: return "FragmentShadingRate";
    case CapBit::ValidationLayer: return "ValidationLayer";
    case CapBit::Swapchain: return "Swapchain";
    case CapBit::SamplerAnisotropy: return "SamplerAnisotropy";
    case CapBit::ShaderInt64: return "ShaderInt64";
    case CapBit::ShaderFloat16: return "ShaderFloat16";
    case CapBit::FillModeNonSolid: return "FillModeNonSolid";
    }
    return "";
}

std::string_view resourceStateName(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined: return "Undefined";
    case ResourceState::General: return "General";
    case ResourceState::CopySource: return "CopySource";
    case ResourceState::CopyDest: return "CopyDest";
    case ResourceState::VertexBuffer: return "VertexBuffer";
    case ResourceState::IndexBuffer: return "IndexBuffer";
    case ResourceState::IndirectArgument: return "IndirectArgument";
    case ResourceState::ConstantBuffer: return "ConstantBuffer";
    case ResourceState::ShaderResource: return "ShaderResource";
    case ResourceState::UnorderedAccess: return "UnorderedAccess";
    case ResourceState::RenderTarget: return "RenderTarget";
    case ResourceState::DepthWrite: return "DepthWrite";
    case ResourceState::DepthRead: return "DepthRead";
    case ResourceState::Present: return "Present";
    case ResourceState::HostRead: return "HostRead";
    case ResourceState::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------------------------
namespace {
using FK = FormatKind;
constexpr FormatInfo fmt(std::string_view name, u8 bytes, u8 channels, FormatKind kind) noexcept {
    return FormatInfo{name, bytes, 1, 1, channels, kind, false};
}
constexpr FormatInfo bc(std::string_view name, u8 bytes, u8 channels, FormatKind kind) noexcept {
    return FormatInfo{name, bytes, 4, 4, channels, kind, true};
}

constexpr std::array<FormatInfo, static_cast<usize>(Format::Count)> kFormats = {{
    FormatInfo{},  // Unknown
    fmt("R8Unorm", 1, 1, FK::Unorm),
    fmt("R8Snorm", 1, 1, FK::Snorm),
    fmt("R8Uint", 1, 1, FK::Uint),
    fmt("RG8Unorm", 2, 2, FK::Unorm),
    fmt("RGBA8Unorm", 4, 4, FK::Unorm),
    fmt("RGBA8Srgb", 4, 4, FK::Srgb),
    fmt("RGBA8Snorm", 4, 4, FK::Snorm),
    fmt("RGBA8Uint", 4, 4, FK::Uint),
    fmt("BGRA8Unorm", 4, 4, FK::Unorm),
    fmt("BGRA8Srgb", 4, 4, FK::Srgb),
    fmt("R16Unorm", 2, 1, FK::Unorm),
    fmt("R16Float", 2, 1, FK::Float),
    fmt("R16Uint", 2, 1, FK::Uint),
    fmt("RG16Unorm", 4, 2, FK::Unorm),
    fmt("RG16Float", 4, 2, FK::Float),
    fmt("RGBA16Unorm", 8, 4, FK::Unorm),
    fmt("RGBA16Float", 8, 4, FK::Float),
    fmt("RGBA16Uint", 8, 4, FK::Uint),
    fmt("R32Float", 4, 1, FK::Float),
    fmt("R32Uint", 4, 1, FK::Uint),
    fmt("R32Sint", 4, 1, FK::Sint),
    fmt("RG32Float", 8, 2, FK::Float),
    fmt("RG32Uint", 8, 2, FK::Uint),
    fmt("RGB32Float", 12, 3, FK::Float),
    fmt("RGBA32Float", 16, 4, FK::Float),
    fmt("RGBA32Uint", 16, 4, FK::Uint),
    fmt("RGB10A2Unorm", 4, 4, FK::Unorm),
    fmt("RG11B10Float", 4, 3, FK::Float),
    fmt("RGB9E5Float", 4, 3, FK::Float),
    fmt("D16Unorm", 2, 1, FK::Depth),
    fmt("D32Float", 4, 1, FK::Depth),
    fmt("D24UnormS8Uint", 4, 2, FK::DepthStencil),
    fmt("D32FloatS8Uint", 8, 2, FK::DepthStencil),
    bc("BC1Unorm", 8, 4, FK::Unorm),
    bc("BC1Srgb", 8, 4, FK::Srgb),
    bc("BC3Unorm", 16, 4, FK::Unorm),
    bc("BC3Srgb", 16, 4, FK::Srgb),
    bc("BC4Unorm", 8, 1, FK::Unorm),
    bc("BC4Snorm", 8, 1, FK::Snorm),
    bc("BC5Unorm", 16, 2, FK::Unorm),
    bc("BC5Snorm", 16, 2, FK::Snorm),
    bc("BC6HUfloat", 16, 3, FK::Float),
    bc("BC6HSfloat", 16, 3, FK::Float),
    bc("BC7Unorm", 16, 4, FK::Unorm),
    bc("BC7Srgb", 16, 4, FK::Srgb),
}};
static_assert(kFormats.size() == static_cast<usize>(Format::Count));
constexpr FormatInfo kUnknownFormat{};
} // namespace

const FormatInfo& formatInfo(Format format) noexcept {
    const auto i = static_cast<usize>(format);
    return i < kFormats.size() ? kFormats[i] : kUnknownFormat;
}

Format linearFormat(Format format) noexcept {
    switch (format) {
    case Format::RGBA8Srgb: return Format::RGBA8Unorm;
    case Format::BGRA8Srgb: return Format::BGRA8Unorm;
    case Format::BC1Srgb: return Format::BC1Unorm;
    case Format::BC3Srgb: return Format::BC3Unorm;
    case Format::BC7Srgb: return Format::BC7Unorm;
    default: return format;
    }
}

u64 formatRowBytes(Format format, u32 width) noexcept {
    const FormatInfo& info = formatInfo(format);
    if (info.blockBytes == 0) return 0;
    const u64 blocks = (static_cast<u64>(width) + info.blockWidth - 1) / info.blockWidth;
    return blocks * info.blockBytes;
}

u64 formatSurfaceBytes(Format format, u32 width, u32 height, u32 depth) noexcept {
    const FormatInfo& info = formatInfo(format);
    if (info.blockBytes == 0) return 0;
    const u64 rows = (static_cast<u64>(height) + info.blockHeight - 1) / info.blockHeight;
    return formatRowBytes(format, width) * rows * std::max<u64>(depth, 1);
}

// ---------------------------------------------------------------------------------------------
// Device::create dispatch
// ---------------------------------------------------------------------------------------------
Result<std::unique_ptr<Device>> Device::create(const DeviceDesc& desc) {
    const DeviceDesc resolved = detail::applyEnvironment(desc);
    switch (resolved.backend) {
    case Backend::Null: return detail::createNullDevice(resolved);
    case Backend::Vulkan: return detail::createVulkanDevice(resolved);
    case Backend::D3D12: break;
    }
    return Error{ErrorCode::Unsupported, std::string(backendName(resolved.backend)) + " backend is not implemented"};
}

Result<std::vector<AdapterInfo>> Device::enumerateAdapters(Backend backend) {
    switch (backend) {
    case Backend::Null: return std::vector<AdapterInfo>{detail::nullAdapterInfo()};
    case Backend::Vulkan: return detail::enumerateVulkanAdapters();
    case Backend::D3D12: break;
    }
    return Error{ErrorCode::Unsupported, "D3D12 backend is not implemented"};
}

namespace detail {

std::optional<std::string> envVar(const char* name) {
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read-only use
    if (!value || !*value) return std::nullopt;
    return std::string(value);
}

DeviceDesc applyEnvironment(const DeviceDesc& desc) {
    DeviceDesc out = desc;
    if (auto v = envVar("HELIOS_RHI_VALIDATION")) {
        out.validation = (*v != "0" && *v != "off" && *v != "false");
    }
    if (auto v = envVar("HELIOS_RHI_INJECT_DEVICE_LOST")) {
        out.debugDeviceLostAfterSubmits = static_cast<u32>(std::strtoul(v->c_str(), nullptr, 10));
    }
    if (auto v = envVar("HELIOS_RHI_CAPS_MASK")) {
        char* end = nullptr;
        const unsigned long long mask = std::strtoull(v->c_str(), &end, 0);
        if (end && *end == '\0') {
            out.capsMask = out.capsMask & static_cast<CapBit>(mask);
        } else {
            HELIOS_LOG_WARN(LogRhi, "Ignoring malformed HELIOS_RHI_CAPS_MASK='{}'", *v);
        }
    }
    return out;
}

MemoryTag defaultBufferTag() {
    static const MemoryTag tag = registerMemoryTag("GpuBuffers");
    return tag;
}

MemoryTag defaultTextureTag() {
    static const MemoryTag tag = registerMemoryTag("GpuTextures");
    return tag;
}

// ---------------------------------------------------------------------------------------------
// Descriptor validation
// ---------------------------------------------------------------------------------------------
Result<void> validateBufferDesc(const BufferDesc& desc) {
    if (desc.size == 0) return makeError(ErrorCode::InvalidArgument, "buffer '{}': size is 0", desc.name);
    if (desc.usage == BufferUsage::None) {
        return makeError(ErrorCode::InvalidArgument, "buffer '{}': no usage flags", desc.name);
    }
    return {};
}

Result<void> validateTextureDesc(const TextureDesc& desc, const Limits& limits) {
    const FormatInfo& info = formatInfo(desc.format);
    if (info.blockBytes == 0) return makeError(ErrorCode::InvalidArgument, "texture '{}': unknown format", desc.name);
    if (desc.width == 0 || desc.height == 0 || desc.depth == 0 || desc.mipLevels == 0 || desc.arrayLayers == 0) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': zero extent, mip or layer count", desc.name);
    }
    if (desc.usage == TextureUsage::None) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': no usage flags", desc.name);
    }
    if (desc.type != TextureType::Tex3D && desc.depth != 1) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': depth > 1 needs TextureType::Tex3D", desc.name);
    }
    if (desc.type == TextureType::Tex3D && desc.arrayLayers != 1) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': 3D textures cannot have layers", desc.name);
    }
    if (desc.type == TextureType::Tex3D &&
        hasAnyFlag(desc.usage, TextureUsage::ColorAttachment | TextureUsage::DepthStencil)) {
        // Attachments are single 2D slices; rendering into 3D slices is not supported (write
        // volumes from compute through uav()).
        return makeError(ErrorCode::Unsupported, "texture '{}': 3D textures cannot be attachments", desc.name);
    }
    if (desc.type == TextureType::Cube && (desc.arrayLayers % 6 != 0 || desc.width != desc.height)) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': cube needs square faces and 6*N layers", desc.name);
    }
    u32 maxDim = std::max({desc.width, desc.height, desc.depth});
    u32 fullChain = 1;
    while (maxDim > 1) {
        maxDim >>= 1;
        ++fullChain;
    }
    if (desc.mipLevels > fullChain) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': {} mips exceed the full chain ({})", desc.name,
                         desc.mipLevels, fullChain);
    }
    const bool depth = isDepthFormat(desc.format);
    if (depth && hasFlag(desc.usage, TextureUsage::ColorAttachment)) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': depth format used as color attachment", desc.name);
    }
    if (!depth && hasFlag(desc.usage, TextureUsage::DepthStencil)) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': color format used as depth attachment", desc.name);
    }
    if (info.compressed && hasAnyFlag(desc.usage, TextureUsage::ColorAttachment | TextureUsage::DepthStencil |
                                                      TextureUsage::Storage)) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': compressed formats are sample/copy only", desc.name);
    }
    if (desc.sampleCount == 0 || desc.sampleCount > 64 || (desc.sampleCount & (desc.sampleCount - 1)) != 0) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': sample count {} is not a power of two <= 64",
                         desc.name, desc.sampleCount);
    }
    if (desc.sampleCount > 1 && (desc.mipLevels != 1 || desc.type != TextureType::Tex2D ||
                                 hasFlag(desc.usage, TextureUsage::Storage))) {
        return makeError(ErrorCode::InvalidArgument, "texture '{}': MSAA textures are single-mip 2D, no storage",
                         desc.name);
    }
    if (limits.maxTextureDimension2D != 0) {
        const u32 limit = desc.type == TextureType::Tex3D ? limits.maxTextureDimension3D : limits.maxTextureDimension2D;
        if (desc.width > limit || desc.height > limit || desc.depth > limit) {
            return makeError(ErrorCode::LimitExceeded, "texture '{}': {}x{}x{} exceeds device limit {}", desc.name,
                             desc.width, desc.height, desc.depth, limit);
        }
        if (limits.maxTextureArrayLayers != 0 && desc.arrayLayers > limits.maxTextureArrayLayers) {
            return makeError(ErrorCode::LimitExceeded, "texture '{}': {} layers exceed device limit {}", desc.name,
                             desc.arrayLayers, limits.maxTextureArrayLayers);
        }
    }
    return {};
}

Result<void> validateGraphicsPipelineDesc(const GraphicsPipelineDesc& desc) {
    HELIOS_TRY(validateShader(desc.vertex, ShaderStage::Vertex, desc.name));
    if (!desc.fragment.spirv.empty()) HELIOS_TRY(validateShader(desc.fragment, ShaderStage::Fragment, desc.name));
    if (desc.colorCount > kMaxColorAttachments) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': {} color attachments (max {})", desc.name,
                         desc.colorCount, kMaxColorAttachments);
    }
    for (u32 i = 0; i < desc.colorCount; ++i) {
        const Format f = desc.colorFormats[i];
        if (f == Format::Unknown || isDepthFormat(f) || isCompressedFormat(f)) {
            return makeError(ErrorCode::InvalidArgument, "pipeline '{}': color attachment {} has format {}", desc.name,
                             i, formatName(f).empty() ? std::string_view("Unknown") : formatName(f));
        }
    }
    if (desc.depthFormat != Format::Unknown && !isDepthFormat(desc.depthFormat)) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': depth format {} is not a depth format", desc.name,
                         formatName(desc.depthFormat));
    }
    if ((desc.depth.testEnable || desc.depth.writeEnable) && desc.depthFormat == Format::Unknown) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': depth test/write without a depth format",
                         desc.name);
    }
    if (desc.colorCount == 0 && desc.depthFormat == Format::Unknown) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': no attachments", desc.name);
    }
    if (desc.sampleCount == 0 || desc.sampleCount > 64 || (desc.sampleCount & (desc.sampleCount - 1)) != 0) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': invalid sample count {}", desc.name,
                         desc.sampleCount);
    }
    return {};
}

Result<void> validateComputePipelineDesc(const ComputePipelineDesc& desc) {
    return validateShader(desc.compute, ShaderStage::Compute, desc.name);
}

Result<ViewDesc> resolveView(const TextureDesc& texture, const ViewDesc& view) {
    ViewDesc out = view;
    if (view.baseMip >= texture.mipLevels || view.baseLayer >= texture.arrayLayers) {
        return makeError(ErrorCode::OutOfRange, "view of '{}': base mip/layer out of range", texture.name);
    }
    if (out.mipCount == kAllMips) out.mipCount = texture.mipLevels - view.baseMip;
    if (out.layerCount == kAllLayers) out.layerCount = texture.arrayLayers - view.baseLayer;
    if (out.mipCount == 0 || out.layerCount == 0 || view.baseMip + out.mipCount > texture.mipLevels ||
        view.baseLayer + out.layerCount > texture.arrayLayers) {
        return makeError(ErrorCode::OutOfRange, "view of '{}': mip/layer range out of range", texture.name);
    }
    if (out.type == ViewType::Default) {
        switch (texture.type) {
        case TextureType::Tex2D: out.type = out.layerCount > 1 ? ViewType::Tex2DArray : ViewType::Tex2D; break;
        case TextureType::Tex3D: out.type = ViewType::Tex3D; break;
        case TextureType::Cube: out.type = out.layerCount > 6 ? ViewType::CubeArray : ViewType::Cube; break;
        }
    }
    const bool ok = [&] {
        switch (out.type) {
        case ViewType::Tex2D: return texture.type != TextureType::Tex3D && out.layerCount == 1;
        case ViewType::Tex2DArray: return texture.type != TextureType::Tex3D;
        case ViewType::Cube: return texture.type == TextureType::Cube && out.layerCount == 6;
        case ViewType::CubeArray: return texture.type == TextureType::Cube && out.layerCount % 6 == 0;
        case ViewType::Tex3D: return texture.type == TextureType::Tex3D;
        case ViewType::Default: return false;
        }
        return false;
    }();
    if (!ok) return makeError(ErrorCode::InvalidArgument, "view of '{}': view type does not fit the texture", texture.name);
    return out;
}

Result<TextureRegion> resolveCopyRegion(const TextureDesc& texture, const TextureRegion& region,
                                        const BufferTextureLayout& layout, u64* bufferBytes) {
    const FormatInfo& info = formatInfo(texture.format);
    if (info.blockBytes == 0) return makeError(ErrorCode::InvalidArgument, "copy of '{}': unknown format", texture.name);
    if (texture.sampleCount > 1) {
        return makeError(ErrorCode::InvalidArgument, "copy of '{}': multisampled textures cannot be copied (resolve first)",
                         texture.name);
    }
    if (texture.format == Format::D32FloatS8Uint) {
        // Its depth aspect copies as 4-byte texels, not the 8-byte interleaved layout FormatInfo describes.
        return makeError(ErrorCode::Unsupported, "copy of '{}': D32FloatS8Uint has no packed buffer layout",
                         texture.name);
    }
    if (region.mip >= texture.mipLevels || region.layerCount == 0 || region.baseLayer >= texture.arrayLayers ||
        region.layerCount > texture.arrayLayers - region.baseLayer) {
        return makeError(ErrorCode::OutOfRange, "copy of '{}': mip {} / layers {}+{} out of range", texture.name,
                         region.mip, region.baseLayer, region.layerCount);
    }
    TextureRegion r = region;
    const u32 w = mipExtent(texture.width, r.mip);
    const u32 h = mipExtent(texture.height, r.mip);
    const u32 d = texture.type == TextureType::Tex3D ? mipExtent(texture.depth, r.mip) : 1;
    if (r.x >= w || r.y >= h || r.z >= d) {
        return makeError(ErrorCode::OutOfRange, "copy of '{}': region origin {},{},{} outside mip {} ({}x{}x{})",
                         texture.name, r.x, r.y, r.z, r.mip, w, h, d);
    }
    if (r.width == 0) r.width = w - r.x;
    if (r.height == 0) r.height = h - r.y;
    if (r.depth == 0) r.depth = d - r.z;
    if (r.width > w - r.x || r.height > h - r.y || r.depth > d - r.z) {
        return makeError(ErrorCode::OutOfRange, "copy of '{}': region {},{},{} {}x{}x{} exceeds mip {} ({}x{}x{})",
                         texture.name, r.x, r.y, r.z, r.width, r.height, r.depth, r.mip, w, h, d);
    }
    if (r.x % info.blockWidth || r.y % info.blockHeight || ((r.x + r.width) % info.blockWidth && r.x + r.width != w) ||
        ((r.y + r.height) % info.blockHeight && r.y + r.height != h)) {
        return makeError(ErrorCode::InvalidArgument, "copy of '{}': region is not aligned to {}x{} blocks", texture.name,
                         info.blockWidth, info.blockHeight);
    }
    const u64 rowBytes = formatRowBytes(texture.format, r.width);
    if (layout.rowPitch != 0 && (layout.rowPitch < rowBytes || layout.rowPitch % info.blockBytes != 0)) {
        return makeError(ErrorCode::InvalidArgument, "copy of '{}': rowPitch {} invalid for {}-byte rows of {}-byte blocks",
                         texture.name, layout.rowPitch, rowBytes, info.blockBytes);
    }
    // Vulkan: bufferOffset must be a multiple of the texel block size (4 for depth/stencil formats).
    const u64 offsetAlign = isDepthFormat(texture.format) ? 4 : info.blockBytes;
    if (layout.offset % offsetAlign != 0) {
        return makeError(ErrorCode::InvalidArgument, "copy of '{}': buffer offset {} is not a multiple of {}",
                         texture.name, layout.offset, offsetAlign);
    }
    if (bufferBytes) {
        const u64 pitch = layout.rowPitch ? layout.rowPitch : rowBytes;
        const u64 rows = (static_cast<u64>(r.height) + info.blockHeight - 1) / info.blockHeight;
        *bufferBytes = pitch * rows * r.depth * r.layerCount;
    }
    return r;
}

SubresourceRange resolveRange(const TextureDesc& texture, const SubresourceRange& range) {
    SubresourceRange out = range;
    out.baseMip = std::min(range.baseMip, texture.mipLevels);
    out.baseLayer = std::min(range.baseLayer, texture.arrayLayers);
    const u32 mipsLeft = texture.mipLevels - out.baseMip;
    const u32 layersLeft = texture.arrayLayers - out.baseLayer;
    out.mipCount = range.mipCount == kAllMips ? mipsLeft : std::min(range.mipCount, mipsLeft);
    out.layerCount = range.layerCount == kAllLayers ? layersLeft : std::min(range.layerCount, layersLeft);
    return out;
}

// ---------------------------------------------------------------------------------------------
// SPIR-V inspection
// ---------------------------------------------------------------------------------------------
namespace {
constexpr u32 kSpirvMagic = 0x07230203u;
constexpr u32 kOpEntryPoint = 15;
constexpr u32 kOpFunction = 54;

ShaderStage stageFromExecutionModel(u32 model) noexcept {
    switch (model) {
    case 0: return ShaderStage::Vertex;    // Vertex
    case 4: return ShaderStage::Fragment;  // Fragment
    case 5: return ShaderStage::Compute;   // GLCompute
    default: return ShaderStage::Other;
    }
}

std::string_view stageName(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    case ShaderStage::Other: return "other";
    }
    return "?";
}
} // namespace

Result<std::vector<SpirvEntryPoint>> spirvEntryPoints(std::span<const u32> words) {
    if (words.size() < 5 || words[0] != kSpirvMagic) {
        return Error{ErrorCode::Corrupt, "not a SPIR-V module (bad magic or too short)"};
    }
    std::vector<SpirvEntryPoint> out;
    usize i = 5;
    while (i < words.size()) {
        const u32 wordCount = words[i] >> 16;
        const u32 opcode = words[i] & 0xFFFFu;
        if (wordCount == 0 || i + wordCount > words.size()) {
            return Error{ErrorCode::Corrupt, "truncated SPIR-V instruction stream"};
        }
        if (opcode == kOpFunction) break;  // entry points precede all function definitions
        if (opcode == kOpEntryPoint && wordCount >= 4) {
            SpirvEntryPoint ep;
            ep.stage = stageFromExecutionModel(words[i + 1]);
            // Literal string: little-endian bytes packed into words, NUL-terminated.
            const char* bytes = reinterpret_cast<const char*>(&words[i + 3]);
            const usize maxBytes = static_cast<usize>(wordCount - 3) * 4;
            const void* nul = std::memchr(bytes, 0, maxBytes);
            ep.name.assign(bytes, nul ? static_cast<usize>(static_cast<const char*>(nul) - bytes) : maxBytes);
            out.push_back(std::move(ep));
        }
        i += wordCount;
    }
    return out;
}

Result<void> validateShader(const ShaderDesc& shader, ShaderStage stage, std::string_view what) {
    if (shader.spirv.empty()) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': missing {} shader", what, stageName(stage));
    }
    auto entries = spirvEntryPoints(shader.spirv);
    if (!entries) {
        return makeError(ErrorCode::InvalidArgument, "pipeline '{}': {} shader: {}", what, stageName(stage),
                         entries.error().message);
    }
    for (const SpirvEntryPoint& ep : *entries) {
        if (ep.name == shader.entryPoint) {
            if (ep.stage == stage) return {};
            return makeError(ErrorCode::InvalidArgument, "pipeline '{}': entry point '{}' is a {} shader, expected {}",
                             what, shader.entryPoint, stageName(ep.stage), stageName(stage));
        }
    }
    std::string available;
    for (const SpirvEntryPoint& ep : *entries) {
        if (!available.empty()) available += ", ";
        available += ep.name;
    }
    return makeError(ErrorCode::NotFound, "pipeline '{}': no entry point '{}' in the {} shader (has: {})", what,
                     shader.entryPoint, stageName(stage), available);
}

} // namespace detail
} // namespace helios::rhi
