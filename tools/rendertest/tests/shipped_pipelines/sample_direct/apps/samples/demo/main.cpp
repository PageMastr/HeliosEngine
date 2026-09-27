// Fixture: a direct compute pipeline through a pointer, without a waiver.
rhi::PipelineH makeCompute(rhi::Device* device, const rhi::ComputePipelineDesc& desc) {
    return device->createComputePipeline(desc).value();
}
