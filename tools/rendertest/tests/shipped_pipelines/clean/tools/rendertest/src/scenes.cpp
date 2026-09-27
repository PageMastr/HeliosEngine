// Fixture: a reasoned waiver on the same line as the call.
Result<rhi::PipelineH> local(rhi::Device* device, const rhi::ComputePipelineDesc& desc) {
    return device->createComputePipeline(desc);  // shipped-pipelines-lint: allow rendertest's own test shaders
}
