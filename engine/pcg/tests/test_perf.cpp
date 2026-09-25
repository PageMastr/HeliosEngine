// Timing checks (serial nightly tier: "perf: ..." cases run as pcg_tests_perf). The WP-0.9c spike
// executable (pcg_hnoise_bench) records the full measurement for the ADR; these cases gate it.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "helios/pcg/pcg.h"

using namespace helios;
using namespace helios::pcg;

namespace {

/// Median ms per tile over `tiles` distinct tiles (after one warm-up tile).
f64 msPerTile(KernelKind kernel, const TerrainProgram& p, u32 tiles) {
    TileEvaluator eval(kernel);
    std::vector<i64> h(kTileSamples);
    REQUIRE(eval.evaluate(p, TileDomain::cubeSphere({CubeFace::PosX, 15, 1, 1}, 1'500'000.0), h).ok());
    std::vector<f64> times;
    for (u32 t = 0; t < tiles; ++t) {
        const TileDomain d = TileDomain::cubeSphere({static_cast<CubeFace>(t % 6), 15, 1000 + 37 * t, 2000 + 11 * t}, 1'500'000.0);
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(eval.evaluate(p, d, h).ok());
        times.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

} // namespace

TEST_CASE("perf: CPU VM ms per 65x65 tile per core on the dispatched AVX2 kernel (03 §5.5a budget 0.5 ms)") {
    if (!kernelSupported(KernelKind::Avx2)) {
        MESSAGE("AVX2 kernel not supported on this CPU; the budget is measured on AVX2 hardware only");
        return;
    }
    REQUIRE(selectKernel(KernelKind::Avx2).ok());
    REQUIRE(activeKernel() == KernelKind::Avx2); // the measurement guard: only an avx2 run counts
    const TerrainProgram full = compileTerrainGraph(makeReferenceGraph40(), {.collisionLevel = true}).value();
    const f64 avx2 = msPerTile(KernelKind::Avx2, full, 24);
    const f64 sse42 = kernelSupported(KernelKind::Sse42) ? msPerTile(KernelKind::Sse42, full, 8) : 0.0;
    const f64 scalar = msPerTile(KernelKind::Scalar, full, 4);
    MESSAGE("reference40 full detail (86 noise evaluations per sample): avx2 " << avx2 << " ms, sse42 " << sse42
                                                                            << " ms, scalar " << scalar << " ms per tile");
    CHECK(avx2 < scalar);
    // 03 §5.5a / 02 §5.8: <= 0.5 ms per tile per core on the AVX2 kernel for the 40-node graph.
    // (The binding measurement is on MIN and SERVER hardware; see docs/adr/ADR-0.9c-hnoise-throughput.md.)
    CHECK(avx2 <= 0.5);
}
