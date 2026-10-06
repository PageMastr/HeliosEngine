#pragma once
// Synchronization2 translation of the RHI's resource states (03 §1.1): the stages, access types and
// image layouts a ResourceState stands for, restricted to what the recording queue family supports.
// Only <vulkan/vulkan_core.h> (no volk, no VMA), so the CPU unit tests check these tables without a
// GPU (tests/test_vk_sync.cpp). All functions are pure and may be called from any thread.
//
// Why per queue: a barrier only orders work on its own queue. Work on another queue family is
// ordered by the timeline-semaphore wait (signals and waits use ALL_COMMANDS, whose access scopes
// cover all memory accesses), so the shader writes the graphics queue made to a buffer are already
// available and visible to a transfer queue's copy. That queue's barrier needs only the stages and
// access types its own commands perform; Vulkan rejects the others (an access type is valid only
// with a stage of the queue that performs it: VUID-VkBufferMemoryBarrier2-srcAccessMask-03906 and
// friends). When none of a state's stages exist on the queue, ALL_COMMANDS remains, so the barrier
// (and an image layout transition) still chains after the semaphore wait.
//
// Queue-family ownership: resources shared by several queue families are created with
// VK_SHARING_MODE_CONCURRENT (vk_device.cpp), so barriers carry VK_QUEUE_FAMILY_IGNORED and no
// release/acquire pair exists. If exclusive sharing arrives with the render graph, the release half
// is recorded on the source queue with that queue's source masks and NONE as its destination half,
// and the acquire half on the destination queue with NONE as its source half (the spec ignores
// those halves); restrictToQueue() then applies to each half on its own queue.

#include <vulkan/vulkan_core.h>

#include "helios/rhi/types.h"

namespace helios::rhi::vk {

/// Pipeline stages, access mask and image layout that a ResourceState stands for, on a queue that
/// supports every stage (see barrierMasks() for a given queue).
struct StateInfo {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
StateInfo stateInfo(ResourceState state) noexcept;

/// Stages a queue family with `queueFlags` can execute ("Supported pipeline stage flags"): graphics,
/// compute and transfer stages by capability (graphics and compute families also transfer), plus
/// TOP_OF_PIPE, BOTTOM_OF_PIPE, ALL_COMMANDS and HOST, which every queue supports. Stages no Helios
/// state uses (ray tracing, video, ...) are not listed and so never kept.
VkPipelineStageFlags2 queueStages(VkQueueFlags queueFlags) noexcept;

/// `stages` with the meta stages expanded as the validation rules expand them on a queue with
/// `queueFlags`: ALL_COMMANDS to every stage the queue executes except HOST, and ALL_GRAPHICS,
/// ALL_TRANSFER, PRE_RASTERIZATION_SHADERS and VERTEX_INPUT to their parts.
VkPipelineStageFlags2 expandStages(VkPipelineStageFlags2 stages, VkQueueFlags queueFlags) noexcept;

/// Access types that at least one of `expandedStages` performs ("Supported access types"). MEMORY_READ
/// and MEMORY_WRITE go with any stage but TOP_OF_PIPE / BOTTOM_OF_PIPE.
VkAccessFlags2 stageAccess(VkPipelineStageFlags2 expandedStages) noexcept;

struct StageAccess {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
};

/// One half of a barrier restricted to a queue family: stages the queue cannot execute are dropped
/// (ALL_COMMANDS when none remain), then access types that none of the remaining stages perform.
/// NONE stays NONE (with no access).
StageAccess restrictToQueue(VkPipelineStageFlags2 stages, VkAccessFlags2 access, VkQueueFlags queueFlags) noexcept;

/// Everything a state transition recorded on a queue with `queueFlags` needs: both halves and the
/// image layouts. Sources without a stage of their own (Undefined, Present) wait for ALL_COMMANDS so
/// the transition chains after earlier work and the presentation engine's semaphore; Undefined also
/// makes earlier writes to the memory available (a new resource may reuse it). Destinations without
/// a stage (Present) use BOTTOM_OF_PIPE.
struct BarrierMasks {
    StageAccess src;
    StageAccess dst;
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};
BarrierMasks barrierMasks(ResourceState before, ResourceState after, VkQueueFlags queueFlags) noexcept;

} // namespace helios::rhi::vk
