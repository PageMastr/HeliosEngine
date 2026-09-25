#include "helios/pcg/terrain_graph.h"

#include <format>

#include "helios/pcg/hnoise.h"
#include "helios/pcg/program.h"

namespace helios::pcg {

namespace {
constexpr u16 kNone = 0xFFFF;
bool isPositionKind(NodeKind k, u16 index) noexcept { return index == 0 || k == NodeKind::Warp; }
} // namespace

std::string_view nodeKindName(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Constant: return "constant";
        case NodeKind::Noise: return "noise";
        case NodeKind::Fbm: return "fbm";
        case NodeKind::Ridged: return "ridged";
        case NodeKind::Warp: return "warp";
        case NodeKind::Add: return "add";
        case NodeKind::Sub: return "sub";
        case NodeKind::Mul: return "mul";
        case NodeKind::Min: return "min";
        case NodeKind::Max: return "max";
        case NodeKind::Clamp: return "clamp";
        case NodeKind::Remap: return "remap";
        case NodeKind::Select: return "select";
        default: return "?";
    }
}

TerrainGraph::TerrainGraph() {
    // Node 0: the base position (kind Warp marks position nodes; it has no inputs).
    TerrainNode base;
    base.kind = NodeKind::Warp;
    m_nodes.push_back(base);
}

void TerrainGraph::fail(std::string message) {
    if (m_error.empty()) m_error = std::move(message);
}

NodeId TerrainGraph::addValue(TerrainNode node) {
    if (m_nodes.size() >= 0xFFFE) {
        fail("graph has too many nodes");
        return {};
    }
    m_nodes.push_back(node);
    return NodeId{static_cast<u16>(m_nodes.size() - 1)};
}

NodeId TerrainGraph::constant(Q32 value) {
    TerrainNode n;
    n.kind = NodeKind::Constant;
    n.k[0] = value;
    return addValue(n);
}

NodeId TerrainGraph::noise(PosId pos, u32 seed, i32 wavelengthExp, Q16 amplitude) {
    TerrainNode n;
    n.kind = NodeKind::Noise;
    n.inputs[0] = pos.index;
    n.fractal = {seed, wavelengthExp, amplitude, Q16::one(), 1};
    return addValue(n);
}

NodeId TerrainGraph::fbm(PosId pos, const FractalParams& params) {
    TerrainNode n;
    n.kind = NodeKind::Fbm;
    n.inputs[0] = pos.index;
    n.fractal = params;
    return addValue(n);
}

NodeId TerrainGraph::ridged(PosId pos, const FractalParams& params) {
    TerrainNode n;
    n.kind = NodeKind::Ridged;
    n.inputs[0] = pos.index;
    n.fractal = params;
    return addValue(n);
}

PosId TerrainGraph::warp(PosId pos, const WarpParams& params) {
    TerrainNode n;
    n.kind = NodeKind::Warp;
    n.inputs[0] = pos.index;
    n.fractal = {params.seed, params.wavelengthExp, params.amplitude, Q16::one(), 1};
    const NodeId id = addValue(n);
    return PosId{id.index};
}

namespace {
TerrainNode binary(NodeKind kind, NodeId a, NodeId b) {
    TerrainNode n;
    n.kind = kind;
    n.inputs[0] = a.index;
    n.inputs[1] = b.index;
    return n;
}
} // namespace

NodeId TerrainGraph::add(NodeId a, NodeId b) { return addValue(binary(NodeKind::Add, a, b)); }
NodeId TerrainGraph::sub(NodeId a, NodeId b) { return addValue(binary(NodeKind::Sub, a, b)); }
NodeId TerrainGraph::mul(NodeId a, NodeId b) { return addValue(binary(NodeKind::Mul, a, b)); }
NodeId TerrainGraph::min(NodeId a, NodeId b) { return addValue(binary(NodeKind::Min, a, b)); }
NodeId TerrainGraph::max(NodeId a, NodeId b) { return addValue(binary(NodeKind::Max, a, b)); }

NodeId TerrainGraph::clamp(NodeId a, Q32 lo, Q32 hi) {
    TerrainNode n;
    n.kind = NodeKind::Clamp;
    n.inputs[0] = a.index;
    n.k[0] = lo;
    n.k[1] = hi;
    return addValue(n);
}

NodeId TerrainGraph::remap(NodeId a, Q32 inLo, Q32 inHi, Q32 outLo, Q32 outHi, bool clampOutput) {
    TerrainNode n;
    n.kind = NodeKind::Remap;
    n.inputs[0] = a.index;
    n.k[0] = inLo;
    n.k[1] = inHi;
    n.k[2] = outLo;
    n.k[3] = outHi;
    n.clampOutput = clampOutput;
    return addValue(n);
}

NodeId TerrainGraph::select(NodeId cond, Q32 threshold, NodeId ifAtLeast, NodeId ifBelow) {
    TerrainNode n;
    n.kind = NodeKind::Select;
    n.inputs[0] = cond.index;
    n.inputs[1] = ifAtLeast.index;
    n.inputs[2] = ifBelow.index;
    n.k[0] = threshold;
    return addValue(n);
}

Result<void> TerrainGraph::validate() const {
    if (!m_error.empty()) return Error{ErrorCode::InvalidArgument, m_error};
    const auto inputCount = [](NodeKind k) -> u32 {
        switch (k) {
            case NodeKind::Constant: return 0;
            case NodeKind::Noise:
            case NodeKind::Fbm:
            case NodeKind::Ridged:
            case NodeKind::Warp:
            case NodeKind::Clamp:
            case NodeKind::Remap: return 1;
            case NodeKind::Select: return 3;
            default: return 2;
        }
    };
    for (usize i = 1; i < m_nodes.size(); ++i) {
        const TerrainNode& n = m_nodes[i];
        if (n.kind >= NodeKind::Count) return makeError(ErrorCode::InvalidArgument, "node {}: invalid kind", i);
        const bool positional = n.kind == NodeKind::Warp || isGenerator(n.kind);
        for (u32 s = 0; s < inputCount(n.kind); ++s) {
            const u16 in = n.inputs[s];
            if (in == kNone || in >= i) {
                return makeError(ErrorCode::InvalidArgument, "node {} ({}): input {} refers to node {}, which is not "
                                 "an earlier node", i, nodeKindName(n.kind), s, in);
            }
            const bool inIsPos = isPositionKind(m_nodes[in].kind, in);
            if (positional && s == 0 && !inIsPos) {
                return makeError(ErrorCode::InvalidArgument, "node {} ({}): input must be a position node", i,
                                 nodeKindName(n.kind));
            }
            if (!(positional && s == 0) && inIsPos) {
                return makeError(ErrorCode::InvalidArgument, "node {} ({}): input {} is a position node", i,
                                 nodeKindName(n.kind), s);
            }
        }
        if (positional) {
            const FractalParams& f = n.fractal;
            if (f.octaves < 1 || f.octaves > kMaxOctaves) {
                return makeError(ErrorCode::OutOfRange, "node {} ({}): octaves {} not in [1, {}]", i,
                                 nodeKindName(n.kind), f.octaves, kMaxOctaves);
            }
            const i32 last = f.wavelengthExp - static_cast<i32>(f.octaves) + 1;
            if (f.wavelengthExp > hnoise::kMaxWavelengthExp || last < hnoise::kMinWavelengthExp) {
                return makeError(ErrorCode::OutOfRange, "node {} ({}): wavelengths 2^{}..2^{} m outside [2^{}, 2^{}] m",
                                 i, nodeKindName(n.kind), last, f.wavelengthExp, hnoise::kMinWavelengthExp,
                                 hnoise::kMaxWavelengthExp);
            }
        }
        if (n.kind == NodeKind::Remap && n.k[0] == n.k[1]) {
            return makeError(ErrorCode::InvalidArgument, "node {} (remap): empty input range", i);
        }
        if (n.kind == NodeKind::Clamp && n.k[1] < n.k[0]) {
            return makeError(ErrorCode::InvalidArgument, "node {} (clamp): hi < lo", i);
        }
    }
    if (!m_output.valid() || m_output.index >= m_nodes.size() || isPositionKind(m_nodes[m_output.index].kind, m_output.index)) {
        return Error{ErrorCode::InvalidState, "terrain graph has no value output (setOutput)"};
    }
    return {};
}

} // namespace helios::pcg
