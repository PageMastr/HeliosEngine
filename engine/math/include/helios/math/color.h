// helios/math/color.h — linear colour, sRGB transfer functions and colour packing.
//
// Colour is always LINEAR (scene-referred, Rec.709/sRGB primaries, D65 white) inside the engine;
// sRGB-encoded values only exist at the edges (8-bit textures, UI hex colours, swapchains).
// Transfer functions implement the exact IEC 61966-2-1 piecewise curve (not a 2.2 gamma).
//
// Packed layouts match Vulkan formats on little-endian machines:
//   packRGBA8 / packSRGBA8   VK_FORMAT_R8G8B8A8_UNORM / _SRGB   (R in bits 0..7)
//   packRGB10A2              VK_FORMAT_A2B10G10R10_UNORM_PACK32 (R in bits 0..9, A in 30..31)
//   packRGB9E5               VK_FORMAT_E5B9G9R9_UFLOAT_PACK32   (shared-exponent HDR)
//   packRGBE                 Radiance .hdr RGBE (R, G, B, E bytes; R in bits 0..7)
//
// Determinism: 8-bit sRGB encode/decode use precomputed tables (bit-identical everywhere);
// the float transfer functions use std::pow and may differ in the last ulp between CRTs.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/vec.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

/// Linear RGBA colour (alpha straight, not premultiplied, unless stated otherwise).
struct Color {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;

    constexpr Color() noexcept = default;
    constexpr Color(f32 ar, f32 ag, f32 ab, f32 aa = 1.0f) noexcept : r(ar), g(ag), b(ab), a(aa) {}
    constexpr explicit Color(const Vec3& rgb, f32 aa = 1.0f) noexcept : r(rgb.x), g(rgb.y), b(rgb.z), a(aa) {}
    constexpr explicit Color(const Vec4& rgba) noexcept : r(rgba.x), g(rgba.y), b(rgba.z), a(rgba.w) {}

    [[nodiscard]] static constexpr Color black() noexcept { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    [[nodiscard]] static constexpr Color white() noexcept { return {1.0f, 1.0f, 1.0f, 1.0f}; }
    [[nodiscard]] static constexpr Color transparent() noexcept { return {0.0f, 0.0f, 0.0f, 0.0f}; }
    /// Decodes sRGB-encoded 8-bit channels (alpha is linear).
    [[nodiscard]] static Color fromSrgb8(u8 r8, u8 g8, u8 b8, u8 a8 = 255) noexcept;
    /// Decodes a CSS-style sRGB hex colour 0xRRGGBB (alpha = 1).
    [[nodiscard]] static Color fromSrgbHex(u32 rrggbb) noexcept;

    [[nodiscard]] constexpr Vec3 rgb() const noexcept { return {r, g, b}; }
    [[nodiscard]] constexpr Vec4 rgba() const noexcept { return {r, g, b, a}; }
    /// Colour with its RGB multiplied by alpha.
    [[nodiscard]] constexpr Color premultiplied() const noexcept { return {r * a, g * a, b * a, a}; }

    friend constexpr Color operator+(const Color& x, const Color& y) noexcept {
        return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a};
    }
    friend constexpr Color operator-(const Color& x, const Color& y) noexcept {
        return {x.r - y.r, x.g - y.g, x.b - y.b, x.a - y.a};
    }
    friend constexpr Color operator*(const Color& x, const Color& y) noexcept {
        return {x.r * y.r, x.g * y.g, x.b * y.b, x.a * y.a};
    }
    friend constexpr Color operator*(const Color& x, f32 s) noexcept {
        return {x.r * s, x.g * s, x.b * s, x.a * s};
    }
    friend constexpr Color operator*(f32 s, const Color& x) noexcept {
        return {x.r * s, x.g * s, x.b * s, x.a * s};
    }
    friend constexpr bool operator==(const Color&, const Color&) noexcept = default;
};

[[nodiscard]] constexpr Color lerp(const Color& x, const Color& y, f32 t) noexcept { return x + (y - x) * t; }
/// Relative luminance (Rec.709 / sRGB primaries) of linear RGB.
[[nodiscard]] constexpr f32 luminance(const Vec3& rgb) noexcept {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}
[[nodiscard]] constexpr f32 luminance(const Color& c) noexcept { return luminance(c.rgb()); }

// ---------------------------------------------------------------------------------------------
// sRGB transfer functions (IEC 61966-2-1).
// ---------------------------------------------------------------------------------------------
/// sRGB-encoded [0,1] -> linear. Values outside [0,1] are extended symmetrically.
[[nodiscard]] inline f32 srgbToLinear(f32 c) noexcept {
    const f32 a = abs(c);
    const f32 l = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return c < 0.0f ? -l : l;
}
/// Linear -> sRGB-encoded. Values outside [0,1] are extended symmetrically.
[[nodiscard]] inline f32 linearToSrgb(f32 l) noexcept {
    const f32 a = abs(l);
    const f32 c = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return l < 0.0f ? -c : c;
}
/// Alpha is left untouched.
[[nodiscard]] inline Color srgbToLinear(const Color& c) noexcept {
    return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), c.a};
}
[[nodiscard]] inline Color linearToSrgb(const Color& c) noexcept {
    return {linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b), c.a};
}
/// Exact table decode of an 8-bit sRGB code value.
[[nodiscard]] f32 srgb8ToLinear(u8 v) noexcept;
/// Encodes linear [0,1] to the nearest 8-bit sRGB code (rounding in the encoded domain, as GPUs
/// do). Table-driven and bit-identical on every platform. NaN -> 0.
[[nodiscard]] u8 linearToSrgb8(f32 linear) noexcept;

// ---------------------------------------------------------------------------------------------
// Packing.
// ---------------------------------------------------------------------------------------------
/// Linear UNORM8 x4 (R in the low byte). Channels are clamped to [0, 1], NaN -> 0.
[[nodiscard]] u32 packRGBA8(const Color& c) noexcept;
[[nodiscard]] Color unpackRGBA8(u32 packed) noexcept;
/// sRGB-encoded RGB + linear alpha (VK_FORMAT_R8G8B8A8_SRGB).
[[nodiscard]] u32 packSRGBA8(const Color& c) noexcept;
[[nodiscard]] Color unpackSRGBA8(u32 packed) noexcept;
/// 10-10-10-2 UNORM (VK_FORMAT_A2B10G10R10_UNORM_PACK32).
[[nodiscard]] u32 packRGB10A2(const Color& c) noexcept;
[[nodiscard]] Color unpackRGB10A2(u32 packed) noexcept;
/// Shared-exponent HDR (VK_FORMAT_E5B9G9R9_UFLOAT_PACK32): 9-bit mantissas, 5-bit exponent,
/// range [0, 65408]. Negative/NaN -> 0; larger values clamp. Relative error <= 2^-9 of the
/// largest channel.
[[nodiscard]] u32 packRGB9E5(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 unpackRGB9E5(u32 packed) noexcept;
/// Radiance RGBE (Ward): 8-bit mantissas + shared 8-bit exponent. Values below 1e-32 -> 0.
[[nodiscard]] u32 packRGBE(const Vec3& rgb) noexcept;
[[nodiscard]] Vec3 unpackRGBE(u32 packed) noexcept;
/// RGBM encoding for storing HDR in 8-bit RGBA textures: rgb = c / (m * range), a = m, with m
/// rounded up to a multiple of 1/255 so the 8-bit quantized result still decodes to <= range.
/// Colours above `range` clamp.
[[nodiscard]] Vec4 encodeRGBM(const Vec3& rgb, f32 range = 6.0f) noexcept;
[[nodiscard]] constexpr Vec3 decodeRGBM(const Vec4& rgbm, f32 range = 6.0f) noexcept {
    return rgbm.xyz() * (rgbm.w * range);
}

// ---------------------------------------------------------------------------------------------
// Colour models.
// ---------------------------------------------------------------------------------------------
/// Chromaticity of a black body at `kelvin` as linear sRGB, normalized so the largest channel is
/// 1 (multiply by the desired luminance/intensity). Uses Krystek's (1985) rational fit of the
/// Planckian locus in CIE 1960 UCS (|dxy| < 1e-3 for 1000–15000 K; smoothly extrapolated to the
/// T -> infinity limit above that), converted via CIE XYZ. Input is clamped to [1000, 40000] K;
/// out-of-gamut (negative) channels clamp to 0. Deterministic (arithmetic only).
[[nodiscard]] Color kelvinToRgb(f32 kelvin) noexcept;

/// RGB (any space, components in [0,1]) -> HSV with h, s, v in [0, 1] (h = hue / 360°).
[[nodiscard]] inline Vec3 rgbToHsv(const Vec3& c) noexcept {
    const f32 mx = maxComponent(c);
    const f32 mn = minComponent(c);
    const f32 delta = mx - mn;
    f32 h = 0.0f;
    if (delta > 0.0f) {
        if (mx == c.x) {
            h = (c.y - c.z) / delta;
            if (h < 0.0f) h += 6.0f;
        } else if (mx == c.y) {
            h = (c.z - c.x) / delta + 2.0f;
        } else {
            h = (c.x - c.y) / delta + 4.0f;
        }
        h *= 1.0f / 6.0f;
        if (h >= 1.0f) h -= 1.0f;
    }
    const f32 s = mx > 0.0f ? delta / mx : 0.0f;
    return {h, s, mx};
}
/// HSV (h wraps, s and v in [0, 1]) -> RGB. A non-finite hue is treated as 0 (red).
[[nodiscard]] inline Vec3 hsvToRgb(const Vec3& hsv) noexcept {
    // fract() is NaN for a non-finite hue (and floorToI32 of NaN would be UB), and it can round
    // up to exactly 1 for tiny negative hues; both map to hue 0, which is the same colour as 1.
    const f32 hue = fract(hsv.x);
    const f32 h6 = (hue >= 0.0f && hue < 1.0f ? hue : 0.0f) * 6.0f;
    const f32 s = saturate(hsv.y), v = hsv.z;
    const i32 sector = min(floorToI32(h6), 5);
    const f32 f = h6 - static_cast<f32>(sector);
    const f32 p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (sector) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
