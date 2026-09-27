// Fixture: a feature creates its pipelines through the record.
Result<void> createFeature(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc) {
    HELIOS_TRY_ASSIGN(m_pso, createShippedPipeline(device, desc));
    return {};
}
