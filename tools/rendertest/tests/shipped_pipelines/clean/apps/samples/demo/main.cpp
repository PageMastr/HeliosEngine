// Fixture: a reasoned waiver on the line above the call.
rhi::PipelineH makePipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    // shipped-pipelines-lint: allow the raw-RHI sample ships no render feature
    return device.createGraphicsPipeline(desc).value();
}
