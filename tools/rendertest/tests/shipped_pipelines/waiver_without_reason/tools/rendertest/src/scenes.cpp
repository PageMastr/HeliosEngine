// Fixture: a waiver must say why.
Result<rhi::PipelineH> noReason(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    // shipped-pipelines-lint: allow
    return device.createGraphicsPipeline(desc);
}
Result<rhi::PipelineH> shortReason(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    return device.createGraphicsPipeline(desc);  // shipped-pipelines-lint: allow test
}
