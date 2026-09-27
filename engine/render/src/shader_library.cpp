// Shipped shader modules and the pipeline record (see helios/render/shader_library.h).

#include "helios/render/shader_library.h"

#include <algorithm>
#include <bit>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <type_traits>

#include "helios_render_shaders.h"

namespace helios::render {

namespace {

/// What a pipeline name stands for: its stages and a fingerprint of every other part of its desc.
struct RecordedPipeline {
    std::vector<ShippedEntryPoint> entries;
    std::string state;
};

struct PipelineRecord {
    std::mutex mutex;
    std::map<std::string, RecordedPipeline, std::less<>> pipelines;
};

PipelineRecord& pipelineRecord() {
    static PipelineRecord record;
    return record;
}

/// Stem of the embedded module whose words `spirv` views (by address), or "" for any other SPIR-V.
std::string_view embeddedModule(std::span<const u32> spirv) noexcept {
    if (spirv.empty()) return {};
    for (std::string_view stem : helios_render_shaders::names()) {
        const std::span<const u32> words = helios_render_shaders::find(stem);
        if (words.data() == spirv.data() && words.size() == spirv.size()) return stem;
    }
    return {};
}

Result<std::vector<ShippedEntryPoint>> resolveStages(std::string_view pipeline,
                                                     std::initializer_list<const rhi::ShaderDesc*> stages) {
    if (pipeline.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "shipped pipelines need a name (helios-rendertest maps names to shaders)"};
    }
    std::vector<ShippedEntryPoint> out;
    for (const rhi::ShaderDesc* stage : stages) {
        // Names every member of ShaderDesc (see stateKey()): a new one fails to compile here.
        const auto& [spirv, entryPoint] = *stage;
        const std::string_view module = embeddedModule(spirv);
        if (module.empty()) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("pipeline '{}': the SPIR-V of '{}' is not a module embedded in helios_render",
                                     pipeline, entryPoint)};
        }
        HELIOS_TRY_ASSIGN(ShaderReflection reflection, reflectSpirv(spirv));
        const ShaderEntryPoint* entry = reflection.findEntryPoint(entryPoint);
        if (!entry) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("pipeline '{}': module '{}' has no entry point '{}'", pipeline, module,
                                     entryPoint)};
        }
        out.push_back({std::string(module), std::string(entryPoint), entry->stage});
    }
    return out;
}

u32 bits(f32 value) noexcept { return std::bit_cast<u32>(value); }

// stateKey() must see every member of the descs except the shaders (recorded as entry points) and the
// name (the record's key). Its structured bindings name every non-static data member of each desc and
// state struct, so a new member, also one that fits in padding, fails to compile until it is added
// here and to the key.

/// Fingerprint of a graphics desc without its shaders and name: topology, raster, depth, attachments
/// (formats and blend of each used color attachment, depth format) and sample count.
std::string stateKey(const rhi::GraphicsPipelineDesc& d) {
    [[maybe_unused]] const auto& [vertex, fragment, topology, raster, depth, colorCount, colorFormats, blend,
                                  depthFormat, sampleCount, name] = d;
    const auto& [cullMode, frontFace, polygonMode, depthClamp, depthBiasConstant, depthBiasSlope,
                 depthBiasClamp] = raster;
    const auto& [testEnable, writeEnable, compareOp] = depth;
    std::string key = std::format("graphics topology={} cull={} front={} fill={} clamp={} bias={:08x}/{:08x}/{:08x} "
                                  "depth={}{}{} depthFormat={} samples={} colors={}",
                                  static_cast<u32>(topology), static_cast<u32>(cullMode),
                                  static_cast<u32>(frontFace), static_cast<u32>(polygonMode), depthClamp,
                                  bits(depthBiasConstant), bits(depthBiasSlope), bits(depthBiasClamp), testEnable,
                                  writeEnable, static_cast<u32>(compareOp), static_cast<u32>(depthFormat),
                                  sampleCount, colorCount);
    for (u32 i = 0; i < colorCount && i < rhi::kMaxColorAttachments; ++i) {
        const auto& [enable, srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp, writeMask] = blend[i];
        key += std::format(" [{} blend={} {}/{}/{} {}/{}/{} mask={}]", static_cast<u32>(colorFormats[i]), enable,
                           static_cast<u32>(srcColor), static_cast<u32>(dstColor), static_cast<u32>(colorOp),
                           static_cast<u32>(srcAlpha), static_cast<u32>(dstAlpha), static_cast<u32>(alphaOp),
                           static_cast<u32>(writeMask));
    }
    return key;
}
/// A compute desc has nothing but its shader and name.
std::string stateKey(const rhi::ComputePipelineDesc& d) {
    [[maybe_unused]] const auto& [compute, name] = d;
    return "compute";
}

/// InvalidArgument when `name` is recorded with other entry points or other state (a variant must
/// have its own name, so the Null trace tells it apart). Caller holds the record's mutex.
Result<void> checkUnchanged(const PipelineRecord& record, std::string_view name, const RecordedPipeline& pipeline) {
    const auto it = record.pipelines.find(name);
    if (it == record.pipelines.end()) return {};
    if (it->second.entries != pipeline.entries) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("pipeline name '{}' is already recorded with other shaders", name)};
    }
    if (it->second.state != pipeline.state) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("pipeline name '{}' is already recorded with other state ({} before, {} now)", name,
                                 it->second.state, pipeline.state)};
    }
    return {};
}

/// Checks the name before creation (so a conflict creates nothing) and records it afterwards; a
/// concurrent conflicting creation found at that point destroys the new pipeline.
template <class Desc>
Result<rhi::PipelineH> createRecorded(rhi::Device& device, const Desc& desc, rhi::PsoPriority priority,
                                      std::vector<ShippedEntryPoint> entries) {
    PipelineRecord& record = pipelineRecord();
    RecordedPipeline recorded{std::move(entries), stateKey(desc)};
    {
        std::lock_guard lock(record.mutex);
        HELIOS_TRY(checkUnchanged(record, desc.name, recorded));
    }
    rhi::PipelineH pipeline;
    if constexpr (std::is_same_v<Desc, rhi::GraphicsPipelineDesc>) {
        HELIOS_TRY_ASSIGN(pipeline, device.createGraphicsPipeline(desc, priority));
    } else {
        HELIOS_TRY_ASSIGN(pipeline, device.createComputePipeline(desc, priority));
    }
    std::lock_guard lock(record.mutex);
    if (auto unchanged = checkUnchanged(record, desc.name, recorded); !unchanged) {
        device.destroy(pipeline);
        return unchanged.error();
    }
    record.pipelines.try_emplace(std::string(desc.name), std::move(recorded));
    return pipeline;
}

} // namespace

std::span<const u32> shippedModule(std::string_view stem) noexcept { return helios_render_shaders::find(stem); }

Result<std::vector<ShippedEntryPoint>> shippedEntryPoints() {
    std::vector<ShippedEntryPoint> out;
    for (std::string_view stem : helios_render_shaders::names()) {
        auto reflection = reflectSpirv(helios_render_shaders::find(stem));
        if (!reflection) {
            return Error{ErrorCode::Corrupt,
                         std::format("embedded module '{}' does not reflect: {}", stem, reflection.error().toString())};
        }
        for (const ShaderEntryPoint& e : reflection->entryPoints) out.push_back({std::string(stem), e.name, e.stage});
    }
    std::sort(out.begin(), out.end(), [](const ShippedEntryPoint& a, const ShippedEntryPoint& b) {
        return a.module != b.module ? a.module < b.module : a.entryPoint < b.entryPoint;
    });
    return out;
}

std::vector<ShippedPipeline> shippedPipelines() {
    PipelineRecord& record = pipelineRecord();
    std::lock_guard lock(record.mutex);
    std::vector<ShippedPipeline> out;
    out.reserve(record.pipelines.size());
    for (const auto& [name, recorded] : record.pipelines) out.push_back({name, recorded.entries});
    return out;
}

Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::GraphicsPipelineDesc& desc,
                                             rhi::PsoPriority priority) {
    HELIOS_TRY_ASSIGN(std::vector<ShippedEntryPoint> entries,
                      desc.fragment.spirv.empty() ? resolveStages(desc.name, {&desc.vertex})
                                                  : resolveStages(desc.name, {&desc.vertex, &desc.fragment}));
    return createRecorded(device, desc, priority, std::move(entries));
}

Result<rhi::PipelineH> createShippedPipeline(rhi::Device& device, const rhi::ComputePipelineDesc& desc,
                                             rhi::PsoPriority priority) {
    HELIOS_TRY_ASSIGN(std::vector<ShippedEntryPoint> entries, resolveStages(desc.name, {&desc.compute}));
    return createRecorded(device, desc, priority, std::move(entries));
}

} // namespace helios::render
