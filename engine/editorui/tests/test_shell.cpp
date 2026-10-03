// The shell and the property grid on a headless ImGui context, driven through the UI test
// harness: item table paths, input injection, edits as transactions, commands, menus and lints.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "edui_test_util.h"
#include "helios/editorui/localize.h"
#include "helios/toolsfw/json_util.h"

using namespace helios;
using namespace helios::edui;
using namespace helios::edui::test;

namespace {

auto click(ShellHarness& h, std::string path) {
    return h.act([&](UiTest::Completion done) { h.ui->click(path, 0, std::move(done)); });
}
auto key(ShellHarness& h, std::string chord, u32 repeat = 1) {
    return h.act([&](UiTest::Completion done) { h.ui->key(chord, repeat, std::move(done)); });
}
auto typeText(ShellHarness& h, std::string text) {
    return h.act([&](UiTest::Completion done) { h.ui->type(text, std::move(done)); });
}

constexpr const char* kFrigateRow = "Documents/docs/list/row[hull/frigate]/select";

TEST_CASE("shell: the item table has stable paths for menus, panels and widgets") {
    ShellHarness h("shell_items");
    const std::pair<const char*, const char*> expected[] = {
        {"MainMenu/File", "menu"},   {"MainMenu/Edit", "menu"},     {"MainMenu/View", "menu"},
        {"Documents/filter", "input"}, {kFrigateRow, "item"},        {"Documents/docs/list/row[itm/scrap_plate]/select", "item"},
        {"History/undo", "item"},    {"History/redo", "item"},      {"Viewport/viewport", "viewport"},
        {"StatusBar", "window"},     {"Inspector", "window"},
    };
    for (const auto& [path, kind] : expected) {
        CAPTURE(path);
        const UiItem* item = h.ui->find(path);
        REQUIRE(item != nullptr);
        CHECK(item->kind == kind);
    }
    CHECK_FALSE(h.ui->find("History/undo")->enabled);
    // "**/" finds a unique suffix; ambiguous suffixes find nothing.
    CHECK(h.ui->find("**/row[hull/frigate]/select") != nullptr);
    CHECK(h.ui->find("**/select") == nullptr);
    // The viewport asked the host for a texture of its size.
    CHECK(h.host->viewportSize.first > 100);
    CHECK(h.host->viewportSize.second > 100);
    // ImGui's implicit "Debug" window never shows up.
    for (const UiItem& i : h.ui->items()) CHECK_FALSE(i.window.starts_with("Default"));
    // JSON export parses and carries the same paths.
    auto doc = refl::JsonDocument::parse(h.ui->itemsJson("Documents"));
    REQUIRE(doc);
    bool found = false;
    for (refl::JsonValue i : doc->root().elements()) found = found || tf::json::getString(i, "path").value_or("") == kFrigateRow;
    CHECK(found);
}

TEST_CASE("shell: the harness costs nothing when disabled") {
    ShellHarness h("shell_disabled");
    REQUIRE_FALSE(h.ui->items().empty());
    h.ui->setEnabled(false);
    CHECK(h.imgui->context()->TestEngine == nullptr);
    CHECK_FALSE(h.imgui->context()->TestEngineHookItems);
    const usize before = h.ui->items().size();
    h.frame();
    CHECK(h.ui->items().size() == before);  // not rebuilt
    h.ui->setEnabled(true);
    h.frame();
    CHECK(h.ui->items().size() == before);
}

TEST_CASE("shell: selecting a record shows its property grid") {
    ShellHarness h("shell_select");
    CHECK(h.ui->find("**/row[mass]/value") == nullptr);
    REQUIRE(click(h, kFrigateRow));
    const tf::Document* selected = h.fw->documents().find(h.fw->selection().primaryDocument());
    REQUIRE(selected != nullptr);
    CHECK(selected->name() == "hull/frigate");
    const UiItem* mass = h.ui->find("Inspector/grid/Grid/row[mass]/value");
    REQUIRE(mass != nullptr);
    CHECK(mass->value == "12000");
    CHECK(mass->kind == "input");
    CHECK(h.ui->find("Inspector/grid/Grid/row[handling]/row[handling/yawRate]/value") != nullptr);
    CHECK(h.ui->find("Inspector/grid/Grid/row[capabilities]/Fly")->value == "true");
    CHECK(h.ui->find("Inspector/Grid/recordName")->value == "hull/frigate");
}

TEST_CASE("shell: typed edits are ui-scripted transactions; Ctrl+Z undoes them") {
    ShellHarness h("shell_edit");
    REQUIRE(click(h, kFrigateRow));
    const std::string original = h.frigate().text();
    REQUIRE(h.act([&](UiTest::Completion d) { h.ui->doubleClick("Inspector/grid/Grid/row[mass]/value", std::move(d)); }));
    REQUIRE(key(h, "ctrl+a"));
    REQUIRE(typeText(h, "13000"));
    REQUIRE(key(h, "enter"));
    CHECK(h.get("mass") == "13000");
    REQUIRE_FALSE(h.fw->history().empty());
    CHECK(h.fw->history().back().tx.origin == tf::Origin::UiScripted);
    CHECK(h.fw->history().size() == 1);
    CHECK(h.ui->find("History/undo")->enabled);
    // Ctrl+Z goes through the shell's shortcut routing to edit.undo.
    REQUIRE(h.act([&](UiTest::Completion d) { h.ui->moveTo(1, 1, std::move(d)); }));
    REQUIRE(key(h, "ctrl+z"));
    CHECK(h.frigate().text() == original);
    CHECK_FALSE(h.frigate().dirty());
    REQUIRE(key(h, "ctrl+y"));
    CHECK(h.get("mass") == "13000");
}

TEST_CASE("shell: a drag is one undo step (merged gesture)") {
    ShellHarness h("shell_drag");
    REQUIRE(click(h, kFrigateRow));
    const std::string before = h.get("handling/yawRate");
    REQUIRE(h.act([&](UiTest::Completion d) {
        h.ui->drag("Inspector/grid/Grid/row[handling]/row[handling/yawRate]/value", "", 40, 0, std::move(d));
    }));
    CHECK(h.get("handling/yawRate") != before);
    // Several frames of dragging, one history entry.
    CHECK(h.fw->history().size() == 1);
    CHECK(h.fw->log().size() > 1);
    REQUIRE(h.fw->undo(tf::Origin::UiScripted));
    CHECK(h.get("handling/yawRate") == before);
}

TEST_CASE("shell: check boxes, list buttons and the History panel edit through commands") {
    ShellHarness h("shell_widgets");
    REQUIRE(click(h, kFrigateRow));
    const std::string original = h.frigate().text();
    REQUIRE(click(h, "Inspector/grid/Grid/row[capabilities]/Mine"));
    CHECK(h.get("capabilities").find("Mine") != std::string::npos);
    REQUIRE(click(h, "Inspector/grid/Grid/row[hardpoints]/add"));
    CHECK(h.frigate().text().find("\"slot\"") != std::string::npos);
    auto hp = refl::getJson(h.frigate().type(), h.frigate().object(), "hardpoints");
    REQUIRE(hp);
    auto parsed = refl::JsonDocument::parse(*hp);
    REQUIRE(parsed);
    CHECK(parsed->root().size() == 3);
    REQUIRE(h.fw->history().size() == 2);
    for (const tf::HistoryEntry& e : h.fw->history()) CHECK(e.tx.origin == tf::Origin::UiScripted);
    REQUIRE(click(h, "History/undo"));
    REQUIRE(click(h, "History/undo"));
    CHECK(h.frigate().text() == original);
    CHECK_FALSE(h.ui->find("History/undo")->enabled);
    CHECK(h.ui->find("History/redo")->enabled);
}

TEST_CASE("shell: an edit made elsewhere shows up in the grid on the next frame") {
    ShellHarness h("shell_external");
    REQUIRE(click(h, kFrigateRow));
    auto r = h.fw->invoker(tf::Origin::Rpc).invoke("doc.setProperty", R"({"doc":"hull/frigate","path":"mass","value":9000})");
    REQUIRE(r);
    h.frame();
    CHECK(h.ui->find("Inspector/grid/Grid/row[mass]/value")->value == "9000");
    // History lists it with its origin.
    const UiItem* entry = nullptr;
    for (const UiItem& i : h.ui->items()) {
        if (i.path.starts_with("History/entries/list/row[") && i.path.ends_with("/entry")) entry = &i;
    }
    REQUIRE(entry != nullptr);
    CHECK(entry->label.find("(rpc)") != std::string::npos);
}

TEST_CASE("shell: menus and the palette run UI commands") {
    ShellHarness h("shell_menus");
    REQUIRE(click(h, "MainMenu/View"));
    REQUIRE(h.ui->find("MainMenu/View/view.theme.light") != nullptr);
    REQUIRE(click(h, "MainMenu/View/view.theme.light"));
    REQUIRE(h.host->requestedTheme);
    CHECK(*h.host->requestedTheme == "light");
    // Palette: Ctrl+Shift+P, type, Enter runs the first match.
    REQUIRE(key(h, "ctrl+shift+p"));
    REQUIRE(h.ui->find("Palette/filter") != nullptr);
    REQUIRE(typeText(h, "UI Scale 200"));
    REQUIRE(key(h, "enter"));
    REQUIRE(h.host->requestedScale);
    CHECK(*h.host->requestedScale == doctest::Approx(2.0));
    CHECK(h.ui->find("Palette/filter") == nullptr);  // closed
    // Window menu toggles panels; the keyboard-reachability list follows.
    CHECK(h.shell->panelPaths().size() == 5);
    REQUIRE(h.fw->invoker(tf::Origin::UiScripted).invoke("window.output"));
    h.frame();
    CHECK(h.shell->panelPaths().size() == 4);
    CHECK(h.ui->find("Output") == nullptr);
    REQUIRE(h.fw->invoker(tf::Origin::UiScripted).invoke("view.resetLayout"));
    h.frame();
    CHECK(h.shell->panelPaths().size() == 5);
    // Ctrl+Q asks the host to quit.
    REQUIRE(key(h, "ctrl+q"));
    CHECK(h.host->quit);
}

TEST_CASE("shell: commands with required arguments open the argument form") {
    ShellHarness h("shell_form");
    REQUIRE(click(h, kFrigateRow));
    REQUIRE(h.fw->invoker(tf::Origin::UiScripted).invoke("ui.commandPalette"));
    h.frame();
    REQUIRE(typeText(h, "Rename"));
    REQUIRE(key(h, "enter"));
    REQUIRE(h.ui->find("CommandForm/run") != nullptr);
    REQUIRE(click(h, "CommandForm/name"));
    REQUIRE(key(h, "ctrl+a"));
    REQUIRE(typeText(h, "hull/frigate_mk2"));
    REQUIRE(click(h, "CommandForm/run"));
    CHECK(h.fw->documents().find(std::string_view("hull/frigate_mk2")) != nullptr);
    CHECK(h.fw->history().back().tx.origin == tf::Origin::UiScripted);
}

TEST_CASE("shell: every command is exposed and the real shell is lint-clean at 100 % and 200 %") {
    for (const f32 scale : {1.0f, 2.0f}) {
        CAPTURE(scale);
        ShellHarness h(scale == 1.0f ? "shell_lint1" : "shell_lint2", 1920 * scale, 1080 * scale);
        applyTheme(h.theme, scale, ImGui::GetStyle());
        h.shell->resetLayout();
        h.frame();
        REQUIRE(click(h, kFrigateRow));
        UiTest::LintOptions o;
        o.panels = h.shell->panelPaths();
        o.commands = &h.fw->commands();
        o.theme = &h.theme;
        const auto issues = h.ui->lint(o);
        for (const LintIssue& i : issues) FAIL_CHECK(i.rule << " " << i.path << ": " << i.message);
    }
}

TEST_CASE("shell: the shell's framework listener does not outlive the shell") {
    ShellHarness h("shell_listener");
    h.shell.reset();
    // Events after the shell is gone (closing documents at shutdown) must not reach it.
    auto r = h.fw->invoker(tf::Origin::Cli).invoke("doc.setProperty", R"({"doc":"hull/frigate","path":"mass","value":13000})");
    CHECK(r);
    CHECK(h.fw->close(h.frigate().id(), true));
}

TEST_CASE("perf: item table costs at most 0.2 ms per frame (07 §4.4)") {
    ShellHarness h("perf_items");
    REQUIRE(click(h, kFrigateRow));
    REQUIRE(h.ui->find("Inspector/grid/Grid/row[mass]/value") != nullptr);
    constexpr int kFrames = 20;
    const auto batch = [&](bool on) {
        h.ui->setEnabled(on);
        h.frame();  // settle after the switch
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kFrames; ++i) h.frame();
        return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
    };
    // Alternate off/on batches so machine load affects both sides alike; take the median difference.
    std::vector<f64> costs;
    f64 offSum = 0;
    for (int round = 0; round < 31; ++round) {
        const f64 off = batch(false);
        const f64 on = batch(true);
        offSum += off;
        costs.push_back(on - off);
    }
    std::sort(costs.begin(), costs.end());
    const f64 median = costs[costs.size() / 2];
    MESSAGE("item table: median +" << median << " ms per frame (frame without it: " << offSum / 31 << " ms, "
                                   << h.ui->items().size() << " items)");
    // The budget holds for optimized builds; Debug and sanitizer builds only report it, as the other
    // `perf:` gates do (engine/render/tests/test_graph_perf.cpp).
#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
    CHECK(median <= 0.2);
#endif
}

} // namespace
