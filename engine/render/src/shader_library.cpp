// Shipped shader modules and the pipeline record (see helios/render/shader_library.h).

#include "helios/render/shader_library.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <map>
#include <mutex>
#include <type_traits>

#include "helios_render_shaders.h"

namespace helios::render {

namespace {

struct PipelineRecord {
    std::mutex mutex;
    std::map<std::string, std::vector<ShippedEntryPoint>, std::less<>> pipelines;
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
        const std::string_view module = embeddedModule(stage->spirv);
        if (module.empty()) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("pipeline '{}': the SPIR-V of '{}' is not a module embedded in helios_render",
                                     pipeline, stage->entryPoint)};
        }
        HELIOS_TRY_ASSIGN(ShaderReflection reflection, reflectSpirv(stage->spirv));
        const ShaderEntryPoint* entry = reflection.findEntryPoint(stage->entryPoint);
        if (!entry) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("pipeline '{}': module '{}' has no entry point '{}'", pipeline, module,
                                     stage->entryPoint)};
        }
        out.push_back({std::string(module), std::string(stage->entryPoint), entry->stage});
    }
    return out;
}

/// InvalidArgument when `name` is recorded with other entry points. Caller holds the record's mutex.
Result<void> checkUnchanged(const PipelineRecord& record, std::string_view name,
                            const std::vector<ShippedEntryPoint>& entries) {
    const auto it = record.pipelines.find(name);
    if (it != record.pipelines.end() && it->second != entries) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("pipeline name '{}' is already recorded with other shaders", name)};
    }
    return {};
}

/// Checks the name before creation (so a conflict creates nothing) and records it afterwards; a
/// concurrent conflicting creation found at that point destroys the new pipeline.
template <class Desc>
Result<rhi::PipelineH> createRecorded(rhi::Device& device, const Desc& desc, rhi::PsoPriority priority,
                                      std::vector<ShippedEntryPoint> entries) {
    PipelineRecord& record = pipelineRecord();
    {
        std::lock_guard lock(record.mutex);
        HELIOS_TRY(checkUnchanged(record, desc.name, entries));
    }
    rhi::PipelineH pipeline;
    if constexpr (std::is_same_v<Desc, rhi::GraphicsPipelineDesc>) {
        HELIOS_TRY_ASSIGN(pipeline, device.createGraphicsPipeline(desc, priority));
    } else {
        HELIOS_TRY_ASSIGN(pipeline, device.createComputePipeline(desc, priority));
    }
    std::lock_guard lock(record.mutex);
    if (auto unchanged = checkUnchanged(record, desc.name, entries); !unchanged) {
        device.destroy(pipeline);
        return unchanged.error();
    }
    record.pipelines.try_emplace(std::string(desc.name), std::move(entries));
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
    for (const auto& [name, entries] : record.pipelines) out.push_back({name, entries});
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
