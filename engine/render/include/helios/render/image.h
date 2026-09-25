#pragma once
// CPU-side RGBA8 images for screenshots, golden images and image comparison (rendertest, uitest).
// PNG I/O goes through stb_image / stb_image_write (vendored, public domain / MIT).
// Threading: plain values; the free functions are thread-safe.

#include <filesystem>
#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::render {

/// 8-bit RGBA image, rows top to bottom, tightly packed (4 bytes per pixel). Color is sRGB-encoded.
struct ImageRgba8 {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;

    ImageRgba8() = default;
    ImageRgba8(u32 w, u32 h) : width(w), height(h), pixels(static_cast<usize>(w) * h * 4, 0) {}

    bool empty() const noexcept { return width == 0 || height == 0; }
    u8* at(u32 x, u32 y) noexcept { return pixels.data() + (static_cast<usize>(y) * width + x) * 4; }
    const u8* at(u32 x, u32 y) const noexcept { return pixels.data() + (static_cast<usize>(y) * width + x) * 4; }
    friend bool operator==(const ImageRgba8&, const ImageRgba8&) = default;
};

/// Reads any PNG (converted to RGBA8).
Result<ImageRgba8> readPng(const std::filesystem::path& path);
/// Writes an RGBA8 PNG (creates parent directories).
Result<void> writePng(const std::filesystem::path& path, const ImageRgba8& image);
/// Encodes to PNG bytes in memory.
std::vector<u8> encodePng(const ImageRgba8& image);
/// Decodes PNG bytes.
Result<ImageRgba8> decodePng(std::span<const u8> bytes);

/// Number of pixels whose RGBA bytes differ.
u64 countDifferentPixels(const ImageRgba8& a, const ImageRgba8& b) noexcept;

} // namespace helios::render
