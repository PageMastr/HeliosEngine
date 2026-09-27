#pragma once
// The shader modules engine/render ships and the pipelines its features build from them
// (docs/plan/03-rendering.md §1.7, §8.4; RC-1).
//
// Every Slang file in engine/render/shaders/ is embedded in helios_render as one SPIR-V module.
// Features create their pipelines through createShippedPipeline(), which records each pipeline's
// name with the entry points it uses and a fingerprint of the rest of its desc (raster, depth, blend,
// attachment formats, topology, samples). One name stands for one pipeline: the same name with other
// shaders or other state is rejected, so a state variant needs its own name (and its own golden),
// also when a feature builds the same pipeline for several output formats in one process.
// `helios-rendertest --coverage` combines the lists: every shipped entry point and every recorded
// pipeline must be bound (a bindPipeline in the Null trace) in the captured frame of a scene that has
// a committed lavapipe golden and a Null trace golden.
// The CTest lint_shipped_pipelines (a textual check) keeps pipelines from bypassing the record: in
// engine/, apps/ and tools/ (except engine/rhi, which implements the API, tools/prebuilt and the
// lint's fixtures), any identifier create<X>Pipeline other than createShippedPipeline and
// createLocalPipeline, whether called with `.`, `->`, across a line break, or named in a member
// pointer or std::invoke, needs a reasoned waiver; only this module's implementation is exempt.
// It cannot see a name built by token pasting or a member pointer obtained outside the scanned files.
// What the coverage check cannot see:
//   * passes without a pipeline (uploads, copies, clears);
//   * whether a bound pipeline's output reaches the golden image (a pipeline bound only into a
//     marked debug output counts as covered);
//   * shaders that are not embedded in helios_render, such as cooked SPIR-V from content (03 §1.7's
//     shipped SPIR-V + .psol, Phase 1).
// Future cost: each call reflects its module (fine for today's handful of PSOs; cache per module
// before 03 §1.7's thousands), and the one-name-one-pipeline rule rejects a hot-reloaded pipeline
// whose entry points or state change (Phase 2 hot reload must re-register the name).
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
/// name with their entry points and the fingerprint of its other state. Fails with InvalidArgument
/// when the name is empty, a stage's SPIR-V is not an embedded module (compared by address), an entry
/// point is not in its module, or the name was recorded before with other entry points or other
/// state (the same desc again, on any device, is accepted); device errors pass through. Nothing stays
/// created or recorded on failure. Thread-safe like Device.
Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc,
                                             rhi::PsoPriority priority = rhi::PsoPriority::Immediate);
Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::ComputePipelineDesc& desc,
                                             rhi::PsoPriority priority = rhi::PsoPriority::Immediate);

} // namespace helios::render
