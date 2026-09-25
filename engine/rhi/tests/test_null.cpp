// Null backend: API behavior, validation, command-stream trace goldens, memory emulation,
// timelines, parallel recording and swapchain flow — everything that must work without a GPU.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/rhi/rhi.h"
#include "rhi_tests_shaders.h"
#include "test_util.h"

using namespace helios;
using namespace helios::rhi;

namespace {

struct NullFixture {
    std::unique_ptr<Device> device;
    NullDevice* null = nullptr;

    explicit NullFixture(DeviceDesc desc = {}) {
        desc.backend = Backend::Null;
        device = Device::create(desc).value();
        null = NullDevice::from(*device);
        REQUIRE(null != nullptr);
    }

    /// True if some recorded validation error contains `needle`; clears the error list.
    bool takeError(std::string_view needle) {
        const auto errors = null->validationErrors();
        null->clearValidationErrors();
        return std::any_of(errors.begin(), errors.end(),
                           [&](const std::string& e) { return e.find(needle) != std::string::npos; });
    }
    usize errorCount() const { return null->validationErrors().size(); }
    /// All validation errors joined, for INFO() context.
    std::string errors() const {
        std::string out;
        for (const std::string& e : null->validationErrors()) out += "\n  " + e;
        return out;
    }
};

#define CHECK_NO_ERRORS(f)        \
    do {                          \
        INFO((f).errors());       \
        CHECK((f).errorCount() == 0); \
    } while (false)

GraphicsPipelineDesc trianglePipelineDesc(Format color = Format::RGBA8Unorm) {
    GraphicsPipelineDesc d;
    d.vertex = ShaderDesc{rhi_test_shaders::triangle(), "vsMain"};
    d.fragment = ShaderDesc{rhi_test_shaders::triangle(), "psMain"};
    d.colorCount = 1;
    d.colorFormats[0] = color;
    d.name = "Triangle";
    return d;
}

TextureH makeTarget(Device& device, std::string_view name = "Color", u32 size = 64) {
    return device
        .createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, size, size,
                                          TextureUsage::ColorAttachment | TextureUsage::TransferSrc, name))
        .value();
}

std::span<const std::byte> bytesOf(const std::vector<u32>& v) { return std::as_bytes(std::span<const u32>(v)); }

} // namespace

TEST_CASE("null: device, caps and adapters") {
    NullFixture f;
    const Caps& caps = f.device->caps();
    CHECK(f.device->backend() == Backend::Null);
    CHECK(caps.adapter.name == "Helios Null Device");
    CHECK(caps.limits.maxPushConstantBytes == 128);
    CHECK(caps.limits.maxBindlessSampledImages == 131072);
    CHECK(caps.has(CapBit::AsyncComputeQueue));
    CHECK_FALSE(caps.has(CapBit::MeshShader));
    auto adapters = Device::enumerateAdapters(Backend::Null);
    REQUIRE(adapters.ok());
    REQUIRE(adapters->size() == 1);
    CHECK(adapters->front().meetsRequirements);
    CHECK(f.device->frameIndex() == 0);
    CHECK_FALSE(f.device->isDeviceLost());

    // capsMask removes optional capabilities.
    DeviceDesc masked;
    masked.capsMask = ~CapBit::AsyncComputeQueue;
    NullFixture g(masked);
    CHECK_FALSE(g.device->caps().has(CapBit::AsyncComputeQueue));
    CHECK(g.device->caps().has(CapBit::TransferQueue));

    DeviceDesc d3d;
    d3d.backend = Backend::D3D12;
    CHECK(Device::create(d3d).errorCode() == ErrorCode::Unsupported);
}

TEST_CASE("null: buffers — descs, mapping, addresses, bindless slots, stale handles") {
    NullFixture f;
    Device& dev = *f.device;
    CHECK(dev.createBuffer({.size = 0}).errorCode() == ErrorCode::InvalidArgument);
    CHECK(dev.createBuffer({.size = 16, .usage = BufferUsage::None}).errorCode() == ErrorCode::InvalidArgument);

    const BufferH gpu = dev.createBuffer({.size = 1000, .usage = BufferUsage::Storage, .name = "Gpu"}).value();
    const BufferH upload =
        dev.createBuffer({.size = 64, .usage = BufferUsage::TransferSrc, .memory = MemoryUsage::Upload, .name = "Up"}).value();
    const BufferH readback =
        dev.createBuffer({.size = 64, .usage = BufferUsage::TransferDst, .memory = MemoryUsage::Readback}).value();
    CHECK(dev.bufferDesc(gpu).name == "Gpu");
    CHECK(dev.bufferDesc(gpu).size == 1000);
    CHECK(dev.map(gpu) == nullptr);
    CHECK(dev.map(upload) != nullptr);
    CHECK(dev.map(readback) != nullptr);

    const u64 a = dev.deviceAddress(gpu);
    const u64 b = dev.deviceAddress(upload);
    CHECK(a != 0);
    CHECK(b != 0);
    CHECK(a != b);
    CHECK(a % 256 == 0);

    const BindlessIndex slot = dev.srv(gpu);
    CHECK(slot != kInvalidBindless);
    CHECK(slot != 0);  // slot 0 is the default resource
    CHECK(dev.srv(upload) == kInvalidBindless);  // no Storage usage
    CHECK(f.takeError("lacks BufferUsage::Storage"));

    CHECK(dev.memoryStats().bufferCount == 3);
    dev.destroy(gpu);
    CHECK(dev.deviceAddress(gpu) == 0);  // stale
    CHECK(dev.bufferDesc(gpu).size == 0);
    dev.destroy(gpu);                    // double destroy is ignored
    const BufferH again = dev.createBuffer({.size = 16, .usage = BufferUsage::Storage}).value();
    CHECK(again != gpu);                   // new generation
    CHECK(dev.srv(again) == slot);         // bindless slot recycled
    CHECK(dev.memoryStats().bufferCount == 3);
}

TEST_CASE("null: textures — validation, views and bindless slots") {
    NullFixture f;
    Device& dev = *f.device;
    auto bad = [&](TextureDesc d) { return dev.createTexture(d).errorCode(); };
    CHECK(bad(TextureDesc::tex2D(Format::RGBA8Unorm, 0, 4, TextureUsage::Sampled)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::Unknown, 4, 4, TextureUsage::Sampled)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::RGBA8Unorm, 4, 4, TextureUsage::Sampled, "x", 4)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::D32Float, 4, 4, TextureUsage::ColorAttachment)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::RGBA8Unorm, 4, 4, TextureUsage::DepthStencil)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::BC7Unorm, 4, 4, TextureUsage::ColorAttachment)) == ErrorCode::InvalidArgument);
    CHECK(bad(TextureDesc::tex2D(Format::RGBA8Unorm, 99999, 4, TextureUsage::Sampled)) == ErrorCode::LimitExceeded);
    TextureDesc cube = TextureDesc::tex2D(Format::RGBA8Unorm, 8, 4, TextureUsage::Sampled);
    cube.type = TextureType::Cube;
    cube.arrayLayers = 6;
    CHECK(bad(cube) == ErrorCode::InvalidArgument);  // not square

    TextureDesc arr = TextureDesc::tex2D(Format::RGBA16Float, 32, 32, TextureUsage::Sampled | TextureUsage::Storage,
                                         "Array", 3);
    arr.arrayLayers = 4;
    const TextureH t = dev.createTexture(arr).value();
    const BindlessIndex whole = dev.srv(t);
    CHECK(whole != kInvalidBindless);
    CHECK(dev.srv(t, {}) == whole);
    const BindlessIndex layer2 = dev.srv(t, ViewDesc{.baseMip = 1, .mipCount = 1, .baseLayer = 2, .layerCount = 1});
    CHECK(layer2 != kInvalidBindless);
    CHECK(layer2 != whole);
    CHECK(dev.srv(t, ViewDesc{.baseMip = 1, .mipCount = 1, .baseLayer = 2, .layerCount = 1}) == layer2);  // cached
    CHECK(dev.srv(t, ViewDesc{.baseMip = 5}) == kInvalidBindless);  // out of range
    CHECK(f.takeError("out of range"));
    CHECK(dev.srv(t, ViewDesc{.type = ViewType::Cube}) == kInvalidBindless);  // not a cube
    CHECK(f.takeError("view type"));
    const BindlessIndex u0 = dev.uav(t, 0);
    const BindlessIndex u1 = dev.uav(t, 1);
    CHECK(u0 != kInvalidBindless);
    CHECK(u1 != u0);
    CHECK(dev.uav(t, 0) == u0);
    CHECK(dev.uav(t, 7) == kInvalidBindless);
    CHECK(f.takeError("uav"));
    CHECK(dev.textureDesc(t).name == "Array");
    CHECK(f.null->textureState(t, 2, 3) == ResourceState::Undefined);
    dev.destroy(t);
    CHECK(dev.textureDesc(t).width == 0);
}

TEST_CASE("null: samplers are deduplicated, slot 0 is the default") {
    NullFixture f;
    Device& dev = *f.device;
    CHECK(dev.sampler({}) == 0);
    SamplerDesc nearest;
    nearest.minFilter = nearest.magFilter = nearest.mipFilter = Filter::Nearest;
    const BindlessIndex n = dev.sampler(nearest);
    CHECK(n == 1);
    CHECK(dev.sampler(nearest) == n);
    SamplerDesc minmax;
    minmax.reduction = SamplerReduction::Max;
    CHECK(dev.sampler(minmax) == 2);
}

TEST_CASE("null: pipelines validate SPIR-V entry points and stages") {
    NullFixture f;
    Device& dev = *f.device;
    CHECK(dev.createGraphicsPipeline(trianglePipelineDesc()).ok());

    GraphicsPipelineDesc wrongEntry = trianglePipelineDesc();
    wrongEntry.fragment.entryPoint = "nope";
    auto r = dev.createGraphicsPipeline(wrongEntry);
    CHECK(r.errorCode() == ErrorCode::NotFound);
    CHECK(r.error().message.find("vsMain") != std::string::npos);  // lists what exists

    GraphicsPipelineDesc wrongStage = trianglePipelineDesc();
    wrongStage.vertex.entryPoint = "psMain";
    CHECK(dev.createGraphicsPipeline(wrongStage).errorCode() == ErrorCode::InvalidArgument);

    GraphicsPipelineDesc noTargets = trianglePipelineDesc();
    noTargets.colorCount = 0;
    CHECK(dev.createGraphicsPipeline(noTargets).errorCode() == ErrorCode::InvalidArgument);

    GraphicsPipelineDesc depthWithoutFormat = trianglePipelineDesc();
    depthWithoutFormat.depth.testEnable = true;
    CHECK(dev.createGraphicsPipeline(depthWithoutFormat).errorCode() == ErrorCode::InvalidArgument);

    const std::vector<u32> garbage = {1, 2, 3, 4, 5, 6};
    ComputePipelineDesc junk{ShaderDesc{garbage, "main"}, "junk"};
    CHECK(dev.createComputePipeline(junk).errorCode() == ErrorCode::InvalidArgument);

    ComputePipelineDesc cs{ShaderDesc{rhi_test_shaders::compute_buffers(), "csWriteAddress"}, "Fill"};
    const PipelineH p = dev.createComputePipeline(cs).value();
    CHECK(dev.isReady(p));
    CHECK(dev.memoryStats().pipelineCount == 2);
    dev.destroy(p);
    CHECK_FALSE(dev.isReady(p));
}

TEST_CASE("null: async pipeline compiles run on the compile pool") {
    jobs::BackgroundPool pool(2, "PSO");
    DeviceDesc desc;
    desc.pipelineCompilePool = &pool;
    NullFixture f(desc);
    Device& dev = *f.device;
    const PipelineH good = dev.createGraphicsPipeline(trianglePipelineDesc(), PsoPriority::Normal).value();
    GraphicsPipelineDesc badDesc = trianglePipelineDesc();
    badDesc.vertex.entryPoint = "missing";
    badDesc.name = "Broken";
    const PipelineH bad = dev.createGraphicsPipeline(badDesc, PsoPriority::Low).value();  // fails later
    pool.waitIdle();
    CHECK(dev.isReady(good));
    CHECK_FALSE(dev.isReady(bad));
    CHECK(f.takeError("async pipeline 'Broken' failed"));

    // Drawing with a pipeline that is not ready is skipped and counted, never fatal.
    const TextureH target = makeTarget(dev);
    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Frame");
    cmd->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    const ColorAttachment color{.texture = target};
    cmd->beginRendering({.colors = {&color, 1}});
    cmd->bindPipeline(bad);
    cmd->draw(3);
    cmd->endRendering();
    CHECK(dev.submit(Queue::Graphics, {&cmd, 1}).ok());
    CHECK(dev.memoryStats().psoMisses == 1);
    CHECK(f.null->trace().find("draw skipped (pipeline \"Broken\" not ready)") != std::string::npos);

    // Indirect dispatches count misses like direct ones (regression: they used to run anyway).
    ComputePipelineDesc brokenCompute{ShaderDesc{rhi_test_shaders::compute_buffers(), "missing"}, "BrokenCompute"};
    const PipelineH badCompute = dev.createComputePipeline(brokenCompute, PsoPriority::Normal).value();
    pool.waitIdle();
    CHECK(f.takeError("async pipeline 'BrokenCompute' failed"));
    const BufferH args = dev.createBuffer({.size = 64, .usage = BufferUsage::Indirect, .name = "Args"}).value();
    CommandList* compute = dev.acquireCommandList(Queue::AsyncCompute, "Compute");
    compute->bindPipeline(badCompute);
    compute->dispatchIndirect(args, 0);
    compute->dispatch(1);
    CHECK(dev.submit(Queue::AsyncCompute, {&compute, 1}).ok());
    CHECK(dev.memoryStats().psoMisses == 3);
    CHECK(f.null->trace().find("dispatchIndirect skipped (pipeline \"BrokenCompute\" not ready)") != std::string::npos);
    CHECK_NO_ERRORS(f);
}

TEST_CASE("null: command-stream trace golden for a small frame") {
    NullFixture f;
    Device& dev = *f.device;
    const TextureH color = makeTarget(dev, "Color", 256);
    const TextureH depth =
        dev.createTexture(TextureDesc::tex2D(Format::D32Float, 256, 256, TextureUsage::DepthStencil, "Depth")).value();
    const BufferH readback = dev.createBuffer({.size = 256 * 256 * 4,
                                               .usage = BufferUsage::TransferDst,
                                               .memory = MemoryUsage::Readback,
                                               .name = "Readback"})
                                 .value();
    GraphicsPipelineDesc pd = trianglePipelineDesc();
    pd.depthFormat = Format::D32Float;
    pd.depth = DepthState{true, true, CompareOp::GreaterOrEqual};
    const PipelineH pipeline = dev.createGraphicsPipeline(pd).value();

    auto recordFrame = [&](ResourceState colorBefore, ResourceState depthBefore) {
        CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Frame");
        {
            ScopedLabel label(*cmd, "Opaque");
            cmd->breadcrumb(1, BreadcrumbStage::Begin);
            const Barrier toTargets[] = {
                Barrier::textureState(color, colorBefore, ResourceState::RenderTarget),
                Barrier::textureState(depth, depthBefore, ResourceState::DepthWrite),
            };
            cmd->barrier(toTargets);
            const ColorAttachment ca{.texture = color, .clearColor = {0.0f, 0.0f, 0.25f, 1.0f}};
            RenderingDesc rd;
            rd.colors = {&ca, 1};
            rd.depth = DepthAttachment{.texture = depth, .store = StoreOp::DontCare};
            cmd->beginRendering(rd);
            cmd->bindPipeline(pipeline);
            const u32 push[2] = {7, 0x3f800000u};
            cmd->pushConstants(push, sizeof(push));
            cmd->draw(3);
            cmd->endRendering();
            cmd->barrier(Barrier::textureState(color, ResourceState::RenderTarget, ResourceState::CopySource));
            cmd->copyTextureToBuffer(color, {}, readback, {});
            cmd->breadcrumb(1, BreadcrumbStage::End);
        }
        return dev.submit(Queue::Graphics, {&cmd, 1}).value();
    };
    const TimelinePoint done = recordFrame(ResourceState::Undefined, ResourceState::Undefined);
    CHECK(done == TimelinePoint{Queue::Graphics, 1});
    CHECK_NO_ERRORS(f);

    const std::string expected =
        "submit Graphics #1 waits=[]\n"
        "  list \"Frame\"\n"
        "    beginLabel \"Opaque\"\n"
        "    breadcrumb pass=1 Begin\n"
        "    barrier texture \"Color\" Undefined->RenderTarget mips=0+1 layers=0+1\n"
        "    barrier texture \"Depth\" Undefined->DepthWrite mips=0+1 layers=0+1\n"
        "    beginRendering area=0,0 256x256 color0=\"Color\" mip=0 layer=0 load=Clear(0,0,0.25,1) store=Store "
        "depth=\"Depth\" load=Clear(0) store=DontCare\n"
        "    bindPipeline \"Triangle\"\n"
        "    pushConstants offset=0 size=8 data=070000000000803f\n"
        "    draw vertices=3 instances=1 firstVertex=0 firstInstance=0\n"
        "    endRendering\n"
        "    barrier texture \"Color\" RenderTarget->CopySource mips=0+1 layers=0+1\n"
        "    copyTextureToBuffer \"Color\" mip=0 layers=0+1 region=0,0,0 256x256x1 -> \"Readback\" offset=0 rowPitch=0\n"
        "    breadcrumb pass=1 End\n"
        "    endLabel\n";
    CHECK(f.null->trace() == expected);
    CHECK(f.null->textureState(color) == ResourceState::CopySource);
    CHECK(f.null->textureState(depth) == ResourceState::DepthWrite);
    CHECK(dev.breadcrumbs()[0].lastBegin == 1u);
    CHECK(dev.breadcrumbs()[0].lastEnd == 1u);

    // Rendering the same frame again gives the same trace (deterministic, names not handles); only
    // the timeline value in the submit header advances.
    f.null->clearTrace();
    CHECK(dev.beginFrame().ok());
    CHECK(dev.frameIndex() == 1);
    CHECK(recordFrame(ResourceState::CopySource, ResourceState::DepthWrite) == TimelinePoint{Queue::Graphics, 2});
    CHECK_NO_ERRORS(f);
    std::string again = expected;
    auto replaceText = [&](std::string_view from, std::string_view to) {
        const usize at = again.find(from);
        REQUIRE(at != std::string::npos);
        again.replace(at, from.size(), to);
    };
    replaceText("submit Graphics #1", "submit Graphics #2");
    replaceText("\"Color\" Undefined->RenderTarget", "\"Color\" CopySource->RenderTarget");
    replaceText("\"Depth\" Undefined->DepthWrite", "\"Depth\" DepthWrite->DepthWrite");
    CHECK(f.null->trace() == again);
}

TEST_CASE("null: validation catches API misuse at record time") {
    NullFixture f;
    Device& dev = *f.device;
    const TextureH target = makeTarget(dev);
    const PipelineH pipeline = dev.createGraphicsPipeline(trianglePipelineDesc()).value();
    const PipelineH compute =
        dev.createComputePipeline({ShaderDesc{rhi_test_shaders::compute_buffers(), "csWriteAddress"}, "cs"}).value();
    const BufferH noUsage = dev.createBuffer({.size = 64, .usage = BufferUsage::Storage, .name = "NoCopy"}).value();
    const BufferH dst = dev.createBuffer({.size = 64, .usage = BufferUsage::TransferDst}).value();
    const ColorAttachment ca{.texture = target};

    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Bad");
    cmd->draw(3);
    CHECK(f.takeError("draw outside beginRendering"));
    cmd->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    cmd->beginRendering({.colors = {&ca, 1}});
    cmd->draw(3);
    CHECK(f.takeError("without a bound graphics pipeline"));
    cmd->beginRendering({.colors = {&ca, 1}});
    CHECK(f.takeError("already rendering"));
    cmd->dispatch(1);
    CHECK(f.takeError("not allowed inside"));
    cmd->copyBuffer(noUsage, 0, dst, 0, 16);
    CHECK(f.takeError("not allowed inside"));
    cmd->bindPipeline(pipeline);
    const std::array<u8, 132> big{};
    cmd->pushConstants(big.data(), 132);
    CHECK(f.takeError("pushConstants"));
    cmd->pushConstants(big.data(), 6);
    CHECK(f.takeError("pushConstants"));
    cmd->endRendering();
    cmd->copyBuffer(noUsage, 0, dst, 0, 16);
    CHECK(f.takeError("lacks the required usage flag"));
    cmd->copyBuffer(dst, 0, dst, 60, 16);
    CHECK(f.takeError("exceeds buffer"));
    cmd->dispatch(1);
    CHECK(f.takeError("without a bound compute pipeline"));
    cmd->bindPipeline(compute);
    cmd->dispatch(1);
    CHECK_NO_ERRORS(f);
    cmd->endLabel();
    CHECK(f.takeError("endLabel without beginLabel"));
    cmd->beginLabel("open");
    cmd->end();
    CHECK(f.takeError("unbalanced beginLabel"));
    cmd->draw(1);
    CHECK(f.takeError("not recording"));

    // Queue capabilities.
    CommandList* copy = dev.acquireCommandList(Queue::Transfer, "Copy");
    copy->bindPipeline(compute);
    CHECK(f.takeError("not supported on the Transfer queue"));
    copy->beginRendering({.colors = {&ca, 1}});
    CHECK(f.takeError("not supported on the Transfer queue"));
    copy->end();
    CommandList* async = dev.acquireCommandList(Queue::AsyncCompute, "Async");
    async->bindPipeline(pipeline);
    CHECK(f.takeError("not supported on the AsyncCompute queue"));
    async->end();

    // Pipeline attachment formats must match the rendering.
    const TextureH hdr = dev.createTexture(TextureDesc::tex2D(Format::RGBA16Float, 64, 64, TextureUsage::ColorAttachment,
                                                              "Hdr"))
                             .value();
    CommandList* mismatch = dev.acquireCommandList(Queue::Graphics, "Mismatch");
    mismatch->barrier(Barrier::textureState(hdr, ResourceState::Undefined, ResourceState::RenderTarget));
    const ColorAttachment hdrAttachment{.texture = hdr};
    mismatch->beginRendering({.colors = {&hdrAttachment, 1}});
    mismatch->bindPipeline(pipeline);
    CHECK(f.takeError("do not match"));
    mismatch->endRendering();
    mismatch->end();

    CommandList* lists[] = {cmd, mismatch};
    CHECK(dev.submit(Queue::Graphics, lists).ok());
    CommandList* one[] = {copy};
    CHECK(dev.submit(Queue::Transfer, one).ok());
    CommandList* two[] = {async};
    CHECK(dev.submit(Queue::AsyncCompute, two).ok());
    CHECK(dev.submit(Queue::Graphics, lists).ok());
    CHECK(f.takeError("already submitted"));
    CHECK(dev.validationErrorCount() > 10);
}

TEST_CASE("null: resource states are validated in submission order") {
    NullFixture f;
    Device& dev = *f.device;
    const TextureH target = makeTarget(dev);
    const BufferH readback =
        dev.createBuffer({.size = 64 * 64 * 4, .usage = BufferUsage::TransferDst, .memory = MemoryUsage::Readback}).value();
    const ColorAttachment ca{.texture = target};

    // Rendering without transitioning the target first.
    CommandList* a = dev.acquireCommandList(Queue::Graphics, "NoBarrier");
    a->beginRendering({.colors = {&ca, 1}});
    a->endRendering();
    CHECK(dev.submit(Queue::Graphics, {&a, 1}).ok());
    CHECK(f.takeError("needs texture 'Color' (mip 0, layer 0) in RenderTarget but it is in Undefined"));

    // Lists recorded in any order validate against the order they are submitted in.
    CommandList* second = dev.acquireCommandList(Queue::Graphics, "Copy");
    second->barrier(Barrier::textureState(target, ResourceState::RenderTarget, ResourceState::CopySource));
    second->copyTextureToBuffer(target, {}, readback, {});
    CommandList* first = dev.acquireCommandList(Queue::Graphics, "Draw");
    first->barrier(Barrier::textureState(target, ResourceState::Undefined, ResourceState::RenderTarget));
    first->beginRendering({.colors = {&ca, 1}});
    first->endRendering();
    CommandList* ordered[] = {first, second};
    CHECK(dev.submit(Queue::Graphics, ordered).ok());
    CHECK_NO_ERRORS(f);
    CHECK(f.null->textureState(target) == ResourceState::CopySource);

    // A barrier whose 'before' does not match the tracked state.
    CommandList* wrong = dev.acquireCommandList(Queue::Graphics, "Wrong");
    wrong->barrier(Barrier::textureState(target, ResourceState::ShaderResource, ResourceState::RenderTarget));
    CHECK(dev.submit(Queue::Graphics, {&wrong, 1}).ok());
    CHECK(f.takeError("expects ShaderResource but it is in CopySource"));

    // Mip-level tracking: transitioning one mip leaves the others alone.
    const TextureH mips = dev.createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                                               TextureUsage::Sampled | TextureUsage::TransferDst, "Mips", 3))
                              .value();
    CommandList* m = dev.acquireCommandList(Queue::Graphics, "Mips");
    m->barrier(Barrier::textureState(mips, ResourceState::Undefined, ResourceState::CopyDest));
    m->barrier(Barrier::textureState(mips, ResourceState::CopyDest, ResourceState::ShaderResource, {1, 1, 0, 1}));
    CHECK(dev.submit(Queue::Graphics, {&m, 1}).ok());
    CHECK_NO_ERRORS(f);
    CHECK(f.null->textureState(mips, 0) == ResourceState::CopyDest);
    CHECK(f.null->textureState(mips, 1) == ResourceState::ShaderResource);
    CHECK(f.null->textureState(mips, 2) == ResourceState::CopyDest);
}

TEST_CASE("null: handle lifetimes are checked at submit") {
    NullFixture f;
    Device& dev = *f.device;
    const BufferH src = dev.createBuffer({.size = 64, .usage = BufferUsage::TransferSrc, .name = "Src"}).value();
    const BufferH dst = dev.createBuffer({.size = 64, .usage = BufferUsage::TransferDst, .name = "Dst"}).value();
    CommandList* cmd = dev.acquireCommandList(Queue::Transfer, "Copy");
    cmd->copyBuffer(src, 0, dst, 0, 64);
    dev.destroy(src);  // destroyed between recording and submission
    CHECK(dev.submit(Queue::Transfer, {&cmd, 1}).ok());
    CHECK(f.takeError("uses a buffer destroyed before submit"));

    CommandList* stale = dev.acquireCommandList(Queue::Transfer, "Stale");
    stale->copyBuffer(src, 0, dst, 0, 64);
    CHECK(f.takeError("null or stale buffer handle"));
    CHECK(dev.submit(Queue::Transfer, {&stale, 1}).ok());
}

TEST_CASE("null: timelines advance per queue and waits are traced and checked") {
    NullFixture f;
    Device& dev = *f.device;
    auto submitEmpty = [&](Queue q, std::span<const TimelinePoint> waits = {}) {
        CommandList* cmd = dev.acquireCommandList(q, "Empty");
        return dev.submit(q, {&cmd, 1}, waits).value();
    };
    const TimelinePoint g1 = submitEmpty(Queue::Graphics);
    const TimelinePoint c1 = submitEmpty(Queue::AsyncCompute);
    const TimelinePoint g2 = submitEmpty(Queue::Graphics);
    CHECK(g1.value == 1);
    CHECK(g2.value == 2);
    CHECK(c1 == TimelinePoint{Queue::AsyncCompute, 1});
    CHECK(dev.completedValue(Queue::Graphics) == 2);
    CHECK(dev.completedValue(Queue::Transfer) == 0);
    CHECK(dev.isComplete(g2));
    CHECK(dev.wait(g2).ok());
    CHECK(dev.wait(TimelinePoint{}).ok());

    const TimelinePoint waits[] = {c1, g2};
    const TimelinePoint t1 = submitEmpty(Queue::Transfer, waits);
    CHECK(t1.value == 1);
    CHECK(f.null->trace().find("submit Transfer #1 waits=[AsyncCompute:1,Graphics:2]") != std::string::npos);
    CHECK_NO_ERRORS(f);

    // Waiting for something that was never submitted would deadlock a real GPU.
    const TimelinePoint future[] = {TimelinePoint{Queue::AsyncCompute, 5}};
    submitEmpty(Queue::Graphics, future);
    CHECK(f.takeError("never submitted"));
    CHECK(dev.wait(TimelinePoint{Queue::Transfer, 9}).errorCode() == ErrorCode::Timeout);
    CHECK(f.takeError("never submitted"));
}

TEST_CASE("null: copies are emulated so upload/readback round-trips return data") {
    NullFixture f;
    Device& dev = *f.device;
    std::vector<u32> data(256);
    for (u32 i = 0; i < data.size(); ++i) data[i] = i * 2654435761u;
    const BufferH gpu =
        dev.createBuffer({.size = 1024, .usage = BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst})
            .value();
    auto up = uploadBuffer(dev, gpu, 0, bytesOf(data));
    REQUIRE(up.ok());
    auto back = readbackBuffer(dev, gpu, 0, kWholeSize, ResourceState::CopyDest, *up);
    REQUIRE(back.ok());
    REQUIRE(back->size() == 1024);
    CHECK(std::memcmp(back->data(), data.data(), 1024) == 0);

    // fill + update
    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Fill");
    // readbackBuffer restored the state it was given (CopyDest), so no transition is needed.
    CHECK(f.errorCount() == 0);
    cmd->fillBuffer(gpu, 0, 64, 0xDEADBEEFu);
    const u32 patch[2] = {1, 2};
    cmd->updateBuffer(gpu, 8, std::as_bytes(std::span<const u32>(patch)));
    const TimelinePoint done = dev.submit(Queue::Graphics, {&cmd, 1}).value();
    back = readbackBuffer(dev, gpu, 0, 16, ResourceState::CopyDest, done);
    REQUIRE(back.ok());
    u32 words[4];
    std::memcpy(words, back->data(), 16);
    CHECK(words[0] == 0xDEADBEEFu);
    CHECK(words[1] == 0xDEADBEEFu);
    CHECK(words[2] == 1u);
    CHECK(words[3] == 2u);

    // Texture sub-region upload and full readback (mip 1 of a 2-layer array).
    TextureDesc td = TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                        TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc,
                                        "Tex", 2);
    td.arrayLayers = 2;
    const TextureH tex = dev.createTexture(td).value();
    std::vector<u8> texels(4 * 4 * 4);
    for (usize i = 0; i < texels.size(); ++i) texels[i] = static_cast<u8>(i + 1);
    TextureRegion region{.mip = 1, .baseLayer = 1, .x = 2, .y = 4, .width = 4, .height = 4};
    REQUIRE(uploadTexture(dev, tex, std::as_bytes(std::span<const u8>(texels)), region).ok());
    CHECK(f.null->textureState(tex, 1, 1) == ResourceState::ShaderResource);
    auto image = readbackTexture(dev, tex, ResourceState::ShaderResource, 1, 1);
    REQUIRE(image.ok());
    REQUIRE(image->size() == 8 * 8 * 4);
    // Row y=5 (second row of the region), x=3 (second texel of the region).
    const u8* px = image->data() + (5 * 8 + 3) * 4;
    const u8* src = texels.data() + (1 * 4 + 1) * 4;
    CHECK(std::memcmp(px, src, 4) == 0);
    CHECK(image->at(0) == 0);  // untouched texels stay zero
    CHECK_NO_ERRORS(f);

    // Size mismatch is rejected before recording anything.
    CHECK(uploadTexture(dev, tex, std::as_bytes(std::span<const u8>(texels.data(), 10))).errorCode() ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("null: parallel recording on the job system, execution in submission order") {
    NullFixture f;
    Device& dev = *f.device;
    constexpr u32 kLists = 16;
    std::vector<BufferH> buffers;
    for (u32 i = 0; i < kLists; ++i) {
        const std::string name = "B" + std::to_string(i);
        buffers.push_back(dev.createBuffer({.size = 64, .usage = BufferUsage::TransferDst, .name = name}).value());
    }
    std::vector<CommandList*> lists(kLists, nullptr);
    {
        jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 4});
        jobs::Counter counter;
        for (u32 i = 0; i < kLists; ++i) {
            js.run(
                [&, i] {
                    // Acquire on the recording thread (per-thread pools), record, close.
                    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Job" + std::to_string(i));
                    cmd->fillBuffer(buffers[i], 0, 64, i);
                    cmd->end();
                    lists[i] = cmd;
                },
                &counter);
        }
        js.wait(counter);
    }
    REQUIRE(dev.submit(Queue::Graphics, lists).ok());
    CHECK_NO_ERRORS(f);
    const std::string trace = f.null->trace();
    usize pos = 0;
    for (u32 i = 0; i < kLists; ++i) {
        const std::string needle = "list \"Job" + std::to_string(i) + "\"";
        const usize at = trace.find(needle, pos);
        REQUIRE_MESSAGE(at != std::string::npos, needle);
        pos = at;
    }
    auto back = readbackBuffer(dev, buffers[9]);
    REQUIRE(back.ok());
    u32 v = 0;
    std::memcpy(&v, back->data(), 4);
    CHECK(v == 9u);
}

TEST_CASE("null: swapchain acquire/present loop validates the Present state") {
    NullFixture f;
    Device& dev = *f.device;
    const SwapchainH sc =
        dev.createSwapchain({.width = 320, .height = 200, .format = Format::BGRA8Srgb, .imageCount = 2, .name = "Main"})
            .value();
    CHECK(dev.swapchainInfo(sc).width == 320);
    CHECK(dev.swapchainInfo(sc).imageCount == 2);
    std::set<u64> seen;
    for (int frame = 0; frame < 4; ++frame) {
        REQUIRE(dev.beginFrame().ok());
        const SwapchainImage image = dev.acquireNextImage(sc).value();
        REQUIRE(image.status == SwapchainStatus::Ok);
        seen.insert(image.texture.toBits());
        CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Frame");
        cmd->barrier(Barrier::textureState(image.texture, ResourceState::Undefined, ResourceState::RenderTarget));
        const ColorAttachment ca{.texture = image.texture};
        cmd->beginRendering({.colors = {&ca, 1}});
        cmd->endRendering();
        cmd->barrier(Barrier::textureState(image.texture, ResourceState::RenderTarget, ResourceState::Present));
        const TimelinePoint waits[] = {image.ready};
        const TimelinePoint done = dev.submit(Queue::Graphics, {&cmd, 1}, waits).value();
        CHECK(dev.present(sc, done).value() == SwapchainStatus::Ok);
    }
    CHECK(seen.size() == 2);
    CHECK_NO_ERRORS(f);

    // Presenting an image that is still a render target is reported.
    const SwapchainImage image = dev.acquireNextImage(sc).value();
    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Forgot");
    cmd->barrier(Barrier::textureState(image.texture, ResourceState::Undefined, ResourceState::RenderTarget));
    const TimelinePoint done = dev.submit(Queue::Graphics, {&cmd, 1}).value();
    CHECK(dev.present(sc, done).ok());
    CHECK(f.takeError("expected Present"));

    // Swapchain images cannot be destroyed directly; resize recreates them.
    dev.destroy(image.texture);
    CHECK(f.takeError("swapchain image"));
    REQUIRE(dev.resizeSwapchain(sc, 640, 480).ok());
    CHECK(dev.swapchainInfo(sc).width == 640);
    CHECK(dev.textureDesc(image.texture).width == 0);  // old images are gone
    CHECK(f.null->trace().find("resize \"Main\" 640x480") != std::string::npos);
    dev.destroy(sc);
    CHECK(dev.swapchainInfo(sc).width == 0);
}

TEST_CASE("null: beginFrame recycles lists and reports lists that were never submitted") {
    NullFixture f;
    Device& dev = *f.device;
    CommandList* a = dev.acquireCommandList(Queue::Graphics, "A");
    CHECK(dev.submit(Queue::Graphics, {&a, 1}).ok());
    REQUIRE(dev.beginFrame().ok());
    CommandList* b = dev.acquireCommandList(Queue::Graphics, "B");
    CHECK(b == a);  // recycled
    (void)dev.acquireCommandList(Queue::Graphics, "Orphan");
    CHECK(dev.submit(Queue::Graphics, {&b, 1}).ok());
    REQUIRE(dev.beginFrame().ok());
    CHECK(f.takeError("'Orphan' was acquired but never submitted"));
    CHECK(dev.frameIndex() == 2);
}

TEST_CASE("null: onMessage receives validation errors from any thread") {
    std::atomic<int> messages{0};
    DeviceDesc desc;
    desc.onMessage = [&](const ValidationMessage& m) {
        if (m.severity == ValidationMessage::Severity::Error) messages.fetch_add(1);
    };
    NullFixture f(desc);
    std::thread t([&] {
        CommandList* cmd = f.device->acquireCommandList(Queue::Graphics, "T");
        cmd->draw(1);
        cmd->end();
    });
    t.join();
    CHECK(messages.load() == 1);
    CHECK(f.device->validationErrorCount() == 1);
}

TEST_CASE("null: injected device loss reports breadcrumbs and fails later calls") {
    std::vector<DeviceLostInfo> lost;
    DeviceDesc desc;
    desc.debugDeviceLostAfterSubmits = 2;
    desc.onDeviceLost = [&](const DeviceLostInfo& info) { lost.push_back(info); };
    NullFixture f(desc);
    Device& dev = *f.device;
    rhitest::QuietRhiLog quiet;
    auto pass = [&](u16 id) {
        CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Pass");
        REQUIRE(cmd != nullptr);
        cmd->breadcrumb(id, BreadcrumbStage::Begin);
        cmd->breadcrumb(id, BreadcrumbStage::End);
        return dev.submit(Queue::Graphics, {&cmd, 1});
    };
    CHECK(pass(10).ok());
    CHECK_FALSE(dev.isDeviceLost());
    auto second = pass(11);
    CHECK(second.errorCode() == ErrorCode::InvalidState);
    CHECK(dev.isDeviceLost());
    REQUIRE(lost.size() == 1);
    CHECK(lost[0].reason.find("injected") != std::string::npos);
    CHECK(lost[0].breadcrumbs[0].lastBegin == 11u);  // names the pass that was running
    CHECK(lost[0].submittedValues[0] == 2u);
    // Everything afterwards fails cleanly; the callback fires only once.
    CHECK(dev.acquireCommandList(Queue::Graphics, "After") == nullptr);
    CHECK(dev.createBuffer({.size = 16}).errorCode() == ErrorCode::InvalidState);
    CHECK(dev.beginFrame().errorCode() == ErrorCode::InvalidState);
    CHECK(uploadBuffer(dev, BufferH{}, 0, std::as_bytes(std::span<const u32>(std::vector<u32>{1}))).errorCode() ==
          ErrorCode::InvalidState);
    CHECK(lost.size() == 1);
}

TEST_CASE("null: stale, recycled and foreign command lists are rejected at submit") {
    NullFixture f;
    Device& dev = *f.device;
    NullFixture other;
    CommandList* foreign = other.device->acquireCommandList(Queue::Graphics, "Foreign");
    CHECK(dev.submit(Queue::Graphics, {&foreign, 1}).ok());  // reported and skipped
    CHECK(f.takeError("not acquired from this device"));
    CHECK(other.device->submit(Queue::Graphics, {&foreign, 1}).ok());

    const BufferH buffer = dev.createBuffer({.size = 64, .usage = BufferUsage::TransferDst, .name = "B"}).value();
    CommandList* stale = dev.acquireCommandList(Queue::Graphics, "Stale");
    stale->fillBuffer(buffer, 0, 64, 1u);
    REQUIRE(dev.beginFrame().ok());
    CHECK(f.takeError("'Stale' was acquired but never submitted"));
    CHECK(dev.submit(Queue::Graphics, {&stale, 1}).ok());  // reported and skipped, like other bad lists
    CHECK(f.takeError("recycled by beginFrame/waitIdle before it was submitted"));
    CHECK(f.null->trace().find("list \"Stale\"") == std::string::npos);

    CommandList* recycled = dev.acquireCommandList(Queue::Graphics, "Recycled");
    REQUIRE(dev.waitIdle().ok());
    CHECK(f.takeError("'Recycled' was acquired but never submitted"));
    recycled->fillBuffer(buffer, 0, 64, 2u);
    CHECK(f.takeError("not recording"));
    CHECK_NO_ERRORS(f);
}

TEST_CASE("null: buffer/texture copies are validated like the Vulkan backend") {
    NullFixture f;
    Device& dev = *f.device;
    const BufferH buffer = dev.createBuffer({.size = 4096,
                                             .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
                                             .name = "Staging"})
                               .value();
    const TextureH tex = dev.createTexture(TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                                              TextureUsage::TransferDst | TextureUsage::TransferSrc, "Tex"))
                             .value();
    TextureDesc msaaDesc = TextureDesc::tex2D(Format::RGBA8Unorm, 16, 16,
                                              TextureUsage::ColorAttachment | TextureUsage::TransferSrc, "Msaa");
    msaaDesc.sampleCount = 4;
    const TextureH msaa = dev.createTexture(msaaDesc).value();
    const TextureH packed = dev.createTexture(TextureDesc::tex2D(Format::D32FloatS8Uint, 8, 8,
                                                                 TextureUsage::DepthStencil | TextureUsage::TransferSrc,
                                                                 "Packed"))
                                .value();
    CommandList* cmd = dev.acquireCommandList(Queue::Graphics, "Copies");
    cmd->copyBufferToTexture(buffer, {}, tex, TextureRegion{.x = 8, .width = 16});
    CHECK(f.takeError("exceeds mip 0"));
    cmd->copyBufferToTexture(buffer, {}, tex, TextureRegion{.x = 16});
    CHECK(f.takeError("outside mip 0"));
    cmd->copyBufferToTexture(buffer, {.offset = 2}, tex, {});
    CHECK(f.takeError("not a multiple of 4"));
    cmd->copyBufferToTexture(buffer, {.rowPitch = 66}, tex, {});
    CHECK(f.takeError("rowPitch 66 invalid"));
    cmd->copyBufferToTexture(buffer, {.offset = 3076}, tex, {});
    CHECK(f.takeError("exceeds buffer"));
    cmd->copyTextureToBuffer(msaa, {}, buffer, {});
    CHECK(f.takeError("multisampled"));
    cmd->copyTextureToBuffer(packed, {}, buffer, {});
    CHECK(f.takeError("D32FloatS8Uint"));
    cmd->copyBufferToTexture(buffer, {.offset = 1024, .rowPitch = 128}, tex, TextureRegion{.x = 4, .y = 4, .width = 8,
                                                                                            .height = 8});
    CHECK_NO_ERRORS(f);
    cmd->end();
    CHECK(dev.submit(Queue::Graphics, {&cmd, 1}).ok());

    // Creation-time checks shared with Vulkan.
    TextureDesc volume = TextureDesc::tex2D(Format::RGBA8Unorm, 8, 8, TextureUsage::ColorAttachment);
    volume.type = TextureType::Tex3D;
    volume.depth = 4;
    CHECK(dev.createTexture(volume).errorCode() == ErrorCode::Unsupported);
    TextureDesc tooManySamples = TextureDesc::tex2D(Format::RGBA8Unorm, 8, 8, TextureUsage::ColorAttachment);
    tooManySamples.sampleCount = 128;
    CHECK(dev.createTexture(tooManySamples).errorCode() == ErrorCode::InvalidArgument);
    // Read-only depth cannot be cleared.
    const TextureH depth =
        dev.createTexture(TextureDesc::tex2D(Format::D32Float, 16, 16, TextureUsage::DepthStencil, "Depth")).value();
    CommandList* ro = dev.acquireCommandList(Queue::Graphics, "ReadOnlyDepth");
    ro->barrier(Barrier::textureState(depth, ResourceState::Undefined, ResourceState::DepthRead));
    RenderingDesc rd;
    rd.depth = DepthAttachment{.texture = depth, .load = LoadOp::Clear, .readOnly = true};
    ro->beginRendering(rd);
    CHECK(f.takeError("read-only depth attachment 'Depth' with LoadOp::Clear"));
    ro->endRendering();
    rd.depth.load = LoadOp::Load;
    ro->beginRendering(rd);
    ro->endRendering();
    ro->end();
    CHECK(dev.submit(Queue::Graphics, {&ro, 1}).ok());
    CHECK_NO_ERRORS(f);
}
