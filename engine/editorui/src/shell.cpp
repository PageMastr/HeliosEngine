#include "helios/editorui/shell.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>

#include "helios/core/version.h"
#include "helios/reflect/type_info.h"
#include "helios/reflect/json.h"
#include "helios/editorui/localize.h"
#include "helios/editorui/ui_test.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/journal.h"
#include "helios/toolsfw/json_util.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "semantic_colors.h"

namespace helios::edui {

namespace {

constexpr usize kOutputLines = 200;

std::string panelWindow(std::string_view id) {
    return std::format("{}###{}", tr(std::string(id).c_str()), id);
}

/// "Ctrl+Shift+P" -> ImGui chord (0 when unknown).
ImGuiKeyChord chordFromText(std::string_view text) {
    ImGuiKeyChord chord = 0;
    ImGuiKey key = ImGuiKey_None;
    std::string_view rest = text;
    while (!rest.empty()) {
        const usize plus = rest.find('+', 1);
        std::string part(rest.substr(0, plus));
        rest = plus == std::string_view::npos ? std::string_view() : rest.substr(plus + 1);
        for (char& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (part == "ctrl") {
            chord |= ImGuiMod_Ctrl;
        } else if (part == "shift") {
            chord |= ImGuiMod_Shift;
        } else if (part == "alt") {
            chord |= ImGuiMod_Alt;
        } else if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z') {
            key = static_cast<ImGuiKey>(ImGuiKey_A + (part[0] - 'a'));
        } else if (part.size() == 1 && part[0] >= '0' && part[0] <= '9') {
            key = static_cast<ImGuiKey>(ImGuiKey_0 + (part[0] - '0'));
        } else if (part.size() >= 2 && part[0] == 'f' && std::isdigit(static_cast<unsigned char>(part[1]))) {
            const int n = std::atoi(part.c_str() + 1);
            if (n >= 1 && n <= 12) key = static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
        } else if (part == "delete") {
            key = ImGuiKey_Delete;
        } else if (part == "escape") {
            key = ImGuiKey_Escape;
        } else if (part == "enter") {
            key = ImGuiKey_Enter;
        } else if (part == "left") {
            key = ImGuiKey_LeftArrow;
        } else if (part == "right") {
            key = ImGuiKey_RightArrow;
        } else if (part == "up") {
            key = ImGuiKey_UpArrow;
        } else if (part == "down") {
            key = ImGuiKey_DownArrow;
        } else if (part == "tab") {
            key = ImGuiKey_Tab;
        }
    }
    return key == ImGuiKey_None ? 0 : (chord | key);
}

std::string menuLabel(const char* label, std::string_view command) {
    return std::format("{}##{}", tr(label), command);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::Path journalRootOf(const tf::Framework& fw) {
    return fw.config().journalRoot.empty() ? tf::defaultJournalRoot() : fw.config().journalRoot;
}

/// Appends the "end" record, so the session is no longer offered for recovery.
Result<void> markSessionEnded(const fs::Path& journal) {
    HELIOS_TRY_ASSIGN(auto writer, tf::JournalWriter::reopen(journal));
    return writer->close(true);
}

} // namespace

Result<std::string> recoverJournal(tf::Framework& framework, const fs::Path& journal) {
    HELIOS_TRY_ASSIGN(const tf::RecoveryReport report, framework.recover(journal));
    std::string note = std::format("Recovered {} transaction(s) from {}", report.replayed, fs::pathToUtf8(journal.filename()));
    bool complete = true;
    for (const tf::RecoveredDocument& d : report.documents) {
        using enum tf::DocRecovery;
        if (d.status == Replayed || d.status == UpToDate || d.status == Created) continue;
        complete = false;
        note += std::format("; {}: {} {}", d.file, tf::docRecoveryName(d.status), d.message);
    }
    if (report.replayed > 0) {
        // What a replay cannot restore exactly (07 §1.2; tf::Framework::recover).
        note += ". Recovered undo and redo steps are ordinary history entries now, and only the recovered "
                "documents' part of a multi-document transaction was replayed";
    }
    if (complete) {
        // Every document came back: mark the journal ended so it is not offered again.
        const Result<void> marked = markSessionEnded(journal);
        if (!marked) note += std::format(" (could not mark {} recovered: {})", fs::pathToUtf8(journal.filename()), marked.error().message);
    } else {
        note += ". The session stays offered under File > Recover Unsaved Session until File > Discard Unsaved Session "
                "stops offering it";
    }
    return note;
}

Shell::Shell(tf::Framework& framework, ShellHost& host) : m_fw(framework), m_host(host) {
    for (std::string_view id : kPanelIds) m_panelOpen[std::string(id)] = true;
    registerCommands();
    m_menus["File"] = {{"doc.create", "New Record...", false},  {"doc.open", "Open Record...", false},
                       {"doc.save", "Save", false},              {"doc.saveAll", "Save All", false},
                       {"doc.revert", "Revert", true},           {"doc.reload", "Reload From Disk", true},
                       {"app.recoverSession", "Recover Unsaved Session", false},
                       {"app.discardSession", "Discard Unsaved Session", false},
                       {"app.quit", "Exit", false}};
    m_menus["Edit"] = {{"edit.undo", "Undo", false},         {"edit.redo", "Redo", false},
                       {"doc.rename", "Rename...", true},     {"doc.destroy", "Delete Record", true},
                       {"selection.back", "Selection Back", false}, {"selection.forward", "Selection Forward", false},
                       {"ui.commandPalette", "Command Palette...", false}};
    m_menus["View"] = {{"view.theme.dark", "Dark Theme", false},     {"view.theme.light", "Light Theme", false},
                       {"view.theme.highContrast", "High Contrast Theme", false},
                       {"view.scale.100", "UI Scale 100%", false},   {"view.scale.150", "UI Scale 150%", false},
                       {"view.scale.200", "UI Scale 200%", false},   {"view.resetLayout", "Reset Layout", false}};
    m_menus["Window"] = {{"window.documents", "Documents", false}, {"window.inspector", "Inspector", false},
                         {"window.history", "History", false},     {"window.output", "Output", false},
                         {"window.viewport", "Viewport", false}};
    m_menus["Tools"] = {{"validate.run", "Validate", false}};
    m_menus["Help"] = {{"help.about", "About Helios", false}};
    m_documentMenu = {{"doc.save", "Save", true},     {"doc.revert", "Revert", true}, {"doc.reload", "Reload From Disk", true},
                      {"doc.rename", "Rename...", true}, {"doc.destroy", "Delete Record", true}};
    tf::CommandBus& bus = m_fw.commands();
    for (const auto& [menu, entries] : m_menus) {
        for (const MenuEntry& e : entries) bus.markExposed(e.command, "MainMenu/" + menu);
    }
    for (const MenuEntry& e : m_documentMenu) bus.markExposed(e.command, "Documents/context");
    // The framework outlives the shell in EditorHost, but may emit events after the shell is gone
    // (closing documents at shutdown): the listener holds a weak token.
    m_fw.addListener([this, alive = std::weak_ptr<int>(m_alive)](const tf::FrameworkEvent& e) {
        if (alive.lock()) onEvent(e);
    });
    // 07 §1.2: after an unclean exit the editor offers the session's replay.
    refreshRecoverable();
    offerRecoverable();
}

void Shell::offerRecoverable() {
    if (m_recoverable.empty()) return;
    const tf::JournalSessionInfo& s = m_recoverable.back();
    std::string line = std::format("An earlier session ({}, {} transaction(s)) ended without saving or crashed: File > Recover "
                                   "Unsaved Session replays it, File > Discard Unsaved Session stops offering it",
                                   s.header.session, s.txCount);
    if (m_recoverable.size() > 1) {
        line += std::format(" ({} such sessions; the newest comes first, and both commands take a session name)", m_recoverable.size());
    }
    log(std::move(line));
}

Result<tf::JournalSessionInfo> Shell::recoverableSession(const tf::CommandContext& ctx) {
    refreshRecoverable();
    if (m_recoverable.empty()) return Error{ErrorCode::NotFound, "no unclean session to recover"};
    if (!ctx.has("session")) return m_recoverable.back();
    HELIOS_TRY_ASSIGN(const std::string name, ctx.stringArg("session"));
    for (const tf::JournalSessionInfo& s : m_recoverable) {
        if (s.header.session == name) return s;
    }
    return Error{ErrorCode::NotFound, std::format("no unclean session named '{}'", name)};
}

void Shell::refreshRecoverable() {
    m_recoverable = tf::listJournalSessions(journalRootOf(m_fw), m_fw.config().project, true);
    // This process's own journal is never offered (listJournalSessions skips running sessions).
}

Shell::~Shell() = default;

void Shell::registerCommands() {
    tf::CommandBus& bus = m_fw.commands();
    const auto add = [&](std::string id, std::string label, std::string category, std::string doc, std::string shortcut,
                         std::function<Result<void>(tf::CommandContext&)> fn) {
        tf::CommandDesc c;
        c.id = std::move(id);
        c.label = std::move(label);
        c.category = std::move(category);
        c.doc = std::move(doc);
        c.shortcut = std::move(shortcut);
        c.headless = false;
        c.execute = std::move(fn);
        (void)bus.add(std::move(c));
    };
    add("ui.commandPalette", "Command Palette", "View", "Lists every command with its shortcut; type to filter, Enter runs.",
        "Ctrl+Shift+P", [this](tf::CommandContext&) -> Result<void> {
            m_openPalette = true;
            return {};
        });
    const std::pair<const char*, const char*> themes[] = {{"dark", "Dark"}, {"light", "Light"}, {"highContrast", "High Contrast"}};
    for (const auto& [id, label] : themes) {
        const std::string name = std::string(id) == "highContrast" ? "high-contrast" : id;
        add(std::string("view.theme.") + id, std::string(label) + " Theme", "View", std::format("Switches to the {} theme.", label), {},
            [this, name](tf::CommandContext&) -> Result<void> {
                m_host.requestTheme(name);
                return {};
            });
    }
    for (const int pct : {100, 150, 200}) {
        add(std::format("view.scale.{}", pct), std::format("UI Scale {}%", pct), "View",
            std::format("Sets the UI scale to {}% (fonts re-rasterize, 07 §1.3).", pct), {}, [this, pct](tf::CommandContext&) -> Result<void> {
                m_host.requestScale(static_cast<f32>(pct) / 100.0f);
                return {};
            });
    }
    add("view.resetLayout", "Reset Layout", "View", "Restores the default dock layout.", {}, [this](tf::CommandContext&) -> Result<void> {
        m_layoutDirty = true;
        for (auto& [id, open] : m_panelOpen) open = true;
        return {};
    });
    for (std::string_view panel : kPanelIds) {
        std::string id(panel);
        std::string lowerId = id;
        lowerId[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowerId[0])));
        add("window." + lowerId, id, "Window", std::format("Shows or hides the {} panel.", id), {}, [this, id](tf::CommandContext&) -> Result<void> {
            m_panelOpen[id] = !m_panelOpen[id];
            return {};
        });
    }
    add("app.quit", "Exit", "File",
        "Closes the editor. The journal ends clean only when every record is saved, so unsaved edits can be recovered at the "
        "next start.",
        "Ctrl+Q", [this](tf::CommandContext&) -> Result<void> {
            m_host.requestQuit();
            return {};
        });
    {
        tf::CommandDesc c;
        c.id = "app.recoverSession";
        c.label = "Recover Unsaved Session";
        c.category = "File";
        c.doc = "Replays the newest (or the named) earlier session of this project that crashed or quit with unsaved records "
                "(07 §1.2); Output lists what came back.";
        c.args = {tf::ArgDesc{"session", tf::ArgType::String, false, "Session name (default: the newest)"}};
        c.headless = false;
        c.canExecute = [this](const tf::CommandContext&) { return !m_recoverable.empty(); };
        c.execute = [this](tf::CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(const tf::JournalSessionInfo session, recoverableSession(ctx));
            auto note = recoverJournal(m_fw, session.path);
            refreshRecoverable();
            if (!note) return std::move(note).error();
            log(*note);
            offerRecoverable();
            return {};
        };
        (void)bus.add(std::move(c));
    }
    {
        // A session that cannot fully replay (its file was edited and saved since: source changed)
        // would otherwise be offered at every start and hide the older ones.
        tf::CommandDesc c;
        c.id = "app.discardSession";
        c.label = "Discard Unsaved Session";
        c.category = "File";
        c.doc = "Stops offering the newest (or the named) earlier unclean session without replaying it. Its journal file "
                "stays, so `helios-tool journal replay <file>` can still replay it.";
        c.args = {tf::ArgDesc{"session", tf::ArgType::String, false, "Session name (default: the newest)"}};
        c.headless = false;
        c.canExecute = [this](const tf::CommandContext&) { return !m_recoverable.empty(); };
        c.execute = [this](tf::CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(const tf::JournalSessionInfo session, recoverableSession(ctx));
            HELIOS_TRY(markSessionEnded(session.path));
            refreshRecoverable();
            log(std::format("Discarded session {} without replaying it; its journal stays at {}", session.header.session,
                            fs::pathToUtf8(session.path)));
            offerRecoverable();
            return {};
        };
        (void)bus.add(std::move(c));
    }
    add("help.about", "About Helios", "Help", "Shows the editor version.", {}, [this](tf::CommandContext&) -> Result<void> {
        m_openAbout = true;
        return {};
    });
}

void Shell::onEvent(const tf::FrameworkEvent& e) {
    using K = tf::FrameworkEvent::Kind;
    const auto name = [&]() -> std::string {
        const tf::Document* d = m_fw.documents().find(e.doc);
        return d ? d->name() : std::string("?");
    };
    switch (e.kind) {
    case K::Committed:
    case K::Undone:
    case K::Redone:
        if (!m_fw.log().empty()) {
            const tf::Transaction& t = m_fw.log().back();
            log(std::format("{} ({})", t.label, tf::originName(t.origin)));
        }
        break;
    case K::Opened: log(std::format("Opened {}", name())); break;
    case K::Saved: log(std::format("Saved {}", name())); break;
    case K::Closed: log("Closed a document"); break;
    case K::Reloaded: log(std::format("Reloaded {} from disk", name())); break;
    case K::Created: break;
    case K::Destroyed: break;
    }
}

void Shell::log(std::string line) {
    m_output.push_back(std::move(line));
    while (m_output.size() > kOutputLines) m_output.pop_front();
}

std::vector<std::string> Shell::panelPaths() const {
    std::vector<std::string> out;
    for (std::string_view id : kPanelIds) {
        if (m_panelOpen.at(std::string(id))) out.emplace_back(id);
    }
    return out;
}

std::string Shell::selectedName() const {
    const tf::Document* d = m_fw.documents().find(m_fw.selection().primaryDocument());
    return d && !d->destroyed() ? d->name() : std::string();
}

std::string Shell::selectionArgs() const {
    const std::string name = selectedName();
    return name.empty() ? std::string() : "{\"doc\":" + tf::json::quote(name) + "}";
}

void Shell::run(const std::string& id, std::string argsJson) {
    const tf::CommandDesc* desc = m_fw.commands().find(id);
    if (!desc) return;
    // Required arguments the caller did not supply: ask for them.
    auto parsed = tf::json::parseObject(argsJson);
    bool missing = false;
    for (const tf::ArgDesc& a : desc->args) {
        if (a.required && (!parsed || !parsed->root().get(a.name).isValid())) missing = true;
    }
    if (missing) {
        openCommandForm(id);
        return;
    }
    auto r = m_host.uiInvoker().invoke(id, argsJson);
    if (!r) {
        log(std::format("{}: {}", desc->label, r.error().message));
    } else if (id == "validate.run") {
        auto doc = refl::JsonDocument::parse(r->result);
        if (doc) {
            log(std::format("Validation: {} error(s)", tf::json::getInteger(doc->root(), "errors").value_or(0)));
            for (refl::JsonValue issue : doc->root().get("issues").elements()) {
                log(std::format("  {}: {}: {}", tf::json::getString(issue, "file").value_or(""), tf::json::getString(issue, "path").value_or(""),
                                tf::json::getString(issue, "message").value_or("")));
            }
        }
    }
}

void Shell::openCommandForm(std::string commandId) {
    const tf::CommandDesc* desc = m_fw.commands().find(commandId);
    if (!desc) return;
    m_formCommand = std::move(commandId);
    m_formArgs.clear();
    m_formError.clear();
    const std::string selected = selectedName();
    for (const tf::ArgDesc& a : desc->args) {
        std::string value;
        if (a.name == "doc") value = selected;
        if (a.name == "name" && !selected.empty() && m_formCommand == "doc.rename") value = selected;
        m_formArgs.emplace_back(a.name, value);
    }
    m_openForm = true;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Shell::handleShortcuts() {
    // Text fields own their keys (Ctrl+Z inside an input is the input's undo).
    if (ImGui::GetIO().WantTextInput) return;
    for (const tf::CommandDesc* c : m_fw.commands().commands()) {
        if (c->shortcut.empty()) continue;
        const ImGuiKeyChord chord = chordFromText(c->shortcut);
        if (chord == 0) continue;
        bool pressed = ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal);
        if (!pressed && c->id == "edit.redo") pressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal);
        if (!pressed) continue;
        const bool needsDoc = std::any_of(c->args.begin(), c->args.end(), [](const tf::ArgDesc& a) { return a.name == "doc" && a.required; });
        if (needsDoc) {
            const std::string args = selectionArgs();
            if (!args.empty()) run(c->id, args);
        } else if (m_host.uiInvoker().canExecute(c->id)) {
            run(c->id);
        }
    }
}

void Shell::draw() {
    handleShortcuts();
    drawMainMenu();
    drawStatusBar();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockId = ImGui::GetID("HeliosMainDock");
    if (m_layoutDirty) {
        buildDefaultLayout(dockId);
        m_layoutDirty = false;
    }
    ImGui::DockSpaceOverViewport(dockId, viewport, ImGuiDockNodeFlags_None);
    drawDocuments();
    drawInspector();
    drawHistory();
    drawOutput();
    drawViewport();
    drawPalette();
    drawCommandForm();
    drawAbout();
}

void Shell::buildDefaultLayout(u32 dockspaceId) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);
    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.16f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.33f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
    ImGui::DockBuilderDockWindow(panelWindow("Documents").c_str(), left);
    ImGui::DockBuilderDockWindow(panelWindow("Inspector").c_str(), right);
    ImGui::DockBuilderDockWindow(panelWindow("History").c_str(), bottom);
    ImGui::DockBuilderDockWindow(panelWindow("Output").c_str(), bottom);
    ImGui::DockBuilderDockWindow(panelWindow("Viewport").c_str(), center);
    ImGui::DockBuilderFinish(dockspaceId);
}

void Shell::drawMenuEntries(const char* menu, const std::vector<MenuEntry>& entries) {
    if (!ImGui::BeginMenu(std::format("{}##{}", tr(menu), menu).c_str())) return;
    for (const MenuEntry& e : entries) {
        const tf::CommandDesc* desc = m_fw.commands().find(e.command);
        if (!desc) continue;
        const std::string args = e.selectionArgs ? selectionArgs() : std::string();
        const bool enabled = e.selectionArgs ? !args.empty() && m_host.uiInvoker().canExecute(e.command, args)
                                             : m_host.uiInvoker().canExecute(e.command) || !desc->args.empty();
        bool checked = false;
        if (e.command.starts_with("window.")) {
            std::string panel = e.command.substr(7);
            panel[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(panel[0])));
            checked = m_panelOpen[panel];
        } else if (e.command.starts_with("view.theme.")) {
            const std::string t = e.command.substr(11);
            checked = (t == "highContrast" ? std::string("high-contrast") : t) == m_host.themeName();
        } else if (e.command.starts_with("view.scale.")) {
            checked = std::lround(m_host.uiScale() * 100.0f) == std::stol(e.command.substr(11));
        }
        std::string label = e.label;
        if (e.command == "edit.undo" && !m_fw.undoLabel().empty()) label = std::format("Undo {}", m_fw.undoLabel());
        if (e.command == "edit.redo" && !m_fw.redoLabel().empty()) label = std::format("Redo {}", m_fw.redoLabel());
        if (ImGui::MenuItem(menuLabel(label.c_str(), e.command).c_str(), desc->shortcut.empty() ? nullptr : desc->shortcut.c_str(), checked,
                            enabled)) {
            run(e.command, args);
        }
        UiTest::annotate(checked ? "checked" : "", desc->doc);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !desc->doc.empty()) ImGui::SetTooltip("%s", desc->doc.c_str());
        if (e.command == "app.quit" || e.command == "doc.saveAll" || e.command == "edit.redo" || e.command == "doc.reload" ||
            e.command == "selection.forward" || e.command == "view.theme.highContrast" || e.command == "view.scale.200") {
            ImGui::Separator();
        }
    }
    ImGui::EndMenu();
}

void Shell::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    for (const char* menu : {"File", "Edit", "View", "Window", "Tools", "Help"}) drawMenuEntries(menu, m_menus[menu]);
    ImGui::EndMainMenuBar();
}

void Shell::drawStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const f32 height = ImGui::GetFrameHeight();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, semanticColorVec4("statusBarBg"));
    if (ImGui::BeginViewportSideBar("##StatusBar", viewport, ImGuiDir_Down, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            // Context accent (07 §1.3): neutral = a local session; shared edit instances and live
            // GM connections (amber, red + shard banner) arrive with T30/T27.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const f32 w = ImGui::GetFontSize() * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y + height),
                                                      ImGui::GetColorU32(semanticColorVec4("accentLocal")));
            ImGui::Dummy(ImVec2(w, 0));
            ImGui::TextUnformatted(tr("LOCAL"));
            ImGui::Separator();
            ImGui::TextUnformatted(m_fw.config().project.c_str());
            usize dirty = 0;
            for (const tf::Document* d : m_fw.documents().documents()) dirty += d->dirty() ? 1 : 0;
            ImGui::Separator();
            if (dirty > 0) ImGui::PushStyleColor(ImGuiCol_Text, semanticColorVec4("dirty"));
            ImGui::TextUnformatted(std::format("{} {}", dirty, tr("unsaved")).c_str());
            if (dirty > 0) ImGui::PopStyleColor();
            const std::string selected = selectedName();
            if (!selected.empty()) {
                ImGui::Separator();
                ImGui::TextUnformatted(selected.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted(std::format("{} | {}%", m_host.themeName(), std::lround(m_host.uiScale() * 100.0f)).c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void Shell::drawDocuments() {
    bool& open = m_panelOpen["Documents"];
    if (!open) return;
    if (ImGui::Begin(panelWindow("Documents").c_str(), &open)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", tr("Filter"), &m_filter);
        UiTest::annotate(m_filter, tr("Filter documents by name"));
        const tf::DocId selected = m_fw.selection().primaryDocument();
        UiTest::pushScope("list");
        if (ImGui::BeginTable("##docs", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn(tr("Table"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            std::vector<const tf::Document*> docs;
            const std::string needle = lowerAscii(m_filter);  // case-insensitive, like the palette
            for (const tf::Document* d : m_fw.documents().documents()) {
                if (d->destroyed()) continue;
                if (!needle.empty() && lowerAscii(d->name()).find(needle) == std::string::npos) continue;
                docs.push_back(d);
            }
            std::sort(docs.begin(), docs.end(), [](const tf::Document* a, const tf::Document* b) { return a->name() < b->name(); });
            for (const tf::Document* d : docs) {
                UiTest::pushScope("row[" + d->name() + "]");
                ImGui::PushID(d->name().c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string label = std::format("{}{}###select", d->name(), d->dirty() ? " *" : "");
                if (ImGui::Selectable(label.c_str(), d->id() == selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    run("selection.set", "{\"doc\":" + tf::json::quote(d->name()) + "}");
                }
                UiTest::annotate(d->dirty() ? "dirty" : "", d->relativePath());
                if (ImGui::BeginPopupContextItem("##context")) {
                    for (const MenuEntry& e : m_documentMenu) {
                        const std::string args = "{\"doc\":" + tf::json::quote(d->name()) + "}";
                        if (ImGui::MenuItem(menuLabel(e.label, e.command).c_str(), nullptr, false, m_host.uiInvoker().canExecute(e.command, args))) {
                            if (e.command == "doc.rename") {
                                (void)m_host.uiInvoker().invoke("selection.set", args);
                                openCommandForm(e.command);
                            } else {
                                run(e.command, args);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                const auto* table = d->type().attr<refl::attrs::Table>();
                ImGui::TextDisabled("%s", table ? std::string(table->name).c_str() : "");
                ImGui::PopID();
                UiTest::popScope();
            }
            ImGui::EndTable();
        }
        UiTest::popScope();
    }
    ImGui::End();
}

void Shell::drawInspector() {
    bool& open = m_panelOpen["Inspector"];
    if (!open) return;
    if (ImGui::Begin(panelWindow("Inspector").c_str(), &open)) {
        const tf::Document* d = m_fw.documents().find(m_fw.selection().primaryDocument());
        if (d && !d->destroyed()) {
            m_grid.draw(m_fw, m_host.uiInvoker(), *d);
        } else {
            ImGui::TextDisabled("%s", tr("Select a record in Documents."));
        }
    }
    ImGui::End();
}

void Shell::drawHistory() {
    bool& open = m_panelOpen["History"];
    if (!open) return;
    if (ImGui::Begin(panelWindow("History").c_str(), &open)) {
        ImGui::BeginDisabled(!m_host.uiInvoker().canExecute("edit.undo"));
        if (ImGui::Button(tr("Undo##undo"))) run("edit.undo");
        UiTest::annotate({}, tr("Undo the last transaction (Ctrl+Z)"));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_host.uiInvoker().canExecute("edit.redo"));
        if (ImGui::Button(tr("Redo##redo"))) run("edit.redo");
        UiTest::annotate({}, tr("Redo (Ctrl+Y)"));
        ImGui::EndDisabled();
        ImGui::Separator();
        UiTest::pushScope("list");
        if (ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_None)) {
            for (const tf::HistoryEntry& e : m_fw.history()) {
                ImGui::PushID(static_cast<int>(e.tx.id.lamport));
                ImGui::BeginDisabled(e.undone);
                UiTest::pushScope(std::format("row[{}]", e.tx.id.lamport));
                ImGui::Selectable(std::format("{}  ({})###entry", e.tx.label, tf::originName(e.tx.origin)).c_str(), false);
                UiTest::annotate(e.undone ? "undone" : "", std::format("{} ops by {}", e.tx.ops.size(), e.tx.author));
                UiTest::popScope();
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        UiTest::popScope();
    }
    ImGui::End();
}

void Shell::drawOutput() {
    bool& open = m_panelOpen["Output"];
    if (!open) return;
    if (ImGui::Begin(panelWindow("Output").c_str(), &open)) {
        if (ImGui::Button(tr("Clear##clear"))) m_output.clear();
        UiTest::annotate({}, tr("Clear the output"));
        ImGui::Separator();
        if (ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            for (const std::string& line : m_output) ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void Shell::drawViewport() {
    bool& open = m_panelOpen["Viewport"];
    if (!open) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin(panelWindow("Viewport").c_str(), &open)) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const u32 w = static_cast<u32>(std::max(1.0f, size.x));
        const u32 h = static_cast<u32>(std::max(1.0f, size.y));
        // An exact-size interactive item with the viewport texture drawn under it (an ImageButton
        // would add frame padding and make the panel scroll).
        ImGui::InvisibleButton("##viewport", ImVec2(static_cast<f32>(w), static_cast<f32>(h)),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (const u64 tex = m_host.viewportTexture(w, h); tex != 0) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(tex), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        UiTest::markViewport();
        UiTest::annotate({}, tr("Viewport (the renderer arrives in Phase 1)"));
        const ImVec2 p = ImGui::GetItemRectMin();
        const f32 pad = ImGui::GetStyle().FramePadding.x * 2;
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad, p.y + pad), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            tr("Viewport: the renderer arrives in Phase 1"));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Shell::drawPalette() {
    const ImGuiID id = ImGui::GetID("##palette");
    if (m_openPalette) {
        m_openPalette = false;
        m_paletteFilter.clear();
        ImGui::OpenPopup(id);
    }
    UiTest::namePopup(id, "Palette");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + ImGui::GetFrameHeight()), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, 0));
    if (!ImGui::BeginPopupEx(id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputTextWithHint("##filter", tr("Type a command"), &m_paletteFilter, ImGuiInputTextFlags_EnterReturnsTrue);
    UiTest::annotate(m_paletteFilter, tr("Filter commands"));
    std::string first;
    const std::string needle = lowerAscii(m_paletteFilter);
    std::vector<const tf::CommandDesc*> matches;
    for (const tf::CommandDesc* c : m_fw.commands().commands()) {
        if (!needle.empty() && lowerAscii(c->label).find(needle) == std::string::npos && lowerAscii(c->id).find(needle) == std::string::npos) continue;
        matches.push_back(c);
    }
    // Every match is listed (07 §4.2: an empty filter lists every command); at most 16 rows show
    // at once and the list scrolls.
    constexpr usize kVisibleRows = 16;
    const f32 rows = static_cast<f32>(std::clamp<usize>(matches.size(), 1, kVisibleRows));
    ImGui::BeginChild("commands", ImVec2(0, rows * ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_None);
    UiTest::pushScope("list");
    // A command runs after the list is drawn: running it may close the popup.
    const tf::CommandDesc* chosen = nullptr;
    for (const tf::CommandDesc* c : matches) {
        if (first.empty()) first = c->id;
        const std::string label = std::format("{}  [{}]###{}", tr(c->label.c_str()), c->category, c->id);
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap)) chosen = c;
        UiTest::annotate(c->id, c->doc);
        if (!c->shortcut.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(c->shortcut.c_str()).x + ImGui::GetCursorPosX());
            ImGui::TextDisabled("%s", c->shortcut.c_str());
        }
    }
    UiTest::popScope();
    ImGui::EndChild();
    if (chosen) {
        ImGui::CloseCurrentPopup();
        run(chosen->id, chosen->args.empty() ? std::string() : selectionArgs());
    }
    if (enter && !first.empty()) {
        ImGui::CloseCurrentPopup();
        const tf::CommandDesc* c = m_fw.commands().find(first);
        run(first, c && !c->args.empty() ? selectionArgs() : std::string());
    }
    ImGui::EndPopup();
}

void Shell::drawCommandForm() {
    const ImGuiID id = ImGui::GetID("##commandForm");
    if (m_openForm) {
        m_openForm = false;
        ImGui::OpenPopup(id);
    }
    UiTest::namePopup(id, "CommandForm");
    if (!ImGui::BeginPopupEx(id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    const tf::CommandDesc* desc = m_fw.commands().find(m_formCommand);
    if (!desc) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted(tr(desc->label.c_str()));
    ImGui::TextDisabled("%s", desc->doc.c_str());
    ImGui::Separator();
    for (usize i = 0; i < m_formArgs.size() && i < desc->args.size(); ++i) {
        const tf::ArgDesc& a = desc->args[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(std::format("{}{}", a.name, a.required ? "" : std::string(" ") + tr("(optional)")).c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24);
        ImGui::InputText(std::format("##{}", a.name).c_str(), &m_formArgs[i].second);
        UiTest::annotate(m_formArgs[i].second, a.doc);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", a.doc.c_str());
        ImGui::PopID();
    }
    if (!m_formError.empty()) ImGui::TextWrapped("%s", m_formError.c_str());
    if (ImGui::Button(tr("Run##run"))) {
        // String arguments are taken as text; everything else as JSON.
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginObject();
        bool ok = true;
        for (usize i = 0; i < m_formArgs.size() && i < desc->args.size(); ++i) {
            const tf::ArgDesc& a = desc->args[i];
            const std::string& v = m_formArgs[i].second;
            if (v.empty() && !a.required) continue;
            w.key(a.name);
            if (a.type == tf::ArgType::String) {
                w.string(v);
            } else if (auto norm = tf::json::normalize(v)) {
                w.raw(*norm);
            } else {
                m_formError = std::format("{}: {}", a.name, norm.error().message);
                ok = false;
                w.null();
            }
        }
        w.endObject();
        if (ok) {
            auto r = m_host.uiInvoker().invoke(m_formCommand, w.take());
            if (r) {
                ImGui::CloseCurrentPopup();
            } else {
                m_formError = r.error().message;
            }
        }
    }
    UiTest::annotate({}, tr("Run the command"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel##cancel"))) ImGui::CloseCurrentPopup();
    UiTest::annotate({}, tr("Close without running"));
    ImGui::EndPopup();
}

void Shell::drawAbout() {
    const ImGuiID id = ImGui::GetID("##about");
    if (m_openAbout) {
        m_openAbout = false;
        ImGui::OpenPopup(id);
    }
    UiTest::namePopup(id, "About");
    if (!ImGui::BeginPopupEx(id, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextUnformatted(std::format("Helios Editor {}.{}.{}", HELIOS_VERSION_MAJOR, HELIOS_VERSION_MINOR, HELIOS_VERSION_PATCH).c_str());
    ImGui::TextUnformatted(std::format("Dear ImGui {}", IMGUI_VERSION).c_str());
    ImGui::TextDisabled("%s", tr("MIT licence. See LICENSE and third_party/MANIFEST.md."));
    if (ImGui::Button(tr("Close##close"))) ImGui::CloseCurrentPopup();
    UiTest::annotate({}, tr("Close"));
    ImGui::EndPopup();
}

} // namespace helios::edui
