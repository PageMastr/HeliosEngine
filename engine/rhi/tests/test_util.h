#pragma once
// Shared helpers for the RHI tests: RGBA8 images, PNG I/O, golden-image comparison and device
// creation for the GPU suites.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/rhi/rhi.h"

namespace rhitest {

using namespace helios;

struct Image {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;  // tightly packed RGBA8, top row first

    u8* pixel(u32 x, u32 y) { return rgba.data() + (static_cast<usize>(y) * width + x) * 4; }
    const u8* pixel(u32 x, u32 y) const { return rgba.data() + (static_cast<usize>(y) * width + x) * 4; }
};

struct CompareResult {
    u32 maxChannelDiff = 0;
    u64 pixelsOverTolerance = 0;
    u64 pixelCount = 0;
    bool sizeMismatch = false;
};

/// Per-pixel comparison: a pixel fails when any channel differs by more than `tolerance`.
CompareResult compareImages(const Image& a, const Image& b, u32 tolerance);
bool writePng(const std::filesystem::path& path, const Image& image);
bool readPng(const std::filesystem::path& path, Image& out);

/// Compares `actual` against tests/golden/<name>.png. A pixel may deviate by `tolerance` per
/// channel; at most `maxBadFraction` of the pixels may exceed it (rasterization-rule differences
/// on triangle edges between Mesa versions). HELIOS_UPDATE_GOLDENS=1 rewrites the golden instead.
/// On failure the actual image and a diff are written to the test output directory.
void checkGolden(const Image& actual, std::string_view name, u32 tolerance = 2, double maxBadFraction = 0.002);

/// Output directory for failure artifacts (created on demand).
std::filesystem::path outputDir();

struct GpuDeviceOptions {
    bool validation = true;  ///< Khronos validation when the layer is installed.
    bool swapchain = false;  ///< Enable WSI (headless tests keep it off).
};

/// Vulkan device for the gpu suites: prefers a software adapter (lavapipe, which the goldens are
/// made with; override with HELIOS_RHI_ADAPTER), enables validation when the layer is installed.
/// Returns null only when HELIOS_SKIP_GPU_TESTS=1 and Vulkan is unavailable (the test then skips);
/// otherwise a creation failure fails the test.
std::unique_ptr<rhi::Device> createGpuDevice(rhi::DeviceDesc desc = {}, GpuDeviceOptions options = {});

/// Silences the RHI log channel while a negative test provokes errors on purpose (they are still
/// counted and checked through validationErrorCount / onMessage).
class QuietRhiLog {
public:
    QuietRhiLog();
    ~QuietRhiLog();
    QuietRhiLog(const QuietRhiLog&) = delete;
    QuietRhiLog& operator=(const QuietRhiLog&) = delete;
};

/// Reads back an RGBA8 texture (in `state`) as an Image.
Image readbackImage(rhi::Device& device, rhi::TextureH texture, rhi::ResourceState state);

} // namespace rhitest
