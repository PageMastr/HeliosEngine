// Vulkan backend robustness: API misuse must be reported (validationErrorCount / onMessage) and
// dropped instead of reaching the driver — a software driver such as lavapipe executes an
// out-of-range copy with plain memcpy and corrupts host memory — list lifetimes are enforced at
// submit, several devices coexist, and released bindless slots fall back to the defaults.
// Runs in the "gpu" suite (rhi_tests_gpu, lavapipe in CI). No window needed.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "helios/rhi/rhi.h"
#include "rhi_tests_shaders.h"
#include "test_util.h"

using namespace helios;
using namespace helios::rhi;

namespace {

BufferH makeBuffer(Device& dev, u64 size, std::string_view name,
                   BufferUsage usage = BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst) {
    return dev.createBuffer({.size = size, .usage = usage, .name = name}).value();
}

std::vector<u32> readWords(Device& dev, BufferH buffer, u32 count, TimelinePoint after) {
    auto bytes = readbackBuffer(dev, buffer, 0, u64(count) * 4, ResourceState::CopyDest, after);
    REQUIRE_MESSAGE(bytes.ok(), (bytes.ok() ? std::string() : bytes.error().toString()));
    std::vector<u32> out(count);
    std::memcpy(out.data(), bytes->data(), bytes->size());
    return out;
}

/// Tracks the device's validation-error count against the expected one, per misuse.
struct ErrorLedger {
    Device& dev;
    u64 expected;
    explicit ErrorLedger(Device& d) : dev(d), expected(d.validationErrorCount()) {}
    void expect(u64 errors, std::string_view what) {
        expected += errors;
        INFO(std::string(what));
        CHECK(dev.validationErrorCount() == expected);
        expected = dev.validationErrorCount();  // keep later checks independent
    }
};

} // namespace

TEST_SUITE("gpu") {

TEST_CASE("gpu: devices keep their own Vulkan function tables (several devices, adapter enumeration)") {
    // Regression: the backend used volk's process-global instance function pointers. Enumerating
    // adapters or creating a device with other extensions rebound them for every live device, so
    // a device could end up calling NULL surface/debug-utils entry points (leaked debug messenger
    // on destruction — reported by the validation layer — and a crash in swapchain code).
    std::atomic<u32> errors{0};
    DeviceDesc desc;
    desc.onMessage = [&](const ValidationMessage& m) {
        if (m.severity == ValidationMessage::Severity::Error) errors.fetch_add(1);
    };
    auto a = rhitest::createGpuDevice(desc);
    if (!a) return;
    REQUIRE(Device::enumerateAdapters(Backend::Vulkan).ok());
    const BufferH bufferA = makeBuffer(*a, 256, "A");
    {
        DeviceDesc bareDesc;
        bareDesc.capsMask = ~(CapBit::DebugUtils | CapBit::MemoryBudget);
        bareDesc.debugNames = false;
        auto b = rhitest::createGpuDevice(bareDesc);
        REQUIRE(b);
        CHECK_FALSE(b->caps().has(CapBit::DebugUtils));
        REQUIRE(Device::enumerateAdapters(Backend::Vulkan).ok());
        // Both devices work side by side.
        const BufferH bufferB = makeBuffer(*b, 256, "B");
        CommandList* cb = b->acquireCommandList(Queue::Graphics, "B");
        cb->fillBuffer(bufferB, 0, kWholeSize, 0xB0B0B0B0u);
        const TimelinePoint doneB = b->submit(Queue::Graphics, {&cb, 1}).value();
        CHECK(readWords(*b, bufferB, 64, doneB)[63] == 0xB0B0B0B0u);
        CHECK(b->validationErrorCount() == 0);
        // A list belongs to the device that created it.
        rhitest::QuietRhiLog quiet;
        CommandList* foreign = b->acquireCommandList(Queue::Graphics, "Foreign");
        CHECK(a->submit(Queue::Graphics, {&foreign, 1}).errorCode() == ErrorCode::InvalidArgument);
        CHECK(a->validationErrorCount() == 1);
        REQUIRE(b->submit(Queue::Graphics, {&foreign, 1}).ok());
    }
    // `a` still names, labels and executes after `b` is gone.
    CommandList* ca = a->acquireCommandList(Queue::Graphics, "A");
    {
        ScopedLabel label(*ca, "After other device");
        ca->insertLabel("marker");
        ca->fillBuffer(bufferA, 0, kWholeSize, 0xA0A0A0A0u);
    }
    const TimelinePoint doneA = a->submit(Queue::Graphics, {&ca, 1}).value();
    CHECK(readWords(*a, bufferA, 64, doneA)[0] == 0xA0A0A0A0u);
    CHECK(a->validationErrorCount() == 1);  // only the foreign submit
    a.reset();  // destruction releases the debug messenger through a's own table
    CHECK(errors.load() == 1);
}

TEST_CASE("gpu: misuse is reported and dropped instead of reaching the driver") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    rhitest::QuietRhiLog quiet;
    const BufferH small = makeBuffer(*dev, 64, "Small");
    const BufferH big = makeBuffer(*dev, 4096, "Big");
    const BufferH indirect =
        makeBuffer(*dev, 32, "Indirect", BufferUsage::Indirect | BufferUsage::Storage | BufferUsage::TransferDst);
    const TextureH tex = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                                               TextureUsage::TransferDst | TextureUsage::TransferSrc, "Tex"))
                             .value();
    const TextureH target =
        dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 32, 32, TextureUsage::ColorAttachment, "Target")).value();
    GraphicsPipelineDesc gd;
    gd.vertex = ShaderDesc{rhi_test_shaders::triangle(), "vsMain"};
    gd.fragment = ShaderDesc{rhi_test_shaders::triangle(), "psMain"};
    gd.colorCount = 1;
    gd.colorFormats[0] = Format::RGBA8Unorm;
    const PipelineH graphics = dev->createGraphicsPipeline(gd).value();
    const PipelineH compute =
        dev->createComputePipeline({ShaderDesc{rhi_test_shaders::compute_buffers(), "csWriteAddress"}, "cs"}).value();
    ErrorLedger ledger(*dev);

    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "Misuse");
    cmd->fillBuffer(small, 0, kWholeSize, 0x11111111u);  // the one valid write
    ledger.expect(0, "valid fill");

    // Buffer transfers out of range: each would be a host-memory overwrite on lavapipe.
    cmd->copyBuffer(big, 0, small, 0, 128);
    ledger.expect(1, "copy past the end of the destination");
    cmd->copyBuffer(big, ~0ull - 8, small, 0, 16);
    ledger.expect(1, "source offset overflow");
    cmd->copyBuffer(big, 0, small, 0, 0);
    ledger.expect(1, "zero-size copy");
    cmd->copyBuffer(big, 0, big, 8, 64);
    ledger.expect(1, "overlapping copy within one buffer");
    cmd->copyBuffer(indirect, 0, small, 0, 16);
    ledger.expect(1, "source lacks TransferSrc");
    cmd->fillBuffer(small, 60, 8, 0);
    ledger.expect(1, "fill past the end");
    cmd->fillBuffer(small, 2, 4, 0);
    ledger.expect(1, "unaligned fill");
    const u32 words[4] = {1, 2, 3, 4};
    cmd->updateBuffer(small, 56, std::as_bytes(std::span<const u32>(words)));
    ledger.expect(1, "update past the end");

    // Buffer <-> texture copies with bad regions or layouts.
    cmd->copyBufferToTexture(big, {}, tex, TextureRegion{.x = 8, .width = 16});
    ledger.expect(1, "region past the texture edge");
    cmd->copyBufferToTexture(big, {}, tex, TextureRegion{.mip = 3});
    ledger.expect(1, "mip out of range");
    cmd->copyBufferToTexture(big, {}, tex, TextureRegion{.x = 16});
    ledger.expect(1, "region origin outside the texture");
    cmd->copyTextureToBuffer(tex, {}, small, {});
    ledger.expect(1, "destination buffer too small (16x16x4 bytes > 64)");
    cmd->copyBufferToTexture(big, {.offset = 2}, tex, {});
    ledger.expect(1, "buffer offset not a multiple of the texel size");
    cmd->copyBufferToTexture(big, {.rowPitch = 8}, tex, {});
    ledger.expect(1, "row pitch shorter than a row");
    cmd->copyBufferToTexture(big, {.offset = 3076}, tex, {});
    ledger.expect(1, "source range past the end of the buffer (3076 + 1024 > 4096)");

    // Compute and indirect arguments.
    cmd->dispatch(1);
    ledger.expect(1, "dispatch without a compute pipeline");
    cmd->bindPipeline(compute);
    cmd->dispatchIndirect(indirect, 24);
    ledger.expect(1, "indirect dispatch arguments past the end");
    cmd->dispatchIndirect(indirect, 2);
    ledger.expect(1, "unaligned indirect offset");
    if (const u32 maxGroups = dev->caps().limits.maxComputeWorkGroupCount[0]; maxGroups < 0xFFFFFFFFu) {
        cmd->dispatch(maxGroups + 1u);
        ledger.expect(1, "group count above the device limit");
    }
    cmd->pushConstants(nullptr, 16);
    ledger.expect(1, "null push-constant data");

    // Rendering-scope misuse.
    cmd->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    cmd->barrier(Barrier::textureState(tex, ResourceState::Undefined, ResourceState::CopyDest, {.baseMip = 4}));
    ledger.expect(1, "barrier outside the texture's mips");
    const ColorAttachment ca{.texture = target};
    const ColorAttachment wrongUsage{.texture = tex};
    cmd->beginRendering({.colors = {&wrongUsage, 1}});
    ledger.expect(1, "attachment without ColorAttachment usage");
    cmd->beginRendering({.colors = {&ca, 1}, .area = Rect{16, 16, 32, 32}});
    ledger.expect(1, "render area outside the attachment");
    cmd->beginRendering({.colors = {&ca, 1}});
    cmd->bindPipeline(graphics);
    cmd->drawIndexed(3);
    ledger.expect(1, "drawIndexed without an index buffer");
    cmd->drawIndirect(indirect, 0, 3);
    ledger.expect(1, "indirect draws past the end of the buffer");
    cmd->bindIndexBuffer(small, 0, IndexType::Uint32);
    ledger.expect(1, "index buffer without Index usage");
    cmd->copyBuffer(big, 0, small, 0, 16);
    ledger.expect(1, "copy inside rendering");
    cmd->draw(3);
    ledger.expect(0, "valid draw");
    cmd->endRendering();

    // Read-only depth cannot be cleared: reported, then loaded instead.
    const TextureH depth =
        dev->createTexture(TextureDesc::tex2D(Format::D32Float, 32, 32, TextureUsage::DepthStencil, "Depth")).value();
    cmd->barrier(Barrier::textureState(depth, ResourceState::Undefined, ResourceState::DepthRead));
    RenderingDesc readOnly;
    readOnly.colors = {&ca, 1};
    readOnly.depth = DepthAttachment{.texture = depth, .load = LoadOp::Clear, .readOnly = true};
    cmd->beginRendering(readOnly);
    ledger.expect(1, "read-only depth attachment with LoadOp::Clear");
    cmd->endRendering();

    // Queue rules are logical: the same code must work where Transfer is a dedicated queue.
    CommandList* transfer = dev->acquireCommandList(Queue::Transfer, "TransferMisuse");
    transfer->bindPipeline(compute);
    ledger.expect(1, "compute pipeline on the Transfer queue");
    transfer->clearTexture(tex, {1, 0, 0, 1});
    ledger.expect(1, "clearTexture on the Transfer queue");
    transfer->end();
    REQUIRE(dev->submit(Queue::Transfer, {&transfer, 1}).ok());

    // Recording into a closed list.
    cmd->end();
    cmd->fillBuffer(small, 0, 4, 0xDEADu);
    cmd->draw(3);
    cmd->end();
    ledger.expect(3, "commands and end() on an ended list");

    const TimelinePoint done = dev->submit(Queue::Graphics, {&cmd, 1}).value();
    const auto result = readWords(*dev, small, 16, done);
    u32 intact = 0;
    for (u32 w : result) intact += w == 0x11111111u ? 1u : 0u;
    CHECK(intact == 16);  // only the valid fill landed

    // Creation-time limits fail cleanly instead of reaching vkCreateImage.
    TextureDesc tooManySamples = TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16, TextureUsage::ColorAttachment);
    tooManySamples.sampleCount = 64;
    CHECK(dev->createTexture(tooManySamples).errorCode() == ErrorCode::Unsupported);
    TextureDesc volumeTarget = TextureDesc::tex2D(Format::RGBA8Unorm, 8, 8, TextureUsage::ColorAttachment);
    volumeTarget.type = TextureType::Tex3D;
    volumeTarget.depth = 4;
    CHECK(dev->createTexture(volumeTarget).errorCode() == ErrorCode::Unsupported);
    TextureDesc packedDepth = TextureDesc::tex2D(Format::D32FloatS8Uint, 8, 8,
                                                 TextureUsage::DepthStencil | TextureUsage::TransferSrc);
    if (auto packed = dev->createTexture(packedDepth)) {
        // Its depth aspect copies as 4-byte texels, not the 8-byte layout Format describes.
        CHECK(readbackTexture(*dev, *packed, ResourceState::Undefined).errorCode() == ErrorCode::Unsupported);
        dev->destroy(*packed);
    }
}

TEST_CASE("gpu: stale, recycled and twice-submitted command lists are rejected at submit") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    rhitest::QuietRhiLog quiet;
    const BufferH buffer = makeBuffer(*dev, 64, "Lists");
    ErrorLedger ledger(*dev);

    CommandList* once = dev->acquireCommandList(Queue::Graphics, "Once");
    once->fillBuffer(buffer, 0, kWholeSize, 1u);
    REQUIRE(dev->submit(Queue::Graphics, {&once, 1}).ok());
    CHECK(dev->submit(Queue::Graphics, {&once, 1}).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "second submit of a one-time list");

    CommandList* dup = dev->acquireCommandList(Queue::Graphics, "Dup");
    dup->barrier(Barrier::bufferState(buffer, ResourceState::CopyDest, ResourceState::CopyDest));  // after 'Once'
    dup->fillBuffer(buffer, 0, kWholeSize, 2u);
    CommandList* twice[] = {dup, dup};
    CHECK(dev->submit(Queue::Graphics, twice).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "the same list twice in one submit");
    const TimelinePoint dupDone = dev->submit(Queue::Graphics, {&dup, 1}).value();  // still usable
    CHECK(readWords(*dev, buffer, 16, dupDone)[15] == 2u);

    CommandList* wrongQueue = dev->acquireCommandList(Queue::AsyncCompute, "WrongQueue");
    CHECK(dev->submit(Queue::Graphics, {&wrongQueue, 1}).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "list submitted to another queue");
    REQUIRE(dev->submit(Queue::AsyncCompute, {&wrongQueue, 1}).ok());

    // Still recording on another thread: ending it from here would race with that thread's pool.
    CommandList* open = nullptr;
    std::thread recorder([&] {
        open = dev->acquireCommandList(Queue::Graphics, "OpenElsewhere");
        open->fillBuffer(buffer, 0, kWholeSize, 9u);
    });
    recorder.join();
    CHECK(dev->submit(Queue::Graphics, {&open, 1}).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "list still recording on another thread");
    const TimelinePoint badWait[] = {TimelinePoint{Queue::AsyncCompute, 1000}};
    CommandList* waits = dev->acquireCommandList(Queue::Graphics, "BadWait");
    CHECK(dev->submit(Queue::Graphics, {&waits, 1}, badWait).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "wait for a value that was never submitted");
    REQUIRE(dev->submit(Queue::Graphics, {&waits, 1}).ok());  // the list was left untouched

    // Acquired in one frame, submitted after beginFrame: its pool belongs to the old frame slot.
    CommandList* stale = dev->acquireCommandList(Queue::Graphics, "Stale");
    stale->fillBuffer(buffer, 0, kWholeSize, 3u);
    REQUIRE(dev->beginFrame().ok());
    CHECK(dev->submit(Queue::Graphics, {&stale, 1}).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "list from a previous frame");

    // waitIdle recycles every pool: the list is back in the initial state.
    CommandList* recycled = dev->acquireCommandList(Queue::Graphics, "Recycled");
    REQUIRE(dev->waitIdle().ok());
    recycled->fillBuffer(buffer, 0, kWholeSize, 4u);
    ledger.expect(1, "recording into a recycled list");
    CHECK(dev->submit(Queue::Graphics, {&recycled, 1}).errorCode() == ErrorCode::InvalidArgument);
    ledger.expect(1, "submitting a recycled list");

    // The device is unaffected: the buffer still holds the last valid write.
    CommandList* check = dev->acquireCommandList(Queue::Graphics, "Check");
    check->fillBuffer(buffer, 0, 4, 5u);
    const TimelinePoint done = dev->submit(Queue::Graphics, {&check, 1}).value();
    const auto words = readWords(*dev, buffer, 16, done);
    CHECK(words[0] == 5u);
    CHECK(words[1] == 2u);
    ledger.expect(0, "valid work after the rejected submits");
}

TEST_CASE("gpu: a destroyed texture's bindless slot falls back to the default texture") {
    auto dev = rhitest::createGpuDevice();
    if (!dev) return;
    std::vector<u8> white(8 * 8 * 4, 255);
    const TextureH tex = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 8, 8,
                                                               TextureUsage::Sampled | TextureUsage::TransferDst, "Doomed"))
                             .value();
    REQUIRE(uploadTexture(*dev, tex, std::as_bytes(std::span<const u8>(white))).ok());
    const BindlessIndex slot = dev->srv(tex);
    REQUIRE(slot != kInvalidBindless);
    dev->destroy(tex);
    for (int i = 0; i < 3; ++i) REQUIRE(dev->beginFrame().ok());  // the deferred free has run

    // A stale index now reads the 1x1 magenta default instead of a destroyed image view.
    SamplerDesc nearest;
    nearest.minFilter = nearest.magFilter = nearest.mipFilter = Filter::Nearest;
    struct Push {
        u32 texture;
        u32 samplerIndex;
        f32 scale[2];
    } push{slot, dev->sampler(nearest), {0.75f, 0.75f}};
    const TextureH target = dev->createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 32, 32,
                                                                  TextureUsage::ColorAttachment | TextureUsage::TransferSrc,
                                                                  "Target"))
                                .value();
    GraphicsPipelineDesc gd;
    gd.vertex = ShaderDesc{rhi_test_shaders::textured_quad(), "vsMain"};
    gd.fragment = ShaderDesc{rhi_test_shaders::textured_quad(), "psMain"};
    gd.colorCount = 1;
    gd.colorFormats[0] = Format::RGBA8Unorm;
    const PipelineH pipeline = dev->createGraphicsPipeline(gd).value();
    CommandList* cmd = dev->acquireCommandList(Queue::Graphics, "StaleSlot");
    cmd->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    const ColorAttachment ca{.texture = target};
    cmd->beginRendering({.colors = {&ca, 1}});
    cmd->bindPipeline(pipeline);
    cmd->pushConstants(push);
    cmd->draw(6);
    cmd->endRendering();
    cmd->barrier(Barrier::textureState(target, ResourceState::RenderTarget, ResourceState::CopySource));
    REQUIRE(dev->submit(Queue::Graphics, {&cmd, 1}).ok());
    const rhitest::Image image = rhitest::readbackImage(*dev, target, ResourceState::CopySource);
    const u8* center = image.pixel(16, 16);
    CHECK(center[0] == 255);
    CHECK(center[1] == 0);
    CHECK(center[2] == 255);
    CHECK(dev->validationErrorCount() == 0);
}

} // TEST_SUITE("gpu")
