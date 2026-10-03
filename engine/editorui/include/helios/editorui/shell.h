#pragma once
// The editor shell (07 §1.3): main menu, dock space with the default layout, panels (Documents,
// Inspector with the property grid, History, Output, Viewport), status bar with the context accent
// (neutral = local session), command palette (Ctrl+Shift+P) and a generic argument form for
// commands. Every action is a ToolsFramework command run through the host's UI invoker; the menus
// register what they expose with the command bus (the "command no menu exposes" layout lint).
//
// Threading: UI thread, inside an ImGui frame.

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"
#include "helios/editorui/property_grid.h"

namespace helios::tf {
class Framework;
class CommandInvoker;
struct FrameworkEvent;
} // namespace helios::tf

namespace helios::edui {

/// What the shell needs from the process that hosts it (EditorHost, or a test harness).
class ShellHost {
public:
    virtual ~ShellHost() = default;
    /// The invoker of the UI input path: Origin::Ui for a human, Origin::UiScripted while
    /// helios-uitest drives the editor.
    virtual tf::CommandInvoker& uiInvoker() = 0;
    virtual std::string_view themeName() const = 0;
    virtual f32 uiScale() const = 0;
    /// Applied before the next frame.
    virtual void requestTheme(std::string_view name) = 0;
    virtual void requestScale(f32 scale) = 0;
    virtual void requestQuit() = 0;
    /// A texture of `width` x `height` pixels showing the viewport this frame (0 = none).
    virtual u64 viewportTexture(u32 width, u32 height) = 0;
};

/// Panel window names ("<title>###<id>"); the id part is the panel's stable path.
inline constexpr std::string_view kPanelIds[] = {"Documents", "Inspector", "History", "Output", "Viewport"};

class Shell {
public:
    Shell(tf::Framework& framework, ShellHost& host);
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    /// Draws one frame of the shell (between ImGui::NewFrame and ImGui::Render).
    void draw();
    /// Rebuilds the default dock layout on the next frame.
    void resetLayout() noexcept { m_layoutDirty = true; }
    /// Panel window paths (for the keyboard-reachability lint).
    std::vector<std::string> panelPaths() const;
    /// Adds a line to the Output panel.
    void log(std::string line);
    const std::deque<std::string>& output() const noexcept { return m_output; }
    /// Opens the command palette / the argument form of a command (UI tests use the commands).
    void openPalette() noexcept { m_openPalette = true; }
    void openCommandForm(std::string commandId);

private:
    struct MenuEntry {
        std::string command;
        const char* label;
        bool selectionArgs;  ///< Pass {"doc": <selected>}.
    };
    void registerCommands();
    void onEvent(const tf::FrameworkEvent& e);
    void handleShortcuts();
    void drawMainMenu();
    void drawMenuEntries(const char* menu, const std::vector<MenuEntry>& entries);
    void drawStatusBar();
    void buildDefaultLayout(u32 dockspaceId);
    void drawDocuments();
    void drawInspector();
    void drawHistory();
    void drawOutput();
    void drawViewport();
    void drawPalette();
    void drawCommandForm();
    void drawAbout();
    /// Runs a command through the UI invoker; commands with unfilled required arguments open the form.
    void run(const std::string& id, std::string argsJson = {});
    std::string selectionArgs() const;
    std::string selectedName() const;

    tf::Framework& m_fw;
    ShellHost& m_host;
    PropertyGrid m_grid;
    std::map<std::string, std::vector<MenuEntry>, std::less<>> m_menus;
    std::vector<MenuEntry> m_documentMenu;
    std::map<std::string, bool, std::less<>> m_panelOpen;
    std::deque<std::string> m_output;
    std::string m_filter;
    std::string m_paletteFilter;
    bool m_layoutDirty = true;
    bool m_openPalette = false;
    bool m_openAbout = false;
    bool m_openForm = false;
    std::string m_formCommand;
    std::vector<std::pair<std::string, std::string>> m_formArgs;
    std::string m_formError;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

} // namespace helios::edui
