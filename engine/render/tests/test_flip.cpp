// ꟻLIP: color-space stages against published CIELAB values, the analytic uniform-image case,
// kernel sizes, identity/symmetry/monotonicity properties, edge sensitivity, the error map, agreement
// with NVIDIA's reference implementation; PNG I/O.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "helios/core/fs.h"
#include "helios/render/flip.h"

using namespace helios;
using namespace helios::render;

namespace {

ImageRgba8 solid(u32 w, u32 h, u8 r, u8 g, u8 b) {
    ImageRgba8 image(w, h);
    for (usize i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = r;
        image.pixels[i + 1] = g;
        image.pixels[i + 2] = b;
        image.pixels[i + 3] = 255;
    }
    return image;
}

/// Vertical black/white edge at column `edge`.
ImageRgba8 edgeImage(u32 w, u32 h, u32 edge) {
    ImageRgba8 image = solid(w, h, 0, 0, 0);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = edge; x < w; ++x) {
            u8* p = image.at(x, y);
            p[0] = p[1] = p[2] = 255;
        }
    }
    return image;
}

FlipResult flip(const ImageRgba8& a, const ImageRgba8& b) {
    auto r = computeFlip(a, b);
    REQUIRE(r.ok());
    return std::move(r).value();
}

} // namespace

TEST_CASE("flip: viewing conditions and filter radii") {
    CHECK(flipDefaultPixelsPerDegree() == doctest::Approx(3840.0 * std::numbers::pi / 180.0));
    CHECK(flipDefaultPixelsPerDegree() == doctest::Approx(67.0206).epsilon(1e-5));
    CHECK(flipdetail::csfRadius(flipDefaultPixelsPerDegree()) == 10);    // ceil(9.05)
    CHECK(flipdetail::featureRadius(flipDefaultPixelsPerDegree()) == 9);  // ceil(3 * 2.748)
}

TEST_CASE("flip: Hunt-adjusted CIELAB of the sRGB primaries matches published Lab values") {
    // Reference CIELAB (D65) of linear sRGB white/red/green/blue; Hunt: a, b scaled by L/100.
    struct Case {
        f64 r, g, b, l, a, bb;
    };
    const Case cases[] = {{1, 1, 1, 100.0, 0.0, 0.0},
                          {1, 0, 0, 53.2408, 80.0925, 67.2032},
                          {0, 1, 0, 87.7347, -86.1827, 83.1793},
                          {0, 0, 1, 32.2970, 79.1875, -107.8602}};
    for (const Case& c : cases) {
        const flipdetail::Lab lab = flipdetail::huntLabFromLinearRgb(c.r, c.g, c.b);
        CAPTURE(c.l);
        CHECK(lab.l == doctest::Approx(c.l).epsilon(2e-4));
        CHECK(lab.a == doctest::Approx(c.a * c.l / 100.0).epsilon(2e-3));
        CHECK(lab.b == doctest::Approx(c.bb * c.l / 100.0).epsilon(2e-3));
    }
    // cmax = HyAB(hunt(green), hunt(blue))^0.7 computed from the published values.
    const f64 lg = 87.7347, ag = -86.1827 * lg / 100, bg = 83.1793 * lg / 100;
    const f64 lb = 32.2970, ab = 79.1875 * lb / 100, bb = -107.8602 * lb / 100;
    const f64 expected = std::pow(std::abs(lg - lb) + std::hypot(ag - ab, bg - bb), 0.7);
    CHECK(flipdetail::maxColorError() == doctest::Approx(expected).epsilon(1e-3));
    // Redistribution: continuous at pc * cmax (-> 0.95) and 1 at cmax.
    const f64 cmax = flipdetail::maxColorError();
    CHECK(flipdetail::redistribute(0.4 * cmax, cmax) == doctest::Approx(0.95));
    CHECK(flipdetail::redistribute(cmax, cmax) == doctest::Approx(1.0));
    CHECK(flipdetail::redistribute(0.0, cmax) == 0.0);
}

TEST_CASE("flip: identical images score 0, uniform black vs white matches the analytic value") {
    const ImageRgba8 black = solid(48, 32, 0, 0, 0);
    const ImageRgba8 white = solid(48, 32, 255, 255, 255);
    const FlipResult same = flip(white, white);
    CHECK(same.mean == 0.0);
    CHECK(same.max == 0.0f);
    CHECK(same.errors.size() == 48u * 32u);

    // Uniform images: filtering changes nothing and there are no features, so every pixel is
    // redistribute(HyAB(white, black)^0.7) with HyAB = 100 (L* 100 vs 0).
    const f64 cmax = flipdetail::maxColorError();
    const f64 expected = flipdetail::redistribute(std::pow(100.0, 0.7), cmax);
    CHECK(expected == doctest::Approx(0.9674).epsilon(2e-3));
    const FlipResult bw = flip(black, white);
    CHECK(bw.mean == doctest::Approx(expected).epsilon(1e-4));
    CHECK(bw.max == doctest::Approx(expected).epsilon(1e-4));
    CHECK(bw.percentile(0.5) == doctest::Approx(expected).epsilon(1e-4));
    CHECK(bw.countAbove(0.5f) == 48u * 32u);
}

TEST_CASE("flip: symmetric, monotonic in the size of a change, and sensitive to edges") {
    const ImageRgba8 a = edgeImage(64, 48, 32);
    ImageRgba8 slight = a;
    ImageRgba8 strong = a;
    for (u32 y = 16; y < 32; ++y) {
        for (u32 x = 8; x < 24; ++x) {
            slight.at(x, y)[0] = 12;  // dark red tint in the black half
            strong.at(x, y)[0] = 160;
        }
    }
    const FlipResult ab = flip(a, slight);
    const FlipResult ba = flip(slight, a);
    CHECK(ab.mean == doctest::Approx(ba.mean).epsilon(1e-6));
    CHECK(ab.max == doctest::Approx(ba.max).epsilon(1e-6));
    const FlipResult as = flip(a, strong);
    CHECK(ab.mean > 0.0);
    CHECK(as.mean > ab.mean);
    CHECK(as.max > ab.max);
    // Error concentrates where the images differ.
    CHECK(as.errors[static_cast<usize>(24) * 64 + 16] > 10.0f * as.errors[static_cast<usize>(40) * 64 + 56]);

    // Moving an edge by one pixel is a visible difference; the feature term drives it.
    const FlipResult moved = flip(a, edgeImage(64, 48, 33));
    CHECK(moved.max > 0.5f);
    CHECK(moved.mean < 0.2);
    CHECK(moved.errors[static_cast<usize>(20) * 64 + 32] > 0.5f);
    CHECK(moved.errors[static_cast<usize>(20) * 64 + 2] < 0.01f);  // far from the edge
    // A one-level change of a dark pixel is far below the golden-image threshold.
    ImageRgba8 tiny = a;
    tiny.at(5, 5)[1] = 1;
    CHECK(flip(a, tiny).mean < 1e-4);
}

TEST_CASE("flip: size mismatch, empty images and the error map") {
    CHECK(computeFlip(solid(4, 4, 0, 0, 0), solid(5, 4, 0, 0, 0)).errorCode() == ErrorCode::InvalidArgument);
    const FlipResult empty = flip(ImageRgba8{}, ImageRgba8{});
    CHECK(empty.errors.empty());
    CHECK(empty.percentile(0.5) == 0.0f);

    const FlipResult bw = flip(solid(8, 8, 0, 0, 0), solid(8, 8, 255, 255, 255));
    const ImageRgba8 map = flipErrorImage(bw);
    CHECK(map.width == 8);
    CHECK(map.at(3, 3)[3] == 255);
    CHECK(map.at(3, 3)[0] > 240);  // high error -> bright end of magma
    const ImageRgba8 zero = flipErrorImage(flip(solid(8, 8, 9, 9, 9), solid(8, 8, 9, 9, 9)));
    CHECK(zero.at(0, 0)[0] < 5);  // no error -> near black
}

TEST_CASE("image: PNG round trip, file I/O and pixel diff counts") {
    ImageRgba8 image(7, 5);
    for (u32 y = 0; y < 5; ++y) {
        for (u32 x = 0; x < 7; ++x) {
            u8* p = image.at(x, y);
            p[0] = static_cast<u8>(x * 30);
            p[1] = static_cast<u8>(y * 50);
            p[2] = static_cast<u8>(x ^ y);
            p[3] = static_cast<u8>(255 - x);
        }
    }
    const std::vector<u8> png = encodePng(image);
    REQUIRE(png.size() > 8);
    CHECK(png[1] == 'P');
    auto decoded = decodePng(png);
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == image);

    auto dir = fs::createUniqueTempDirectory("render-image");
    REQUIRE(dir.ok());
    const std::filesystem::path path = dir.value() / "sub" / "image.png";
    REQUIRE(writePng(path, image).ok());
    auto read = readPng(path);
    REQUIRE(read.ok());
    CHECK(read.value() == image);
    CHECK(readPng(dir.value() / "missing.png").errorCode() != ErrorCode::Ok);
    CHECK(decodePng(std::span<const u8>(png.data(), 10)).errorCode() == ErrorCode::ParseError);
    CHECK(writePng(path, ImageRgba8{}).errorCode() == ErrorCode::InvalidArgument);

    ImageRgba8 other = image;
    CHECK(countDifferentPixels(image, other) == 0);
    other.at(1, 1)[3] ^= 1;
    other.at(6, 4)[0] ^= 1;
    CHECK(countDifferentPixels(image, other) == 2);
    CHECK(countDifferentPixels(image, ImageRgba8(3, 3)) == 35);
    (void)fs::removeAll(dir.value());
}

TEST_CASE("flip: matches NVIDIA's reference implementation on procedural images") {
    // Expected values come from NVIDIA's reference ꟻLIP (pip package flip-evaluator 1.7,
    // flip_evaluator.evaluate(ref, test, "LDR") at its default 67.02 pixels per degree) run on the
    // same images, generated identically in Python. Agreement to ~1e-6 checks the whole pipeline
    // (CSF filters, Hunt/HyAB, feature detectors, redistribution) rather than single stages.
    constexpr u32 w = 64;
    constexpr u32 h = 48;
    auto noise = [&] {
        ImageRgba8 a(w, h);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8* p = a.at(x, y);
                p[0] = static_cast<u8>((x * 37 + y * 11) % 256);
                p[1] = static_cast<u8>((x * x + 3 * y) % 256);
                p[2] = static_cast<u8>(((x ^ y) * 4) % 256);
                p[3] = 255;
            }
        }
        return a;
    };
    ImageRgba8 edited = noise();
    for (u32 y = 8; y < 24; ++y) {
        for (u32 x = 10; x < 30; ++x) edited.at(x, y)[0] = static_cast<u8>(255 - edited.at(x, y)[0]);
    }
    for (u32 y = 30; y < 40; ++y) {
        for (u32 x = 40; x < 60; ++x) {
            for (u32 c = 0; c < 3; ++c) edited.at(x, y)[c] = static_cast<u8>(edited.at(x, y)[c] / 2 + 64);
        }
    }
    auto gradient = [&](u32 shift) {
        ImageRgba8 g(w, h);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8* p = g.at(x, y);
                p[0] = static_cast<u8>((x * 4) % 256);
                p[1] = static_cast<u8>((y * 5) % 256);
                p[2] = static_cast<u8>(std::min(255u, (x + y + shift) * 2));
                p[3] = 255;
            }
        }
        return g;
    };
    struct Expected {
        f64 mean, max, at5x5, at20x15, at50x35;
    };
    auto check = [&](const FlipResult& r, const Expected& e) {
        auto at = [&](u32 x, u32 y) { return static_cast<f64>(r.errors[static_cast<usize>(y) * w + x]); };
        CHECK(r.mean == doctest::Approx(e.mean).epsilon(1e-4));
        CHECK(static_cast<f64>(r.max) == doctest::Approx(e.max).epsilon(1e-4));
        CHECK(at(5, 5) == doctest::Approx(e.at5x5).epsilon(1e-3));
        CHECK(at(20, 15) == doctest::Approx(e.at20x15).epsilon(1e-4));
        CHECK(at(50, 35) == doctest::Approx(e.at50x35).epsilon(1e-4));
    };
    check(flip(noise(), edited), {0.1006335, 0.7495291, 0.0173516, 0.2270817, 0.4830123});
    check(flip(gradient(0), gradient(3)), {0.0925208, 0.1253008, 0.0408122, 0.0756577, 0.1136432});
}
