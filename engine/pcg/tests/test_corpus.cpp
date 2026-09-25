// The checked-in hnoise corpus (tests/corpus/hnoise) holds on every CPU kernel. Regenerate it only for
// a deliberate algorithm change: HELIOS_UPDATE_HNOISE_CORPUS=1 pcg_tests -tc="corpus:*" (then update the
// Slang twin and re-run pcg_gpu_tests).
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "corpus.h"

using namespace helios;
using namespace helios::pcg;
using namespace helios::pcg::test;

namespace {

const Corpus& checkedInCorpus() {
    static const Corpus corpus = [] {
        const char* update = std::getenv("HELIOS_UPDATE_HNOISE_CORPUS");
        if (update && std::string(update) == "1") {
            const Corpus fresh = generateCorpus();
            const auto written = writeCorpus(corpusDir(), fresh);
            if (!written.ok()) FAIL(written.error().toString());
            MESSAGE("hnoise corpus regenerated in " << corpusDir().string());
        }
        auto loaded = loadCorpus(corpusDir());
        if (!loaded.ok()) FAIL(loaded.error().toString());
        return *loaded;
    }();
    return corpus;
}

} // namespace

TEST_CASE("corpus: lattice hashes and noise values") {
    const Corpus& c = checkedInCorpus();
    REQUIRE(c.lattice.size() >= 200);
    for (const LatticeCase& l : c.lattice) {
        CHECK(hnoise::latticeHash(l.seed, l.cell[0], l.cell[1], l.cell[2]) == l.hash);
        LatticeCoord coord;
        coord.cell = l.cell;
        coord.frac = l.frac;
        CHECK(hnoise::noise3(l.seed, coord) == l.noise);
    }
}

TEST_CASE("corpus: tile programs, positions and heights on every CPU kernel") {
    const Corpus& c = checkedInCorpus();
    REQUIRE(c.tiles.size() >= 30);
    std::vector<KernelKind> kernels;
    for (KernelKind k : {KernelKind::Scalar, KernelKind::Sse42, KernelKind::Avx2}) {
        if (kernelSupported(k)) kernels.push_back(k);
    }
    std::vector<i64> heights(kTileSamples);
    for (const TileCase& t : c.tiles) {
        INFO("case " << t.name);
        const TerrainGraph g = graphByName(t.graph).value();
        const TerrainProgram p = compileTerrainGraph(g, t.options).value();
        CHECK(p.hash() == t.programHash);
        CHECK(hashPositions(referencePositions(t.domain)) == t.positionsHash);
        for (KernelKind k : kernels) {
            TileEvaluator eval(k);
            REQUIRE(eval.evaluate(p, t.domain, heights).ok());
            CHECK_MESSAGE(hashHeights(heights) == t.heightsHash, "kernel " << kernelName(k));
            CHECK(heights[0] == t.probes[0]);
            CHECK(heights[kTileSamples / 2] == t.probes[1]);
            CHECK(heights[kTileSamples - 1] == t.probes[2]);
            std::vector<FixedPos> positions(kTileSamples);
            for (u32 i = 0; i < kTileSamples; ++i) positions[i] = eval.position(i);
            CHECK(hashPositions(positions) == t.positionsHash);
        }
    }
}

TEST_CASE("corpus: the checked-in files equal what the scalar reference generates now") {
    // Catches an accidental algorithm change even if every kernel changed the same way.
    const Corpus& c = checkedInCorpus();
    const Corpus fresh = generateCorpus();
    REQUIRE(fresh.lattice.size() == c.lattice.size());
    REQUIRE(fresh.tiles.size() == c.tiles.size());
    for (usize i = 0; i < c.lattice.size(); ++i) {
        CHECK(fresh.lattice[i].hash == c.lattice[i].hash);
        CHECK(fresh.lattice[i].noise == c.lattice[i].noise);
    }
    for (usize i = 0; i < c.tiles.size(); ++i) {
        INFO("case " << c.tiles[i].name);
        CHECK(fresh.tiles[i].name == c.tiles[i].name);
        CHECK(fresh.tiles[i].heightsHash == c.tiles[i].heightsHash);
        CHECK(fresh.tiles[i].programHash == c.tiles[i].programHash);
    }
}
