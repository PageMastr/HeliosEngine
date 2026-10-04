// Layout lints (07 §4.4) against seeded violations, one rule at a time, and a clean control.

#include <doctest/doctest.h>

#include <functional>
#include <string>

#include "edui_test_util.h"
#include "helios/toolsfw/command.h"
#include "helios/toolsfw/json_util.h"

using namespace helios;
using namespace helios::edui;
using namespace helios::edui::test;

namespace {

/// Runs `body` inside a fixed-size window "Probe" for a few frames and returns the lint issues.
std::vector<LintIssue> lintOf(const std::function<void()>& body, UiTest::LintOptions options = {},
                              ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) {
    HeadlessImGui imgui(800, 600);
    UiTest ui(imgui.context(), [](const UiInputEvent&) {});
    ui.setEnabled(true);
    for (int i = 0; i < 3; ++i) {
        ui.beginFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowSize(ImVec2(300, 200));
        if (ImGui::Begin("Probe", nullptr, flags)) body();
        ImGui::End();
        ImGui::Render();
        ui.endFrame();
        HeadlessImGui::acknowledgeTextures();
    }
    return ui.lint(options);
}

bool has(const std::vector<LintIssue>& issues, std::string_view rule, std::string_view path = {}) {
    for (const LintIssue& i : issues) {
        if (i.rule == rule && (path.empty() || i.path == path)) return true;
    }
    return false;
}

TEST_CASE("lint: a clean window has no issues") {
    const auto issues = lintOf([] {
        ImGui::Button("Apply##apply");
        ImGui::SameLine();
        ImGui::Button("Cancel##cancel");
        static bool flag = true;
        ImGui::Checkbox("Enabled##enabled", &flag);
    });
    for (const LintIssue& i : issues) FAIL_CHECK(i.rule << " " << i.path << ": " << i.message);
}

TEST_CASE("lint: clipped label") {
    const auto issues = lintOf([] { ImGui::Button("A label far wider than its button##b", ImVec2(40, 0)); });
    CHECK(has(issues, "clipped", "Probe/b"));
}

TEST_CASE("lint: overlapping interactive items") {
    const auto issues = lintOf([] {
        ImGui::SetCursorPos(ImVec2(20, 40));
        ImGui::Button("One##one", ImVec2(100, 30));
        ImGui::SetCursorPos(ImVec2(60, 50));
        ImGui::Button("Two##two", ImVec2(100, 30));
    });
    CHECK(has(issues, "overlap", "Probe/one"));
}

TEST_CASE("lint: item outside a window that cannot scroll") {
    const auto issues = lintOf([] {
        ImGui::SetCursorPos(ImVec2(250, 40));
        ImGui::Button("Far##far", ImVec2(120, 0));
    });
    CHECK(has(issues, "outside", "Probe/far"));
    // The same item in a window that scrolls horizontally is fine.
    const auto scrolling = lintOf(
        [] {
            ImGui::SetCursorPos(ImVec2(250, 40));
            ImGui::Button("Far##far", ImVec2(120, 0));
        },
        {}, ImGuiWindowFlags_HorizontalScrollbar);
    CHECK_FALSE(has(scrolling, "outside"));
}

TEST_CASE("lint: interactive item without label or tooltip") {
    const auto issues = lintOf([] {
        ImGui::Button("##nolabel", ImVec2(30, 0));
        ImGui::Button("##annotated", ImVec2(30, 0));
        UiTest::annotate({}, "Explains itself");
    });
    CHECK(has(issues, "unlabeled", "Probe/nolabel"));
    CHECK_FALSE(has(issues, "unlabeled", "Probe/annotated"));
}

TEST_CASE("lint: low-contrast theme tokens") {
    auto t = builtinTheme("dark");
    REQUIRE(t);
    Theme bad = *t;
    bad.colors["text"] = bad.colors["frameBg"];
    UiTest::LintOptions o;
    o.theme = &bad;
    const auto issues = lintOf([] { ImGui::Button("Ok##ok"); }, o);
    CHECK(has(issues, "contrast"));
    o.theme = &*t;
    CHECK_FALSE(has(lintOf([] { ImGui::Button("Ok##ok"); }, o), "contrast"));
}

TEST_CASE("lint: a command that no menu exposes") {
    tf::CommandBus bus;
    const auto add = [&](std::string id, bool paletteOnly) {
        tf::CommandDesc c;
        c.id = std::move(id);
        c.label = c.id;
        c.paletteOnly = paletteOnly;
        c.execute = [](tf::CommandContext&) -> Result<void> { return {}; };
        REQUIRE(bus.add(std::move(c)));
    };
    add("probe.hidden", false);
    add("probe.menu", false);
    add("probe.palette", true);
    bus.markExposed("probe.menu", "MainMenu/Probe");
    UiTest::LintOptions o;
    o.commands = &bus;
    const auto issues = lintOf([] { ImGui::Button("Ok##ok"); }, o);
    CHECK(has(issues, "unexposed", "command/probe.hidden"));
    CHECK_FALSE(has(issues, "unexposed", "command/probe.menu"));
    CHECK_FALSE(has(issues, "unexposed", "command/probe.palette"));
}

TEST_CASE("lint: panels keyboard focus cycling cannot reach") {
    UiTest::LintOptions o;
    o.panels = {"Probe", "Missing"};
    const auto reachable = lintOf([] { ImGui::Button("Ok##ok"); }, o);
    CHECK_FALSE(has(reachable, "unreachable", "Probe"));
    CHECK(has(reachable, "unreachable", "Missing"));
    const auto unreachable = lintOf([] { ImGui::Button("Ok##ok"); }, o, ImGuiWindowFlags_NoNavFocus);
    CHECK(has(unreachable, "unreachable", "Probe"));
}

TEST_CASE("lint: issues serialize to JSON") {
    const std::vector<LintIssue> issues = {{"clipped", "Probe/b", "label \"x\" is wide"}};
    auto doc = refl::JsonDocument::parse(lintIssuesJson(issues));
    REQUIRE(doc);
    REQUIRE(doc->root().size() == 1);
    const refl::JsonValue first = *doc->root().elements().begin();
    CHECK(tf::json::getString(first, "rule").value_or("") == "clipped");
    CHECK(tf::json::getString(first, "message").value_or("") == "label \"x\" is wide");
}

} // namespace
