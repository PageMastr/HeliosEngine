// Scalar reference of fixed-point hnoise (helios/pcg/hnoise.h). Written for clarity, one sample at a
// time, with helios::Q16 / Q2.30 arithmetic; the SIMD kernels (src/kernels) and the Slang twin are
// checked against it bit for bit.
#include "helios/pcg/hnoise.h"

#include <cmath>

namespace helios::pcg::hnoise {

LatticeCoord latticeCoord(const FixedPos& p, i32 wavelengthExp) noexcept {
    LatticeCoord c;
    splitCoord(p.x, wavelengthExp, c.cell[0], c.frac[0]);
    splitCoord(p.y, wavelengthExp, c.cell[1], c.frac[1]);
    splitCoord(p.z, wavelengthExp, c.cell[2], c.frac[2]);
    return c;
}

i32 noise3(u32 seed, const LatticeCoord& c) noexcept {
    const auto next = [](i32 v) { return static_cast<i32>(static_cast<u32>(v) + 1u); };
    const i32 x0 = c.cell[0], y0 = c.cell[1], z0 = c.cell[2];
    const i32 x1 = next(x0), y1 = next(y0), z1 = next(z0);
    const i32 fx = c.frac[0], fy = c.frac[1], fz = c.frac[2];
    const i32 gx = fx - 65536, gy = fy - 65536, gz = fz - 65536;
    const i32 n000 = gradientDot(latticeHash(seed, x0, y0, z0), fx, fy, fz);
    const i32 n100 = gradientDot(latticeHash(seed, x1, y0, z0), gx, fy, fz);
    const i32 n010 = gradientDot(latticeHash(seed, x0, y1, z0), fx, gy, fz);
    const i32 n110 = gradientDot(latticeHash(seed, x1, y1, z0), gx, gy, fz);
    const i32 n001 = gradientDot(latticeHash(seed, x0, y0, z1), fx, fy, gz);
    const i32 n101 = gradientDot(latticeHash(seed, x1, y0, z1), gx, fy, gz);
    const i32 n011 = gradientDot(latticeHash(seed, x0, y1, z1), fx, gy, gz);
    const i32 n111 = gradientDot(latticeHash(seed, x1, y1, z1), gx, gy, gz);
    const i32 wx = fade(fx), wy = fade(fy), wz = fade(fz);
    const i32 x00 = lerp(n000, n100, wx), x10 = lerp(n010, n110, wx);
    const i32 x01 = lerp(n001, n101, wx), x11 = lerp(n011, n111, wx);
    const i32 y0v = lerp(x00, x10, wy), y1v = lerp(x01, x11, wy);
    return lerp(y0v, y1v, wz);
}

i32 equiAngularWarp(i32 s) noexcept {
    // Horner in s^2 with Q2.30 rounding at every step (the kernels and the twin do exactly this).
    const Q30 x = Q30::fromRaw(s);
    const Q30 x2 = x * x;
    Q30 acc = Q30::fromRaw(kWarpCoeffs[5]);
    for (int i = 4; i >= 0; --i) acc = Q30::fromRaw(kWarpCoeffs[static_cast<usize>(i)]) + acc * x2;
    return (x * acc).raw();
}

u32 rsqrtNewtonQ30(u32 s) noexcept {
    u32 y = (1u << 30) - mulQ30u(s - (1u << 30), kRsqrtSlopeQ30);
    for (u32 i = 0; i < kRsqrtIterations; ++i) {
        const u32 y2 = mulQ30u(y, y);
        const u32 sy2 = mulQ30u(s, y2);
        const u32 t = (3u << 30) - sy2;
        y = static_cast<u32>((static_cast<u64>(y) * t + (u64(1) << 30)) >> 31);
    }
    return y;
}

std::array<i32, 3> cubeSphereDirection(CubeFace face, i32 s, i32 t) noexcept {
    const i32 sw = equiAngularWarp(s);
    const i32 tw = equiAngularWarp(t);
    const u32 len2 = static_cast<u32>(mulQ30(sw, sw)) + static_cast<u32>(mulQ30(tw, tw)) + (1u << 30);
    const i32 inv = static_cast<i32>(rsqrtNewtonQ30(len2));
    // Cube point n + s*u + t*v in math's face bases (helios/math/spherical.h).
    const CubeFaceBasis basis = cubeFaceBasis(face);
    std::array<i32, 3> comp{};
    for (int a = 0; a < 3; ++a) {
        const i32 n = static_cast<i32>(basis.normal[a]);
        const i32 u = static_cast<i32>(basis.u[a]);
        const i32 v = static_cast<i32>(basis.v[a]);
        // Exactly one of n, u, v is non-zero per axis (the bases are signed permutations).
        comp[static_cast<usize>(a)] = n != 0 ? n * (1 << 30) : (u != 0 ? u * sw : v * tw);
    }
    std::array<i32, 3> unit{};
    for (usize a = 0; a < 3; ++a) unit[a] = mulQ30(comp[a], inv);
    return unit;
}

FixedPos cubeSpherePosition(CubeFace face, i32 s, i32 t, u32 radiusQ8) noexcept {
    const std::array<i32, 3> unit = cubeSphereDirection(face, s, t);
    const i64 r = static_cast<i64>(radiusQ8 & 0x7FFFFFFFu);
    return {(unit[0] * r) >> 6, (unit[1] * r) >> 6, (unit[2] * r) >> 6};
}

u32 radiusToQ8(f64 metres) noexcept {
    if (!(metres > 0.0)) return 0;
    const f64 q = std::round(metres * 256.0);
    if (q >= 2147483647.0) return 0x7FFFFFFFu;
    return static_cast<u32>(q);
}

} // namespace helios::pcg::hnoise
