// rhi_triangle — minimal Helios RHI sample: an SDL3 window, a Vulkan swapchain and a spinning,
// vertex-pulled triangle, with window-resize handling.
//
//   rhi_triangle [--frames=N] [--screenshot=out.png] [--width=W] [--height=H] [--vsync=0|1]
//                [--resize-at=N] [--validation]
//
// --frames=N exits after N frames (0 = until the window is closed); --screenshot saves the last
// frame (needs --frames); --resize-at=N resizes the window programmatically at frame N to
// exercise the swapchain-recreation path in automated runs (e.g. under `xvfb-run -a`).
// Exit code: 0 on success, 1 on setup failure, 2 if the RHI reported validation errors.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "helios/core/cmdline.h"
#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/rhi/rhi.h"
#include "rhi_triangle_shaders.h"

using namespace helios;
using namespace helios::rhi;

namespace {

struct Push {
    f32 angle;
    f32 aspect;
    f32 pad[2];
};

Result<PipelineH> createPipeline(Device& device, Format colorFormat) {
    GraphicsPipelineDesc desc;
    desc.vertex = ShaderDesc{rhi_triangle_shaders::triangle(), "vsMain"};
    desc.fragment = ShaderDesc{rhi_triangle_shaders::triangle(), "psMain"};
    desc.colorCount = 1;
    desc.colorFormats[0] = colorFormat;
    desc.name = "SpinningTriangle";
    return device.createGraphicsPipeline(desc);
}

bool saveScreenshot(const std::string& path, const u8* pixels, u32 width, u32 height, Format format) {
    std::vector<u8> rgba(pixels, pixels + static_cast<usize>(width) * height * 4);
    const bool bgra = format == Format::BGRA8Unorm || format == Format::BGRA8Srgb;
    for (usize i = 0; i < rgba.size(); i += 4) {
        if (bgra) std::swap(rgba[i], rgba[i + 2]);
        rgba[i + 3] = 255;
    }
    return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                          static_cast<int>(width * 4)) != 0;
}

int run(const CommandLine& cmdline) {
    const i64 frameLimit = std::max<i64>(0, cmdline.getInt("frames", 0));
    const std::string screenshot(cmdline.getString("screenshot"));
    const i64 resizeAt = cmdline.getInt("resize-at", -1);
    const int width = static_cast<int>(cmdline.getInt("width", 1280));
    const int height = static_cast<int>(cmdline.getInt("height", 720));
    const bool vsync = cmdline.getBool("vsync", true);
    if (!screenshot.empty() && frameLimit == 0) {
        HELIOS_LOG_ERROR("--screenshot needs --frames=N (the last frame is saved)");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        HELIOS_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("Helios RHI triangle", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        HELIOS_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int exitCode = 0;
    {
        DeviceDesc deviceDesc;
        deviceDesc.backend = Backend::Vulkan;
        deviceDesc.appName = "rhi_triangle";
        deviceDesc.validation = cmdline.has("validation");
        auto created = Device::create(deviceDesc);
        if (!created) {
            HELIOS_LOG_ERROR("Vulkan device creation failed: {}", created.error());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        std::unique_ptr<Device> device = std::move(created).value();
        HELIOS_LOG_INFO("Adapter: {} ({})", device->caps().adapter.name, device->caps().adapter.driverInfo);

        int pixelW = width;
        int pixelH = height;
        SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
        SwapchainDesc scDesc;
        scDesc.sdlWindow = window;
        scDesc.width = static_cast<u32>(pixelW);
        scDesc.height = static_cast<u32>(pixelH);
        scDesc.format = Format::BGRA8Srgb;
        scDesc.presentMode = vsync ? PresentMode::Fifo : PresentMode::Mailbox;
        scDesc.name = "Main";
        auto swapchainResult = device->createSwapchain(scDesc);
        if (!swapchainResult) {
            HELIOS_LOG_ERROR("Swapchain creation failed: {}", swapchainResult.error());
            device.reset();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        const SwapchainH swapchain = *swapchainResult;
        SwapchainInfo info = device->swapchainInfo(swapchain);
        auto pipelineResult = createPipeline(*device, info.format);
        if (!pipelineResult) {
            HELIOS_LOG_ERROR("Pipeline creation failed: {}", pipelineResult.error());
            device->destroy(swapchain);
            device.reset();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        PipelineH pipeline = *pipelineResult;
        HELIOS_LOG_INFO("Swapchain {}x{} {} ({} images)", info.width, info.height, formatName(info.format),
                        info.imageCount);
        if (!screenshot.empty() && !hasFlag(info.usage, TextureUsage::TransferSrc)) {
            // Fail loudly: a smoke test must not pass without the image it asked for.
            HELIOS_LOG_ERROR("--screenshot: this surface's swapchain images cannot be copied (no TransferSrc)");
            exitCode = 1;
        }

        BufferH readback;
        bool running = true;
        bool needResize = false;
        bool forceResize = false;  // the swapchain itself asked for it (OutOfDate / Suboptimal)
        u32 resizes = 0;
        i64 frame = 0;
        i64 presented = 0;
        while (running && exitCode == 0) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) needResize = true;
            }
            if (!running) break;
            if (frame == resizeAt) {
                SDL_SetWindowSize(window, width * 3 / 4, height * 3 / 4);
                SDL_SyncWindow(window);
                needResize = true;
            }
            if (needResize) {
                SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
                // Size events also arrive when nothing changed (e.g. at window creation).
                if (!forceResize && static_cast<u32>(pixelW) == info.width && static_cast<u32>(pixelH) == info.height) {
                    needResize = false;
                }
            }
            if (needResize) {
                if (auto r = device->resizeSwapchain(swapchain, static_cast<u32>(pixelW), static_cast<u32>(pixelH)); !r) {
                    HELIOS_LOG_ERROR("resizeSwapchain failed: {}", r.error());
                    exitCode = 1;
                    break;
                }
                const Format oldFormat = info.format;
                info = device->swapchainInfo(swapchain);
                if (info.format != oldFormat && info.format != Format::Unknown) {
                    device->destroy(pipeline);
                    pipeline = createPipeline(*device, info.format).value();
                }
                if (info.width != 0) {
                    ++resizes;
                    HELIOS_LOG_INFO("Swapchain resized to {}x{}", info.width, info.height);
                    if (readback.isValid()) {
                        device->destroy(readback);
                        readback = {};
                    }
                }
                needResize = false;
                forceResize = false;
            }
            if (!device->beginFrame()) {
                exitCode = 1;
                break;
            }
            auto acquired = device->acquireNextImage(swapchain);
            if (!acquired) {
                HELIOS_LOG_ERROR("acquireNextImage failed: {}", acquired.error());
                exitCode = 1;
                break;
            }
            if (acquired->status == SwapchainStatus::OutOfDate) {
                needResize = forceResize = true;
                SDL_Delay(5);  // minimized or mid-resize
                continue;
            }
            const SwapchainImage image = *acquired;
            if (image.status == SwapchainStatus::Suboptimal) needResize = forceResize = true;

            const bool takeShot = !screenshot.empty() && frame == frameLimit - 1 &&
                                  hasFlag(info.usage, TextureUsage::TransferSrc);
            if (takeShot && !readback.isValid()) {
                readback = device
                               ->createBuffer({.size = u64(info.width) * info.height * 4,
                                               .usage = BufferUsage::TransferDst,
                                               .memory = MemoryUsage::Readback,
                                               .name = "Screenshot"})
                               .value();
            }

            CommandList* cmd = device->acquireCommandList(Queue::Graphics, "Frame");
            {
                ScopedLabel label(*cmd, "Triangle");
                cmd->barrier(Barrier::textureState(image.texture, ResourceState::Undefined, ResourceState::RenderTarget));
                const ColorAttachment color{.texture = image.texture, .clearColor = {0.02f, 0.02f, 0.05f, 1.0f}};
                cmd->beginRendering({.colors = {&color, 1}});
                cmd->bindPipeline(pipeline);
                cmd->pushConstants(Push{static_cast<f32>(frame) * 0.03f,
                                        static_cast<f32>(info.width) / static_cast<f32>(std::max(info.height, 1u)),
                                        {0.0f, 0.0f}});
                cmd->draw(3);
                cmd->endRendering();
            }
            if (takeShot) {
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
            const TimelinePoint waits[] = {image.ready};
            auto done = device->submit(Queue::Graphics, {&cmd, 1}, waits);
            if (!done) {
                HELIOS_LOG_ERROR("submit failed: {}", done.error());
                exitCode = 1;
                break;
            }
            auto status = device->present(swapchain, *done);
            if (!status) {
                HELIOS_LOG_ERROR("present failed: {}", status.error());
                exitCode = 1;
                break;
            }
            if (*status != SwapchainStatus::Ok) needResize = forceResize = true;
            ++presented;
            if (takeShot) {
                if (device->wait(*done)) {
                    device->invalidateMapped(readback);
                    const auto* pixels = static_cast<const u8*>(device->map(readback));
                    if (pixels && saveScreenshot(screenshot, pixels, info.width, info.height, info.format)) {
                        HELIOS_LOG_INFO("Saved screenshot {} ({}x{})", screenshot, info.width, info.height);
                    } else {
                        HELIOS_LOG_ERROR("Could not write screenshot {}", screenshot);
                        exitCode = 1;
                    }
                }
            }
            ++frame;
            if (frameLimit > 0 && frame >= frameLimit) running = false;
        }
        if (!screenshot.empty() && frame < frameLimit && exitCode == 0) {
            HELIOS_LOG_ERROR("Stopped after {} of {} frames; no screenshot", frame, frameLimit);
            exitCode = 1;
        }
        (void)device->waitIdle();
        if (readback.isValid()) device->destroy(readback);
        device->destroy(pipeline);
        device->destroy(swapchain);
        HELIOS_LOG_INFO("Rendered {} frames ({} presented, {} swapchain resizes), {} validation errors", frame,
                        presented, resizes, device->validationErrorCount());
        if (exitCode == 0 && device->validationErrorCount() != 0) exitCode = 2;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
    // On Windows, fromProcess() reads the full Unicode command line instead of ANSI argv.
    const CommandLine cmdline = platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv);
    return run(cmdline);
}
