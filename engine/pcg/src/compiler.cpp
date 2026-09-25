// Terrain graph -> register bytecode (helios/pcg/program.h).
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#include "helios/core/hash.h"
#include "helios/pcg/program.h"

namespace helios::pcg {

namespace {

Opcode opcodeOf(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Constant: return Opcode::Const;
        case NodeKind::Noise: return Opcode::Noise;
        case NodeKind::Fbm: return Opcode::Fbm;
        case NodeKind::Ridged: return Opcode::Ridged;
        case NodeKind::Warp: return Opcode::Warp;
        case NodeKind::Add: return Opcode::Add;
        case NodeKind::Sub: return Opcode::Sub;
        case NodeKind::Mul: return Opcode::Mul;
        case NodeKind::Min: return Opcode::Min;
        case NodeKind::Max: return Opcode::Max;
        case NodeKind::Clamp: return Opcode::Clamp;
        case NodeKind::Remap: return Opcode::Remap;
        default: return Opcode::Select;
    }
}

u32 valueInputCount(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Add:
        case NodeKind::Sub:
        case NodeKind::Mul:
        case NodeKind::Min:
        case NodeKind::Max: return 2;
        case NodeKind::Clamp:
        case NodeKind::Remap: return 1;
        case NodeKind::Select: return 3;
        default: return 0;
    }
}

bool isPositionNode(const TerrainNode& n, usize index) noexcept { return index == 0 || n.kind == NodeKind::Warp; }
bool usesPosition(NodeKind k) noexcept { return isGenerator(k) || k == NodeKind::Warp; }

/// Upper bound of |hnoise::noise3| in Q16 units of 1: the maximum over a cell of the fade-weighted
/// sum of each corner's best gradient dot (the two largest |offset| components) is 1.03635, at
/// fractions near (0.36, 0.48, 0.5); 1.04 also covers the fixed-point rounding of the lerps.
constexpr f64 kNoiseBound = 1.04;
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

/// a * b for non-negative bounds, where 0 * inf is 0 (a factor that is exactly zero).
f64 mulBound(f64 a, f64 b) noexcept { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

f64 absQ32(Q32 v) noexcept { return std::abs(v.toDouble()); }

/// How much one octave of a generator can move the generator's own value, metres.
f64 octaveEffect(const TerrainNode& n, u32 octave) noexcept {
    const f64 amp = std::abs(octaveAmplitude(n.fractal.amplitude, n.fractal.gain, octave).toDouble());
    return n.kind == NodeKind::Ridged ? amp : amp * kNoiseBound; // ridged octaves are amp * t^2, t in [0, 1]
}

/// Static error analysis for the level-adaptive rule (03 §5.5a): for every live value node, a bound
/// on |value| (forward) and on how much a change of its value can move the graph output
/// (backward: a Lipschitz bound through add/sub/min/max/clamp (1), remap (|scale|) and mul (the other
/// operand's bound)). A select condition gets an infinite bound: it switches between the branches.
/// Pure f64 arithmetic on (graph) only, so CPU and GPU programs skip identically.
std::vector<f64> outputSensitivity(const std::vector<TerrainNode>& nodes, const std::vector<u8>& live, u16 output) {
    const usize count = nodes.size();
    std::vector<f64> bound(count, 0.0);
    for (usize i = 1; i < count; ++i) {
        if (!live[i]) continue;
        const TerrainNode& n = nodes[i];
        const auto in = [&](u32 s) { return bound[n.inputs[s]]; };
        switch (n.kind) {
            case NodeKind::Constant: bound[i] = absQ32(n.k[0]); break;
            case NodeKind::Noise:
            case NodeKind::Fbm:
            case NodeKind::Ridged: {
                f64 b = 0.0;
                for (u32 o = 0; o < n.fractal.octaves; ++o) b += octaveEffect(n, o);
                bound[i] = b;
                break;
            }
            case NodeKind::Add:
            case NodeKind::Sub: bound[i] = in(0) + in(1); break;
            case NodeKind::Mul: bound[i] = mulBound(in(0), in(1)); break;
            case NodeKind::Min:
            case NodeKind::Max: bound[i] = std::max(in(0), in(1)); break;
            case NodeKind::Clamp: bound[i] = std::max(absQ32(n.k[0]), absQ32(n.k[1])); break;
            case NodeKind::Remap: {
                const f64 scale = absQ32((n.k[3] - n.k[2]) / (n.k[1] - n.k[0]));
                bound[i] = absQ32(n.k[2]) + mulBound(in(0) + absQ32(n.k[0]), scale);
                if (n.clampOutput) bound[i] = std::min(bound[i], std::max(absQ32(n.k[2]), absQ32(n.k[3])));
                break;
            }
            case NodeKind::Select: bound[i] = std::max(in(1), in(2)); break;
            default: break; // position nodes carry no value
        }
    }
    std::vector<f64> sens(count, 0.0);
    sens[output] = 1.0;
    for (usize i = count; i-- > 1;) {
        if (!live[i] || sens[i] == 0.0) continue;
        const TerrainNode& n = nodes[i];
        const f64 s = sens[i];
        const auto pass = [&](u32 slot, f64 gain) { sens[n.inputs[slot]] += mulBound(s, gain); };
        switch (n.kind) {
            case NodeKind::Add:
            case NodeKind::Sub:
            case NodeKind::Min:
            case NodeKind::Max:
                pass(0, 1.0);
                pass(1, 1.0);
                break;
            case NodeKind::Mul:
                pass(0, bound[n.inputs[1]]);
                pass(1, bound[n.inputs[0]]);
                break;
            case NodeKind::Clamp: pass(0, 1.0); break;
            case NodeKind::Remap: pass(0, absQ32((n.k[3] - n.k[2]) / (n.k[1] - n.k[0]))); break;
            case NodeKind::Select:
                sens[n.inputs[0]] = kInf;
                pass(1, 1.0);
                pass(2, 1.0);
                break;
            default: break;
        }
    }
    return sens;
}

} // namespace

Instr Instr::make(Opcode op, u8 dst, u8 a, u8 b, u8 c) noexcept {
    Instr in;
    in.w[0] = static_cast<u32>(op) | (u32(dst) << 8) | (u32(a) << 16) | (u32(b) << 24);
    in.w[1] = c;
    return in;
}

void Instr::setFractal(u8 octaves, i32 wavelengthExp, u32 seed, i32 amplitude, i32 gain) noexcept {
    w[1] = (w[1] & 0xFF0000FFu) | (u32(octaves) << 8) | (u32(static_cast<u8>(static_cast<i8>(wavelengthExp))) << 16);
    w[2] = seed;
    w[3] = static_cast<u32>(amplitude);
    w[4] = static_cast<u32>(gain);
}

void Instr::setFlags(u8 flags) noexcept { w[1] = (w[1] & 0x00FFFFFFu) | (u32(flags) << 24); }

void Instr::setK(u32 i, i64 value) noexcept {
    w[8 + 2 * i] = static_cast<u32>(static_cast<u64>(value));
    w[9 + 2 * i] = static_cast<u32>(static_cast<u64>(value) >> 32);
}

Q16 octaveAmplitude(Q16 amplitude, Q16 gain, u32 octave) noexcept {
    Q16 a = amplitude;
    for (u32 i = 0; i < octave; ++i) a = a * gain;
    return a;
}

u64 TerrainProgram::hash() const noexcept {
    const std::span<const u32> ws = words();
    u64 h = hash64(ws.data(), ws.size_bytes(), 0x9C6D1A2Bu);
    const u32 meta[3] = {heightRegisters, positionRegisters, outputRegister};
    return hash64(meta, sizeof(meta), h);
}

Result<TerrainProgram> compileTerrainGraph(const TerrainGraph& graph, const CompileOptions& options) {
    HELIOS_TRY(graph.validate());
    // An infinite spacing or amplitude budget would skip every octave; a NaN would disable the rule
    // silently. Both are caller errors.
    const auto finiteNonNegative = [](f64 v) { return v >= 0.0 && v <= std::numeric_limits<f64>::max(); };
    if (!finiteNonNegative(options.sampleSpacingMetres) || !finiteNonNegative(options.minSkipSpacingMetres) ||
        !finiteNonNegative(options.skipWavelengthFactor) || !finiteNonNegative(options.skippedAmplitudePerSpacing)) {
        return Error{ErrorCode::InvalidArgument, "compileTerrainGraph: CompileOptions must be finite and >= 0"};
    }
    const std::vector<TerrainNode>& nodes = graph.nodes();
    const usize count = nodes.size();

    // Reachability from the output (unused nodes are not evaluated).
    std::vector<u8> live(count, 0);
    live[graph.output().index] = 1;
    for (usize i = count; i-- > 1;) {
        if (!live[i]) continue;
        const TerrainNode& n = nodes[i];
        if (usesPosition(n.kind)) live[n.inputs[0]] = 1;
        for (u32 s = 0; s < valueInputCount(n.kind); ++s) live[n.inputs[s]] = 1;
    }
    live[0] = 1;

    // Level-adaptive octaves (03 §5.5a). Candidates are octaves whose wavelength is below
    // factor x spacing. Each candidate is weighed by how far it can move the graph OUTPUT: its own
    // amplitude times the noise bound times the generator's output sensitivity (a generator that a
    // remap or mul scales up counts with that gain; one feeding a select condition is never
    // skipped). Greedy and deterministic: repeatedly drop the candidate with the smallest output
    // effect among the generators' current finest octaves (ties: lowest node index) while the summed
    // effect stays below the limit. Only a generator's finest octaves are ever dropped. A pure
    // function of (graph, options), so the CPU and GPU skip identically.
    std::vector<u8> keptOctaves(count, 0);
    for (usize i = 1; i < count; ++i) {
        if (live[i] && usesPosition(nodes[i].kind)) keptOctaves[i] = nodes[i].fractal.octaves;
    }
    f64 skippedAmplitude = 0.0;
    u32 skippedOctaves = 0;
    if (!options.collisionLevel && options.sampleSpacingMetres > options.minSkipSpacingMetres) {
        const f64 limitWavelength = options.skipWavelengthFactor * options.sampleSpacingMetres;
        const f64 limitAmplitude = options.maxSkippedAmplitudeMetres();
        const std::vector<f64> sensitivity = outputSensitivity(nodes, live, graph.output().index);
        for (;;) {
            usize best = 0;
            f64 bestAmp = 0.0;
            for (usize i = 1; i < count; ++i) {
                const TerrainNode& n = nodes[i];
                if (!live[i] || !isGenerator(n.kind) || keptOctaves[i] == 0) continue;
                const u32 k = keptOctaves[i] - 1u;
                const f64 wavelength = std::ldexp(1.0, n.fractal.wavelengthExp - static_cast<i32>(k));
                if (!(wavelength < limitWavelength)) continue;
                const f64 amp = mulBound(octaveEffect(n, k), sensitivity[i]);
                if (!(amp < kInf)) continue;
                if (best == 0 || amp < bestAmp) {
                    best = i;
                    bestAmp = amp;
                }
            }
            if (best == 0 || !(skippedAmplitude + bestAmp < limitAmplitude)) break;
            skippedAmplitude += bestAmp;
            ++skippedOctaves;
            --keptOctaves[best];
        }
    }

    // Last use of every node (instruction index = node index).
    std::vector<usize> lastUse(count, 0);
    for (usize i = 1; i < count; ++i) {
        if (!live[i]) continue;
        const TerrainNode& n = nodes[i];
        if (usesPosition(n.kind)) lastUse[n.inputs[0]] = i;
        for (u32 s = 0; s < valueInputCount(n.kind); ++s) lastUse[n.inputs[s]] = i;
    }
    lastUse[graph.output().index] = count; // survives to the end

    TerrainProgram program;
    std::array<bool, kMaxHeightRegisters> heightBusy{};
    std::array<bool, kMaxPositionRegisters> posBusy{};
    posBusy[0] = true; // p0: the base position, kept for TileEvaluator::position()
    std::vector<u8> reg(count, 0);
    u32 heightUsed = 0, posUsed = 1;

    for (usize i = 1; i < count; ++i) {
        if (!live[i]) continue;
        const TerrainNode& n = nodes[i];
        // Release inputs that die here first, so dst may reuse one (every op is element-wise).
        const auto release = [&](u16 in) {
            if (lastUse[in] != i) return;
            if (isPositionNode(nodes[in], in)) {
                if (in != 0) posBusy[reg[in]] = false;
            } else {
                heightBusy[reg[in]] = false;
            }
        };
        if (usesPosition(n.kind)) release(n.inputs[0]);
        for (u32 s = 0; s < valueInputCount(n.kind); ++s) release(n.inputs[s]);

        const bool posOut = n.kind == NodeKind::Warp;
        u32 r = 0;
        if (posOut) {
            while (r < kMaxPositionRegisters && posBusy[r]) ++r;
            if (r == kMaxPositionRegisters) {
                return makeError(ErrorCode::LimitExceeded, "terrain graph needs more than {} live position registers "
                                 "(node {})", kMaxPositionRegisters, i);
            }
            posBusy[r] = true;
            posUsed = std::max(posUsed, r + 1);
        } else {
            while (r < kMaxHeightRegisters && heightBusy[r]) ++r;
            if (r == kMaxHeightRegisters) {
                return makeError(ErrorCode::LimitExceeded, "terrain graph needs more than {} live height registers "
                                 "(node {})", kMaxHeightRegisters, i);
            }
            heightBusy[r] = true;
            heightUsed = std::max(heightUsed, r + 1);
        }
        reg[i] = static_cast<u8>(r);
        // A node nobody reads (only possible for the output) keeps its register to the end.

        Instr in;
        if (isGenerator(n.kind) && keptOctaves[i] == 0) {
            in = Instr::make(Opcode::Const, reg[i]); // every octave skipped
            in.setK(0, 0);
        } else {
            u8 a = 0, b = 0, c = 0;
            if (usesPosition(n.kind)) {
                a = reg[n.inputs[0]];
            } else {
                const u32 inputs = valueInputCount(n.kind);
                if (inputs > 0) a = reg[n.inputs[0]];
                if (inputs > 1) b = reg[n.inputs[1]];
                if (inputs > 2) c = reg[n.inputs[2]];
            }
            in = Instr::make(opcodeOf(n.kind), reg[i], a, b, c);
            if (usesPosition(n.kind)) {
                in.setFractal(keptOctaves[i], n.fractal.wavelengthExp, n.fractal.seed, n.fractal.amplitude.raw(),
                              n.fractal.gain.raw());
                program.evaluatedOctaves += n.kind == NodeKind::Warp ? 3u : keptOctaves[i];
            }
            switch (n.kind) {
                case NodeKind::Constant: in.setK(0, n.k[0].raw()); break;
                case NodeKind::Clamp:
                    in.setK(0, n.k[0].raw());
                    in.setK(1, n.k[1].raw());
                    break;
                case NodeKind::Remap: {
                    const Q32 scale = (n.k[3] - n.k[2]) / (n.k[1] - n.k[0]);
                    in.setK(0, n.k[0].raw());
                    in.setK(1, scale.raw());
                    in.setK(2, n.k[2].raw());
                    in.setK(3, n.k[3].raw());
                    if (n.clampOutput) in.setFlags(kRemapClampFlag);
                    break;
                }
                case NodeKind::Select: in.setK(0, n.k[0].raw()); break;
                default: break;
            }
        }
        program.code.push_back(in);
    }

    program.heightRegisters = static_cast<u8>(heightUsed);
    program.positionRegisters = static_cast<u8>(posUsed);
    program.outputRegister = reg[graph.output().index];
    program.skippedOctaves = skippedOctaves;
    program.skippedAmplitudeMetres = skippedAmplitude;
    return program;
}

} // namespace helios::pcg
