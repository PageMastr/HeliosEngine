#include "helios/editorui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>

#include "helios/reflect/json.h"
#include "imgui.h"

#include "embedded_font.h"
#include "embedded_themes.h"

namespace helios::edui {

namespace {
/// The theme of the last applyTheme() (semanticColor()); UI thread.
Theme& appliedTheme() {
    static Theme theme;
    return theme;
}
} // namespace

Result<Color> parseColor(std::string_view text) {
    if (text.size() != 7 && text.size() != 9) return Error{ErrorCode::ParseError, std::format("bad color '{}'", text)};
    if (text[0] != '#') return Error{ErrorCode::ParseError, std::format("bad color '{}'", text)};
    u8 v[4] = {0, 0, 0, 255};
    for (usize i = 0; i + 1 < text.size(); i += 2) {
        u32 byte = 0;
        for (usize k = 1; k <= 2; ++k) {
            const char c = text[i + k];
            byte <<= 4;
            if (c >= '0' && c <= '9') {
                byte |= static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                byte |= static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                byte |= static_cast<u32>(c - 'A' + 10);
            } else {
                return Error{ErrorCode::ParseError, std::format("bad color '{}'", text)};
            }
        }
        v[i / 2] = static_cast<u8>(byte);
    }
    return Color{v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f};
}

u32 packColor(const Color& c) noexcept {
    const auto q = [](f32 x) { return static_cast<u32>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
}

Color over(const Color& top, const Color& bottom) noexcept {
    const f32 a = top.a;
    return Color{top.r * a + bottom.r * (1 - a), top.g * a + bottom.g * (1 - a), top.b * a + bottom.b * (1 - a), 1.0f};
}

f64 relativeLuminance(const Color& c) noexcept {
    const auto lin = [](f64 v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

f64 contrastRatio(const Color& a, const Color& b) noexcept {
    const f64 la = relativeLuminance(a);
    const f64 lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Color Theme::color(std::string_view token) const noexcept {
    const auto it = colors.find(token);
    return it == colors.end() ? Color{1, 0, 1, 1} : it->second;
}

std::vector<std::string_view> Theme::requiredTokens() {
    return {"text",          "textDisabled",   "windowBg",     "childBg",         "popupBg",       "border",
            "frameBg",       "frameBgHovered", "frameBgActive", "titleBg",        "titleBgActive", "menuBarBg",
            "scrollbarBg",   "scrollbarGrab",  "checkMark",    "sliderGrab",      "button",        "buttonHovered",
            "buttonActive",  "header",         "headerHovered", "headerActive",   "separator",     "tab",
            "tabHovered",    "tabSelected",    "tabDimmed",    "tabDimmedSelected", "dockingPreview", "tableHeaderBg",
            "tableRowBgAlt", "textSelectedBg", "navHighlight", "statusBarBg",     "accentLocal",   "accentShared",
            "accentLive",    "badgeServer",    "badgeClient",  "dirty",           "error",         "viewportBg",
            "viewportGrid"};
}

Result<Theme> Theme::parse(std::string_view jsonc, std::string_view sourceName) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(jsonc, sourceName));
    const refl::JsonValue root = doc.root();
    if (!root.isObject()) return Error{ErrorCode::ParseError, std::format("{}: expected an object", sourceName)};
    Theme t;
    t.name = std::string(root.get("name").asString());
    t.highContrast = root.get("highContrast").isBool() && root.get("highContrast").asBool();
    for (const auto& m : root.get("colors").members()) {
        HELIOS_TRY_ASSIGN(const Color c, parseColor(m.value.asString()));
        t.colors.emplace(std::string(m.key), c);
    }
    const refl::JsonValue metrics = root.get("metrics");
    // A token file comes from the user (--theme-file): metrics outside sane DIP ranges (a negative
    // or huge font, padding that swallows the window) are rejected instead of reaching ImGui.
    std::string badMetric;
    const auto metric = [&](std::string_view key, f32& out, f32 lo, f32 hi) {
        const refl::JsonValue v = metrics.get(key);
        if (!v.isValid()) return;
        f32 x = 0;
        if (!v.getF32(x) || !std::isfinite(x) || x < lo || x > hi) {
            if (badMetric.empty()) badMetric = std::format("metric '{}' must be a number in [{}, {}]", key, lo, hi);
            return;
        }
        out = x;
    };
    metric("fontSize", t.metrics.fontSize, 6, 72);
    metric("windowRounding", t.metrics.windowRounding, 0, 24);
    metric("frameRounding", t.metrics.frameRounding, 0, 24);
    metric("tabRounding", t.metrics.tabRounding, 0, 24);
    metric("windowBorderSize", t.metrics.windowBorderSize, 0, 4);
    metric("frameBorderSize", t.metrics.frameBorderSize, 0, 4);
    metric("framePaddingX", t.metrics.framePaddingX, 0, 32);
    metric("framePaddingY", t.metrics.framePaddingY, 0, 32);
    metric("itemSpacingX", t.metrics.itemSpacingX, 0, 32);
    metric("itemSpacingY", t.metrics.itemSpacingY, 0, 32);
    if (!badMetric.empty()) return Error{ErrorCode::ParseError, std::format("{}: {}", sourceName, badMetric)};
    if (t.name.empty()) return Error{ErrorCode::ParseError, std::format("{}: theme without a name", sourceName)};
    for (std::string_view token : requiredTokens()) {
        if (!t.has(token)) return Error{ErrorCode::ParseError, std::format("{}: missing color token '{}'", sourceName, token)};
    }
    return t;
}

std::vector<std::string_view> builtinThemeNames() {
    return {"dark", "light", "high-contrast"};
}

Result<Theme> builtinTheme(std::string_view name) {
    if (name == "dark") return Theme::parse(detail::kThemeDark, "themes/dark.jsonc");
    if (name == "light") return Theme::parse(detail::kThemeLight, "themes/light.jsonc");
    if (name == "high-contrast") return Theme::parse(detail::kThemeHighContrast, "themes/high_contrast.jsonc");
    return Error{ErrorCode::NotFound, std::format("unknown theme '{}'", name)};
}

void addEditorFont(ImGuiIO& io) {
    ImFontConfig cfg;
    // The bytes live in the executable: ImGui must neither free nor copy-and-own them.
    cfg.FontDataOwnedByAtlas = false;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "Roboto-Regular");
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(detail::kRobotoRegular), static_cast<int>(detail::kRobotoRegularSize),
                                   0.0f, &cfg);
}

void applyTheme(const Theme& theme, f32 scale, ImGuiStyle& style) {
    appliedTheme() = theme;
    style = ImGuiStyle();
    const ThemeMetrics& m = theme.metrics;
    style.WindowRounding = m.windowRounding;
    style.ChildRounding = m.windowRounding;
    style.PopupRounding = m.windowRounding;
    style.FrameRounding = m.frameRounding;
    style.GrabRounding = m.frameRounding;
    style.ScrollbarRounding = m.frameRounding;
    style.TabRounding = m.tabRounding;
    style.WindowBorderSize = m.windowBorderSize;
    style.ChildBorderSize = m.windowBorderSize;
    style.PopupBorderSize = m.windowBorderSize;
    style.FrameBorderSize = m.frameBorderSize;
    style.FramePadding = ImVec2(m.framePaddingX, m.framePaddingY);
    style.ItemSpacing = ImVec2(m.itemSpacingX, m.itemSpacingY);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    // Deterministic, calm UI: tooltips follow the delays set by the host, no alpha fades.
    style.ScaleAllSizes(scale);
    style.FontSizeBase = m.fontSize;
    style.FontScaleDpi = scale;

    const auto set = [&](ImGuiCol col, std::string_view token) {
        const Color c = theme.color(token);
        style.Colors[col] = ImVec4(c.r, c.g, c.b, c.a);
    };
    set(ImGuiCol_Text, "text");
    set(ImGuiCol_TextDisabled, "textDisabled");
    set(ImGuiCol_WindowBg, "windowBg");
    set(ImGuiCol_ChildBg, "childBg");
    set(ImGuiCol_PopupBg, "popupBg");
    set(ImGuiCol_Border, "border");
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    set(ImGuiCol_FrameBg, "frameBg");
    set(ImGuiCol_FrameBgHovered, "frameBgHovered");
    set(ImGuiCol_FrameBgActive, "frameBgActive");
    set(ImGuiCol_TitleBg, "titleBg");
    set(ImGuiCol_TitleBgActive, "titleBgActive");
    set(ImGuiCol_TitleBgCollapsed, "titleBg");
    set(ImGuiCol_MenuBarBg, "menuBarBg");
    set(ImGuiCol_ScrollbarBg, "scrollbarBg");
    set(ImGuiCol_ScrollbarGrab, "scrollbarGrab");
    set(ImGuiCol_ScrollbarGrabHovered, "sliderGrab");
    set(ImGuiCol_ScrollbarGrabActive, "sliderGrab");
    set(ImGuiCol_CheckMark, "checkMark");
    set(ImGuiCol_SliderGrab, "sliderGrab");
    set(ImGuiCol_SliderGrabActive, "sliderGrab");
    set(ImGuiCol_Button, "button");
    set(ImGuiCol_ButtonHovered, "buttonHovered");
    set(ImGuiCol_ButtonActive, "buttonActive");
    set(ImGuiCol_Header, "header");
    set(ImGuiCol_HeaderHovered, "headerHovered");
    set(ImGuiCol_HeaderActive, "headerActive");
    set(ImGuiCol_Separator, "separator");
    set(ImGuiCol_SeparatorHovered, "navHighlight");
    set(ImGuiCol_SeparatorActive, "navHighlight");
    set(ImGuiCol_ResizeGrip, "separator");
    set(ImGuiCol_ResizeGripHovered, "navHighlight");
    set(ImGuiCol_ResizeGripActive, "navHighlight");
    set(ImGuiCol_Tab, "tab");
    set(ImGuiCol_TabHovered, "tabHovered");
    set(ImGuiCol_TabSelected, "tabSelected");
    set(ImGuiCol_TabSelectedOverline, "navHighlight");
    set(ImGuiCol_TabDimmed, "tabDimmed");
    set(ImGuiCol_TabDimmedSelected, "tabDimmedSelected");
    set(ImGuiCol_TabDimmedSelectedOverline, "separator");
    set(ImGuiCol_DockingPreview, "dockingPreview");
    set(ImGuiCol_DockingEmptyBg, "windowBg");
    set(ImGuiCol_TableHeaderBg, "tableHeaderBg");
    set(ImGuiCol_TableBorderStrong, "border");
    set(ImGuiCol_TableBorderLight, "separator");
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    set(ImGuiCol_TableRowBgAlt, "tableRowBgAlt");
    set(ImGuiCol_TextSelectedBg, "textSelectedBg");
    set(ImGuiCol_DragDropTarget, "navHighlight");
    set(ImGuiCol_NavCursor, "navHighlight");
    set(ImGuiCol_NavWindowingHighlight, "navHighlight");
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.35f);
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.35f);
    set(ImGuiCol_PlotLines, "checkMark");
    set(ImGuiCol_PlotLinesHovered, "navHighlight");
    set(ImGuiCol_PlotHistogram, "checkMark");
    set(ImGuiCol_PlotHistogramHovered, "navHighlight");
    set(ImGuiCol_TextLink, "checkMark");
    if (theme.highContrast) {
        // Focus and selection are visible without color: thick outlines.
        style.FrameBorderSize = std::max(style.FrameBorderSize, 1.0f * scale);
        style.TabBorderSize = 1.0f * scale;
    }
}

Color semanticColor(std::string_view token) noexcept {
    return appliedTheme().color(token);
}

std::vector<ContrastPair> drawnContrastPairs() {
    std::vector<ContrastPair> pairs;
    // Body text: panels, popups, input fields, menus, buttons, selected and hovered rows (headers),
    // tabs, table headers and the alternate table rows, and the status bar.
    for (std::string_view bg : {"windowBg", "childBg", "popupBg", "frameBg", "frameBgHovered", "frameBgActive", "titleBg",
                                "titleBgActive", "menuBarBg", "button", "buttonHovered", "buttonActive", "header", "headerHovered",
                                "headerActive", "tab", "tabHovered", "tabSelected", "tabDimmed", "tabDimmedSelected", "tableHeaderBg",
                                "statusBarBg"}) {
        pairs.push_back({"text", bg});
    }
    pairs.push_back({"text", "tableRowBgAlt"});
    pairs.push_back({"text", "headerHovered", "popupBg"});  // the palette's hovered command
    // Disabled text: notes and counts on panels and popups, input hints (InputTextWithHint draws
    // them in TextDisabled on the frame), the Documents table's "Table" cell on selected, hovered
    // and pressed rows and on alternate rows, and the palette's shortcuts on a hovered command.
    for (std::string_view bg : {"windowBg", "childBg", "popupBg", "frameBg", "frameBgHovered", "frameBgActive", "header",
                                "headerHovered", "headerActive", "tableRowBgAlt"}) {
        pairs.push_back({"textDisabled", bg});
    }
    pairs.push_back({"textDisabled", "headerHovered", "popupBg"});
    // Semantic text: the property grid's badges (on plain and alternate rows), its dirty marker
    // and errors, and the status bar's unsaved count.
    for (std::string_view fg : {"badgeServer", "badgeClient", "error"}) {
        pairs.push_back({fg, "windowBg"});
        pairs.push_back({fg, "tableRowBgAlt"});
    }
    pairs.push_back({"dirty", "windowBg"});
    pairs.push_back({"dirty", "statusBarBg"});
    return pairs;
}

std::vector<ContrastIssue> checkContrast(const Theme& theme) {
    std::vector<ContrastIssue> issues;
    const f64 required = theme.highContrast ? 7.0 : 4.5;
    const Color base = theme.color("windowBg");
    for (const ContrastPair& p : drawnContrastPairs()) {
        const Color under = p.under.empty() ? base : over(theme.color(p.under), base);
        const Color background = over(theme.color(p.background), under);
        const f64 ratio = contrastRatio(over(theme.color(p.foreground), background), background);
        if (ratio + 1e-9 < required) {
            std::string bg(p.background);
            if (!p.under.empty()) bg += " over " + std::string(p.under);
            issues.push_back({std::string(p.foreground), std::move(bg), ratio, required});
        }
    }
    return issues;
}

} // namespace helios::edui
