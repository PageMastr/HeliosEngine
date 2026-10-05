// Resource states -> synchronization2 masks, per queue family (see vk_sync.h).

#include "vk_sync.h"

namespace helios::rhi::vk {
namespace {

constexpr VkPipelineStageFlags2 kShaderStages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// Every queue.
constexpr VkPipelineStageFlags2 kAnyQueueStages = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT |
                                                  VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
// Graphics, compute or transfer queues (graphics and compute families support transfers implicitly).
constexpr VkPipelineStageFlags2 kTransferStages = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                                  VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                                                  VK_PIPELINE_STAGE_2_CLEAR_BIT;
// Graphics or compute queues.
constexpr VkPipelineStageFlags2 kGraphicsOrComputeStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
// Compute queues.
constexpr VkPipelineStageFlags2 kComputeStages = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
// Shader stages of the graphics pipeline.
constexpr VkPipelineStageFlags2 kPreRasterShaders =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
    VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
    VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
constexpr VkPipelineStageFlags2 kVertexInput =
    VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
// Graphics queues: every stage ALL_GRAPHICS stands for, and the meta stages themselves.
constexpr VkPipelineStageFlags2 kGraphicsPipelineStages =
    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | kVertexInput | kPreRasterShaders | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
    kDepthStages | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkPipelineStageFlags2 kGraphicsStages = kGraphicsPipelineStages | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                                                  VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT |
                                                  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
constexpr VkPipelineStageFlags2 kAllShaderStages = kPreRasterShaders | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

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

VkPipelineStageFlags2 queueStages(VkQueueFlags queueFlags) noexcept {
    VkPipelineStageFlags2 stages = kAnyQueueStages;
    if (queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) stages |= kTransferStages;
    if (queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) stages |= kGraphicsOrComputeStages;
    if (queueFlags & VK_QUEUE_COMPUTE_BIT) stages |= kComputeStages;
    if (queueFlags & VK_QUEUE_GRAPHICS_BIT) stages |= kGraphicsStages;
    return stages;
}

VkPipelineStageFlags2 expandStages(VkPipelineStageFlags2 stages, VkQueueFlags queueFlags) noexcept {
    VkPipelineStageFlags2 e = stages;
    if (e & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) e |= queueStages(queueFlags) & ~VK_PIPELINE_STAGE_2_HOST_BIT;
    if (e & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT) e |= kGraphicsPipelineStages;
    if (e & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) {
        e |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT;
    }
    if (e & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) e |= kPreRasterShaders;
    if (e & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT) e |= kVertexInput;
    return e;
}

VkAccessFlags2 stageAccess(VkPipelineStageFlags2 s) noexcept {
    VkAccessFlags2 a = VK_ACCESS_2_NONE;
    if (s & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) a |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (s & VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT) a |= VK_ACCESS_2_INDEX_READ_BIT;
    if (s & VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT) a |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    if (s & kAllShaderStages) {
        a |= VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (s & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) a |= VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
    if (s & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
        a |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (s & kDepthStages) {
        a |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT)) {
        a |= VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if (s & VK_PIPELINE_STAGE_2_CLEAR_BIT) a |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (s & VK_PIPELINE_STAGE_2_HOST_BIT) a |= VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    if (s & ~(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT)) {
        a |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    return a;
}

StageAccess restrictToQueue(VkPipelineStageFlags2 stages, VkAccessFlags2 access, VkQueueFlags queueFlags) noexcept {
    if (stages == VK_PIPELINE_STAGE_2_NONE) return {};
    VkPipelineStageFlags2 kept = stages & queueStages(queueFlags);
    if (kept == VK_PIPELINE_STAGE_2_NONE) kept = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return {kept, access & stageAccess(expandStages(kept, queueFlags))};
}

BarrierMasks barrierMasks(ResourceState before, ResourceState after, VkQueueFlags queueFlags) noexcept {
    const StateInfo src = stateInfo(before);
    const StateInfo dst = stateInfo(after);
    const bool noSourceStage = before == ResourceState::Undefined || before == ResourceState::Present;
    // Discarding contents is still a write after whatever wrote them before (e.g. memory reused by a
    // new resource): write-after-write needs those writes made available, not just ordered.
    const VkAccessFlags2 srcAccess = before == ResourceState::Undefined ? VK_ACCESS_2_MEMORY_WRITE_BIT : src.access;
    BarrierMasks m;
    m.src = restrictToQueue(noSourceStage ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : src.stages, srcAccess, queueFlags);
    m.dst = restrictToQueue(dst.stages == VK_PIPELINE_STAGE_2_NONE ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT : dst.stages,
                            dst.access, queueFlags);
    m.oldLayout = src.layout;
    m.newLayout = dst.layout;
    return m;
}

} // namespace helios::rhi::vk
