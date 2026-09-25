#pragma once
// Texture/buffer element formats and their properties. Backend-neutral: the Vulkan backend maps
// them to VkFormat, a D3D12 backend would map them to DXGI_FORMAT. Channel order in names is the
// memory order (RGBA8 = R in byte 0); BGRA8 exists for swapchains.
// Threading: pure functions over immutable tables.

#include <string_view>

#include "helios/core/types.h"

namespace helios::rhi {

enum class Format : u16 {
    Unknown = 0,
    // 8-bit
    R8Unorm,
    R8Snorm,
    R8Uint,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8Srgb,
    RGBA8Snorm,
    RGBA8Uint,
    BGRA8Unorm,
    BGRA8Srgb,
    // 16-bit
    R16Unorm,
    R16Float,
    R16Uint,
    RG16Unorm,
    RG16Float,
    RGBA16Unorm,
    RGBA16Float,
    RGBA16Uint,
    // 32-bit
    R32Float,
    R32Uint,
    R32Sint,
    RG32Float,
    RG32Uint,
    RGB32Float,
    RGBA32Float,
    RGBA32Uint,
    // Packed
    RGB10A2Unorm,
    RG11B10Float,
    RGB9E5Float,
    // Depth / stencil
    D16Unorm,
    D32Float,
    D24UnormS8Uint,
    D32FloatS8Uint,
    // Block compressed (4x4 blocks)
    BC1Unorm,
    BC1Srgb,
    BC3Unorm,
    BC3Srgb,
    BC4Unorm,
    BC4Snorm,
    BC5Unorm,
    BC5Snorm,
    BC6HUfloat,
    BC6HSfloat,
    BC7Unorm,
    BC7Srgb,
    Count
};

/// How shaders see the channels.
enum class FormatKind : u8 { Unknown, Unorm, Snorm, Uint, Sint, Float, Srgb, Depth, DepthStencil };

struct FormatInfo {
    std::string_view name; ///< "RGBA8Unorm"
    u8 blockBytes = 0;     ///< Bytes per texel, or per block for compressed formats.
    u8 blockWidth = 1;     ///< Texels per block horizontally (4 for BC).
    u8 blockHeight = 1;
    u8 channels = 0;
    FormatKind kind = FormatKind::Unknown;
    bool compressed = false;
};

/// Properties of `format`; Format::Unknown and out-of-range values return an all-zero info.
const FormatInfo& formatInfo(Format format) noexcept;

inline std::string_view formatName(Format format) noexcept { return formatInfo(format).name; }
inline bool isDepthFormat(Format f) noexcept {
    const FormatKind k = formatInfo(f).kind;
    return k == FormatKind::Depth || k == FormatKind::DepthStencil;
}
inline bool hasStencil(Format f) noexcept { return formatInfo(f).kind == FormatKind::DepthStencil; }
inline bool isSrgbFormat(Format f) noexcept { return formatInfo(f).kind == FormatKind::Srgb; }
inline bool isCompressedFormat(Format f) noexcept { return formatInfo(f).compressed; }

/// Linear (non-sRGB) twin of an sRGB format (RGBA8Srgb -> RGBA8Unorm); other formats unchanged.
Format linearFormat(Format format) noexcept;

/// Bytes of one tightly packed row of `width` texels (rounded up to whole blocks).
u64 formatRowBytes(Format format, u32 width) noexcept;
/// Bytes of one tightly packed width x height x depth subresource (whole blocks).
u64 formatSurfaceBytes(Format format, u32 width, u32 height, u32 depth = 1) noexcept;

/// Extent of `mip` for a base extent (never below 1).
constexpr u32 mipExtent(u32 base, u32 mip) noexcept {
    const u32 v = mip >= 32 ? 0u : (base >> mip);
    return v == 0 ? 1u : v;
}

} // namespace helios::rhi
