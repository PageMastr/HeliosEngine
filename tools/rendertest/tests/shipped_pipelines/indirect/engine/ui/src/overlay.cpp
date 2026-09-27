// Fixture: indirect and split forms of a direct call in a module outside engine/render (4 findings).
void overlay(rhi::Device& device, const rhi::GraphicsPipelineDesc& wire, rhi::PsoPriority prio) {
    auto create = &rhi::Device::createGraphicsPipeline;
    (void)(device.*create)(wire, prio);
    (void)std::invoke(&rhi::Device::createComputePipeline, device, compute, prio);
    (void)device.
        createGraphicsPipeline(wire);
    (void)device.createGraphicsPipeline
        (wire);
}
