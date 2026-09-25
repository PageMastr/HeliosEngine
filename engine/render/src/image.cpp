// PNG I/O for RGBA8 images through stb (static linkage so other stb users never clash).

#include "helios/render/image.h"

#include <cstring>
#include <format>
#include <limits>

#include "helios/core/fs.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4456 4457 4505 4701 4702 4703)
#endif
#include <stb_image.h>
#include <stb_image_write.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace helios::render {

namespace {
void appendBytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<u8>*>(context);
    const u8* bytes = static_cast<const u8*>(data);
    out->insert(out->end(), bytes, bytes + size);
}
} // namespace

std::vector<u8> encodePng(const ImageRgba8& image) {
    std::vector<u8> out;
    if (image.empty()) return out;
    stbi_write_png_to_func(&appendBytes, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                           image.pixels.data(), static_cast<int>(image.width * 4));
    return out;
}

Result<ImageRgba8> decodePng(std::span<const u8> bytes) {
    // stb takes an int length: larger inputs would be truncated (or turn negative).
    if (bytes.size() > static_cast<usize>(std::numeric_limits<int>::max())) {
        return Error{ErrorCode::ParseError, "PNG decode failed: input larger than 2 GiB"};
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!data) return Error{ErrorCode::ParseError, std::format("PNG decode failed: {}", stbi_failure_reason())};
    ImageRgba8 image(static_cast<u32>(w), static_cast<u32>(h));
    std::memcpy(image.pixels.data(), data, image.pixels.size());
    stbi_image_free(data);
    return image;
}

Result<ImageRgba8> readPng(const std::filesystem::path& path) {
    HELIOS_TRY_ASSIGN(std::vector<u8> bytes, fs::readFile(path));
    auto image = decodePng(bytes);
    if (!image) return Error{image.error().code, std::format("{}: {}", fs::pathToUtf8(path), image.error().message)};
    return image;
}

Result<void> writePng(const std::filesystem::path& path, const ImageRgba8& image) {
    if (image.empty()) return Error{ErrorCode::InvalidArgument, "writePng: empty image"};
    const std::vector<u8> bytes = encodePng(image);
    if (bytes.empty()) return Error{ErrorCode::IoError, "writePng: encoding failed"};
    if (path.has_parent_path()) HELIOS_TRY(fs::createDirectories(path.parent_path()));
    return fs::writeFile(path, bytes);
}

u64 countDifferentPixels(const ImageRgba8& a, const ImageRgba8& b) noexcept {
    if (a.width != b.width || a.height != b.height) return static_cast<u64>(std::max(a.width, b.width)) * std::max(a.height, b.height);
    u64 count = 0;
    for (usize i = 0; i < a.pixels.size(); i += 4) {
        if (std::memcmp(&a.pixels[i], &b.pixels[i], 4) != 0) ++count;
    }
    return count;
}

} // namespace helios::render
