#pragma once
// The shader modules engine/render ships and the pipelines its features build from them
// (docs/plan/03-rendering.md §1.7, §8.4; RC-1).
//
// Every Slang file in engine/render/shaders/ is embedded in helios_render as one SPIR-V module.
// Features create their pipelines through createShippedPipeline(), which records each pipeline's
// name with the entry points it uses. `helios-rendertest --coverage` combines the two lists to check
// RC-1's "every shipped feature has a lavapipe golden" mechanically: every shipped entry point and
// every recorded pipeline must be bound in the captured frame of a scene that has a committed
// lavapipe golden and a Null trace golden. A pipeline created without createShippedPipeline() leaves
// its entry points unreached, so it fails that check instead of passing unnoticed.
//
// Threading: every function is thread-safe (the pipeline record is guarded by a mutex).

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/render/shader_reflection.h"
#include "helios/rhi/device.h"

namespace helios::render {

/// One entry point of a shader module embedded in helios_render.
struct ShippedEntryPoint {
    std::string module;      ///< File stem: "forward" for engine/render/shaders/forward.slang.
    std::string entryPoint;  ///< SPIR-V entry-point name ("vsMain").
    ShaderStage stage = ShaderStage::Unknown;
    friend bool operator==(const ShippedEntryPoint&, const ShippedEntryPoint&) = default;
};

/// A named pipeline that engine/render created from its embedded modules in this process.
struct ShippedPipeline {
    std::string name;                            ///< The pipeline's debug name ("Forward.Geometry").
    std::vector<ShippedEntryPoint> entryPoints;  ///< Vertex, fragment or compute, in that order.
};

/// SPIR-V words of the embedded module with this file stem ("forward"); empty if there is none.
std::span<const u32> shippedModule(std::string_view stem) noexcept;

/// Every entry point of every embedded module, reflected from its SPIR-V and sorted by module and
/// entry point. Fails only when an embedded module does not reflect (a build defect).
Result<std::vector<ShippedEntryPoint>> shippedEntryPoints();

/// Every pipeline recorded by createShippedPipeline() so far, on any device, sorted by name.
std::vector<ShippedPipeline> shippedPipelines();

/// Creates a pipeline whose stages all come from helios_render's embedded modules and records its
/// name with their entry points. Fails with InvalidArgument when the name is empty, a stage's SPIR-V
/// is not an embedded module (compared by address), an entry point is not in its module, or the name
/// was recorded before with other entry points; device errors pass through. Nothing stays created or
/// recorded on failure. Thread-safe like Device.
Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc,
                                             rhi::PsoPriority priority = rhi::PsoPriority::Immediate);
Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::ComputePipelineDesc& desc,
                                             rhi::PsoPriority priority = rhi::PsoPriority::Immediate);

} // namespace helios::render
