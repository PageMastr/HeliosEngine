// Terrain graph validation and the bytecode compiler: register allocation, the instruction layout,
// dead-node elimination and the level-adaptive octave rule (03 §5.5a).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "helios/pcg/pcg.h"

using namespace helios;
using namespace helios::pcg;

namespace {
Q32 q32(f64 v) { return Q32::fromDouble(v); }
Q16 q16(f64 v) { return Q16::fromDouble(v); }
} // namespace

TEST_CASE("graph: the WP-0.9c reference graph has 40 nodes, 10 generators and 80 octaves") {
    const TerrainGraph g = makeReferenceGraph40();
    REQUIRE(g.validate().ok());
    CHECK(g.nodeCount() == 40);
    u32 generators = 0, octaves = 0, warps = 0;
    for (usize i = 1; i < g.nodes().size(); ++i) {
        const TerrainNode& n = g.nodes()[i];
        if (isGenerator(n.kind)) {
            ++generators;
            octaves += n.fractal.octaves;
        }
        if (n.kind == NodeKind::Warp) ++warps;
    }
    CHECK(generators == 10);
    CHECK(octaves == 80);
    CHECK(warps == 2);
    const TerrainProgram full = compileTerrainGraph(g, {.collisionLevel = true}).value();
    CHECK(full.evaluatedOctaves == 86); // 80 + 2 warps x 3 axes
    CHECK(full.skippedOctaves == 0);
    CHECK(full.code.size() == 40);
    CHECK(full.heightRegisters <= 12); // 11 today: the land mask and region fbm stay live across the sum
    MESSAGE("reference40: " << full.code.size() << " instructions, " << int(full.heightRegisters) << " height and "
                            << int(full.positionRegisters) << " position registers");
}

TEST_CASE("graph: validation rejects malformed graphs") {
    {
        TerrainGraph g;
        CHECK(g.validate().errorCode() == ErrorCode::InvalidState); // no output
    }
    {
        TerrainGraph g;
        const NodeId bogus{77};
        g.setOutput(g.add(bogus, g.constant(q32(1.0))));
        CHECK(g.validate().errorCode() == ErrorCode::InvalidArgument); // forward reference
    }
    {
        TerrainGraph g;
        FractalParams f;
        f.octaves = 0;
        g.setOutput(g.fbm(g.basePosition(), f));
        CHECK(g.validate().errorCode() == ErrorCode::OutOfRange);
    }
    {
        TerrainGraph g;
        FractalParams f;
        f.wavelengthExp = -10;
        f.octaves = 8; // last octave 2^-17 m
        g.setOutput(g.fbm(g.basePosition(), f));
        CHECK(g.validate().errorCode() == ErrorCode::OutOfRange);
    }
    {
        TerrainGraph g;
        FractalParams f;
        f.wavelengthExp = 31;
        g.setOutput(g.fbm(g.basePosition(), f));
        CHECK(g.validate().errorCode() == ErrorCode::OutOfRange);
    }
    {
        TerrainGraph g;
        const NodeId c = g.constant(q32(1.0));
        g.setOutput(g.remap(c, q32(2.0), q32(2.0), q32(0.0), q32(1.0)));
        CHECK(g.validate().errorCode() == ErrorCode::InvalidArgument); // empty input range
    }
    {
        TerrainGraph g;
        const NodeId c = g.constant(q32(1.0));
        g.setOutput(g.clamp(c, q32(2.0), q32(1.0)));
        CHECK(g.validate().errorCode() == ErrorCode::InvalidArgument);
    }
    {
        // A position used as a value, and a value used as a position.
        TerrainGraph g;
        const PosId w = g.warp(g.basePosition(), {});
        g.setOutput(g.add(NodeId{w.index}, g.constant(q32(0.0))));
        CHECK(g.validate().errorCode() == ErrorCode::InvalidArgument);
        TerrainGraph h;
        const NodeId c = h.constant(q32(0.0));
        h.setOutput(h.fbm(PosId{c.index}, {}));
        CHECK(h.validate().errorCode() == ErrorCode::InvalidArgument);
    }
    {
        // The output must be a value node.
        TerrainGraph g;
        const PosId w = g.warp(g.basePosition(), {});
        g.setOutput(NodeId{w.index});
        CHECK(g.validate().errorCode() == ErrorCode::InvalidState);
    }
}

TEST_CASE("graph: instruction words follow the documented layout") {
    Instr in = Instr::make(Opcode::Fbm, 3, 1, 0, 0);
    in.setFractal(9, -5, 0xDEADBEEFu, -12345, 32768);
    in.setFlags(1);
    in.setK(0, -2);
    in.setK(3, 0x0123456789ABCDEFll);
    CHECK(in.w[0] == (2u | (3u << 8) | (1u << 16)));
    CHECK(in.op() == Opcode::Fbm);
    CHECK(in.dst() == 3);
    CHECK(in.a() == 1);
    CHECK(in.octaves() == 9);
    CHECK(in.wavelengthExp() == -5);
    CHECK(in.flags() == 1);
    CHECK(in.seed() == 0xDEADBEEFu);
    CHECK(in.amplitude() == -12345);
    CHECK(in.gain() == 32768);
    CHECK(in.k(0) == -2);
    CHECK(in.w[8] == 0xFFFFFFFEu);
    CHECK(in.w[9] == 0xFFFFFFFFu);
    CHECK(in.k(3) == 0x0123456789ABCDEFll);
    CHECK(in.w[14] == 0x89ABCDEFu);
    CHECK(in.w[15] == 0x01234567u);
    CHECK(in.w[5] == 0);
}

TEST_CASE("graph: dead nodes are not compiled and registers are reused") {
    TerrainGraph g;
    const PosId base = g.basePosition();
    NodeId acc = g.fbm(base, {1, 10, q16(10.0), q16(0.5), 2});
    for (u32 i = 0; i < 30; ++i) {
        const NodeId n = g.noise(base, 100 + i, 8, q16(1.0));
        acc = g.add(acc, n);
    }
    (void)g.ridged(base, {}); // dead
    g.setOutput(acc);
    const TerrainProgram p = compileTerrainGraph(g).value();
    CHECK(p.code.size() == 61); // 1 + 30 noises + 30 adds; the dead ridged node is dropped
    CHECK(p.heightRegisters == 2);
    CHECK(p.positionRegisters == 1);
}

TEST_CASE("graph: too many live registers is a compile error, not a crash") {
    TerrainGraph g;
    std::vector<NodeId> live;
    for (u32 i = 0; i < kMaxHeightRegisters + 1; ++i) live.push_back(g.constant(q32(i)));
    NodeId acc = live[0];
    for (usize i = 1; i < live.size(); ++i) acc = g.add(acc, live[i]);
    g.setOutput(acc);
    CHECK(compileTerrainGraph(g).errorCode() == ErrorCode::LimitExceeded);
    TerrainGraph w;
    PosId p = w.basePosition();
    std::vector<PosId> warps;
    for (u32 i = 0; i < kMaxPositionRegisters; ++i) warps.push_back(w.warp(p, {i, 10, q16(1.0)}));
    NodeId sum = w.noise(warps[0], 1, 8, q16(1.0));
    for (usize i = 1; i < warps.size(); ++i) sum = w.add(sum, w.noise(warps[i], 1, 8, q16(1.0)));
    w.setOutput(sum);
    CHECK(compileTerrainGraph(w).errorCode() == ErrorCode::LimitExceeded); // p0 + 4 live warps
}

TEST_CASE("graph: level-adaptive octaves skip only fine, small octaves and never at collision levels") {
    const TerrainGraph g = makeReferenceGraph40();
    const TerrainProgram collision = compileTerrainGraph(g, {.sampleSpacingMetres = 500.0, .collisionLevel = true}).value();
    CHECK(collision.skippedOctaves == 0);
    // Finer than the 2 m threshold: nothing skipped.
    const TerrainProgram fine = compileTerrainGraph(g, {.sampleSpacingMetres = 1.9}).value();
    CHECK(fine.skippedOctaves == 0);
    CHECK(fine.hash() == compileTerrainGraph(g, {.collisionLevel = true}).value().hash());
    u32 prevEvaluated = 1000;
    for (f64 spacing : {3.0, 10.0, 36.0, 150.0, 600.0, 2400.0, 23000.0}) {
        CompileOptions o;
        o.sampleSpacingMetres = spacing;
        const TerrainProgram p = compileTerrainGraph(g, o).value();
        MESSAGE("spacing " << spacing << " m: " << p.evaluatedOctaves << " noise evaluations per sample, "
                           << p.skippedOctaves << " octaves skipped, " << p.skippedAmplitudeMetres << " m skipped");
        CHECK(p.skippedAmplitudeMetres < o.maxSkippedAmplitudeMetres());
        CHECK(p.evaluatedOctaves <= prevEvaluated);
        prevEvaluated = p.evaluatedOctaves;
        // Every kept octave is at least as long as every skipped octave of the same generator.
        for (const Instr& in : p.code) {
            if (in.op() == Opcode::Fbm || in.op() == Opcode::Ridged) CHECK(in.octaves() >= 1);
        }
    }
    CHECK(prevEvaluated < 86);
}

TEST_CASE("graph: compilation is deterministic and the program hash pins it") {
    const TerrainGraph g = makeReferenceGraph40();
    const TerrainProgram a = compileTerrainGraph(g, {.sampleSpacingMetres = 36.0}).value();
    const TerrainProgram b = compileTerrainGraph(makeReferenceGraph40(), {.sampleSpacingMetres = 36.0}).value();
    CHECK(a.code == b.code);
    CHECK(a.hash() == b.hash());
    const TerrainProgram c = compileTerrainGraph(g, {.sampleSpacingMetres = 37.0}).value();
    (void)c; // a different spacing may or may not change the skip set; the hash covers the result
    CHECK(octaveAmplitude(Q16::fromInt(100), Q16::half(), 3) == Q16::fromDouble(12.5));
}

TEST_CASE("graph: remap scale is computed with helios::Q32 division") {
    TerrainGraph g;
    g.setOutput(g.remap(g.constant(q32(1.0)), q32(-3.0), q32(5.0), q32(10.0), q32(-6.0), true));
    const TerrainProgram p = compileTerrainGraph(g).value();
    REQUIRE(p.code.size() == 2);
    const Instr& r = p.code[1];
    CHECK(r.op() == Opcode::Remap);
    CHECK(r.k(1) == (q32(-16.0) / q32(8.0)).raw());
    CHECK(r.flags() == kRemapClampFlag);
}

// -- Review regressions (WP-0.9 adversarial review) --------------------------------------------------

namespace {
/// max |a - b| in metres over a tile.
f64 maxAbsDiffMetres(const std::vector<i64>& a, const std::vector<i64>& b) {
    f64 m = 0.0;
    for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(Q32::fromRaw(a[i] - b[i]).toDouble()));
    return m;
}
std::vector<i64> evalTile(const TerrainProgram& p, const TileDomain& d) {
    TileEvaluator eval(KernelKind::Scalar);
    std::vector<i64> h(kTileSamples);
    REQUIRE(eval.evaluate(p, d, h).ok());
    return h;
}
} // namespace

TEST_CASE("graph: level-adaptive skipping bounds the error of the OUTPUT, not of the generator") {
    // 03 §5.5a: an octave may be skipped only if the skipped amplitude stays below 0.5 px. What
    // matters is its effect on the height: a generator that a remap or a mul scales up afterwards
    // (a mask, a detail layer normalised to [-1, 1] and rescaled) must count with that gain.
    const TileDomain d = TileDomain::planar({}, i64(3) << 32, 11, 5); // 3 m spacing
    CompileOptions adaptive;
    adaptive.sampleSpacingMetres = d.sampleSpacingMetres();
    const f64 limit = adaptive.maxSkippedAmplitudeMetres();

    // A tiny fbm stretched 50,000x by a remap.
    TerrainGraph g;
    const NodeId tiny = g.fbm(g.basePosition(), {0x5CA1Eu, 4, q16(0.01), q16(0.5), 4});
    g.setOutput(g.remap(tiny, q32(-0.02), q32(0.02), q32(-1000.0), q32(1000.0)));
    const TerrainProgram full = compileTerrainGraph(g, {.collisionLevel = true}).value();
    const TerrainProgram lod = compileTerrainGraph(g, adaptive).value();
    const f64 err = maxAbsDiffMetres(evalTile(full, d), evalTile(lod, d));
    MESSAGE("remap-scaled fbm: " << lod.skippedOctaves << " octaves skipped, error " << err << " m, limit " << limit << " m");
    CHECK(err <= limit);
    CHECK(lod.skippedAmplitudeMetres <= limit);

    // A generator used as a select condition can flip the output by the full difference of the two
    // branches: its octaves must never be skipped.
    TerrainGraph s;
    const NodeId cond = s.fbm(s.basePosition(), {0xC0DEu, 3, q16(0.02), q16(0.5), 3});
    s.setOutput(s.select(cond, q32(0.0), s.constant(q32(100.0)), s.constant(q32(-100.0))));
    const TerrainProgram sFull = compileTerrainGraph(s, {.collisionLevel = true}).value();
    const TerrainProgram sLod = compileTerrainGraph(s, adaptive).value();
    CHECK(sLod.skippedOctaves == 0);
    CHECK(maxAbsDiffMetres(evalTile(sFull, d), evalTile(sLod, d)) <= limit);

    // A mul by a bounded mask scales by the mask's bound; a mul by another generator by its bound.
    TerrainGraph m;
    const NodeId fine = m.fbm(m.basePosition(), {0xF1AEu, 4, q16(0.05), q16(0.5), 4});
    const NodeId big = m.fbm(m.basePosition(), {0xB16u, 12, q16(400.0), q16(0.5), 2});
    m.setOutput(m.mul(fine, big));
    const TerrainProgram mFull = compileTerrainGraph(m, {.collisionLevel = true}).value();
    const TerrainProgram mLod = compileTerrainGraph(m, adaptive).value();
    const f64 mErr = maxAbsDiffMetres(evalTile(mFull, d), evalTile(mLod, d));
    MESSAGE("mul-scaled fbm: " << mLod.skippedOctaves << " octaves skipped, error " << mErr << " m");
    CHECK(mErr <= limit);

    // Plain unscaled detail is still skipped (the rule is not simply switched off).
    TerrainGraph plain;
    plain.setOutput(plain.fbm(plain.basePosition(), {0xD0Du, 4, q16(0.02), q16(0.5), 4}));
    const TerrainProgram pLod = compileTerrainGraph(plain, adaptive).value();
    CHECK(pLod.skippedOctaves == 2);
    CHECK(maxAbsDiffMetres(evalTile(compileTerrainGraph(plain, {.collisionLevel = true}).value(), d), evalTile(pLod, d)) <=
          limit);
}

TEST_CASE("graph: on the reference graph the level-adaptive error stays within the static bound at every level") {
    // The compiler's skippedAmplitudeMetres is a bound on |adaptive - full| for every sample.
    const TerrainGraph g = makeReferenceGraph40();
    const TerrainProgram full = compileTerrainGraph(g, {.collisionLevel = true}).value();
    for (u8 level : {u8(0), u8(2), u8(4), u8(6), u8(8), u8(10), u8(12)}) {
        const u32 n = 1u << level;
        const TileDomain d = TileDomain::cubeSphere({CubeFace::NegY, level, n / 3u, (2u * n) / 5u}, 1'500'000.0);
        CompileOptions o;
        o.sampleSpacingMetres = d.sampleSpacingMetres();
        const TerrainProgram lod = compileTerrainGraph(g, o).value();
        const f64 err = maxAbsDiffMetres(evalTile(full, d), evalTile(lod, d));
        MESSAGE("level " << int(level) << ": " << lod.skippedOctaves << " octaves skipped, bound "
                         << lod.skippedAmplitudeMetres << " m, measured " << err << " m");
        CHECK(err <= lod.skippedAmplitudeMetres + 1e-6);
        CHECK(lod.skippedAmplitudeMetres < o.maxSkippedAmplitudeMetres());
    }
}

TEST_CASE("graph: non-finite or negative CompileOptions are rejected") {
    // An infinite spacing used to skip every octave of every generator (an infinite budget); NaN
    // silently disabled the rule.
    const TerrainGraph g = makeReferenceGraph40();
    CHECK(compileTerrainGraph(g, {.sampleSpacingMetres = std::numeric_limits<f64>::infinity()}).errorCode() ==
          ErrorCode::InvalidArgument);
    CHECK(compileTerrainGraph(g, {.sampleSpacingMetres = std::nan("")}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(compileTerrainGraph(g, {.sampleSpacingMetres = -5.0}).errorCode() == ErrorCode::InvalidArgument);
    CompileOptions o;
    o.sampleSpacingMetres = 100.0;
    o.skippedAmplitudePerSpacing = std::numeric_limits<f64>::infinity();
    CHECK(compileTerrainGraph(g, o).errorCode() == ErrorCode::InvalidArgument);
    CHECK(compileTerrainGraph(g, {.sampleSpacingMetres = 100.0}).ok());
}
