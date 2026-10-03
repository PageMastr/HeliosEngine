// helios-uitest: the editor UI test driver (07 §4.4). It starts helios-editor in test mode on a
// copy of a fixture project, drives it over the remote-control socket with ui.* input injection
// (the same SDL3 path a mouse and keyboard take), and checks the result through the item table,
// the ToolsFramework RPC methods, the layout lints and ꟻLIP image goldens. The built-in suite is the
// ED-15 harness self-test (shell and property grid at 100 % / 200 %, dark and high-contrast). See
// README.md.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/core/process.h"
#include "helios/core/version.h"
#include "helios/render/flip.h"
#include "helios/render/image.h"
#include "helios/toolsfw/journal.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/rpc.h"

using namespace helios;

namespace {

constexpr std::string_view kUsage = R"(helios-uitest - editor UI test driver (ED-15 harness self-test)

  helios-uitest --editor=<helios-editor> --fixture=<project dir> --goldens=<dir> --out=<dir> [options]

  --update-goldens     write the captures as the new goldens (review them before committing)
  --present            show the editor window while the suite runs
  --width=<dip> --height=<dip>   editor window size at 100 % (default 1920x1080, 07 §4.4)
  --flip-mean=<f>      mean ꟻLIP error allowed per golden (default 0.01, 03 §8.4)
  --flip-hot=<f>       fraction of pixels allowed above ꟻLIP 0.1 (default 0.001, 07 §4.4)
  --dump-items         print the item table after the fixture opens, then continue
  --keep-going         run every check even after a failure
  --log-level=<level>  (default warn)

Exit code 0 when every check passes, 1 when one fails, 2 on usage or setup errors.
)";

struct Check {
    std::string name;
    bool ok = true;
    std::string detail;
};

class Suite {
public:
    Suite(tf::RpcClient& rpc, const CommandLine& cl, fs::Path out, fs::Path goldens)
        : m_rpc(rpc), m_cl(cl), m_out(std::move(out)), m_goldens(std::move(goldens)) {}

    bool run();
    const std::vector<Check>& checks() const noexcept { return m_checks; }

private:
    Result<std::string> call(std::string_view method, std::string_view params = {}) { return m_rpc.call(method, params, 120000); }
    Result<refl::JsonDocument> callJson(std::string_view method, std::string_view params = {}) {
        HELIOS_TRY_ASSIGN(const std::string text, call(method, params));
        return refl::JsonDocument::parse(text, method);
    }
    /// Records a check; returns `ok`.
    bool check(std::string name, bool ok, std::string detail = {}) {
        std::fprintf(stdout, "%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
        std::fflush(stdout);
        m_checks.push_back({std::move(name), ok, std::move(detail)});
        if (!ok) m_failed = true;
        return ok;
    }
    bool failed() const noexcept { return m_failed && !m_cl.has("keep-going"); }
    bool step(std::string name, const Result<std::string>& r) {
        return check(std::move(name), r.hasValue(), r ? std::string() : r.error().message);
    }

    Result<std::string> docGet(std::string_view path) {
        return call("doc.get", std::format(R"({{"doc":"hull/frigate","path":{}}})", tf::json::quote(path)));
    }
    Result<std::string> docText() {
        HELIOS_TRY_ASSIGN(auto d, callJson("doc.text", R"({"doc":"hull/frigate"})"));
        return std::string(d.root().asString());
    }
    Result<std::string> state(std::string_view key) {
        HELIOS_TRY_ASSIGN(auto d, callJson("ui.state"));
        const refl::JsonValue v = d.root().get(key);
        if (v.isString()) return std::string(v.asString());
        return tf::json::compact(v);
    }

    bool itemTable();
    bool goldens();
    bool compareGolden(const std::string& name, const fs::Path& capture);
    bool uiEdits();
    bool keyboard();

    tf::RpcClient& m_rpc;
    const CommandLine& m_cl;
    fs::Path m_out;
    fs::Path m_goldens;
    std::vector<Check> m_checks;
    bool m_failed = false;
};

std::string param(std::string_view key, std::string_view value) {
    return std::format(R"({{{}:{}}})", tf::json::quote(key), tf::json::quote(value));
}

bool Suite::run() {
    if (!itemTable() && failed()) return false;
    if (!goldens() && failed()) return false;
    if (!uiEdits() && failed()) return false;
    if (!keyboard() && failed()) return false;
    return !m_failed;
}

/// The item table: stable paths, kinds and enabled state of the shell's widgets.
bool Suite::itemTable() {
    if (!step("wait for the Documents list", call("ui.waitFor", param("path", "Documents/docs/list/row[hull/frigate]/select")))) return false;
    auto items = callJson("ui.items");
    if (!check("ui.items", items.hasValue(), items ? "" : items.error().message)) return false;
    if (m_cl.has("dump-items")) {
        for (refl::JsonValue i : items->root().elements()) {
            std::fprintf(stdout, "  %s  (%s)\n", std::string(tf::json::getString(i, "path").value_or("")).c_str(),
                         std::string(tf::json::getString(i, "kind").value_or("")).c_str());
        }
    }
    std::map<std::string, refl::JsonValue, std::less<>> byPath;
    for (refl::JsonValue i : items->root().elements()) byPath.emplace(std::string(tf::json::getString(i, "path").value_or("")), i);
    const std::pair<const char*, const char*> expected[] = {
        {"MainMenu/File", "menu"},           {"MainMenu/Help", "menu"},  {"Documents/filter", "input"},
        {"Documents/docs/list/row[hull/frigate]/select", "item"},       {"History/undo", "item"},
        {"Viewport/viewport", "viewport"},   {"Inspector", "window"},    {"StatusBar", "window"},
    };
    bool ok = true;
    for (const auto& [path, kind] : expected) {
        const auto it = byPath.find(path);
        const bool found = it != byPath.end() && tf::json::getString(it->second, "kind").value_or("") == kind;
        ok = check(std::format("item {} ({})", path, kind), found) && ok;
    }
    if (const auto it = byPath.find("History/undo"); it != byPath.end()) {
        ok = check("History/undo is disabled with an empty history", !tf::json::getBool(it->second, "enabled").value_or(true)) && ok;
    }
    // Selecting the Frigate through the UI fills the Inspector with its property grid.
    if (!step("click Documents/.../row[hull/frigate]/select", call("ui.click", param("path", "Documents/docs/list/row[hull/frigate]/select")))) return false;
    auto mass = call("ui.waitFor", R"({"path":"**/row[mass]/value","value":"12000"})");
    ok = check("Inspector shows mass = 12000", mass.hasValue(), mass ? "" : mass.error().message) && ok;
    auto sel = call("selection.get");
    ok = check("selection holds one record", sel.hasValue() && sel->find("\"doc\"") != std::string::npos, sel ? *sel : sel.error().message) && ok;
    return ok;
}

bool Suite::compareGolden(const std::string& name, const fs::Path& capture) {
    const fs::Path golden = m_goldens / fs::pathFromUtf8(name);
    if (m_cl.has("update-goldens")) {
        std::error_code ec;
        std::filesystem::create_directories(m_goldens, ec);
        std::filesystem::copy_file(capture, golden, std::filesystem::copy_options::overwrite_existing, ec);
        return check(std::format("golden {} updated", name), !ec, ec ? ec.message() : "");
    }
    auto ref = render::readPng(golden);
    if (!ref) return check(std::format("golden {}", name), false, std::format("{} (run with --update-goldens)", ref.error().message));
    auto test = render::readPng(capture);
    if (!test) return check(std::format("golden {}", name), false, test.error().message);
    if (ref->width != test->width || ref->height != test->height) {
        return check(std::format("golden {}", name), false,
                     std::format("size {}x{}, golden {}x{}", test->width, test->height, ref->width, ref->height));
    }
    // Identical images have a ꟻLIP error of exactly 0 everywhere, so they pass without the metric.
    // ꟻLIP costs about a minute per 1080p image in Debug sanitizer builds, which run every test.
    const u64 differing = render::countDifferentPixels(*ref, *test);
    if (differing == 0) {
        return check(std::format("golden {}", name), true, "identical to the golden (0 pixels differ, so ꟻLIP mean and max are 0)");
    }
    auto flip = render::computeFlip(*ref, *test);
    if (!flip) return check(std::format("golden {}", name), false, flip.error().message);
    const f64 meanLimit = m_cl.getFloat("flip-mean", 0.01);
    const f64 hotLimit = m_cl.getFloat("flip-hot", 0.001);
    const f64 hot = static_cast<f64>(flip->countAbove(0.1f)) / static_cast<f64>(std::max<u64>(1, u64(flip->width) * flip->height));
    const bool ok = flip->mean <= meanLimit && hot <= hotLimit;
    if (!ok) (void)render::writePng(m_out / fs::pathFromUtf8(name + ".flip.png"), render::flipErrorImage(*flip));
    return check(std::format("golden {}", name), ok,
                 std::format("FLIP mean {:.5f} (<= {}), max {:.3f}, {:.4f} % of pixels > 0.1 (<= {} %), {} pixel(s) differ", flip->mean,
                             meanLimit, flip->max, hot * 100.0, hotLimit * 100.0, differing));
}

/// ED-15: the shell and the property grid at 100 % and 200 %, dark and high contrast, against
/// ꟻLIP goldens, with the layout lints clean in every configuration.
bool Suite::goldens() {
    bool ok = true;
    const std::pair<const char*, const char*> themes[] = {{"dark", "dark"}, {"high-contrast", "hc"}};
    const std::pair<int, const char*> scales[] = {{1, "100"}, {2, "200"}};
    fs::Path firstGrid;
    for (const auto& [theme, themeTag] : themes) {
        for (const auto& [scale, pct] : scales) {
            const std::string cfg = std::format("{} {}%", theme, pct);
            if (!step("configure " + cfg, call("ui.configure", std::format(R"({{"theme":"{}","scale":{}}})", theme, scale)))) return false;
            // Park the mouse on the menu bar's empty left edge: nothing is hovered in the captures.
            if (!step("park mouse", call("ui.move", R"({"x":1,"y":1})"))) return false;
            auto lint = callJson("ui.lint");
            if (!lint) return check("ui.lint " + cfg, false, lint.error().message);
            ok = check("layout lints " + cfg, lint->root().size() == 0, tf::json::compact(lint->root())) && ok;
            for (const auto& [target, kind] : {std::pair{"window", "shell"}, std::pair{"Inspector", "grid"}}) {
                const std::string name = std::format("{}_{}_{}.png", kind, themeTag, pct);
                const fs::Path file = m_out / fs::pathFromUtf8(name);
                auto cap = callJson("ui.capture", std::format(R"({{"target":"{}","file":{}}})", target, tf::json::quote(fs::pathToUtf8(file))));
                if (!check("capture " + name, cap.hasValue(), cap ? "" : cap.error().message)) {
                    ok = false;
                    continue;
                }
                if (std::string_view(kind) == "shell") {
                    ok = check("viewport masked in " + name, cap->root().get("masks").size() == 1, tf::json::compact(cap->root().get("masks"))) && ok;
                }
                if (firstGrid.empty() && std::string_view(kind) == "grid") firstGrid = file;
                ok = compareGolden(name, file) && ok;
            }
        }
    }
    // Determinism within the run: back to the first configuration, the grid is pixel-identical.
    if (!step("configure dark 100% again", call("ui.configure", R"({"theme":"dark","scale":1})"))) return false;
    (void)call("ui.move", R"({"x":1,"y":1})");
    const fs::Path again = m_out / "grid_dark_100_again.png";
    auto cap = call("ui.capture", std::format(R"({{"target":"Inspector","file":{}}})", tf::json::quote(fs::pathToUtf8(again))));
    if (cap && !firstGrid.empty()) {
        auto a = render::readPng(firstGrid);
        auto b = render::readPng(again);
        const bool same = a && b && *a == *b;
        ok = check("captures are deterministic (dark 100% grid twice)", same,
                   a && b ? std::format("{} pixel(s) differ", render::countDifferentPixels(*a, *b)) : "unreadable capture") && ok;
    } else {
        ok = check("captures are deterministic", false, cap ? "no first capture" : cap.error().message) && ok;
    }
    // Layout lints at the five scales of 07 §4.4 (no captures needed), then with +40 %
    // pseudo-localized labels.
    for (const char* scale : {"0.75", "1", "1.5", "2", "2.5"}) {
        if (!step(std::format("configure dark {}x", scale), call("ui.configure", std::format(R"({{"theme":"dark","scale":{}}})", scale)))) return false;
        auto lint = callJson("ui.lint");
        ok = check(std::format("layout lints dark {}x", scale), lint && lint->root().size() == 0,
                   lint ? tf::json::compact(lint->root()) : lint.error().message) && ok;
    }
    if (step("configure pseudo-localization", call("ui.configure", R"({"theme":"dark","scale":1,"pseudoLoc":true})"))) {
        auto lint = callJson("ui.lint");
        ok = check("layout lints with pseudo-localized labels", lint && lint->root().size() == 0,
                   lint ? tf::json::compact(lint->root()) : lint.error().message) && ok;
        (void)call("ui.configure", R"({"pseudoLoc":false})");
    }
    return ok;
}

/// Edits through the UI input path (origin ui-scripted) against the same edits through RPC:
/// both must produce the same document ("dual path"), and undo restores the fixture exactly.
bool Suite::uiEdits() {
    bool ok = true;
    auto original = docText();
    if (!check("doc.text", original.hasValue(), original ? "" : original.error().message)) return false;
    // Double-click turns the drag field into a text field; type the new value.
    const std::string mass = "Inspector/grid/Grid/row[mass]/value";
    if (!step("double-click mass", call("ui.doubleClick", param("path", mass)))) return false;
    if (!step("select all", call("ui.key", R"({"chord":"ctrl+a"})"))) return false;
    if (!step("type 13000", call("ui.type", R"({"text":"13000"})"))) return false;
    if (!step("enter", call("ui.key", R"({"chord":"enter"})"))) return false;
    auto v = docGet("mass");
    ok = check("UI edit set mass = 13000", v && *v == "13000", v ? *v : v.error().message) && ok;
    auto hist = callJson("tx.history");
    std::string origin;
    if (hist && hist->root().size() > 0) {
        refl::JsonValue last;
        for (refl::JsonValue e : hist->root().elements()) last = e;
        origin = std::string(tf::json::getString(last, "origin").value_or(""));
    }
    ok = check("UI edit is a transaction with origin ui-scripted", origin == "ui-scripted", origin) && ok;
    auto uiText = docText();
    // Undo from the keyboard (Ctrl+Z goes through the shell's shortcut routing).
    (void)call("ui.move", R"({"x":1,"y":1})");
    if (!step("ctrl+z", call("ui.key", R"({"chord":"ctrl+z"})"))) return false;
    auto undone = docText();
    ok = check("Ctrl+Z restores the record byte for byte", undone && original && *undone == *original) && ok;
    // The same edit through the command bus over RPC.
    auto rpcEdit = call("cmd.invoke", R"({"id":"doc.setProperty","args":{"doc":"hull/frigate","path":"mass","value":13000}})");
    ok = check("RPC edit (cmd.invoke doc.setProperty)", rpcEdit.hasValue(), rpcEdit ? "" : rpcEdit.error().message) && ok;
    auto rpcText = docText();
    ok = check("dual path: UI and RPC edits give identical documents", uiText && rpcText && *uiText == *rpcText) && ok;
    // The grid follows edits made elsewhere.
    auto shown = call("ui.waitFor", R"({"path":"Inspector/grid/Grid/row[mass]/value","value":"13000","timeoutFrames":30})");
    ok = check("grid shows the RPC edit", shown.hasValue(), shown ? "" : shown.error().message) && ok;
    (void)call("tx.undo");
    // A flag check box: one click, one transaction.
    if (!step("click capabilities/Mine", call("ui.click", param("path", "Inspector/grid/Grid/row[capabilities]/Mine")))) return false;
    auto caps = docGet("capabilities");
    ok = check("check box added Mine", caps && caps->find("Mine") != std::string::npos, caps ? *caps : caps.error().message) && ok;
    if (!step("History/undo button", call("ui.click", param("path", "History/undo")))) return false;
    auto restored = docText();
    ok = check("History panel undo restores the record", restored && original && *restored == *original) && ok;
    auto docs = callJson("doc.list");
    usize dirty = 0, count = 0;
    if (docs) {
        for (refl::JsonValue d : docs->root().elements()) {
            ++count;
            dirty += tf::json::getBool(d, "dirty").value_or(true) ? 1 : 0;
        }
    }
    ok = check("every record is clean after undo", docs && count == 2 && dirty == 0, std::format("{} document(s), {} dirty", count, dirty)) && ok;
    return ok;
}

/// Keyboard reachability: the command palette, menus, and Ctrl+Tab over every panel.
bool Suite::keyboard() {
    bool ok = true;
    if (!step("ctrl+shift+p", call("ui.key", R"({"chord":"ctrl+shift+p"})"))) return false;
    auto palette = call("ui.waitFor", R"({"path":"Palette/filter","timeoutFrames":30})");
    ok = check("Ctrl+Shift+P opens the command palette", palette.hasValue(), palette ? "" : palette.error().message) && ok;
    (void)call("ui.type", R"({"text":"light theme"})");
    (void)call("ui.key", R"({"chord":"enter"})");
    (void)call("ui.frames", R"({"count":2})");
    auto theme = state("theme");
    ok = check("palette runs the first match (Light Theme)", theme && *theme == "light", theme ? *theme : theme.error().message) && ok;
    // The same command from the View menu.
    (void)call("ui.click", param("path", "MainMenu/View"));
    auto item = call("ui.waitFor", R"({"path":"MainMenu/View/view.theme.dark","timeoutFrames":30})");
    ok = check("View menu lists the theme commands", item.hasValue(), item ? "" : item.error().message) && ok;
    (void)call("ui.click", param("path", "MainMenu/View/view.theme.dark"));
    (void)call("ui.frames", R"({"count":2})");
    theme = state("theme");
    ok = check("View > Dark Theme", theme && *theme == "dark", theme ? *theme : theme.error().message) && ok;
    // Ctrl+Tab window switching reaches every panel. ImGui orders the Ctrl+Tab list by recent
    // focus; focusing Documents first and pressing Tab n times (Ctrl held) lands on the n-th
    // entry, and the entries after it keep their places, so n = 1..10 visits the whole list.
    std::set<std::string> reached;
    for (int n = 1; n <= 10; ++n) {
        if (!call("ui.click", param("path", "Documents/docs/list/row[hull/frigate]/select"))) break;
        if (!call("ui.key", std::format(R"({{"chord":"ctrl+tab","repeat":{}}})", n))) break;
        if (auto f = state("focused")) reached.insert(*f);
    }
    std::string missing;
    for (const char* panel : {"Documents", "Inspector", "History", "Output", "Viewport"}) {
        if (!reached.contains(panel)) missing += std::string(missing.empty() ? "" : ", ") + panel;
    }
    std::string seen;
    for (const std::string& s : reached) seen += (seen.empty() ? "" : ", ") + s;
    ok = check("Ctrl+Tab reaches every panel", missing.empty(), missing.empty() ? "reached " + seen : "missing " + missing + "; reached " + seen) && ok;
    return ok;
}

int run(const CommandLine& cl) {
    if (cl.has("help") || cl.has("h")) {
        std::fputs(kUsage.data(), stdout);
        return 0;
    }
    if (cl.has("version")) {
        std::printf("helios-uitest %s\n", version::kString);
        return 0;
    }
    log::setLevel(log::Level::Warn);
    if (auto lvl = cl.value("log-level")) {
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    }
    for (const char* required : {"editor", "fixture", "goldens", "out"}) {
        if (!cl.value(required)) {
            std::fprintf(stderr, "helios-uitest: --%s=<...> is required\n%s", required, kUsage.data());
            return 2;
        }
    }
    const fs::Path editor = fs::pathFromUtf8(*cl.value("editor"));
    const fs::Path fixture = fs::pathFromUtf8(*cl.value("fixture"));
    const fs::Path goldens = fs::pathFromUtf8(*cl.value("goldens"));
    const fs::Path out = fs::pathFromUtf8(*cl.value("out"));

    // A private copy of the fixture: the suite edits it and the editor journals next to it.
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    std::filesystem::create_directories(out / "project", ec);
    std::filesystem::copy(fixture, out / "project", std::filesystem::copy_options::recursive, ec);
    if (ec) {
        std::fprintf(stderr, "helios-uitest: copying the fixture: %s\n", ec.message().c_str());
        return 2;
    }

    const std::string endpoint = std::format("helios-uitest-{}", tf::currentProcessId());
    ProcessDesc desc;
    desc.executable = editor;
    desc.args = {"--test-mode", "--rpc=" + endpoint, "--project-root=" + fs::pathToUtf8(out / "project"), "--project=uitest-fixture",
                 "--journal-dir=" + fs::pathToUtf8(out / "journal"), std::format("--width={}", cl.getInt("width", 1920)),
                 std::format("--height={}", cl.getInt("height", 1080)), "--theme=dark", "--scale=1",
                 std::format("--log-level={}", cl.getString("log-level", "warn"))};
    if (cl.has("present")) desc.args.push_back("--present");
    auto child = Process::spawn(desc);
    if (!child) {
        std::fprintf(stderr, "helios-uitest: starting %s: %s\n", fs::pathToUtf8(editor).c_str(), child.error().message.c_str());
        return 2;
    }
    auto rpc = tf::RpcClient::connect(endpoint, 60000);
    if (!rpc) {
        std::fprintf(stderr, "helios-uitest: connecting to the editor: %s\n", rpc.error().message.c_str());
        (void)child->kill();
        (void)child->wait();
        return 2;
    }
    Suite suite(**rpc, cl, out, goldens);
    const auto start = std::chrono::steady_clock::now();
    bool ok = suite.run();
    (void)(*rpc)->call("app.quit");
    auto exited = child->wait(std::chrono::milliseconds(30000));
    if (!exited || !*exited) {
        (void)child->kill();
        (void)child->wait();
        ok = false;
        std::fprintf(stdout, "FAIL  editor did not exit within 30 s of app.quit\n");
    } else if (**exited != 0) {
        ok = false;
        std::fprintf(stdout, "FAIL  editor exit code %d (2 = RHI validation errors)\n", **exited);
    }
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();

    // Machine-readable report next to the captures.
    refl::JsonWriter w;
    w.beginObject();
    w.key("ok");
    w.boolean(ok);
    w.key("seconds");
    w.number(seconds);
    w.key("checks");
    w.beginArray();
    usize failed = 0;
    for (const Check& c : suite.checks()) {
        failed += c.ok ? 0 : 1;
        w.beginObject();
        w.key("name");
        w.string(c.name);
        w.key("ok");
        w.boolean(c.ok);
        w.key("detail");
        w.string(c.detail);
        w.endObject();
    }
    w.endArray();
    w.endObject();
    (void)fs::writeTextFile(out / "report.json", w.take());
    std::fprintf(stdout, "%s: %zu check(s), %zu failed, %.1f s (report: %s)\n", ok ? "PASSED" : "FAILED", suite.checks().size(), failed,
                 seconds, fs::pathToUtf8(out / "report.json").c_str());
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const CommandLine cl = platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv);
    return run(cl);
}
