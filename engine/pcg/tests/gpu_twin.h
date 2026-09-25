// Runs the Slang hnoise twin (shaders/pcg/hnoise_tile.slang) through engine/rhi compute: tile heights
// and positions for the conformance test, lattice cases for the corpus, and timed dispatches for the
// WP-0.9c spike. Shared by pcg_gpu_tests and pcg_hnoise_bench (graphics builds only; the pcg module
// itself never links rhi).
#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/pcg/pcg.h"
#include "helios/rhi/rhi.h"

namespace helios::pcg::test {

struct LatticeCase;

struct GpuTiming {
    f64 msPerDispatch = 0.0; ///< median wall time of submit + wait for `tiles` tiles (baseline subtracted)
    f64 baselineMs = 0.0;    ///< median wall time of an empty dispatch (submit overhead)
    u32 tiles = 0;
    f64 tilesPer0_8ms() const noexcept { return msPerDispatch > 0.0 ? tiles * 0.8 / msPerDispatch : 0.0; }
};

class GpuTwin {
public:
    /// Creates a Vulkan device (software adapter preferred unless HELIOS_RHI_ADAPTER overrides it)
    /// and the twin's pipelines.
    static Result<std::unique_ptr<GpuTwin>> create(bool preferSoftware = true);
    ~GpuTwin();
    GpuTwin(const GpuTwin&) = delete;
    GpuTwin& operator=(const GpuTwin&) = delete;

    const rhi::Caps& caps() const noexcept;
    std::string adapterDescription() const;
    u64 validationErrors() const noexcept;

    /// Evaluates `program` on every domain: heights (tiles x kTileSamples) and, if requested, base
    /// positions (tiles x kTileSamples).
    Result<void> evaluateTiles(const TerrainProgram& program, std::span<const TileDomain> domains, std::vector<i64>& heights,
                               std::vector<FixedPos>* positions = nullptr);

    /// latticeHash and noise3 of every case on the GPU.
    Result<void> evaluateLattice(std::span<const LatticeCase> cases, std::vector<u32>& hashes, std::vector<i32>& noises);

    /// Times one dispatch of `tiles` tiles of `program` (median of `repeats`); heights stay on the GPU.
    Result<GpuTiming> timeTiles(const TerrainProgram& program, std::span<const TileDomain> domains, u32 repeats);

private:
    GpuTwin() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace helios::pcg::test
