// Render graph compile: culling, versions, barriers and layout transitions, async batches and
// waits, aliasing, placement, store ops, partitioning, entry resolution, validation and dumps.

#include <doctest/doctest.h>

#include "graph_test_util.h"
#include "helios/core/time.h"

using namespace graphtest;
using rhi::Format;
using rhi::ResourceState;

namespace {

const RgTextureDesc kTex = RgTextureDesc::tex2D(Format::RGBA8Unorm, 64, 64);
const RgTextureDesc kHdr = RgTextureDesc::tex2D(Format::RGBA16Float, 64, 64);
const RgBufferDesc kBuf{256};

/// Fake imported handle for compile-only tests (never dereferenced without a device).
rhi::TextureH fakeTexture(u32 index) { return rhi::TextureH(index, 1); }
rhi::BufferH fakeBuffer(u32 index) { return rhi::BufferH(index, 1); }

rhi::TextureDesc importDesc(Format format = Format::RGBA8Unorm, u32 size = 64, u32 mips = 1) {
    rhi::TextureDesc d = rhi::TextureDesc::tex2D(format, size, size, kAllColorUsage, {}, mips);
    return d;
}

rhi::BufferDesc bufferDesc(u64 size = 256) {
    return {.size = size,
            .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc | rhi::BufferUsage::TransferDst};
}

std::vector<std::vector<ResourceState>> initialStates(const RgPlan& plan) {
    std::vector<std::vector<ResourceState>> states;
    for (const RgPhysicalInfo& phys : plan.physicals) {
        const RgResourceInfo& r = plan.resources[phys.residents.front()];
        states.emplace_back(phys.subresourceCount(), r.imported ? r.import.initialState : ResourceState::Undefined);
    }
    return states;
}

} // namespace

TEST_CASE("graph: culling keeps only what side-effect passes need") {
    for (bool cull : {true, false}) {
        CAPTURE(cull);
        RenderGraph graph("Cull");
        RgTexture out = graph.importTexture("Out", fakeTexture(1), importDesc());
        RgTexture t1;
        RgTexture t2;
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { t1 = b.write(b.create("T1", kTex)); }, {});
        graph.addPass("B", PassFlags::Compute,
                      [&](RgBuilder& b) {
                          b.read(t1);
                          out = b.write(out);
                      },
                      {});
        graph.addPass("C", PassFlags::Compute, [&](RgBuilder& b) { t2 = b.write(b.create("T2", kTex)); }, {});
        graph.addPass("D", PassFlags::Compute,
                      [&](RgBuilder& b) {
                          b.read(t2);
                          b.write(b.create("T3", kTex));
                      },
                      {});
        graph.addPass("E", PassFlags::Compute | PassFlags::NeverCull,
                      [&](RgBuilder& b) { b.write(b.create("T4", kTex)); }, {});
        REQUIRE(graph.errors().empty());
        REQUIRE(graph.compile({.cull = cull}).ok());
        const RgPlan& plan = graph.plan();
        CHECK(pass(plan, "A").culled == false);
        CHECK(pass(plan, "B").culled == false);
        CHECK(pass(plan, "C").culled == cull);
        CHECK(pass(plan, "D").culled == cull);
        CHECK(pass(plan, "E").culled == false);
        CHECK(plan.stats.culledPassCount == (cull ? 2u : 0u));
        CHECK(plan.order.size() == (cull ? 3u : 5u));
        CHECK(resource(plan, "T2").used == !cull);
        CHECK(resource(plan, "T3").used == !cull);
        CHECK(resource(plan, "T1").used);
    }
}

TEST_CASE("graph: culling follows versions (overwrite vs read-modify-write)") {
    for (bool preserve : {false, true}) {
        CAPTURE(preserve);
        RenderGraph graph("Versions");
        RgTexture out = graph.importTexture("Out", fakeTexture(1), importDesc());
        RgTexture t;
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { t = b.write(b.create("T", kTex)); }, {});
        graph.addPass("B", PassFlags::Compute, [&](RgBuilder& b) { t = preserve ? b.readWrite(t) : b.write(t); }, {});
        graph.addPass("C", PassFlags::Compute,
                      [&](RgBuilder& b) {
                          b.read(t);
                          out = b.write(out);
                      },
                      {});
        CHECK(t.version == 2);
        REQUIRE(graph.compile().ok());
        CHECK(pass(graph.plan(), "A").culled == !preserve);
        CHECK(pass(graph.plan(), "B").culled == false);
        CHECK(resource(graph.plan(), "T").versionCount == 3);
    }
}

TEST_CASE("graph: stale versions and other setup mistakes fail compile with a message") {
    SUBCASE("stale read") {
        RenderGraph graph;
        RgTexture v1;
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { v1 = b.write(b.create("T", kTex)); }, {});
        graph.addPass("B", PassFlags::Compute, [&](RgBuilder& b) { b.write(v1); }, {});
        graph.addPass("C", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(v1); }, {});
        CHECK(hasError(graph, "stale 'T' version 1"));
        auto r = graph.compile();
        REQUIRE(!r.ok());
        CHECK(r.error().code == ErrorCode::InvalidArgument);
        CHECK(r.error().message.find("pass 'B' already produced version 2") != std::string::npos);
        CHECK(!graph.isCompiled());
    }
    SUBCASE("read before any write") {
        RenderGraph graph;
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { b.read(b.create("T", kTex)); }, {});
        CHECK(hasError(graph, "reads 'T' before any pass wrote it"));
    }
    SUBCASE("async compute cannot use graphics-only states") {
        RenderGraph graph;
        graph.addPass("A", PassFlags::AsyncCompute, [&](RgBuilder& b) {
            RgTexture t = b.create("T", kTex);
            b.colorAttachment(t, 0);
        }, {});
        CHECK(hasError(graph, "attachments need PassFlags::Raster"));
        RgBuffer buf;
        graph.addPass("W", PassFlags::Compute, [&](RgBuilder& b) { buf = b.write(b.create("B", kBuf)); }, {});
        graph.addPass("X", PassFlags::AsyncCompute, [&](RgBuilder& b) { b.read(buf, BufferRead::Vertex); }, {});
        CHECK(hasError(graph, "async-compute passes cannot use 'B' as VertexBuffer"));
    }
    SUBCASE("imported usage is checked") {
        RenderGraph graph;
        rhi::TextureDesc d = importDesc();
        d.usage = rhi::TextureUsage::Sampled;
        RgTexture t = graph.importTexture("Hist", fakeTexture(1), d);
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { b.write(t); }, {});
        CHECK(hasError(graph, "imported 'Hist' lacks usage Storage (has Sampled)"));
    }
    SUBCASE("conflicting states in one pass") {
        RenderGraph graph;
        RgTexture t = graph.importTexture("T", fakeTexture(1), importDesc());
        graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) {
            b.read(t);
            b.write(t, TextureWrite::CopyDest);
        }, {});
        CHECK(hasError(graph, "'T' is used as both ShaderResource and CopyDest"));
    }
    SUBCASE("raster passes need attachments of one extent") {
        RenderGraph graph;
        graph.addPass("Empty", PassFlags::Raster, [&](RgBuilder&) {}, {});
        CHECK(hasError(graph, "needs at least one attachment"));
        graph.addPass("Mixed", PassFlags::Raster, [&](RgBuilder& b) {
            b.colorAttachment(b.create("C", kTex), 0);
            b.depthAttachment(b.create("D", RgTextureDesc::tex2D(Format::D32Float, 32, 32)));
        }, {});
        CHECK(hasError(graph, "is 32x32, others are 64x64"));
        graph.addPass("Gap", PassFlags::Raster, [&](RgBuilder& b) { b.colorAttachment(b.create("C1", kTex), 1); }, {});
        CHECK(hasError(graph, "contiguous"));
        graph.addPass("DepthAsColor", PassFlags::Raster, [&](RgBuilder& b) {
            b.colorAttachment(b.create("Z", RgTextureDesc::tex2D(Format::D32Float, 64, 64)), 0);
        }, {});
        CHECK(hasError(graph, "used as a color attachment"));
    }
    SUBCASE("pass kind flags and ranges") {
        RenderGraph graph;
        graph.addPass("Two", PassFlags::Compute | PassFlags::Copy, [&](RgBuilder&) {}, {});
        CHECK(hasError(graph, "exactly one of Raster, Compute, AsyncCompute or Copy"));
        graph.addPass("Range", PassFlags::Compute, [&](RgBuilder& b) {
            b.write(b.create("M", RgTextureDesc::tex2D(Format::RGBA8Unorm, 64, 64, 2)), TextureWrite::Storage, {2, 1, 0, 1});
        }, {});
        CHECK(hasError(graph, "outside 'M' (2 mips, 1 layers)"));
        graph.addPass("Desc", PassFlags::Compute, [&](RgBuilder& b) {
            b.create("Huge", RgTextureDesc::tex2D(Format::RGBA8Unorm, 64, 64, 9));
        }, {});
        CHECK(hasError(graph, "invalid description for texture 'Huge'"));
        graph.addPass("Copy", PassFlags::Copy, [&](RgBuilder& b) { b.write(b.create("S", kTex)); }, {});
        CHECK(hasError(graph, "copy passes can only use copy states"));
    }
}

TEST_CASE("graph: ranges, mips and descriptions that overflow 32 bits are rejected") {
    // base + count used to be summed in u32: {2, 0xFFFFFFFE} wrapped to 0 and passed the bounds check,
    // and an attachment mip of 0xFFFFFFFF reached mipExtent() (a shift by 2^32 - 1) and beginRendering.
    RenderGraph graph;
    const RgTextureDesc mipped = RgTextureDesc::tex2D(Format::RGBA8Unorm, 64, 64, 4);
    graph.addPass("WrapRange", PassFlags::Compute, [&](RgBuilder& b) {
        b.write(b.create("A", mipped), TextureWrite::Storage, {2, 0xFFFFFFFEu, 0, 1});
    }, {});
    CHECK(hasError(graph, "outside 'A' (4 mips, 1 layers)"));
    graph.addPass("WrapLayers", PassFlags::Compute, [&](RgBuilder& b) {
        b.write(b.create("B", mipped), TextureWrite::Storage, {0, 1, 1, 0xFFFFFFFFu});
    }, {});
    CHECK(hasError(graph, "outside 'B' (4 mips, 1 layers)"));
    graph.addPass("WrapBase", PassFlags::Compute, [&](RgBuilder& b) {
        b.write(b.create("C", mipped), TextureWrite::Storage, {0xFFFFFFFFu, 1, 0, 1});
    }, {});
    CHECK(hasError(graph, "outside 'C' (4 mips, 1 layers)"));
    graph.addPass("AttachmentMip", PassFlags::Raster, [&](RgBuilder& b) {
        b.colorAttachment(b.create("D", mipped), 0, rhi::LoadOp::Clear, {0, 0, 0, 1}, 0xFFFFFFFFu);
    }, {});
    CHECK(hasError(graph, "outside 'D' (4 mips, 1 layers)"));
    graph.addPass("DepthLayer", PassFlags::Raster, [&](RgBuilder& b) {
        b.depthAttachment(b.create("Z", RgTextureDesc::tex2D(Format::D32Float, 64, 64)), rhi::LoadOp::Clear, 0.0f, 0,
                          0xFFFFFFFFu);
    }, {});
    CHECK(hasError(graph, "outside 'Z' (1 mips, 1 layers)"));
    CHECK(!graph.compile().ok());

    // mipLevels * arrayLayers used to wrap to 0 subresources: compile then indexed past the end of
    // its per-subresource state (heap overflow).
    RenderGraph layers;
    layers.addPass("Layers", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
        RgTextureDesc d = RgTextureDesc::tex2D(Format::R8Unorm, 65536, 1, 16);
        d.arrayLayers = 1u << 28;
        b.write(b.create("Wrap", d), TextureWrite::Storage, {0, 1, 0, 1});
    }, {});
    CHECK(hasError(layers, "invalid description for texture 'Wrap'"));
    CHECK(!layers.compile().ok());
}

TEST_CASE("graph: barriers and layout transitions are derived from usages") {
    RenderGraph graph("Frame");
    RgTexture out = graph.importTexture("Out", fakeTexture(1), importDesc(),
                                        {.initialState = ResourceState::Undefined, .finalState = ResourceState::CopySource});
    RgTexture color;
    graph.addPass("Clear", PassFlags::Copy,
                  [&](RgBuilder& b) { color = b.write(b.create("Color", kHdr), TextureWrite::CopyDest); }, {});
    graph.addPass("Geometry", PassFlags::Raster,
                  [&](RgBuilder& b) {
                      color = b.colorAttachment(color, 0, rhi::LoadOp::Load);
                      b.depthAttachment(b.create("Depth", RgTextureDesc::tex2D(Format::D32Float, 64, 64)));
                  },
                  {});
    graph.addPass("Post", PassFlags::Compute,
                  [&](RgBuilder& b) {
                      b.read(color);
                      out = b.write(out);
                  },
                  {});
    REQUIRE(graph.compile().ok());
    const RgPlan& plan = graph.plan();
    CHECK(pre(plan, "Clear") == "Color:entry->CopyDest");
    CHECK(pre(plan, "Geometry") == "Color:CopyDest->RenderTarget, Depth:entry->DepthWrite");
    CHECK(pre(plan, "Post") == "Color:RenderTarget->ShaderResource, Out:entry->UnorderedAccess");
    CHECK(post(plan, "Post") == "Out:UnorderedAccess->CopySource");
    CHECK(post(plan, "Clear").empty());
    // Depth is never read again: DontCare; the color attachment feeds Post: Store.
    CHECK(pass(plan, "Geometry").depthStore == rhi::StoreOp::DontCare);
    CHECK(pass(plan, "Geometry").colorStore.at(0) == rhi::StoreOp::Store);
    // Usage flags of transients come from their accesses.
    CHECK(resource(plan, "Color").textureUsage ==
          (rhi::TextureUsage::TransferDst | rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled));
    CHECK(resource(plan, "Depth").textureUsage == rhi::TextureUsage::DepthStencil);
    CHECK(batchesText(plan) == "G[Clear,Geometry,Post]");
    CHECK(plan.physicals.size() == 3);
    CHECK(plan.physicals[resource(plan, "Out").physical].finalStates.at(0) == ResourceState::CopySource);
}

TEST_CASE("graph: same-state hazards get barriers, read-after-read does not") {
    RenderGraph graph("Uav");
    RgTexture t;
    graph.addPass("A", PassFlags::Compute, [&](RgBuilder& b) { t = b.write(b.create("T", kTex)); }, {});
    graph.addPass("B", PassFlags::Compute, [&](RgBuilder& b) { t = b.readWrite(t); }, {});
    graph.addPass("C", PassFlags::Compute, [&](RgBuilder& b) { b.read(t, TextureRead::Storage); }, {});
    graph.addPass("D", PassFlags::Compute, [&](RgBuilder& b) { b.read(t, TextureRead::Storage); }, {});
    graph.addPass("E", PassFlags::Compute, [&](RgBuilder& b) { t = b.write(t); }, {});
    graph.addPass("F", PassFlags::Compute, [&](RgBuilder& b) { b.read(t); }, {});
    REQUIRE(graph.compile({.cull = false}).ok());
    const RgPlan& plan = graph.plan();
    CHECK(pre(plan, "A") == "T:entry->UnorderedAccess");
    CHECK(pre(plan, "B") == "T:UnorderedAccess->UnorderedAccess");  // WAW / RAW (read-modify-write)
    CHECK(pre(plan, "C") == "T:UnorderedAccess->UnorderedAccess");  // RAW
    CHECK(pre(plan, "D").empty());                                   // read after read
    CHECK(pre(plan, "E") == "T:UnorderedAccess->UnorderedAccess");  // WAR
    CHECK(pre(plan, "F") == "T:UnorderedAccess->ShaderResource");
}

TEST_CASE("graph: per-subresource tracking for a mip chain") {
    RenderGraph graph("Mips");
    RgTexture t;
    const RgTextureDesc desc = RgTextureDesc::tex2D(Format::RGBA16Float, 64, 64, 4);
    graph.addPass("Mip0", PassFlags::Compute,
                  [&](RgBuilder& b) { t = b.write(b.create("T", desc), TextureWrite::Storage, {0, 1, 0, 1}); }, {});
    for (u32 m = 1; m < 4; ++m) {
        graph.addPass(std::format("Mip{}", m), PassFlags::Compute,
                      [&, m](RgBuilder& b) {
                          b.read(t, TextureRead::Sampled, {m - 1, 1, 0, 1});
                          t = b.write(t, TextureWrite::Storage, {m, 1, 0, 1});
                      },
                      {});
    }
    graph.addPass("Use", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(t); }, {});
    REQUIRE(graph.compile().ok());
    const RgPlan& plan = graph.plan();
    CHECK(plan.stats.culledPassCount == 0);
    CHECK(pre(plan, "Mip0") == "T:entry->UnorderedAccess m0+1 l0+1");
    CHECK(pre(plan, "Mip1") == "T:UnorderedAccess->ShaderResource m0+1 l0+1, T:entry->UnorderedAccess m1+1 l0+1");
    CHECK(pre(plan, "Mip2") == "T:UnorderedAccess->ShaderResource m1+1 l0+1, T:entry->UnorderedAccess m2+1 l0+1");
    CHECK(pre(plan, "Mip3") == "T:UnorderedAccess->ShaderResource m2+1 l0+1, T:entry->UnorderedAccess m3+1 l0+1");
    CHECK(pre(plan, "Use") == "T:UnorderedAccess->ShaderResource m3+1 l0+1");
    // Partial writes keep the other subresources: each depends on the previous version.
    CHECK(pass(plan, "Mip2").accesses.back().readVersion == 2);
    CHECK(pass(plan, "Mip2").accesses.back().writeVersion == 3);
}

TEST_CASE("graph: async compute batches overlap graphics and wait only where needed") {
    for (bool async : {true, false}) {
        CAPTURE(async);
        RenderGraph graph("Async");
        RgBuffer a;
        RgBuffer b2;
        graph.addPass("G0", PassFlags::Compute, [&](RgBuilder& b) { a = b.write(b.create("A", kBuf)); }, {});
        graph.addPass("X1", PassFlags::AsyncCompute,
                      [&](RgBuilder& b) {
                          b.read(a);
                          b2 = b.write(b.create("B", kBuf));
                      },
                      {});
        graph.addPass("G2", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.write(b.create("C", kBuf)); },
                      {});
        graph.addPass("G3", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(b2); }, {});
        REQUIRE(graph.compile({.asyncCompute = async}).ok());
        const RgPlan& plan = graph.plan();
        if (async) {
            CHECK(batchesText(plan) == "G[G0] A[X1]<-0 G[G2] G[G3]<-1");
            CHECK(pass(plan, "X1").queue == rhi::Queue::AsyncCompute);
            CHECK(pre(plan, "X1") == "A:UnorderedAccess->ShaderResource, B:entry->UnorderedAccess");
            CHECK(plan.stats.crossQueueWaitCount == 2);
        } else {
            CHECK(batchesText(plan) == "G[G0,X1,G2,G3]");
            CHECK(pass(plan, "X1").queue == rhi::Queue::Graphics);
            CHECK(plan.stats.crossQueueWaitCount == 0);
        }
        CHECK(pre(plan, "G3") == "B:UnorderedAccess->ShaderResource");
    }
}

TEST_CASE("graph: waits implied by vector clocks or by later waits are dropped") {
    SUBCASE("latest producer per queue only") {
        RenderGraph graph("Waits");
        RgBuffer a, b2;
        graph.addPass("G0", PassFlags::Compute, [&](RgBuilder& b) { a = b.write(b.create("A", kBuf)); }, {});
        graph.addPass("G1", PassFlags::Compute, [&](RgBuilder& b) { b2 = b.write(b.create("B", kBuf)); }, {});
        graph.addPass("X2", PassFlags::AsyncCompute | PassFlags::NeverCull,
                      [&](RgBuilder& b) {
                          b.read(a);
                          b.read(b2);
                      },
                      {});
        REQUIRE(graph.compile().ok());
        CHECK(batchesText(graph.plan()) == "G[G0] G[G1] A[X2]<-1");
    }
    SUBCASE("a dependency already known through queue order does not split or wait") {
        RenderGraph graph("Implied");
        RgBuffer a;
        graph.addPass("X0", PassFlags::AsyncCompute, [&](RgBuilder& b) { a = b.write(b.create("A", kBuf)); }, {});
        graph.addPass("G1", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(a); }, {});
        graph.addPass("G2", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(a); }, {});
        REQUIRE(graph.compile().ok());
        CHECK(batchesText(graph.plan()) == "A[X0] G[G1,G2]<-0");
    }
    SUBCASE("round trip: a wait implied by another wait's clock") {
        // X0 -> G1 -> X2 -> G3, and G3 also reads X0's output: waiting for X2 implies X0.
        RenderGraph graph("RoundTrip");
        RgBuffer a, b1, c;
        graph.addPass("X0", PassFlags::AsyncCompute, [&](RgBuilder& b) { a = b.write(b.create("A", kBuf)); }, {});
        graph.addPass("G1", PassFlags::Compute, [&](RgBuilder& b) {
            b.read(a);
            b1 = b.write(b.create("B", kBuf));
        }, {});
        graph.addPass("X2", PassFlags::AsyncCompute, [&](RgBuilder& b) {
            b.read(b1);
            c = b.write(b.create("C", kBuf));
        }, {});
        graph.addPass("G3", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
            b.read(c);
            b.read(a);
        }, {});
        REQUIRE(graph.compile().ok());
        CHECK(batchesText(graph.plan()) == "A[X0] G[G1]<-0 A[X2]<-1 G[G3]<-2");
    }
}

TEST_CASE("graph: graphics-only states are released on graphics before async use") {
    RenderGraph graph("Release");
    RgTexture color;
    graph.addPass("G0", PassFlags::Raster, [&](RgBuilder& b) { color = b.colorAttachment(b.create("Color", kTex), 0); },
                  {});
    graph.addPass("X1", PassFlags::AsyncCompute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(color); }, {});
    graph.addPass("G2", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(color); }, {});
    REQUIRE(graph.compile().ok());
    const RgPlan& plan = graph.plan();
    CHECK(post(plan, "G0") == "Color:RenderTarget->ShaderResource");
    CHECK(pre(plan, "X1").empty());
    CHECK(pre(plan, "G2").empty());  // same state, the release happened on graphics
    CHECK(batchesText(plan) == "G[G0] A[X1]<-0 G[G2]");
}

TEST_CASE("graph: imported final states go after the last use or into a graphics epilogue") {
    for (ResourceState fin : {ResourceState::ShaderResource, ResourceState::Present}) {
        CAPTURE(rhi::resourceStateName(fin));
        RenderGraph graph("Final");
        RgTexture out = graph.importTexture("Out", fakeTexture(1), importDesc(), {.finalState = fin});
        RgTexture untouched = graph.importTexture("Idle", fakeTexture(2), importDesc(),
                                                  {.initialState = ResourceState::RenderTarget, .finalState = fin});
        (void)untouched;
        graph.addPass("X0", PassFlags::AsyncCompute, [&](RgBuilder& b) { out = b.write(out); }, {});
        REQUIRE(graph.compile().ok());
        const RgPlan& plan = graph.plan();
        if (fin == ResourceState::ShaderResource) {
            CHECK(post(plan, "X0") == "Out:UnorderedAccess->ShaderResource");
            CHECK(batchesText(plan) == "A[X0] G(epilogue)[]");
            CHECK(barriersText(plan, plan.batches.back().barriers) == "Idle:entry->ShaderResource");
        } else {
            CHECK(post(plan, "X0").empty());
            CHECK(batchesText(plan) == "A[X0] G(epilogue)[]<-0");
            CHECK(barriersText(plan, plan.batches.back().barriers) ==
                  "Out:UnorderedAccess->Present, Idle:entry->Present");
        }
        CHECK(plan.physicals[resource(plan, "Idle").physical].finalStates.at(0) == fin);
    }
}

TEST_CASE("graph: transients with ordered lifetimes share a physical resource") {
    for (bool alias : {true, false}) {
        CAPTURE(alias);
        RenderGraph graph("Alias");
        RgTexture t1, t2, t3;
        graph.addPass("P0", PassFlags::Compute, [&](RgBuilder& b) { t1 = b.write(b.create("T1", kTex)); }, {});
        graph.addPass("P1", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(t1); }, {});
        graph.addPass("P2", PassFlags::Compute, [&](RgBuilder& b) {
            t2 = b.write(b.create("T2", kTex));
            t3 = b.write(b.create("T3", kHdr));
        }, {});
        graph.addPass("P3", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
            b.read(t2);
            b.read(t3);
        }, {});
        REQUIRE(graph.compile({.alias = alias}).ok());
        const RgPlan& plan = graph.plan();
        const bool shared = resource(plan, "T1").physical == resource(plan, "T2").physical;
        CHECK(shared == alias);
        CHECK(resource(plan, "T3").physical != resource(plan, "T1").physical);
        CHECK(plan.stats.transientBytes == 3 * 65536);  // each rounded up to the 64 KiB placement alignment
        if (alias) {
            CHECK(plan.physicals.size() == 2);
            CHECK(pre(plan, "P2") == "T1:ShaderResource->UnorderedAccess, T3:entry->UnorderedAccess");
            CHECK(plan.stats.pooledBytes == plan.stats.transientBytes - 65536);
            CHECK(plan.physicals[resource(plan, "T1").physical].residents.size() == 2);
        } else {
            CHECK(plan.physicals.size() == 3);
            CHECK(pre(plan, "P2") == "T2:entry->UnorderedAccess, T3:entry->UnorderedAccess");
            CHECK(plan.stats.pooledBytes == plan.stats.transientBytes);
        }
    }
}

TEST_CASE("graph: no aliasing between work that may run concurrently on two queues") {
    for (bool synced : {false, true}) {
        CAPTURE(synced);
        RenderGraph graph("AsyncAlias");
        RgTexture t1, t2;
        RgBuffer sync;
        graph.addPass("X1", PassFlags::AsyncCompute, [&](RgBuilder& b) { t1 = b.write(b.create("T1", kTex)); }, {});
        graph.addPass("X2", PassFlags::AsyncCompute | PassFlags::NeverCull, [&](RgBuilder& b) {
            b.read(t1);
            if (synced) sync = b.write(b.create("Sync", kBuf));
        }, {});
        graph.addPass("G1", PassFlags::Compute, [&](RgBuilder& b) {
            if (synced) b.read(sync);
            t2 = b.write(b.create("T2", kTex));
        }, {});
        graph.addPass("G2", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(t2); }, {});
        REQUIRE(graph.compile().ok());
        const RgPlan& plan = graph.plan();
        CHECK((resource(plan, "T1").physical == resource(plan, "T2").physical) == synced);
        if (synced) {
            CHECK(batchesText(plan) == "A[X1,X2] G[G1,G2]<-0");
            // Graphics takes the memory over from async compute after the wait.
            CHECK(pre(plan, "G1") == "Sync:UnorderedAccess->ShaderResource, T1:ShaderResource->UnorderedAccess");
        } else {
            CHECK(batchesText(plan) == "A[X1,X2] G[G1,G2]");
        }
    }
}

TEST_CASE("graph: placement plan packs transients with disjoint lifetimes") {
    for (bool overlap : {false, true}) {
        CAPTURE(overlap);
        RenderGraph graph("Placement");
        RgTexture big, small;
        const RgTextureDesc bigDesc = RgTextureDesc::tex2D(Format::RGBA16Float, 256, 256);  // 512 KiB
        if (overlap) {
            graph.addPass("P0", PassFlags::Compute, [&](RgBuilder& b) { big = b.write(b.create("Big", bigDesc)); }, {});
            graph.addPass("P1", PassFlags::Compute, [&](RgBuilder& b) { small = b.write(b.create("Small", kTex)); }, {});
            graph.addPass("P2", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
                b.read(big);
                b.read(small);
            }, {});
        } else {
            graph.addPass("P0", PassFlags::Compute, [&](RgBuilder& b) { big = b.write(b.create("Big", bigDesc)); }, {});
            graph.addPass("P1", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(big); }, {});
            graph.addPass("P2", PassFlags::Compute, [&](RgBuilder& b) { small = b.write(b.create("Small", kTex)); }, {});
            graph.addPass("P3", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(small); }, {});
        }
        REQUIRE(graph.compile().ok());
        const RgPlan& plan = graph.plan();
        CHECK(resource(plan, "Big").heap == RgHeapKind::Textures);
        CHECK(resource(plan, "Big").bytes == 524288);
        CHECK(resource(plan, "Small").bytes == 65536);
        CHECK(resource(plan, "Big").heapOffset == 0);
        CHECK(resource(plan, "Small").heapOffset == (overlap ? 524288u : 0u));
        CHECK(plan.stats.placedBytes == (overlap ? 589824u : 524288u));
        CHECK(plan.stats.pooledBytes == 589824);  // different descriptions never pool
    }
}

TEST_CASE("graph: command lists are partitioned across batches by pass count") {
    RenderGraph graph("Part");
    RgBuffer g;
    RgBuffer a;
    graph.addPass("G0", PassFlags::Compute, [&](RgBuilder& b) { g = b.write(b.create("G", kBuf)); }, {});
    for (int i = 1; i < 6; ++i) {
        graph.addPass(std::format("G{}", i), PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) { g = b.readWrite(g); },
                      {});
    }
    graph.addPass("A0", PassFlags::AsyncCompute, [&](RgBuilder& b) { a = b.write(b.create("A", kBuf)); }, {});
    graph.addPass("A1", PassFlags::AsyncCompute | PassFlags::NeverCull, [&](RgBuilder& b) { a = b.readWrite(a); }, {});
    REQUIRE(graph.compile({.maxCommandLists = 4}).ok());
    const RgPlan& plan = graph.plan();
    REQUIRE(plan.batches.size() == 2);
    CHECK(plan.batches[0].listCount == 3);
    CHECK(plan.batches[1].listCount == 1);
    REQUIRE(plan.lists.size() == 4);
    CHECK(plan.lists[0].name == "Part:G0+1");
    CHECK(plan.lists[1].name == "Part:G2+1");
    CHECK(plan.lists[2].name == "Part:G4+1");
    CHECK(plan.lists[3].name == "Part:A0+1");
    CHECK(pass(plan, "G3").list == 1);
    REQUIRE(graph.compile({.maxCommandLists = 1}).ok());
    CHECK(graph.plan().lists.size() == 2);  // at least one list per batch
    REQUIRE(graph.compile({.maxCommandLists = 64}).ok());
    CHECK(graph.plan().lists.size() == 8);  // at most one list per pass
}

TEST_CASE("graph: entry barriers resolve against current states, with a prologue when needed") {
    RenderGraph graph("Entry");
    RgTexture hist = graph.importTexture("Hist", fakeTexture(1), importDesc(),
                                         {.initialState = ResourceState::RenderTarget});
    RgTexture tex = graph.importTexture("Tex", fakeTexture(2), importDesc(),
                                        {.initialState = ResourceState::ShaderResource});
    RgBuffer buf = graph.importBuffer("Buf", fakeBuffer(3), bufferDesc(), {.initialState = ResourceState::UnorderedAccess});
    graph.addPass("G0", PassFlags::Compute, [&](RgBuilder& b) {
        b.read(tex);
        buf = b.readWrite(buf);
    }, {});
    graph.addPass("X1", PassFlags::AsyncCompute | PassFlags::NeverCull, [&](RgBuilder& b) { b.read(hist); }, {});
    REQUIRE(graph.compile().ok());
    const RgPlan& plan = graph.plan();
    CHECK(pre(plan, "G0") == "Tex:entry->ShaderResource, Buf:entry->UnorderedAccess");
    CHECK(pre(plan, "X1") == "Hist:entry->ShaderResource");
    CHECK(batchesText(plan) == "G[G0] A[X1]");

    const RgPlan resolved = resolveEntryBarriers(plan, initialStates(plan));
    CHECK(pre(resolved, "G0") == "Buf:UnorderedAccess->UnorderedAccess");  // write state: kept; read-only match: dropped
    CHECK(pre(resolved, "X1").empty());
    CHECK(batchesText(resolved) == "G(prologue)[] G[G0] A[X1]<-0");
    CHECK(barriersText(resolved, resolved.batches[0].barriers) == "Hist:RenderTarget->ShaderResource");
    CHECK(resolved.batches[1].queueValue == 2);
    CHECK(resolved.lists.front().name == "Entry:prologue");
    CHECK(pass(resolved, "G0").batch == 1);
    CHECK(pass(resolved, "X1").list == resolved.batches[2].firstList);
    // Everything already in place: nothing to do.
    std::vector<std::vector<ResourceState>> ready = initialStates(plan);
    ready[resource(plan, "Hist").physical].assign(1, ResourceState::ShaderResource);
    const RgPlan quiet = resolveEntryBarriers(plan, ready);
    CHECK(batchesText(quiet) == "G[G0] A[X1]");
    CHECK(quiet.stats.barrierCount == 1);
}

TEST_CASE("graph: dumps are deterministic and describe the plan") {
    auto build = [](RenderGraph& graph) {
        RgTexture out = graph.importTexture("Backbuffer", fakeTexture(1), importDesc(),
                                            {.finalState = ResourceState::Present});
        RgTexture color;
        RgBuffer lum;
        graph.addPass("Geometry", PassFlags::Raster, [&](RgBuilder& b) {
            color = b.colorAttachment(b.create("SceneColor", kHdr), 0);
            b.depthAttachment(b.create("Depth", RgTextureDesc::tex2D(Format::D32Float, 64, 64)));
        }, {});
        graph.addPass("Exposure", PassFlags::AsyncCompute, [&](RgBuilder& b) {
            b.read(color);
            lum = b.write(b.create("Exposure", RgBufferDesc{16}));
        }, {});
        graph.addPass("Debug", PassFlags::Compute, [&](RgBuilder& b) {
            b.read(color);
            b.write(b.create("DebugView", kTex));
        }, {});
        graph.addPass("Tonemap", PassFlags::Raster, [&](RgBuilder& b) {
            b.read(color);
            b.read(lum);
            out = b.colorAttachment(out, 0, rhi::LoadOp::DontCare);
        }, {});
    };
    RenderGraph a("Dump");
    RenderGraph b("Dump");
    build(a);
    build(b);
    REQUIRE(a.compile().ok());
    REQUIRE(b.compile().ok());
    const std::string text = a.dumpText();
    CHECK(text == b.dumpText());
    CHECK(a.dumpGraphviz() == b.dumpGraphviz());
    CHECK(text.find("RenderGraph \"Dump\": passes=4 culled=1") != std::string::npos);
    CHECK(text.find("culled:\n  \"Debug\" Compute\n") != std::string::npos);
    CHECK(text.find("post p0:SceneColor RenderTarget->ShaderResource") != std::string::npos);
    CHECK(text.find("post p3:Backbuffer RenderTarget->Present") != std::string::npos);
    CHECK(text.find("depth store=DontCare") != std::string::npos);
    const std::string dot = a.dumpGraphviz();
    CHECK(dot.starts_with("digraph \"Dump\" {"));
    CHECK(dot.find("label=\"Debug\\n(culled)\"") != std::string::npos);
    CHECK(dot.find("label=\"wait\"") != std::string::npos);
    CHECK(rgDumpPlan(a.plan()) == text);
}

TEST_CASE("graph: compile budget (200 passes)") {
    // Budget: <= 2 ms on REF for 200 passes without the Phase 2 graph cache (03 §2.2). Checked
    // loosely here (debug builds, shared CI machines); the measured value is reported.
    RenderGraph graph("Budget");
    std::vector<RgTexture> live;
    RgBuffer counter;
    graph.addPass("Init", PassFlags::Compute, [&](RgBuilder& b) { counter = b.write(b.create("Counter", kBuf)); }, {});
    for (u32 i = 0; i < 200; ++i) {
        const PassFlags kind = i % 5 == 4 ? PassFlags::AsyncCompute : (i % 3 == 0 ? PassFlags::Raster : PassFlags::Compute);
        graph.addPass(std::format("P{}", i), kind | (i % 17 == 0 ? PassFlags::NeverCull : PassFlags::None),
                      [&, i, kind](RgBuilder& b) {
                          if (!live.empty()) b.read(live.back());
                          if (live.size() > 2) b.read(live[live.size() - 3]);
                          RgTexture t = b.create(std::format("T{}", i), i % 2 ? kTex : kHdr);
                          live.push_back(kind == PassFlags::Raster ? b.colorAttachment(t, 0) : b.write(t));
                          if (i % 10 == 0 && kind != PassFlags::Raster) counter = b.readWrite(counter);
                      },
                      {});
    }
    graph.addPass("Final", PassFlags::Compute | PassFlags::NeverCull, [&](RgBuilder& b) {
        b.read(live.back());
        b.read(counter);
    }, {});
    REQUIRE(graph.errors().empty());
    REQUIRE(graph.compile().ok());  // warm-up
    const Stopwatch timer;
    constexpr int kRuns = 5;
    for (int i = 0; i < kRuns; ++i) REQUIRE(graph.compile().ok());
    const f64 ms = timer.elapsedMillis() / kRuns;
    const RgPlan& plan = graph.plan();
    MESSAGE("compile of " << plan.stats.passCount << " passes: " << ms << " ms, " << plan.stats.batchCount << " batches, "
                          << plan.stats.barrierCount << " barriers, " << plan.physicals.size() << " physicals for "
                          << plan.resources.size() << " resources, pooled " << plan.stats.pooledBytes / 1024
                          << " KiB of " << plan.stats.transientBytes / 1024 << " KiB");
    CHECK(ms < 50.0);
    CHECK(plan.stats.pooledBytes * 10 < plan.stats.transientBytes * 7);  // >= 30 % saved (03 §2.2 target)
}
