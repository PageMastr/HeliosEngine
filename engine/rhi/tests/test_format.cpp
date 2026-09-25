// Format tables, enum names and other backend-independent helpers.

#include <doctest/doctest.h>

#include <set>
#include <string>

#include "helios/rhi/rhi.h"

using namespace helios;
using namespace helios::rhi;

TEST_CASE("rhi format: every format has a unique name and sane block data") {
    std::set<std::string> names;
    for (u32 i = 1; i < static_cast<u32>(Format::Count); ++i) {
        const FormatInfo& info = formatInfo(static_cast<Format>(i));
        INFO("format index " << i);
        CHECK_FALSE(info.name.empty());
        CHECK(names.insert(std::string(info.name)).second);
        CHECK(info.blockBytes > 0);
        CHECK(info.channels > 0);
        CHECK(info.kind != FormatKind::Unknown);
        CHECK((info.compressed ? (info.blockWidth == 4 && info.blockHeight == 4)
                               : (info.blockWidth == 1 && info.blockHeight == 1)));
    }
    CHECK(formatInfo(Format::Unknown).blockBytes == 0);
    CHECK(formatInfo(static_cast<Format>(9999)).blockBytes == 0);
}

TEST_CASE("rhi format: classification") {
    CHECK(isDepthFormat(Format::D32Float));
    CHECK(isDepthFormat(Format::D24UnormS8Uint));
    CHECK_FALSE(isDepthFormat(Format::RGBA8Unorm));
    CHECK(hasStencil(Format::D32FloatS8Uint));
    CHECK_FALSE(hasStencil(Format::D32Float));
    CHECK(isSrgbFormat(Format::BGRA8Srgb));
    CHECK(isCompressedFormat(Format::BC7Unorm));
    CHECK(linearFormat(Format::RGBA8Srgb) == Format::RGBA8Unorm);
    CHECK(linearFormat(Format::BC1Srgb) == Format::BC1Unorm);
    CHECK(linearFormat(Format::R32Float) == Format::R32Float);
    CHECK(formatName(Format::RG11B10Float) == "RG11B10Float");
}

TEST_CASE("rhi format: row and surface sizes round up to whole blocks") {
    CHECK(formatRowBytes(Format::RGBA8Unorm, 256) == 1024);
    CHECK(formatRowBytes(Format::RGB32Float, 3) == 36);
    CHECK(formatSurfaceBytes(Format::RGBA16Float, 4, 4) == 128);
    CHECK(formatSurfaceBytes(Format::R8Unorm, 5, 3, 2) == 30);
    // BC: 4x4 blocks; a 5x5 surface needs 2x2 blocks.
    CHECK(formatRowBytes(Format::BC1Unorm, 5) == 16);
    CHECK(formatSurfaceBytes(Format::BC1Unorm, 5, 5) == 32);
    CHECK(formatSurfaceBytes(Format::BC7Unorm, 1, 1) == 16);
    CHECK(formatSurfaceBytes(Format::Unknown, 4, 4) == 0);
}

TEST_CASE("rhi format: mip extents never reach zero") {
    CHECK(mipExtent(256, 0) == 256);
    CHECK(mipExtent(256, 3) == 32);
    CHECK(mipExtent(256, 8) == 1);
    CHECK(mipExtent(256, 12) == 1);
    CHECK(mipExtent(5, 1) == 2);
    CHECK(mipExtent(1, 40) == 1);
}

TEST_CASE("rhi names: backends, queues, states, caps") {
    CHECK(backendName(Backend::Vulkan) == "Vulkan");
    CHECK(queueName(Queue::AsyncCompute) == "AsyncCompute");
    CHECK(resourceStateName(ResourceState::RenderTarget) == "RenderTarget");
    CHECK(adapterTypeName(AdapterType::Cpu) == "Cpu");
    CHECK(capBitName(CapBit::MeshShader) == "MeshShader");
    CHECK(capBitName(CapBit::MeshShader | CapBit::RayQuery).empty());
    for (u32 s = 0; s < static_cast<u32>(ResourceState::Count); ++s) {
        CHECK(resourceStateName(static_cast<ResourceState>(s)) != "?");
    }
}

TEST_CASE("rhi types: descriptor helpers") {
    const TextureDesc d = TextureDesc::tex2D(Format::RGBA16Float, 640, 360, TextureUsage::Sampled | TextureUsage::ColorAttachment,
                                             "HDR", 4);
    CHECK(d.width == 640);
    CHECK(d.mipLevels == 4);
    CHECK(hasFlag(d.usage, TextureUsage::ColorAttachment));
    const Barrier b = Barrier::textureState(TextureH(3, 1), ResourceState::RenderTarget, ResourceState::ShaderResource);
    CHECK(b.kind == Barrier::Kind::Texture);
    CHECK(b.range.mipCount == kAllMips);
    const BlendState pm = BlendState::premultiplied();
    CHECK(pm.enable);
    CHECK(pm.dstColor == BlendFactor::OneMinusSrcAlpha);
    CHECK(TimelinePoint{}.isNull());
}
