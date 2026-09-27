// Render-graph compile budget (engine/render/README.md): a 200-pass graph compiles in <= 2 ms of CPU
// on REF without a topology cache. 03 §2.2's <= 0.3 ms for 200 passes is the Phase 2 cached case
// (an unchanged topology skips steps 1-5). The timing is reported always and asserted only in
// optimized builds without sanitizers; it runs in render_tests_perf (serial, nightly).

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <vector>

#include "helios/core/time.h"
#include "helios/render/render_graph.h"

namespace {

using namespace helios;
using namespace helios::render;

#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
constexpr bool kAssertBudget = true;
#else
constexpr bool kAssertBudget = false;
#endif
constexpr f64 kBudgetMs = 2.0;

/// 200 passes of every kind with a read window of three, a shared counter buffer every tenth pass,
/// periodic NeverCull roots and every fifth pass on async compute (the shape of the functional test
/// "graph: compile budget (200 passes)").
void buildGraph(RenderGraph& graph) {
    const RgBufferDesc buffer{4096};
    const RgTextureDesc ldr = RgTextureDesc::tex2D(rhi::Format::RGBA8Unorm, 256, 256);
    const RgTextureDesc hdr = RgTextureDesc::tex2D(rhi::Format::RGBA16Float, 256, 256);
    std::vector<RgTexture> live;
    RgBuffer counter;
    graph.addPass(
        "Init", PassFlags::Compute, [&](RgBuilder& b) { counter = b.write(b.create("Counter", buffer)); }, {});
    for (u32 i = 0; i < 200; ++i) {
        const PassFlags kind =
            i % 5 == 4 ? PassFlags::AsyncCompute : (i % 3 == 0 ? PassFlags::Raster : PassFlags::Compute);
        graph.addPass(std::format("P{}", i), kind | (i % 17 == 0 ? PassFlags::NeverCull : PassFlags::None),
                      [&, i, kind](RgBuilder& b) {
                          if (!live.empty()) b.read(live.back());
                          if (live.size() > 2) b.read(live[live.size() - 3]);
                          const RgTexture t = b.create(std::format("T{}", i), i % 2 ? ldr : hdr);
                          live.push_back(kind == PassFlags::Raster ? b.colorAttachment(t, 0) : b.write(t));
                          if (i % 10 == 0 && kind != PassFlags::Raster) counter = b.readWrite(counter);
                      },
                      {});
    }
    graph.addPass("Final", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
        b.read(live.back());
        b.read(counter);
    }, {});
}

TEST_CASE("perf: render graph compile of 200 passes <= 2 ms (no topology cache)") {
    RenderGraph graph("Budget");
    buildGraph(graph);
    REQUIRE(graph.errors().empty());
    REQUIRE(graph.compile().ok());  // warm-up
    // Best of 10 batches of 5 compiles: robust against other processes on a shared machine.
    f64 best = 1e30;
    for (int batch = 0; batch < 10; ++batch) {
        const Stopwatch timer;
        for (int run = 0; run < 5; ++run) REQUIRE(graph.compile().ok());
        best = std::min(best, timer.elapsedMillis() / 5.0);
    }
    const RgPlan& plan = graph.plan();
    MESSAGE(std::format("graph compile: {} passes in {:.3f} ms (budget {} ms), {} batches, {} barriers",
                        plan.stats.passCount, best, kBudgetMs, plan.stats.batchCount, plan.stats.barrierCount));
    if (kAssertBudget) CHECK(best <= kBudgetMs);
}

} // namespace
