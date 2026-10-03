#pragma once
// Shared helpers of the editorui tests: a headless Dear ImGui context (no window, no GPU; textures
// are acknowledged as if a renderer had uploaded them), a ShellHost stub and a harness that runs
// the shell with the UI test hooks, injecting ui.* input straight into ImGui's input queue.

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include "helios/core/fs.h"
#include "helios/editorui/shell.h"
#include "helios/editorui/theme.h"
#include "helios/editorui/ui_test.h"
#include "helios/reflect/path.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/samples.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace helios::edui::test {

/// A fresh directory under the test work directory.
inline fs::Path freshDir(std::string_view name) {
    const fs::Path dir = fs::pathFromUtf8(HELIOS_EDUI_TEST_WORK_DIR) / fs::pathFromUtf8(name);
    (void)fs::removeAll(dir);
    REQUIRE(fs::createDirectories(dir));
    return dir;
}

/// A copy of the UI fixture project (tests/fixture) in a fresh directory.
inline fs::Path fixtureCopy(std::string_view name) {
    const fs::Path dir = freshDir(name);
    std::error_code ec;
    std::filesystem::copy(fs::pathFromUtf8(HELIOS_EDUI_FIXTURE_DIR), dir, std::filesystem::copy_options::recursive, ec);
    REQUIRE_FALSE(ec);
    return dir;
}

/// A headless ImGui context: fixed display size and time step, the embedded editor font, and a
/// pretend renderer that acknowledges texture requests.
class HeadlessImGui {
public:
    explicit HeadlessImGui(f32 width = 1280, f32 height = 800) {
        m_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(width, height);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigInputTextCursorBlink = false;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        addEditorFont(io);
    }
    ~HeadlessImGui() {
        ImGui::SetCurrentContext(m_ctx);
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGui::DestroyContext(m_ctx);
    }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;

    ImGuiContext* context() const noexcept { return m_ctx; }

    /// After ImGui::Render(): mark every texture request as done.
    static void acknowledgeTextures() {
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
            if (t->Status == ImTextureStatus_WantCreate) {
                t->SetTexID(static_cast<ImTextureID>(1));
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantUpdates) {
                t->SetStatus(ImTextureStatus_OK);
            } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
                t->SetTexID(ImTextureID_Invalid);
                t->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

private:
    ImGuiContext* m_ctx = nullptr;
};

/// ImGui key of a ui_test key name.
inline ImGuiKey imguiKey(std::string_view name) {
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'a'));
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (name[0] - '0'));
    if (name == "enter") return ImGuiKey_Enter;
    if (name == "escape") return ImGuiKey_Escape;
    if (name == "tab") return ImGuiKey_Tab;
    if (name == "backspace") return ImGuiKey_Backspace;
    if (name == "delete") return ImGuiKey_Delete;
    if (name == "ctrl") return ImGuiKey_LeftCtrl;
    if (name == "shift") return ImGuiKey_LeftShift;
    if (name == "alt") return ImGuiKey_LeftAlt;
    if (name == "f2") return ImGuiKey_F2;
    if (name == "left") return ImGuiKey_LeftArrow;
    if (name == "right") return ImGuiKey_RightArrow;
    return ImGuiKey_None;
}

/// Feeds a ui_test event into the current context's input queue (what the SDL3 backend does).
inline void injectIntoImGui(const UiInputEvent& e) {
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
    case UiInputEvent::Type::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
    case UiInputEvent::Type::MouseDown: io.AddMouseButtonEvent(e.button, true); break;
    case UiInputEvent::Type::MouseUp: io.AddMouseButtonEvent(e.button, false); break;
    case UiInputEvent::Type::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
    case UiInputEvent::Type::KeyDown:
    case UiInputEvent::Type::KeyUp: {
        const bool down = e.type == UiInputEvent::Type::KeyDown;
        io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & kModCtrl) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (e.mods & kModShift) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (e.mods & kModAlt) != 0);
        io.AddKeyEvent(imguiKey(e.key), down);
        break;
    }
    case UiInputEvent::Type::Text: io.AddInputCharactersUTF8(e.text.c_str()); break;
    case UiInputEvent::Type::Focus: io.AddFocusEvent(e.focused); break;
    }
}

/// ShellHost stub recording what the shell asks for.
struct StubHost final : ShellHost {
    explicit StubHost(tf::Framework& fw) : framework(fw) {}
    tf::CommandInvoker& uiInvoker() override { return framework.invoker(tf::Origin::UiScripted); }
    std::string_view themeName() const override { return theme; }
    f32 uiScale() const override { return scale; }
    void requestTheme(std::string_view name) override { requestedTheme = std::string(name); }
    void requestScale(f32 s) override { requestedScale = s; }
    void requestQuit() override { quit = true; }
    u64 viewportTexture(u32 w, u32 h) override {
        viewportSize = {w, h};
        return 7;
    }

    tf::Framework& framework;
    std::string theme = "dark";
    f32 scale = 1.0f;
    std::optional<std::string> requestedTheme;
    std::optional<f32> requestedScale;
    bool quit = false;
    std::pair<u32, u32> viewportSize{0, 0};
};

/// A framework over a copy of the fixture, the shell, the harness and a headless ImGui context.
struct ShellHarness {
    fs::Path root;
    std::unique_ptr<tf::Framework> fw;
    std::unique_ptr<HeadlessImGui> imgui;
    std::unique_ptr<StubHost> host;
    std::unique_ptr<Shell> shell;
    std::unique_ptr<UiTest> ui;
    Theme theme;

    explicit ShellHarness(std::string_view name, f32 width = 1920, f32 height = 1080) {
        REQUIRE(tf::samples::registerSampleTypes());
        root = fixtureCopy(name);
        tf::FrameworkConfig cfg;
        cfg.project = "edui-test";
        cfg.projectRoot = root;
        cfg.journal = false;
        auto created = tf::Framework::create(cfg);
        REQUIRE(created);
        fw = std::move(*created);
        REQUIRE(fw->openAll());
        imgui = std::make_unique<HeadlessImGui>(width, height);
        host = std::make_unique<StubHost>(*fw);
        shell = std::make_unique<Shell>(*fw, *host);
        ui = std::make_unique<UiTest>(imgui->context(), [](const UiInputEvent& e) { injectIntoImGui(e); });
        ui->setEnabled(true);
        auto t = builtinTheme("dark");
        REQUIRE(t);
        theme = *t;
        applyTheme(theme, 1.0f, ImGui::GetStyle());
        for (int i = 0; i < 3; ++i) frame();
    }
    ~ShellHarness() {
        ui.reset();
        shell.reset();
    }

    void frame() {
        ImGui::SetCurrentContext(imgui->context());
        ui->beginFrame();
        ImGui::NewFrame();
        shell->draw();
        ImGui::Render();
        ui->endFrame();
        HeadlessImGui::acknowledgeTextures();
    }

    /// Runs frames until every queued action finished (at most `limit` frames).
    void settle(int limit = 600) {
        for (int i = 0; i < limit && ui->pendingActions() > 0; ++i) frame();
        frame();
    }

    /// Queues `action` (a UiTest call taking a Completion) and runs it; returns its result.
    template <class F>
    Result<std::string> act(F&& action) {
        std::optional<Result<std::string>> out;
        action([&](const Result<std::string>& r) { out = r; });
        settle();
        REQUIRE(out.has_value());
        return *out;
    }

    const tf::Document& frigate() const { return *fw->documents().find(std::string_view("hull/frigate")); }
    std::string get(std::string_view path) const {
        auto v = refl::getJson(frigate().type(), frigate().object(), path);
        REQUIRE(v);
        return *v;
    }
};

} // namespace helios::edui::test
