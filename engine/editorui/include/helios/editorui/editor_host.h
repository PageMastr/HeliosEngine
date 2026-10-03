#pragma once
// EditorHost: the helios-editor process (07 §1.1, §1.3). It owns the SDL3 window, the RHI device
// and swapchain, the Dear ImGui context with its SDL3 platform backend and the Helios RHI renderer
// backend, the ToolsFramework, the remote-control socket and the Shell, and runs the frame loop.
//
// Each rendered frame is a render graph: an optional "Viewport" pass (the Phase 0 placeholder grid,
// a bindless texture the shell shows as an image), the "EditorUI" pass (ImGui draw lists into the
// persistent RGBA8 EditorFrame texture), an optional "Capture" copy into a readback buffer
// (screenshots and helios-uitest captures) and the "Present" pass onto the swapchain image.
//
// Test mode (helios-uitest, 07 §4.4) makes runs deterministic: a fixed 1/60 s clock, the embedded
// editor font only, no cursor blink, no OS input (only ui.* events injected through the SDL3
// backend seam), no layout file, a hidden window unless `present` is set, and frames rendered only
// when a capture asks for one. Its edits carry Origin::UiScripted.
//
// Remote control: the framework's JSON-RPC methods (tf::registerFrameworkRpc) plus
//   ui.items {filter?}, ui.click {path, button?}, ui.doubleClick {path}, ui.hover {path},
//   ui.move {x, y}, ui.drag {from, to? | dx, dy}, ui.type {text}, ui.key {chord},
//   ui.scroll {path, steps}, ui.waitFor {path, enabled?, value?, timeoutFrames?},
//   ui.frames {count}, ui.capture {file, target?}, ui.lint, ui.state,
//   ui.configure {theme?, scale?, pseudoLoc?, resetLayout?}, app.quit.
// ui.* actions complete after whole frames; their responses arrive then.
//
// Threading: everything runs on the thread that calls create() and run() (the UI thread); the
// RPC server only reads sockets on its own threads.

#include <memory>
#include <string>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/editorui/shell.h"

namespace helios::tf {
class Framework;
}

namespace helios::edui {

struct EditorConfig {
    std::string project = "project";
    /// Project root (records/<table>/*.hrec are opened at start).
    fs::Path projectRoot;
    std::string user = "local";
    /// Deterministic test mode (see the header comment).
    bool testMode = false;
    /// Show the window and present frames (test mode: off unless requested).
    bool present = true;
    /// UI scale (1 = 100 %); 0 = the display's content scale (test mode: 1).
    f32 scale = 0.0f;
    std::string theme = "dark";
    /// A theme token file to use instead of the built-in theme.
    fs::Path themeFile;
    /// Remote-control endpoint name ("" = helios-editor-<pid>; "-" = none).
    std::string rpcEndpoint;
    /// Window size in DIPs (pixels at 100 %).
    u32 width = 1600;
    u32 height = 900;
    /// Stop after this many frames (0 = until closed).
    u64 maxFrames = 0;
    /// Save the last frame as PNG (needs maxFrames).
    fs::Path screenshot;
    bool validation = false;
    bool journal = true;
    fs::Path journalRoot;
    /// Journal to replay at start ("auto" = the newest unclean session of the project).
    std::string recover;
    /// Pseudo-localized UI text (layout testing).
    bool pseudoLoc = false;
    /// Framework clock and keyed-list keys fixed (test mode sets these).
    bool deterministicIds = false;
};

class EditorHost final : public ShellHost {
public:
    /// Opens the window, device, framework, remote control and shell. Fails with a message
    /// suitable for the user (no display, no Vulkan device, bad project, ...).
    static Result<std::unique_ptr<EditorHost>> create(const EditorConfig& config);
    ~EditorHost() override;
    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    /// Runs frames until quit or maxFrames. Returns the process exit code (0 ok, 1 failure,
    /// 2 RHI validation errors).
    int run();
    /// Runs one frame; false once the editor should exit.
    bool frame();

    tf::Framework& framework() noexcept;
    Shell& shell() noexcept;
    u64 frameCount() const noexcept;
    /// The RPC endpoint name ("" when remote control is off).
    const std::string& endpoint() const noexcept;

    // ShellHost
    tf::CommandInvoker& uiInvoker() override;
    std::string_view themeName() const override;
    f32 uiScale() const override;
    void requestTheme(std::string_view name) override;
    void requestScale(f32 scale) override;
    void requestQuit() override;
    u64 viewportTexture(u32 width, u32 height) override;

    struct Impl;  ///< Internal.

private:
    explicit EditorHost(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

} // namespace helios::edui
