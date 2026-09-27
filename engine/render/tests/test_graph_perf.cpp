// Render-graph compile budget: 03 §2.2 item 7 ("Budget ≤ 0.3 ms CPU for 200 passes") and §8.1.5's
// "Graph compile on a topology change" row, ≤ 0.3 ms on REF at 60 fps (≤ 0.2 ms in the 120 fps
// Performance column; a topology-cache hit ≤ 0.05 ms, Phase 2). The graph has no topology cache yet,
// so every compile() here is a full compile: the topology-change case. The gate is the 60 fps
// number; the 120 fps number is reported, not asserted, because this machine is not REF. The timing
// is reported always and asserted only in optimized builds without sanitizers; it runs in
// render_tests_perf (serial, nightly).

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
constexpr f64 kBudgetMs = 0.3;           // 03 §8.1.5, REF 60 fps: the gate
constexpr f64 kPerformanceModeMs = 0.2;  // 03 §8.1.5, REF 120 fps: reported only
constexpr u32 kPasses = 200;

/// Exactly kPasses passes of every kind: Init, a chain with a read window of three, a shared counter
/// buffer every tenth pass, periodic NeverCull roots, every fifth pass on async compute, and Final
/// (the shape of the functional test "graph: compile budget (200 passes)").
void buildGraph(RenderGraph& graph) {
    const RgBufferDesc buffer{4096};
    const RgTextureDesc ldr = RgTextureDesc::tex2D(rhi::Format::RGBA8Unorm, 256, 256);
    const RgTextureDesc hdr = RgTextureDesc::tex2D(rhi::Format::RGBA16Float, 256, 256);
    std::vector<RgTexture> live;
    RgBuffer counter;
    graph.addPass(
        "Init", PassFlags::Compute, [&](RgBuilder& b) { counter = b.write(b.create("Counter", buffer)); }, {});
    for (u32 i = 0; i < kPasses - 2; ++i) {
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

TEST_CASE("perf: render graph compile of 200 passes <= 0.3 ms (no topology cache)") {
    RenderGraph graph("Budget");
    buildGraph(graph);
    REQUIRE(graph.errors().empty());
    REQUIRE(graph.passCount() == kPasses);
    REQUIRE(graph.compile().ok());  // warm-up
    // Best of 10 batches of 5 compiles: robust against other processes on a shared machine.
    f64 best = 1e30;
    for (int batch = 0; batch < 10; ++batch) {
        const Stopwatch timer;
        for (int run = 0; run < 5; ++run) REQUIRE(graph.compile().ok());
        best = std::min(best, timer.elapsedMillis() / 5.0);
    }
    const RgPlan& plan = graph.plan();
    MESSAGE(std::format("graph compile: {} passes in {:.3f} ms (gate {} ms at REF 60 fps; REF 120 fps budget {} ms, "
                        "{} here), {} batches, {} barriers",
                        plan.stats.passCount, best, kBudgetMs, kPerformanceModeMs,
                        best <= kPerformanceModeMs ? "within" : "over", plan.stats.batchCount,
                        plan.stats.barrierCount));
    if (kAssertBudget) CHECK(best <= kBudgetMs);
}

} // namespace
