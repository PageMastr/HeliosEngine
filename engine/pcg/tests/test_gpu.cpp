// GPU conformance of the Slang hnoise twin (03 §5.5, RT-04's C++/Slang clause): on the Vulkan adapter
// (lavapipe in CI), every corpus lattice case and every corpus tile (positions and heights) is
// bit-identical to the CPU reference. The whole executable carries the `gpu` CTest label.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "corpus.h"
#include "gpu_twin.h"

using namespace helios;
using namespace helios::pcg;
using namespace helios::pcg::test;

namespace {

GpuTwin* twin() {
    static std::unique_ptr<GpuTwin> instance = [] {
        auto created = GpuTwin::create(true);
        if (!created.ok()) {
            const char* skip = std::getenv("HELIOS_SKIP_GPU_TESTS");
            if (skip && std::string_view(skip) == "1") return std::unique_ptr<GpuTwin>();
            FAIL("Vulkan device creation failed: " << created.error().toString()
                                                   << " (set HELIOS_SKIP_GPU_TESTS=1 on machines without Vulkan)");
        }
        MESSAGE("GPU twin adapter: " << (*created)->adapterDescription());
        return std::move(created).value();
    }();
    return instance.get();
}

const Corpus& corpus() {
    static const Corpus c = [] {
        auto loaded = loadCorpus(corpusDir());
        if (!loaded.ok()) FAIL(loaded.error().toString());
        return std::move(loaded).value();
    }();
    return c;
}

} // namespace

TEST_CASE("gpu twin: lattice hashes and noise values match the corpus") {
    GpuTwin* gpu = twin();
    if (!gpu) return;
    const Corpus& c = corpus();
    std::vector<u32> hashes;
    std::vector<i32> noises;
    REQUIRE(gpu->evaluateLattice(c.lattice, hashes, noises).ok());
    REQUIRE(hashes.size() == c.lattice.size());
    u32 hashMismatches = 0, noiseMismatches = 0;
    for (usize i = 0; i < c.lattice.size(); ++i) {
        hashMismatches += hashes[i] != c.lattice[i].hash ? 1u : 0u;
        noiseMismatches += noises[i] != c.lattice[i].noise ? 1u : 0u;
    }
    CHECK(hashMismatches == 0);
    CHECK(noiseMismatches == 0);
    CHECK(gpu->validationErrors() == 0);
}

TEST_CASE("gpu twin: every corpus tile is bit-identical to the CPU reference (positions and heights)") {
    GpuTwin* gpu = twin();
    if (!gpu) return;
    const Corpus& c = corpus();
    u32 checked = 0;
    for (const TileCase& t : c.tiles) {
        INFO("case " << t.name);
        const TerrainProgram p = compileTerrainGraph(graphByName(t.graph).value(), t.options).value();
        REQUIRE(p.hash() == t.programHash);
        std::vector<i64> heights;
        std::vector<FixedPos> positions;
        const TileDomain domains[1] = {t.domain};
        REQUIRE(gpu->evaluateTiles(p, domains, heights, &positions).ok());
        CHECK(hashPositions(positions) == t.positionsHash);
        CHECK(hashHeights(heights) == t.heightsHash);
        if (hashHeights(heights) != t.heightsHash) {
            // Diagnose: first differing sample against the CPU scalar kernel.
            TileEvaluator eval(KernelKind::Scalar);
            std::vector<i64> cpu(kTileSamples);
            REQUIRE(eval.evaluate(p, t.domain, cpu).ok());
            for (u32 i = 0; i < kTileSamples; ++i) {
                if (cpu[i] != heights[i]) {
                    MESSAGE("first mismatch at sample " << i << ": cpu " << cpu[i] << " gpu " << heights[i]);
                    break;
                }
            }
        }
        ++checked;
    }
    CHECK(checked == c.tiles.size());
    CHECK(gpu->validationErrors() == 0);
}

TEST_CASE("gpu twin: a batch of tiles in one dispatch equals the CPU kernel tile by tile") {
    GpuTwin* gpu = twin();
    if (!gpu) return;
    const TerrainProgram p = compileTerrainGraph(makeReferenceGraph40(), {.collisionLevel = true}).value();
    std::vector<TileDomain> domains;
    for (u32 t = 0; t < 12; ++t) {
        domains.push_back(TileDomain::cubeSphere({static_cast<CubeFace>(t % 6), static_cast<u8>(10 + t % 6), 17 * t, 5 * t},
                                                 1'500'000.0 + 1000.0 * t));
    }
    std::vector<i64> heights;
    REQUIRE(gpu->evaluateTiles(p, domains, heights).ok());
    TileEvaluator eval(bestSupportedKernel());
    std::vector<i64> cpu(kTileSamples);
    for (usize t = 0; t < domains.size(); ++t) {
        REQUIRE(eval.evaluate(p, domains[t], cpu).ok());
        CHECK(std::equal(cpu.begin(), cpu.end(), heights.begin() + static_cast<isize>(t * kTileSamples)));
    }
}

TEST_CASE("gpu twin: programs and domains are validated before upload (review regression)") {
    // The shader masks register indices but cannot see a read-before-write (its registers start at
    // zero / the base position, the CPU's persist) or a tile outside the lattice.
    GpuTwin* gpu = twin();
    if (!gpu) return;
    const TileDomain ok = TileDomain::cubeSphere({CubeFace::PosX, 4, 3, 3}, 1'500'000.0);
    TerrainProgram bad;
    bad.heightRegisters = 2;
    bad.code.push_back(Instr::make(Opcode::Add, 0, 1, 1));
    std::vector<i64> heights;
    CHECK(gpu->evaluateTiles(bad, std::span<const TileDomain>(&ok, 1), heights).errorCode() == ErrorCode::Corrupt);
    const TerrainProgram good = compileTerrainGraph(makeSingleNodeGraph(NodeKind::Fbm)).value();
    const TileDomain outside = TileDomain::cubeSphere({CubeFace::PosX, 4, 16, 3}, 1'500'000.0);
    CHECK(gpu->evaluateTiles(good, std::span<const TileDomain>(&outside, 1), heights).errorCode() == ErrorCode::InvalidArgument);
    CHECK(gpu->evaluateTiles(good, std::span<const TileDomain>(), heights).errorCode() == ErrorCode::InvalidArgument);
    REQUIRE(gpu->evaluateTiles(good, std::span<const TileDomain>(&ok, 1), heights).ok());
    std::vector<i64> cpu(kTileSamples);
    TileEvaluator eval(KernelKind::Scalar);
    REQUIRE(eval.evaluate(good, ok, cpu).ok());
    CHECK(std::equal(cpu.begin(), cpu.end(), heights.begin()));
}
