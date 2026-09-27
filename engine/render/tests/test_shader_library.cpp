// Shipped shader modules and the pipeline record (helios/render/shader_library.h), which
// `helios-rendertest --coverage` uses to check RC-1's "every shipped feature has a golden".

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "helios/render/forward.h"
#include "helios/render/shader_library.h"
#include "render_tests_shaders.h"

namespace {

using namespace helios;
using namespace helios::render;

std::unique_ptr<rhi::Device> nullDevice() {
    rhi::DeviceDesc desc;
    desc.appName = "render_tests";
    desc.backend = rhi::Backend::Null;
    auto device = rhi::Device::create(desc);
    REQUIRE(device.ok());
    return std::move(device).value();
}

/// "module:entry" of each stage of the recorded pipeline `name` (empty when it is not recorded).
std::vector<std::string> recordedStages(std::string_view name) {
    std::vector<std::string> out;
    for (const ShippedPipeline& p : shippedPipelines()) {
        if (p.name != name) continue;
        for (const ShippedEntryPoint& e : p.entryPoints) out.push_back(e.module + ":" + e.entryPoint);
    }
    return out;
}

using Stages = std::vector<std::string>;

TEST_CASE("shader library: every embedded module's entry points are listed with their stages, sorted") {
    auto listed = shippedEntryPoints();
    REQUIRE(listed.ok());
    const std::vector<ShippedEntryPoint>& entries = listed.value();
    CHECK(std::is_sorted(entries.begin(), entries.end(), [](const ShippedEntryPoint& a, const ShippedEntryPoint& b) {
        return a.module != b.module ? a.module < b.module : a.entryPoint < b.entryPoint;
    }));
    auto has = [&](const char* module, const char* entry, ShaderStage stage) {
        return std::find(entries.begin(), entries.end(), ShippedEntryPoint{module, entry, stage}) != entries.end();
    };
    CHECK(has("forward", "vsMain", ShaderStage::Vertex));
    CHECK(has("forward", "psMain", ShaderStage::Fragment));
    CHECK(has("forward", "psNormals", ShaderStage::Fragment));
    CHECK(has("exposure", "csExposure", ShaderStage::Compute));
    CHECK(has("tonemap", "vsFullscreen", ShaderStage::Vertex));
    CHECK(has("tonemap", "psTonemap", ShaderStage::Fragment));
    for (const ShippedEntryPoint& e : entries) CHECK_FALSE(shippedModule(e.module).empty());
    CHECK(shippedModule("no-such-module").empty());
}

TEST_CASE("shader library: the forward renderer records every pipeline with its entry points") {
    std::unique_ptr<rhi::Device> device = nullDevice();
    {
        auto renderer = ForwardRenderer::create(*device, rhi::Format::RGBA8Unorm);
        REQUIRE(renderer.ok());
        // A second renderer records the same names with the same shaders: no conflict.
        auto second = ForwardRenderer::create(*device, rhi::Format::RGBA8Unorm);
        REQUIRE(second.ok());
    }
    CHECK(recordedStages("Forward.Geometry") == Stages{"forward:vsMain", "forward:psMain"});
    CHECK(recordedStages("Forward.DebugNormals") == Stages{"forward:vsMain", "forward:psNormals"});
    CHECK(recordedStages("Forward.Exposure") == Stages{"exposure:csExposure"});
    CHECK(recordedStages("Forward.Tonemap.RGBA8Unorm") == Stages{"tonemap:vsFullscreen", "tonemap:psTonemap"});
    const std::vector<ShippedPipeline> all = shippedPipelines();
    CHECK(std::is_sorted(all.begin(), all.end(),
                         [](const ShippedPipeline& a, const ShippedPipeline& b) { return a.name < b.name; }));
    const std::vector<ShippedEntryPoint> shipped = shippedEntryPoints().value();
    for (const ShippedPipeline& p : all) {
        for (const ShippedEntryPoint& e : p.entryPoints) {
            CHECK(std::find(shipped.begin(), shipped.end(), e) != shipped.end());
        }
    }
}

TEST_CASE("shader library: forward renderers for two output formats coexist in one process") {
    // Round-3 review mutant MF1: the tonemap's attachment format is pipeline state, so one name for
    // every output format rejected the second renderer (an editor viewport next to a swapchain).
    std::unique_ptr<rhi::Device> device = nullDevice();
    auto unorm = ForwardRenderer::create(*device, rhi::Format::RGBA8Unorm);
    REQUIRE(unorm.ok());
    auto srgb = ForwardRenderer::create(*device, rhi::Format::BGRA8Srgb);
    REQUIRE(srgb.ok());
    CHECK(recordedStages("Forward.Tonemap.RGBA8Unorm") == Stages{"tonemap:vsFullscreen", "tonemap:psTonemap"});
    CHECK(recordedStages("Forward.Tonemap.BGRA8Srgb") == Stages{"tonemap:vsFullscreen", "tonemap:psTonemap"});
}

TEST_CASE("shader library: unnamed, foreign, unknown-entry and conflicting pipelines are rejected") {
    std::unique_ptr<rhi::Device> device = nullDevice();
    rhi::ComputePipelineDesc compute;
    compute.compute = {shippedModule("exposure"), "csExposure"};
    compute.name = "";
    CHECK(createShippedPipeline(*device, compute).errorCode() == ErrorCode::InvalidArgument);

    // Modules are identified by address: other SPIR-V, even a copy of an embedded module, is foreign.
    compute.name = "Test.Foreign";
    compute.compute = {render_test_shaders::reflect_compute(), "csMain"};
    CHECK(createShippedPipeline(*device, compute).errorCode() == ErrorCode::InvalidArgument);
    const std::span<const u32> exposure = shippedModule("exposure");
    const std::vector<u32> copy(exposure.begin(), exposure.end());
    compute.compute = {copy, "csExposure"};
    CHECK(createShippedPipeline(*device, compute).errorCode() == ErrorCode::InvalidArgument);
    CHECK(recordedStages("Test.Foreign").empty());

    compute.name = "Test.Missing";
    compute.compute = {shippedModule("exposure"), "csMissing"};
    CHECK(createShippedPipeline(*device, compute).errorCode() == ErrorCode::InvalidArgument);
    CHECK(recordedStages("Test.Missing").empty());

    rhi::GraphicsPipelineDesc tone;
    tone.vertex = {shippedModule("tonemap"), "vsFullscreen"};
    tone.fragment = {shippedModule("tonemap"), "psTonemap"};
    tone.colorCount = 1;
    tone.colorFormats[0] = rhi::Format::RGBA8Unorm;
    tone.name = "Test.Renamed";
    auto first = createShippedPipeline(*device, tone);
    REQUIRE(first.ok());
    // The same name with other shaders would make a Null trace's pipeline name ambiguous.
    rhi::GraphicsPipelineDesc other = tone;
    other.fragment = {shippedModule("forward"), "psNormals"};
    CHECK(createShippedPipeline(*device, other).errorCode() == ErrorCode::InvalidArgument);
    CHECK(recordedStages("Test.Renamed") == Stages{"tonemap:vsFullscreen", "tonemap:psTonemap"});
    auto again = createShippedPipeline(*device, tone);
    REQUIRE(again.ok());
    device->destroy(first.value());
    device->destroy(again.value());

    // Device errors pass through and record nothing.
    rhi::GraphicsPipelineDesc invalid = tone;
    invalid.name = "Test.Invalid";
    invalid.colorCount = 0;
    CHECK_FALSE(createShippedPipeline(*device, invalid).ok());
    CHECK(recordedStages("Test.Invalid").empty());
}

TEST_CASE("shader library: a state variant needs its own name; the same desc on another device is accepted") {
    // Round-2 review mutant C11: a copy of a recorded desc with one state field changed, under the copied
    // name, would be bound under that name in the Null trace and count as covered by the original's golden.
    std::unique_ptr<rhi::Device> first = nullDevice();
    std::unique_ptr<rhi::Device> second = nullDevice();
    rhi::GraphicsPipelineDesc geo;
    geo.vertex = {shippedModule("forward"), "vsMain"};
    geo.fragment = {shippedModule("forward"), "psMain"};
    geo.raster.cullMode = rhi::CullMode::Back;
    geo.depth = {true, true, rhi::CompareOp::GreaterOrEqual};
    geo.colorCount = 1;
    geo.colorFormats[0] = rhi::Format::RGBA16Float;
    geo.depthFormat = rhi::Format::D32Float;
    geo.name = "Test.StateVariant";
    auto original = createShippedPipeline(*first, geo);
    REQUIRE(original.ok());

    // Every part of the state counts: one row per member of the fingerprint (review mutants FK1-FK6
    // each dropped one of them from the key).
    using Desc = rhi::GraphicsPipelineDesc;
    struct Variant {
        const char* field;
        void (*change)(Desc&);
    };
    const Variant variants[] = {
        {"topology", [](Desc& d) { d.topology = rhi::PrimitiveTopology::LineList; }},
        {"raster.cullMode", [](Desc& d) { d.raster.cullMode = rhi::CullMode::None; }},  // C11
        {"raster.frontFace", [](Desc& d) { d.raster.frontFace = rhi::FrontFace::Clockwise; }},
        {"raster.polygonMode", [](Desc& d) { d.raster.polygonMode = rhi::PolygonMode::Line; }},
        {"raster.depthClamp", [](Desc& d) { d.raster.depthClamp = true; }},
        {"raster.depthBiasConstant", [](Desc& d) { d.raster.depthBiasConstant = 1.0f; }},
        {"raster.depthBiasSlope", [](Desc& d) { d.raster.depthBiasSlope = 1.0f; }},
        {"raster.depthBiasClamp", [](Desc& d) { d.raster.depthBiasClamp = 1.0f; }},
        {"depth.testEnable", [](Desc& d) { d.depth.testEnable = false; }},
        {"depth.writeEnable", [](Desc& d) { d.depth.writeEnable = false; }},
        {"depth.compareOp", [](Desc& d) { d.depth.compareOp = rhi::CompareOp::Always; }},
        {"colorCount", [](Desc& d) { d.colorCount = 2; }},
        {"colorFormats[0]", [](Desc& d) { d.colorFormats[0] = rhi::Format::RGBA8Unorm; }},
        {"blend[0].enable", [](Desc& d) { d.blend[0].enable = true; }},
        {"blend[0].srcColor", [](Desc& d) { d.blend[0].srcColor = rhi::BlendFactor::SrcAlpha; }},
        {"blend[0].dstColor", [](Desc& d) { d.blend[0].dstColor = rhi::BlendFactor::One; }},
        {"blend[0].colorOp", [](Desc& d) { d.blend[0].colorOp = rhi::BlendOp::Max; }},
        {"blend[0].srcAlpha", [](Desc& d) { d.blend[0].srcAlpha = rhi::BlendFactor::Zero; }},
        {"blend[0].dstAlpha", [](Desc& d) { d.blend[0].dstAlpha = rhi::BlendFactor::One; }},
        {"blend[0].alphaOp", [](Desc& d) { d.blend[0].alphaOp = rhi::BlendOp::Min; }},
        {"blend[0].writeMask", [](Desc& d) { d.blend[0].writeMask = rhi::ColorWrite::R; }},
        {"depthFormat", [](Desc& d) { d.depthFormat = rhi::Format::D16Unorm; }},
        {"sampleCount", [](Desc& d) { d.sampleCount = 4; }},
    };
    for (const Variant& row : variants) {
        CAPTURE(row.field);
        rhi::GraphicsPipelineDesc variant = geo;
        row.change(variant);
        auto rejected = createShippedPipeline(*first, variant);
        CHECK(rejected.errorCode() == ErrorCode::InvalidArgument);
        if (!rejected) CHECK(rejected.error().message.find("other state") != std::string::npos);
    }
    // State of color attachments beyond colorCount is not part of the pipeline.
    rhi::GraphicsPipelineDesc unused = geo;
    unused.colorFormats[3] = rhi::Format::RGBA8Unorm;
    unused.blend[3] = rhi::BlendState::premultiplied();
    auto same = createShippedPipeline(*first, unused);
    REQUIRE(same.ok());

    // Every renderer builds its own pipelines: the identical desc on another device is the same pipeline.
    auto onSecond = createShippedPipeline(*second, geo);
    REQUIRE(onSecond.ok());
    CHECK(recordedStages("Test.StateVariant") == Stages{"forward:vsMain", "forward:psMain"});
    first->destroy(original.value());
    first->destroy(same.value());
    second->destroy(onSecond.value());

    // A variant with its own name is its own pipeline (and needs its own golden).
    rhi::GraphicsPipelineDesc twoSided = geo;
    twoSided.raster.cullMode = rhi::CullMode::None;
    twoSided.name = "Test.StateVariant.TwoSided";
    auto renamed = createShippedPipeline(*first, twoSided);
    REQUIRE(renamed.ok());
    first->destroy(renamed.value());
}

} // namespace
