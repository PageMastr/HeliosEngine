// Vulkan backend on a real (or software) GPU: offscreen rendering with golden images, bindless
// textures, compute through device addresses and bindless buffers, cross-queue timelines,
// readback, async pipelines, parallel recording and diagnostics. CTest runs these as
// `rhi_tests_gpu` (label "gpu"); in CI they run on lavapipe. No window is needed.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/math/mat.h"
#include "helios/math/vec.h"
#include "helios/rhi/rhi.h"
#include "rhi_tests_shaders.h"
#include "test_util.h"

using namespace helios;
using namespace helios::rhi;
using rhitest::Image;

namespace {

constexpr u32 kSize = 256;  // lavapipe is a CPU rasterizer: keep targets small

TextureH makeTarget(Device& dev, std::string_view name, u32 size = kSize, Format format = Format::RGBA8Unorm) {
    return dev
        .createTexture(TextureDesc::tex2D(format, size, size, TextureUsage::ColorAttachment | TextureUsage::TransferSrc,
                                          name))
        .value();
}

PipelineH makeGraphicsPipeline(Device& dev, std::span<const u32> spirv, std::string_view name,
                               Format color = Format::RGBA8Unorm, RasterState raster = {}) {
    GraphicsPipelineDesc d;
    d.vertex = ShaderDesc{spirv, "vsMain"};
    d.fragment = ShaderDesc{spirv, "psMain"};
    d.colorCount = 1;
    d.colorFormats[0] = color;
    d.raster = raster;
    d.name = name;
    auto p = dev.createGraphicsPipeline(d);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().toString()));
    return *p;
}

PipelineH makeComputePipeline(Device& dev, std::span<const u32> spirv, std::string_view entry, std::string_view name,
                              PsoPriority priority = PsoPriority::Immediate) {
    auto p = dev.createComputePipeline(ComputePipelineDesc{ShaderDesc{spirv, entry}, name}, priority);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().toString()));
    return *p;
}

/// Records: target Undefined/CopySource -> RenderTarget, clear, draw via `record`, -> CopySource.
template <class F>
TimelinePoint renderPass(Device& dev, TextureH target, ResourceState before, const std::array<f32, 4>& clear,
                         F&& record) {
    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Render");
    cmd->barrier(Barrier::textureState(target, before, ResourceState::RenderTarget));
    const ColorAttachment ca{.texture = target, .clearColor = clear};
    cmd->beginRendering({.colors = {&ca, 1}});
    record(*cmd);
    cmd->endRendering();
    cmd->barrier(Barrier::textureState(target, ResourceState::RenderTarget, ResourceState::CopySource));
    auto done = dev.submit(Queue::Graphics, {&cmd, 1});
    REQUIRE(done.ok());
    return *done;
}

std::vector<u32> readWords(Device& dev, BufferH buffer, u32 count, ResourceState state, TimelinePoint after = {},
                           Queue queue = Queue::Graphics) {
    auto bytes = readbackBuffer(dev, buffer, 0, u64(count) * 4, state, after, queue);
    REQUIRE_MESSAGE(bytes.ok(), (bytes.ok() ? std::string() : bytes.error().toString()));
    std::vector<u32> out(count);
    std::memcpy(out.data(), bytes->data(), bytes->size());
    return out;
}

BufferH makeStorageBuffer(Device& dev, u64 size, std::string_view name) {
    return dev
        .createBuffer({.size = size,
                       .usage = BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst,
                       .name = name})
        .value();
}

} // namespace

TEST_SUITE("gpu") {

TEST_CASE("gpu: device creation, adapter and capabilities") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    const Caps& caps = dev->caps();
    MESSAGE("adapter: " << caps.adapter.name << " (" << adapterTypeName(caps.adapter.type) << ", "
                        << caps.adapter.driverName << " " << caps.adapter.driverInfo << ")");
    CHECK(caps.backend == Backend::Vulkan);
    CHECK(caps.adapter.meetsRequirements);
    CHECK(caps.adapter.apiVersion >= ((1u << 22) | (3u << 12)));
    CHECK(caps.limits.maxPushConstantBytes == 128);
    CHECK(caps.limits.maxBindlessSampledImages > 1000);
    CHECK(caps.limits.maxBindlessSampledImages <= 131072);
    CHECK(caps.limits.maxBindlessSamplers <= 128);
    CHECK(caps.limits.maxTextureDimension2D >= 4096);
    CHECK_FALSE(caps.has(CapBit::Swapchain));  // headless tests disable surfaces

    auto adapters = Device::enumerateAdapters(Backend::Vulkan);
    REQUIRE(adapters.ok());
    CHECK_FALSE(adapters->empty());

    const MemoryStats stats = dev->memoryStats();
    CHECK_FALSE(stats.heaps.empty());
    CHECK(stats.textureCount >= 2);  // default resources behind bindless slot 0
    CHECK(dev->validationErrorCount() == 0);

    // Masking a capability hides it (CI uses this to exercise fallbacks).
    DeviceDesc masked;
    masked.capsMask = ~(CapBit::DebugUtils | CapBit::MemoryBudget);
    auto dev2 = rhitest::createGpuDevice(masked);
    REQUIRE(dev2);
    CHECK_FALSE(dev2->caps().has(CapBit::DebugUtils));
    CHECK_FALSE(dev2->caps().has(CapBit::MemoryBudget));
}

TEST_CASE("gpu: clears through dynamic rendering and clearTexture read back exactly") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    const TextureH a = makeTarget(*dev, "ClearA");
    renderPass(*dev, a, ResourceState::Undefined, {1.0f, 0.0f, 0.2f, 1.0f}, [](CommandList&) {});
    const Image imageA = rhitest::readbackImage(*dev, a, ResourceState::CopySource);
    bool allMatch = true;
    for (u32 y = 0; y < kSize && allMatch; ++y) {
        for (u32 x = 0; x < kSize && allMatch; ++x) {
            const u8* p = imageA.pixel(x, y);
            allMatch = p[0] == 255 && p[1] == 0 && p[2] == 51 && p[3] == 255;
        }
    }
    CHECK(allMatch);

    const TextureH b = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 64, 32,
                                                             TextureUsage::TransferDst | TextureUsage::TransferSrc, "ClearB"))
                           .value();
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "ClearTexture");
    cmd->barrier(Barrier::textureState(b, ResourceState::Undefined, ResourceState::CopyDest));
    cmd->clearTexture(b, {0.0f, 1.0f, 0.0f, 1.0f});
    cmd->barrier(Barrier::textureState(b, ResourceState::CopyDest, ResourceState::CopySource));
    REQUIRE(dev->submit(Queue::Graphics, {&cmd, 1}).ok());
    const Image imageB = rhitest::readbackImage(*dev, b, ResourceState::CopySource);
    CHECK(imageB.width == 64);
    CHECK(imageB.height == 32);
    CHECK(imageB.pixel(63, 31)[1] == 255);
    CHECK(imageB.pixel(0, 0)[0] == 0);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: triangle matches the golden image and renders deterministically") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    const TextureH target = makeTarget(*dev, "Triangle");
    const PipelineH pipeline = makeGraphicsPipeline(*dev, rhi_test_shaders::triangle(), "Triangle");
    auto draw = [&](CommandList& cmd) {
        cmd.bindPipeline(pipeline);
        cmd.draw(3);
    };
    renderPass(*dev, target, ResourceState::Undefined, {0.05f, 0.05f, 0.1f, 1.0f}, draw);
    const Image first = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    REQUIRE(dev->beginFrame().ok());
    renderPass(*dev, target, ResourceState::CopySource, {0.05f, 0.05f, 0.1f, 1.0f}, draw);
    const Image second = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    CHECK(first.rgba == second.rgba);  // race check (03 §8.4): bit-identical re-render
    // Orientation: the apex (+Y in clip space) is near the top of the image.
    CHECK(first.pixel(kSize / 2, kSize / 8)[2] > 128);   // blue apex vertex region
    CHECK(first.pixel(kSize / 2, kSize - 4)[2] < 64);    // below the base: background
    rhitest::checkGolden(first, "triangle");
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: textured quad samples a bindless texture with a bindless sampler") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    // 8x8 texture: four colored quadrants plus a white border column, uploaded through staging.
    constexpr u32 kTex = 8;
    std::vector<u8> texels(kTex * kTex * 4);
    for (u32 y = 0; y < kTex; ++y) {
        for (u32 x = 0; x < kTex; ++x) {
            u8* p = &texels[(y * kTex + x) * 4];
            const bool right = x >= kTex / 2;
            const bool bottom = y >= kTex / 2;
            p[0] = right ? 255 : 20;
            p[1] = bottom ? 255 : 20;
            p[2] = (right && bottom) ? 20 : 200;
            p[3] = 255;
            if (x == 0) p[0] = p[1] = p[2] = 255;
        }
    }
    const TextureH tex = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, kTex, kTex,
                                                               TextureUsage::Sampled | TextureUsage::TransferDst, "Checker"))
                             .value();
    auto uploaded = uploadTexture(*dev, tex, std::as_bytes(std::span<const u8>(texels)));
    REQUIRE(uploaded.ok());
    SamplerDesc nearest;
    nearest.minFilter = nearest.magFilter = nearest.mipFilter = Filter::Nearest;
    nearest.addressU = nearest.addressV = AddressMode::ClampToEdge;
    struct Push {
        u32 texture;
        u32 samplerIndex;
        f32 scale[2];
    } push{dev->srv(tex), dev->sampler(nearest), {0.75f, 0.75f}};
    REQUIRE(push.texture != kInvalidBindless);
    REQUIRE(push.texture != 0);
    REQUIRE(push.samplerIndex != kInvalidBindless);

    const TextureH target = makeTarget(*dev, "Quad");
    const PipelineH pipeline = makeGraphicsPipeline(*dev, rhi_test_shaders::textured_quad(), "TexturedQuad");
    auto draw = [&](CommandList& cmd) {
        cmd.bindPipeline(pipeline);
        cmd.pushConstants(push);
        cmd.draw(6);
    };
    renderPass(*dev, target, ResourceState::Undefined, {0.0f, 0.0f, 0.0f, 1.0f}, draw);
    const Image first = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    renderPass(*dev, target, ResourceState::CopySource, {0.0f, 0.0f, 0.0f, 1.0f}, draw);
    const Image second = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    CHECK(first.rgba == second.rgba);
    // Texel (0,0) of the texture (white column) lands at the quad's top-left.
    const u32 left = kSize / 8 + 2;
    const u32 top = kSize / 8 + 2;
    CHECK(first.pixel(left, top)[0] == 255);
    CHECK(first.pixel(left, top)[2] == 255);
    // Bottom-right quadrant is yellow-ish (255, 255, 20).
    const u8* br = first.pixel(kSize - kSize / 8 - 4, kSize - kSize / 8 - 4);
    CHECK(br[0] == 255);
    CHECK(br[1] == 255);
    CHECK(br[2] == 20);
    rhitest::checkGolden(first, "textured_quad");
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: compute writes buffers through device addresses and the bindless heap") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    constexpr u32 kCount = 1000;
    const BufferH viaAddress = makeStorageBuffer(*dev, kCount * 4, "ViaAddress");
    const BufferH viaBindless = makeStorageBuffer(*dev, kCount * 4, "ViaBindless");
    const PipelineH writeAddress =
        makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csWriteAddress", "WriteAddress");
    const PipelineH writeBindless =
        makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csWriteBindless", "WriteBindless");
    struct Push {
        u64 output;
        u32 count;
        u32 bindlessBuffer;
        u32 salt;
        u32 pad;
    };
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Compute");
    cmd->bindPipeline(writeAddress);
    cmd->pushConstants(Push{dev->deviceAddress(viaAddress), kCount, 0, 7, 0});
    cmd->dispatch((kCount + 63) / 64);
    cmd->bindPipeline(writeBindless);
    cmd->pushConstants(Push{0, kCount, dev->srv(viaBindless), 3, 0});
    cmd->dispatch((kCount + 63) / 64);
    cmd->barrier(Barrier::global(ResourceState::UnorderedAccess, ResourceState::CopySource));
    const TimelinePoint done = dev->submit(Queue::Graphics, {&cmd, 1}).value();
    const auto a = readWords(*dev, viaAddress, kCount, ResourceState::CopySource, done);
    const auto b = readWords(*dev, viaBindless, kCount, ResourceState::CopySource, done);
    u32 mismatches = 0;
    for (u32 i = 0; i < kCount; ++i) {
        mismatches += (a[i] != i * i + 7) ? 1u : 0u;
        mismatches += (b[i] != (i ^ 0xABCDu) + 3) ? 1u : 0u;
    }
    CHECK(mismatches == 0);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: compute writes a bindless storage image") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    constexpr u32 kDim = 64;
    const TextureH image = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, kDim, kDim,
                                                                 TextureUsage::Storage | TextureUsage::TransferSrc, "Gradient"))
                               .value();
    const PipelineH pipeline = makeComputePipeline(*dev, rhi_test_shaders::storage_image(), "csMain", "Gradient");
    const u32 push[4] = {dev->uav(image, 0), kDim, kDim, 0};
    REQUIRE(push[0] != kInvalidBindless);
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Storage");
    cmd->barrier(Barrier::textureState(image, ResourceState::Undefined, ResourceState::UnorderedAccess));
    cmd->bindPipeline(pipeline);
    cmd->pushConstants(push, sizeof(push));
    cmd->dispatch(kDim / 8, kDim / 8);
    REQUIRE(dev->submit(Queue::Graphics, {&cmd, 1}).ok());
    const Image result = rhitest::readbackImage(*dev, image, ResourceState::UnorderedAccess);
    int worst = 0;
    for (u32 y = 0; y < kDim; ++y) {
        for (u32 x = 0; x < kDim; ++x) {
            const u8* p = result.pixel(x, y);
            const int er = std::abs(int(p[0]) - int(std::lround(x / 63.0 * 255.0)));
            const int eg = std::abs(int(p[1]) - int(std::lround(y / 63.0 * 255.0)));
            worst = std::max({worst, er, eg, std::abs(int(p[2]) - 64), std::abs(int(p[3]) - 255)});
        }
    }
    CHECK(worst <= 1);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: helios::math matrices upload unchanged (mul(M, v) == M * v)") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    Mat4 m;
    for (int i = 0; i < 16; ++i) m.data()[i] = static_cast<f32>(i * 3 - 7) * 0.5f;  // non-symmetric
    const Vec4 v(1.5f, -2.0f, 0.25f, 3.0f);
    const Vec4 expected = m * v;
    const BufferH out = makeStorageBuffer(*dev, 32, "MatrixOut");
    struct Push {
        f32 m[16];
        f32 v[4];
        u64 output;
    } push{};
    std::memcpy(push.m, m.data(), sizeof(push.m));
    push.v[0] = v.x;
    push.v[1] = v.y;
    push.v[2] = v.z;
    push.v[3] = v.w;
    push.output = dev->deviceAddress(out);
    static_assert(sizeof(Push) == 88);
    const PipelineH pipeline = makeComputePipeline(*dev, rhi_test_shaders::matrix(), "csMain", "Matrix");
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Matrix");
    cmd->bindPipeline(pipeline);
    cmd->pushConstants(push);
    cmd->dispatch(1);
    const TimelinePoint done = dev->submit(Queue::Graphics, {&cmd, 1}).value();
    const auto words = readWords(*dev, out, 8, ResourceState::UnorderedAccess, done);
    f32 result[8];
    std::memcpy(result, words.data(), sizeof(result));
    CHECK(result[0] == doctest::Approx(expected.x));
    CHECK(result[1] == doctest::Approx(expected.y));
    CHECK(result[2] == doctest::Approx(expected.z));
    CHECK(result[3] == doctest::Approx(expected.w));
    // Slang's m[0] is the first row = element (row 0, column c) = data()[c * 4].
    for (int c = 0; c < 4; ++c) CHECK(result[4 + c] == doctest::Approx(m.data()[c * 4]));
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: counter-clockwise triangles are front faces and clip-space +Y is up") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    constexpr u32 kDim = 64;
    const TextureH target = makeTarget(*dev, "Winding", kDim);
    RasterState cullBack;
    cullBack.cullMode = CullMode::Back;
    cullBack.frontFace = FrontFace::CounterClockwise;
    const PipelineH pipeline = makeGraphicsPipeline(*dev, rhi_test_shaders::winding(), "Winding", Format::RGBA8Unorm, cullBack);
    struct Push {
        f32 p[8];
        f32 color[4];
    };
    // CCW in the upper half (y > 0): must be drawn, red, near the top of the image.
    const Push ccw{{-0.9f, 0.1f, -0.1f, 0.1f, -0.5f, 0.9f, 0, 0}, {1, 0, 0, 1}};
    // CW in the lower half: must be culled.
    const Push cw{{0.1f, -0.1f, 0.9f, -0.1f, 0.5f, -0.9f, 0, 0}, {0, 1, 0, 1}};
    renderPass(*dev, target, ResourceState::Undefined, {0, 0, 0, 1}, [&](CommandList& cmd) {
        cmd.bindPipeline(pipeline);
        cmd.pushConstants(ccw);
        cmd.draw(3);
        cmd.pushConstants(cw);
        cmd.draw(3);
    });
    const Image image = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    // Centroid of the CCW triangle: (-0.5, 0.367) -> pixel (16, ~20).
    const u8* inside = image.pixel(16, 21);
    CHECK(inside[0] == 255);
    CHECK(inside[1] == 0);
    // The mirrored position in the bottom half stays clear, as does the CW triangle.
    CHECK(image.pixel(16, kDim - 22)[0] == 0);
    const u8* culled = image.pixel(48, 44);
    CHECK(culled[1] == 0);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: timeline semaphores order work across compute, graphics and transfer queues") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    const std::string asyncKind = dev->caps().has(CapBit::AsyncComputeQueue) ? "dedicated" : "aliased";
    const std::string transferKind = dev->caps().has(CapBit::TransferQueue) ? "dedicated" : "aliased";
    MESSAGE("async compute queue: " << asyncKind << ", transfer queue: " << transferKind);
    constexpr u32 kCount = 4096;
    const BufferH buffer = makeStorageBuffer(*dev, kCount * 4, "Chain");
    const PipelineH write = makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csWriteAddress", "Write");
    const PipelineH increment = makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csIncrement", "Inc");
    struct Push {
        u64 output;
        u32 count;
        u32 bindlessBuffer;
        u32 salt;
        u32 pad;
    } push{dev->deviceAddress(buffer), kCount, 0, 11, 0};

    for (int round = 0; round < 3; ++round) {
        CommandList* compute = dev->acquireCommandList(Queue::AsyncCompute, "Write");
        compute->bindPipeline(write);
        compute->pushConstants(push);
        compute->dispatch(kCount / 64);
        const TimelinePoint c = dev->submit(Queue::AsyncCompute, {&compute, 1}).value();

        CommandList* graphics = dev->acquireCommandList(Queue::Graphics, "Increment");
        graphics->bindPipeline(increment);
        graphics->pushConstants(push);
        graphics->dispatch(kCount / 64);
        const TimelinePoint waitsC[] = {c};
        const TimelinePoint g = dev->submit(Queue::Graphics, {&graphics, 1}, waitsC).value();

        // Transfer-queue readback that waits on the graphics timeline.
        const auto words = readWords(*dev, buffer, kCount, ResourceState::UnorderedAccess, g, Queue::Transfer);
        u32 mismatches = 0;
        for (u32 i = 0; i < kCount; ++i) mismatches += (words[i] != i * i + 11 + 1000) ? 1u : 0u;
        CHECK(mismatches == 0);
        CHECK(dev->isComplete(c));
        CHECK(dev->isComplete(g));
        CHECK(dev->wait(g, 0).ok());  // already complete: zero timeout succeeds
        CHECK(c.value == static_cast<u64>(round + 1));
        REQUIRE(dev->beginFrame().ok());
    }
    CHECK(dev->completedValue(Queue::AsyncCompute) == 3);
    CHECK(dev->completedValue(Queue::Transfer) == 3);
    CHECK(dev->wait(TimelinePoint{Queue::Transfer, 99}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: upload and readback helpers round-trip buffers and texture regions") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    std::vector<u32> data(1024);
    for (u32 i = 0; i < data.size(); ++i) data[i] = i * 2654435761u;
    const BufferH buffer = makeStorageBuffer(*dev, data.size() * 4, "RoundTrip");
    auto up = uploadBuffer(*dev, buffer, 0, std::as_bytes(std::span<const u32>(data)));
    REQUIRE(up.ok());
    const auto back = readWords(*dev, buffer, static_cast<u32>(data.size()), ResourceState::CopyDest, *up);
    CHECK(back == data);

    TextureDesc td = TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                        TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc,
                                        "Mips", 3);
    td.arrayLayers = 2;
    const TextureH tex = dev->createTexture(td).value();
    std::vector<u8> texels(8 * 8 * 4);
    for (usize i = 0; i < texels.size(); ++i) texels[i] = static_cast<u8>(i * 7 + 1);
    REQUIRE(uploadTexture(*dev, tex, std::as_bytes(std::span<const u8>(texels)), TextureRegion{.mip = 1, .baseLayer = 1})
                .ok());
    auto image = readbackTexture(*dev, tex, ResourceState::ShaderResource, 1, 1);
    REQUIRE(image.ok());
    CHECK(*image == texels);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: deferred destruction frees memory and recycles bindless slots across frames") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    const u64 baseBytes = dev->memoryStats().bufferBytes;
    const BufferH a = makeStorageBuffer(*dev, 1 << 20, "A");
    const BindlessIndex slot = dev->srv(a);
    CHECK(dev->memoryStats().bufferBytes >= baseBytes + (1 << 20));
    // Use it on the GPU, destroy right after submission (allowed: destruction is deferred).
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Fill");
    cmd->fillBuffer(a, 0, kWholeSize, 0x12345678u);
    REQUIRE(dev->submit(Queue::Graphics, {&cmd, 1}).ok());
    dev->destroy(a);
    CHECK(dev->deviceAddress(a) == 0);  // handle is stale immediately
    for (u32 i = 0; i < 3; ++i) REQUIRE(dev->beginFrame().ok());
    CHECK(dev->memoryStats().bufferBytes == baseBytes);
    const BufferH b = makeStorageBuffer(*dev, 64, "B");
    CHECK(dev->srv(b) == slot);
    dev->destroy(b);
    REQUIRE(dev->waitIdle().ok());
    CHECK(dev->memoryStats().bufferBytes == baseBytes);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: async pipeline compilation on a background pool") {
    jobs::BackgroundPool pool(2, "PSO");
    DeviceDesc desc;
    desc.pipelineCompilePool = &pool;
    auto dev = rhitest::createGpuDevice(desc);
    if (!dev) return;
    const PipelineH pipeline = makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csWriteAddress",
                                                   "AsyncWrite", PsoPriority::Normal);
    const auto start = std::chrono::steady_clock::now();
    while (!dev->isReady(pipeline) && std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(dev->isReady(pipeline));
    const BufferH out = makeStorageBuffer(*dev, 256, "AsyncOut");
    struct Push {
        u64 output;
        u32 count;
        u32 bindlessBuffer;
        u32 salt;
        u32 pad;
    };
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Async");
    cmd->bindPipeline(pipeline);
    cmd->pushConstants(Push{dev->deviceAddress(out), 64, 0, 5, 0});
    cmd->dispatch(1);
    const TimelinePoint done = dev->submit(Queue::Graphics, {&cmd, 1}).value();
    const auto words = readWords(*dev, out, 64, ResourceState::UnorderedAccess, done);
    CHECK(words[10] == 105u);
    dev->destroy(pipeline);
    CHECK(dev->validationErrorCount() == 0);
    CHECK(dev->memoryStats().psoMisses == 0);
}

TEST_CASE("gpu: command lists recorded in parallel on the job system") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    constexpr u32 kJobs = 8;
    constexpr u32 kPerJob = 256;
    const BufferH buffer = makeStorageBuffer(*dev, kJobs * kPerJob * 4, "Parallel");
    const PipelineH write = makeComputePipeline(*dev, rhi_test_shaders::compute_buffers(), "csWriteAddress", "Write");
    std::vector<CommandList*> lists(kJobs, nullptr);
    {
        jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 4});
        jobs::Counter counter;
        for (u32 j = 0; j < kJobs; ++j) {
            js.run(
                [&, j] {
                    struct Push {
                        u64 output;
                        u32 count;
                        u32 bindlessBuffer;
                        u32 salt;
                        u32 pad;
                    };
                    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Job");
                    cmd->bindPipeline(write);
                    cmd->pushConstants(Push{dev->deviceAddress(buffer) + u64(j) * kPerJob * 4, kPerJob, 0, j * 100000, 0});
                    cmd->dispatch(kPerJob / 64);
                    cmd->end();
                    lists[j] = cmd;
                },
                &counter);
        }
        js.wait(counter);
    }
    const TimelinePoint done = dev->submit(Queue::Graphics, lists).value();
    const auto words = readWords(*dev, buffer, kJobs * kPerJob, ResourceState::UnorderedAccess, done);
    u32 mismatches = 0;
    for (u32 j = 0; j < kJobs; ++j) {
        for (u32 i = 0; i < kPerJob; ++i) mismatches += (words[j * kPerJob + i] != i * i + j * 100000) ? 1u : 0u;
    }
    CHECK(mismatches == 0);
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: breadcrumbs, labels, pipeline cache and usage errors") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Breadcrumbs");
    {
        ScopedLabel label(*cmd, "Pass 42", 0xFF8000FFu);
        cmd->breadcrumb(42, BreadcrumbStage::Begin);
        cmd->insertLabel("marker");
        cmd->breadcrumb(42, BreadcrumbStage::End);
    }
    const TimelinePoint done = dev->submit(Queue::Graphics, {&cmd, 1}).value();
    REQUIRE(dev->wait(done).ok());
    const auto crumbs = dev->breadcrumbs();
    CHECK((crumbs[0].lastBegin & 0xFFFF) == 42u);
    CHECK((crumbs[0].lastEnd & 0xFFFF) == 42u);

    const PipelineH p = makeGraphicsPipeline(*dev, rhi_test_shaders::triangle(), "CacheMe");
    CHECK_FALSE(dev->pipelineCacheData().empty());
    dev->destroy(p);

    // Misuse is reported and skipped instead of crashing the driver.
    rhitest::QuietRhiLog quiet;
    const u64 before = dev->validationErrorCount();
    CommandList* bad = dev->acquireCommandList(Queue::Graphics, "Bad");
    bad->dispatch(1);                       // no compute pipeline bound
    bad->bindPipeline(PipelineH(123, 7));   // stale handle
    bad->endRendering();                    // not rendering
    REQUIRE(dev->submit(Queue::Graphics, {&bad, 1}).ok());
    CHECK(dev->validationErrorCount() == before + 3);
    CHECK(dev->createGraphicsPipeline(GraphicsPipelineDesc{}).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("gpu: CPU recording overhead stays within budget") {
    // Budget (03 §8.1: recording <= 2.0 ms per frame across jobs, <= 1,000 CPU commands per
    // frame): the RHI wrapper around bindPipeline/pushConstants/draw must stay far below 1 us per
    // draw on REF hardware. lavapipe's own vkCmd* cost is included here, so the CI bound is loose;
    // it catches pathological regressions (locks or allocations per command), not micro-costs.
    // Validation layers multiply the per-command cost (~30x measured with Khronos validation on
    // lavapipe), so time without them; CI that forces validation (HELIOS_RHI_VALIDATION=1) only
    // gets the functional part of this test.
    auto dev = rhitest::createGpuDevice({}, {.validation = false});
    if (!dev) return;
    const bool timed = !dev->caps().has(CapBit::ValidationLayer);
    const TextureH target = makeTarget(*dev, "Overhead", 16);
    const PipelineH pipeline = makeGraphicsPipeline(*dev, rhi_test_shaders::triangle(), "Overhead");
    constexpr u32 kDraws = 20000;
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Overhead");
    cmd->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    const ColorAttachment ca{.texture = target};
    cmd->beginRendering({.colors = {&ca, 1}});
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kDraws; ++i) {
        if (i % 100 == 0) cmd->bindPipeline(pipeline);
        const u32 push[4] = {i, i + 1, i + 2, i + 3};
        cmd->pushConstants(push, sizeof(push));
        cmd->draw(3);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    cmd->endRendering();
    cmd->end();  // recorded but never submitted: waitIdle recycles it
    const double perDraw = us / kDraws;
    const std::string note = timed ? "" : " [validation layer active: not checked]";
    MESSAGE("recording: " << perDraw << " us per bind/push/draw (" << kDraws << " draws)" << note);
    if (timed) CHECK(perDraw < 5.0);
    REQUIRE(dev->waitIdle().ok());
    CHECK(dev->validationErrorCount() == 0);
}

TEST_CASE("gpu: injected device loss runs the crash-diagnostics path") {
    std::vector<DeviceLostInfo> lost;
    DeviceDesc desc;
    desc.debugDeviceLostAfterSubmits = 3;  // device creation itself submits once (default resources)
    desc.onDeviceLost = [&](const DeviceLostInfo& info) { lost.push_back(info); };
    auto dev = rhitest::createGpuDevice(desc);
    if (!dev) return;
    rhitest::QuietRhiLog quiet;
    auto pass = [&](u16 id) {
        CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Pass");
        REQUIRE(cmd != nullptr);
        cmd->breadcrumb(id, BreadcrumbStage::Begin);
        cmd->breadcrumb(id, BreadcrumbStage::End);
        return dev->submit(Queue::Graphics, {&cmd, 1});
    };
    CHECK(pass(5).ok());
    CHECK(pass(6).errorCode() == ErrorCode::InvalidState);
    CHECK(dev->isDeviceLost());
    REQUIRE(lost.size() == 1);
    CHECK(lost[0].reason.find("VK_ERROR_DEVICE_LOST") != std::string::npos);
    CHECK((lost[0].breadcrumbs[0].lastBegin & 0xFFFF) == 6u);
    CHECK(dev->acquireCommandList(Queue::Graphics, "After") == nullptr);
    CHECK(dev->createBuffer({.size = 16}).errorCode() == ErrorCode::InvalidState);
    CHECK(dev->beginFrame().errorCode() == ErrorCode::InvalidState);
    // Destruction after a loss must still be clean (no hang, no crash): happens at scope exit.
}

} // TEST_SUITE("gpu")
