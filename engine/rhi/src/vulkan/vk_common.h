#pragma once
// Vulkan backend internals. No file outside src/vulkan includes Vulkan headers (03 §1.6).

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <volk.h>

#include <vk_mem_alloc.h>

#include <string_view>

#include "helios/rhi/types.h"

namespace helios::rhi::vk {

/// Human-readable VkResult name ("VK_ERROR_DEVICE_LOST").
std::string_view resultName(VkResult result) noexcept;

VkFormat toVkFormat(Format format) noexcept;
Format fromVkFormat(VkFormat format) noexcept;
VkImageAspectFlags aspectOf(Format format) noexcept;

/// Pipeline stages, access mask and image layout that a ResourceState stands for.
struct StateInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
StateInfo stateInfo(ResourceState state) noexcept;
/// Restricts a stage mask to what a queue family supports (falls back to ALL_COMMANDS).
VkPipelineStageFlags2 maskStages(VkPipelineStageFlags2 stages, VkQueueFlags queueFlags) noexcept;

VkFilter toVkFilter(Filter f) noexcept;
VkSamplerMipmapMode toVkMipmapMode(Filter f) noexcept;
VkSamplerAddressMode toVkAddressMode(AddressMode m) noexcept;
VkCompareOp toVkCompareOp(CompareOp op) noexcept;
VkBorderColor toVkBorderColor(BorderColor c) noexcept;
VkSamplerReductionMode toVkReduction(SamplerReduction r) noexcept;
VkPrimitiveTopology toVkTopology(PrimitiveTopology t) noexcept;
VkCullModeFlags toVkCullMode(CullMode m) noexcept;
VkFrontFace toVkFrontFace(FrontFace f) noexcept;
VkPolygonMode toVkPolygonMode(PolygonMode m) noexcept;
VkBlendFactor toVkBlendFactor(BlendFactor f) noexcept;
VkBlendOp toVkBlendOp(BlendOp op) noexcept;
VkAttachmentLoadOp toVkLoadOp(LoadOp op) noexcept;
VkAttachmentStoreOp toVkStoreOp(StoreOp op) noexcept;
VkImageViewType toVkViewType(ViewType type) noexcept;
VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) noexcept;
VkImageUsageFlags toVkImageUsage(TextureUsage usage) noexcept;
VkSampleCountFlagBits toVkSamples(u32 count) noexcept;
VkPresentModeKHR toVkPresentMode(PresentMode mode) noexcept;

} // namespace helios::rhi::vk
