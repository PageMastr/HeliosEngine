// Render graph execution on the Null backend: trace golden, parallel vs serial recording, the
// resource pool across frames (states, cross-frame waits, trimming), swapchain import, the
// execute-time prologue, external waits and RgContext validation.

#include <doctest/doctest.h>

#include <array>

#include "golden_util.h"
#include "graph_test_util.h"
#include "helios/core/jobs.h"
#include "helios/rhi/utils.h"

using namespace graphtest;
using rhi::Format;
using rhi::ResourceState;

namespace {

/// Clear -> Geometry (color Load + depth) -> Post (async compute) -> Composite -> Readback, plus a
/// culled debug pass. Records only clears/copies/fills (the Null backend needs real SPIR-V for
/// pipelines), which is enough for its state validation.
struct SmallFrame {
    RgTexture color, depth, out;
    RgBuffer stats, readback;
};

void buildSmallFrame(RenderGraph& graph, rhi::TextureH outTexture, const rhi::TextureDesc& outDesc,
                     rhi::BufferH readbackBuffer, const rhi::BufferDesc& readbackDesc) {
    auto f = std::make_shared<SmallFrame>();
    f->out = graph.importTexture("Out", outTexture, outDesc, {.finalState = ResourceState::CopySource});
    f->readback = graph.importBuffer("Readback", readbackBuffer, readbackDesc);
    graph.addPass("Clear", PassFlags::Copy,
                  [f](RgBuilder& b) {
                      f->color = b.write(b.create("SceneColor", RgTextureDesc::tex2D(Format::RGBA16Float, 64, 64)),
                                         TextureWrite::CopyDest);
                  },
                  [f](RgContext& ctx) { ctx.cmd().clearTexture(ctx.texture(f->color), {0.1f, 0.2f, 0.3f, 1.0f}); });
    graph.addPass("Geometry", PassFlags::Raster,
                  [f](RgBuilder& b) {
                      f->color = b.colorAttachment(f->color, 0, rhi::LoadOp::Load);
                      f->depth = b.depthAttachment(b.create("SceneDepth", RgTextureDesc::tex2D(Format::D32Float, 64, 64)));
                  },
                  {});
    graph.addPass("Debug", PassFlags::Compute,
                  [f](RgBuilder& b) {
                      b.read(f->color);
                      b.write(b.create("DebugView", RgTextureDesc::tex2D(Format::RGBA8Unorm, 64, 64)));
                  },
                  {});
    graph.addPass("Stats", PassFlags::AsyncCompute,
                  [f](RgBuilder& b) {
                      b.read(f->color);
                      f->stats = b.write(b.create("Stats", RgBufferDesc{256}), BufferWrite::CopyDest);
                  },
                  [f](RgContext& ctx) { ctx.cmd().fillBuffer(ctx.buffer(f->stats), 0, rhi::kWholeSize, 7u); });
    graph.addPass("Composite", PassFlags::Copy,
                  [f](RgBuilder& b) {
                      b.read(f->color, TextureRead::CopySource);
                      b.read(f->stats, BufferRead::CopySource);
                      f->out = b.write(f->out, TextureWrite::CopyDest);
                  },
                  [f](RgContext& ctx) { ctx.cmd().clearTexture(ctx.texture(f->out), {1.0f, 0.5f, 0.25f, 1.0f}); });
    graph.addPass("Readback", PassFlags::Copy,
                  [f](RgBuilder& b) {
                      b.read(f->stats, BufferRead::CopySource);
                      f->readback = b.write(f->readback, BufferWrite::CopyDest);
                  },
                  [f](RgContext& ctx) {
                      ctx.cmd().copyBuffer(ctx.buffer(f->stats), 0, ctx.buffer(f->readback), 0, 256);
                  });
}

struct SmallFrameFixture : NullDeviceFixture {
    rhi::TextureH out;
    rhi::BufferH readback;
    SmallFrameFixture() {
        out = texture("Out", Format::RGBA8Unorm, 64, 64, kAllColorUsage);
        readback = device
                       ->createBuffer({.size = 256, .usage = rhi::BufferUsage::TransferDst,
                                       .memory = rhi::MemoryUsage::Readback, .name = "Readback"})
                       .value();
    }
    RenderGraph build(const char* name = "Small") {
        RenderGraph graph(name);
        buildSmallFrame(graph, out, device->textureDesc(out), readback, device->bufferDesc(readback));
        return graph;
    }
};

} // namespace

TEST_CASE("graph null: small frame trace golden (plan + command stream)") {
    SmallFrameFixture fx;
    RgResourcePool pool(*fx.device);
    RenderGraph graph = fx.build();
    REQUIRE(graph.compile({.maxCommandLists = 4}).ok());
    auto result = graph.execute(*fx.device, pool);
    REQUIRE(result.ok());
    CHECK_NULL_CLEAN(fx);
    CHECK(result->submitCount == graph.executedPlan().batches.size());
    CHECK(result->lastPoints[0].value >= 1);
    CHECK(fx.null->textureState(fx.out) == ResourceState::CopySource);
    // The async fill really reached the readback buffer through the graph's copies.
    auto bytes = rhi::readbackBuffer(*fx.device, fx.readback, 0, 4, ResourceState::CopyDest, result->graphics());
    REQUIRE(bytes.ok());
    CHECK(bytes.value()[0] == 7);
    goldentest::checkTextGolden("small_frame", rgDumpPlan(graph.executedPlan()) + "--- Null trace ---\n" + fx.null->trace());
}

TEST_CASE("graph null: parallel recording gives the same command stream as serial recording") {
    std::string traces[2];
    for (int parallel = 0; parallel < 2; ++parallel) {
        SmallFrameFixture fx;
        RgResourcePool pool(*fx.device);
        jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 3});
        RenderGraph graph = fx.build();
        REQUIRE(graph.compile({.maxCommandLists = 12}).ok());
        CHECK(graph.plan().lists.size() >= 4);
        for (int frame = 0; frame < 3; ++frame) {
            REQUIRE(fx.device->beginFrame().ok());
            REQUIRE(graph.execute(*fx.device, pool, {.jobs = parallel ? &js : nullptr}).ok());
        }
        CHECK_NULL_CLEAN(fx);
        traces[parallel] = fx.null->trace();
    }
    CHECK(traces[0] == traces[1]);
}

TEST_CASE("graph null: per-mip bindless views of transients get their slots at creation, not in recording order") {
    // Mips are written in reverse order on alternating queues; lazily allocated, the slots would
    // follow the (job-dependent) recording order. Allocated with the texture, they follow the mips.
    constexpr u32 kMips = 4;
    using Slots = std::array<rhi::BindlessIndex, kMips>;
    for (int parallel = 0; parallel < 2; ++parallel) {
        INFO("parallel=" << parallel);
        NullDeviceFixture fx;
        RgResourcePool pool(*fx.device);
        jobs::JobSystem js(jobs::JobSystemDesc{.workerCount = 3});
        auto uavs = std::make_shared<Slots>();
        auto srvs = std::make_shared<Slots>();
        RenderGraph graph("Views");
        struct Data {
            RgTexture chain;
        };
        RgTexture chain = graph.addPass<Data>(
                                   "Create", PassFlags::Compute,
                                   [](RgBuilder& b, Data& d) {
                                       d.chain = b.create("Chain", RgTextureDesc::tex2D(Format::RGBA16Float, 32, 32, kMips));
                                   },
                                   [](const Data&, RgContext&) {})
                              .chain;
        for (u32 i = 0; i < kMips; ++i) {
            const u32 mip = kMips - 1 - i;
            chain = graph
                        .addPass<Data>(
                            std::format("Mip{}", mip), i % 2 ? PassFlags::AsyncCompute : PassFlags::Compute,
                            [&](RgBuilder& b, Data& d) { d.chain = b.write(chain, TextureWrite::Storage, {mip, 1, 0, 1}); },
                            [uavs, srvs, mip](const Data& d, RgContext& ctx) {
                                (*uavs)[mip] = ctx.uav(d.chain, mip);
                                (*srvs)[mip] = ctx.srv(d.chain, {.baseMip = mip, .mipCount = 1});
                            })
                        .chain;
        }
        graph.addPass("Sample", PassFlags::Compute | PassFlags::NeverCull,
                      [&](RgBuilder& b) { b.read(chain, TextureRead::Sampled); }, [](RgContext&) {});
        REQUIRE(graph.compile().ok());
        REQUIRE(graph.execute(*fx.device, pool, {.jobs = parallel ? &js : nullptr}).ok());
        CHECK_NULL_CLEAN(fx);
        for (u32 m = 0; m < kMips; ++m) {
            CHECK((*uavs)[m] != rhi::kInvalidBindless);
            CHECK((*srvs)[m] != rhi::kInvalidBindless);
            if (m > 0) {
                CHECK((*uavs)[m - 1] < (*uavs)[m]);
                CHECK((*srvs)[m - 1] < (*srvs)[m]);
            }
        }
    }
}

TEST_CASE("graph null: the pool carries states and last uses across frames and trims unused resources") {
    SmallFrameFixture fx;
    RgResourcePool pool(*fx.device, 2);
    for (int frame = 0; frame < 2; ++frame) {
        CAPTURE(frame);
        REQUIRE(fx.device->beginFrame().ok());
        fx.null->clearTrace();
        RenderGraph graph = fx.build();
        REQUIRE(graph.compile().ok());
        REQUIRE(graph.execute(*fx.device, pool).ok());
        CHECK_NULL_CLEAN(fx);
        CHECK(pool.textureCount() == 2);  // SceneColor, SceneDepth (DebugView is culled)
        CHECK(pool.bufferCount() == 1);
        const RgPlan& executed = graph.executedPlan();
        const std::string clear = pre(executed, "Clear");
        const std::string trace = fx.null->trace();
        if (frame == 0) {
            CHECK(clear == "SceneColor:Undefined->CopyDest");
        } else {
            // Second frame: entry barriers start from the states frame 0 left in the pool.
            CHECK(clear == "SceneColor:CopySource->CopyDest");
            CHECK(trace.find("barrier buffer \"Stats\" CopySource->CopyDest") != std::string::npos);
        }
    }
    CHECK(pool.bytes() == rgTextureBytes(RgTextureDesc::tex2D(Format::RGBA16Float, 64, 64)) +
                              rgTextureBytes(RgTextureDesc::tex2D(Format::D32Float, 64, 64)) + 256);
    // Two completed frames without these resources trim them (keepFrames = 2): the third frame's
    // execute destroys them (the frame in progress never counts, another graph may still use them).
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(fx.device->beginFrame().ok());
        RenderGraph empty("Empty");
        RgBuffer b;
        empty.addPass("Fill", PassFlags::Copy | PassFlags::NeverCull,
                      [&](RgBuilder& builder) { b = builder.write(builder.create("Tiny", RgBufferDesc{64}), BufferWrite::CopyDest); },
                      {});
        REQUIRE(empty.compile().ok());
        REQUIRE(empty.execute(*fx.device, pool).ok());
    }
    CHECK(pool.textureCount() == 0);
    CHECK(pool.bufferCount() == 1);
    pool.clear();
    CHECK(pool.bufferCount() == 0);
    CHECK_NULL_CLEAN(fx);
}

TEST_CASE("graph null: cross-frame waits for pooled resources last used on another queue") {
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device);
    auto build = [&](bool asyncFirst) {
        RenderGraph graph("CrossFrame");
        struct Data {
            RgBuffer buf;
        };
        const PassFlags writer = asyncFirst ? PassFlags::AsyncCompute : PassFlags::Compute;
        // Pass data lives in the graph: the graph outlives this lambda's locals.
        graph.addPass<Data>(
            "Write", writer | PassFlags::NeverCull,
            [&](RgBuilder& b, Data& d) { d.buf = b.write(b.create("Shared", RgBufferDesc{128}), BufferWrite::CopyDest); },
            [](const Data& d, RgContext& ctx) { ctx.cmd().fillBuffer(ctx.buffer(d.buf), 0, rhi::kWholeSize, 1u); });
        return graph;
    };
    {
        RenderGraph frame0 = build(true);
        REQUIRE(frame0.compile().ok());
        REQUIRE(frame0.execute(*fx.device, pool).ok());
    }
    fx.null->clearTrace();
    REQUIRE(fx.device->beginFrame().ok());
    {
        RenderGraph frame1 = build(false);
        REQUIRE(frame1.compile().ok());
        REQUIRE(frame1.execute(*fx.device, pool).ok());
    }
    CHECK_NULL_CLEAN(fx);
    // Frame 1 uses the pooled buffer on graphics after frame 0 used it on async compute.
    CHECK(fx.null->trace().starts_with("submit Graphics #1 waits=[AsyncCompute:1]\n"));
}

TEST_CASE("graph null: swapchain images are imported with their ready point and end in Present") {
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device);
    const rhi::SwapchainH sc =
        fx.device->createSwapchain({.width = 64, .height = 64, .format = Format::BGRA8Srgb, .imageCount = 2, .name = "Main"})
            .value();
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(fx.device->beginFrame().ok());
        const rhi::SwapchainImage image = fx.device->acquireNextImage(sc).value();
        RenderGraph graph("Present");
        RgTexture back = graph.importTexture("Backbuffer", image.texture, fx.device->textureDesc(image.texture),
                                             {.finalState = ResourceState::Present, .waitFor = image.ready});
        graph.addPass("Draw", PassFlags::Raster,
                      [&](RgBuilder& b) {
                          back = b.colorAttachment(back, 0, rhi::LoadOp::Clear, {0.0f, 0.0f, 1.0f, 1.0f});
                      },
                      {});
        REQUIRE(graph.compile().ok());
        auto result = graph.execute(*fx.device, pool);
        REQUIRE(result.ok());
        CHECK(post(graph.executedPlan(), "Draw") == "Backbuffer:RenderTarget->Present");
        REQUIRE(fx.device->present(sc, result->graphics()).ok());
    }
    CHECK_NULL_CLEAN(fx);
    CHECK(fx.null->trace().find("color0=\"Main[") != std::string::npos);
}

TEST_CASE("graph null: every queue that touches an import waits for its ready point") {
    // Review regression: only the first submission touching an import waited for RgImport::waitFor,
    // so an async-compute batch reading the same data (no dependency on that graphics batch) could
    // run before the producer (upload, previous frame, swapchain acquire) finished.
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device);
    const rhi::BufferH data = fx.buffer("Streamed", 256, rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst);
    rhi::CommandList* up = fx.device->acquireCommandList(rhi::Queue::Transfer, "Upload");
    up->barrier(rhi::Barrier::bufferState(data, ResourceState::Undefined, ResourceState::ShaderResource));
    const rhi::TimelinePoint uploaded = fx.device->submit(rhi::Queue::Transfer, {&up, 1}).value();
    fx.null->clearTrace();

    RenderGraph graph("ImportWait");
    const RgBuffer in = graph.importBuffer("Streamed", data, fx.device->bufferDesc(data),
                                           {.initialState = ResourceState::ShaderResource, .waitFor = uploaded});
    graph.addPass("GraphicsRead", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(in); }, {});
    graph.addPass("AsyncRead", PassFlags::AsyncCompute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(in); }, {});
    REQUIRE(graph.compile().ok());
    CHECK(batchesText(graph.plan()) == "G[GraphicsRead] A[AsyncRead]");
    REQUIRE(graph.execute(*fx.device, pool).ok());
    CHECK_NULL_CLEAN(fx);
    const std::string trace = fx.null->trace();
    INFO(trace);
    CHECK(trace.find("submit Graphics #1 waits=[Transfer:1]") != std::string::npos);
    CHECK(trace.find("submit AsyncCompute #1 waits=[Transfer:1]") != std::string::npos);
}

TEST_CASE("graph null: sub-views of imported textures get their slots before recording, in plan order") {
    // Views of imported textures that passes declare (uav(mip) of Storage accesses, one-mip SRVs of
    // mipmapped Sampled reads) are created in plan order on the calling thread, like transients', so
    // bindless slot numbers never depend on how the recording jobs interleave. The callbacks below
    // ask for them in reverse mip order: lazily allocated slots would come out reversed.
    struct Slots {
        std::array<rhi::BindlessIndex, 4> srv{};
        std::array<rhi::BindlessIndex, 4> uav{};
    };
    auto run = [](jobs::JobSystem* jobSystem, std::vector<Slots>& seen) {
        NullDeviceFixture fx;
        RgResourcePool pool(*fx.device);
        const rhi::TextureH chain = fx.texture("Chain", Format::RGBA16Float, 64, 64, kAllColorUsage, 4);
        const rhi::TextureH target = fx.texture("Target", Format::RGBA16Float, 64, 64, kAllColorUsage, 4);
        RenderGraph graph("ImportedViews");
        const RgTexture src = graph.importTexture("Chain", chain, fx.device->textureDesc(chain),
                                                  {.initialState = ResourceState::ShaderResource});
        RgTexture dst = graph.importTexture("Target", target, fx.device->textureDesc(target));
        seen.assign(4, Slots{});
        for (u32 p = 0; p < 4; ++p) {
            Slots* out = &seen[p];
            graph.addPass(
                std::format("P{}", p), PassFlags::Compute,
                [&](RgBuilder& b) {
                    b.read(src, TextureRead::Sampled);
                    dst = b.readWrite(dst, TextureWrite::Storage);
                },
                [src, out, dstId = dst.id](RgContext& ctx) {
                    for (u32 m = 4; m-- > 0;) {
                        out->srv[m] = ctx.srv(src, {.baseMip = m, .mipCount = 1});
                        out->uav[m] = ctx.uav(RgTexture{dstId, 0}, m);
                        ctx.cmd().insertLabel(std::format("mip{} srv={} uav={}", m, out->srv[m], out->uav[m]));
                    }
                });
        }
        REQUIRE(graph.compile({.maxCommandLists = 8}).ok());
        REQUIRE(graph.execute(*fx.device, pool, {.jobs = jobSystem}).ok());
        CHECK_NULL_CLEAN(fx);
        CHECK(graph.contextErrorCount() == 0);
        return fx.null->trace();
    };
    std::vector<Slots> slots;
    const std::string serial = run(nullptr, slots);
    for (const Slots& s : slots) {
        for (u32 m = 0; m + 1 < 4; ++m) {
            CAPTURE(m);
            CHECK(s.srv[m] < s.srv[m + 1]);
            CHECK(s.uav[m] < s.uav[m + 1]);
            CHECK(s.srv[m] == slots[0].srv[m]);
            CHECK(s.uav[m] == slots[0].uav[m]);
        }
    }
    jobs::JobSystem jobSystem(jobs::JobSystemDesc{.workerCount = 3});
    for (int i = 0; i < 10; ++i) {
        CAPTURE(i);
        std::vector<Slots> parallel;
        CHECK(run(&jobSystem, parallel) == serial);
    }
}

TEST_CASE("graph null: pool trimming counts frames, not executions") {
    // Two graphs per frame share one pool, each using its own transient. Counting executions, a
    // resource used once per frame looked unused after the other graph ran and was destroyed and
    // recreated every frame.
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device, 1);
    auto graphUsing = [](const char* name, u64 size) {
        RenderGraph graph(name);
        graph.addPass("Fill", PassFlags::Copy | PassFlags::NeverCull,
                      [size](RgBuilder& b) { b.write(b.create("T", RgBufferDesc{size}), BufferWrite::CopyDest); }, {});
        return graph;
    };
    std::string firstTrace;
    for (int frame = 0; frame < 4; ++frame) {
        CAPTURE(frame);
        REQUIRE(fx.device->beginFrame().ok());
        for (auto [name, size] : {std::pair<const char*, u64>{"A", 256}, std::pair<const char*, u64>{"B", 512}}) {
            RenderGraph graph = graphUsing(name, size);
            REQUIRE(graph.compile().ok());
            REQUIRE(graph.execute(*fx.device, pool).ok());
            CHECK(pool.bufferCount() == (frame == 0 && std::string_view(name) == "A" ? 1u : 2u));
        }
    }
    // The pooled buffers were created once: frame 3's entry barriers start from frame 2's states.
    CHECK(fx.null->trace().find("barrier buffer \"T\" CopyDest->CopyDest") != std::string::npos);
    // A frame without them trims both (keepFrames = 1).
    REQUIRE(fx.device->beginFrame().ok());
    REQUIRE(fx.device->beginFrame().ok());
    {
        RenderGraph other = graphUsing("C", 1024);
        REQUIRE(other.compile().ok());
        REQUIRE(other.execute(*fx.device, pool).ok());
    }
    CHECK(pool.bufferCount() == 1);
    CHECK_NULL_CLEAN(fx);
}

TEST_CASE("graph null: execute-time prologue for a graphics-only state consumed by async compute") {
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device);
    const rhi::TextureH hist = fx.texture("History", Format::RGBA8Unorm, 32, 32, kAllColorUsage);
    // Put the history into RenderTarget, as last frame's graphics pass would have left it.
    rhi::CommandList* cmd = fx.device->acquireCommandList(rhi::Queue::Graphics, "Setup");
    cmd->barrier(rhi::Barrier::textureState(hist, ResourceState::Undefined, ResourceState::RenderTarget));
    REQUIRE(fx.device->submit(rhi::Queue::Graphics, {&cmd, 1}).ok());
    fx.null->clearTrace();

    RenderGraph graph("Prologue");
    RgTexture h = graph.importTexture("History", hist, fx.device->textureDesc(hist),
                                      {.initialState = ResourceState::RenderTarget, .finalState = ResourceState::ShaderResource});
    RgBuffer out;
    graph.addPass("Reproject", PassFlags::AsyncCompute | PassFlags::NeverCull,
                  [&](RgBuilder& b) {
                      b.read(h);
                      out = b.write(b.create("Out", RgBufferDesc{64}), BufferWrite::CopyDest);
                  },
                  [&out](RgContext& ctx) { ctx.cmd().fillBuffer(ctx.buffer(out), 0, 64, 3u); });
    REQUIRE(graph.compile().ok());
    CHECK(batchesText(graph.plan()) == "A[Reproject]");
    REQUIRE(graph.execute(*fx.device, pool).ok());
    CHECK_NULL_CLEAN(fx);
    CHECK(batchesText(graph.executedPlan()) == "G(prologue)[] A[Reproject]<-0");
    const std::string trace = fx.null->trace();
    CHECK(trace.starts_with("submit Graphics #2 waits=[]\n  list \"Prologue:prologue\"\n    barrier texture \"History\" "
                            "RenderTarget->ShaderResource"));
    CHECK(trace.find("submit AsyncCompute #1 waits=[Graphics:2]") != std::string::npos);
    CHECK(fx.null->textureState(hist) == ResourceState::ShaderResource);
}

TEST_CASE("graph null: extra waits, context validation and execute errors") {
    NullDeviceFixture fx;
    RgResourcePool pool(*fx.device);
    RenderGraph notCompiled;
    CHECK(notCompiled.execute(*fx.device, pool).errorCode() == ErrorCode::InvalidState);

    NullDeviceFixture other;
    RgResourcePool foreignPool(*other.device);
    RenderGraph graph("Ctx");
    RgBuffer a, b;
    rhi::BufferH seen;
    graph.addPass("A", PassFlags::Copy | PassFlags::NeverCull,
                  [&](RgBuilder& builder) {
                      a = builder.write(builder.create("A", RgBufferDesc{64}), BufferWrite::CopyDest);
                      b = builder.create("B", RgBufferDesc{64});
                  },
                  [&](RgContext& ctx) {
                      CHECK(ctx.passName() == "A");
                      CHECK(ctx.renderArea().width == 0);
                      CHECK(ctx.size(a) == 64);
                      CHECK(ctx.buffer(a).isValid());
                      seen = ctx.buffer(b);  // not declared by this pass
                  });
    REQUIRE(graph.compile().ok());
    CHECK(graph.execute(*fx.device, foreignPool).errorCode() == ErrorCode::InvalidArgument);

    // An upload on the transfer queue the graph must wait for.
    rhi::CommandList* up = fx.device->acquireCommandList(rhi::Queue::Transfer, "Upload");
    const rhi::TimelinePoint uploaded = fx.device->submit(rhi::Queue::Transfer, {&up, 1}).value();
    fx.null->clearTrace();
    const rhi::TimelinePoint waits[] = {uploaded};
    {
        QuietLog quiet;
        REQUIRE(graph.execute(*fx.device, pool, {.waits = waits}).ok());
    }
    CHECK(!seen.isValid());
    CHECK(graph.contextErrorCount() == 1);
    CHECK(fx.null->trace().starts_with("submit Graphics #1 waits=[Transfer:1]"));
    CHECK_NULL_CLEAN(fx);
}
