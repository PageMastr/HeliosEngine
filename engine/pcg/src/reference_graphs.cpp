#include "helios/pcg/reference_graphs.h"

#include "helios/core/assert.h"

namespace helios::pcg {

namespace {
Q32 q32(f64 v) { return Q32::fromDouble(v); }
Q16 q16(f64 v) { return Q16::fromDouble(v); }
FractalParams fractal(u32 seed, i32 e, f64 amp, f64 gain, u8 octaves) {
    return FractalParams{seed, e, q16(amp), q16(gain), octaves};
}
} // namespace

TerrainGraph makeReferenceGraph40() {
    TerrainGraph g;
    const PosId base = g.basePosition();
    // Domain warps: continent-scale and hill-scale.
    const PosId warpA = g.warp(base, {0xA11CE001u, 15, q16(2000.0)});
    const PosId warpB = g.warp(warpA, {0xA11CE002u, 11, q16(250.0)});
    // Continents and the land mask.
    const NodeId continents = g.fbm(warpA, fractal(0xC0A71E01u, 20, 3000.0, 0.5, 8));
    const NodeId land = g.remap(continents, q32(-500.0), q32(800.0), q32(0.0), q32(1.0), true);
    const NodeId ocean = g.remap(continents, q32(-2000.0), q32(0.0), q32(-1.0), q32(0.0), true);
    // Ridged mountains behind a regional mask.
    const NodeId mountains = g.ridged(warpB, fractal(0x3D07A1E5u, 14, 1500.0, 0.5, 12));
    const NodeId mountainRegion = g.fbm(base, fractal(0x3E610A11u, 17, 1.0, 0.5, 7));
    const NodeId mountainMask = g.remap(mountainRegion, q32(-0.2), q32(0.6), q32(0.0), q32(1.0), true);
    const NodeId mountainsMasked = g.mul(mountains, mountainMask);
    const NodeId mountainsOnLand = g.mul(mountainsMasked, land);
    // Hills and plains.
    const NodeId hills = g.fbm(warpB, fractal(0x4111500Du, 12, 200.0, 0.5, 8));
    const NodeId hillsOnLand = g.mul(hills, land);
    const NodeId plains = g.fbm(warpA, fractal(0x9A1A1115u, 13, 60.0, 0.55, 9));
    const NodeId lowland = g.sub(plains, g.constant(q32(10.0)));
    // Fine detail everywhere (0.25 m at the finest octave).
    const NodeId detail = g.fbm(base, fractal(0xDE7A1100u, 7, 8.0, 0.5, 10));
    // Crater-like field: inverted ridges where a coarse noise exceeds a threshold.
    const NodeId zero = g.constant(q32(0.0));
    const NodeId craterRidges = g.ridged(base, fractal(0xC4A7E401u, 10, 40.0, 0.45, 8));
    const NodeId craterBowls = g.sub(zero, craterRidges);
    const NodeId craterRegion = g.noise(base, 0xC4A7E402u, 15, q16(1.0));
    const NodeId craters = g.select(craterRegion, q32(0.3), craterBowls, zero);
    const NodeId cratersOnLand = g.mul(craters, land);
    // Terraced plateaus: a clamped fbm, flattened where it saturates.
    const NodeId terraceBase = g.fbm(base, fractal(0x7E44ACE5u, 11, 120.0, 0.5, 8));
    const NodeId terraces = g.clamp(terraceBase, q32(-40.0), q32(80.0));
    const NodeId terraceMask = g.remap(mountainRegion, q32(-0.6), q32(-0.1), q32(1.0), q32(0.0), true);
    const NodeId terracesMasked = g.mul(terraces, terraceMask);
    // Erosion-like channels.
    const NodeId erosion = g.ridged(warpB, fractal(0xE4051011u, 9, 15.0, 0.5, 9));
    const NodeId erosionCut = g.sub(zero, g.mul(erosion, land));
    // Sum the layers.
    NodeId h = g.add(continents, mountainsOnLand);
    h = g.add(h, hillsOnLand);
    h = g.add(h, lowland);
    h = g.add(h, detail);
    h = g.add(h, cratersOnLand);
    h = g.add(h, terracesMasked);
    h = g.add(h, erosionCut);
    // Ocean floor: deepen below sea level, then flatten the abyss and cap the peaks.
    h = g.add(h, g.mul(ocean, g.constant(q32(800.0))));
    h = g.clamp(h, q32(-6000.0), q32(9000.0));
    g.setOutput(h);
    return g;
}

TerrainGraph makeSingleNodeGraph(NodeKind kind) {
    TerrainGraph g;
    const PosId base = g.basePosition();
    const FractalParams f = fractal(0x51A61Eu, 12, 250.0, 0.5, 6);
    const NodeId a = g.fbm(base, fractal(0xA0A0A0A0u, 11, 300.0, 0.5, 3));
    const NodeId b = g.fbm(base, fractal(0xB0B0B0B0u, 10, 200.0, 0.5, 3));
    NodeId out;
    switch (kind) {
        case NodeKind::Constant: out = g.constant(q32(-1234.5678)); break;
        case NodeKind::Noise: out = g.noise(base, 0x0015E001u, 9, q16(750.0)); break;
        case NodeKind::Fbm: out = g.fbm(base, f); break;
        case NodeKind::Ridged: out = g.ridged(base, f); break;
        case NodeKind::Warp: out = g.fbm(g.warp(base, {0x3A4B5C6Du, 10, q16(900.0)}), f); break;
        case NodeKind::Add: out = g.add(a, b); break;
        case NodeKind::Sub: out = g.sub(a, b); break;
        case NodeKind::Mul: out = g.mul(a, b); break;
        case NodeKind::Min: out = g.min(a, b); break;
        case NodeKind::Max: out = g.max(a, b); break;
        case NodeKind::Clamp: out = g.clamp(a, q32(-50.5), q32(75.25)); break;
        case NodeKind::Remap: out = g.remap(a, q32(-300.0), q32(300.0), q32(10.0), q32(-40.0), true); break;
        case NodeKind::Select: out = g.select(a, q32(12.5), b, g.constant(q32(3.0))); break;
        default: HELIOS_ASSERT(false, "makeSingleNodeGraph: invalid node kind"); break;
    }
    g.setOutput(out);
    return g;
}

} // namespace helios::pcg
