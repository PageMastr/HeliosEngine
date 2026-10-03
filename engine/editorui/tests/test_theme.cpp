// Themes and the contrast lint, pseudo-localization, key chords.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "helios/core/fs.h"
#include "helios/editorui/localize.h"
#include "helios/editorui/theme.h"
#include "helios/editorui/ui_test.h"
#include "imgui.h"

using namespace helios;
using namespace helios::edui;

namespace {

TEST_CASE("theme: colors parse and pack") {
    auto c = parseColor("#FF8000");
    REQUIRE(c);
    CHECK(c->r == doctest::Approx(1.0));
    CHECK(c->g == doctest::Approx(128.0 / 255.0));
    CHECK(c->b == doctest::Approx(0.0));
    CHECK(c->a == doctest::Approx(1.0));
    auto a = parseColor("#10203040");
    REQUIRE(a);
    CHECK(packColor(*a) == 0x40302010u);
    CHECK_FALSE(parseColor("FF8000"));
    CHECK_FALSE(parseColor("#FF80"));
    CHECK_FALSE(parseColor("#GG8000"));
}

TEST_CASE("theme: WCAG luminance and contrast") {
    const Color black{0, 0, 0, 1};
    const Color white{1, 1, 1, 1};
    CHECK(relativeLuminance(black) == doctest::Approx(0.0));
    CHECK(relativeLuminance(white) == doctest::Approx(1.0));
    CHECK(contrastRatio(black, white) == doctest::Approx(21.0));
    CHECK(contrastRatio(white, black) == doctest::Approx(21.0));
    // #777777 on white is the textbook 4.48:1 (just below AA).
    auto grey = parseColor("#777777");
    REQUIRE(grey);
    CHECK(contrastRatio(*grey, white) == doctest::Approx(4.48).epsilon(0.01));
    // Half-transparent white over black is mid grey.
    const Color mixed = over(Color{1, 1, 1, 0.5f}, black);
    CHECK(mixed.r == doctest::Approx(0.5));
    CHECK(mixed.a == doctest::Approx(1.0));
}

TEST_CASE("theme: built-in themes parse, carry every token and pass the contrast lint") {
    const auto names = builtinThemeNames();
    REQUIRE(names.size() == 3);
    for (std::string_view name : names) {
        CAPTURE(name);
        auto t = builtinTheme(name);
        REQUIRE(t);
        CHECK(t->name == name);
        for (std::string_view token : Theme::requiredTokens()) CHECK(t->has(token));
        const auto issues = checkContrast(*t);
        for (const ContrastIssue& i : issues) {
            FAIL_CHECK(std::string(name) << ": " << i.foreground << " on " << i.background << " is " << i.ratio << ":1 < " << i.required);
        }
    }
    auto hc = builtinTheme("high-contrast");
    REQUIRE(hc);
    CHECK(hc->highContrast);
    CHECK_FALSE(builtinTheme("sepia"));
}

TEST_CASE("theme: embedded copies match the token files") {
    const fs::Path dir = fs::pathFromUtf8(HELIOS_EDUI_THEME_DIR);
    const std::pair<const char*, const char*> files[] = {{"dark", "dark.jsonc"}, {"light", "light.jsonc"}, {"high-contrast", "high_contrast.jsonc"}};
    for (const auto& [name, file] : files) {
        CAPTURE(name);
        auto text = fs::readTextFile(dir / file);
        REQUIRE(text);
        auto fromFile = Theme::parse(*text, file);
        REQUIRE(fromFile);
        auto builtin = builtinTheme(name);
        REQUIRE(builtin);
        CHECK(fromFile->colors == builtin->colors);
        CHECK(fromFile->metrics.fontSize == builtin->metrics.fontSize);
    }
}

TEST_CASE("theme: the contrast lint catches a low-contrast token pair") {
    auto t = builtinTheme("dark");
    REQUIRE(t);
    Theme bad = *t;
    bad.colors["text"] = bad.colors["windowBg"];
    const auto issues = checkContrast(bad);
    REQUIRE_FALSE(issues.empty());
    CHECK(issues.front().foreground == "text");
    CHECK(issues.front().ratio < 4.5);
    // The same colours pass AA but fail the high-contrast 7:1 requirement.
    Theme mid = *t;
    mid.highContrast = true;
    mid.colors["text"] = *parseColor("#9A9A9A");
    mid.colors["windowBg"] = *parseColor("#1E1E1E");
    bool sevenFails = false;
    for (const ContrastIssue& i : checkContrast(mid)) sevenFails = sevenFails || (i.foreground == "text" && i.required == 7.0);
    CHECK(sevenFails);
}

TEST_CASE("theme: the contrast lint checks the pairs the shell draws") {
    auto t = builtinTheme("dark");
    REQUIRE(t);
    const auto has = [](const std::vector<ContrastIssue>& issues, std::string_view fg, std::string_view bg) {
        return std::any_of(issues.begin(), issues.end(), [&](const ContrastIssue& i) { return i.foreground == fg && i.background == bg; });
    };
    // Round 1's dark values: disabled text on the selected Documents row (3.85:1) and input hints
    // on a frame (4.35:1) are drawn, so the lint must see them.
    Theme old = *t;
    old.colors["textDisabled"] = *parseColor("#8C939D");
    const auto issues = checkContrast(old);
    CHECK(has(issues, "textDisabled", "header"));
    CHECK(has(issues, "textDisabled", "frameBg"));
    CHECK(has(issues, "textDisabled", "headerHovered over popupBg"));
    // Semantic tokens are checked where they are drawn, and drawn from the applied theme.
    Theme dim = *t;
    dim.colors["dirty"] = dim.colors["statusBarBg"];
    CHECK(has(checkContrast(dim), "dirty", "statusBarBg"));
    Theme badge = *t;
    badge.colors["badgeServer"] = badge.colors["windowBg"];
    CHECK(has(checkContrast(badge), "badgeServer", "tableRowBgAlt"));
    // The Viewport panel's note is drawn on the cleared viewport image.
    Theme viewport = *t;
    viewport.colors["viewportBg"] = viewport.colors["textDisabled"];
    CHECK(has(checkContrast(viewport), "textDisabled", "viewportBg"));
    for (const ContrastPair& p : drawnContrastPairs()) {
        CAPTURE(p.foreground);
        CHECK(t->has(p.foreground));
        CHECK(t->has(p.background));
    }
    ImGuiStyle style;
    applyTheme(*t, 1.0f, style);
    for (std::string_view token : {"badgeServer", "badgeClient", "dirty", "error", "statusBarBg", "accentLocal"}) {
        CAPTURE(token);
        CHECK(semanticColor(token) == t->color(token));
    }
    CHECK_FALSE(semanticColor("badgeServer") == semanticColor("badgeClient"));
}

TEST_CASE("theme: badges on hovered and pressed rows, and selected text, reach the contrast floor") {
    // Computed independently of drawnContrastPairs(), so a pair dropped from the lint still fails here.
    for (const char* name : {"dark", "light", "high-contrast"}) {
        auto t = builtinTheme(name);
        REQUIRE(t);
        const f64 required = t->highContrast ? 7.0 : 4.5;
        const Color base = t->color("windowBg");
        const auto ratio = [&](std::string_view fg, std::string_view bg, std::string_view under) {
            const Color b = over(t->color(bg), over(t->color(under), base));
            return contrastRatio(over(t->color(fg), b), b);
        };
        CAPTURE(std::string(name));
        for (std::string_view badge : {"badgeServer", "badgeClient"}) {
            CAPTURE(badge);
            CHECK(ratio(badge, "headerHovered", "windowBg") >= required);  // light before: 4.25, 4.48
            CHECK(ratio(badge, "headerActive", "windowBg") >= required);   // light before: 4.04, 4.27
        }
        CHECK(ratio("text", "textSelectedBg", "frameBg") >= required);     // high contrast before: 6.78
    }
}

TEST_CASE("theme: the contrast lint sees badges on a hovered name cell and selected text") {
    const auto has = [](const std::vector<ContrastIssue>& issues, std::string_view fg, std::string_view bg) {
        return std::any_of(issues.begin(), issues.end(), [&](const ContrastIssue& i) { return i.foreground == fg && i.background == bg; });
    };
    // Round 2's light badges and high-contrast selection fail pairs the editor draws.
    auto light = builtinTheme("light");
    REQUIRE(light);
    Theme oldLight = *light;
    oldLight.colors["badgeServer"] = *parseColor("#9C4A00");
    oldLight.colors["badgeClient"] = *parseColor("#256B2A");
    const auto lightIssues = checkContrast(oldLight);
    CHECK(has(lightIssues, "badgeServer", "headerHovered"));
    CHECK(has(lightIssues, "badgeServer", "headerActive"));
    CHECK(has(lightIssues, "badgeClient", "headerActive"));
    auto hc = builtinTheme("high-contrast");
    REQUIRE(hc);
    Theme oldHc = *hc;
    oldHc.colors["textSelectedBg"] = *parseColor("#FFE60066");
    CHECK(has(checkContrast(oldHc), "text", "textSelectedBg over frameBg"));
}

TEST_CASE("theme: metrics outside their ranges are rejected") {
    auto dark = builtinTheme("dark");
    REQUIRE(dark);
    std::string colors;
    for (const auto& [name, c] : dark->colors) {
        colors += std::format("{}\"{}\": \"#{:02X}{:02X}{:02X}\"", colors.empty() ? "" : ", ", name, packColor(c) & 0xFF,
                              (packColor(c) >> 8) & 0xFF, (packColor(c) >> 16) & 0xFF);
    }
    const auto parse = [&](std::string_view metrics) {
        return Theme::parse(std::format(R"({{"name": "x", "colors": {{{}}}, "metrics": {{{}}}}})", colors, metrics), "x.jsonc");
    };
    CHECK(parse(R"("fontSize": 16)"));
    for (const char* bad : {R"("fontSize": -1)", R"("fontSize": 4000)", R"("framePaddingX": -3)", R"("windowBorderSize": 100)",
                            R"("itemSpacingY": "wide")"}) {
        CAPTURE(bad);
        auto r = parse(bad);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("metric") != std::string::npos);
    }
}

TEST_CASE("theme: a token file without every token is rejected") {
    auto r = Theme::parse(R"({"name": "partial", "colors": {"text": "#FFFFFF"}})", "partial.jsonc");
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("missing color token") != std::string::npos);
}

TEST_CASE("theme: applyTheme scales metrics and fonts") {
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    auto t = builtinTheme("dark");
    REQUIRE(t);
    ImGuiStyle one;
    applyTheme(*t, 1.0f, one);
    ImGuiStyle two;
    applyTheme(*t, 2.0f, two);
    CHECK(two.FramePadding.x == doctest::Approx(one.FramePadding.x * 2));
    CHECK(two.ItemSpacing.y == doctest::Approx(one.ItemSpacing.y * 2));
    CHECK(two.FontScaleDpi == doctest::Approx(2.0));
    CHECK(one.FontSizeBase == doctest::Approx(t->metrics.fontSize));
    // Applying twice does not compound the scale.
    applyTheme(*t, 2.0f, two);
    CHECK(two.FramePadding.x == doctest::Approx(one.FramePadding.x * 2));
    const Color text = t->color("text");
    CHECK(two.Colors[ImGuiCol_Text].x == doctest::Approx(text.r));
    ImGui::DestroyContext(ctx);
}

TEST_CASE("theme: applyTheme takes every ImGui color slot from the theme") {
    // Dear ImGui 1.92 added InputTextCursor, CheckboxSelectedBg, TreeLines, DragDropTargetBg and
    // UnsavedMarker; applyTheme() left them at ImGuiStyle()'s dark defaults, which match no token.
    // Every slot must be a token color of the theme or one of the two fixed overlays (transparent,
    // and the 35 % black dimming); an unset slot is now magenta, which matches neither.
    const Color clear{0, 0, 0, 0};
    const Color dim{0, 0, 0, 0.35f};
    for (std::string_view name : builtinThemeNames()) {
        auto t = builtinTheme(name);
        REQUIRE(t);
        CHECK_FALSE(std::any_of(t->colors.begin(), t->colors.end(), [](const auto& kv) { return kv.second == Color{1, 0, 1, 1}; }));
        ImGuiStyle style;
        applyTheme(*t, 1.0f, style);
        CAPTURE(std::string(name));
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            const ImVec4& v = style.Colors[i];
            const Color c{v.x, v.y, v.z, v.w};
            const bool token = std::any_of(t->colors.begin(), t->colors.end(), [&](const auto& kv) { return kv.second == c; });
            const std::string slot = ImGui::GetStyleColorName(i);
            CAPTURE(slot);
            CHECK((token || c == clear || c == dim));
        }
    }
}

TEST_CASE("theme: the text caret and a checked box's check mark are visible in every theme") {
    // WCAG 2.2 SC 1.4.11 (non-text contrast): at least 3:1 against the adjacent color. Before the
    // fix the light theme's caret was ImGui's dark default, white on its white fields (1.00:1).
    for (std::string_view name : builtinThemeNames()) {
        auto t = builtinTheme(name);
        REQUIRE(t);
        ImGuiStyle style;
        applyTheme(*t, 1.0f, style);
        const auto color = [&](ImGuiCol col) {
            const ImVec4& v = style.Colors[col];
            return Color{v.x, v.y, v.z, v.w};
        };
        const Color window = color(ImGuiCol_WindowBg);
        const Color field = over(color(ImGuiCol_FrameBg), window);
        const Color checked = over(color(ImGuiCol_CheckboxSelectedBg), window);
        CAPTURE(std::string(name));
        CHECK(contrastRatio(over(color(ImGuiCol_InputTextCursor), field), field) >= 3.0);
        CHECK(contrastRatio(over(color(ImGuiCol_CheckMark), checked), checked) >= 3.0);
    }
}

TEST_CASE("localize: pseudo-localization grows text and keeps ids") {
    const std::string p = pseudoLocalize("Save All##doc.saveAll");
    CHECK(p.ends_with("##doc.saveAll"));
    CHECK(p.find("Save") == std::string::npos);  // accented
    const std::string visible = p.substr(0, p.find("##"));
    CHECK(visible.front() == '[');
    CHECK(visible.back() == ']');
    const std::string q = pseudoLocalize("Inspector###Inspector");
    CHECK(q.ends_with("###Inspector"));
    // tr() is the identity until pseudo-localization is switched on.
    setPseudoLocalization(false);
    const char* plain = "Undo##undo";
    CHECK(tr(plain) == plain);
    setPseudoLocalization(true);
    CHECK(std::string(tr(plain)) == pseudoLocalize(plain));
    setPseudoLocalization(false);
}

TEST_CASE("localize: pseudo-localized text is at least 40 % longer on screen") {
    // Measured in code points: accented letters are multi-byte in UTF-8.
    const auto codepoints = [](const std::string& s) {
        usize n = 0;
        for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? 1 : 0;
        return n;
    };
    for (const char* text : {"File", "Record", "Command Palette", "Delete Record"}) {
        CAPTURE(text);
        const std::string p = pseudoLocalize(text);
        CHECK(static_cast<f64>(codepoints(p)) >= std::ceil(static_cast<f64>(std::string(text).size()) * 1.4));
    }
}

TEST_CASE("ui chords parse") {
    auto c = parseChord("Ctrl+Shift+P");
    REQUIRE(c);
    CHECK(c->first == (kModCtrl | kModShift));
    CHECK(c->second == "p");
    auto esc = parseChord("esc");
    REQUIRE(esc);
    CHECK(esc->first == 0);
    CHECK(esc->second == "escape");
    auto f2 = parseChord("F2");
    REQUIRE(f2);
    CHECK(f2->second == "f2");
    auto alone = parseChord("ctrl");
    REQUIRE(alone);
    CHECK(alone->second == "ctrl");
    CHECK_FALSE(parseChord("ctrl+"));
    CHECK_FALSE(parseChord("ctrl+hyper"));
    CHECK_FALSE(parseChord(""));
    for (std::string_view k : uiKeyNames()) CHECK(parseChord(k));
}

} // namespace
