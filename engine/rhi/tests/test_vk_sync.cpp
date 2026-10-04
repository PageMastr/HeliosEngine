// Barrier masks per queue family (src/vulkan/vk_sync.h), checked without a GPU against the Vulkan
// spec's synchronization tables, transcribed here independently of the implementation: which queue
// capabilities each pipeline stage needs ("Supported pipeline stage flags") and which stages each
// access type goes with ("Supported access types"). The first hardware run (win-gpu, NVIDIA, a
// dedicated TRANSFER|SPARSE_BINDING queue) failed VUID-VkBufferMemoryBarrier2-srcAccessMask-03906/03907
// because a transfer-queue barrier carried shader access types; lavapipe has one universal queue
// family, so only this test catches that class of error in CI.

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

#include "vulkan/vk_sync.h"

using namespace helios;
using namespace helios::rhi;
using namespace helios::rhi::vk;

namespace {

// Queue families as real adapters report them.
struct Family {
    const char* name;
    VkQueueFlags flags;
};
constexpr Family kFamilies[] = {
    {"graphics", VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT},
    {"compute", VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT},
    {"compute (no transfer bit)", VK_QUEUE_COMPUTE_BIT},
    {"transfer", VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT},
    {"transfer only", VK_QUEUE_TRANSFER_BIT},
};

// "Supported pipeline stage flags": the queue capabilities (any of) a stage needs; 0 = every queue.
constexpr VkQueueFlags G = VK_QUEUE_GRAPHICS_BIT, C = VK_QUEUE_COMPUTE_BIT, T = VK_QUEUE_TRANSFER_BIT;
struct StageRow {
    VkPipelineStageFlags2 stage;
    VkQueueFlags needs;
};
constexpr StageRow kStageTable[] = {
    {VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0},
    {VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0},
    {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
    {VK_PIPELINE_STAGE_2_HOST_BIT, 0},
    {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, G | C},
    {VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, G},
    {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, G},
    {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, G},
    {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, G},
    {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, G},
    {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, G},
    {VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, G},
    {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, G},
    {VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, G},
    {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, C},
    {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, G | C | T},
    {VK_PIPELINE_STAGE_2_COPY_BIT, G | C | T},
    {VK_PIPELINE_STAGE_2_RESOLVE_BIT, G | C | T},
    {VK_PIPELINE_STAGE_2_BLIT_BIT, G | C | T},
    {VK_PIPELINE_STAGE_2_CLEAR_BIT, G | C | T},
};

// "Supported access types": the stages (any of) an access type goes with, meta stages expanded.
// MEMORY_READ / MEMORY_WRITE go with any stage.
constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
struct AccessRow {
    VkAccessFlags2 access;
    VkPipelineStageFlags2 stages;
};
constexpr AccessRow kAccessTable[] = {
    {VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT},
    {VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT},
    {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT},
    {VK_ACCESS_2_UNIFORM_READ_BIT, kShaders},
    {VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, kShaders},
    {VK_ACCESS_2_SHADER_STORAGE_READ_BIT, kShaders},
    {VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kShaders},
    {VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
    {VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT},
    {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
    {VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT},
    {VK_ACCESS_2_TRANSFER_READ_BIT,
     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT},
    {VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                                         VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT},
    {VK_ACCESS_2_HOST_READ_BIT, VK_PIPELINE_STAGE_2_HOST_BIT},
    {VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT},
};

bool stageOnQueue(VkPipelineStageFlags2 stage, VkQueueFlags queue) {
    for (const StageRow& row : kStageTable) {
        if (row.stage == stage) return row.needs == 0 || (row.needs & queue) != 0;
    }
    return false;  // not in the table: the masks must not use it
}

/// The stages a stage mask stands for on `queue`, as the validation layer expands it.
VkPipelineStageFlags2 oracleExpand(VkPipelineStageFlags2 mask, VkQueueFlags queue) {
    VkPipelineStageFlags2 out = mask;
    if (mask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
        for (const StageRow& row : kStageTable) {
            if (row.stage != VK_PIPELINE_STAGE_2_HOST_BIT && stageOnQueue(row.stage, queue)) out |= row.stage;
        }
    }
    if (out & VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT) {
        for (const StageRow& row : kStageTable) {
            if (row.needs == G) out |= row.stage;  // the graphics pipeline's own stages
        }
        out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if (out & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT) {
        out |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    }
    if (out & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) {
        out |= VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
               VK_PIPELINE_STAGE_2_CLEAR_BIT;
    }
    return out;
}

/// Problems the validation layer would report for one half of a barrier recorded on `queue`.
std::vector<std::string> problems(VkPipelineStageFlags2 stages, VkAccessFlags2 access, VkQueueFlags queue) {
    std::vector<std::string> out;
    for (u32 bit = 0; bit < 64; ++bit) {
        const VkPipelineStageFlags2 stage = VkPipelineStageFlags2{1} << bit;
        if ((stages & stage) && !stageOnQueue(stage, queue)) out.push_back(std::format("stage bit {} not on the queue", bit));
    }
    const VkPipelineStageFlags2 expanded = oracleExpand(stages, queue);
    for (u32 bit = 0; bit < 64; ++bit) {
        const VkAccessFlags2 a = VkAccessFlags2{1} << bit;
        if (!(access & a)) continue;
        if (a == VK_ACCESS_2_MEMORY_READ_BIT || a == VK_ACCESS_2_MEMORY_WRITE_BIT) {
            if (!(expanded & ~(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT))) {
                out.push_back("MEMORY_* access without a stage");
            }
            continue;
        }
        bool supported = false;
        for (const AccessRow& row : kAccessTable) {
            if (row.access == a) supported = (row.stages & expanded) != 0;
        }
        if (!supported) out.push_back(std::format("access bit {} not supported by the stage mask", bit));
    }
    return out;
}

constexpr ResourceState kStates[] = {
    ResourceState::Undefined,      ResourceState::General,         ResourceState::CopySource,
    ResourceState::CopyDest,       ResourceState::VertexBuffer,    ResourceState::IndexBuffer,
    ResourceState::IndirectArgument, ResourceState::ConstantBuffer, ResourceState::ShaderResource,
    ResourceState::UnorderedAccess, ResourceState::RenderTarget,   ResourceState::DepthWrite,
    ResourceState::DepthRead,      ResourceState::Present,         ResourceState::HostRead,
};
static_assert(std::size(kStates) == static_cast<usize>(ResourceState::Count));

} // namespace

TEST_CASE("vk sync: every state transition is valid on every queue family") {
    for (const Family& family : kFamilies) {
        for (ResourceState before : kStates) {
            for (ResourceState after : kStates) {
                if (after == ResourceState::Undefined) continue;  // rejected before translation
                const BarrierMasks m = barrierMasks(before, after, family.flags);
                CAPTURE(family.name);
                CAPTURE(resourceStateName(before));
                CAPTURE(resourceStateName(after));
                const std::vector<std::string> src = problems(m.src.stages, m.src.access, family.flags);
                const std::vector<std::string> dst = problems(m.dst.stages, m.dst.access, family.flags);
                CHECK_MESSAGE(src.empty(), (src.empty() ? std::string() : "src: " + src.front()));
                CHECK_MESSAGE(dst.empty(), (dst.empty() ? std::string() : "dst: " + dst.front()));
                // A barrier always has a source and a destination stage (sync2 allows NONE, but the
                // transition must chain with earlier work and semaphore waits).
                CHECK(m.src.stages != VK_PIPELINE_STAGE_2_NONE);
                CHECK(m.dst.stages != VK_PIPELINE_STAGE_2_NONE);
                CHECK(m.oldLayout == stateInfo(before).layout);
                CHECK(m.newLayout == stateInfo(after).layout);
            }
        }
    }
}

TEST_CASE("vk sync: a universal queue keeps every stage and access type of a state") {
    const VkQueueFlags universal = kFamilies[0].flags;
    for (ResourceState before : kStates) {
        for (ResourceState after : kStates) {
            if (after == ResourceState::Undefined) continue;
            const BarrierMasks m = barrierMasks(before, after, universal);
            const StateInfo src = stateInfo(before), dst = stateInfo(after);
            CAPTURE(resourceStateName(before));
            CAPTURE(resourceStateName(after));
            if (before == ResourceState::Undefined || before == ResourceState::Present) {
                CHECK(m.src.stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            } else {
                CHECK(m.src.stages == src.stages);
                CHECK(m.src.access == src.access);
            }
            if (dst.stages != VK_PIPELINE_STAGE_2_NONE) {
                CHECK(m.dst.stages == dst.stages);
                CHECK(m.dst.access == dst.access);
            }
        }
    }
    // Undefined still makes earlier writes to the memory available (write-after-write).
    CHECK(barrierMasks(ResourceState::Undefined, ResourceState::CopyDest, universal).src.access ==
          VK_ACCESS_2_MEMORY_WRITE_BIT);
}

TEST_CASE("vk sync: a dedicated transfer queue drops shader stages and access types") {
    const VkQueueFlags transfer = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    // The win-gpu case: readbackBuffer(UnorderedAccess) on Queue::Transfer.
    const BarrierMasks in = barrierMasks(ResourceState::UnorderedAccess, ResourceState::CopySource, transfer);
    CHECK(in.src.stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(in.src.access == VK_ACCESS_2_NONE);
    CHECK(in.dst.stages == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    CHECK(in.dst.access == VK_ACCESS_2_TRANSFER_READ_BIT);
    const BarrierMasks out = barrierMasks(ResourceState::CopySource, ResourceState::UnorderedAccess, transfer);
    CHECK(out.src.stages == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    CHECK(out.src.access == VK_ACCESS_2_TRANSFER_READ_BIT);
    CHECK(out.dst.stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(out.dst.access == VK_ACCESS_2_NONE);
    // An image still gets its layout transition, chained at ALL_COMMANDS.
    const BarrierMasks image = barrierMasks(ResourceState::ShaderResource, ResourceState::CopyDest, transfer);
    CHECK(image.oldLayout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    CHECK(image.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(image.src.stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    // Host reads after a copy keep their host half on any queue.
    const BarrierMasks host = barrierMasks(ResourceState::CopyDest, ResourceState::HostRead, transfer);
    CHECK(host.dst.stages == VK_PIPELINE_STAGE_2_HOST_BIT);
    CHECK(host.dst.access == VK_ACCESS_2_HOST_READ_BIT);
}

TEST_CASE("vk sync: a dedicated compute queue keeps the compute stage of shader states") {
    const VkQueueFlags compute = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    const BarrierMasks m = barrierMasks(ResourceState::UnorderedAccess, ResourceState::ShaderResource, compute);
    CHECK(m.src.stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(m.src.access == (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
    CHECK(m.dst.stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(m.dst.access == (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT));
    // A render target handed to async compute: no attachment stage on this queue.
    const BarrierMasks rt = barrierMasks(ResourceState::RenderTarget, ResourceState::ShaderResource, compute);
    CHECK(rt.src.stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(rt.src.access == VK_ACCESS_2_NONE);
    CHECK(rt.newLayout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    // Indirect arguments are read by dispatchIndirect on compute queues.
    const BarrierMasks ind = barrierMasks(ResourceState::UnorderedAccess, ResourceState::IndirectArgument, compute);
    CHECK(ind.dst.stages == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    CHECK(ind.dst.access == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
}
