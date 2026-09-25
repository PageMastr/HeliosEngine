// helios/pcg/tile.h — tile domains and the SoA tile evaluator (02 §5.8 "Tiles", "Evaluation").
//
// A tile is 65 x 65 samples (64 x 64 quads, shared edges with its neighbours). Sample (i, j) has
// index j * 65 + i; i runs along the face's u axis (x), j along v (y). Heights are Q32.32 metres.
//
// Domains:
//   * CubeSphere: CubeTile {face, level, x, y} on a body of radius R (math's CubeFace bases,
//     fixed-point EquiAngular warp; hnoise.h). The same tile serves rendering and collision.
//   * Planar: a flat grid (test worlds, benches): position = origin + (x*64 + i, y*64 + j, 0) * spacing.
//
// TileEvaluator evaluates a TerrainProgram over one tile with a CPU kernel (kernel.h): each
// instruction runs over the whole tile before the next (one indirect call per op per tile).
//
// Threading: a TileEvaluator owns scratch registers (about 1 MB) and is single-threaded; use one per
// worker. Programs and domains are shared read-only.
#pragma once

#include <memory>
#include <span>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/math/spherical.h"
#include "helios/pcg/hnoise.h"
#include "helios/pcg/kernel.h"
#include "helios/pcg/program.h"

namespace helios::pcg {

inline constexpr u32 kTileEdge = 65;
inline constexpr u32 kTileSamples = kTileEdge * kTileEdge;
/// Samples per register plane, padded to a multiple of the widest kernel (8 lanes).
inline constexpr u32 kTileStride = (kTileSamples + 7) & ~7u;

enum class DomainKind : u8 { Planar = 0, CubeSphere = 1 };

/// Where a tile's samples are. Packs into kTileDomainWords u32 words for the GPU twin.
struct TileDomain {
    DomainKind kind = DomainKind::Planar;
    CubeTile tile{};          ///< cube-sphere tile, or planar tile x/y (face and level ignored)
    u32 radiusQ8 = 0;         ///< cube-sphere radius, Q24.8 metres
    FixedPos origin{};        ///< planar origin (Q32.32 metres)
    i64 spacingRaw = 0;       ///< planar sample spacing (Q32.32 metres)

    /// Cube-sphere tile of a body with the given radius (metres, < 8,388 km).
    static TileDomain cubeSphere(const CubeTile& tile, f64 radiusMetres) noexcept;
    /// Planar tile (x, y) of a grid whose tiles are 64 samples of `spacingRaw` apart.
    static TileDomain planar(const FixedPos& origin, i64 spacingRaw, u32 tileX, u32 tileY) noexcept;

    /// Approximate sample spacing in metres (for CompileOptions): the planar spacing, or the
    /// EquiAngular arc length per sample, (pi/2) R / (64 * 2^level).
    f64 sampleSpacingMetres() const noexcept;

    /// GPU/corpus packing (see shaders/pcg/hnoise.slang TileDomainGpu).
    static constexpr u32 kWords = 16;
    std::array<u32, kWords> pack() const noexcept;
};

/// Checks that a domain names a real tile: a known kind; for cube-sphere tiles a face < 6, a level
/// <= hnoise::kMaxCubeLevel and x, y < 2^level. TileEvaluator::evaluate() refuses anything else.
Result<void> validateTileDomain(const TileDomain& domain) noexcept;

/// Base sample position of sample `index` (the domain step of the scalar reference).
FixedPos samplePosition(const TileDomain& domain, u32 index) noexcept;

/// Stable 64-bit hash of a height tile (XXH3 over the little-endian Q32.32 values).
u64 hashHeights(std::span<const i64> heights) noexcept;

class TileEvaluator {
public:
    /// Uses `kernel`, or the active kernel (activeKernel()) when none is given. An unsupported
    /// explicit kernel falls back to the active one; kernel() reports what is used.
    explicit TileEvaluator(std::optional<KernelKind> kernel = std::nullopt);
    ~TileEvaluator();
    TileEvaluator(const TileEvaluator&) = delete;
    TileEvaluator& operator=(const TileEvaluator&) = delete;

    KernelKind kernel() const noexcept { return m_kernel; }

    /// Evaluates `program` over the tile into `heights` (kTileSamples values). Fails on a program
    /// that uses more registers than the evaluator supports or a malformed instruction.
    Result<void> evaluate(const TerrainProgram& program, const TileDomain& domain, std::span<i64> heights);

    /// Base positions of the last evaluated tile (x, y, z planes of kTileStride samples each).
    FixedPos position(u32 index) const noexcept;

    struct Scratch;

private:
    KernelKind m_kernel;
    std::unique_ptr<Scratch> m_scratch;
};

/// Reference evaluation of one sample, straight from hnoise.h with helios::Q16/Q32 arithmetic (no
/// kernel code). Slow; used by tests to check the kernels independently. Returns 0 for a program or
/// domain that validateTerrainProgram() / validateTileDomain() reject (its register indices are
/// only safe to use after that check).
i64 evaluateSampleReference(const TerrainProgram& program, const TileDomain& domain, u32 index);

} // namespace helios::pcg
