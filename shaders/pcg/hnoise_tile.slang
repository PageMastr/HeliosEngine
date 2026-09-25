// Compute entry points of the hnoise twin (conformance tests and the WP-0.9c throughput spike).
// The renderer's tile producer (03 §5.4) will call evaluateSample() the same way.
import pcg.hnoise;

struct TilePush {
    uint* program;     // instrCount * 16 words (helios/pcg/program.h)
    uint* domains;     // tileCount * 16 words (TileDomain::pack)
    uint2* heights;    // tileCount * 4225 Q32.32 heights {lo, hi}
    uint2* positions;  // tileCount * 4225 * 3 base positions, written when flags & 1
    uint instrCount;
    uint outputReg;
    uint tileCount;
    uint flags;        // bit 0: write positions (a flag, not a null test: comparing a pointer with
                       // null converts it to a 64-bit integer, which would need shaderInt64)
};
[[vk::push_constant]] ConstantBuffer<TilePush> gTile;

/// One invocation per sample: x = sample (0 .. 4224), y = tile.
[shader("compute")]
[numthreads(64, 1, 1)]
void csTile(uint3 id : SV_DispatchThreadID) {
    const uint sample = id.x;
    const uint tile = id.y;
    if (sample >= kTileSamples || tile >= gTile.tileCount) return;
    uint domain[16];
    for (uint w = 0; w < kDomainWords; ++w) domain[w] = gTile.domains[tile * kDomainWords + w];
    uint2 basePos[3];
    const uint2 h = evaluateSample(gTile.program, gTile.instrCount, gTile.outputReg, domain, sample, basePos);
    const uint index = tile * kTileSamples + sample;
    gTile.heights[index] = h;
    if ((gTile.flags & 1u) != 0u) {
        gTile.positions[index * 3u + 0u] = basePos[0];
        gTile.positions[index * 3u + 1u] = basePos[1];
        gTile.positions[index * 3u + 2u] = basePos[2];
    }
}

struct LatticePush {
    uint* cases;  // count * 7 words: seed, cell x y z, frac x y z
    uint* results; // count * 2 words: latticeHash, noise3 (Q16)
    uint count;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<LatticePush> gLattice;

[shader("compute")]
[numthreads(64, 1, 1)]
void csLattice(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= gLattice.count) return;
    const uint base = i * 7u;
    const uint seed = gLattice.cases[base];
    const int3 cell = int3(int(gLattice.cases[base + 1u]), int(gLattice.cases[base + 2u]), int(gLattice.cases[base + 3u]));
    const int3 frac = int3(int(gLattice.cases[base + 4u]), int(gLattice.cases[base + 5u]), int(gLattice.cases[base + 6u]));
    gLattice.results[i * 2u] = latticeHash(seed, cell.x, cell.y, cell.z);
    gLattice.results[i * 2u + 1u] = uint(noise3(cell, frac, seed + 0x165667B1u + 12u));
}
