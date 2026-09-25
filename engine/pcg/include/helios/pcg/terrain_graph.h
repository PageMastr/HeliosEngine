// helios/pcg/terrain_graph.h — the terrain node graph (02 §5.8 "Terrain graph").
//
// A body's terrain is a typed DAG of value nodes (heights and masks in Q32.32 metres) and position
// nodes (Q32.32 sample positions; the base domain position and domain warps). Nodes can only refer
// to nodes created before them, so every graph is acyclic by construction. compileTerrainGraph()
// (program.h) turns a graph into register bytecode that the CPU kernels and the Slang twin evaluate
// over whole 65 x 65 tiles.
//
// Node set (Phase 0; 02 §5.8's generator/filter families grow on top of these):
//   constant                       out = k
//   noise(pos, seed, e, amp)       out = amp * hnoise(pos at wavelength 2^e m)
//   fbm(pos, fractal)              out = sum_k amp_k * hnoise_k, amp_{k+1} = amp_k * gain, e_k = e - k
//   ridged(pos, fractal)           out = sum_k amp_k * (1 - |hnoise_k|)^2   (clamped at 0 inside)
//   warp(pos, seed, e, amp)        pos' = pos + amp * (hnoise_x, hnoise_y, hnoise_z)   (domain warp)
//   add sub mul min max            Q32.32 arithmetic (add/sub wrap; mul rounds like helios::Q32)
//   clamp(a, lo, hi)               min(max(a, lo), hi)
//   remap(a, inLo, inHi, outLo, outHi[, clamp])   outLo + (a - inLo) * ((outHi - outLo) / (inHi - inLo))
//   select(cond, threshold, a, b)  cond >= threshold ? a : b
// Amplitudes are Q16.16 metres (|amp| < 32768 m per octave); gains are Q16.
//
// Threading: a graph is a plain value; build it on one thread, then share it read-only.
#pragma once

#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/math/fixed.h"

namespace helios::pcg {

enum class NodeKind : u8 {
    Constant,
    Noise,
    Fbm,
    Ridged,
    Warp, ///< position node
    Add,
    Sub,
    Mul,
    Min,
    Max,
    Clamp,
    Remap,
    Select,
    Count
};

/// Display name ("fbm", "warp", ...).
std::string_view nodeKindName(NodeKind kind) noexcept;
/// True for the fractal generators whose octave count the level-adaptive rule may shorten.
constexpr bool isGenerator(NodeKind k) noexcept {
    return k == NodeKind::Noise || k == NodeKind::Fbm || k == NodeKind::Ridged;
}

/// A value node (height or mask). Default-constructed ids are invalid.
struct NodeId {
    u16 index = 0xFFFF;
    constexpr bool valid() const noexcept { return index != 0xFFFF; }
    friend constexpr bool operator==(NodeId, NodeId) = default;
};
/// A position node. PosId{0} is the tile's base position (TerrainGraph::basePosition()).
struct PosId {
    u16 index = 0xFFFF;
    constexpr bool valid() const noexcept { return index != 0xFFFF; }
    friend constexpr bool operator==(PosId, PosId) = default;
};

/// Parameters of noise, fbm and ridged generators.
struct FractalParams {
    u32 seed = 0;
    i32 wavelengthExp = 10; ///< base wavelength 2^e metres, e in [-16, 30]
    Q16 amplitude = Q16::fromInt(100); ///< metres, first octave
    Q16 gain = Q16::half();            ///< amplitude ratio between octaves
    u8 octaves = 8;                    ///< 1..24 (wavelength of the last octave >= 2^-16 m)
};

/// Parameters of a domain warp.
struct WarpParams {
    u32 seed = 0;
    i32 wavelengthExp = 12;
    Q16 amplitude = Q16::fromInt(500); ///< metres of displacement per axis at |noise| = 1
};

/// One node of the graph. Inputs refer to earlier nodes (value or position, per kind).
struct TerrainNode {
    NodeKind kind = NodeKind::Constant;
    u16 inputs[3] = {0xFFFF, 0xFFFF, 0xFFFF}; ///< value inputs (a, b, c) or the position input in [0]
    FractalParams fractal;                   ///< generators and warp (octaves = 1 for noise/warp)
    Q32 k[4] = {};                           ///< constants: value / lo, hi / inLo, inHi, outLo, outHi / threshold
    bool clampOutput = false;                ///< remap
};

class TerrainGraph {
public:
    TerrainGraph();

    /// The tile's base sample position (always node 0).
    PosId basePosition() const noexcept { return PosId{0}; }

    NodeId constant(Q32 value);
    NodeId noise(PosId pos, u32 seed, i32 wavelengthExp, Q16 amplitude);
    NodeId fbm(PosId pos, const FractalParams& params);
    NodeId ridged(PosId pos, const FractalParams& params);
    PosId warp(PosId pos, const WarpParams& params);
    NodeId add(NodeId a, NodeId b);
    NodeId sub(NodeId a, NodeId b);
    NodeId mul(NodeId a, NodeId b);
    NodeId min(NodeId a, NodeId b);
    NodeId max(NodeId a, NodeId b);
    NodeId clamp(NodeId a, Q32 lo, Q32 hi);
    NodeId remap(NodeId a, Q32 inLo, Q32 inHi, Q32 outLo, Q32 outHi, bool clampOutput = false);
    NodeId select(NodeId cond, Q32 threshold, NodeId ifAtLeast, NodeId ifBelow);

    /// The node whose value is the tile height.
    void setOutput(NodeId node) { m_output = node; }
    NodeId output() const noexcept { return m_output; }

    /// All nodes including the base position (node 0).
    const std::vector<TerrainNode>& nodes() const noexcept { return m_nodes; }
    /// Nodes excluding the base position.
    usize nodeCount() const noexcept { return m_nodes.size() - 1; }

    /// Checks references, parameter ranges and the output. The first builder error (for example an
    /// input id from another graph) is reported here too.
    Result<void> validate() const;

private:
    NodeId addValue(TerrainNode node);
    void fail(std::string message);

    std::vector<TerrainNode> m_nodes;
    NodeId m_output;
    std::string m_error;
};

} // namespace helios::pcg
