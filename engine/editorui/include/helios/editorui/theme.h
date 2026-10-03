#pragma once
// Editor themes (07 §1.3, §4.3): dark (default), light and high-contrast token files
// (engine/editorui/themes/*.jsonc, embedded at build time; `--theme-file` loads another). Tokens
// map onto ImGui's style; metrics are DIPs scaled by the UI scale. The contrast check is the
// "theme-token text contrast below 4.5:1, or 7:1 in high contrast" layout lint of 07 §4.4.
//
// Threading: plain values and pure functions.

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

struct ImGuiStyle;

namespace helios::edui {

/// sRGB color with alpha, components in [0, 1].
struct Color {
    f32 r = 0, g = 0, b = 0, a = 1;
    friend bool operator==(const Color&, const Color&) = default;
};

/// "#RRGGBB" or "#RRGGBBAA".
Result<Color> parseColor(std::string_view text);
/// Packed 0xAABBGGRR (ImGui's IM_COL32 order).
u32 packColor(const Color& c) noexcept;
/// `top` composited over opaque `bottom` (sRGB-space "over", as ImGui blends).
Color over(const Color& top, const Color& bottom) noexcept;
/// WCAG 2.2 relative luminance and contrast ratio (1..21) of two opaque colors.
f64 relativeLuminance(const Color& c) noexcept;
f64 contrastRatio(const Color& a, const Color& b) noexcept;

struct ThemeMetrics {
    f32 fontSize = 15;
    f32 windowRounding = 2;
    f32 frameRounding = 2;
    f32 tabRounding = 2;
    f32 windowBorderSize = 1;
    f32 frameBorderSize = 0;
    f32 framePaddingX = 6;
    f32 framePaddingY = 3;
    f32 itemSpacingX = 8;
    f32 itemSpacingY = 4;
};

struct Theme {
    std::string name;
    bool highContrast = false;
    std::map<std::string, Color, std::less<>> colors;
    ThemeMetrics metrics;

    /// Token color (magenta when missing, so a gap is visible).
    Color color(std::string_view token) const noexcept;
    bool has(std::string_view token) const noexcept { return colors.contains(token); }

    /// Parses a token file (JSONC). Every token in requiredTokens() must be present.
    static Result<Theme> parse(std::string_view jsonc, std::string_view sourceName = "<theme>");
    /// The token names the shell uses.
    static std::vector<std::string_view> requiredTokens();
};

/// Built-in themes: "dark", "light", "high-contrast".
std::vector<std::string_view> builtinThemeNames();
Result<Theme> builtinTheme(std::string_view name);

/// Writes the theme into an ImGui style at `scale` (1 = 100 %): colors, metrics x scale.
void applyTheme(const Theme& theme, f32 scale, ImGuiStyle& style);

struct ContrastIssue {
    std::string foreground;
    std::string background;
    f64 ratio = 0;
    f64 required = 0;
};

/// Checks every text-on-background pair the shell draws; empty when the theme passes.
std::vector<ContrastIssue> checkContrast(const Theme& theme);

} // namespace helios::edui
