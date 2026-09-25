// ꟻLIP (LDR), implemented from the paper (see flip.h).

#include "helios/render/flip.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace helios::render {

namespace {

constexpr f64 kPi = std::numbers::pi;
constexpr f64 kQc = 0.7;  // color error exponent
constexpr f64 kQf = 0.5;  // feature error exponent
constexpr f64 kPc = 0.4;  // error redistribution: fraction of cmax ...
constexpr f64 kPt = 0.95; // ... mapped to this value
constexpr f64 kFeatureWidth = 0.082;  // degrees, peak to trough of the edge detector

// D65 reference white and its inverse (XYZ, Y = 1).
constexpr f64 kWhite[3] = {0.950428545, 1.000000000, 1.088900371};
constexpr f64 kInvWhite[3] = {1.052156925, 1.000000000, 0.918357670};

struct V3 {
    f64 x, y, z;
};

f64 srgbToLinear(f64 c) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92; }

V3 linearRgbToXyz(V3 c) {
    // sRGB primaries, D65 (exact rational form of the standard matrix).
    return {(10135552.0 * c.x + 8788810.0 * c.y + 4435075.0 * c.z) / 24577794.0,
            (2613072.0 * c.x + 8788810.0 * c.y + 887015.0 * c.z) / 12288897.0,
            (1425312.0 * c.x + 8788810.0 * c.y + 70074185.0 * c.z) / 73733382.0};
}

V3 xyzToLinearRgb(V3 c) {
    return {3.241003275 * c.x - 1.537398934 * c.y - 0.498615861 * c.z,
            -0.969224334 * c.x + 1.875930071 * c.y + 0.041554224 * c.z,
            0.055639423 * c.x - 0.204011202 * c.y + 1.057148933 * c.z};
}

V3 xyzToYcxcz(V3 c) {
    const f64 x = c.x * kInvWhite[0];
    const f64 y = c.y * kInvWhite[1];
    const f64 z = c.z * kInvWhite[2];
    return {116.0 * y - 16.0, 500.0 * (x - y), 200.0 * (y - z)};
}

V3 ycxczToXyz(V3 c) {
    const f64 y = (c.x + 16.0) / 116.0;
    const f64 x = y + c.y / 500.0;
    const f64 z = y - c.z / 200.0;
    return {x * kWhite[0], y * kWhite[1], z * kWhite[2]};
}

f64 labF(f64 t) {
    constexpr f64 delta = 6.0 / 29.0;
    return t > delta * delta * delta ? std::cbrt(t) : t / (3.0 * delta * delta) + 4.0 / 29.0;
}

flipdetail::Lab xyzToHuntLab(V3 c) {
    const f64 fx = labF(c.x * kInvWhite[0]);
    const f64 fy = labF(c.y * kInvWhite[1]);
    const f64 fz = labF(c.z * kInvWhite[2]);
    const f64 l = 116.0 * fy - 16.0;
    const f64 a = 500.0 * (fx - fy);
    const f64 b = 200.0 * (fy - fz);
    return {l, 0.01 * l * a, 0.01 * l * b};
}

/// Normalized 1D Gaussian exp(-pi^2 (x dx)^2 / b) over [-r, r].
std::vector<f32> csfGaussian(f64 b, u32 radius, f64 ppd, f64& sumOut) {
    std::vector<f64> g(2 * radius + 1);
    f64 sum = 0.0;
    const f64 dx = 1.0 / ppd;
    for (u32 i = 0; i < g.size(); ++i) {
        const f64 x = (static_cast<f64>(i) - radius) * dx;
        g[i] = std::exp(-kPi * kPi * x * x / b);
        sum += g[i];
    }
    sumOut = sum;
    std::vector<f32> out(g.size());
    for (u32 i = 0; i < g.size(); ++i) out[i] = static_cast<f32>(g[i] / sum);
    return out;
}

struct Plane {
    u32 w = 0, h = 0;
    std::vector<f32> v;
    Plane() = default;
    Plane(u32 width, u32 height) : w(width), h(height), v(static_cast<usize>(width) * height, 0.0f) {}
    f32& at(u32 x, u32 y) { return v[static_cast<usize>(y) * w + x]; }
    f32 at(u32 x, u32 y) const { return v[static_cast<usize>(y) * w + x]; }
};

/// Separable convolution (horizontal kernel kx, then vertical ky), clamp-to-edge borders.
Plane convolve(const Plane& in, const std::vector<f32>& kx, const std::vector<f32>& ky) {
    const i32 rx = static_cast<i32>(kx.size() / 2);
    const i32 ry = static_cast<i32>(ky.size() / 2);
    const i32 w = static_cast<i32>(in.w);
    const i32 h = static_cast<i32>(in.h);
    Plane tmp(in.w, in.h);
    for (i32 y = 0; y < h; ++y) {
        const f32* row = in.v.data() + static_cast<usize>(y) * in.w;
        for (i32 x = 0; x < w; ++x) {
            f32 acc = 0.0f;
            for (i32 k = -rx; k <= rx; ++k) {
                const i32 sx = std::clamp(x + k, 0, w - 1);
                acc += kx[static_cast<usize>(k + rx)] * row[sx];
            }
            tmp.at(static_cast<u32>(x), static_cast<u32>(y)) = acc;
        }
    }
    Plane out(in.w, in.h);
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            f32 acc = 0.0f;
            for (i32 k = -ry; k <= ry; ++k) {
                const i32 sy = std::clamp(y + k, 0, h - 1);
                acc += ky[static_cast<usize>(k + ry)] * tmp.at(static_cast<u32>(x), static_cast<u32>(sy));
            }
            out.at(static_cast<u32>(x), static_cast<u32>(y)) = acc;
        }
    }
    return out;
}

/// Contrast-sensitivity filter of one opponent channel: sum of up to two Gaussians.
struct CsfChannel {
    f64 a1, b1, a2, b2;
};
constexpr CsfChannel kCsfA{1.0, 0.0047, 0.0, 1e-5};
constexpr CsfChannel kCsfRg{1.0, 0.0053, 0.0, 1e-5};
constexpr CsfChannel kCsfBy{34.1, 0.04, 13.5, 0.025};

Plane csfFilter(const Plane& in, const CsfChannel& c, u32 radius, f64 ppd) {
    f64 sum1 = 0.0;
    const std::vector<f32> g1 = csfGaussian(c.b1, radius, ppd, sum1);
    if (c.a2 == 0.0) return convolve(in, g1, g1);
    // K = (w1 G1n (x) G1n + w2 G2n (x) G2n) with the 2D normalization of the paper's kernel.
    f64 sum2 = 0.0;
    const std::vector<f32> g2 = csfGaussian(c.b2, radius, ppd, sum2);
    const f64 s1 = c.a1 * std::sqrt(kPi / c.b1) * sum1 * sum1;
    const f64 s2 = c.a2 * std::sqrt(kPi / c.b2) * sum2 * sum2;
    const f32 w1 = static_cast<f32>(s1 / (s1 + s2));
    const f32 w2 = static_cast<f32>(s2 / (s1 + s2));
    const Plane p1 = convolve(in, g1, g1);
    const Plane p2 = convolve(in, g2, g2);
    Plane out(in.w, in.h);
    for (usize i = 0; i < out.v.size(); ++i) out.v[i] = w1 * p1.v[i] + w2 * p2.v[i];
    return out;
}

/// 1D feature kernels: derivative (edge) or second derivative (point) of a Gaussian, positive
/// weights normalized to 1 and negative ones to -1, plus the normalized Gaussian for the other axis.
struct FeatureKernels {
    std::vector<f32> edge, point, gauss;
};

FeatureKernels featureKernels(f64 ppd) {
    const f64 sd = 0.5 * kFeatureWidth * ppd;
    const u32 radius = flipdetail::featureRadius(ppd);
    const usize n = 2 * static_cast<usize>(radius) + 1;
    std::vector<f64> g(n), e(n), p(n);
    f64 gsum = 0.0;
    for (usize i = 0; i < n; ++i) {
        const f64 x = static_cast<f64>(i) - radius;
        g[i] = std::exp(-(x * x) / (2.0 * sd * sd));
        gsum += g[i];
        e[i] = -x * g[i];
        p[i] = (x * x / (sd * sd) - 1.0) * g[i];
    }
    auto normalize = [](std::vector<f64>& k) {
        f64 pos = 0.0, neg = 0.0;
        for (f64 v : k) (v > 0 ? pos : neg) += v;
        for (f64& v : k) v = v > 0 ? v / pos : (v < 0 ? v / -neg : 0.0);
    };
    normalize(e);
    normalize(p);
    FeatureKernels out;
    for (usize i = 0; i < n; ++i) {
        out.edge.push_back(static_cast<f32>(e[i]));
        out.point.push_back(static_cast<f32>(p[i]));
        out.gauss.push_back(static_cast<f32>(g[i] / gsum));
    }
    return out;
}

/// Edge and point feature magnitudes of a normalized luminance plane.
void features(const Plane& y, const FeatureKernels& k, Plane& edges, Plane& points) {
    const Plane ex = convolve(y, k.edge, k.gauss);
    const Plane ey = convolve(y, k.gauss, k.edge);
    const Plane px = convolve(y, k.point, k.gauss);
    const Plane py = convolve(y, k.gauss, k.point);
    edges = Plane(y.w, y.h);
    points = Plane(y.w, y.h);
    for (usize i = 0; i < y.v.size(); ++i) {
        edges.v[i] = std::sqrt(ex.v[i] * ex.v[i] + ey.v[i] * ey.v[i]);
        points.v[i] = std::sqrt(px.v[i] * px.v[i] + py.v[i] * py.v[i]);
    }
}

struct Opponent {
    Plane y, cx, cz;
};

Opponent toYcxcz(const ImageRgba8& image) {
    // 8-bit sRGB -> linear via a table (exact per value).
    f64 lut[256];
    for (u32 i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0);
    Opponent o{Plane(image.width, image.height), Plane(image.width, image.height), Plane(image.width, image.height)};
    for (usize i = 0; i < o.y.v.size(); ++i) {
        const u8* px = image.pixels.data() + i * 4;
        const V3 ycc = xyzToYcxcz(linearRgbToXyz({lut[px[0]], lut[px[1]], lut[px[2]]}));
        o.y.v[i] = static_cast<f32>(ycc.x);
        o.cx.v[i] = static_cast<f32>(ycc.y);
        o.cz.v[i] = static_cast<f32>(ycc.z);
    }
    return o;
}

/// Filtered YCxCz -> clamped linear RGB -> Hunt-adjusted Lab, per pixel.
std::vector<flipdetail::Lab> perceptual(const Opponent& o, f64 ppd) {
    const u32 radius = flipdetail::csfRadius(ppd);
    const Plane y = csfFilter(o.y, kCsfA, radius, ppd);
    const Plane cx = csfFilter(o.cx, kCsfRg, radius, ppd);
    const Plane cz = csfFilter(o.cz, kCsfBy, radius, ppd);
    std::vector<flipdetail::Lab> out(y.v.size());
    for (usize i = 0; i < y.v.size(); ++i) {
        V3 rgb = xyzToLinearRgb(ycxczToXyz({y.v[i], cx.v[i], cz.v[i]}));
        rgb = {std::clamp(rgb.x, 0.0, 1.0), std::clamp(rgb.y, 0.0, 1.0), std::clamp(rgb.z, 0.0, 1.0)};
        out[i] = xyzToHuntLab(linearRgbToXyz(rgb));
    }
    return out;
}

} // namespace

namespace flipdetail {

Lab huntLabFromLinearRgb(f64 r, f64 g, f64 b) noexcept { return xyzToHuntLab(linearRgbToXyz({r, g, b})); }

f64 hyab(const Lab& x, const Lab& y) noexcept {
    const f64 da = x.a - y.a;
    const f64 db = x.b - y.b;
    return std::abs(x.l - y.l) + std::sqrt(da * da + db * db);
}

f64 maxColorError() noexcept {
    return std::pow(hyab(huntLabFromLinearRgb(0.0, 1.0, 0.0), huntLabFromLinearRgb(0.0, 0.0, 1.0)), kQc);
}

f64 redistribute(f64 powerError, f64 cmax) noexcept {
    const f64 pccmax = kPc * cmax;
    return powerError < pccmax ? (kPt / pccmax) * powerError
                               : kPt + ((powerError - pccmax) / (cmax - pccmax)) * (1.0 - kPt);
}

u32 csfRadius(f64 ppd) noexcept {
    // Widest Gaussian of all channels (b = 0.04): three standard deviations.
    return static_cast<u32>(std::ceil(3.0 * std::sqrt(0.04 / (2.0 * kPi * kPi)) * ppd));
}

u32 featureRadius(f64 ppd) noexcept { return static_cast<u32>(std::ceil(3.0 * 0.5 * kFeatureWidth * ppd)); }

} // namespace flipdetail

f64 flipDefaultPixelsPerDegree() noexcept { return 0.7 * (3840.0 / 0.7) * kPi / 180.0; }

f32 FlipResult::percentile(f64 fraction) const {
    if (errors.empty()) return 0.0f;
    std::vector<f32> sorted = errors;
    std::sort(sorted.begin(), sorted.end());
    const f64 clamped = std::clamp(fraction, 0.0, 1.0);
    const usize index = static_cast<usize>(clamped * static_cast<f64>(sorted.size() - 1) + 0.5);
    return sorted[index];
}

u64 FlipResult::countAbove(f32 threshold) const noexcept {
    u64 n = 0;
    for (f32 e : errors) n += e > threshold ? 1 : 0;
    return n;
}

Result<FlipResult> computeFlip(const ImageRgba8& reference, const ImageRgba8& test, const FlipOptions& options) {
    if (reference.width != test.width || reference.height != test.height) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("FLIP: size mismatch {}x{} vs {}x{}", reference.width, reference.height, test.width,
                                 test.height)};
    }
    FlipResult result;
    result.width = reference.width;
    result.height = reference.height;
    if (reference.empty()) return result;
    const f64 ppd = options.pixelsPerDegree > 0.0 ? options.pixelsPerDegree : flipDefaultPixelsPerDegree();

    const Opponent ref = toYcxcz(reference);
    const Opponent tst = toYcxcz(test);

    // Color pipeline.
    const std::vector<flipdetail::Lab> refLab = perceptual(ref, ppd);
    const std::vector<flipdetail::Lab> tstLab = perceptual(tst, ppd);
    const f64 cmax = flipdetail::maxColorError();

    // Feature pipeline on the unfiltered, normalized luminance.
    const FeatureKernels kernels = featureKernels(ppd);
    auto normalizedY = [](const Plane& y) {
        Plane out(y.w, y.h);
        for (usize i = 0; i < y.v.size(); ++i) out.v[i] = (y.v[i] + 16.0f) / 116.0f;
        return out;
    };
    Plane refEdges, refPoints, tstEdges, tstPoints;
    features(normalizedY(ref.y), kernels, refEdges, refPoints);
    features(normalizedY(tst.y), kernels, tstEdges, tstPoints);

    result.errors.resize(refLab.size());
    f64 sum = 0.0;
    f32 maxError = 0.0f;
    const f64 invSqrt2 = 1.0 / std::numbers::sqrt2;
    for (usize i = 0; i < refLab.size(); ++i) {
        const f64 colorError = flipdetail::redistribute(std::pow(flipdetail::hyab(refLab[i], tstLab[i]), kQc), cmax);
        const f64 edgeDiff = std::abs(static_cast<f64>(refEdges.v[i]) - tstEdges.v[i]);
        const f64 pointDiff = std::abs(static_cast<f64>(refPoints.v[i]) - tstPoints.v[i]);
        const f64 featureError = std::pow(std::min(1.0, invSqrt2 * std::max(edgeDiff, pointDiff)), kQf);
        const f64 flip = std::pow(colorError, 1.0 - featureError);
        const f32 value = static_cast<f32>(std::clamp(flip, 0.0, 1.0));
        result.errors[i] = value;
        sum += value;
        maxError = std::max(maxError, value);
    }
    result.mean = sum / static_cast<f64>(result.errors.size());
    result.max = maxError;
    return result;
}

ImageRgba8 flipErrorImage(const FlipResult& result) {
    // Samples of the magma color map (matplotlib, CC0) at 0, 1/4, 1/2, 3/4, 1; linear in between.
    static constexpr f64 kMagma[5][3] = {{0.001462, 0.000466, 0.013866},
                                         {0.316654, 0.071690, 0.485380},
                                         {0.716387, 0.214982, 0.475290},
                                         {0.986700, 0.535582, 0.382210},
                                         {0.987053, 0.991438, 0.749504}};
    ImageRgba8 image(result.width, result.height);
    for (usize i = 0; i < result.errors.size(); ++i) {
        const f64 t = std::clamp(static_cast<f64>(result.errors[i]), 0.0, 1.0) * 4.0;
        const u32 k = std::min(static_cast<u32>(t), 3u);
        const f64 f = t - k;
        u8* px = image.pixels.data() + i * 4;
        for (u32 c = 0; c < 3; ++c) {
            const f64 v = kMagma[k][c] + (kMagma[k + 1][c] - kMagma[k][c]) * f;
            px[c] = static_cast<u8>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
        }
        px[3] = 255;
    }
    return image;
}

} // namespace helios::render
