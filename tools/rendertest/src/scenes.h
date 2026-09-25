#pragma once
// helios-rendertest scenes: deterministic test scenes rendered offscreen through the render graph
// (fixed camera, time and seeds). Each renders into an imported RGBA8 output texture.

#include <memory>
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

} // namespace helios::rendertest
