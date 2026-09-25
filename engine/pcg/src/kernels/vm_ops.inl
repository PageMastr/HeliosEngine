// vm_ops.inl — the terrain VM's op bodies, written once against a lane type (02 §5.8 "SIMD kernels").
//
// Included by vm_scalar.cpp (1 lane), vm_sse42.cpp (4 lanes) and vm_avx2.cpp (8 lanes) inside an
// anonymous namespace, after `using L = <lane type>;`. Every function here is internal to its TU.
// The semantics are those of the scalar reference in helios/pcg/hnoise.h and
// helios/pcg/program.h; the per-op rounding and wrapping rules are spelled out there. Integer ops
// only (the pcg_lint_kernels CTest rejects floating-point types in this directory).
//
// Lane type interface (all static):
//   V                     W x i32
//   set1 load loadU store storeU add sub mullo band bor bxor
//   slli<N> srli<N> srai<N> (constant counts), sll srl sra (uniform run-time counts, 0..31)
//   cmpgt cmpeq (masks: all ones / zero), select(mask, ifTrue, ifFalse), abs, max
//   uniform(v, out)       true if every lane equals lane 0 (stored in out)
//   mulQ16(a, b)          (a*b + 2^15) >> 16 with a 64-bit product
//   fade(t)               hnoise::fade, bit-exact
//   mulQ30(a, b)          (a*b + 2^29) >> 30, signed
//   mulQ30u / mulQ31u     unsigned (a*b + 2^29) >> 30 and (a*b + 2^30) >> 31
//   mulWide(a, b, lo, hi) signed 32x32 -> 64 product as (lo, hi) planes; mulWideU unsigned
//   Acc, accZero, accMulAdd(acc, a, b) (acc += a*b in 64 bits), accStore(i64* out, acc)
//   Q (kQWidth x i64): loadQ storeQ set1Q addQ subQ minQ maxQ selectGeQ mulQ32

using V = L::V;
using Q = L::Q;
constexpr u32 W = L::kWidth;
constexpr u32 QW = L::kQWidth;

constexpr u32 kP2 = 0x85EBCA77u;
constexpr u32 kP3 = 0xC2B2AE3Du;
constexpr u32 kP4 = 0x27D4EB2Fu;
constexpr u32 kP5 = 0x165667B1u;
constexpr u32 kOctaveSeedStep = 0x9E3779B9u;
constexpr u32 kWarpAxisSeedStep = 0x632BE5ABu;
constexpr i32 kOneQ16 = 65536;
constexpr i32 kOneQ30 = 1 << 30;
constexpr i32 kWarpC3 = 173443014, kWarpC5 = 42458960, kWarpC7 = 11716253, kWarpC9 = 1124922, kWarpC11 = 1685457;
constexpr i32 kWarpC1 = kOneQ30 - (kWarpC3 + kWarpC5 + kWarpC7 + kWarpC9 + kWarpC11);
constexpr u32 kRsqrtSlope = 226908346u;
constexpr u32 kMaxOct = 24;

// Instruction word accessors (layout: helios/pcg/program.h).
HELIOS_FORCEINLINE u32 opDst(const u32* w) { return (w[0] >> 8) & 0xFFu; }
HELIOS_FORCEINLINE u32 opA(const u32* w) { return (w[0] >> 16) & 0xFFu; }
HELIOS_FORCEINLINE u32 opB(const u32* w) { return (w[0] >> 24) & 0xFFu; }
HELIOS_FORCEINLINE u32 opC(const u32* w) { return w[1] & 0xFFu; }
HELIOS_FORCEINLINE u32 opOctaves(const u32* w) { return (w[1] >> 8) & 0xFFu; }
HELIOS_FORCEINLINE i32 opExp(const u32* w) { return static_cast<i32>(static_cast<i8>(static_cast<u8>(w[1] >> 16))); }
HELIOS_FORCEINLINE u32 opFlags(const u32* w) { return (w[1] >> 24) & 0xFFu; }
HELIOS_FORCEINLINE i64 opK(const u32* w, u32 i) {
    return static_cast<i64>((static_cast<u64>(w[9 + 2 * i]) << 32) | w[8 + 2 * i]);
}

HELIOS_FORCEINLINE i32 scalarMulQ16(i32 a, i32 b) {
    return static_cast<i32>((static_cast<i64>(a) * b + (i64(1) << 15)) >> 16);
}

// --- hnoise ------------------------------------------------------------------------------------

HELIOS_FORCEINLINE V rotl17(V h) { return L::bor(L::slli<17>(h), L::srli<15>(h)); }

/// One xxHash32 round: rotl(h + v*P3, 17) * P4, with v*P3 precomputed.
HELIOS_FORCEINLINE V xxRound(V h, V vTimesP3) {
    return L::mullo(rotl17(L::add(h, vTimesP3)), L::set1(static_cast<i32>(kP4)));
}

HELIOS_FORCEINLINE V xxAvalanche(V h) {
    h = L::bxor(h, L::srli<15>(h));
    h = L::mullo(h, L::set1(static_cast<i32>(kP2)));
    h = L::bxor(h, L::srli<13>(h));
    h = L::mullo(h, L::set1(static_cast<i32>(kP3)));
    return L::bxor(h, L::srli<16>(h));
}

HELIOS_FORCEINLINE V gradDot(V h, V x, V y, V z) {
    const V g = L::srli<28>(h);
    const V u = L::select(L::cmpgt(L::set1(8), g), x, y);
    const V isEdge = L::cmpeq(L::band(g, L::set1(13)), L::set1(12)); // g == 12 || g == 14
    const V v = L::select(L::cmpgt(L::set1(4), g), y, L::select(isEdge, x, z));
    const V su = L::srai<31>(L::slli<31>(g)); // -(g & 1)
    const V sv = L::srai<31>(L::slli<30>(g)); // -((g >> 1) & 1)
    return L::add(L::sub(L::bxor(u, su), su), L::sub(L::bxor(v, sv), sv));
}

/// hnoise::fade (single rounding; the lane types implement it with their 64-bit product layouts).
HELIOS_FORCEINLINE V fade(V t) { return L::fade(t); }

HELIOS_FORCEINLINE V lerpQ16(V a, V b, V w) { return L::add(a, L::mulQ16(w, L::sub(b, a))); }

/// hnoise::noise3 for W lanes whose cells may differ. hinit = seed + P5 + 12 (the xxHash32 start
/// value). The x rounds are shared between corners (the per-corner result is still exactly XXH32),
/// and each (y, z) edge is interpolated along x as soon as its two corners exist, which keeps fewer
/// vectors live.
HELIOS_FORCEINLINE V noise3(const V cell[3], const V frac[3], u32 hinit) {
    const V p3 = L::set1(static_cast<i32>(kP3));
    const V p4 = L::set1(static_cast<i32>(kP4));
    const V h0 = L::set1(static_cast<i32>(hinit));
    const V tx0 = L::mullo(cell[0], p3);
    const V hx0 = L::mullo(rotl17(L::add(h0, tx0)), p4);
    const V hx1 = L::mullo(rotl17(L::add(h0, L::add(tx0, p3))), p4); // (x + 1) * P3
    const V ty0 = L::mullo(cell[1], p3);
    const V tz0 = L::mullo(cell[2], p3);
    const V one = L::set1(kOneQ16);
    const V fx = frac[0], gx = L::sub(fx, one);
    const V wx = fade(fx);
    V edge[4]; // x-interpolated edges, index = (dz << 1) | dy
    for (u32 e = 0; e < 4; ++e) {
        const u32 dy = e & 1u, dz = e >> 1;
        const V ty = dy ? L::add(ty0, p3) : ty0;
        const V tz = dz ? L::add(tz0, p3) : tz0;
        const V y = dy ? L::sub(frac[1], one) : frac[1];
        const V z = dz ? L::sub(frac[2], one) : frac[2];
        const V n0 = gradDot(xxAvalanche(xxRound(xxRound(hx0, ty), tz)), fx, y, z);
        const V n1 = gradDot(xxAvalanche(xxRound(xxRound(hx1, ty), tz)), gx, y, z);
        edge[e] = lerpQ16(n0, n1, wx);
    }
    const V wy = fade(frac[1]), wz = fade(frac[2]);
    return lerpQ16(lerpQ16(edge[0], edge[1], wy), lerpQ16(edge[2], edge[3], wy), wz);
}

HELIOS_FORCEINLINE u32 scalarRound(u32 h, u32 v) {
    h += v * kP3;
    h = (h << 17) | (h >> 15);
    return h * kP4;
}

/// The eight corner hashes of one lattice cell, computed once with scalar code.
struct CellHashes {
    i32 cell[3] = {0, 0, 0};
    bool valid = false;
    u32 h[8] = {}; ///< corner (dx | dy << 1 | dz << 2)
};

HELIOS_FORCEINLINE void hashCell(CellHashes& c, const i32 cell[3], u32 hinit) {
    if (c.valid && c.cell[0] == cell[0] && c.cell[1] == cell[1] && c.cell[2] == cell[2]) return;
    for (u32 k = 0; k < 8; ++k) {
        u32 h = hinit;
        h = scalarRound(h, static_cast<u32>(cell[0]) + (k & 1u));
        h = scalarRound(h, static_cast<u32>(cell[1]) + ((k >> 1) & 1u));
        h = scalarRound(h, static_cast<u32>(cell[2]) + ((k >> 2) & 1u));
        h ^= h >> 15;
        h *= kP2;
        h ^= h >> 13;
        h *= kP3;
        h ^= h >> 16;
        c.h[k] = h;
    }
    c.cell[0] = cell[0];
    c.cell[1] = cell[1];
    c.cell[2] = cell[2];
    c.valid = true;
}

/// hnoise::noise3 for W lanes that share one lattice cell: the corner hashes and gradient choices
/// are uniform, so each gradient dot is one add of two (possibly negated) offsets. Bit-identical to
/// noise3() (same hashes, same gradient table, same fade and lerps).
HELIOS_FORCEINLINE V noise3Coherent(const CellHashes& c, const V frac[3]) {
    const V one = L::set1(kOneQ16);
    const V zero = L::set1(0);
    V comp[3][2][2]; // [axis][corner bit: offset f or f - 1][negated]
    for (u32 a = 0; a < 3; ++a) {
        comp[a][0][0] = frac[a];
        comp[a][1][0] = L::sub(frac[a], one);
        comp[a][0][1] = L::sub(zero, comp[a][0][0]);
        comp[a][1][1] = L::sub(zero, comp[a][1][0]);
    }
    const auto dot = [&](u32 k) {
        const u32 g = c.h[k] >> 28;
        const u32 bit[3] = {k & 1u, (k >> 1) & 1u, (k >> 2) & 1u};
        const u32 ua = g < 8 ? 0u : 1u;
        const u32 va = g < 4 ? 1u : ((g == 12 || g == 14) ? 0u : 2u);
        return L::add(comp[ua][bit[ua]][g & 1u], comp[va][bit[va]][(g >> 1) & 1u]);
    };
    const V wx = fade(frac[0]);
    const V e0 = lerpQ16(dot(0), dot(1), wx), e1 = lerpQ16(dot(2), dot(3), wx);
    const V e2 = lerpQ16(dot(4), dot(5), wx), e3 = lerpQ16(dot(6), dot(7), wx);
    const V wy = fade(frac[1]), wz = fade(frac[2]);
    return lerpQ16(lerpQ16(e0, e1, wy), lerpQ16(e2, e3, wy), wz);
}

/// noise3 through the coherent path when every lane is in the same cell (the common case for
/// wavelengths well above the lane footprint), else the general path. `cache` remembers the last
/// cell of this octave, since neighbouring blocks usually share it.
HELIOS_FORCEINLINE V noise3Auto(const V cell[3], const V frac[3], u32 hinit, CellHashes& cache) {
    i32 c[3];
    if (L::uniform(cell[0], c[0]) && L::uniform(cell[1], c[1]) && L::uniform(cell[2], c[2])) {
        hashCell(cache, c, hinit);
        return noise3Coherent(cache, frac);
    }
    return noise3(cell, frac, hinit);
}

/// Splits Q32.32 positions (hi, lo planes) at wavelength 2^e metres (hnoise::splitCoord).
HELIOS_FORCEINLINE void splitCoord(V hi, V lo, i32 e, V& cell, V& frac) {
    if (e >= 0) {
        cell = L::sra(hi, e);
    } else {
        cell = L::bor(L::sll(hi, -e), L::srl(lo, 32 + e));
    }
    const i32 s = 16 + e;
    const V mask = L::set1(0xFFFF);
    if (s >= 32) {
        frac = L::band(L::sra(hi, s - 32), mask);
    } else if (s == 0) {
        frac = L::band(lo, mask);
    } else {
        frac = L::band(L::bor(L::srl(lo, s), L::sll(hi, 32 - s)), mask);
    }
}

/// (hi, lo) += (bHi, bLo), 64-bit two's complement.
HELIOS_FORCEINLINE void add64(V& hi, V& lo, V bHi, V bLo) {
    const V sum = L::add(lo, bLo);
    const V bias = L::set1(static_cast<i32>(0x80000000u));
    const V carry = L::cmpgt(L::bxor(lo, bias), L::bxor(sum, bias)); // sum <u lo
    hi = L::sub(L::add(hi, bHi), carry);
    lo = sum;
}

// --- Generators ----------------------------------------------------------------------------------

template <bool kRidged>
void opFractal(const u32* w, vm::Registers& r) {
    const u32 dst = opDst(w), pa = opA(w);
    u32 octaves = opOctaves(w);
    if (octaves > kMaxOct) octaves = kMaxOct;
    const i32 e0 = opExp(w);
    const u32 seed = w[2];
    i32 amp = static_cast<i32>(w[3]);
    const i32 gain = static_cast<i32>(w[4]);
    i32 amps[kMaxOct];
    u32 hinit[kMaxOct];
    i32 exps[kMaxOct];
    for (u32 k = 0; k < octaves; ++k) {
        amps[k] = amp;
        hinit[k] = seed + k * kOctaveSeedStep + kP5 + 12u;
        exps[k] = e0 - static_cast<i32>(k);
        amp = scalarMulQ16(amp, gain);
    }
    CellHashes cache[kMaxOct];
    const i32* hiPlane[3] = {r.posHi[pa][0], r.posHi[pa][1], r.posHi[pa][2]};
    const u32* loPlane[3] = {r.posLo[pa][0], r.posLo[pa][1], r.posLo[pa][2]};
    i64* out = r.h[dst];
    for (u32 base = 0; base < vm::kStride; base += W) {
        V hi[3], lo[3];
        for (u32 a = 0; a < 3; ++a) {
            hi[a] = L::load(hiPlane[a] + base);
            lo[a] = L::loadU(loPlane[a] + base);
        }
        L::Acc acc = L::accZero();
        for (u32 k = 0; k < octaves; ++k) {
            V cell[3], frac[3];
            for (u32 a = 0; a < 3; ++a) splitCoord(hi[a], lo[a], exps[k], cell[a], frac[a]);
            V n = noise3Auto(cell, frac, hinit[k], cache[k]);
            if constexpr (kRidged) {
                const V t = L::max(L::sub(L::set1(kOneQ16), L::abs(n)), L::set1(0));
                n = L::mulQ16(t, t);
            }
            L::accMulAdd(acc, L::set1(amps[k]), n);
        }
        L::accStore(out + base, acc);
    }
}

void opFbm(const u32* w, vm::Registers& r) { opFractal<false>(w, r); }
void opRidged(const u32* w, vm::Registers& r) { opFractal<true>(w, r); }

void opWarp(const u32* w, vm::Registers& r) {
    const u32 dst = opDst(w), pa = opA(w);
    const i32 e = opExp(w);
    const u32 seed = w[2];
    const V amp = L::set1(static_cast<i32>(w[3]));
    u32 hinit[3];
    CellHashes cache[3];
    for (u32 a = 0; a < 3; ++a) hinit[a] = seed + a * kWarpAxisSeedStep + kP5 + 12u;
    for (u32 base = 0; base < vm::kStride; base += W) {
        V hi[3], lo[3], cell[3], frac[3];
        for (u32 a = 0; a < 3; ++a) {
            hi[a] = L::load(r.posHi[pa][a] + base);
            lo[a] = L::loadU(r.posLo[pa][a] + base);
            splitCoord(hi[a], lo[a], e, cell[a], frac[a]);
        }
        for (u32 a = 0; a < 3; ++a) {
            const V n = noise3Auto(cell, frac, hinit[a], cache[a]);
            V pLo, pHi;
            L::mulWide(amp, n, pLo, pHi);
            V outHi = hi[a], outLo = lo[a];
            add64(outHi, outLo, pHi, pLo);
            L::store(r.posHi[dst][a] + base, outHi);
            L::storeU(r.posLo[dst][a] + base, outLo);
        }
    }
}

// --- Height arithmetic ---------------------------------------------------------------------------

void opConst(const u32* w, vm::Registers& r) {
    const Q k = L::set1Q(opK(w, 0));
    i64* out = r.h[opDst(w)];
    for (u32 i = 0; i < vm::kStride; i += QW) L::storeQ(out + i, k);
}

template <class F>
HELIOS_FORCEINLINE void binaryOp(const u32* w, vm::Registers& r, F&& f) {
    const i64* a = r.h[opA(w)];
    const i64* b = r.h[opB(w)];
    i64* out = r.h[opDst(w)];
    for (u32 i = 0; i < vm::kStride; i += QW) L::storeQ(out + i, f(L::loadQ(a + i), L::loadQ(b + i)));
}

void opAdd(const u32* w, vm::Registers& r) { binaryOp(w, r, [](Q a, Q b) { return L::addQ(a, b); }); }
void opSub(const u32* w, vm::Registers& r) { binaryOp(w, r, [](Q a, Q b) { return L::subQ(a, b); }); }
void opMul(const u32* w, vm::Registers& r) { binaryOp(w, r, [](Q a, Q b) { return L::mulQ32(a, b); }); }
void opMin(const u32* w, vm::Registers& r) { binaryOp(w, r, [](Q a, Q b) { return L::minQ(a, b); }); }
void opMax(const u32* w, vm::Registers& r) { binaryOp(w, r, [](Q a, Q b) { return L::maxQ(a, b); }); }

void opClamp(const u32* w, vm::Registers& r) {
    const Q lo = L::set1Q(opK(w, 0)), hi = L::set1Q(opK(w, 1));
    const i64* a = r.h[opA(w)];
    i64* out = r.h[opDst(w)];
    for (u32 i = 0; i < vm::kStride; i += QW) L::storeQ(out + i, L::minQ(L::maxQ(L::loadQ(a + i), lo), hi));
}

void opRemap(const u32* w, vm::Registers& r) {
    const i64 k2 = opK(w, 2), k3 = opK(w, 3);
    const Q inLo = L::set1Q(opK(w, 0)), scale = L::set1Q(opK(w, 1)), outLo = L::set1Q(k2);
    const Q lowest = L::set1Q(k2 < k3 ? k2 : k3), highest = L::set1Q(k2 < k3 ? k3 : k2);
    const bool clampOut = (opFlags(w) & 1u) != 0;
    const i64* a = r.h[opA(w)];
    i64* out = r.h[opDst(w)];
    for (u32 i = 0; i < vm::kStride; i += QW) {
        Q v = L::addQ(outLo, L::mulQ32(L::subQ(L::loadQ(a + i), inLo), scale));
        if (clampOut) v = L::minQ(L::maxQ(v, lowest), highest);
        L::storeQ(out + i, v);
    }
}

void opSelect(const u32* w, vm::Registers& r) {
    const Q threshold = L::set1Q(opK(w, 0));
    const i64* cond = r.h[opA(w)];
    const i64* ifGe = r.h[opB(w)];
    const i64* ifLt = r.h[opC(w)];
    i64* out = r.h[opDst(w)];
    for (u32 i = 0; i < vm::kStride; i += QW) {
        L::storeQ(out + i, L::selectGeQ(L::loadQ(cond + i), threshold, L::loadQ(ifGe + i), L::loadQ(ifLt + i)));
    }
}

// --- Domain --------------------------------------------------------------------------------------

HELIOS_FORCEINLINE V equiAngularWarp(V s) {
    const V s2 = L::mulQ30(s, s);
    V acc = L::set1(kWarpC11);
    acc = L::add(L::set1(kWarpC9), L::mulQ30(acc, s2));
    acc = L::add(L::set1(kWarpC7), L::mulQ30(acc, s2));
    acc = L::add(L::set1(kWarpC5), L::mulQ30(acc, s2));
    acc = L::add(L::set1(kWarpC3), L::mulQ30(acc, s2));
    acc = L::add(L::set1(kWarpC1), L::mulQ30(acc, s2));
    return L::mulQ30(s, acc);
}

HELIOS_FORCEINLINE V rsqrtNewton(V s) {
    const V one = L::set1(kOneQ30);
    V y = L::sub(one, L::mulQ30u(L::sub(s, one), L::set1(static_cast<i32>(kRsqrtSlope))));
    const V three = L::set1(static_cast<i32>(3u << 30));
    for (u32 it = 0; it < 4; ++it) {
        const V y2 = L::mulQ30u(y, y);
        const V sy2 = L::mulQ30u(s, y2);
        y = L::mulQ31u(y, L::sub(three, sy2));
    }
    return y;
}

// Per face: axis and sign of the normal, of u and of v (math's cubeFaceBasis).
struct FaceMap {
    u32 nAxis, uAxis, vAxis;
    i32 nSign, uSign, vSign;
};
constexpr FaceMap kFaceMaps[6] = {
    {0, 2, 1, 1, -1, 1},  // +X: (1, t, -s)
    {0, 2, 1, -1, 1, 1},  // -X: (-1, t, s)
    {1, 0, 2, 1, 1, -1},  // +Y: (s, 1, -t)
    {1, 0, 2, -1, 1, 1},  // -Y: (s, -1, t)
    {2, 0, 1, 1, 1, 1},   // +Z: (s, t, 1)
    {2, 0, 1, -1, -1, 1}, // -Z: (-s, t, -1)
};

HELIOS_FORCEINLINE V negateIf(V v, i32 sign) { return sign < 0 ? L::sub(L::set1(0), v) : v; }

void domainOp(const vm::Domain& d, vm::Registers& r) {
    const u32 kind = d.w[0];
    const u32 tileX = d.w[3], tileY = d.w[4];
    i32* hiOut[3] = {r.posHi[0][0], r.posHi[0][1], r.posHi[0][2]};
    u32* loOut[3] = {r.posLo[0][0], r.posLo[0][1], r.posLo[0][2]};
    if (kind == 1) {
        const u32 face = d.w[1] < 6 ? d.w[1] : 0;
        const u32 level = d.w[2] <= 25 ? d.w[2] : 25;
        const i32 shift = static_cast<i32>(25 - level);
        const V radius = L::set1(static_cast<i32>(d.w[5] & 0x7FFFFFFFu));
        const FaceMap& fm = kFaceMaps[face];
        for (u32 base = 0; base < vm::kStride; base += W) {
            const V i = L::loadU(r.sampleI + base), j = L::loadU(r.sampleJ + base);
            const V one = L::set1(kOneQ30);
            const V s = L::sub(L::sll(L::add(L::set1(static_cast<i32>(tileX * 64u)), i), shift), one);
            const V t = L::sub(L::sll(L::add(L::set1(static_cast<i32>(tileY * 64u)), j), shift), one);
            const V sw = equiAngularWarp(s), tw = equiAngularWarp(t);
            const V len2 = L::add(L::add(L::mulQ30(sw, sw), L::mulQ30(tw, tw)), one);
            const V inv = rsqrtNewton(len2);
            V comp[3];
            comp[fm.nAxis] = L::set1(fm.nSign * kOneQ30);
            comp[fm.uAxis] = negateIf(sw, fm.uSign);
            comp[fm.vAxis] = negateIf(tw, fm.vSign);
            for (u32 a = 0; a < 3; ++a) {
                const V unit = L::mulQ30(comp[a], inv);
                V lo, hi;
                L::mulWide(unit, radius, lo, hi);
                // (hi:lo) >> 6, arithmetic.
                const V outLo = L::bor(L::srli<6>(lo), L::slli<26>(hi));
                const V outHi = L::srai<6>(hi);
                L::store(hiOut[a] + base, outHi);
                L::storeU(loOut[a] + base, outLo);
            }
        }
    } else {
        const V spacingLo = L::set1(static_cast<i32>(d.w[12]));
        const V spacingHi = L::set1(static_cast<i32>(d.w[13]));
        V originHi[3], originLo[3];
        for (u32 a = 0; a < 3; ++a) {
            originLo[a] = L::set1(static_cast<i32>(d.w[6 + 2 * a]));
            originHi[a] = L::set1(static_cast<i32>(d.w[7 + 2 * a]));
        }
        for (u32 base = 0; base < vm::kStride; base += W) {
            const V m[2] = {L::add(L::set1(static_cast<i32>(tileX * 64u)), L::loadU(r.sampleI + base)),
                            L::add(L::set1(static_cast<i32>(tileY * 64u)), L::loadU(r.sampleJ + base))};
            for (u32 a = 0; a < 2; ++a) {
                // m * spacing (u32 x i64, wrapping): lo/hi of m * spacingLo, plus m * spacingHi.
                V lo, hi;
                L::mulWideU(m[a], spacingLo, lo, hi);
                hi = L::add(hi, L::mullo(m[a], spacingHi));
                add64(hi, lo, originHi[a], originLo[a]);
                L::store(hiOut[a] + base, hi);
                L::storeU(loOut[a] + base, lo);
            }
            L::store(hiOut[2] + base, originHi[2]);
            L::storeU(loOut[2] + base, originLo[2]);
        }
    }
}

constexpr vm::Kernels makeKernels(const char* name) {
    vm::Kernels k{name, W, &domainOp, {}};
    k.ops[0] = &opConst;   // Const
    k.ops[1] = &opFbm;     // Noise (a one-octave fbm)
    k.ops[2] = &opFbm;     // Fbm
    k.ops[3] = &opRidged;  // Ridged
    k.ops[4] = &opWarp;    // Warp
    k.ops[5] = &opAdd;
    k.ops[6] = &opSub;
    k.ops[7] = &opMul;
    k.ops[8] = &opMin;
    k.ops[9] = &opMax;
    k.ops[10] = &opClamp;
    k.ops[11] = &opRemap;
    k.ops[12] = &opSelect;
    return k;
}
