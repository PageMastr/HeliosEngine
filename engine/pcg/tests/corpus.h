// The shared hnoise conformance corpus (tests/corpus/hnoise, README there): lattice hashes and
// noise values, tile base positions and per-tile height hashes for the per-node graphs and the
// WP-0.9c reference graph. The CPU kernels (pcg_tests) and the Slang twin (pcg_gpu_tests) must
// reproduce every value bit for bit.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/pcg/pcg.h"

namespace helios::pcg::test {

struct LatticeCase {
    u32 seed = 0;
    std::array<i32, 3> cell{};
    std::array<i32, 3> frac{};
    u32 hash = 0;  ///< latticeHash(seed, cell)
    i32 noise = 0; ///< noise3(seed, {cell, frac}), Q16
};

struct TileCase {
    std::string name;
    std::string graph; ///< "reference40" or "node:<kind name>"
    TileDomain domain;
    CompileOptions options;
    u64 programHash = 0;
    u64 positionsHash = 0; ///< hashPositions() of the base positions
    u64 heightsHash = 0;   ///< hashHeights() of the tile
    std::array<i64, 3> probes{}; ///< heights of samples 0, 2112 (centre) and 4224, for diagnostics
};

struct Corpus {
    std::vector<LatticeCase> lattice;
    std::vector<TileCase> tiles;
};

/// The graph a corpus case names.
Result<TerrainGraph> graphByName(std::string_view name);

/// Hash of a tile's base positions (x, y, z planes of kTileSamples values, little-endian).
u64 hashPositions(std::span<const FixedPos> positions);
/// Base positions of every sample of a domain from the scalar reference.
std::vector<FixedPos> referencePositions(const TileDomain& domain);

/// The corpus as the scalar reference computes it (used to (re)generate the files).
Corpus generateCorpus();

Result<Corpus> loadCorpus(const std::filesystem::path& dir);
Result<void> writeCorpus(const std::filesystem::path& dir, const Corpus& corpus);

/// Directory of the checked-in corpus (HELIOS_HNOISE_CORPUS_DIR).
std::filesystem::path corpusDir();

} // namespace helios::pcg::test
