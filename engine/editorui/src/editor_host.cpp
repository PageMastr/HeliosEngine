#include "helios/editorui/editor_host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/random.h"
#include "display_scale.h"
#include "editor_pipelines.h"
#include "helios/editorui/imgui_renderer.h"
#include "helios/editorui/localize.h"
#include "helios/editorui/theme.h"
#include "helios/editorui/ui_test.h"
#include "helios/render/image.h"
#include "helios/render/render_graph.h"
#include "helios/rhi/rhi.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/journal.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/rpc.h"
#include "helios_editorui_shaders.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"

namespace helios::edui {

namespace {

constexpr f32 kFixedDelta = 1.0f / 60.0f;
/// Frames test mode runs at start before it waits for ui.* actions (dock layout, font atlas).
constexpr u64 kSettleFrames = 3;
constexpr rhi::Format kFrameFormat = rhi::Format::RGBA8Unorm;

struct PresentPush {
    u32 source;
    u32 decodeSrgb;
};

struct GridPush {
    f32 background[4];
    f32 lineColor[4];
    f32 spacing;
    f32 pad[3];
};
static_assert(sizeof(GridPush) == 48);

bool isSrgb(rhi::Format f) noexcept {
    return f == rhi::Format::BGRA8Srgb || f == rhi::Format::RGBA8Srgb;
}

struct SdlKey {
    SDL_Keycode key = SDLK_UNKNOWN;
    SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
};

/// SDL key and scancode of a ui_test key name (parseChord() spelling).
std::optional<SdlKey> sdlKey(std::string_view name) {
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        const int i = name[0] - 'a';
        return SdlKey{static_cast<SDL_Keycode>(SDLK_A + static_cast<SDL_Keycode>(i)), static_cast<SDL_Scancode>(SDL_SCANCODE_A + i)};
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        const int d = name[0] - '0';
        return SdlKey{static_cast<SDL_Keycode>(SDLK_0 + static_cast<SDL_Keycode>(d)),
                      d == 0 ? SDL_SCANCODE_0 : static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (d - 1))};
    }
    if (name.size() >= 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
        int n = 0;
        for (usize i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') return std::nullopt;
            n = n * 10 + (name[i] - '0');
        }
        if (n < 1 || n > 12) return std::nullopt;
        return SdlKey{static_cast<SDL_Keycode>(SDLK_F1 + static_cast<SDL_Keycode>(n - 1)), static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + (n - 1))};
    }
    static constexpr std::pair<std::string_view, SdlKey> kNamed[] = {
        {"enter", {SDLK_RETURN, SDL_SCANCODE_RETURN}},        {"escape", {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE}},
        {"tab", {SDLK_TAB, SDL_SCANCODE_TAB}},                {"backspace", {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE}},
        {"delete", {SDLK_DELETE, SDL_SCANCODE_DELETE}},       {"space", {SDLK_SPACE, SDL_SCANCODE_SPACE}},
        {"up", {SDLK_UP, SDL_SCANCODE_UP}},                   {"down", {SDLK_DOWN, SDL_SCANCODE_DOWN}},
        {"left", {SDLK_LEFT, SDL_SCANCODE_LEFT}},             {"right", {SDLK_RIGHT, SDL_SCANCODE_RIGHT}},
        {"home", {SDLK_HOME, SDL_SCANCODE_HOME}},             {"end", {SDLK_END, SDL_SCANCODE_END}},
        {"pageup", {SDLK_PAGEUP, SDL_SCANCODE_PAGEUP}},       {"pagedown", {SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN}},
        {"ctrl", {SDLK_LCTRL, SDL_SCANCODE_LCTRL}},           {"shift", {SDLK_LSHIFT, SDL_SCANCODE_LSHIFT}},
        {"alt", {SDLK_LALT, SDL_SCANCODE_LALT}},              {"super", {SDLK_LGUI, SDL_SCANCODE_LGUI}},
    };
    for (const auto& [n, k] : kNamed) {
        if (n == name) return k;
    }
    return std::nullopt;
}

SDL_Keymod sdlMods(u32 mods) noexcept {
    u32 m = SDL_KMOD_NONE;
    if (mods & kModCtrl) m |= SDL_KMOD_LCTRL;
    if (mods & kModShift) m |= SDL_KMOD_LSHIFT;
    if (mods & kModAlt) m |= SDL_KMOD_LALT;
    if (mods & kModSuper) m |= SDL_KMOD_LGUI;
    return static_cast<SDL_Keymod>(m);
}

std::string rectJson(i64 x, i64 y, i64 w, i64 h) {
    return std::format("[{},{},{},{}]", x, y, w, h);
}

} // namespace

struct EditorHost::Impl {
    EditorConfig config;
    EditorHost* self = nullptr;

    SDL_Window* window = nullptr;
    bool sdlInit = false;
    std::unique_ptr<rhi::Device> device;
    rhi::SwapchainH swapchain;
    rhi::SwapchainInfo swapInfo;
    bool needResize = false;
    rhi::PipelineH presentPipeline;
    rhi::Format presentFormat = rhi::Format::Unknown;
    rhi::PipelineH gridPipeline;
    std::unique_ptr<render::RgResourcePool> pool;
    rhi::TextureH frameTex;
    u32 frameW = 0, frameH = 0;
    rhi::TextureH viewportTex;
    u32 viewportW = 0, viewportH = 0;
    bool viewportUsed = false;
    rhi::BufferH readback;
    u64 readbackSize = 0;

    ImGuiContext* imgui = nullptr;
    bool platformInit = false;
    std::unique_ptr<ImGuiRenderer> renderer;
    std::string iniPath;

    std::unique_ptr<tf::Framework> fw;
    std::unique_ptr<tf::RpcServer> rpc;
    std::unique_ptr<Shell> shell;
    std::unique_ptr<UiTest> uiTest;

    Theme theme;
    std::string themeName;
    f32 scale = 1.0f;
    /// Whether display changes still move the scale (not after --scale, HELIOS_EDITOR_SCALE, test
    /// mode or a View > UI Scale choice).
    detail::ScalePolicy scalePolicy;
    bool styleDirty = true;
    std::optional<std::string> pendingTheme;
    std::optional<f32> pendingScale;
    bool quit = false;
    u64 frames = 0;
    int exitCode = 0;

    ~Impl() { shutdown(); }

    bool presenting() const noexcept { return swapchain.isValid(); }

    /// Framebuffer size: DIP size x scale in test mode, the window's pixel size otherwise.
    void displayPixels(u32& w, u32& h) const {
        if (config.testMode || !window) {
            w = static_cast<u32>(std::lround(static_cast<f32>(config.width) * scale));
            h = static_cast<u32>(std::lround(static_cast<f32>(config.height) * scale));
            return;
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        w = static_cast<u32>(std::max(pw, 1));
        h = static_cast<u32>(std::max(ph, 1));
    }

    Result<void> ensurePresentPipeline(rhi::Format format) {
        if (presentPipeline.isValid() && presentFormat == format) return {};
        device->destroy(presentPipeline);
        rhi::GraphicsPipelineDesc desc;
        desc.vertex = rhi::ShaderDesc{helios_editorui_shaders::present(), "vsFullscreen"};
        desc.fragment = rhi::ShaderDesc{helios_editorui_shaders::present(), "psPresent"};
        desc.colorCount = 1;
        desc.colorFormats[0] = format;
        desc.name = "EditorPresent";
        HELIOS_TRY_ASSIGN(presentPipeline, detail::editorPipeline(*device, desc));
        presentFormat = format;
        return {};
    }

    Result<void> ensureGridPipeline() {
        rhi::GraphicsPipelineDesc desc;
        desc.vertex = rhi::ShaderDesc{helios_editorui_shaders::viewport(), "vsFullscreen"};
        desc.fragment = rhi::ShaderDesc{helios_editorui_shaders::viewport(), "psGrid"};
        desc.colorCount = 1;
        desc.colorFormats[0] = kFrameFormat;
        desc.name = "EditorViewportGrid";
        HELIOS_TRY_ASSIGN(gridPipeline, detail::editorPipeline(*device, desc));
        return {};
    }

    Result<void> ensureFrameTexture(u32 w, u32 h) {
        if (frameTex.isValid() && frameW == w && frameH == h) return {};
        device->destroy(frameTex);
        frameTex = {};
        HELIOS_TRY_ASSIGN(frameTex, device->createTexture(rhi::TextureDesc::tex2D(
                                        kFrameFormat, w, h,
                                        rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc,
                                        "EditorFrame")));
        frameW = w;
        frameH = h;
        return {};
    }

    Result<void> ensureReadback(u64 bytes) {
        if (readback.isValid() && readbackSize >= bytes) return {};
        device->destroy(readback);
        readback = {};
        HELIOS_TRY_ASSIGN(readback, device->createBuffer({.size = bytes,
                                                          .usage = rhi::BufferUsage::TransferDst,
                                                          .memory = rhi::MemoryUsage::Readback,
                                                          .name = "EditorCapture"}));
        readbackSize = bytes;
        return {};
    }

    Result<void> loadTheme(std::string_view name) {
        if (!config.themeFile.empty() && name == "file") {
            HELIOS_TRY_ASSIGN(const std::string text, fs::readTextFile(config.themeFile));
            HELIOS_TRY_ASSIGN(theme, Theme::parse(text, fs::pathToUtf8(config.themeFile)));
        } else {
            HELIOS_TRY_ASSIGN(theme, builtinTheme(name));
        }
        themeName = theme.name;
        styleDirty = true;
        return {};
    }

    /// Applies requested theme / scale changes before the next ImGui frame.
    void applyPending() {
        if (pendingTheme) {
            if (auto r = loadTheme(*pendingTheme); !r) {
                HELIOS_LOG_WARN("theme '{}': {}", *pendingTheme, r.error());
                if (shell) shell->log(std::format("Theme '{}': {}", *pendingTheme, r.error().message));
            }
            pendingTheme.reset();
        }
        if (pendingScale) {
            const f32 s = std::clamp(*pendingScale, detail::kMinUiScale, detail::kMaxUiScale);
            if (s != scale) {
                scale = s;
                styleDirty = true;
                // Test mode resizes the window with the scale, so the default layout is rebuilt to
                // keep the panels' proportions for the goldens. A user's own dock layout is kept.
                if (shell && config.testMode) shell->resetLayout();
                if (config.testMode && window && presenting()) {
                    u32 w = 0, h = 0;
                    displayPixels(w, h);
                    SDL_SetWindowSize(window, static_cast<int>(w), static_cast<int>(h));
                    SDL_SyncWindow(window);
                    needResize = true;
                }
            }
            pendingScale.reset();
        }
        if (!styleDirty) return;
        ImGuiStyle& style = ImGui::GetStyle();
        applyTheme(theme, scale, style);
        if (config.testMode) {
            // No time-based effects in captures: tooltips appear on the first hovered frame.
            style.HoverDelayShort = 0.0f;
            style.HoverDelayNormal = 0.0f;
            style.HoverStationaryDelay = 0.0f;
        }
        styleDirty = false;
    }

    void inject(const UiInputEvent& ev) {
        if (!window) return;
        const SDL_WindowID id = SDL_GetWindowID(window);
        SDL_Event e;
        std::memset(&e, 0, sizeof(e));
        switch (ev.type) {
        case UiInputEvent::Type::MouseMove:
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = id;
            e.motion.x = ev.x;
            e.motion.y = ev.y;
            break;
        case UiInputEvent::Type::MouseDown:
        case UiInputEvent::Type::MouseUp:
            e.type = ev.type == UiInputEvent::Type::MouseDown ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = id;
            e.button.button = static_cast<Uint8>(ev.button == 0 ? SDL_BUTTON_LEFT : ev.button == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE);
            e.button.down = ev.type == UiInputEvent::Type::MouseDown;
            e.button.clicks = 1;
            e.button.x = ev.x;
            e.button.y = ev.y;
            break;
        case UiInputEvent::Type::Wheel:
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = id;
            e.wheel.x = ev.x;
            e.wheel.y = ev.y;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            break;
        case UiInputEvent::Type::KeyDown:
        case UiInputEvent::Type::KeyUp: {
            const auto k = sdlKey(ev.key);
            if (!k) {
                HELIOS_LOG_WARN("ui input: no SDL key for '{}'", ev.key);
                return;
            }
            e.type = ev.type == UiInputEvent::Type::KeyDown ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = id;
            e.key.key = k->key;
            e.key.scancode = k->scancode;
            e.key.mod = sdlMods(ev.mods);
            e.key.down = ev.type == UiInputEvent::Type::KeyDown;
            break;
        }
        case UiInputEvent::Type::Text:
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = id;
            e.text.text = ev.text.c_str();  // read synchronously by ProcessEvent
            break;
        case UiInputEvent::Type::Focus:
            e.type = ev.focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
            e.window.windowID = id;
            break;
        }
        ImGui_ImplSDL3_ProcessEvent(&e);
    }

    /// Renders this frame: viewport placeholder, UI, optional capture copy, present.
    Result<void> render(ImDrawData* dd, const UiTest::CaptureRequest* shot, bool screenshotOnly) {
        u32 w = 0, h = 0;
        displayPixels(w, h);
        HELIOS_TRY(ensureFrameTexture(w, h));
        HELIOS_TRY(renderer->prepare(dd));
        if (shot) HELIOS_TRY(ensureReadback(static_cast<u64>(w) * h * 4));

        render::RenderGraph graph("EditorFrame");
        render::RgTexture frame = graph.importTexture("EditorFrame", frameTex, device->textureDesc(frameTex));
        render::RgTexture viewport;
        if (viewportUsed && viewportTex.isValid() && gridPipeline.isValid()) {
            viewport = graph.importTexture("EditorViewport", viewportTex, device->textureDesc(viewportTex));
            GridPush push{};
            const Color bg = theme.color("viewportBg");
            const Color line = theme.color("viewportGrid");
            const f32 bgv[4] = {bg.r, bg.g, bg.b, 1.0f};
            const f32 lv[4] = {line.r, line.g, line.b, 1.0f};
            std::memcpy(push.background, bgv, sizeof(bgv));
            std::memcpy(push.lineColor, lv, sizeof(lv));
            push.spacing = std::max(4.0f, std::round(32.0f * scale));
            const rhi::PipelineH pipeline = gridPipeline;
            graph.addPass(
                "Viewport", render::PassFlags::Raster,
                [&](render::RgBuilder& b) { viewport = b.colorAttachment(viewport, 0, rhi::LoadOp::Clear, {bg.r, bg.g, bg.b, 1.0f}); },
                [pipeline, push](render::RgContext& ctx) {
                    ctx.cmd().bindPipeline(pipeline);
                    ctx.cmd().pushConstants(push);
                    ctx.cmd().draw(3);
                });
        }
        const Color clear = theme.color("windowBg");
        ImGuiRenderer* r = renderer.get();
        graph.addPass(
            "EditorUI", render::PassFlags::Raster,
            [&](render::RgBuilder& b) {
                if (viewport.isValid()) b.read(viewport, render::TextureRead::Sampled);
                frame = b.colorAttachment(frame, 0, rhi::LoadOp::Clear, {clear.r, clear.g, clear.b, 1.0f});
            },
            [r, dd](render::RgContext& ctx) { r->record(ctx.cmd(), dd); });
        if (shot) {
            render::RgImport readable;
            readable.finalState = rhi::ResourceState::HostRead;
            render::RgBuffer rb = graph.importBuffer("EditorCapture", readback, device->bufferDesc(readback), readable);
            graph.addPass(
                "Capture", render::PassFlags::Copy | render::PassFlags::NeverCull,
                [&](render::RgBuilder& b) {
                    b.read(frame, render::TextureRead::CopySource);
                    rb = b.write(rb, render::BufferWrite::CopyDest);
                },
                [frame, rb, w, h](render::RgContext& ctx) {
                    rhi::TextureRegion region;
                    region.width = w;
                    region.height = h;
                    region.depth = 1;
                    ctx.cmd().copyTextureToBuffer(ctx.texture(frame), region, ctx.buffer(rb), {});
                });
        }
        bool present = false;
        if (presenting() && !screenshotOnly) {
            auto acquired = device->acquireNextImage(swapchain);
            if (!acquired) return std::move(acquired).error();
            if (acquired->status == rhi::SwapchainStatus::OutOfDate) {
                needResize = true;
            } else {
                if (acquired->status == rhi::SwapchainStatus::Suboptimal) needResize = true;
                const rhi::SwapchainImage image = *acquired;
                render::RgTexture back = graph.importTexture(
                    "Backbuffer", image.texture, device->textureDesc(image.texture),
                    {.finalState = rhi::ResourceState::Present, .waitFor = image.ready});
                const rhi::PipelineH pipeline = presentPipeline;
                const u32 decode = isSrgb(swapInfo.format) ? 1u : 0u;
                graph.addPass(
                    "Present", render::PassFlags::Raster,
                    [&](render::RgBuilder& b) {
                        b.read(frame, render::TextureRead::Sampled);
                        back = b.colorAttachment(back, 0, rhi::LoadOp::Clear);
                    },
                    [pipeline, frame, decode](render::RgContext& ctx) {
                        ctx.cmd().bindPipeline(pipeline);
                        ctx.cmd().pushConstants(PresentPush{ctx.srv(frame), decode});
                        ctx.cmd().draw(3);
                    });
                present = true;
            }
        }
        HELIOS_TRY(graph.compile());
        HELIOS_TRY_ASSIGN(const render::RgExecuteResult done, graph.execute(*device, *pool));
        if (present) {
            HELIOS_TRY_ASSIGN(const rhi::SwapchainStatus status, device->present(swapchain, done.graphics()));
            if (status != rhi::SwapchainStatus::Ok) needResize = true;
        }
        if (!shot) return {};
        HELIOS_TRY(device->wait(done.graphics()));
        device->invalidateMapped(readback);
        const auto* pixels = static_cast<const u8*>(device->map(readback));
        if (!pixels) return Error{ErrorCode::InvalidState, "capture: readback buffer is not mapped"};
        // Crop to the request (display pixels == framebuffer pixels: the host scales ImGui itself).
        const i64 x0 = std::clamp<i64>(static_cast<i64>(std::floor(shot->rect.x)), 0, w);
        const i64 y0 = std::clamp<i64>(static_cast<i64>(std::floor(shot->rect.y)), 0, h);
        const i64 x1 = std::clamp<i64>(static_cast<i64>(std::ceil(shot->rect.right())), x0, w);
        const i64 y1 = std::clamp<i64>(static_cast<i64>(std::ceil(shot->rect.bottom())), y0, h);
        if (x1 <= x0 || y1 <= y0) return Error{ErrorCode::InvalidArgument, "capture: empty rectangle"};
        render::ImageRgba8 image(static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0));
        for (i64 y = y0; y < y1; ++y) {
            std::memcpy(image.at(0, static_cast<u32>(y - y0)), pixels + (static_cast<usize>(y) * w + static_cast<usize>(x0)) * 4,
                        static_cast<usize>(x1 - x0) * 4);
        }
        for (usize i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
        // Viewports are masked: their content belongs to the renderer's goldens (03 §8.4).
        std::string masks = "[";
        if (!screenshotOnly) {
            for (const UiItem& item : uiTest->items()) {
                if (item.kind != "viewport" || !item.visible) continue;
                const i64 mx0 = std::clamp<i64>(static_cast<i64>(std::floor(item.rect.x)), x0, x1);
                const i64 my0 = std::clamp<i64>(static_cast<i64>(std::floor(item.rect.y)), y0, y1);
                const i64 mx1 = std::clamp<i64>(static_cast<i64>(std::ceil(item.rect.right())), x0, x1);
                const i64 my1 = std::clamp<i64>(static_cast<i64>(std::ceil(item.rect.bottom())), y0, y1);
                if (mx1 <= mx0 || my1 <= my0) continue;
                for (i64 y = my0; y < my1; ++y) {
                    for (i64 x = mx0; x < mx1; ++x) {
                        u8* p = image.at(static_cast<u32>(x - x0), static_cast<u32>(y - y0));
                        p[0] = p[1] = p[2] = 0;
                        p[3] = 255;
                    }
                }
                if (masks.size() > 1) masks += ',';
                masks += rectJson(mx0 - x0, my0 - y0, mx1 - mx0, my1 - my0);
            }
        }
        masks += ']';
        HELIOS_TRY(render::writePng(fs::pathFromUtf8(shot->file), image));
        lastCapture = std::format(R"({{"file":{},"width":{},"height":{},"masks":{}}})", tf::json::quote(shot->file), image.width,
                                  image.height, masks);
        return {};
    }
    std::string lastCapture;

    Result<void> handleResize() {
        if (!needResize || !presenting()) return {};
        u32 w = 0, h = 0;
        displayPixels(w, h);
        HELIOS_TRY(device->resizeSwapchain(swapchain, w, h));
        swapInfo = device->swapchainInfo(swapchain);
        if (swapInfo.format != rhi::Format::Unknown) HELIOS_TRY(ensurePresentPipeline(swapInfo.format));
        needResize = false;
        return {};
    }

    void registerRpc();

    void shutdown() {
        if (uiTest) uiTest->cancelAll();
        if (device) (void)device->waitIdle();
        rpc.reset();
        shell.reset();
        uiTest.reset();
        if (imgui) {
            ImGui::SetCurrentContext(imgui);
            if (renderer) renderer->shutdown(ImGui::GetIO());
            if (platformInit) ImGui_ImplSDL3_Shutdown();
            platformInit = false;
            ImGui::DestroyContext(imgui);
            imgui = nullptr;
        }
        renderer.reset();
        if (device) {
            device->destroy(frameTex);
            device->destroy(viewportTex);
            device->destroy(readback);
            device->destroy(presentPipeline);
            device->destroy(gridPipeline);
            pool.reset();
            if (swapchain.isValid()) device->destroy(swapchain);
            swapchain = {};
            (void)device->waitIdle();
            device.reset();
        }
        fw.reset();
        if (window) SDL_DestroyWindow(window);
        window = nullptr;
        if (sdlInit) SDL_Quit();
        sdlInit = false;
    }
};

// ---------------------------------------------------------------------------------------------
// Remote control: ui.* methods
// ---------------------------------------------------------------------------------------------
void EditorHost::Impl::registerRpc() {
    tf::RpcServer& s = *rpc;
    UiTest* t = uiTest.get();
    const auto args = [](const tf::RpcRequest& req, const tf::RpcResponder& resp) -> std::optional<refl::JsonDocument> {
        auto doc = tf::json::parseObject(req.params, req.method);
        if (!doc) {
            resp.error(tf::rpcerr::kInvalidParams, doc.error().message);
            return std::nullopt;
        }
        return std::move(*doc);
    };
    const auto str = [](const refl::JsonDocument& d, std::string_view key) {
        return std::string(tf::json::getString(d.root(), key).value_or(""));
    };
    const auto finish = [](const tf::RpcResponder& resp) {
        return [resp](const Result<std::string>& r) { resp.finish(r); };
    };
    const auto requireHarness = [t](const tf::RpcResponder& resp) {
        if (t->enabled()) return true;
        resp.error(tf::rpcerr::kConflict, "the UI test harness is off (start helios-editor with --test-mode)");
        return false;
    };

    s.registerMethod(
        "ui.items",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            resp.result(t->itemsJson(str(*d, "filter")));
        },
        "Item table of the last frame {filter?: path substring}");
    s.registerMethod(
        "ui.click",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            const u8 button = static_cast<u8>(std::clamp<i64>(tf::json::getInteger(d->root(), "button").value_or(0), 0, 2));
            t->click(str(*d, "path"), button, finish(resp));
        },
        "Clicks an item {path, button?: 0 left | 1 right | 2 middle}");
    s.registerMethod(
        "ui.doubleClick",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->doubleClick(str(*d, "path"), finish(resp));
        },
        "Double-clicks an item {path}");
    s.registerMethod(
        "ui.hover",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->hover(str(*d, "path"), finish(resp));
        },
        "Moves the mouse over an item {path}");
    s.registerMethod(
        "ui.move",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->moveTo(static_cast<f32>(tf::json::getNumber(d->root(), "x").value_or(0)),
                      static_cast<f32>(tf::json::getNumber(d->root(), "y").value_or(0)), finish(resp));
        },
        "Moves the mouse to display pixels {x, y}");
    s.registerMethod(
        "ui.drag",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->drag(str(*d, "from"), str(*d, "to"), static_cast<f32>(tf::json::getNumber(d->root(), "dx").value_or(0)),
                    static_cast<f32>(tf::json::getNumber(d->root(), "dy").value_or(0)), finish(resp));
        },
        "Drags from an item to another item or by pixels {from, to? | dx, dy}");
    s.registerMethod(
        "ui.type",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->type(str(*d, "text"), finish(resp));
        },
        "Types text into the focused item {text}");
    s.registerMethod(
        "ui.key",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            const u32 repeat = static_cast<u32>(std::clamp<i64>(tf::json::getInteger(d->root(), "repeat").value_or(1), 1, 64));
            t->key(str(*d, "chord"), repeat, finish(resp));
        },
        "Presses a key chord {chord: \"ctrl+shift+p\", repeat?: key presses while the modifiers stay down}");
    s.registerMethod(
        "ui.scroll",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->scroll(str(*d, "path"), static_cast<f32>(tf::json::getNumber(d->root(), "steps").value_or(-1)), finish(resp));
        },
        "Scrolls the mouse wheel over an item {path, steps}");
    s.registerMethod(
        "ui.waitFor",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            std::optional<std::string> value;
            if (auto v = tf::json::getString(d->root(), "value")) value = std::string(*v);
            const u32 timeout = static_cast<u32>(std::clamp<i64>(tf::json::getInteger(d->root(), "timeoutFrames").value_or(300), 1, 100000));
            t->waitFor(str(*d, "path"), tf::json::getBool(d->root(), "enabled"), value, timeout, finish(resp));
        },
        "Waits until an item exists {path, enabled?, value?, timeoutFrames?}");
    s.registerMethod(
        "ui.frames",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            t->waitFrames(static_cast<u32>(std::clamp<i64>(tf::json::getInteger(d->root(), "count").value_or(1), 1, 100000)), finish(resp));
        },
        "Waits for frames {count}");
    s.registerMethod(
        "ui.capture",
        [=](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d || !requireHarness(resp)) return;
            const std::string file = str(*d, "file");
            if (file.empty()) {
                resp.error(tf::rpcerr::kInvalidParams, "ui.capture: 'file' is required");
                return;
            }
            std::string target = str(*d, "target");
            t->capture(target.empty() ? std::string("window") : target, file, finish(resp));
        },
        "Captures the window or a window/item into a PNG {file, target?}; viewports are masked");
    s.registerMethod(
        "ui.lint",
        [this, t](const tf::RpcRequest&, const tf::RpcResponder& resp) {
            if (!t->enabled()) {
                resp.error(tf::rpcerr::kConflict, "the UI test harness is off");
                return;
            }
            UiTest::LintOptions o;
            o.panels = shell->panelPaths();
            o.commands = &fw->commands();
            o.theme = &theme;
            resp.result(lintIssuesJson(t->lint(o)));
        },
        "Layout lints over the last frame: [{rule, path, message}]");
    s.registerMethod(
        "ui.state",
        [this, t](const tf::RpcRequest&, const tf::RpcResponder& resp) {
            u32 w = 0, h = 0;
            displayPixels(w, h);
            const ImGuiContext& g = *imgui;
            std::string focused;
            if (g.NavWindow) {
                ImGuiWindow* win = g.NavWindow;
                while (win->ParentWindow && (win->Flags & ImGuiWindowFlags_ChildWindow) && !win->DockIsActive) win = win->ParentWindow;
                focused = win->Name;
                if (const usize p = focused.find("###"); p != std::string::npos) focused = focused.substr(p + 3);
            }
            resp.result(std::format(R"({{"frame":{},"theme":{},"scale":{},"display":[{},{}],"pendingActions":{},"testMode":{},"focused":{},"pseudoLoc":{}}})",
                                    frames, tf::json::quote(themeName), scale, w, h, t->pendingActions(), config.testMode ? "true" : "false",
                                    tf::json::quote(focused), pseudoLocalization() ? "true" : "false"));
        },
        "Frame number, theme, scale, display size, focused window");
    s.registerMethod(
        "ui.configure",
        [=, this](const tf::RpcRequest& req, const tf::RpcResponder& resp) {
            auto d = args(req, resp);
            if (!d) return;
            if (auto th = tf::json::getString(d->root(), "theme")) {
                if (*th != "file" && !builtinTheme(*th)) {
                    resp.error(tf::rpcerr::kInvalidParams, std::format("unknown theme '{}'", *th));
                    return;
                }
                pendingTheme = std::string(*th);
            }
            if (auto sc = tf::json::getNumber(d->root(), "scale")) pendingScale = static_cast<f32>(*sc);
            if (auto p = tf::json::getBool(d->root(), "pseudoLoc")) setPseudoLocalization(*p);
            if (tf::json::getBool(d->root(), "resetLayout").value_or(false)) shell->resetLayout();
            // Answer once the new settings have been laid out (layout, font atlas and hover state).
            t->waitFrames(3, finish(resp));
        },
        "Sets {theme?, scale?, pseudoLoc?, resetLayout?}; answers after the UI settled");
    s.registerMethod(
        "app.quit",
        [this](const tf::RpcRequest&, const tf::RpcResponder& resp) {
            resp.result("true");
            quit = true;
        },
        "Closes the editor");
}

// ---------------------------------------------------------------------------------------------
// EditorHost
// ---------------------------------------------------------------------------------------------
EditorHost::EditorHost(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {
    m_impl->self = this;
}

EditorHost::~EditorHost() = default;

Result<std::unique_ptr<EditorHost>> EditorHost::create(const EditorConfig& config) {
    auto impl = std::make_unique<Impl>();
    Impl& m = *impl;
    m.config = config;
    if (config.testMode) m.config.deterministicIds = true;
    if (config.width < 320 || config.height < 240) return Error{ErrorCode::InvalidArgument, "window size below 320x240"};
    setPseudoLocalization(config.pseudoLoc);

    // ---- framework (first: a bad project fails before any window appears) ------------------
    tf::FrameworkConfig fc;
    fc.project = config.project;
    fc.projectRoot = config.projectRoot;
    fc.user = config.user;
    fc.journal = config.journal;
    fc.journalRoot = config.journalRoot;
    if (m.config.deterministicIds) {
        auto time = std::make_shared<i64>(1'700'000'000'000'000'000ll);
        fc.clock = [time] { return *time += 1'000'000; };
        auto rng = std::make_shared<Xoshiro256>(0x68656c696f73ull);
        fc.newKey = [rng] {
            const u64 hi = (rng->nextU64() & ~0xF000ull) | 0x4000ull;
            const u64 lo = (rng->nextU64() & ~(3ull << 62)) | (2ull << 62);
            return Guid(hi, lo);
        };
    }
    HELIOS_TRY_ASSIGN(m.fw, tf::Framework::create(fc));
    if (!config.projectRoot.empty()) {
        HELIOS_TRY_ASSIGN(const usize opened, m.fw->openAll());
        HELIOS_LOG_INFO("Opened {} record(s) under {}", opened, fs::pathToUtf8(config.projectRoot));
    }
    std::string recoveryNote;
    if (!config.recover.empty()) {
        fs::Path journalFile = fs::pathFromUtf8(config.recover);
        if (config.recover == "auto") {
            const fs::Path root = config.journalRoot.empty() ? tf::defaultJournalRoot() : config.journalRoot;
            const auto sessions = tf::listJournalSessions(root, config.project, true);
            journalFile = sessions.empty() ? fs::Path() : sessions.back().path;
        }
        if (!journalFile.empty()) {
            HELIOS_TRY_ASSIGN(recoveryNote, recoverJournal(*m.fw, journalFile));
            HELIOS_LOG_INFO("{}", recoveryNote);
        }
    }
    // Without --recover the shell offers any unclean session (Output line, File > Recover).

    // ---- theme -----------------------------------------------------------------------------
    HELIOS_TRY(m.loadTheme(config.themeFile.empty() ? std::string_view(config.theme) : std::string_view("file")));

    // ---- window and device -------------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) return Error{ErrorCode::Unsupported, std::format("SDL_Init: {}", SDL_GetError())};
    m.sdlInit = true;
    // --scale, then HELIOS_EDITOR_SCALE (07 §4.4: forces the scale through the same font
    // re-rasterization path as a per-monitor DPI change), then the display's content scale.
    f32 envScale = 0.0f;
    if (const char* env = SDL_getenv("HELIOS_EDITOR_SCALE"); env && *env) envScale = static_cast<f32>(SDL_atof(env));
    m.scalePolicy.forced = config.scale > 0 || envScale > 0 || config.testMode;
    if (config.scale > 0) {
        m.scale = config.scale;
    } else if (envScale > 0) {
        m.scale = envScale;
    } else if (config.testMode) {
        m.scale = 1.0f;
    } else {
        // A first guess to size the window; the window's own display decides below.
        const f32 s = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        m.scale = s > 0 ? s : 1.0f;
    }
    m.scale = std::clamp(m.scale, detail::kMinUiScale, detail::kMaxUiScale);
    const bool show = !config.testMode || config.present;
    m.config.present = show;
    u32 pw = 0, ph = 0;
    {
        pw = static_cast<u32>(std::lround(static_cast<f32>(config.width) * m.scale));
        ph = static_cast<u32>(std::lround(static_cast<f32>(config.height) * m.scale));
    }
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (!show) flags |= SDL_WINDOW_HIDDEN;
    if (!config.testMode) flags |= SDL_WINDOW_RESIZABLE;
    const std::string title = std::format("Helios Editor - {}", config.project);
    m.window = SDL_CreateWindow(title.c_str(), static_cast<int>(pw), static_cast<int>(ph), flags);
    if (!m.window) return Error{ErrorCode::Unsupported, std::format("SDL_CreateWindow: {}", SDL_GetError())};
    // Per-monitor DPI (07 §1.3): the scale of the display the window opened on, not the primary's.
    if (const auto s = m.scalePolicy.onDisplayScale(m.scale, SDL_GetWindowDisplayScale(m.window))) m.scale = *s;

    rhi::DeviceDesc dd;
    dd.backend = rhi::Backend::Vulkan;
    dd.appName = "helios-editor";
    dd.validation = config.validation;
    HELIOS_TRY_ASSIGN(m.device, rhi::Device::create(dd));
    HELIOS_LOG_INFO("Adapter: {} ({})", m.device->caps().adapter.name, m.device->caps().adapter.driverInfo);
    m.pool = std::make_unique<render::RgResourcePool>(*m.device);
    if (show) {
        u32 w = 0, h = 0;
        m.displayPixels(w, h);
        rhi::SwapchainDesc sc;
        sc.sdlWindow = m.window;
        sc.width = w;
        sc.height = h;
        sc.format = rhi::Format::BGRA8Unorm;
        sc.presentMode = rhi::PresentMode::Fifo;
        sc.name = "Editor";
        HELIOS_TRY_ASSIGN(m.swapchain, m.device->createSwapchain(sc));
        m.swapInfo = m.device->swapchainInfo(m.swapchain);
        HELIOS_TRY(m.ensurePresentPipeline(m.swapInfo.format));
    }
    HELIOS_TRY(m.ensureGridPipeline());

    // ---- ImGui ---------------------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    m.imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(m.imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    if (config.testMode) {
        io.IniFilename = nullptr;
        io.ConfigInputTextCursorBlink = false;
        io.ConfigNavCursorVisibleAuto = false;
    } else {
        const fs::Path root = config.journalRoot.empty() ? tf::defaultJournalRoot() : config.journalRoot;
        m.iniPath = fs::pathToUtf8(root.parent_path() / "editor" / tf::journalDirectory(root, config.project).filename() / "layout.ini");
        (void)fs::createDirectories(fs::pathFromUtf8(m.iniPath).parent_path());
        io.IniFilename = m.iniPath.c_str();
    }
    // The embedded vector font only (fixed across machines; re-rasterized at every scale).
    io.Fonts->AddFontDefaultVector();
    if (!ImGui_ImplSDL3_InitForVulkan(m.window)) return Error{ErrorCode::Unsupported, "ImGui SDL3 backend initialization failed"};
    m.platformInit = true;
    HELIOS_TRY_ASSIGN(m.renderer, ImGuiRenderer::create(*m.device, kFrameFormat));
    m.renderer->installBackend(io);

    // ---- shell, harness, remote control ----------------------------------------------------------
    std::unique_ptr<EditorHost> host(new EditorHost(std::move(impl)));
    Impl& h = *host->m_impl;
    h.uiTest = std::make_unique<UiTest>(h.imgui, [&h](const UiInputEvent& e) { h.inject(e); });
    h.uiTest->setEnabled(config.testMode);
    h.shell = std::make_unique<Shell>(*h.fw, *host);
    if (!recoveryNote.empty()) h.shell->log(recoveryNote);
    if (config.rpcEndpoint != "-") {
        const std::string name = config.rpcEndpoint.empty() ? std::format("helios-editor-{}", tf::currentProcessId()) : config.rpcEndpoint;
        HELIOS_TRY_ASSIGN(h.rpc, tf::RpcServer::start(name));
        tf::registerFrameworkRpc(*h.rpc, *h.fw);
        h.registerRpc();
        HELIOS_LOG_INFO("Remote control: {}", h.rpc->path());
    }
    return host;
}

bool EditorHost::frame() {
    Impl& m = *m_impl;
    if (m.quit) return false;
    ImGui::SetCurrentContext(m.imgui);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) m.quit = true;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) m.needResize = true;
        // Moved to a monitor with another scale, or the user changed the display's scale: the
        // fonts re-rasterize at the new scale (unless the scale was forced or picked by the user).
        if (e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED && m.window) {
            if (const auto s = m.scalePolicy.onDisplayScale(m.scale, SDL_GetWindowDisplayScale(m.window))) m.pendingScale = *s;
        }
        // Test mode takes input only from ui.* actions, so OS events cannot disturb a run.
        if (!m.config.testMode) ImGui_ImplSDL3_ProcessEvent(&e);
    }
    if (m.quit) return false;
    if (m.rpc) m.rpc->pump();
    if (m.quit) return false;
    // Minimized: no swapchain images to draw into; keep serving remote control without spinning.
    if (!m.config.testMode && m.window && (SDL_GetWindowFlags(m.window) & SDL_WINDOW_MINIMIZED) != 0) {
        SDL_Delay(10);
        return true;
    }
    // Test mode under remote control: the UI clock only advances while a ui.* action runs, so the
    // number of frames (and every timer driven by them) between two actions never depends on how
    // fast the driver sends its requests.
    if (m.config.testMode && m.rpc && m.config.maxFrames == 0 && m.frames >= kSettleFrames && m.uiTest->pendingActions() == 0) {
        SDL_Delay(1);
        return true;
    }
    if (auto r = m.device->beginFrame(); !r) {
        HELIOS_LOG_ERROR("beginFrame: {}", r.error());
        m.exitCode = 1;
        return false;
    }
    if (auto r = m.handleResize(); !r) {
        HELIOS_LOG_ERROR("swapchain resize: {}", r.error());
        m.exitCode = 1;
        return false;
    }
    m.uiTest->beginFrame();
    m.applyPending();

    ImGuiIO& io = ImGui::GetIO();
    if (!m.config.testMode) ImGui_ImplSDL3_NewFrame();
    u32 w = 0, h = 0;
    m.displayPixels(w, h);
    // ImGui works in framebuffer pixels; the UI scale is applied through the style and fonts.
    io.DisplaySize = ImVec2(static_cast<f32>(w), static_cast<f32>(h));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    if (m.config.testMode) io.DeltaTime = kFixedDelta;
    m.viewportUsed = false;
    ImGui::NewFrame();
    m.shell->draw();
    ImGui::Render();
    m.uiTest->endFrame();
    ImDrawData* dd = ImGui::GetDrawData();
    if (auto r = m.renderer->updateTextures(dd); !r) HELIOS_LOG_ERROR("ImGui textures: {}", r.error());

    const bool last = m.config.maxFrames != 0 && m.frames + 1 >= m.config.maxFrames;
    const UiTest::CaptureRequest* capture = m.uiTest->captureRequest();
    std::optional<UiTest::CaptureRequest> screenshot;
    if (!capture && last && !m.config.screenshot.empty()) {
        screenshot = UiTest::CaptureRequest{UiRect{0, 0, static_cast<f32>(w), static_cast<f32>(h)}, fs::pathToUtf8(m.config.screenshot)};
    }
    const UiTest::CaptureRequest* shot = capture ? capture : (screenshot ? &*screenshot : nullptr);
    if (m.presenting() || shot) {
        auto r = m.render(dd, shot, false);
        if (capture) {
            m.uiTest->finishCapture(r ? Result<std::string>(m.lastCapture) : Result<std::string>(r.error()));
        } else if (screenshot) {
            if (r) {
                HELIOS_LOG_INFO("Saved screenshot {} ({}x{})", fs::pathToUtf8(m.config.screenshot), w, h);
            } else {
                HELIOS_LOG_ERROR("screenshot: {}", r.error());
                m.exitCode = 1;
            }
        } else if (!r) {
            HELIOS_LOG_ERROR("frame: {}", r.error());
            m.exitCode = 1;
            return false;
        }
    }
    ++m.frames;
    if (last) m.quit = true;
    return !m.quit;
}

int EditorHost::run() {
    Impl& m = *m_impl;
    while (frame()) {
    }
    // 07 §1.2: an unclean journal is what makes the next start offer the replay. A failure exit or a
    // quit with unsaved records (there is no save prompt yet) must leave it unclean.
    if (m.fw && m.fw->journal()) (void)m.fw->journal()->close(journalEndsClean(m.exitCode, *m.fw));
    if (m.device && m.device->validationErrorCount() != 0 && m.exitCode == 0) {
        HELIOS_LOG_ERROR("{} RHI validation error(s)", m.device->validationErrorCount());
        m.exitCode = 2;
    }
    HELIOS_LOG_INFO("Editor ran {} frame(s)", m.frames);
    return m.exitCode;
}

bool EditorHost::journalEndsClean(int exitCode, const tf::Framework& framework) noexcept {
    if (exitCode == 1) return false;
    for (const tf::Document* d : framework.documents().documents()) {
        if (d->dirty()) return false;
    }
    return true;
}

tf::Framework& EditorHost::framework() noexcept {
    return *m_impl->fw;
}
Shell& EditorHost::shell() noexcept {
    return *m_impl->shell;
}
u64 EditorHost::frameCount() const noexcept {
    return m_impl->frames;
}
const std::string& EditorHost::endpoint() const noexcept {
    static const std::string kNone;
    return m_impl->rpc ? m_impl->rpc->endpoint() : kNone;
}

tf::CommandInvoker& EditorHost::uiInvoker() {
    return m_impl->fw->invoker(m_impl->config.testMode ? tf::Origin::UiScripted : tf::Origin::Ui);
}
std::string_view EditorHost::themeName() const {
    return m_impl->themeName;
}
f32 EditorHost::uiScale() const {
    return m_impl->scale;
}
void EditorHost::requestTheme(std::string_view name) {
    m_impl->pendingTheme = std::string(name);
}
void EditorHost::requestScale(f32 scale) {
    // Only View > UI Scale calls this (display changes set pendingScale directly): the user's
    // choice stays when the window moves to another monitor.
    m_impl->pendingScale = m_impl->scalePolicy.choose(scale);
}
void EditorHost::requestQuit() {
    m_impl->quit = true;
}

u64 EditorHost::viewportTexture(u32 width, u32 height) {
    Impl& m = *m_impl;
    if (!m.gridPipeline.isValid() || width == 0 || height == 0) return 0;
    if (!m.viewportTex.isValid() || m.viewportW != width || m.viewportH != height) {
        m.device->destroy(m.viewportTex);  // deferred until frames in flight finish
        m.viewportTex = {};
        auto t = m.device->createTexture(rhi::TextureDesc::tex2D(kFrameFormat, width, height,
                                                                 rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled,
                                                                 "EditorViewport"));
        if (!t) {
            HELIOS_LOG_WARN("viewport texture {}x{}: {}", width, height, t.error());
            return 0;
        }
        m.viewportTex = *t;
        m.viewportW = width;
        m.viewportH = height;
    }
    m.viewportUsed = true;
    return m.device->srv(m.viewportTex);
}

} // namespace helios::edui
