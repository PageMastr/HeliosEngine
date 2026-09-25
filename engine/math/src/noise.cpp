// Deterministic noise (see helios/math/noise.h). Every function is a template over f32/f64 using
// only exactly specified IEEE operations; contraction is disabled by fp_control.h. Do not
// "optimize" expressions here (reassociation changes results): golden tests pin every bit.
#include "fp_control.h"

#include "helios/math/noise.h"

#include <cmath>
#include <limits>

namespace helios::noise {
namespace {

// Odd multipliers decorrelating the lattice axes before the avalanche mix.
constexpr u32 kPrimeX = 0x8DA6B343u;
constexpr u32 kPrimeY = 0xD8163841u;
constexpr u32 kPrimeZ = 0xCB1AB31Fu;
constexpr u32 kPrimeW = 0x165667B1u;
// Per-axis seed offsets (so the four axis mixes differ even at coordinate 0).
constexpr u32 kSaltY = 0x85EBCA6Bu;
constexpr u32 kSaltZ = 0xC2B2AE35u;
constexpr u32 kSaltW = 0x27D4EB2Fu;
constexpr u32 kOctaveStep = 0x9E3779B9u;
constexpr u32 kCellularSalt = 0x3C6EF372u;

// --- Lattice hashing ---------------------------------------------------------------------------
// Gradients and feature points are keyed by a hash of the integer lattice cell. Each coordinate is
// mixed on its own axis first (a seeded bijection of u32), then the per-axis hashes are summed and
// mixed again. Never replace this with ONE mix of a linear combination, hashU32(s + x*Px + y*Py +
// z*Pz): that value is constant along the kernel of the linear form mod 2^32, which always
// contains short integer vectors (about 2^(32/d) long in d dimensions; for the multipliers above
// (-1039, 243, -1146) in 3D and (-137, 94, -142, -33) in 4D), so the noise would repeat exactly every
// ~1.6k units in 3D and ~220 units in 4D. Summed non-linear axis mixes have no translational
// period other than the 2^32 wrap of each coordinate (regression test "noise: no short lattice
// periods").
struct LatticeSeed {
    u32 x, y, z, w;
};
LatticeSeed latticeSeed(u32 seed) noexcept {
    const u32 s = hashU32(seed);
    return {s, s + kSaltY, s + kSaltZ, s + kSaltW};
}
// Coordinates are taken mod 2^32 (static_cast<u32> of the i64 cell index).
u32 hashAxisX(i64 c, const LatticeSeed& ls) noexcept {
    return hashU32(static_cast<u32>(c) * kPrimeX + ls.x);
}
u32 hashAxisY(i64 c, const LatticeSeed& ls) noexcept {
    return hashU32(static_cast<u32>(c) * kPrimeY + ls.y);
}
u32 hashAxisZ(i64 c, const LatticeSeed& ls) noexcept {
    return hashU32(static_cast<u32>(c) * kPrimeZ + ls.z);
}
u32 hashAxisW(i64 c, const LatticeSeed& ls) noexcept {
    return hashU32(static_cast<u32>(c) * kPrimeW + ls.w);
}

// Output scales, from the extremes of the raw sums over 10^7 random samples (raw max in
// brackets). Perlin 2D is exactly bounded by sqrt(1/2); the others are scaled to peak just under
// 1 and clamped, so the documented [-1, 1] range always holds.
constexpr f64 kPerlin2Scale = 1.4142135623730951;  // [0.7071]
constexpr f64 kPerlin3Scale = 1.0;                 // [0.9935]
constexpr f64 kPerlin4Scale = 0.865;               // [1.1517]
constexpr f64 kSimplex2Scale = 98.0;               // [0.010080]
constexpr f64 kSimplex3Scale = 76.0;               // [0.013006]
constexpr f64 kSimplex4Scale = 62.0;               // [0.015911]

template <class T>
T clampUnit(T v) noexcept {
    return v < T(-1) ? T(-1) : (v > T(1) ? T(1) : v);
}

template <class T>
i64 floorI64(T x) noexcept {
    const i64 i = static_cast<i64>(x);
    return static_cast<T>(i) > x ? i - 1 : i;
}

template <class T>
T fade(T t) noexcept {
    return t * t * t * (t * (t * T(6) - T(15)) + T(10));
}

template <class T>
T lerpT(T a, T b, T t) noexcept {
    return a + t * (b - a);
}

template <class T>
T unit16(u32 v) noexcept {
    return static_cast<T>(v & 0xFFFFu) * T(1.0 / 65536.0);
}

// --- Gradients ---------------------------------------------------------------------------------
// 2D: 8 unit vectors at 45° steps.
template <class T>
T grad2(u32 h, T x, T y) noexcept {
    constexpr T k = T(0.70710678118654752440);
    switch (h >> 29) {
        case 0: return x;
        case 1: return -x;
        case 2: return y;
        case 3: return -y;
        case 4: return k * (x + y);
        case 5: return k * (y - x);
        case 6: return k * (x - y);
        default: return -k * (x + y);
    }
}
// 3D: Perlin's 12 cube-edge vectors, 4 of them repeated to fill 16 slots.
template <class T>
T grad3(u32 h, T x, T y, T z) noexcept {
    switch (h >> 28) {
        case 0: return x + y;
        case 1: return y - x;
        case 2: return x - y;
        case 3: return -x - y;
        case 4: return x + z;
        case 5: return z - x;
        case 6: return x - z;
        case 7: return -x - z;
        case 8: return y + z;
        case 9: return z - y;
        case 10: return y - z;
        case 11: return -y - z;
        case 12: return x + y;
        case 13: return z - y;
        case 14: return y - x;
        default: return -y - z;
    }
}
// 4D: the 32 edge vectors of the tesseract (one zero component, the others +-1).
template <class T>
T grad4(u32 h, T x, T y, T z, T w) noexcept {
    const u32 i = h >> 27;
    T a, b, c;
    switch (i >> 3) {
        case 0:
            a = y;
            b = z;
            c = w;
            break;
        case 1:
            a = x;
            b = z;
            c = w;
            break;
        case 2:
            a = x;
            b = y;
            c = w;
            break;
        default:
            a = x;
            b = y;
            c = z;
            break;
    }
    return ((i & 1u) ? -a : a) + ((i & 2u) ? -b : b) + ((i & 4u) ? -c : c);
}

// --- Perlin (improved) -------------------------------------------------------------------------
template <class T>
T perlin2(T x, T y, u32 seed) noexcept {
    const LatticeSeed ls = latticeSeed(seed);
    const i64 ix = floorI64(x), iy = floorI64(y);
    const T fx = x - static_cast<T>(ix), fy = y - static_cast<T>(iy);
    const u32 hx0 = hashAxisX(ix, ls), hx1 = hashAxisX(ix + 1, ls);
    const u32 hy0 = hashAxisY(iy, ls), hy1 = hashAxisY(iy + 1, ls);
    const T n00 = grad2(hashU32(hx0 + hy0), fx, fy);
    const T n10 = grad2(hashU32(hx1 + hy0), fx - T(1), fy);
    const T n01 = grad2(hashU32(hx0 + hy1), fx, fy - T(1));
    const T n11 = grad2(hashU32(hx1 + hy1), fx - T(1), fy - T(1));
    const T u = fade(fx), v = fade(fy);
    return clampUnit(lerpT(lerpT(n00, n10, u), lerpT(n01, n11, u), v) * T(kPerlin2Scale));
}

template <class T>
T perlin3(T x, T y, T z, u32 seed) noexcept {
    const LatticeSeed ls = latticeSeed(seed);
    const i64 ix = floorI64(x), iy = floorI64(y), iz = floorI64(z);
    const T fx = x - static_cast<T>(ix), fy = y - static_cast<T>(iy), fz = z - static_cast<T>(iz);
    const u32 hx0 = hashAxisX(ix, ls), hx1 = hashAxisX(ix + 1, ls);
    const u32 hy0 = hashAxisY(iy, ls), hy1 = hashAxisY(iy + 1, ls);
    const u32 hz0 = hashAxisZ(iz, ls), hz1 = hashAxisZ(iz + 1, ls);
    const T gx = fx - T(1), gy = fy - T(1), gz = fz - T(1);
    const T n000 = grad3(hashU32(hx0 + hy0 + hz0), fx, fy, fz);
    const T n100 = grad3(hashU32(hx1 + hy0 + hz0), gx, fy, fz);
    const T n010 = grad3(hashU32(hx0 + hy1 + hz0), fx, gy, fz);
    const T n110 = grad3(hashU32(hx1 + hy1 + hz0), gx, gy, fz);
    const T n001 = grad3(hashU32(hx0 + hy0 + hz1), fx, fy, gz);
    const T n101 = grad3(hashU32(hx1 + hy0 + hz1), gx, fy, gz);
    const T n011 = grad3(hashU32(hx0 + hy1 + hz1), fx, gy, gz);
    const T n111 = grad3(hashU32(hx1 + hy1 + hz1), gx, gy, gz);
    const T u = fade(fx), v = fade(fy), w = fade(fz);
    const T nx00 = lerpT(n000, n100, u), nx10 = lerpT(n010, n110, u);
    const T nx01 = lerpT(n001, n101, u), nx11 = lerpT(n011, n111, u);
    const T nxy0 = lerpT(nx00, nx10, v), nxy1 = lerpT(nx01, nx11, v);
    return clampUnit(lerpT(nxy0, nxy1, w) * T(kPerlin3Scale));
}

template <class T>
T perlin4(T x, T y, T z, T w, u32 seed) noexcept {
    const LatticeSeed ls = latticeSeed(seed);
    const i64 ix = floorI64(x), iy = floorI64(y), iz = floorI64(z), iw = floorI64(w);
    const T f[4] = {x - static_cast<T>(ix), y - static_cast<T>(iy), z - static_cast<T>(iz),
                    w - static_cast<T>(iw)};
    // axisHash[k][0/1]: hash of the lower/upper lattice coordinate along axis k.
    const u32 axisHash[4][2] = {{hashAxisX(ix, ls), hashAxisX(ix + 1, ls)},
                                {hashAxisY(iy, ls), hashAxisY(iy + 1, ls)},
                                {hashAxisZ(iz, ls), hashAxisZ(iz + 1, ls)},
                                {hashAxisW(iw, ls), hashAxisW(iw + 1, ls)}};
    // Corner c has bit k set when it is the +1 corner along axis k.
    T n[16];
    for (u32 c = 0; c < 16; ++c) {
        u32 h = 0;
        T d[4];
        for (u32 k = 0; k < 4; ++k) {
            const u32 hi = (c >> k) & 1u;
            h += axisHash[k][hi];
            d[k] = hi != 0u ? f[k] - T(1) : f[k];
        }
        n[c] = grad4(hashU32(h), d[0], d[1], d[2], d[3]);
    }
    const T fu[4] = {fade(f[0]), fade(f[1]), fade(f[2]), fade(f[3])};
    // Reduce along x, then y, then z, then w (fixed order).
    T r8[8];
    for (u32 i = 0; i < 8; ++i) r8[i] = lerpT(n[2 * i], n[2 * i + 1], fu[0]);
    T r4[4];
    for (u32 i = 0; i < 4; ++i) r4[i] = lerpT(r8[2 * i], r8[2 * i + 1], fu[1]);
    const T r2a = lerpT(r4[0], r4[1], fu[2]);
    const T r2b = lerpT(r4[2], r4[3], fu[2]);
    return clampUnit(lerpT(r2a, r2b, fu[3]) * T(kPerlin4Scale));
}

// --- Simplex -----------------------------------------------------------------------------------
template <class T>
T simplexKernel(T r2) noexcept {
    const T t = T(0.5) - r2;
    return t > T(0) ? (t * t) * (t * t) : T(0);
}

template <class T>
T simplex2(T x, T y, u32 seed) noexcept {
    constexpr T kF2 = T(0.36602540378443864676);  // (sqrt(3) - 1) / 2
    constexpr T kG2 = T(0.21132486540518711775);  // (3 - sqrt(3)) / 6
    const LatticeSeed ls = latticeSeed(seed);
    const T sk = (x + y) * kF2;
    const i64 i = floorI64(x + sk), j = floorI64(y + sk);
    const T t = static_cast<T>(i + j) * kG2;
    const T x0 = x - (static_cast<T>(i) - t), y0 = y - (static_cast<T>(j) - t);
    const u32 i1 = x0 > y0 ? 1u : 0u, j1 = 1u - i1;
    const T x1 = x0 - static_cast<T>(i1) + kG2, y1 = y0 - static_cast<T>(j1) + kG2;
    const T x2 = x0 - T(1) + T(2) * kG2, y2 = y0 - T(1) + T(2) * kG2;
    const u32 hx[2] = {hashAxisX(i, ls), hashAxisX(i + 1, ls)};
    const u32 hy[2] = {hashAxisY(j, ls), hashAxisY(j + 1, ls)};
    const T k0 = simplexKernel(x0 * x0 + y0 * y0);
    const T k1 = simplexKernel(x1 * x1 + y1 * y1);
    const T k2 = simplexKernel(x2 * x2 + y2 * y2);
    T n = T(0);
    if (k0 > T(0)) n += k0 * grad2(hashU32(hx[0] + hy[0]), x0, y0);
    if (k1 > T(0)) n += k1 * grad2(hashU32(hx[i1] + hy[j1]), x1, y1);
    if (k2 > T(0)) n += k2 * grad2(hashU32(hx[1] + hy[1]), x2, y2);
    return clampUnit(n * T(kSimplex2Scale));
}

template <class T>
T simplex3(T x, T y, T z, u32 seed) noexcept {
    constexpr T kF3 = T(1.0 / 3.0);
    constexpr T kG3 = T(1.0 / 6.0);
    const LatticeSeed ls = latticeSeed(seed);
    const T sk = (x + y + z) * kF3;
    const i64 i = floorI64(x + sk), j = floorI64(y + sk), k = floorI64(z + sk);
    const T t = static_cast<T>(i + j + k) * kG3;
    const T x0 = x - (static_cast<T>(i) - t), y0 = y - (static_cast<T>(j) - t),
            z0 = z - (static_cast<T>(k) - t);
    // Simplex traversal order from the rank of the offsets.
    u32 i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 1;
            k2 = 0;
        } else if (x0 >= z0) {
            i1 = 1;
            j1 = 0;
            k1 = 0;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        } else {
            i1 = 0;
            j1 = 0;
            k1 = 1;
            i2 = 1;
            j2 = 0;
            k2 = 1;
        }
    } else {
        if (y0 < z0) {
            i1 = 0;
            j1 = 0;
            k1 = 1;
            i2 = 0;
            j2 = 1;
            k2 = 1;
        } else if (x0 < z0) {
            i1 = 0;
            j1 = 1;
            k1 = 0;
            i2 = 0;
            j2 = 1;
            k2 = 1;
        } else {
            i1 = 0;
            j1 = 1;
            k1 = 0;
            i2 = 1;
            j2 = 1;
            k2 = 0;
        }
    }
    const T x1 = x0 - static_cast<T>(i1) + kG3, y1 = y0 - static_cast<T>(j1) + kG3,
            z1 = z0 - static_cast<T>(k1) + kG3;
    const T x2 = x0 - static_cast<T>(i2) + T(2) * kG3, y2 = y0 - static_cast<T>(j2) + T(2) * kG3,
            z2 = z0 - static_cast<T>(k2) + T(2) * kG3;
    const T x3 = x0 - T(1) + T(3) * kG3, y3 = y0 - T(1) + T(3) * kG3, z3 = z0 - T(1) + T(3) * kG3;
    const u32 hx[2] = {hashAxisX(i, ls), hashAxisX(i + 1, ls)};
    const u32 hy[2] = {hashAxisY(j, ls), hashAxisY(j + 1, ls)};
    const u32 hz[2] = {hashAxisZ(k, ls), hashAxisZ(k + 1, ls)};
    T n = T(0);
    T kk = simplexKernel(x0 * x0 + y0 * y0 + z0 * z0);
    if (kk > T(0)) n += kk * grad3(hashU32(hx[0] + hy[0] + hz[0]), x0, y0, z0);
    kk = simplexKernel(x1 * x1 + y1 * y1 + z1 * z1);
    if (kk > T(0)) n += kk * grad3(hashU32(hx[i1] + hy[j1] + hz[k1]), x1, y1, z1);
    kk = simplexKernel(x2 * x2 + y2 * y2 + z2 * z2);
    if (kk > T(0)) n += kk * grad3(hashU32(hx[i2] + hy[j2] + hz[k2]), x2, y2, z2);
    kk = simplexKernel(x3 * x3 + y3 * y3 + z3 * z3);
    if (kk > T(0)) n += kk * grad3(hashU32(hx[1] + hy[1] + hz[1]), x3, y3, z3);
    return clampUnit(n * T(kSimplex3Scale));
}

template <class T>
T simplex4(T x, T y, T z, T w, u32 seed) noexcept {
    constexpr T kF4 = T(0.30901699437494742410);  // (sqrt(5) - 1) / 4
    constexpr T kG4 = T(0.13819660112501051518);  // (5 - sqrt(5)) / 20
    const LatticeSeed ls = latticeSeed(seed);
    const T sk = (x + y + z + w) * kF4;
    const i64 cell[4] = {floorI64(x + sk), floorI64(y + sk), floorI64(z + sk), floorI64(w + sk)};
    const T t = static_cast<T>(cell[0] + cell[1] + cell[2] + cell[3]) * kG4;
    const T d0[4] = {x - (static_cast<T>(cell[0]) - t), y - (static_cast<T>(cell[1]) - t),
                     z - (static_cast<T>(cell[2]) - t), w - (static_cast<T>(cell[3]) - t)};
    // Rank each offset against the others (ties broken by axis order) to find the simplex.
    int rank[4] = {0, 0, 0, 0};
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            if (d0[a] > d0[b])
                ++rank[a];
            else
                ++rank[b];
        }
    }
    // axisHash[a][0/1]: hash of the lower/upper lattice coordinate along axis a.
    const u32 axisHash[4][2] = {{hashAxisX(cell[0], ls), hashAxisX(cell[0] + 1, ls)},
                                {hashAxisY(cell[1], ls), hashAxisY(cell[1] + 1, ls)},
                                {hashAxisZ(cell[2], ls), hashAxisZ(cell[2] + 1, ls)},
                                {hashAxisW(cell[3], ls), hashAxisW(cell[3] + 1, ls)}};
    T n = T(0);
    // Vertex v (0..4): offset o_a = 1 if rank[a] >= 4 - v (vertex 0 = origin, vertex 4 = +1,+1,+1,+1).
    for (int v = 0; v < 5; ++v) {
        T d[4];
        u32 h = 0;
        T r2 = T(0);
        for (int a = 0; a < 4; ++a) {
            const u32 o = (v > 0 && rank[a] >= 4 - v) ? 1u : 0u;
            d[a] = d0[a] - static_cast<T>(o) + static_cast<T>(v) * kG4;
            h += axisHash[a][o];
            r2 += d[a] * d[a];
        }
        const T kk = simplexKernel(r2);
        if (kk > T(0)) n += kk * grad4(hashU32(h), d[0], d[1], d[2], d[3]);
    }
    return clampUnit(n * T(kSimplex4Scale));
}

// --- Dimension-generic dispatch ----------------------------------------------------------------
template <class T>
T basisNoise(const TVec2<T>& p, u32 seed, Basis b) noexcept {
    return b == Basis::Perlin ? perlin2(p.x, p.y, seed) : simplex2(p.x, p.y, seed);
}
template <class T>
T basisNoise(const TVec3<T>& p, u32 seed, Basis b) noexcept {
    return b == Basis::Perlin ? perlin3(p.x, p.y, p.z, seed) : simplex3(p.x, p.y, p.z, seed);
}
template <class T>
T basisNoise(const TVec4<T>& p, u32 seed, Basis b) noexcept {
    return b == Basis::Perlin ? perlin4(p.x, p.y, p.z, p.w, seed) : simplex4(p.x, p.y, p.z, p.w, seed);
}

int clampOctaves(int octaves) noexcept { return octaves < 1 ? 1 : (octaves > 32 ? 32 : octaves); }

template <class V>
typename V::ValueType fbmImpl(const V& p, u32 seed, const FbmParams& prm) noexcept {
    using T = typename V::ValueType;
    const int octaves = clampOctaves(prm.octaves);
    const T lacunarity = static_cast<T>(prm.lacunarity);
    const T persistence = static_cast<T>(prm.persistence);
    T freq = static_cast<T>(prm.frequency);
    T amp = T(1), sum = T(0), norm = T(0);
    for (int o = 0; o < octaves; ++o) {
        sum += amp * basisNoise(p * freq, seed + static_cast<u32>(o) * kOctaveStep, prm.basis);
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return norm > T(0) ? clampUnit(sum / norm) : T(0);
}

template <class V>
typename V::ValueType ridgedImpl(const V& p, u32 seed, const RidgedParams& prm) noexcept {
    using T = typename V::ValueType;
    const int octaves = clampOctaves(prm.octaves);
    const T lacunarity = static_cast<T>(prm.lacunarity);
    const T persistence = static_cast<T>(prm.persistence);
    const T offset = static_cast<T>(prm.offset);
    const T gain = static_cast<T>(prm.gain);
    T freq = static_cast<T>(prm.frequency);
    T spectral = T(1), weight = T(1), sum = T(0), norm = T(0);
    for (int o = 0; o < octaves; ++o) {
        T signal =
            offset - std::fabs(basisNoise(p * freq, seed + static_cast<u32>(o) * kOctaveStep, prm.basis));
        signal *= signal;
        signal *= weight;
        const T wg = signal * gain;
        weight = wg > T(1) ? T(1) : (wg < T(0) ? T(0) : wg);
        sum += signal * spectral;
        norm += spectral * offset * offset;
        spectral *= persistence;
        freq *= lacunarity;
    }
    if (!(norm > T(0))) return T(0);
    const T r = sum / norm;
    return r > T(1) ? T(1) : r;
}

template <class T>
TVec2<T> warpImpl(const TVec2<T>& p, u32 seed, const WarpParams& prm) noexcept {
    const T a = static_cast<T>(prm.amplitude);
    return {p.x + a * fbmImpl(p, hashCombineU32(seed, 0x51ED270Bu), prm.fbm),
            p.y + a * fbmImpl(p, hashCombineU32(seed, 0x6A09E667u), prm.fbm)};
}
template <class T>
TVec3<T> warpImpl(const TVec3<T>& p, u32 seed, const WarpParams& prm) noexcept {
    const T a = static_cast<T>(prm.amplitude);
    return {p.x + a * fbmImpl(p, hashCombineU32(seed, 0x51ED270Bu), prm.fbm),
            p.y + a * fbmImpl(p, hashCombineU32(seed, 0x6A09E667u), prm.fbm),
            p.z + a * fbmImpl(p, hashCombineU32(seed, 0xBB67AE85u), prm.fbm)};
}

// --- Cellular ----------------------------------------------------------------------------------
template <class T>
T boxDistance(T f, T lo, T hi) noexcept {
    return f < lo ? lo - f : (f > hi ? f - hi : T(0));
}

template <class T>
T clampJitter(T j) noexcept {
    return j > T(0) ? (j < T(1) ? j : T(1)) : T(0);
}

constexpr u32 kCellIdSalt = 0xA511E9B3u;

// Per-cell hash shared by cellular() and cellularSite(): the lattice hash of the cell with the
// cellular seed. The jitter offsets use bits 0..15 and 16..31 of h (and of
// h2 = hashU32(h ^ kOctaveStep) for z); the id is hashU32(h ^ kCellIdSalt).
LatticeSeed cellularSeed(u32 seed) noexcept { return latticeSeed(seed ^ kCellularSalt); }
u32 cellHash2(const LatticeSeed& ls, i64 cx, i64 cy) noexcept {
    return hashU32(hashAxisX(cx, ls) + hashAxisY(cy, ls));
}
u32 cellHash3(const LatticeSeed& ls, i64 cx, i64 cy, i64 cz) noexcept {
    return hashU32(hashAxisX(cx, ls) + hashAxisY(cy, ls) + hashAxisZ(cz, ls));
}

template <class T>
TCellular<T> cellular2(T x, T y, u32 seed, T jitterIn) noexcept {
    const LatticeSeed ls = cellularSeed(seed);
    const T jitter = clampJitter(jitterIn);
    const i64 ix = floorI64(x), iy = floorI64(y);
    const T fx = x - static_cast<T>(ix), fy = y - static_cast<T>(iy);
    const T lo = T(0.5) - T(0.5) * jitter, hi = T(0.5) + T(0.5) * jitter;
    const T edge = min(min(fx, T(1) - fx), min(fy, T(1) - fy));
    T d1 = std::numeric_limits<T>::infinity(), d2 = d1;
    u32 id = 0;
    for (int r = 0; r <= 8; ++r) {
        if (r > 0) {
            // Every feature point in ring r is at least this far away; stop once F2 is closer.
            const T bound = static_cast<T>(r - 1) + lo + edge;
            if (bound * bound >= d2) break;
        }
        for (int dy = -r; dy <= r; ++dy) {
            const bool fullRow = dy == -r || dy == r;
            const T by = boxDistance(fy, static_cast<T>(dy) + lo, static_cast<T>(dy) + hi);
            const u32 hy = hashAxisY(iy + dy, ls);
            for (int dx = -r; dx <= r; dx += fullRow ? 1 : 2 * r) {
                const T bx = boxDistance(fx, static_cast<T>(dx) + lo, static_cast<T>(dx) + hi);
                if (bx * bx + by * by >= d2) continue;
                const u32 h = hashU32(hashAxisX(ix + dx, ls) + hy);  // == cellHash2(ls, ix + dx, iy + dy)
                const T ox = static_cast<T>(dx) + lo + jitter * unit16<T>(h) - fx;
                const T oy = static_cast<T>(dy) + lo + jitter * unit16<T>(h >> 16) - fy;
                const T d = ox * ox + oy * oy;
                if (d < d1) {
                    d2 = d1;
                    d1 = d;
                    id = h;
                } else if (d < d2) {
                    d2 = d;
                }
            }
        }
    }
    return {std::sqrt(d1), std::sqrt(d2), hashU32(id ^ kCellIdSalt)};
}

template <class T>
TCellular<T> cellular3(T x, T y, T z, u32 seed, T jitterIn) noexcept {
    const LatticeSeed ls = cellularSeed(seed);
    const T jitter = clampJitter(jitterIn);
    const i64 ix = floorI64(x), iy = floorI64(y), iz = floorI64(z);
    const T fx = x - static_cast<T>(ix), fy = y - static_cast<T>(iy), fz = z - static_cast<T>(iz);
    const T lo = T(0.5) - T(0.5) * jitter, hi = T(0.5) + T(0.5) * jitter;
    const T edge = min(min(min(fx, T(1) - fx), min(fy, T(1) - fy)), min(fz, T(1) - fz));
    T d1 = std::numeric_limits<T>::infinity(), d2 = d1;
    u32 id = 0;
    for (int r = 0; r <= 8; ++r) {
        if (r > 0) {
            const T bound = static_cast<T>(r - 1) + lo + edge;
            if (bound * bound >= d2) break;
        }
        for (int dz = -r; dz <= r; ++dz) {
            const T bz = boxDistance(fz, static_cast<T>(dz) + lo, static_cast<T>(dz) + hi);
            const u32 hz = hashAxisZ(iz + dz, ls);
            for (int dy = -r; dy <= r; ++dy) {
                const bool fullRow = dz == -r || dz == r || dy == -r || dy == r;
                const T by = boxDistance(fy, static_cast<T>(dy) + lo, static_cast<T>(dy) + hi);
                const u32 hyz = hashAxisY(iy + dy, ls) + hz;
                for (int dx = -r; dx <= r; dx += fullRow ? 1 : 2 * r) {
                    const T bx = boxDistance(fx, static_cast<T>(dx) + lo, static_cast<T>(dx) + hi);
                    if (bx * bx + by * by + bz * bz >= d2) continue;
                    // == cellHash3(ls, ix + dx, iy + dy, iz + dz) (u32 addition is associative).
                    const u32 h = hashU32(hashAxisX(ix + dx, ls) + hyz);
                    const u32 h2 = hashU32(h ^ kOctaveStep);
                    const T ox = static_cast<T>(dx) + lo + jitter * unit16<T>(h) - fx;
                    const T oy = static_cast<T>(dy) + lo + jitter * unit16<T>(h >> 16) - fy;
                    const T oz = static_cast<T>(dz) + lo + jitter * unit16<T>(h2) - fz;
                    const T d = ox * ox + oy * oy + oz * oz;
                    if (d < d1) {
                        d2 = d1;
                        d1 = d;
                        id = h;
                    } else if (d < d2) {
                        d2 = d;
                    }
                }
            }
        }
    }
    return {std::sqrt(d1), std::sqrt(d2), hashU32(id ^ kCellIdSalt)};
}

}  // namespace

f32 perlin(const Vec2& p, u32 seed) noexcept { return perlin2(p.x, p.y, seed); }
f32 perlin(const Vec3& p, u32 seed) noexcept { return perlin3(p.x, p.y, p.z, seed); }
f32 perlin(const Vec4& p, u32 seed) noexcept { return perlin4(p.x, p.y, p.z, p.w, seed); }
f64 perlin(const DVec2& p, u32 seed) noexcept { return perlin2(p.x, p.y, seed); }
f64 perlin(const DVec3& p, u32 seed) noexcept { return perlin3(p.x, p.y, p.z, seed); }
f64 perlin(const DVec4& p, u32 seed) noexcept { return perlin4(p.x, p.y, p.z, p.w, seed); }

f32 simplex(const Vec2& p, u32 seed) noexcept { return simplex2(p.x, p.y, seed); }
f32 simplex(const Vec3& p, u32 seed) noexcept { return simplex3(p.x, p.y, p.z, seed); }
f32 simplex(const Vec4& p, u32 seed) noexcept { return simplex4(p.x, p.y, p.z, p.w, seed); }
f64 simplex(const DVec2& p, u32 seed) noexcept { return simplex2(p.x, p.y, seed); }
f64 simplex(const DVec3& p, u32 seed) noexcept { return simplex3(p.x, p.y, p.z, seed); }
f64 simplex(const DVec4& p, u32 seed) noexcept { return simplex4(p.x, p.y, p.z, p.w, seed); }

f32 fbm(const Vec2& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }
f32 fbm(const Vec3& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }
f32 fbm(const Vec4& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }
f64 fbm(const DVec2& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }
f64 fbm(const DVec3& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }
f64 fbm(const DVec4& p, u32 seed, const FbmParams& params) noexcept { return fbmImpl(p, seed, params); }

f32 ridged(const Vec2& p, u32 seed, const RidgedParams& params) noexcept {
    return ridgedImpl(p, seed, params);
}
f32 ridged(const Vec3& p, u32 seed, const RidgedParams& params) noexcept {
    return ridgedImpl(p, seed, params);
}
f64 ridged(const DVec2& p, u32 seed, const RidgedParams& params) noexcept {
    return ridgedImpl(p, seed, params);
}
f64 ridged(const DVec3& p, u32 seed, const RidgedParams& params) noexcept {
    return ridgedImpl(p, seed, params);
}

Vec2 domainWarp(const Vec2& p, u32 seed, const WarpParams& params) noexcept {
    return warpImpl(p, seed, params);
}
Vec3 domainWarp(const Vec3& p, u32 seed, const WarpParams& params) noexcept {
    return warpImpl(p, seed, params);
}
DVec2 domainWarp(const DVec2& p, u32 seed, const WarpParams& params) noexcept {
    return warpImpl(p, seed, params);
}
DVec3 domainWarp(const DVec3& p, u32 seed, const WarpParams& params) noexcept {
    return warpImpl(p, seed, params);
}

CellularSite2 cellularSite(const IVec2& cell, u32 seed, f64 jitterIn) noexcept {
    const LatticeSeed ls = cellularSeed(seed);
    const f64 jitter = clampJitter(jitterIn);
    const f64 lo = 0.5 - 0.5 * jitter;
    const u32 h = cellHash2(ls, cell.x, cell.y);
    return {{static_cast<f64>(cell.x) + (lo + jitter * unit16<f64>(h)),
             static_cast<f64>(cell.y) + (lo + jitter * unit16<f64>(h >> 16))},
            hashU32(h ^ kCellIdSalt)};
}

CellularSite3 cellularSite(const IVec3& cell, u32 seed, f64 jitterIn) noexcept {
    const LatticeSeed ls = cellularSeed(seed);
    const f64 jitter = clampJitter(jitterIn);
    const f64 lo = 0.5 - 0.5 * jitter;
    const u32 h = cellHash3(ls, cell.x, cell.y, cell.z);
    const u32 h2 = hashU32(h ^ kOctaveStep);
    return {{static_cast<f64>(cell.x) + (lo + jitter * unit16<f64>(h)),
             static_cast<f64>(cell.y) + (lo + jitter * unit16<f64>(h >> 16)),
             static_cast<f64>(cell.z) + (lo + jitter * unit16<f64>(h2))},
            hashU32(h ^ kCellIdSalt)};
}

Cellular cellular(const Vec2& p, u32 seed, f32 jitter) noexcept { return cellular2(p.x, p.y, seed, jitter); }
Cellular cellular(const Vec3& p, u32 seed, f32 jitter) noexcept {
    return cellular3(p.x, p.y, p.z, seed, jitter);
}
DCellular cellular(const DVec2& p, u32 seed, f64 jitter) noexcept {
    return cellular2(p.x, p.y, seed, jitter);
}
DCellular cellular(const DVec3& p, u32 seed, f64 jitter) noexcept {
    return cellular3(p.x, p.y, p.z, seed, jitter);
}

}  // namespace helios::noise
