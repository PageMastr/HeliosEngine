// Fixture: the record itself may create pipelines directly.
Result<rhi::PipelineH> createRecorded(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    return device.createGraphicsPipeline(desc);
}
