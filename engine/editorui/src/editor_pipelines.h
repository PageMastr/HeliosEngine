#pragma once
// The one place where EditorUI creates graphics pipelines (internal).
//
// The editor's pipelines (the ImGui renderer, the present pass and the placeholder viewport grid) use
// helios_editorui's own shaders (shaders/*.slang), not engine/render's embedded modules, so
// render::createShippedPipeline() and its RC-1 record (03 §9.3) do not apply to them. They are covered
// instead by the ED-15 ꟻLIP goldens of helios-uitest (07 §4.4: the shell and the property grid on
// lavapipe, captured twice and compared pixel for pixel), which render all three. Funnelling every
// creation through editorPipeline() keeps the shipped-pipelines lint's waiver to one reasoned line.
//
// Threading: as rhi::Device (any thread; pipelines are device objects).

#include "helios/core/result.h"
#include "helios/rhi/device.h"

namespace helios::edui::detail {

/// Creates an EditorUI graphics pipeline on `device` (see the header comment).
Result<rhi::PipelineH> editorPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc);

} // namespace helios::edui::detail
