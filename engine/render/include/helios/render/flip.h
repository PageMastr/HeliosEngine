#pragma once
// ꟻLIP (LDR) image difference metric — Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström and
// Fairchild, "FLIP: A Difference Evaluator for Alternating Images", Proc. ACM on Computer Graphics
// and Interactive Techniques 3(2), 2020. Implemented here from the published algorithm (NVIDIA's
// reference implementation is BSD-3-Clause; no code is copied from it).
//
// Per pixel, in [0, 1] (0 = indistinguishable when flipping between the two images):
//   * Color: both images go to the opponent space YCxCz, are low-pass filtered with the
//     contrast-sensitivity functions of the achromatic, red-green and blue-yellow channels (sums of
//     Gaussians for the viewing distance in pixels per degree), are clamped to the linear-RGB cube,
//     converted to CIELAB with the Hunt adjustment (a, b scaled by 0.01 L), compared with HyAB
//     (|dL| + |d(a,b)|), raised to qc = 0.7 and remapped so that pc = 0.4 of the maximum (green vs
//     blue) maps to pt = 0.95.
//   * Features: edges and points of the normalized luminance, detected with first/second
//     Gaussian-derivative filters (peak-to-trough width 0.082 degrees); the larger difference of the
//     edge and point magnitudes, scaled by 1/sqrt(2) and raised to qf = 0.5.
//   * FLIP = colorError ^ (1 - featureError).
// Filters are applied separably with clamp-to-edge borders. Default viewing conditions follow the
// paper: a 0.7 m wide, 3840-pixel monitor at 0.7 m (67.02 pixels per degree).
//
// Golden-image policy (03 §8.4): mean ≤ 0.01 plus a per-test maximum.
// Threading: pure functions. Cost: ~0.1–0.3 s for a 640x360 pair on one core.

#include <vector>

#include "helios/core/result.h"
#include "helios/render/image.h"

namespace helios::render {

struct FlipOptions {
    /// Observer's pixels per degree of visual angle: distance * (resolution / width) * pi / 180.
    f64 pixelsPerDegree = 0.0;  ///< 0 = flipDefaultPixelsPerDegree().
};

struct FlipResult {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> errors;  ///< Per pixel, row-major, in [0, 1].
    f64 mean = 0.0;
    f32 max = 0.0f;

    /// Error value below which `fraction` (0..1) of the pixels lie.
    f32 percentile(f64 fraction) const;
    /// Pixels with an error above `threshold`.
    u64 countAbove(f32 threshold) const noexcept;
};

/// 0.7 m * (3840 px / 0.7 m) * pi / 180 = 67.0206...
f64 flipDefaultPixelsPerDegree() noexcept;

/// ꟻLIP between two sRGB images of equal size (alpha ignored). InvalidArgument on size mismatch.
Result<FlipResult> computeFlip(const ImageRgba8& reference, const ImageRgba8& test, const FlipOptions& options = {});

/// Error map as an image (magma color map, black = no difference).
ImageRgba8 flipErrorImage(const FlipResult& result);

/// Internals exposed for tests.
namespace flipdetail {
struct Lab {
    f64 l, a, b;
};
/// Linear RGB -> Hunt-adjusted CIELAB (D65), as used by the color pipeline.
Lab huntLabFromLinearRgb(f64 r, f64 g, f64 b) noexcept;
/// HyAB distance between two (Hunt-adjusted) Lab colors.
f64 hyab(const Lab& x, const Lab& y) noexcept;
/// Maximum color error (Hunt-adjusted green vs blue) raised to qc.
f64 maxColorError() noexcept;
/// Remaps a HyAB^qc error to [0, 1].
f64 redistribute(f64 powerError, f64 cmax) noexcept;
/// Spatial filter radius (pixels) at `ppd`.
u32 csfRadius(f64 ppd) noexcept;
/// Feature filter radius (pixels) at `ppd`.
u32 featureRadius(f64 ppd) noexcept;
} // namespace flipdetail

} // namespace helios::render
