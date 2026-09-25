// Backend-neutral enums -> Vulkan.

#include "vk_common.h"

namespace helios::rhi::vk {

std::string_view resultName(VkResult r) noexcept {
    switch (r) {
#define HELIOS_VK_RESULT(x) \
    case x: return #x;
        HELIOS_VK_RESULT(VK_SUCCESS)
        HELIOS_VK_RESULT(VK_NOT_READY)
        HELIOS_VK_RESULT(VK_TIMEOUT)
        HELIOS_VK_RESULT(VK_EVENT_SET)
        HELIOS_VK_RESULT(VK_EVENT_RESET)
        HELIOS_VK_RESULT(VK_INCOMPLETE)
        HELIOS_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
        HELIOS_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        HELIOS_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
        HELIOS_VK_RESULT(VK_ERROR_DEVICE_LOST)
        HELIOS_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
        HELIOS_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        HELIOS_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
        HELIOS_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        HELIOS_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
        HELIOS_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS)
        HELIOS_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED)
        HELIOS_VK_RESULT(VK_ERROR_FRAGMENTED_POOL)
        HELIOS_VK_RESULT(VK_ERROR_OUT_OF_POOL_MEMORY)
        HELIOS_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
        HELIOS_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        HELIOS_VK_RESULT(VK_SUBOPTIMAL_KHR)
        HELIOS_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
        HELIOS_VK_RESULT(VK_ERROR_VALIDATION_FAILED_EXT)
#undef HELIOS_VK_RESULT
    default: return "VK_ERROR_(unknown)";
    }
}

VkFormat toVkFormat(Format format) noexcept {
    switch (format) {
    case Format::Unknown: return VK_FORMAT_UNDEFINED;
    case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
    case Format::R8Snorm: return VK_FORMAT_R8_SNORM;
    case Format::R8Uint: return VK_FORMAT_R8_UINT;
    case Format::RG8Unorm: return VK_FORMAT_R8G8_UNORM;
    case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::RGBA8Snorm: return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::RGBA8Uint: return VK_FORMAT_R8G8B8A8_UINT;
    case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16Unorm: return VK_FORMAT_R16_UNORM;
    case Format::R16Float: return VK_FORMAT_R16_SFLOAT;
    case Format::R16Uint: return VK_FORMAT_R16_UINT;
    case Format::RG16Unorm: return VK_FORMAT_R16G16_UNORM;
    case Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA16Uint: return VK_FORMAT_R16G16B16A16_UINT;
    case Format::R32Float: return VK_FORMAT_R32_SFLOAT;
    case Format::R32Uint: return VK_FORMAT_R32_UINT;
    case Format::R32Sint: return VK_FORMAT_R32_SINT;
    case Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RG32Uint: return VK_FORMAT_R32G32_UINT;
    case Format::RGB32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::RGBA32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::RGB10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::RG11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::RGB9E5Float: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case Format::D16Unorm: return VK_FORMAT_D16_UNORM;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32FloatS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC1Srgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC3Srgb: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::BC4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::BC4Snorm: return VK_FORMAT_BC4_SNORM_BLOCK;
    case Format::BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::BC5Snorm: return VK_FORMAT_BC5_SNORM_BLOCK;
    case Format::BC6HUfloat: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case Format::BC6HSfloat: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case Format::BC7Unorm: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::BC7Srgb: return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::Count: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format fromVkFormat(VkFormat format) noexcept {
    for (u32 i = 1; i < static_cast<u32>(Format::Count); ++i) {
        if (toVkFormat(static_cast<Format>(i)) == format) return static_cast<Format>(i);
    }
    return Format::Unknown;
}

VkImageAspectFlags aspectOf(Format format) noexcept {
    switch (formatInfo(format).kind) {
    case FormatKind::Depth: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case FormatKind::DepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

namespace {
constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
} // namespace

StateInfo stateInfo(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined:
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
    case ResourceState::General:
        return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::CopySource:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case ResourceState::CopyDest:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    case ResourceState::VertexBuffer:
        return {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::IndexBuffer:
        return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::IndirectArgument:
        return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::ConstantBuffer:
        return {kShaderStages, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::ShaderResource:
        return {kShaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    case ResourceState::UnorderedAccess:
        return {kShaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::RenderTarget:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL};
    case ResourceState::DepthWrite:
        return {kDepthStages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL};
    case ResourceState::DepthRead:
        return {kDepthStages | kShaderStages,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    case ResourceState::Present:
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    case ResourceState::HostRead:
        return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::Count: break;
    }
    return {};
}

VkPipelineStageFlags2 maskStages(VkPipelineStageFlags2 stages, VkQueueFlags queueFlags) noexcept {
    if (queueFlags & VK_QUEUE_GRAPHICS_BIT) return stages;
    VkPipelineStageFlags2 allowed = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT |
                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT |
                                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    if (queueFlags & VK_QUEUE_COMPUTE_BIT) {
        allowed |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    return (stages & ~allowed) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : stages;
}

VkFilter toVkFilter(Filter f) noexcept { return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }
VkSamplerMipmapMode toVkMipmapMode(Filter f) noexcept {
    return f == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
VkSamplerAddressMode toVkAddressMode(AddressMode m) noexcept {
    switch (m) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
VkCompareOp toVkCompareOp(CompareOp op) noexcept {
    switch (op) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}
VkBorderColor toVkBorderColor(BorderColor c) noexcept {
    switch (c) {
    case BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case BorderColor::OpaqueBlack: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case BorderColor::OpaqueWhite: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}
VkSamplerReductionMode toVkReduction(SamplerReduction r) noexcept {
    switch (r) {
    case SamplerReduction::WeightedAverage: return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    case SamplerReduction::Min: return VK_SAMPLER_REDUCTION_MODE_MIN;
    case SamplerReduction::Max: return VK_SAMPLER_REDUCTION_MODE_MAX;
    }
    return VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
}
VkPrimitiveTopology toVkTopology(PrimitiveTopology t) noexcept {
    switch (t) {
    case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
VkCullModeFlags toVkCullMode(CullMode m) noexcept {
    switch (m) {
    case CullMode::None: return VK_CULL_MODE_NONE;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}
// The negative-height viewport flips Y, which also flips the winding Vulkan computes in framebuffer
// space; the two cancel, so the math module's CCW front faces map to VK_FRONT_FACE_COUNTER_CLOCKWISE
// (verified by the winding golden test).
VkFrontFace toVkFrontFace(FrontFace f) noexcept {
    return f == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}
VkPolygonMode toVkPolygonMode(PolygonMode m) noexcept {
    return m == PolygonMode::Line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}
VkBlendFactor toVkBlendFactor(BlendFactor f) noexcept {
    switch (f) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
VkBlendOp toVkBlendOp(BlendOp op) noexcept {
    switch (op) {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}
VkAttachmentLoadOp toVkLoadOp(LoadOp op) noexcept {
    switch (op) {
    case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}
VkAttachmentStoreOp toVkStoreOp(StoreOp op) noexcept {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
VkImageViewType toVkViewType(ViewType type) noexcept {
    switch (type) {
    case ViewType::Default:
    case ViewType::Tex2D: return VK_IMAGE_VIEW_TYPE_2D;
    case ViewType::Tex2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case ViewType::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case ViewType::CubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case ViewType::Tex3D: return VK_IMAGE_VIEW_TYPE_3D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}
VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) noexcept {
    // Every buffer has a device address (BDA is baseline, 03 §1.1).
    VkBufferUsageFlags f = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (hasFlag(usage, BufferUsage::Vertex)) f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasFlag(usage, BufferUsage::Index)) f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasFlag(usage, BufferUsage::Uniform)) f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasFlag(usage, BufferUsage::Storage)) f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasFlag(usage, BufferUsage::Indirect)) f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (hasFlag(usage, BufferUsage::TransferSrc)) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (hasFlag(usage, BufferUsage::TransferDst)) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return f;
}
VkImageUsageFlags toVkImageUsage(TextureUsage usage) noexcept {
    VkImageUsageFlags f = 0;
    if (hasFlag(usage, TextureUsage::Sampled)) f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (hasFlag(usage, TextureUsage::Storage)) f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (hasFlag(usage, TextureUsage::ColorAttachment)) f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (hasFlag(usage, TextureUsage::DepthStencil)) f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (hasFlag(usage, TextureUsage::TransferSrc)) f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (hasFlag(usage, TextureUsage::TransferDst)) f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
}
VkSampleCountFlagBits toVkSamples(u32 count) noexcept {
    switch (count) {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return static_cast<VkSampleCountFlagBits>(0);  // matches no supported-counts mask
    }
}
VkPresentModeKHR toVkPresentMode(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
    case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
    case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

} // namespace helios::rhi::vk
