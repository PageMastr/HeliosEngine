// CPU kernel equivalence (RT-04's kernel-width clause): the scalar, SSE4.2 and AVX2 kernels produce
// identical bits for every op, domain and the reference graph, and the scalar kernel matches the
// independent per-sample reference (hnoise.h + helios::Q16/Q32).
#include <doctest/doctest.h>

#include <vector>

#include "helios/core/cpu.h"
#include "helios/core/cvar.h"
#include "helios/pcg/pcg.h"

using namespace helios;
using namespace helios::pcg;

namespace {

Q32 q32(f64 v) { return Q32::fromDouble(v); }
Q16 q16(f64 v) { return Q16::fromDouble(v); }

u64 splitmix(u64& s) {
    u64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::vector<KernelKind> supportedKernels() {
    std::vector<KernelKind> out;
    for (KernelKind k : {KernelKind::Scalar, KernelKind::Sse42, KernelKind::Avx2}) {
        if (kernelSupported(k)) out.push_back(k);
    }
    return out;
}

std::vector<i64> evaluate(KernelKind kernel, const TerrainProgram& p, const TileDomain& d) {
    TileEvaluator eval(kernel);
    REQUIRE(eval.kernel() == kernel);
    std::vector<i64> h(kTileSamples);
    REQUIRE(eval.evaluate(p, d, h).ok());
    return h;
}

std::vector<TileDomain> testDomains() {
    std::vector<TileDomain> out;
    out.push_back(TileDomain::cubeSphere({CubeFace::PosX, 15, 12345, 23456}, 1'500'000.0));
    out.push_back(TileDomain::cubeSphere({CubeFace::NegZ, 3, 7, 0}, 6'400'000.0));
    out.push_back(TileDomain::cubeSphere({CubeFace::NegY, 0, 0, 0}, 250'000.0));
    out.push_back(TileDomain::cubeSphere({CubeFace::PosY, 25, (1u << 25) - 1u, 0}, 1'500'000.0));
    out.push_back(TileDomain::planar({Q32::fromDouble(-12345.5).raw(), Q32::fromDouble(9876.25).raw(), 0}, i64(1) << 30, 2, 9));
    out.push_back(TileDomain::planar({-(i64(1) << 60), i64(1) << 58, -(i64(1) << 40)}, i64(3) << 33, 1000, 0));
    return out;
}

/// A random but valid graph using every node kind (fuzzing the kernels against each other).
TerrainGraph randomGraph(u64 seed) {
    TerrainGraph g;
    u64 rng = seed;
    std::vector<NodeId> values;
    std::vector<PosId> positions = {g.basePosition()};
    const auto pickValue = [&]() { return values[splitmix(rng) % values.size()]; };
    const auto pickPos = [&]() { return positions[splitmix(rng) % positions.size()]; };
    const auto randQ32 = [&](f64 range) { return q32((static_cast<f64>(splitmix(rng) % 20001) / 10000.0 - 1.0) * range); };
    values.push_back(g.fbm(g.basePosition(), {static_cast<u32>(splitmix(rng)), 11, q16(500.0), q16(0.5), 4}));
    for (int i = 0; i < 24; ++i) {
        const u32 kind = static_cast<u32>(splitmix(rng) % static_cast<u64>(NodeKind::Count));
        const i32 e = static_cast<i32>(splitmix(rng) % 20) - 4;
        const u8 oct = static_cast<u8>(1 + splitmix(rng) % 6);
        const u32 s = static_cast<u32>(splitmix(rng));
        const Q16 amp = q16(static_cast<f64>(splitmix(rng) % 60000) / 2.0 - 15000.0);
        const Q16 gain = q16(static_cast<f64>(splitmix(rng) % 1000) / 1000.0);
        switch (static_cast<NodeKind>(kind)) {
            case NodeKind::Constant: values.push_back(g.constant(randQ32(20000.0))); break;
            case NodeKind::Noise: {
                const PosId p = pickPos();
                values.push_back(g.noise(p, s, e, amp));
                break;
            }
            case NodeKind::Fbm: {
                const PosId p = pickPos();
                values.push_back(g.fbm(p, {s, e, amp, gain, oct}));
                break;
            }
            case NodeKind::Ridged: {
                const PosId p = pickPos();
                values.push_back(g.ridged(p, {s, e, amp, gain, oct}));
                break;
            }
            case NodeKind::Warp:
                if (positions.size() < 3) {
                    const PosId p = pickPos();
                    positions.push_back(g.warp(p, {s, e, amp}));
                }
                break;
            // Operands are drawn into locals first: the evaluation order of function arguments is
            // unspecified, and the graph must not depend on the compiler.
            case NodeKind::Add: {
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.add(x, y));
                break;
            }
            case NodeKind::Sub: {
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.sub(x, y));
                break;
            }
            case NodeKind::Mul: {
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.mul(x, y));
                break;
            }
            case NodeKind::Min: {
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.min(x, y));
                break;
            }
            case NodeKind::Max: {
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.max(x, y));
                break;
            }
            case NodeKind::Clamp: {
                const Q32 a = randQ32(5000.0), b = randQ32(5000.0);
                const NodeId x = pickValue();
                values.push_back(g.clamp(x, a < b ? a : b, a < b ? b : a));
                break;
            }
            case NodeKind::Remap: {
                const Q32 a = randQ32(5000.0);
                const Q32 b = a + q32(1.0 + static_cast<f64>(splitmix(rng) % 5000));
                const Q32 lo = randQ32(3000.0), hi = randQ32(3000.0);
                const bool clampOut = (splitmix(rng) & 1) != 0;
                const NodeId x = pickValue();
                values.push_back(g.remap(x, a, b, lo, hi, clampOut));
                break;
            }
            case NodeKind::Select: {
                const NodeId c = pickValue();
                const Q32 t = randQ32(1000.0);
                const NodeId x = pickValue(), y = pickValue();
                values.push_back(g.select(c, t, x, y));
                break;
            }
            default: break;
        }
    }
    // Sum a few values so most of the graph is live.
    NodeId out = values.back();
    for (usize i = 0; i + 1 < values.size(); i += 3) out = g.add(out, values[i]);
    g.setOutput(out);
    return g;
}

} // namespace

TEST_CASE("kernels: selection reports the compiled kernels and honours CPUID") {
    const CpuGateReport& gate = cpuGate();
    MESSAGE("CPU: " << gate.brand << " (" << gate.usable.toString() << ")");
    CHECK(kernelSupported(KernelKind::Scalar));
    CHECK(kernelWidth(KernelKind::Scalar) == 1);
#if defined(__x86_64__) || defined(_M_X64)
    CHECK(kernelWidth(KernelKind::Sse42) == 4);
    CHECK(kernelWidth(KernelKind::Avx2) == 8);
    CHECK(kernelSupported(KernelKind::Avx2) == gate.usable.containsAll(cpuRequiredFeatures(CpuRequirement::Avx2Image)));
#endif
    CHECK(parseKernelName("avx2") == KernelKind::Avx2);
    CHECK(parseKernelName("sse42") == KernelKind::Sse42);
    CHECK(parseKernelName("scalar") == KernelKind::Scalar);
    CHECK_FALSE(parseKernelName("neon").has_value());
    CHECK(kernelName(bestSupportedKernel()) == kernelName(bestSupportedKernel()));
}

TEST_CASE("kernels: pcg.kernel selects the kernel and falls back when unsupported") {
    auto& reg = CVarRegistry::instance();
    REQUIRE(reg.find("pcg.kernel") != nullptr);
    CHECK(reg.get("pcg.kernel").value() == "avx2"); // the documented default
    REQUIRE(reg.set("pcg.kernel", "scalar", CVarSource::Code).ok());
    CHECK(initializePcgModule() == KernelKind::Scalar);
    CHECK(activeKernel() == KernelKind::Scalar);
    REQUIRE(reg.set("pcg.kernel", "bogus", CVarSource::Code).ok());
    CHECK(initializePcgModule() == bestSupportedKernel()); // bogus -> avx2 -> best supported
    REQUIRE(reg.set("pcg.kernel", "avx2", CVarSource::Code).ok());
    CHECK(initializePcgModule() == bestSupportedKernel());
    if (kernelSupported(KernelKind::Avx2)) CHECK(activeKernel() == KernelKind::Avx2);
    CHECK(selectKernel(KernelKind::Scalar).ok());
    CHECK(activeKernel() == KernelKind::Scalar);
    CHECK(initializePcgModule() == bestSupportedKernel()); // restore for the other tests
}

TEST_CASE("kernels: every single-node graph gives identical bits on every kernel and domain") {
    const std::vector<KernelKind> kernels = supportedKernels();
    MESSAGE("kernels under test: " << kernels.size());
    for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
        const TerrainGraph g = makeSingleNodeGraph(static_cast<NodeKind>(k));
        const TerrainProgram p = compileTerrainGraph(g, {.collisionLevel = true}).value();
        for (const TileDomain& d : testDomains()) {
            const std::vector<i64> ref = evaluate(KernelKind::Scalar, p, d);
            for (KernelKind kernel : kernels) {
                const std::vector<i64> h = evaluate(kernel, p, d);
                CHECK_MESSAGE(h == ref, nodeKindName(static_cast<NodeKind>(k)) << " differs on " << kernelName(kernel));
            }
        }
    }
}

TEST_CASE("kernels: the scalar kernel matches the per-sample reference") {
    // The reference shares no code with the kernels: hnoise.h functions and helios::Q16/Q32 only.
    std::vector<TerrainGraph> graphs;
    for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) graphs.push_back(makeSingleNodeGraph(static_cast<NodeKind>(k)));
    graphs.push_back(makeReferenceGraph40());
    for (const TerrainGraph& g : graphs) {
        const TerrainProgram p = compileTerrainGraph(g, {.collisionLevel = true}).value();
        for (const TileDomain& d : testDomains()) {
            const std::vector<i64> h = evaluate(KernelKind::Scalar, p, d);
            TileEvaluator eval(KernelKind::Scalar);
            std::vector<i64> tmp(kTileSamples);
            REQUIRE(eval.evaluate(p, d, tmp).ok());
            for (u32 idx : {0u, 1u, 64u, 65u, 2112u, 4159u, 4223u, 4224u}) {
                REQUIRE(h[idx] == evaluateSampleReference(p, d, idx));
                const FixedPos pos = eval.position(idx);
                REQUIRE(pos == samplePosition(d, idx));
            }
        }
    }
}

TEST_CASE("kernels: the reference graph is identical on every kernel (full detail and level-adaptive)") {
    const TerrainGraph g = makeReferenceGraph40();
    const std::vector<KernelKind> kernels = supportedKernels();
    for (const TileDomain& d : testDomains()) {
        for (bool collision : {true, false}) {
            CompileOptions o;
            o.collisionLevel = collision;
            o.sampleSpacingMetres = d.sampleSpacingMetres();
            const TerrainProgram p = compileTerrainGraph(g, o).value();
            const std::vector<i64> ref = evaluate(KernelKind::Scalar, p, d);
            for (KernelKind kernel : kernels) CHECK(evaluate(kernel, p, d) == ref);
        }
    }
}

TEST_CASE("kernels: random graphs (every op, extreme constants) agree across kernels") {
    const std::vector<KernelKind> kernels = supportedKernels();
    const std::vector<TileDomain> domains = testDomains();
    for (u64 seed = 1; seed <= 40; ++seed) {
        const TerrainGraph g = randomGraph(seed * 0x1234567ull);
        auto program = compileTerrainGraph(g, {.collisionLevel = true});
        if (!program.ok()) continue; // a random graph may exceed the register file
        const TileDomain& d = domains[seed % domains.size()];
        const std::vector<i64> ref = evaluate(KernelKind::Scalar, *program, d);
        for (KernelKind kernel : kernels) {
            CHECK_MESSAGE(evaluate(kernel, *program, d) == ref, "seed " << seed << " kernel " << kernelName(kernel));
        }
        for (u32 idx : {0u, 777u, 4224u}) CHECK(ref[idx] == evaluateSampleReference(*program, d, idx));
    }
}

TEST_CASE("kernels: Q32 multiply and wrapping arithmetic at the extremes") {
    const i64 extremes[] = {0, 1, -1, INT64_MAX, INT64_MIN, i64(1) << 32, -(i64(1) << 32), 0x7FFFFFFF80000000ll,
                            static_cast<i64>(0x8000000080000000ull), 0x00000000FFFFFFFFll, -0x00000000FFFFFFFFll};
    const std::vector<KernelKind> kernels = supportedKernels();
    for (i64 a : extremes) {
        for (i64 b : extremes) {
            TerrainGraph g;
            const NodeId ca = g.constant(Q32::fromRaw(a)), cb = g.constant(Q32::fromRaw(b));
            NodeId out = g.mul(ca, cb);
            out = g.add(out, g.sub(ca, cb));
            out = g.add(out, g.max(ca, cb));
            out = g.sub(out, g.min(ca, cb));
            out = g.add(out, g.select(ca, Q32::fromRaw(b), ca, cb));
            g.setOutput(out);
            const TerrainProgram p = compileTerrainGraph(g).value();
            const TileDomain d = TileDomain::planar({}, 1, 0, 0);
            const i64 expect = evaluateSampleReference(p, d, 0);
            const Q32 qa = Q32::fromRaw(a), qb = Q32::fromRaw(b);
            const Q32 manual = qa * qb + (qa - qb) + std::max(qa, qb) - std::min(qa, qb) + (qa >= qb ? qa : qb);
            CHECK(expect == manual.raw());
            for (KernelKind kernel : kernels) CHECK(evaluate(kernel, p, d)[4224] == expect);
        }
    }
}

TEST_CASE("kernels: evaluation rejects malformed programs") {
    TileEvaluator eval(KernelKind::Scalar);
    std::vector<i64> h(kTileSamples);
    const TileDomain d = TileDomain::planar({}, i64(1) << 32, 0, 0);
    TerrainProgram p = compileTerrainGraph(makeSingleNodeGraph(NodeKind::Add)).value();
    TerrainProgram bad = p;
    bad.code[0].w[0] = (bad.code[0].w[0] & ~0xFFu) | 0x7Fu; // opcode out of range
    CHECK(eval.evaluate(bad, d, h).errorCode() == ErrorCode::Corrupt);
    bad = p;
    bad.code.back().w[0] |= 0xFFu << 16; // register out of range
    CHECK(eval.evaluate(bad, d, h).errorCode() == ErrorCode::Corrupt);
    bad = p;
    bad.code[0].setFractal(30, -10, 1, 1, 1); // octaves run below 2^-16 m
    CHECK(eval.evaluate(bad, d, h).errorCode() == ErrorCode::Corrupt);
    bad = p;
    bad.heightRegisters = 40;
    CHECK(eval.evaluate(bad, d, h).errorCode() == ErrorCode::InvalidArgument);
    std::vector<i64> small(10);
    CHECK(eval.evaluate(p, d, small).errorCode() == ErrorCode::InvalidArgument);
    CHECK(eval.evaluate(p, d, h).ok());
}

TEST_CASE("kernels: tiles are seamless: shared edges of neighbouring tiles are bit-identical") {
    const TerrainProgram p = compileTerrainGraph(makeReferenceGraph40(), {.collisionLevel = true}).value();
    const KernelKind k = bestSupportedKernel();
    const f64 radius = 1'500'000.0;
    // Same face: tile (x, y) right edge == tile (x + 1, y) left edge.
    const auto a = evaluate(k, p, TileDomain::cubeSphere({CubeFace::PosZ, 12, 100, 200}, radius));
    const auto b = evaluate(k, p, TileDomain::cubeSphere({CubeFace::PosZ, 12, 101, 200}, radius));
    const auto c = evaluate(k, p, TileDomain::cubeSphere({CubeFace::PosZ, 12, 100, 201}, radius));
    for (u32 j = 0; j < kTileEdge; ++j) CHECK(a[j * kTileEdge + 64] == b[j * kTileEdge + 0]);
    for (u32 i = 0; i < kTileEdge; ++i) CHECK(a[64 * kTileEdge + i] == c[0 * kTileEdge + i]);
    // Across a cube edge: +X's u = +1 column equals -Z's u = -1 column (both v = +Y).
    const u32 n = 1u << 12;
    const auto px = evaluate(k, p, TileDomain::cubeSphere({CubeFace::PosX, 12, n - 1, 777}, radius));
    const auto nz = evaluate(k, p, TileDomain::cubeSphere({CubeFace::NegZ, 12, 0, 777}, radius));
    for (u32 j = 0; j < kTileEdge; ++j) CHECK(px[j * kTileEdge + 64] == nz[j * kTileEdge + 0]);
    // A parent tile's samples coincide with every other sample of its children.
    const auto parent = evaluate(k, p, TileDomain::cubeSphere({CubeFace::PosZ, 11, 50, 100}, radius));
    for (u32 j = 0; j < 32; ++j) {
        for (u32 i = 0; i < 32; ++i) CHECK(parent[j * kTileEdge + i] == a[(2 * j) * kTileEdge + 2 * i]);
    }
}

TEST_CASE("kernels: evaluation is repeatable and independent of the evaluator instance") {
    const TerrainProgram p = compileTerrainGraph(makeReferenceGraph40(), {.collisionLevel = true}).value();
    const TileDomain d = TileDomain::cubeSphere({CubeFace::NegX, 14, 9000, 3000}, 1'500'000.0);
    TileEvaluator a;
    std::vector<i64> h1(kTileSamples), h2(kTileSamples), h3(kTileSamples);
    REQUIRE(a.evaluate(p, d, h1).ok());
    // An unrelated tile in between must not leak state.
    REQUIRE(a.evaluate(compileTerrainGraph(makeSingleNodeGraph(NodeKind::Warp)).value(),
                       TileDomain::planar({}, 1 << 20, 4, 4), h2).ok());
    REQUIRE(a.evaluate(p, d, h2).ok());
    TileEvaluator b;
    REQUIRE(b.evaluate(p, d, h3).ok());
    CHECK(h1 == h2);
    CHECK(h1 == h3);
    CHECK(hashHeights(h1) == hashHeights(h3));
}

// -- Review regressions (WP-0.9 adversarial review) --------------------------------------------------

TEST_CASE("kernels: a program that reads a register before writing it is rejected (CPU/GPU twin divergence)") {
    // The CPU evaluator keeps its scratch registers between tiles while the Slang twin starts every
    // sample with zero heights and p1..p3 = p0, so a read-before-write would make the result depend
    // on the previous tile on the CPU and differ from the GPU. Compiled programs never do it; a
    // hand-made or corrupted one must be refused before it can reach either twin.
    TileEvaluator eval(KernelKind::Scalar);
    std::vector<i64> h(kTileSamples);
    const TileDomain d = TileDomain::planar({}, i64(1) << 32, 0, 0);
    TerrainProgram p;
    p.heightRegisters = 2;
    p.positionRegisters = 1;
    p.outputRegister = 0;
    p.code.push_back(Instr::make(Opcode::Add, 0, 1, 1)); // h1 is never written
    CHECK(eval.evaluate(p, d, h).errorCode() == ErrorCode::Corrupt);
    // A position register read before any warp wrote it.
    p.positionRegisters = 2;
    p.code.assign(1, Instr::make(Opcode::Noise, 0, 1));
    p.code[0].setFractal(1, 8, 7, 65536, 65536);
    CHECK(eval.evaluate(p, d, h).errorCode() == ErrorCode::Corrupt);
    // An output register nothing writes (an empty program included).
    p.positionRegisters = 1;
    p.code.assign(1, Instr::make(Opcode::Const, 1));
    CHECK(eval.evaluate(p, d, h).errorCode() == ErrorCode::Corrupt);
    p.code.clear();
    CHECK(eval.evaluate(p, d, h).errorCode() == ErrorCode::Corrupt);
    // Writing before reading is fine, including an in-place update (dst == a).
    p.code.assign(1, Instr::make(Opcode::Const, 1));
    p.code[0].setK(0, i64(5) << 32);
    p.code.push_back(Instr::make(Opcode::Add, 1, 1, 1));
    p.code.push_back(Instr::make(Opcode::Add, 0, 1, 1));
    REQUIRE(eval.evaluate(p, d, h).ok());
    CHECK(h[0] == (i64(20) << 32));
}

TEST_CASE("kernels: tile domains outside the cube-sphere lattice are rejected, not clamped") {
    // A level above 25 used to be evaluated as level 25 and a face >= 6 as +X; tile coordinates at or
    // beyond 2^level wrapped around the face. All of them silently produced some other tile.
    TileEvaluator eval(KernelKind::Scalar);
    std::vector<i64> h(kTileSamples);
    const TerrainProgram p = compileTerrainGraph(makeSingleNodeGraph(NodeKind::Fbm)).value();
    const f64 r = 1'500'000.0;
    CHECK(eval.evaluate(p, TileDomain::cubeSphere({CubeFace::PosX, 26, 0, 0}, r), h).errorCode() == ErrorCode::InvalidArgument);
    CHECK(eval.evaluate(p, TileDomain::cubeSphere({CubeFace::PosX, 3, 8, 0}, r), h).errorCode() == ErrorCode::InvalidArgument);
    CHECK(eval.evaluate(p, TileDomain::cubeSphere({CubeFace::PosX, 3, 0, 8}, r), h).errorCode() == ErrorCode::InvalidArgument);
    CHECK(eval.evaluate(p, TileDomain::cubeSphere({static_cast<CubeFace>(6), 3, 0, 0}, r), h).errorCode() ==
          ErrorCode::InvalidArgument);
    TileDomain bad = TileDomain::planar({}, i64(1) << 32, 0, 0);
    bad.kind = static_cast<DomainKind>(7);
    CHECK(eval.evaluate(p, bad, h).errorCode() == ErrorCode::InvalidArgument);
    CHECK(validateTileDomain(TileDomain::cubeSphere({CubeFace::NegZ, 25, (1u << 25) - 1u, (1u << 25) - 1u}, r)).ok());
    CHECK(eval.evaluate(p, TileDomain::cubeSphere({CubeFace::PosX, 3, 7, 7}, r), h).ok());
    // The per-sample reference indexes its register arrays with the program's register fields:
    // it refuses what the evaluator refuses instead of writing out of bounds.
    TerrainProgram wild = p;
    wild.code.back().w[0] |= 0xC8u << 8; // dst = 200
    CHECK(evaluateSampleReference(wild, TileDomain::planar({}, i64(1) << 32, 0, 0), 5) == 0);
    CHECK(evaluateSampleReference(p, TileDomain::cubeSphere({CubeFace::PosX, 26, 0, 0}, r), 5) == 0);
}
