#include "editor_pipelines.h"

namespace helios::edui::detail {

Result<rhi::PipelineH> editorPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    // shipped-pipelines-lint: allow EditorUI's own shaders, covered by the ED-15 goldens (editor_pipelines.h)
    return device.createGraphicsPipeline(desc);
}

} // namespace helios::edui::detail
