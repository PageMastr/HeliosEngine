#include "test_util.h"

using namespace helios;
using helios::test::Rng;

TEST_CASE("color: sRGB transfer functions follow IEC 61966-2-1") {
    CHECK(srgbToLinear(0.0f) == 0.0f);
    CHECK(approxEqual(srgbToLinear(1.0f), 1.0f, 1e-6f));
    CHECK(approxEqual(srgbToLinear(0.5f), 0.214041144f, 1e-6f));
    CHECK(approxEqual(linearToSrgb(0.5f), 0.735356983f, 1e-6f));
    CHECK(approxEqual(linearToSrgb(1.0f), 1.0f, 1e-6f));
    // Linear segment below the thresholds.
    CHECK(approxEqual(srgbToLinear(0.04f), 0.04f / 12.92f, 1e-9f));
    CHECK(approxEqual(linearToSrgb(0.003f), 0.003f * 12.92f, 1e-9f));
    // The two pieces meet (continuity at the threshold).
    CHECK(approxEqual(srgbToLinear(0.04045f), srgbToLinear(0.0404501f), 1e-6f));
    // (The standard's rounded constants leave a ~1.5e-5 step at the encode threshold.)
    CHECK(approxEqual(linearToSrgb(0.0031308f), linearToSrgb(0.0031309f), 2e-5f));
    // Symmetric extension for out-of-range (e.g. wide-gamut) values.
    CHECK(srgbToLinear(-0.5f) == -srgbToLinear(0.5f));
    CHECK(linearToSrgb(-0.25f) == -linearToSrgb(0.25f));
    for (int i = 0; i <= 1000; ++i) {
        const f32 x = static_cast<f32>(i) / 1000.0f;
        CHECK(approxEqual(linearToSrgb(srgbToLinear(x)), x, 2e-6f));
    }
    const Color c = srgbToLinear(Color(0.5f, 0.25f, 1.0f, 0.3f));
    CHECK(c.a == 0.3f);
    CHECK(approxEqual(linearToSrgb(c).g, 0.25f, 1e-6f));
}

TEST_CASE("color: 8-bit sRGB tables are exact and round-trip") {
    for (int i = 0; i < 256; ++i) {
        const u8 code = static_cast<u8>(i);
        const f32 lin = srgb8ToLinear(code);
        CHECK(approxEqual(lin, srgbToLinear(static_cast<f32>(i) / 255.0f), 1e-6f));
        CHECK(linearToSrgb8(lin) == code);
        if (i > 0) CHECK(lin > srgb8ToLinear(static_cast<u8>(i - 1)));
    }
    CHECK(linearToSrgb8(-1.0f) == 0);
    CHECK(linearToSrgb8(2.0f) == 255);
    CHECK(linearToSrgb8(std::nanf("")) == 0);
    // Matches rounding in the encoded domain (away from exact ties).
    Rng rng(70);
    for (int i = 0; i < 20000; ++i) {
        const f32 x = rng.rangef(0.0f, 1.0f);
        const f32 enc = linearToSrgb(x) * 255.0f;
        if (std::fabs(enc - std::floor(enc) - 0.5f) < 1e-3f) continue;
        CHECK(static_cast<int>(linearToSrgb8(x)) == static_cast<int>(std::floor(enc + 0.5f)));
    }
    const Color orange = Color::fromSrgbHex(0xFF8000u);
    CHECK(orange.r == 1.0f);
    CHECK(approxEqual(orange.g, 0.215860500f, 1e-6f));
    CHECK(orange.b == 0.0f);
    CHECK(orange.a == 1.0f);
    CHECK(Color::fromSrgb8(0, 0, 0, 128).a == unpackUnorm8(128));
}

TEST_CASE("color: RGBA8 / SRGBA8 / RGB10A2 packing layouts") {
    CHECK(packRGBA8(Color(1.0f, 0.0f, 0.0f, 0.0f)) == 0x000000FFu);
    CHECK(packRGBA8(Color(0.0f, 0.0f, 0.0f, 1.0f)) == 0xFF000000u);
    CHECK(packRGBA8(Color(2.0f, -1.0f, std::nanf(""), 0.5f)) == 0x800000FFu);  // clamp, NaN -> 0
    for (u32 v = 0; v < 256; ++v) {
        const u32 p = v | ((255u - v) << 8) | ((v * 7u & 255u) << 16) | (v << 24);
        CHECK(packRGBA8(unpackRGBA8(p)) == p);
        CHECK(packSRGBA8(unpackSRGBA8(p)) == p);
    }
    CHECK(packSRGBA8(Color(0.5f, 0.0f, 1.0f, 1.0f)) == (0xFF000000u | (255u << 16) | 188u));
    CHECK(packRGB10A2(Color(1.0f, 0.0f, 0.0f, 0.0f)) == 0x3FFu);
    CHECK(packRGB10A2(Color(0.0f, 1.0f, 0.0f, 0.0f)) == 0x3FFu << 10);
    CHECK(packRGB10A2(Color(0.0f, 0.0f, 1.0f, 0.0f)) == 0x3FFu << 20);
    CHECK(packRGB10A2(Color(0.0f, 0.0f, 0.0f, 1.0f)) == 0xC0000000u);
    Rng rng(71);
    for (int i = 0; i < 5000; ++i) {
        const Color c{rng.rangef(0.0f, 1.0f), rng.rangef(0.0f, 1.0f), rng.rangef(0.0f, 1.0f),
                      rng.rangef(0.0f, 1.0f)};
        const Color d = unpackRGB10A2(packRGB10A2(c));
        CHECK(std::fabs(d.r - c.r) <= 0.5f / 1023.0f + 1e-7f);
        CHECK(std::fabs(d.b - c.b) <= 0.5f / 1023.0f + 1e-7f);
        CHECK(std::fabs(d.a - c.a) <= 0.5f / 3.0f + 1e-7f);
        const Color e = unpackRGBA8(packRGBA8(c));
        CHECK(std::fabs(e.g - c.g) <= 0.5f / 255.0f + 1e-7f);
    }
}

TEST_CASE("color: shared-exponent RGB9E5 (Vulkan E5B9G9R9)") {
    CHECK(packRGB9E5(Vec3(0.0f)) == 0u);
    CHECK(unpackRGB9E5(packRGB9E5(Vec3(1.0f, 1.0f, 1.0f))) == Vec3(1.0f, 1.0f, 1.0f));
    CHECK(unpackRGB9E5(packRGB9E5(Vec3(0.5f, 0.25f, 0.0f))) == Vec3(0.5f, 0.25f, 0.0f));
    CHECK(unpackRGB9E5(packRGB9E5(Vec3(1e9f, -5.0f, std::nanf("")))) == Vec3(65408.0f, 0.0f, 0.0f));
    Rng rng(72);
    for (int i = 0; i < 20000; ++i) {
        const f32 scale = std::pow(2.0f, rng.rangef(-14.0f, 15.0f));
        const Vec3 c = rng.vec3(0.0f, 1.0f) * scale;
        const Vec3 d = unpackRGB9E5(packRGB9E5(c));
        const f32 mx = maxComponent(c);
        // Half a mantissa step of the shared exponent (maxc / 511), or the denormal floor 2^-25.
        CHECK(maxComponent(abs(d - c)) <= max(mx / 511.0f, std::ldexp(1.0f, -25)) * 1.0001f);
        CHECK(packRGB9E5(d) == packRGB9E5(c));  // decoded values re-encode identically
    }
}

TEST_CASE("color: Radiance RGBE and RGBM") {
    CHECK(packRGBE(Vec3(0.0f)) == 0u);
    CHECK(unpackRGBE(0u) == Vec3(0.0f));
    CHECK(packRGBE(Vec3(1e-40f)) == 0u);
    CHECK((packRGBE(Vec3(1.0f, 0.5f, 0.25f)) >> 24) == 129u);  // 1.0 = 0.5 * 2^1
    Rng rng(73);
    for (int i = 0; i < 20000; ++i) {
        const f32 scale = std::pow(2.0f, rng.rangef(-60.0f, 60.0f));
        const Vec3 c = rng.vec3(0.0f, 1.0f) * scale;
        const Vec3 d = unpackRGBE(packRGBE(c));
        const f32 mx = maxComponent(c);
        CHECK(maxComponent(abs(d - c)) <= mx * (1.0f / 128.0f));
    }
    const f32 range = 6.0f;
    for (int i = 0; i < 5000; ++i) {
        const Vec3 c = rng.vec3(0.0f, range);
        const Vec4 e = encodeRGBM(c, range);
        CHECK(std::fabs(e.w * 255.0f - std::round(e.w * 255.0f)) < 1e-3f);  // m sits on the 8-bit grid
        CHECK(maxComponent(e.xyz()) <= 1.0f);
        CHECK(approxEqual(decodeRGBM(e, range), c, 1e-5f * range));
        // Through an 8-bit texture: error bounded by the rgb quantization step times m*range.
        const Vec4 q = unpackUnorm4x8(packUnorm4x8(e));
        CHECK(maxComponent(abs(decodeRGBM(q, range) - c)) <= (0.5f / 255.0f) * q.w * range + 1e-5f);
    }
    CHECK(encodeRGBM(Vec3(0.0f)) == Vec4(0.0f));
    CHECK(approxEqual(decodeRGBM(encodeRGBM(Vec3(100.0f, 1.0f, 0.0f), range), range).x, range,
                      1e-5f));  // clamps
}

TEST_CASE("color: black-body (kelvin) colours for stars") {
    const Color sun = kelvinToRgb(5778.0f);
    CHECK(sun.r == 1.0f);
    CHECK(sun.g > 0.8f);
    CHECK(sun.b > 0.75f);
    const Color white = kelvinToRgb(6500.0f);
    CHECK(minComponent(white.rgb()) > 0.9f);
    const Color red = kelvinToRgb(1000.0f);
    CHECK(red.r == 1.0f);
    CHECK(red.g < 0.05f);
    CHECK(red.b == 0.0f);
    const Color m = kelvinToRgb(3000.0f);
    CHECK(m.r == 1.0f);
    CHECK(m.g > m.b);
    const Color o = kelvinToRgb(20000.0f);
    CHECK(o.b == 1.0f);
    CHECK(o.g > o.r);
    // Monotonic: warmer stars are redder, hotter ones bluer.
    f32 prevBlueRatio = -1.0f;
    for (f32 k = 1000.0f; k <= 40000.0f; k += 250.0f) {
        const Color c = kelvinToRgb(k);
        CHECK(maxComponent(c.rgb()) == 1.0f);
        CHECK(minComponent(c.rgb()) >= 0.0f);
        const f32 ratio = c.b / c.r;
        CHECK(ratio >= prevBlueRatio);
        prevBlueRatio = ratio;
    }
    CHECK(kelvinToRgb(500.0f) == kelvinToRgb(1000.0f));
    CHECK(kelvinToRgb(1e6f) == kelvinToRgb(40000.0f));
    CHECK(kelvinToRgb(std::nanf("")) == kelvinToRgb(1000.0f));
    // Pinned values (deterministic arithmetic only): {kelvin, r bits, g bits, b bits}.
    struct G {
        f32 kelvin;
        u32 r, g, b;
    };
    // clang-format off
    static const G golden[] = {
        {1000.0f, 0x3f800000u, 0x3c0d9bc1u, 0x00000000u},
        {1850.0f, 0x3f800000u, 0x3e617903u, 0x00000000u},
        {2700.0f, 0x3f800000u, 0x3ed4f39du, 0x3dc97b10u},
        {3500.0f, 0x3f800000u, 0x3f121067u, 0x3e84aa5du},
        {5778.0f, 0x3f800000u, 0x3f60351eu, 0x3f52929du},
        {6500.0f, 0x3f800000u, 0x3f711db7u, 0x3f7e06c4u},
        {9000.0f, 0x3f2c764eu, 0x3f3dccadu, 0x3f800000u},
        {15000.0f, 0x3eeb0c93u, 0x3f14b17bu, 0x3f800000u},
        {30000.0f, 0x3eb7f6f6u, 0x3ef9ed22u, 0x3f800000u},
        {40000.0f, 0x3eae7952u, 0x3eef98fcu, 0x3f800000u},
    };
    // clang-format on
    for (const G& gv : golden) {
        const Color c = kelvinToRgb(gv.kelvin);
        CAPTURE(gv.kelvin);
        CHECK(helios::test::bits(c.r) == gv.r);
        CHECK(helios::test::bits(c.g) == gv.g);
        CHECK(helios::test::bits(c.b) == gv.b);
    }
}

TEST_CASE("color: HSV and luminance") {
    CHECK(approxEqual(rgbToHsv(Vec3(1.0f, 0.0f, 0.0f)), Vec3(0.0f, 1.0f, 1.0f)));
    CHECK(approxEqual(rgbToHsv(Vec3(0.0f, 1.0f, 0.0f)), Vec3(1.0f / 3.0f, 1.0f, 1.0f)));
    CHECK(approxEqual(rgbToHsv(Vec3(0.0f, 0.0f, 1.0f)), Vec3(2.0f / 3.0f, 1.0f, 1.0f)));
    CHECK(approxEqual(rgbToHsv(Vec3(0.5f)), Vec3(0.0f, 0.0f, 0.5f)));
    CHECK(approxEqual(hsvToRgb(Vec3(1.0f / 6.0f, 1.0f, 1.0f)), Vec3(1.0f, 1.0f, 0.0f)));
    CHECK(approxEqual(hsvToRgb(Vec3(1.0f, 1.0f, 1.0f)), Vec3(1.0f, 0.0f, 0.0f)));  // hue wraps
    // Non-finite hue (e.g. from rgbToHsv of a NaN colour) is hue 0, not a NaN-to-int cast (UB).
    CHECK(hsvToRgb(Vec3(std::nanf(""), 1.0f, 1.0f)) == Vec3(1.0f, 0.0f, 0.0f));
    CHECK(hsvToRgb(Vec3(kInfinity, 1.0f, 1.0f)) == Vec3(1.0f, 0.0f, 0.0f));
    CHECK(approxEqual(hsvToRgb(Vec3(-1e-9f, 1.0f, 1.0f)), Vec3(1.0f, 0.0f, 0.0f)));  // fract rounds to 1
    Rng rng(74);
    for (int i = 0; i < 5000; ++i) {
        const Vec3 c = rng.vec3(0.0f, 1.0f);
        CHECK(approxEqual(hsvToRgb(rgbToHsv(c)), c, 2e-6f));
        const Vec3 h = rgbToHsv(c);
        CHECK(h.x >= 0.0f);
        CHECK(h.x < 1.0f);
    }
    CHECK(approxEqual(luminance(Color::white()), 1.0f, 1e-6f));
    CHECK(luminance(Color::black()) == 0.0f);
    CHECK(Color::transparent().a == 0.0f);
    CHECK(Color(0.5f, 0.5f, 0.5f, 0.5f).premultiplied() == Color(0.25f, 0.25f, 0.25f, 0.5f));
    CHECK(lerp(Color::black(), Color::white(), 0.5f) == Color(0.5f, 0.5f, 0.5f, 1.0f));
    CHECK(Color(Vec3(1.0f, 2.0f, 3.0f), 0.5f).rgba() == Vec4(1.0f, 2.0f, 3.0f, 0.5f));
}
