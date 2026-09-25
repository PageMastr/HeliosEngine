// hnoise.slang — the GPU twin of fixed-point hnoise and the terrain VM (02 §5.8, 03 §5.5 and §5.5a).
// `import pcg.hnoise;`
//
// Normative reference: engine/pcg/include/helios/pcg/hnoise.h (scalar) and the op bodies in
// engine/pcg/src/kernels/vm_ops.inl. Every function here must produce the same bits; the shared
// corpus (tests/corpus/hnoise) and pcg_gpu_tests prove it on lavapipe and vendor GPUs.
//
// 32-bit integer operations only (03 §5.5a; enforced by the pcg_lint_twin CTest): Q32.32 values and
// positions are uint2 {x = low word, y = high word} with explicit carries, and 32 x 32 -> 64 products
// use umulExtended / imulExtended (SPIR-V OpUMulExtended / OpSMulExtended), which need no
// shaderInt64. No floating point anywhere.
//
// Terrain programs are interpreted per sample: every invocation of a dispatch runs the same
// instruction stream, so control flow stays uniform. Instruction layout: helios/pcg/program.h.
module hnoise;

import glsl; // umulExtended / imulExtended

public static const uint kTileEdge = 65;
public static const uint kTileSamples = 65 * 65;
public static const uint kInstrWords = 16;
public static const uint kDomainWords = 16;

static const uint kP2 = 0x85EBCA77u;
static const uint kP3 = 0xC2B2AE3Du;
static const uint kP4 = 0x27D4EB2Fu;
static const uint kP5 = 0x165667B1u;
static const uint kOctaveSeedStep = 0x9E3779B9u;
static const uint kWarpAxisSeedStep = 0x632BE5ABu;
static const int kOneQ16 = 65536;
static const int kOneQ30 = 1 << 30;
static const int kWarpC3 = 173443014;
static const int kWarpC5 = 42458960;
static const int kWarpC7 = 11716253;
static const int kWarpC9 = 1124922;
static const int kWarpC11 = 1685457;
static const int kWarpC1 = (1 << 30) - (173443014 + 42458960 + 11716253 + 1124922 + 1685457);
static const uint kRsqrtSlope = 226908346u;

// ---------------------------------------------------------------------------------------------
// 64-bit two's complement on uint2 {lo, hi}
// ---------------------------------------------------------------------------------------------

public uint2 add64(uint2 a, uint2 b) {
    const uint lo = a.x + b.x;
    const uint carry = lo < a.x ? 1u : 0u;
    return uint2(lo, a.y + b.y + carry);
}

public uint2 sub64(uint2 a, uint2 b) {
    const uint borrow = a.x < b.x ? 1u : 0u;
    return uint2(a.x - b.x, a.y - b.y - borrow);
}

/// a < b, signed.
public bool lt64(uint2 a, uint2 b) {
    const int ah = int(a.y);
    const int bh = int(b.y);
    return ah < bh || (ah == bh && a.x < b.x);
}

public uint2 min64(uint2 a, uint2 b) { return lt64(b, a) ? b : a; }
public uint2 max64(uint2 a, uint2 b) { return lt64(a, b) ? b : a; }

/// Signed 32 x 32 -> 64.
public uint2 mulWide(int a, int b) {
    int hi;
    int lo;
    imulExtended(a, b, hi, lo);
    return uint2(uint(lo), uint(hi));
}

/// Unsigned 32 x 32 -> 64.
public uint2 mulWideU(uint a, uint b) {
    uint hi;
    uint lo;
    umulExtended(a, b, hi, lo);
    return uint2(lo, hi);
}

/// Low 32 bits of (p + 2^(s-1)) >> s for 0 < s < 32.
uint roundShift(uint2 p, uint s) {
    p = add64(p, uint2(1u << (s - 1u), 0u));
    return (p.x >> s) | (p.y << (32u - s));
}

public int mulQ16(int a, int b) { return int(roundShift(mulWide(a, b), 16u)); }
public int mulQ30(int a, int b) { return int(roundShift(mulWide(a, b), 30u)); }
public uint mulQ30u(uint a, uint b) { return roundShift(mulWideU(a, b), 30u); }
public uint mulQ31u(uint a, uint b) { return roundShift(mulWideU(a, b), 31u); }

/// Q32.32 product with helios::Q32 rounding: bits 32..95 of a*b + 2^31, from 32-bit partial products.
public uint2 mulQ32(uint2 a, uint2 b) {
    const uint2 ll = mulWideU(a.x, b.x);
    const uint2 lh = mulWideU(a.x, b.y);
    const uint2 hl = mulWideU(a.y, b.x);
    const uint hh = a.y * b.y; // only its low word reaches bits 32..95
    const uint2 t = add64(ll, uint2(0x80000000u, 0u));
    uint2 r = uint2(t.y, 0u);
    r = add64(r, lh);
    r = add64(r, hl);
    r.y += hh;
    // Signed correction: subtract b << 32 if a < 0 and a << 32 if b < 0.
    if (int(a.y) < 0) r.y -= b.x;
    if (int(b.y) < 0) r.y -= a.x;
    return r;
}

// ---------------------------------------------------------------------------------------------
// hnoise
// ---------------------------------------------------------------------------------------------

uint rotl17(uint h) { return (h << 17u) | (h >> 15u); }
uint xxRound(uint h, uint vTimesP3) { return rotl17(h + vTimesP3) * kP4; }
uint xxAvalanche(uint h) {
    h ^= h >> 15u;
    h *= kP2;
    h ^= h >> 13u;
    h *= kP3;
    h ^= h >> 16u;
    return h;
}

/// xxHash32 of the little-endian cell (x, y, z) with `seed`.
public uint latticeHash(uint seed, int x, int y, int z) {
    uint h = seed + kP5 + 12u;
    h = xxRound(h, uint(x) * kP3);
    h = xxRound(h, uint(y) * kP3);
    h = xxRound(h, uint(z) * kP3);
    return xxAvalanche(h);
}

/// Perlin's improved-noise gradient (top four hash bits) dotted with (x, y, z).
public int gradDot(uint h, int x, int y, int z) {
    const uint g = h >> 28u;
    const int u = g < 8u ? x : y;
    const int v = g < 4u ? y : ((g == 12u || g == 14u) ? x : z);
    return ((g & 1u) != 0u ? -u : u) + ((g & 2u) != 0u ? -v : v);
}

/// hnoise::fade with a single rounding: t3 = (t^2 * t) >> 24, b = 10 * 2^32 + t (6t - 15 * 2^16),
/// (t3 * b + 2^39) >> 40. The product t3 * b is < 2^60, so its high word alone gives the result.
public int fade(int t) {
    const uint tu = uint(t);
    const uint t2 = tu * tu;
    uint hi;
    uint lo;
    umulExtended(t2, tu, hi, lo);
    const uint t3 = (hi << 8u) | (lo >> 24u);
    int bhi;
    int blo;
    imulExtended(t, 6 * t - 15 * kOneQ16, bhi, blo);
    bhi += 10; // + 10 * 2^32
    uint phi;
    uint plo;
    umulExtended(t3, uint(blo), phi, plo);
    phi += t3 * uint(bhi);          // (t3 * bhi) << 32
    return int((phi + 128u) >> 8u); // + 2^39, >> 40
}

int lerpQ16(int a, int b, int w) { return a + mulQ16(w, b - a); }

/// hnoise::noise3; hinit = seed + P5 + 12. Hash rounds are shared between corners exactly like the
/// CPU kernels (the result is the per-corner xxHash32 either way).
public int noise3(int3 cell, int3 frac, uint hinit) {
    const uint tx0 = uint(cell.x) * kP3;
    const uint tx1 = tx0 + kP3;
    const uint hx0 = rotl17(hinit + tx0) * kP4;
    const uint hx1 = rotl17(hinit + tx1) * kP4;
    const uint ty0 = uint(cell.y) * kP3;
    const uint ty1 = ty0 + kP3;
    const uint h00 = xxRound(hx0, ty0);
    const uint h10 = xxRound(hx1, ty0);
    const uint h01 = xxRound(hx0, ty1);
    const uint h11 = xxRound(hx1, ty1);
    const uint tz0 = uint(cell.z) * kP3;
    const uint tz1 = tz0 + kP3;
    const int fx = frac.x;
    const int fy = frac.y;
    const int fz = frac.z;
    const int gx = fx - kOneQ16;
    const int gy = fy - kOneQ16;
    const int gz = fz - kOneQ16;
    const int n000 = gradDot(xxAvalanche(xxRound(h00, tz0)), fx, fy, fz);
    const int n100 = gradDot(xxAvalanche(xxRound(h10, tz0)), gx, fy, fz);
    const int n010 = gradDot(xxAvalanche(xxRound(h01, tz0)), fx, gy, fz);
    const int n110 = gradDot(xxAvalanche(xxRound(h11, tz0)), gx, gy, fz);
    const int n001 = gradDot(xxAvalanche(xxRound(h00, tz1)), fx, fy, gz);
    const int n101 = gradDot(xxAvalanche(xxRound(h10, tz1)), gx, fy, gz);
    const int n011 = gradDot(xxAvalanche(xxRound(h01, tz1)), fx, gy, gz);
    const int n111 = gradDot(xxAvalanche(xxRound(h11, tz1)), gx, gy, gz);
    const int wx = fade(fx);
    const int wy = fade(fy);
    const int wz = fade(fz);
    const int x00 = lerpQ16(n000, n100, wx);
    const int x10 = lerpQ16(n010, n110, wx);
    const int x01 = lerpQ16(n001, n101, wx);
    const int x11 = lerpQ16(n011, n111, wx);
    const int y0 = lerpQ16(x00, x10, wy);
    const int y1 = lerpQ16(x01, x11, wy);
    return lerpQ16(y0, y1, wz);
}

/// Splits a Q32.32 coordinate {lo, hi} at wavelength 2^e metres (hnoise::splitCoord).
public void splitCoord(uint2 p, int e, out int cell, out int frac) {
    const int hi = int(p.y);
    if (e >= 0) {
        cell = hi >> uint(e);
    } else {
        cell = int((p.y << uint(-e)) | (p.x >> uint(32 + e)));
    }
    const int s = 16 + e;
    if (s >= 32) {
        frac = (hi >> uint(s - 32)) & 0xFFFF;
    } else if (s == 0) {
        frac = int(p.x & 0xFFFFu);
    } else {
        frac = int(((p.x >> uint(s)) | (p.y << uint(32 - s))) & 0xFFFFu);
    }
}

// ---------------------------------------------------------------------------------------------
// Tile domain (helios/pcg/tile.h TileDomain::pack)
// ---------------------------------------------------------------------------------------------

int equiAngularWarp(int s) {
    const int s2 = mulQ30(s, s);
    int acc = kWarpC11;
    acc = kWarpC9 + mulQ30(acc, s2);
    acc = kWarpC7 + mulQ30(acc, s2);
    acc = kWarpC5 + mulQ30(acc, s2);
    acc = kWarpC3 + mulQ30(acc, s2);
    acc = kWarpC1 + mulQ30(acc, s2);
    return mulQ30(s, acc);
}

uint rsqrtNewton(uint s) {
    uint y = uint(kOneQ30) - mulQ30u(s - uint(kOneQ30), kRsqrtSlope);
    for (uint it = 0; it < 4u; ++it) {
        const uint y2 = mulQ30u(y, y);
        const uint sy2 = mulQ30u(s, y2);
        y = mulQ31u(y, (3u << 30u) - sy2);
    }
    return y;
}

// Per face (math's cubeFaceBasis): axis of the normal, of u and of v, and their signs.
static const uint kFaceNAxis[6] = { 0, 0, 1, 1, 2, 2 };
static const uint kFaceUAxis[6] = { 2, 2, 0, 0, 0, 0 };
static const uint kFaceVAxis[6] = { 1, 1, 2, 2, 1, 1 };
static const int kFaceNSign[6] = { 1, -1, 1, -1, 1, -1 };
static const int kFaceUSign[6] = { -1, 1, 1, 1, 1, -1 };
static const int kFaceVSign[6] = { 1, 1, -1, 1, 1, 1 };

/// Base position (Q32.32 metres per axis) of `sample` (0 .. 4224) of a packed tile domain.
public void domainPosition(uint domain[16], uint sample, out uint2 pos[3]) {
    const uint i = sample % kTileEdge;
    const uint j = sample / kTileEdge;
    const uint tileX = domain[3];
    const uint tileY = domain[4];
    if (domain[0] == 1u) {
        const uint face = domain[1] < 6u ? domain[1] : 0u;
        const uint level = domain[2] <= 25u ? domain[2] : 25u;
        const uint shift = 25u - level;
        const int radius = int(domain[5] & 0x7FFFFFFFu);
        const int s = int(((tileX * 64u + i) << shift) - uint(kOneQ30));
        const int t = int(((tileY * 64u + j) << shift) - uint(kOneQ30));
        const int sw = equiAngularWarp(s);
        const int tw = equiAngularWarp(t);
        const uint len2 = uint(mulQ30(sw, sw)) + uint(mulQ30(tw, tw)) + uint(kOneQ30);
        const int inv = int(rsqrtNewton(len2));
        int comp[3];
        comp[kFaceNAxis[face]] = kFaceNSign[face] * kOneQ30;
        comp[kFaceUAxis[face]] = kFaceUSign[face] < 0 ? -sw : sw;
        comp[kFaceVAxis[face]] = kFaceVSign[face] < 0 ? -tw : tw;
        for (uint a = 0; a < 3u; ++a) {
            const int unit = mulQ30(comp[a], inv);
            const uint2 p = mulWide(unit, radius);
            // (hi:lo) >> 6, arithmetic.
            pos[a] = uint2((p.x >> 6u) | (p.y << 26u), uint(int(p.y) >> 6u));
        }
    } else {
        const uint2 spacing = uint2(domain[12], domain[13]);
        const uint m[2] = { tileX * 64u + i, tileY * 64u + j };
        for (uint a = 0; a < 2u; ++a) {
            uint2 p = mulWideU(m[a], spacing.x);
            p.y += m[a] * spacing.y;
            pos[a] = add64(p, uint2(domain[6 + 2 * a], domain[7 + 2 * a]));
        }
        pos[2] = uint2(domain[10], domain[11]);
    }
}

// ---------------------------------------------------------------------------------------------
// Terrain VM (per sample)
// ---------------------------------------------------------------------------------------------

uint2 widen(int v) { return uint2(uint(v), v < 0 ? 0xFFFFFFFFu : 0u); }

uint2 fractal(uint* prog, uint base, uint2 px, uint2 py, uint2 pz, bool ridged) {
    const uint w1 = prog[base + 1u];
    const uint octaves = min((w1 >> 8u) & 0xFFu, 24u);
    const int e0 = int(w1 << 8u) >> 24u;
    const uint seed = prog[base + 2u];
    int amp = int(prog[base + 3u]);
    const int gain = int(prog[base + 4u]);
    uint2 acc = uint2(0u, 0u);
    for (uint k = 0; k < octaves; ++k) {
        const int e = e0 - int(k);
        int3 cell;
        int3 frac;
        splitCoord(px, e, cell.x, frac.x);
        splitCoord(py, e, cell.y, frac.y);
        splitCoord(pz, e, cell.z, frac.z);
        int n = noise3(cell, frac, seed + k * kOctaveSeedStep + kP5 + 12u);
        if (ridged) {
            const int t = max(kOneQ16 - abs(n), 0);
            n = mulQ16(t, t);
        }
        acc = add64(acc, mulWide(amp, n));
        amp = mulQ16(amp, gain);
    }
    return acc;
}

uint2 constantK(uint* prog, uint base, uint i) { return uint2(prog[base + 8u + 2u * i], prog[base + 9u + 2u * i]); }

/// Evaluates `instrCount` instructions for one sample; returns height register `outputReg`
/// (Q32.32) and the sample's base position. Register indices are masked to the register file
/// (the CPU validates programs before upload).
public uint2 evaluateSample(uint* prog, uint instrCount, uint outputReg, uint domain[16], uint sample, out uint2 basePos[3]) {
    uint2 h[16];
    for (uint r = 0; r < 16u; ++r) h[r] = uint2(0u, 0u);
    uint2 px[4];
    uint2 py[4];
    uint2 pz[4];
    domainPosition(domain, sample, basePos);
    for (uint r = 0; r < 4u; ++r) {
        px[r] = basePos[0];
        py[r] = basePos[1];
        pz[r] = basePos[2];
    }
    for (uint n = 0; n < instrCount; ++n) {
        const uint base = n * kInstrWords;
        const uint w0 = prog[base];
        const uint op = w0 & 0xFFu;
        const uint dst = (w0 >> 8u) & 0xFFu;
        const uint a = (w0 >> 16u) & 0xFFu;
        const uint b = (w0 >> 24u) & 0xFFu;
        const uint c = prog[base + 1u] & 0xFFu;
        switch (op) {
        case 0u: // Const
            h[dst & 15u] = constantK(prog, base, 0u);
            break;
        case 1u: // Noise
        case 2u: // Fbm
            h[dst & 15u] = fractal(prog, base, px[a & 3u], py[a & 3u], pz[a & 3u], false);
            break;
        case 3u: // Ridged
            h[dst & 15u] = fractal(prog, base, px[a & 3u], py[a & 3u], pz[a & 3u], true);
            break;
        case 4u: { // Warp
            const uint w1 = prog[base + 1u];
            const int e = int(w1 << 8u) >> 24u;
            const uint seed = prog[base + 2u];
            const int amp = int(prog[base + 3u]);
            const uint2 src[3] = { px[a & 3u], py[a & 3u], pz[a & 3u] };
            int3 cell;
            int3 frac;
            splitCoord(src[0], e, cell.x, frac.x);
            splitCoord(src[1], e, cell.y, frac.y);
            splitCoord(src[2], e, cell.z, frac.z);
            uint2 outp[3];
            for (uint axis = 0; axis < 3u; ++axis) {
                const int nv = noise3(cell, frac, seed + axis * kWarpAxisSeedStep + kP5 + 12u);
                outp[axis] = add64(src[axis], mulWide(amp, nv));
            }
            px[dst & 3u] = outp[0];
            py[dst & 3u] = outp[1];
            pz[dst & 3u] = outp[2];
            break;
        }
        case 5u: h[dst & 15u] = add64(h[a & 15u], h[b & 15u]); break;
        case 6u: h[dst & 15u] = sub64(h[a & 15u], h[b & 15u]); break;
        case 7u: h[dst & 15u] = mulQ32(h[a & 15u], h[b & 15u]); break;
        case 8u: h[dst & 15u] = min64(h[a & 15u], h[b & 15u]); break;
        case 9u: h[dst & 15u] = max64(h[a & 15u], h[b & 15u]); break;
        case 10u: // Clamp
            h[dst & 15u] = min64(max64(h[a & 15u], constantK(prog, base, 0u)), constantK(prog, base, 1u));
            break;
        case 11u: { // Remap
            const uint2 k2 = constantK(prog, base, 2u);
            const uint2 k3 = constantK(prog, base, 3u);
            uint2 v = add64(k2, mulQ32(sub64(h[a & 15u], constantK(prog, base, 0u)), constantK(prog, base, 1u)));
            if (((prog[base + 1u] >> 24u) & 1u) != 0u) v = min64(max64(v, min64(k2, k3)), max64(k2, k3));
            h[dst & 15u] = v;
            break;
        }
        case 12u: // Select
            h[dst & 15u] = lt64(h[a & 15u], constantK(prog, base, 0u)) ? h[c & 15u] : h[b & 15u];
            break;
        default: break;
        }
    }
    return h[outputReg & 15u];
}
