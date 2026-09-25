// Swapchains without a window: SDL's "offscreen" video driver creates VK_EXT_headless_surface
// surfaces (lavapipe and most Mesa drivers support it), so acquire/present/resize run in the gpu
// suite with no display server. Skips with a message where the driver lacks headless surfaces
// (typical proprietary Windows drivers); rhi_triangle_smoke covers real windows there.

#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

#include "helios/rhi/rhi.h"
#include "test_util.h"

using namespace helios;
using namespace helios::rhi;

namespace {

/// SDL video on the offscreen driver for the scope of a test.
struct OffscreenVideo {
    bool ok = false;
    SDL_Window* window = nullptr;

    OffscreenVideo(int width, int height) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            MESSAGE("skipping: SDL offscreen video driver unavailable: " << SDL_GetError());
            return;
        }
        window = SDL_CreateWindow("rhi_tests", width, height, SDL_WINDOW_VULKAN);
        if (!window) {
            MESSAGE("skipping: offscreen Vulkan window unavailable: " << SDL_GetError());
            return;
        }
        ok = true;
    }
    ~OffscreenVideo() {
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
    OffscreenVideo(const OffscreenVideo&) = delete;
    OffscreenVideo& operator=(const OffscreenVideo&) = delete;
};

/// Clears the next swapchain image, copies it to `readback` (if the images allow it) and presents.
/// Tracks each image's state so re-acquired images transition from Present, not Undefined.
struct FrameLoop {
    Device& dev;
    SwapchainH swapchain;
    std::unordered_map<u64, ResourceState> states;
    u32 frames = 0;

    /// Returns the cleared image's pixels (BGRA/RGBA as the swapchain format says), or nullopt
    /// when the swapchain cannot be read back.
    std::optional<std::vector<u8>> frame(const std::array<f32, 4>& clear) {
        REQUIRE(dev.beginFrame().ok());
        auto acquired = dev.acquireNextImage(swapchain);
        REQUIRE_MESSAGE(acquired.ok(), (acquired.ok() ? std::string() : acquired.error().toString()));
        REQUIRE(acquired->status != SwapchainStatus::OutOfDate);
        const SwapchainImage image = *acquired;
        const SwapchainInfo info = dev.swapchainInfo(swapchain);
        const bool readable = hasFlag(info.usage, TextureUsage::TransferSrc);
        BufferH readback;
        if (readable) {
            readback = dev.createBuffer({.size = u64(info.width) * info.height * 4,
                                         .usage = BufferUsage::TransferDst,
                                         .memory = MemoryUsage::Readback,
                                         .name = "SwapchainReadback"})
                           .value();
        }
        ResourceState& state = states.try_emplace(image.texture.toBits(), ResourceState::Undefined).first->second;
        CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "SwapchainFrame");
        cmd->barrier(Barrier::textureState(image.texture, state, ResourceState::RenderTarget));
        const ColorAttachment ca{.texture = image.texture, .clearColor = clear};
        cmd->beginRendering({.colors = {&ca, 1}});
        cmd->endRendering();
        if (readable) {
            cmd->barrier(Barrier::textureState(image.texture, ResourceState::RenderTarget, ResourceState::CopySource));
            cmd->copyTextureToBuffer(image.texture, {}, readback, {});
            const Barrier after[] = {
                Barrier::textureState(image.texture, ResourceState::CopySource, ResourceState::Present),
                Barrier::bufferState(readback, ResourceState::CopyDest, ResourceState::HostRead),
            };
            cmd->barrier(after);
        } else {
            cmd->barrier(Barrier::textureState(image.texture, ResourceState::RenderTarget, ResourceState::Present));
        }
        state = ResourceState::Present;
        const TimelinePoint waits[] = {image.ready};
        const TimelinePoint done = dev.submit(Queue::Graphics, {&cmd, 1}, waits).value();
        auto presented = dev.present(swapchain, done);
        REQUIRE_MESSAGE(presented.ok(), (presented.ok() ? std::string() : presented.error().toString()));
        ++frames;
        if (!readable) return std::nullopt;
        REQUIRE(dev.wait(done).ok());
        dev.invalidateMapped(readback);
        const auto* bytes = static_cast<const u8*>(dev.map(readback));
        std::vector<u8> pixels(bytes, bytes + u64(info.width) * info.height * 4);
        dev.destroy(readback);
        return pixels;
    }
};

/// Red and blue channel of the pixel at (x, y) for an 8-bit RGBA or BGRA swapchain.
std::pair<u8, u8> redBlue(const std::vector<u8>& pixels, const SwapchainInfo& info, u32 x, u32 y) {
    const u8* p = pixels.data() + (u64(y) * info.width + x) * 4;
    const bool bgra = info.format == Format::BGRA8Unorm || info.format == Format::BGRA8Srgb;
    return bgra ? std::pair<u8, u8>{p[2], p[0]} : std::pair<u8, u8>{p[0], p[2]};
}

} // namespace

TEST_SUITE("gpu") {

TEST_CASE("gpu: windowless swapchain (SDL offscreen driver): acquire, present, resize, second device") {
    OffscreenVideo video(64, 48);
    if (!video.ok) return;
    auto dev = rhitest::createGpuDevice({}, {.swapchain = true});
    if (!dev) return;
    REQUIRE(dev->caps().has(CapBit::Swapchain));
    auto created = dev->createSwapchain({.sdlWindow = video.window,
                                         .width = 64,
                                         .height = 48,
                                         .format = Format::BGRA8Unorm,
                                         .presentMode = PresentMode::Fifo,
                                         .imageCount = 3,
                                         .name = "Offscreen"});
    if (!created && created.errorCode() == ErrorCode::Unsupported) {
        MESSAGE("skipping: no headless surface support: " << created.error().toString());
        return;
    }
    REQUIRE_MESSAGE(created.ok(), (created.ok() ? std::string() : created.error().toString()));
    const SwapchainH swapchain = *created;
    SwapchainInfo info = dev->swapchainInfo(swapchain);
    CHECK(info.width == 64);
    CHECK(info.height == 48);
    CHECK(info.imageCount >= 2);
    CHECK(info.format != Format::Unknown);

    // Regression: adapter enumeration and a second device (without WSI) used to rebind volk's
    // process-global instance pointers; this swapchain's resize/destroy then called NULL.
    REQUIRE(Device::enumerateAdapters(Backend::Vulkan).ok());
    {
        auto headless = rhitest::createGpuDevice();
        REQUIRE(headless);
    }

    FrameLoop loop{*dev, swapchain, {}, 0};
    std::optional<std::vector<u8>> pixels;
    u32 expectedFrames = 2 * info.imageCount;  // every image re-acquired at least once (Present -> RenderTarget)
    for (u32 i = 0; i < 2 * info.imageCount; ++i) pixels = loop.frame({1.0f, 0.0f, 0.0f, 1.0f});
    if (pixels) {
        CHECK(redBlue(*pixels, info, 0, 0) == std::pair<u8, u8>{255, 0});
        CHECK(redBlue(*pixels, info, 63, 47) == std::pair<u8, u8>{255, 0});
    }

    REQUIRE(dev->resizeSwapchain(swapchain, 96, 40).ok());
    info = dev->swapchainInfo(swapchain);
    CHECK(info.width == 96);
    CHECK(info.height == 40);
    loop.states.clear();  // new images start Undefined
    expectedFrames += 2 * info.imageCount;
    for (u32 i = 0; i < 2 * info.imageCount; ++i) pixels = loop.frame({0.0f, 0.0f, 1.0f, 1.0f});
    if (pixels) {
        CHECK(pixels->size() == 96u * 40u * 4u);
        CHECK(redBlue(*pixels, info, 95, 39) == std::pair<u8, u8>{0, 255});
    } else {
        MESSAGE("swapchain images are not TransferSrc here: pixels not checked");
    }
    CHECK(loop.frames == expectedFrames);
    REQUIRE(dev->waitIdle().ok());
    dev->destroy(swapchain);
    CHECK(dev->swapchainInfo(swapchain).width == 0);
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE("gpu")
