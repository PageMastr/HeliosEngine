// Fixture: tools/lint/tests/ holds the other lints' fixtures and is not scanned.
void seeded(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) { (void)device.createGraphicsPipeline(desc); }
