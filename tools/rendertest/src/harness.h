#pragma once
// helios-rendertest harness (03 §8.4): renders scenes offscreen on Vulkan (lavapipe in CI) or the
// Null backend, twice (serial, then parallel recording; results must be bit-identical, which
// catches missing barriers), and compares them with goldens — PNGs scored with ꟻLIP on Vulkan,
// command-stream traces on Null. Writes per-scene JSON results and an HTML/Markdown report.

#include <filesystem>
#include <memory>
#include <span>
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
    /// Fail instead of running without the layer (DeviceDesc::requireValidation).
    bool requireValidation = false;
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
    bool validation = false;  ///< Vulkan: the Khronos validation layer checked the renders.
    u64 differentPixels = 0;  ///< Pixels not bit-identical to the golden.
    f64 milliseconds = 0.0;
    std::string actualFile;  ///< Relative to the output directory.
    std::string goldenFile;
    std::string flipFile;
    bool passed() const noexcept { return status == "pass" || status == "updated" || status == "skip"; }
};

/// A device of `backend` for the tests (Vulkan prefers a software adapter so every machine compares
/// against the same lavapipe goldens; HELIOS_RHI_ADAPTER overrides). With `requireValidation`,
/// creation fails when the Khronos validation layer does not load. `validationFromEnvironment` false
/// ignores HELIOS_RHI_VALIDATION (DeviceDesc::validationFromEnvironment).
Result<std::unique_ptr<rhi::Device>> createTestDevice(Backend backend, bool validation, bool requireValidation = false,
                                                      bool validationFromEnvironment = true);

/// Seeded validation error (RC-1's goldens run under the layer): on a Vulkan device that requires
/// the Khronos layer, records a barrier whose source state is wrong (the texture is in CopyDest, the
/// barrier claims ShaderResource) and submits it. The Vulkan RHI does not track states, so only the
/// layer can report it. Returns the layer's name and version and the first message it reported;
/// fails when device creation fails, the layer reports nothing, or the device's
/// validationErrorCount() (what fails a golden) did not count it.
struct ValidationSelfTest {
    std::string layer;    ///< Caps::validationLayer.
    std::string message;  ///< First "Validation Error: [ ... ]" message, trimmed.
    u64 errors = 0;
};
Result<ValidationSelfTest> runValidationSelfTest();

/// Runs one scene on `device`: init, render twice, compare with the golden (or update it), write
/// artifacts and `<out>/<backend>/<scene>.json`.
SceneResult runScene(Scene& scene, rhi::Device& device, const RunOptions& options);

Result<void> writeResult(const SceneResult& result, const std::filesystem::path& file);
Result<SceneResult> readResult(const std::filesystem::path& file);
/// RC-1 coverage (helios/render/shader_library.h): the golden scenes that bind each shipped shader
/// entry point and each shipped pipeline in their captured frame.
struct CoverageItem {
    std::string name;                 ///< "forward:vsMain (vertex)" or a pipeline name.
    std::vector<std::string> scenes;  ///< Covering scenes, sorted; empty = not covered.
};
struct CoverageReport {
    std::vector<CoverageItem> entryPoints;
    std::vector<CoverageItem> pipelines;
    std::vector<std::string> problems;  ///< Uncovered items and scenes without committed goldens.
    bool ok() const noexcept { return problems.empty(); }
};

/// Renders each scene once on a fresh Null device (no GPU) and maps the pipelines bound in its
/// captured frame to shipped entry points. A scene counts only when both of its goldens are
/// committed: `<goldenDir>/vulkan-llvmpipe/<scene>.png` and `<goldenDir>/null/<scene>.txt`. Also
/// fails when a scene's own pipeline (createLocalPipeline) reuses a shipped pipeline's name.
Result<CoverageReport> measureCoverage(std::span<Scene* const> scenes, const std::filesystem::path& goldenDir);

/// Collects every `<out>/*/*.json` result.
std::vector<SceneResult> collectResults(const std::filesystem::path& outDir);
/// Writes `<out>/report.html` and `<out>/report.md`. Returns false when a result failed or the
/// suite exceeded `budgetSeconds` (RC-1: <= 10 min).
bool writeReport(const std::vector<SceneResult>& results, const std::filesystem::path& outDir, f64 budgetSeconds,
                 std::string& summary);

} // namespace helios::rendertest
