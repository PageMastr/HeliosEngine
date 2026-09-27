// Fixture: identifiers that are not create<X>Pipeline, and a waiver alone on the line above a call
// that spans lines.
Result<PipelineH> createPipeline(Device& device, Format format);              // a helper named createPipeline
void inspect(const Slang& gfx) { (void)gfx.createGraphicsPipelineState(desc); } // another API's name
void f(Device& dev) {
    HELIOS_TRY_ASSIGN(pso,
                      // shipped-pipelines-lint: allow the test's compute pipeline renders nothing
                      dev.createComputePipeline(desc));
}
