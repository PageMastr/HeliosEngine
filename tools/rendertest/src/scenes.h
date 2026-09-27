#pragma once
// helios-rendertest scenes: deterministic test scenes rendered offscreen through the render graph
// (fixed camera, time and seeds). Each renders into an imported 8-bit RGBA output texture
// (SceneInfo::outputFormat).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/render/render_graph.h"
#include "helios/rhi/device.h"

namespace helios::rendertest {

struct SceneInfo {
    std::string_view name;
    std::string_view description;
    u32 width = 320;
    u32 height = 180;
    /// Golden policy (03 §8.4): mean ꟻLIP <= maxMeanFlip and every pixel <= maxFlip.
    f64 maxMeanFlip = 0.01;
    f32 maxFlip = 0.5f;
    /// Frames rendered before the captured one (temporal warm-up).
    u32 warmupFrames = 0;
    /// Format of the output texture (read back as 8-bit RGBA; an sRGB format stores encoded bytes).
    rhi::Format outputFormat = rhi::Format::RGBA8Unorm;
};

class Scene {
public:
    virtual ~Scene() = default;
    virtual const SceneInfo& info() const noexcept = 0;
    /// Creates pipelines and static resources on `device` for an output of `outputFormat`.
    virtual Result<void> init(rhi::Device& device, rhi::Format outputFormat) = 0;
    /// Adds the passes rendering frame `frame` into `output`.
    virtual void addPasses(render::RenderGraph& graph, render::RgTexture output, u32 frame) = 0;
    /// Releases what init() created.
    virtual void destroy(rhi::Device& device) = 0;
};

/// All scenes, in suite order.
std::vector<std::unique_ptr<Scene>> createScenes();

/// Creates a pipeline from rendertest's own shaders and records its name. Scenes create every
/// pipeline that is not engine/render's through these (tools/rendertest/tests/shipped_pipelines_lint.cmake
/// allows no other direct call), so measureCoverage() can reject a local pipeline named like a
/// shipped one, which would make a Null trace count as coverage of the shipped pipeline.
/// Thread-safe like Device.
Result<rhi::PipelineH> createLocalPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc);
Result<rhi::PipelineH> createLocalPipeline(rhi::Device& device, const rhi::ComputePipelineDesc& desc);
/// Names passed to createLocalPipeline() so far in this process, sorted.
std::vector<std::string> localPipelineNames();

} // namespace helios::rendertest
