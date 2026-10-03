// Themes and the contrast lint, pseudo-localization, key chords.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "helios/core/fs.h"
#include "helios/editorui/localize.h"
#include "helios/editorui/theme.h"
#include "helios/editorui/ui_test.h"
#include "imgui.h"

#include "embedded_font.h"

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

TEST_CASE("theme: the editor font is the embedded Roboto 2.138 Regular, byte for byte") {
    auto file = fs::readFile(fs::pathFromUtf8(HELIOS_EDUI_FONT_FILE));
    REQUIRE(file);
    REQUIRE(file->size() == edui::detail::kRobotoRegularSize);
    CHECK(std::memcmp(file->data(), edui::detail::kRobotoRegular, file->size()) == 0);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    addEditorFont(io);
    REQUIRE(io.Fonts->Sources.Size == 1);
    CHECK(std::string_view(io.Fonts->Sources[0].Name) == "Roboto-Regular");
    CHECK_FALSE(io.Fonts->Sources[0].FontDataOwnedByAtlas);  // the bytes are static
    CHECK(io.Fonts->Sources[0].FontData == edui::detail::kRobotoRegular);
    ImGui::DestroyContext(ctx);
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
