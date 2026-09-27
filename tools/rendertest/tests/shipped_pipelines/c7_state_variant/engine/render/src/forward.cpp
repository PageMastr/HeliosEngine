// Fixture: round-1 review mutant C7 in ForwardRenderer::create. The two-sided variant reuses
// forward:vsMain and forward:psMain, which Forward.Geometry already covers, so rendertest.coverage
// alone cannot see that no golden binds it.
Result<std::unique_ptr<ForwardRenderer>> ForwardRenderer::create(rhi::Device& device, rhi::Format outputFormat) {
    rhi::GraphicsPipelineDesc geo;
    geo.vertex = {helios_render_shaders::forward(), "vsMain"};
    geo.fragment = {helios_render_shaders::forward(), "psMain"};
    geo.name = "Forward.Geometry";
    HELIOS_TRY_ASSIGN(r->m_geometry, createShippedPipeline(device, geo));
    rhi::GraphicsPipelineDesc wire = geo;
    wire.raster.cullMode = rhi::CullMode::None;
    wire.name = "Forward.TwoSided";
    (void)device.createGraphicsPipeline(wire);
    return r;
}
