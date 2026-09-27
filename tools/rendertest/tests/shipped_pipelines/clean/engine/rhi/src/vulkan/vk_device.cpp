// Fixture: engine/rhi implements and tests the Device API; it is not scanned.
Result<PipelineH> VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc, PsoPriority priority) {
    return createPipelineImpl(desc, priority);
}
