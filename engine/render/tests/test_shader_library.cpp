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
    CHECK(recordedStages("Forward.Tonemap") == Stages{"tonemap:vsFullscreen", "tonemap:psTonemap"});
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

} // namespace
