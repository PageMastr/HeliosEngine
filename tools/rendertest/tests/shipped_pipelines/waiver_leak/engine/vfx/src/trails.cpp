// Fixture: a waiver after code covers its own line only, not the next one.
void trails(rhi::Device& device, const rhi::GraphicsPipelineDesc& a, const rhi::GraphicsPipelineDesc& b) {
    (void)device.createGraphicsPipeline(a);  // shipped-pipelines-lint: allow a documented exception here
    (void)device.createGraphicsPipeline(b);
}
