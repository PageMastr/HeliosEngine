// pcg_hnoise_bench — the WP-0.9c hnoise throughput spike (03 §5.5a; docs/adr/ADR-0.9c-hnoise-throughput.md).
//
//   pcg_hnoise_bench [--tiles=N] [--gpu-tiles=N] [--repeats=N] [--no-gpu] [--hardware-gpu] [--allow-non-avx2]
//                    [--json=PATH]
//
// CPU: the reference 40-node graph, ms per 65x65 tile per core (one thread) on every supported kernel,
// at full detail (every octave: collision levels) and level-adaptive (a level-10 render tile of a 1,500 km body).
// The budgeted number is the AVX2 kernel's; the run is invalid (exit code 2) unless the dispatched
// kernel logged pcg.kernel=avx2 (02 §5.8 measurement guard). The SSE4.2 twin is reported for
// information and must hash identically. GPU (graphics builds): tiles per 0.8 ms on the Vulkan adapter
// (lavapipe in CI: throughput for the CI budget only, never a performance claim), after a conformance
// check of the timed batch against the CPU. The GPU defaults to the software adapter (CI); pass
// --hardware-gpu on the win-gpu runner and the MIN boxes (HELIOS_RHI_ADAPTER picks a specific one).
// GPU time is the wall time of submit + wait for one dispatch minus an empty dispatch's (the RHI has no
// timestamp queries yet), so it slightly overstates the kernel time.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/cpu.h"
#include "helios/pcg/pcg.h"

#if defined(HELIOS_PCG_BENCH_GPU)
#include "gpu_twin.h"
#endif

using namespace helios;
using namespace helios::pcg;

namespace {

constexpr f64 kRadius1500km = 1'500'000.0;

struct Args {
    u32 tiles = 32;
    u32 gpuTiles = 64;
    u32 repeats = 5;
    bool gpu = true;
    bool hardwareGpu = false;
    bool allowNonAvx2 = false;
    std::string json;
};

bool parseU32(std::string_view text, u32& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(std::string(text).c_str(), &end, 10);
    if (!end || *end != '\0' || v == 0 || v > 100000) return false;
    out = static_cast<u32>(v);
    return true;
}

std::vector<TileDomain> collisionTiles(u32 n) {
    std::vector<TileDomain> out;
    for (u32 t = 0; t < n; ++t) {
        out.push_back(TileDomain::cubeSphere({static_cast<CubeFace>(t % 6), 15, 4000 + 131 * t, 9000 + 71 * t}, kRadius1500km));
    }
    return out;
}

std::vector<TileDomain> renderTiles(u32 n, u8 level) {
    std::vector<TileDomain> out;
    const u32 res = 1u << level;
    for (u32 t = 0; t < n; ++t) {
        out.push_back(TileDomain::cubeSphere({static_cast<CubeFace>(t % 6), level, (37 * t + 5) % res, (53 * t + 11) % res},
                                             kRadius1500km));
    }
    return out;
}

/// JSON string body: quotes, backslashes and control characters escaped (adapter and CPU names are
/// driver-provided text).
std::string jsonEscape(std::string_view text) {
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out;
}

struct CpuResult {
    KernelKind kernel;
    f64 medianMs = 0.0;
    f64 meanMs = 0.0;
    u64 hash = 0; // combined hash of every tile
};

CpuResult measureCpu(KernelKind kernel, const TerrainProgram& program, const std::vector<TileDomain>& tiles) {
    TileEvaluator eval(kernel);
    std::vector<i64> h(kTileSamples);
    (void)eval.evaluate(program, tiles.front(), h); // warm-up
    std::vector<f64> times;
    u64 combined = 0x9E3779B97F4A7C15ull;
    for (const TileDomain& d : tiles) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!eval.evaluate(program, d, h).ok()) std::abort();
        times.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
        combined = combined * 0x100000001B3ull ^ hashHeights(h);
    }
    CpuResult r{kernel};
    r.hash = combined;
    for (f64 t : times) r.meanMs += t;
    r.meanMs /= static_cast<f64>(times.size());
    std::sort(times.begin(), times.end());
    r.medianMs = times[times.size() / 2];
    return r;
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.starts_with("--tiles=") && parseU32(a.substr(8), args.tiles)) continue;
        if (a.starts_with("--gpu-tiles=") && parseU32(a.substr(12), args.gpuTiles)) continue;
        if (a.starts_with("--repeats=") && parseU32(a.substr(10), args.repeats)) continue;
        if (a == "--no-gpu") {
            args.gpu = false;
            continue;
        }
        if (a == "--hardware-gpu") {
            args.hardwareGpu = true;
            continue;
        }
        if (a == "--allow-non-avx2") {
            args.allowNonAvx2 = true;
            continue;
        }
        if (a.starts_with("--json=")) {
            args.json = std::string(a.substr(7));
            continue;
        }
        std::fprintf(stderr, "usage: pcg_hnoise_bench [--tiles=N] [--gpu-tiles=N] [--repeats=N] [--no-gpu] "
                             "[--hardware-gpu] [--allow-non-avx2] [--json=PATH]\n");
        return 1;
    }

    const CpuGateReport& cpu = cpuGate();
    const KernelKind dispatched = initializePcgModule(); // logs pcg.kernel=<name>
    std::printf("hnoise throughput spike (WP-0.9c)\nCPU: %s\npcg.kernel=%s (dispatched)\n", cpu.brand.c_str(),
                std::string(kernelName(dispatched)).c_str());
    if (dispatched != KernelKind::Avx2 && !args.allowNonAvx2) {
        std::fprintf(stderr, "INVALID RUN: the dispatched kernel is %s, not avx2 (02 §5.8 measurement guard)\n",
                     std::string(kernelName(dispatched)).c_str());
        return 2;
    }

    const TerrainGraph graph = makeReferenceGraph40();
    const TerrainProgram full = compileTerrainGraph(graph, {.collisionLevel = true}).value();
    const std::vector<TileDomain> fullTiles = collisionTiles(args.tiles);
    const u8 renderLevel = 10;
    CompileOptions adaptiveOptions;
    adaptiveOptions.sampleSpacingMetres = renderTiles(1, renderLevel).front().sampleSpacingMetres();
    const TerrainProgram adaptive = compileTerrainGraph(graph, adaptiveOptions).value();
    const std::vector<TileDomain> adaptiveTiles = renderTiles(args.tiles, renderLevel);
    std::printf("reference graph: %zu nodes, %zu instructions; full detail %u noise evaluations/sample; "
                "level %u (%.1f m spacing) level-adaptive %u (%u octaves skipped, %.3f m)\n",
                graph.nodeCount(), full.code.size(), full.evaluatedOctaves, renderLevel,
                adaptiveOptions.sampleSpacingMetres, adaptive.evaluatedOctaves, adaptive.skippedOctaves,
                adaptive.skippedAmplitudeMetres);

    std::vector<CpuResult> fullResults, adaptiveResults;
    for (KernelKind k : {KernelKind::Avx2, KernelKind::Sse42, KernelKind::Scalar}) {
        if (!kernelSupported(k)) continue;
        // The scalar reference is ~8x slower: fewer tiles keep the smoke run short.
        const u32 n = k == KernelKind::Scalar ? std::max<u32>(1, args.tiles / 4) : args.tiles;
        const std::vector<TileDomain> f(fullTiles.begin(), fullTiles.begin() + n);
        const std::vector<TileDomain> a(adaptiveTiles.begin(), adaptiveTiles.begin() + n);
        fullResults.push_back(measureCpu(k, full, f));
        adaptiveResults.push_back(measureCpu(k, adaptive, a));
    }

    std::printf("\nCPU VM, ms per 65x65 tile per core (median / mean over %u tiles):\n", args.tiles);
    std::printf("  %-8s %22s %22s\n", "kernel", "full detail", "level-adaptive");
    for (usize i = 0; i < fullResults.size(); ++i) {
        std::printf("  %-8s %10.3f / %-9.3f %10.3f / %-9.3f\n", std::string(kernelName(fullResults[i].kernel)).c_str(),
                    fullResults[i].medianMs, fullResults[i].meanMs, adaptiveResults[i].medianMs, adaptiveResults[i].meanMs);
    }

    // Twin hash equality: every kernel over the same tiles (the scalar kernel only covers a prefix, so
    // compare it separately on that prefix).
    bool twinsEqual = true;
    {
        const u32 prefix = std::max<u32>(1, args.tiles / 4);
        const std::vector<TileDomain> f(fullTiles.begin(), fullTiles.begin() + prefix);
        u64 first = 0;
        bool haveFirst = false;
        for (KernelKind k : {KernelKind::Avx2, KernelKind::Sse42, KernelKind::Scalar}) {
            if (!kernelSupported(k)) continue;
            const u64 h = measureCpu(k, full, f).hash;
            if (!haveFirst) {
                first = h;
                haveFirst = true;
            } else if (h != first) {
                twinsEqual = false;
            }
            std::printf("  twin hash %-6s %016llx\n", std::string(kernelName(k)).c_str(), static_cast<unsigned long long>(h));
        }
        std::printf("kernel twins hash identically: %s\n", twinsEqual ? "yes" : "NO");
    }

    const f64 avx2Full = !fullResults.empty() && fullResults.front().kernel == KernelKind::Avx2 ? fullResults.front().medianMs : -1.0;
    const bool cpuGreen = avx2Full >= 0.0 && avx2Full <= 0.5;
    std::printf("CPU clause (<= 0.5 ms per tile per core, AVX2, full detail): %.3f ms -> %s\n", avx2Full,
                cpuGreen ? "green" : "RED (F3 trigger)");

    std::string gpuJson = "null";
#if defined(HELIOS_PCG_BENCH_GPU)
    if (args.gpu) {
        auto twin = test::GpuTwin::create(!args.hardwareGpu);
        if (!twin.ok()) {
            std::printf("GPU: unavailable (%s)\n", twin.error().toString().c_str());
        } else {
            test::GpuTwin& gpu = **twin;
            std::printf("\nGPU adapter: %s\n", gpu.adapterDescription().c_str());
            // Conformance of the timed workload first.
            const std::vector<TileDomain> batch = collisionTiles(args.gpuTiles);
            std::vector<i64> gpuHeights;
            if (!gpu.evaluateTiles(full, batch, gpuHeights).ok()) {
                std::fprintf(stderr, "GPU tile evaluation failed\n");
                return 3;
            }
            TileEvaluator eval(dispatched);
            std::vector<i64> h(kTileSamples);
            bool conform = true;
            for (usize t = 0; t < batch.size(); ++t) {
                (void)eval.evaluate(full, batch[t], h);
                conform = conform && std::equal(h.begin(), h.end(), gpuHeights.begin() + static_cast<isize>(t * kTileSamples));
            }
            std::printf("GPU batch of %u tiles bit-identical to the CPU: %s\n", args.gpuTiles, conform ? "yes" : "NO");
            auto tf = gpu.timeTiles(full, batch, args.repeats);
            auto ta = gpu.timeTiles(adaptive, renderTiles(args.gpuTiles, renderLevel), args.repeats);
            if (tf.ok() && ta.ok()) {
                std::printf("GPU: %u tiles per dispatch: full detail %.2f ms (%.2f tiles per 0.8 ms), level-adaptive "
                            "%.2f ms (%.2f tiles per 0.8 ms); empty-dispatch overhead %.3f ms subtracted\n",
                            args.gpuTiles, tf->msPerDispatch, tf->tilesPer0_8ms(), ta->msPerDispatch, ta->tilesPer0_8ms(),
                            tf->baselineMs);
                gpuJson = "{\"adapter\": \"" + jsonEscape(gpu.adapterDescription()) + "\", \"tilesPerDispatch\": " +
                          std::to_string(args.gpuTiles) + ", \"fullMs\": " + std::to_string(tf->msPerDispatch) +
                          ", \"fullTilesPer0_8ms\": " + std::to_string(tf->tilesPer0_8ms()) +
                          ", \"adaptiveMs\": " + std::to_string(ta->msPerDispatch) +
                          ", \"adaptiveTilesPer0_8ms\": " + std::to_string(ta->tilesPer0_8ms()) +
                          ", \"conformant\": " + (conform ? "true" : "false") + "}";
            }
            if (!conform) return 3;
        }
    }
#endif

    if (!args.json.empty()) {
        if (FILE* f = std::fopen(args.json.c_str(), "wb")) {
            std::fprintf(f, "{\n  \"cpu\": \"%s\",\n  \"kernel\": \"%s\",\n  \"tiles\": %u,\n", jsonEscape(cpu.brand).c_str(),
                         std::string(kernelName(dispatched)).c_str(), args.tiles);
            std::fprintf(f, "  \"cpuMsPerTile\": {");
            for (usize i = 0; i < fullResults.size(); ++i) {
                std::fprintf(f, "%s\"%s\": {\"full\": %.4f, \"adaptive\": %.4f}", i ? ", " : "",
                             std::string(kernelName(fullResults[i].kernel)).c_str(), fullResults[i].medianMs,
                             adaptiveResults[i].medianMs);
            }
            std::fprintf(f, "},\n  \"twinsEqual\": %s,\n  \"gpu\": %s\n}\n", twinsEqual ? "true" : "false", gpuJson.c_str());
            std::fclose(f);
        }
    }
    return twinsEqual ? 0 : 4;
}
