// Fixture: tools/rendertest/tests/ holds the lint's own fixtures and is not scanned.
void seeded(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) { (void)device.createGraphicsPipeline(desc); }
