#pragma once
// helios-rendertest harness (03 §8.4): renders scenes offscreen on Vulkan (lavapipe in CI) or the
// Null backend, twice (serial, then parallel recording; results must be bit-identical, which
// catches missing barriers), and compares them with goldens — PNGs scored with ꟻLIP on Vulkan,
// command-stream traces on Null. Writes per-scene JSON results and an HTML/Markdown report.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/rhi/device.h"
#include "scenes.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::rendertest {

enum class Backend { Vulkan, Null };
std::string_view backendName(Backend backend) noexcept;

struct RunOptions {
    Backend backend = Backend::Vulkan;
    std::filesystem::path goldenDir;
    std::filesystem::path outDir;
    bool updateGoldens = false;
    bool validation = false;  ///< Khronos validation (when installed); errors fail the scene.
    jobs::JobSystem* jobs = nullptr;
};

struct SceneResult {
    std::string scene;
    std::string backend;
    std::string status;   ///< "pass", "fail", "updated" or "skip".
    std::string message;
    std::string adapter;  ///< Adapter and driver (Vulkan).
    std::string goldenKey;  ///< Golden set used ("vulkan-llvmpipe", "null").
    f64 meanFlip = 0.0;
    f64 maxFlip = 0.0;
    f64 maxMeanFlip = 0.0;  ///< Thresholds applied.
    f64 maxAllowedFlip = 0.0;
    bool renderedTwiceIdentical = false;
    u64 differentPixels = 0;  ///< Pixels not bit-identical to the golden.
    f64 milliseconds = 0.0;
    std::string actualFile;  ///< Relative to the output directory.
    std::string goldenFile;
    std::string flipFile;
    bool passed() const noexcept { return status == "pass" || status == "updated" || status == "skip"; }
};

/// A device of `backend` for the tests (Vulkan prefers a software adapter so every machine compares
/// against the same lavapipe goldens; HELIOS_RHI_ADAPTER overrides).
Result<std::unique_ptr<rhi::Device>> createTestDevice(Backend backend, bool validation);

/// Runs one scene on `device`: init, render twice, compare with the golden (or update it), write
/// artifacts and `<out>/<backend>/<scene>.json`.
SceneResult runScene(Scene& scene, rhi::Device& device, const RunOptions& options);

Result<void> writeResult(const SceneResult& result, const std::filesystem::path& file);
Result<SceneResult> readResult(const std::filesystem::path& file);
/// Collects every `<out>/*/*.json` result.
std::vector<SceneResult> collectResults(const std::filesystem::path& outDir);
/// Writes `<out>/report.html` and `<out>/report.md`. Returns false when a result failed or the
/// suite exceeded `budgetSeconds` (RC-1: <= 10 min).
bool writeReport(const std::vector<SceneResult>& results, const std::filesystem::path& outDir, f64 budgetSeconds,
                 std::string& summary);

} // namespace helios::rendertest
