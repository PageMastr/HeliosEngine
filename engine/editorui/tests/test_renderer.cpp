// The ImGui renderer backend on the Null RHI (validation of states, usage and push constants),
// the golden policy of ED-15, and the UI fixture project.

#include <doctest/doctest.h>

#include <cstring>
#include <format>
#include <string>
#include <tuple>

#include "edui_test_util.h"
#include "helios/editorui/imgui_renderer.h"
#include "helios/render/flip.h"
#include "helios/render/image.h"
#include "helios/rhi/null_device.h"
#include "helios/rhi/rhi.h"

using namespace helios;
using namespace helios::edui;
using namespace helios::edui::test;

namespace {

std::unique_ptr<rhi::Device> nullDevice() {
    rhi::DeviceDesc d;
    d.backend = rhi::Backend::Null;
    d.appName = "editorui_tests";
    auto device = rhi::Device::create(d);
    REQUIRE(device);
    return std::move(*device);
}

TEST_CASE("renderer: the shell renders through the RHI without validation errors") {
    ShellHarness h("renderer_shell", 1280, 720);
    auto device = nullDevice();
    rhi::NullDevice* nd = rhi::NullDevice::from(*device);
    REQUIRE(nd != nullptr);
    auto created = ImGuiRenderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(created);
    ImGuiRenderer& renderer = **created;
    ImGui::SetCurrentContext(h.imgui->context());
    // Hand the textures back to a real backend: drop what the headless stand-in acknowledged.
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        t->SetTexID(ImTextureID_Invalid);
        t->SetStatus(ImTextureStatus_WantCreate);
    }
    renderer.installBackend(ImGui::GetIO());
    auto target = device->createTexture(rhi::TextureDesc::tex2D(rhi::Format::RGBA8Unorm, 1280, 720,
                                                                rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc, "Target"));
    REQUIRE(target);
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(device->beginFrame());
        ImGui::NewFrame();
        h.shell->draw();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        REQUIRE(renderer.updateTextures(dd));
        REQUIRE(renderer.prepare(dd));
        rhi::CommandList* cmd = device->acquireCommandList(rhi::Queue::Graphics, "EditorUI");
        REQUIRE(cmd != nullptr);
        cmd->barrier(rhi::Barrier::textureState(*target, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget));
        rhi::ColorAttachment color;
        color.texture = *target;
        rhi::RenderingDesc rendering;
        rendering.colors = {&color, 1};
        cmd->beginRendering(rendering);
        renderer.record(*cmd, dd);
        cmd->endRendering();
        REQUIRE(device->submit(rhi::Queue::Graphics, {&cmd, 1}));
    }
    CHECK(renderer.liveTextures() >= 1);  // the font atlas
    for (const std::string& e : nd->validationErrors()) FAIL_CHECK(e);
    CHECK(device->validationErrorCount() == 0);
    // The trace shows vertex-pulled, scissored, indexed draws.
    const std::string trace = nd->trace();
    CHECK(trace.find("drawIndexed") != std::string::npos);
    CHECK(trace.find("scissor ") != std::string::npos);
    REQUIRE(device->waitIdle());
    renderer.shutdown(ImGui::GetIO());
    CHECK(renderer.liveTextures() == 0);
    device->destroy(*target);
}

TEST_CASE("renderer: a DPI change grows the font atlas through dynamic textures") {
    HeadlessImGui imgui(800, 600);
    auto device = nullDevice();
    auto created = ImGuiRenderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(created);
    ImGuiRenderer& renderer = **created;
    renderer.installBackend(ImGui::GetIO());
    const auto run = [&](f32 scale) {
        ImGui::GetStyle().FontScaleDpi = scale;
        REQUIRE(device->beginFrame());
        ImGui::NewFrame();
        ImGui::Begin("Text");
        ImGui::TextUnformatted("The quick brown fox jumps over the lazy dog 0123456789");
        ImGui::End();
        ImGui::Render();
        REQUIRE(renderer.updateTextures(ImGui::GetDrawData()));
    };
    run(1.0f);
    const ImTextureData* atlas = ImGui::GetIO().Fonts->TexData;
    REQUIRE(atlas != nullptr);
    CHECK(atlas->Status == ImTextureStatus_OK);
    CHECK(atlas->GetTexID() != ImTextureID_Invalid);
    run(2.0f);  // glyphs at twice the size are rasterized and uploaded
    run(2.0f);
    CHECK(ImGui::GetIO().Fonts->TexData->Status == ImTextureStatus_OK);
    CHECK(device->validationErrorCount() == 0);
    REQUIRE(device->waitIdle());
    renderer.shutdown(ImGui::GetIO());
}

TEST_CASE("goldens: the ED-15 set exists at the pinned sizes") {
    const fs::Path dir = fs::pathFromUtf8(HELIOS_EDUI_GOLDEN_DIR);
    for (const char* theme : {"dark", "hc"}) {
        for (const auto& [pct, w, h] : {std::tuple{"100", 1920u, 1080u}, std::tuple{"200", 3840u, 2160u}}) {
            const std::string shell = std::format("shell_{}_{}.png", theme, pct);
            CAPTURE(shell);
            auto img = render::readPng(dir / shell);
            REQUIRE(img);
            CHECK(img->width == w);
            CHECK(img->height == h);
            auto grid = render::readPng(dir / std::format("grid_{}_{}.png", theme, pct));
            REQUIRE(grid);
            CHECK(grid->width < w);
        }
    }
}

TEST_CASE("goldens: the ꟻLIP policy passes identical images and fails a one-pixel layout shift") {
    // 07 §4.4: mean error <= 0.01 and <= 0.1 % of pixels above 0.1 (helios-uitest defaults).
    auto golden = render::readPng(fs::pathFromUtf8(HELIOS_EDUI_GOLDEN_DIR) / "grid_dark_100.png");
    REQUIRE(golden);
    const auto passes = [](const render::FlipResult& f) {
        const f64 hot = static_cast<f64>(f.countAbove(0.1f)) / static_cast<f64>(u64(f.width) * f.height);
        return f.mean <= 0.01 && hot <= 0.001;
    };
    auto same = render::computeFlip(*golden, *golden);
    REQUIRE(same);
    CHECK(passes(*same));
    render::ImageRgba8 shifted(golden->width, golden->height);
    for (u32 y = 0; y < golden->height; ++y) {
        for (u32 x = 0; x < golden->width; ++x) {
            const u32 sx = x == 0 ? 0 : x - 1;
            std::memcpy(shifted.at(x, y), golden->at(sx, y), 4);
        }
    }
    auto moved = render::computeFlip(*golden, shifted);
    REQUIRE(moved);
    CHECK_FALSE(passes(*moved));
}

TEST_CASE("fixture: the UI fixture is canonical and its Frigate is the sample record") {
    REQUIRE(tf::samples::registerSampleTypes());
    const fs::Path root = fixtureCopy("fixture_check");
    auto frigate = fs::readTextFile(root / "records" / "hull" / "frigate.hrec");
    REQUIRE(frigate);
    CHECK(*frigate == tf::samples::sampleHullRecordText());
    tf::FrameworkConfig cfg;
    cfg.projectRoot = root;
    cfg.journal = false;
    auto fw = tf::Framework::create(cfg);
    REQUIRE(fw);
    auto opened = (*fw)->openAll();
    REQUIRE(opened);
    CHECK(*opened == 2);
    for (const tf::Document* d : (*fw)->documents().documents()) {
        CAPTURE(d->relativePath());
        auto bytes = fs::readTextFile(d->path());
        REQUIRE(bytes);
        CHECK(*bytes == d->text());  // canonical JSONC with LF endings (see fixture/.gitattributes)
        CHECK_FALSE(d->dirty());
    }
}

} // namespace
