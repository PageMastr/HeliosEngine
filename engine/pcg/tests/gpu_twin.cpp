#include "gpu_twin.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include "corpus.h"
#include "helios/rhi/utils.h"
#include HELIOS_PCG_TWIN_SHADERS_HEADER

namespace helios::pcg::test {

namespace {

struct TilePush {
    u64 program;
    u64 domains;
    u64 heights;
    u64 positions;
    u32 instrCount;
    u32 outputReg;
    u32 tileCount;
    u32 flags;
};
static_assert(sizeof(TilePush) == 48);

struct LatticePush {
    u64 cases;
    u64 results;
    u32 count;
    u32 pad;
};
static_assert(sizeof(LatticePush) == 24);

constexpr u32 kGroup = 64;

} // namespace

struct GpuTwin::Impl {
    std::unique_ptr<rhi::Device> device;
    rhi::PipelineH tilePipeline;
    rhi::PipelineH latticePipeline;

    ~Impl() {
        if (device) {
            (void)device->waitIdle();
            device->destroy(tilePipeline);
            device->destroy(latticePipeline);
            (void)device->waitIdle();
        }
    }

    Result<rhi::BufferH> uploadBuffer(std::span<const std::byte> bytes, std::string_view name) {
        HELIOS_TRY_ASSIGN(rhi::BufferH buf, device->createBuffer({.size = std::max<u64>(bytes.size(), 16),
                                                                  .usage = rhi::BufferUsage::Storage,
                                                                  .memory = rhi::MemoryUsage::Upload,
                                                                  .name = name}));
        void* mapped = device->map(buf);
        if (!mapped) return Error{ErrorCode::Unsupported, "upload buffer is not mappable"};
        std::memcpy(mapped, bytes.data(), bytes.size());
        device->flushMapped(buf);
        return buf;
    }

    Result<rhi::BufferH> outputBuffer(u64 size, std::string_view name) {
        return device->createBuffer({.size = std::max<u64>(size, 16),
                                     .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSrc,
                                     .memory = rhi::MemoryUsage::GpuOnly,
                                     .name = name});
    }

    Result<rhi::TimelinePoint> dispatchTiles(const TilePush& push, u32 tiles) {
        rhi::CommandList* cmd = device->acquireCommandList(rhi::Queue::Graphics, "hnoise tiles");
        if (!cmd) return Error{ErrorCode::InvalidState, "device lost"};
        cmd->bindPipeline(tilePipeline);
        cmd->pushConstants(push);
        cmd->dispatch((kTileSamples + kGroup - 1) / kGroup, std::max<u32>(tiles, 1));
        cmd->barrier(rhi::Barrier::global(rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource));
        return device->submit(rhi::Queue::Graphics, {&cmd, 1});
    }
};

Result<std::unique_ptr<GpuTwin>> GpuTwin::create(bool preferSoftware) {
    std::unique_ptr<GpuTwin> twin(new GpuTwin());
    twin->m_impl = std::make_unique<Impl>();
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.appName = "pcg_gpu_twin";
    desc.enableSwapchain = false;
    desc.validation = true; // used when the Khronos layer is installed
    desc.adapterPreference = preferSoftware ? rhi::AdapterPreference::Software : rhi::AdapterPreference::HighPerformance;
    HELIOS_TRY_ASSIGN(twin->m_impl->device, rhi::Device::create(desc));
    rhi::Device& dev = *twin->m_impl->device;
    const std::span<const u32> spirv = pcg_twin_shaders::hnoise_tile();
    HELIOS_TRY_ASSIGN(twin->m_impl->tilePipeline,
                      dev.createComputePipeline(rhi::ComputePipelineDesc{rhi::ShaderDesc{spirv, "csTile"}, "hnoise csTile"}));
    HELIOS_TRY_ASSIGN(twin->m_impl->latticePipeline,
                      dev.createComputePipeline(rhi::ComputePipelineDesc{rhi::ShaderDesc{spirv, "csLattice"}, "hnoise csLattice"}));
    return twin;
}

GpuTwin::~GpuTwin() = default;

const rhi::Caps& GpuTwin::caps() const noexcept { return m_impl->device->caps(); }

std::string GpuTwin::adapterDescription() const {
    const rhi::AdapterInfo& a = caps().adapter;
    return std::format("{} ({} {})", a.name, a.driverName, a.driverInfo);
}

u64 GpuTwin::validationErrors() const noexcept { return m_impl->device->validationErrorCount(); }

Result<void> GpuTwin::evaluateTiles(const TerrainProgram& program, std::span<const TileDomain> domains,
                                    std::vector<i64>& heights, std::vector<FixedPos>* positions) {
    rhi::Device& dev = *m_impl->device;
    const u32 tiles = static_cast<u32>(domains.size());
    // The shader masks register indices but cannot detect a read-before-write or a bad domain: the
    // CPU validates everything it uploads, exactly as TileEvaluator does.
    HELIOS_TRY(validateTerrainProgram(program));
    if (tiles == 0 || tiles > 65535) return Error{ErrorCode::InvalidArgument, "gpu twin: 1..65535 tiles per dispatch"};
    std::vector<u32> domainWords;
    for (const TileDomain& d : domains) {
        HELIOS_TRY(validateTileDomain(d));
        const auto w = d.pack();
        domainWords.insert(domainWords.end(), w.begin(), w.end());
    }
    HELIOS_TRY_ASSIGN(rhi::BufferH prog, m_impl->uploadBuffer(std::as_bytes(program.words()), "hnoise program"));
    HELIOS_TRY_ASSIGN(rhi::BufferH doms, m_impl->uploadBuffer(std::as_bytes(std::span<const u32>(domainWords)), "hnoise domains"));
    const u64 heightBytes = u64(tiles) * kTileSamples * sizeof(i64);
    const u64 posBytes = positions ? heightBytes * 3 : 16;
    HELIOS_TRY_ASSIGN(rhi::BufferH out, m_impl->outputBuffer(heightBytes, "hnoise heights"));
    HELIOS_TRY_ASSIGN(rhi::BufferH pos, m_impl->outputBuffer(posBytes, "hnoise positions"));
    const TilePush push{dev.deviceAddress(prog), dev.deviceAddress(doms), dev.deviceAddress(out), dev.deviceAddress(pos),
                        static_cast<u32>(program.code.size()), program.outputRegister, tiles, positions ? 1u : 0u};
    HELIOS_TRY_ASSIGN(const rhi::TimelinePoint done, m_impl->dispatchTiles(push, tiles));
    HELIOS_TRY_ASSIGN(const std::vector<u8> bytes,
                      rhi::readbackBuffer(dev, out, 0, heightBytes, rhi::ResourceState::CopySource, done));
    heights.resize(u64(tiles) * kTileSamples);
    std::memcpy(heights.data(), bytes.data(), heightBytes);
    if (positions) {
        HELIOS_TRY_ASSIGN(const std::vector<u8> pbytes,
                          rhi::readbackBuffer(dev, pos, 0, posBytes, rhi::ResourceState::CopySource, done));
        positions->resize(u64(tiles) * kTileSamples);
        std::vector<i64> raw(posBytes / sizeof(i64));
        std::memcpy(raw.data(), pbytes.data(), posBytes);
        for (usize i = 0; i < positions->size(); ++i) (*positions)[i] = {raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]};
    }
    dev.destroy(prog);
    dev.destroy(doms);
    dev.destroy(out);
    dev.destroy(pos);
    HELIOS_TRY(dev.waitIdle());
    return {};
}

Result<void> GpuTwin::evaluateLattice(std::span<const LatticeCase> cases, std::vector<u32>& hashes, std::vector<i32>& noises) {
    rhi::Device& dev = *m_impl->device;
    std::vector<u32> words;
    for (const LatticeCase& c : cases) {
        words.push_back(c.seed);
        for (i32 v : c.cell) words.push_back(static_cast<u32>(v));
        for (i32 v : c.frac) words.push_back(static_cast<u32>(v));
    }
    const u32 count = static_cast<u32>(cases.size());
    HELIOS_TRY_ASSIGN(rhi::BufferH in, m_impl->uploadBuffer(std::as_bytes(std::span<const u32>(words)), "hnoise lattice cases"));
    HELIOS_TRY_ASSIGN(rhi::BufferH out, m_impl->outputBuffer(u64(count) * 8, "hnoise lattice results"));
    rhi::CommandList* cmd = dev.acquireCommandList(rhi::Queue::Graphics, "hnoise lattice");
    if (!cmd) return Error{ErrorCode::InvalidState, "device lost"};
    cmd->bindPipeline(m_impl->latticePipeline);
    cmd->pushConstants(LatticePush{dev.deviceAddress(in), dev.deviceAddress(out), count, 0});
    cmd->dispatch((count + kGroup - 1) / kGroup);
    cmd->barrier(rhi::Barrier::global(rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource));
    HELIOS_TRY_ASSIGN(const rhi::TimelinePoint done, dev.submit(rhi::Queue::Graphics, {&cmd, 1}));
    HELIOS_TRY_ASSIGN(const std::vector<u8> bytes,
                      rhi::readbackBuffer(dev, out, 0, u64(count) * 8, rhi::ResourceState::CopySource, done));
    std::vector<u32> results(u64(count) * 2);
    std::memcpy(results.data(), bytes.data(), results.size() * sizeof(u32));
    hashes.resize(count);
    noises.resize(count);
    for (u32 i = 0; i < count; ++i) {
        hashes[i] = results[i * 2];
        noises[i] = static_cast<i32>(results[i * 2 + 1]);
    }
    dev.destroy(in);
    dev.destroy(out);
    HELIOS_TRY(dev.waitIdle());
    return {};
}

Result<GpuTiming> GpuTwin::timeTiles(const TerrainProgram& program, std::span<const TileDomain> domains, u32 repeats) {
    rhi::Device& dev = *m_impl->device;
    const u32 tiles = static_cast<u32>(domains.size());
    // The shader masks register indices but cannot detect a read-before-write or a bad domain: the
    // CPU validates everything it uploads, exactly as TileEvaluator does.
    HELIOS_TRY(validateTerrainProgram(program));
    if (tiles == 0 || tiles > 65535) return Error{ErrorCode::InvalidArgument, "gpu twin: 1..65535 tiles per dispatch"};
    std::vector<u32> domainWords;
    for (const TileDomain& d : domains) {
        HELIOS_TRY(validateTileDomain(d));
        const auto w = d.pack();
        domainWords.insert(domainWords.end(), w.begin(), w.end());
    }
    HELIOS_TRY_ASSIGN(rhi::BufferH prog, m_impl->uploadBuffer(std::as_bytes(program.words()), "hnoise program"));
    HELIOS_TRY_ASSIGN(rhi::BufferH doms, m_impl->uploadBuffer(std::as_bytes(std::span<const u32>(domainWords)), "hnoise domains"));
    HELIOS_TRY_ASSIGN(rhi::BufferH out, m_impl->outputBuffer(u64(tiles) * kTileSamples * sizeof(i64), "hnoise heights"));
    TilePush push{dev.deviceAddress(prog), dev.deviceAddress(doms), dev.deviceAddress(out), dev.deviceAddress(out),
                  static_cast<u32>(program.code.size()), program.outputRegister, tiles, 0u};
    const auto measure = [&](u32 tileCount) -> Result<f64> {
        push.tileCount = tileCount;
        std::vector<f64> times;
        for (u32 r = 0; r < repeats + 1; ++r) { // the first run warms up (pipeline, caches)
            const auto t0 = std::chrono::steady_clock::now();
            HELIOS_TRY_ASSIGN(const rhi::TimelinePoint done, m_impl->dispatchTiles(push, std::max<u32>(tileCount, 1)));
            HELIOS_TRY(dev.wait(done));
            const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r > 0) times.push_back(ms);
            HELIOS_TRY(dev.beginFrame());
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    };
    // tileCount 0: every invocation returns at once, so this is submit + dispatch overhead.
    HELIOS_TRY_ASSIGN(const f64 baseline, measure(0));
    HELIOS_TRY_ASSIGN(const f64 full, measure(tiles));
    dev.destroy(prog);
    dev.destroy(doms);
    dev.destroy(out);
    HELIOS_TRY(dev.waitIdle());
    GpuTiming t;
    t.tiles = tiles;
    t.baselineMs = baseline;
    t.msPerDispatch = std::max(full - baseline, 1e-6);
    return t;
}

} // namespace helios::pcg::test
