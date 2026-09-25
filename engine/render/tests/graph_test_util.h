#pragma once
// Helpers shared by the render-graph tests: plan lookups, compact barrier text and a Null device.

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/log.h"
#include "helios/render/render_graph.h"
#include "helios/rhi/null_device.h"
#include "helios/rhi/rhi.h"

namespace graphtest {

using namespace helios;
using namespace helios::render;

inline u32 passIndex(const RgPlan& plan, std::string_view name) {
    for (u32 p = 0; p < plan.passes.size(); ++p) {
        if (plan.passes[p].name == name) return p;
    }
    FAIL("no pass named " << std::string(name));
    return kRgInvalid;
}

inline const RgPassInfo& pass(const RgPlan& plan, std::string_view name) { return plan.passes[passIndex(plan, name)]; }

inline u32 resourceIndex(const RgPlan& plan, std::string_view name) {
    for (u32 r = 0; r < plan.resources.size(); ++r) {
        if (plan.resources[r].name == name) return r;
    }
    FAIL("no resource named " << std::string(name));
    return kRgInvalid;
}

inline const RgResourceInfo& resource(const RgPlan& plan, std::string_view name) {
    return plan.resources[resourceIndex(plan, name)];
}

/// "Name:Before->After" (+ " m<base>+<count> l<base>+<count>" when not the whole resource),
/// physicals named after their first resident.
inline std::string barrierText(const RgPlan& plan, const RgBarrier& b) {
    const RgPhysicalInfo& phys = plan.physicals[b.physical];
    std::string text = plan.resources[phys.residents.front()].name + ":" +
                       (b.before == kRgEntryState ? std::string("entry") : std::string(rhi::resourceStateName(b.before))) +
                       "->" + std::string(rhi::resourceStateName(b.after));
    if (phys.isTexture && (b.range.mipCount != phys.texture.mipLevels || b.range.layerCount != phys.texture.arrayLayers)) {
        text += std::format(" m{}+{} l{}+{}", b.range.baseMip, b.range.mipCount, b.range.baseLayer, b.range.layerCount);
    }
    return text;
}

inline std::string barriersText(const RgPlan& plan, std::span<const RgBarrier> list) {
    std::string out;
    for (const RgBarrier& b : list) {
        if (!out.empty()) out += ", ";
        out += barrierText(plan, b);
    }
    return out;
}

inline std::string pre(const RgPlan& plan, std::string_view passName) {
    return barriersText(plan, pass(plan, passName).preBarriers);
}
inline std::string post(const RgPlan& plan, std::string_view passName) {
    return barriersText(plan, pass(plan, passName).postBarriers);
}

/// Names of the passes of every batch: "G[A,B] A[C]<-0 G[D]<-1" (queue initial, waits after "<-").
inline std::string batchesText(const RgPlan& plan) {
    std::string out;
    for (const RgBatchInfo& b : plan.batches) {
        if (!out.empty()) out += ' ';
        out += b.queue == rhi::Queue::Graphics ? 'G' : (b.queue == rhi::Queue::AsyncCompute ? 'A' : 'T');
        if (b.kind == RgBatchInfo::Kind::Prologue) out += "(prologue)";
        if (b.kind == RgBatchInfo::Kind::Epilogue) out += "(epilogue)";
        out += '[';
        for (usize i = 0; i < b.passes.size(); ++i) {
            if (i) out += ',';
            out += plan.passes[b.passes[i]].name;
        }
        out += ']';
        if (!b.waits.empty()) {
            out += "<-";
            for (usize i = 0; i < b.waits.size(); ++i) out += (i ? "," : "") + std::to_string(b.waits[i]);
        }
    }
    return out;
}

inline bool hasError(const RenderGraph& graph, std::string_view needle) {
    return std::any_of(graph.errors().begin(), graph.errors().end(),
                       [&](const std::string& e) { return e.find(needle) != std::string::npos; });
}

struct NullDeviceFixture {
    std::unique_ptr<rhi::Device> device;
    rhi::NullDevice* null = nullptr;

    NullDeviceFixture() {
        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Null;
        device = rhi::Device::create(desc).value();
        null = rhi::NullDevice::from(*device);
        REQUIRE(null != nullptr);
    }
    std::string errors() const {
        std::string out;
        for (const std::string& e : null->validationErrors()) out += "\n  " + e;
        return out;
    }
    usize errorCount() const { return null->validationErrors().size(); }

    rhi::TextureH texture(std::string_view name, rhi::Format format, u32 w, u32 h, rhi::TextureUsage usage, u32 mips = 1,
                          u32 layers = 1) {
        rhi::TextureDesc d = rhi::TextureDesc::tex2D(format, w, h, usage, name, mips);
        d.arrayLayers = layers;
        return device->createTexture(d).value();
    }
    rhi::BufferH buffer(std::string_view name, u64 size, rhi::BufferUsage usage) {
        return device->createBuffer({.size = size, .usage = usage, .name = name}).value();
    }
};

/// Silences the RenderGraph log channel while a negative test provokes errors on purpose.
class QuietLog {
public:
    QuietLog() { log::setChannelLevel("RenderGraph", log::Level::Off); }
    ~QuietLog() { log::clearChannelLevel("RenderGraph"); }
    QuietLog(const QuietLog&) = delete;
    QuietLog& operator=(const QuietLog&) = delete;
};

#define CHECK_NULL_CLEAN(f)          \
    do {                             \
        INFO((f).errors());          \
        CHECK((f).errorCount() == 0); \
    } while (false)

inline constexpr rhi::TextureUsage kAllColorUsage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage |
                                                    rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc |
                                                    rhi::TextureUsage::TransferDst;

} // namespace graphtest
