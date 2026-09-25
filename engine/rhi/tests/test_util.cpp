#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <format>

#include "helios/core/log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace rhitest {

CompareResult compareImages(const Image& a, const Image& b, u32 tolerance) {
    CompareResult r;
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        r.sizeMismatch = true;
        return r;
    }
    r.pixelCount = static_cast<u64>(a.width) * a.height;
    for (usize p = 0; p < r.pixelCount; ++p) {
        u32 worst = 0;
        for (usize c = 0; c < 4; ++c) {
            const int d = std::abs(int(a.rgba[p * 4 + c]) - int(b.rgba[p * 4 + c]));
            worst = std::max(worst, static_cast<u32>(d));
        }
        r.maxChannelDiff = std::max(r.maxChannelDiff, worst);
        if (worst > tolerance) ++r.pixelsOverTolerance;
    }
    return r;
}

bool writePng(const std::filesystem::path& path, const Image& image) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::string p = path.string();
    return stbi_write_png(p.c_str(), static_cast<int>(image.width), static_cast<int>(image.height), 4,
                          image.rgba.data(), static_cast<int>(image.width * 4)) != 0;
}

bool readPng(const std::filesystem::path& path, Image& out) {
    int w = 0;
    int h = 0;
    int channels = 0;
    const std::string p = path.string();
    stbi_uc* data = stbi_load(p.c_str(), &w, &h, &channels, 4);
    if (!data) return false;
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgba.assign(data, data + static_cast<usize>(w) * static_cast<usize>(h) * 4);
    stbi_image_free(data);
    return true;
}

std::filesystem::path outputDir() {
    std::filesystem::path dir(HELIOS_RHI_TEST_OUTPUT_DIR);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void checkGolden(const Image& actual, std::string_view name, u32 tolerance, double maxBadFraction) {
    const std::filesystem::path golden = std::filesystem::path(HELIOS_RHI_GOLDEN_DIR) / (std::string(name) + ".png");
    const char* update = std::getenv("HELIOS_UPDATE_GOLDENS");
    if (update && std::string_view(update) == "1") {
        REQUIRE(writePng(golden, actual));
        MESSAGE("updated golden " << golden.string());
        return;
    }
    Image expected;
    if (!readPng(golden, expected)) {
        writePng(outputDir() / (std::string(name) + ".actual.png"), actual);
        FAIL("missing golden " << golden.string() << " (run with HELIOS_UPDATE_GOLDENS=1 to create it)");
        return;
    }
    const CompareResult r = compareImages(actual, expected, tolerance);
    const double badFraction = r.pixelCount ? double(r.pixelsOverTolerance) / double(r.pixelCount) : 1.0;
    const bool ok = !r.sizeMismatch && badFraction <= maxBadFraction;
    if (!ok) {
        writePng(outputDir() / (std::string(name) + ".actual.png"), actual);
        if (!r.sizeMismatch) {
            Image diff = actual;
            for (usize i = 0; i < diff.rgba.size(); i += 4) {
                u8 worst = 0;
                for (usize c = 0; c < 3; ++c) {
                    worst = std::max<u8>(worst, static_cast<u8>(std::abs(int(actual.rgba[i + c]) - int(expected.rgba[i + c]))));
                }
                const u8 v = worst > tolerance ? u8{255} : static_cast<u8>(worst * 16);
                diff.rgba[i] = v;
                diff.rgba[i + 1] = 0;
                diff.rgba[i + 2] = 0;
                diff.rgba[i + 3] = 255;
            }
            writePng(outputDir() / (std::string(name) + ".diff.png"), diff);
        }
    }
    INFO("golden " << name << ": size mismatch " << r.sizeMismatch << ", max channel diff " << r.maxChannelDiff
                   << ", pixels over tolerance " << r.pixelsOverTolerance << "/" << r.pixelCount
                   << " (artifacts in " << outputDir().string() << ")");
    CHECK(ok);
}

QuietRhiLog::QuietRhiLog() { log::setChannelLevel("RHI", log::Level::Off); }
QuietRhiLog::~QuietRhiLog() { log::setChannelLevel("RHI", log::Level::Error); }

std::unique_ptr<rhi::Device> createGpuDevice(rhi::DeviceDesc desc, GpuDeviceOptions options) {
    desc.backend = rhi::Backend::Vulkan;
    desc.appName = "rhi_tests";
    desc.enableSwapchain = options.swapchain;  // headless tests: offscreen targets only
    desc.validation = options.validation;      // used when VK_LAYER_KHRONOS_validation is installed
    desc.adapterPreference = rhi::AdapterPreference::Software;
    auto device = rhi::Device::create(desc);
    if (!device) {
        const char* skip = std::getenv("HELIOS_SKIP_GPU_TESTS");
        if (skip && std::string_view(skip) == "1") {
            MESSAGE("skipping GPU test: " << device.error().toString());
            return nullptr;
        }
        FAIL("Vulkan device creation failed: " << device.error().toString()
                                               << " (set HELIOS_SKIP_GPU_TESTS=1 to skip on machines without Vulkan)");
        return nullptr;
    }
    return std::move(device).value();
}

Image readbackImage(rhi::Device& device, rhi::TextureH texture, rhi::ResourceState state) {
    const rhi::TextureDesc desc = device.textureDesc(texture);
    REQUIRE(rhi::formatInfo(desc.format).blockBytes == 4);
    auto bytes = rhi::readbackTexture(device, texture, state);
    REQUIRE_MESSAGE(bytes.ok(), (bytes.ok() ? std::string() : bytes.error().toString()));
    Image image;
    image.width = desc.width;
    image.height = desc.height;
    image.rgba = std::move(bytes).value();
    if (desc.format == rhi::Format::BGRA8Unorm || desc.format == rhi::Format::BGRA8Srgb) {
        for (usize i = 0; i < image.rgba.size(); i += 4) std::swap(image.rgba[i], image.rgba[i + 2]);
    }
    return image;
}

} // namespace rhitest
