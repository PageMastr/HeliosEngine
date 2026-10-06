// helios-rendertest command line (see tools/rendertest/README.md).

#include "cli.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>

#include "harness.h"
#include "helios-rendertest_shaders.h"
#include "helios/core/fs.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"

#ifndef HELIOS_RENDERTEST_GOLDEN_DIR
#define HELIOS_RENDERTEST_GOLDEN_DIR "golden"
#endif

namespace helios::rendertest {

namespace {

constexpr std::string_view kUsage = R"(usage: helios-rendertest [options]

Renders the golden-image test scenes offscreen (docs/plan/03-rendering.md §8.4, RC-1), twice each
(serial and parallel command recording; results must be bit-identical), and compares them with the
goldens: ꟻLIP on Vulkan (lavapipe goldens in CI), command-stream traces on the Null backend.

options:
  --backend <b>          vulkan (default), null or all
  --scene <name>         run only this scene (repeatable)
  --list                 list the scenes and exit
  --golden-dir <dir>     golden root (default: the source tree's tools/rendertest/golden)
  --out <dir>            output directory for images, traces, results and reports (default: rendertest-output)
  --update-goldens       write the current output as the new goldens (also HELIOS_UPDATE_GOLDENS=1)
  --report               also write report.html / report.md over every result in --out
  --report-only          only aggregate the results already in --out
  --budget <seconds>     suite time budget checked by the report (default 600, RC-1)
  --validation           enable Khronos validation when installed (errors fail the scene)
  --require-validation   like --validation, but fail when the layer does not load; HELIOS_SKIP_GPU_TESTS=1
                         still skips a machine without a Vulkan device, never a missing layer
  --validation-self-test load the layer (required), print its name and version, and check that it
                         reports a seeded barrier from the wrong state (Vulkan only; no scenes run)
  --expect-scenes <list> comma-separated scene names; fail if the built-in list differs
  --coverage             check that every shipped shader entry point and pipeline of engine/render is
                         bound by a scene with committed lavapipe and Null goldens (RC-1; Null
                         backend, no GPU; --scene limits the scenes that count)
  --seed-name-collision  test hook for rendertest.cli: with --coverage, first create a rendertest
                         pipeline named like a shipped one, which the check must reject
  --print-probe          test hook for rendertest.validation-required: print whether the Khronos layer
                         is active (in the call chain, with its description) on a default device
                         (HELIOS_RHI_VALIDATION applies) and on the no-validation probe device (it must
                         not be, whatever the environment says)
  -h, --help             this text

environment: HELIOS_SKIP_GPU_TESTS=1 skips (passes) Vulkan scenes on a machine without a Vulkan loader,
driver or adapter (any other device error still fails); HELIOS_RHI_ADAPTER selects an adapter (index or
name substring).
)";

bool envIs(const char* name, std::string_view value) {
    const char* v = std::getenv(name);
    return v && std::string_view(v) == value;
}

std::vector<std::string> split(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        const usize comma = list.find_first_of(",;");
        const std::string_view item = list.substr(0, comma);
        if (!item.empty()) out.emplace_back(item);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return out;
}

/// A device for a run, or why there is none: `skip` (HELIOS_SKIP_GPU_TESTS=1 on a machine without a
/// Vulkan device) or `error`. Only the RHI's errors for a missing loader, driver or adapter can be a
/// skip; any other error fails, so a validation layer that fails (a missing layer, a missing library,
/// a layer whose vkCreateInstance fails, also when the loader forces it on every instance) is never a
/// skip. A no-Vulkan error with a required layer is a skip only when the loader forces no layers and a
/// probe device without validation fails too; otherwise the layer may have failed as if there were no
/// driver.
struct TestDevice {
    std::unique_ptr<rhi::Device> device;
    std::string skip;
    std::string error;
};

/// The Vulkan RHI's errors for a machine without Vulkan (engine/rhi/src/vulkan/vk_device.cpp): no
/// loader, no driver (the loader's VK_ERROR_INCOMPATIBLE_DRIVER), no physical device, or none that
/// meets Helios' requirements.
bool isNoVulkanError(const std::string& error) {
    for (const char* marker : {"Vulkan loader not found", "VK_ERROR_INCOMPATIBLE_DRIVER", "no Vulkan physical devices",
                               "no Vulkan adapter meets Helios' requirements"}) {
        if (error.find(marker) != std::string::npos) return true;
    }
    return false;
}

/// The loader enables these layers on every instance, the probe's included, so the probe cannot tell a
/// missing driver from a forced layer that fails as if there were none.
bool loaderForcesLayers() {
    for (const char* name : {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE"}) {
        const char* v = std::getenv(name);
        if (v && *v) return true;
    }
    return false;
}

/// A Vulkan device without validation whatever HELIOS_RHI_VALIDATION says (the probe of openDevice).
Result<std::unique_ptr<rhi::Device>> probeDevice() {
    return createTestDevice(Backend::Vulkan, false, false, /*validationFromEnvironment=*/false);
}

TestDevice openDevice(Backend backend, bool validation, bool requireValidation) {
    auto device = createTestDevice(backend, validation, requireValidation);
    if (device) return {std::move(device).value(), {}, {}};
    const std::string why = device.error().toString();
    if (backend != Backend::Vulkan || !envIs("HELIOS_SKIP_GPU_TESTS", "1")) {
        return {nullptr, {}, why + (backend == Backend::Vulkan ? " (HELIOS_SKIP_GPU_TESTS=1 skips machines without Vulkan)" : "")};
    }
    if (!isNoVulkanError(why)) {
        return {nullptr, {}, why + " (not a missing Vulkan loader, driver or adapter" +
                                 (requireValidation ? "; the required Khronos validation layer may be what failed" : "") +
                                 "; HELIOS_SKIP_GPU_TESTS does not skip that)"};
    }
    if (requireValidation && loaderForcesLayers()) {
        return {nullptr, {}, why + " (the loader forces layers on every instance, VK_INSTANCE_LAYERS or "
                                   "VK_LOADER_LAYERS_ENABLE, so a failing layer cannot be told from a missing "
                                   "driver; HELIOS_SKIP_GPU_TESTS does not skip that)"};
    }
    if (requireValidation && probeDevice()) {
        return {nullptr, {}, why + " (a Vulkan device exists, so the required Khronos validation layer is what failed; "
                                   "HELIOS_SKIP_GPU_TESTS does not skip that)"};
    }
    return {nullptr, why, {}};
}

/// --seed-name-collision: a rendertest pipeline named "Forward.Geometry" (rendertest's triangle shader).
bool seedNameCollision() {
    auto device = createTestDevice(Backend::Null, false);
    if (!device) return false;
    rhi::GraphicsPipelineDesc d;
    d.vertex = {rendertest_shaders::triangle(), "vsMain"};
    d.fragment = {rendertest_shaders::triangle(), "psMain"};
    d.colorCount = 1;
    d.colorFormats[0] = rhi::Format::RGBA8Unorm;
    d.name = "Forward.Geometry";
    auto pipeline = createLocalPipeline(*device.value(), d);
    if (!pipeline) return false;
    device.value()->destroy(pipeline.value());
    return true;
}

} // namespace

int runCli(const std::vector<std::string>& args, std::string& out, std::string& err) {
    std::vector<Backend> backends{Backend::Vulkan};
    std::set<std::string> only;
    std::filesystem::path goldenDir = fs::pathFromUtf8(HELIOS_RENDERTEST_GOLDEN_DIR);
    std::filesystem::path outDir = "rendertest-output";
    bool update = envIs("HELIOS_UPDATE_GOLDENS", "1");
    bool report = false;
    bool reportOnly = false;
    bool list = false;
    bool validation = false;
    bool requireValidation = false;
    bool selfTest = false;
    bool coverage = false;
    bool seedCollision = false;
    bool printProbe = false;
    f64 budget = 600.0;
    std::optional<std::vector<std::string>> expected;
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::optional<std::string> {
            if (i + 1 >= args.size()) return std::nullopt;
            return args[++i];
        };
        if (a == "--backend") {
            const auto v = value();
            if (!v || (*v != "vulkan" && *v != "null" && *v != "all")) {
                err += "helios-rendertest: --backend must be vulkan, null or all\n";
                return 2;
            }
            backends = *v == "vulkan" ? std::vector{Backend::Vulkan}
                       : *v == "null" ? std::vector{Backend::Null}
                                      : std::vector{Backend::Null, Backend::Vulkan};
        } else if (a == "--scene") {
            const auto v = value();
            if (!v) {
                err += "helios-rendertest: missing value for --scene\n";
                return 2;
            }
            only.insert(*v);
        } else if (a == "--golden-dir" || a == "--out" || a == "--budget" || a == "--expect-scenes") {
            const auto v = value();
            if (!v) {
                err += "helios-rendertest: missing value for " + a + "\n";
                return 2;
            }
            if (a == "--golden-dir") goldenDir = fs::pathFromUtf8(*v);
            if (a == "--out") outDir = fs::pathFromUtf8(*v);
            if (a == "--budget") {
                char* end = nullptr;
                budget = std::strtod(v->c_str(), &end);
                if (end == v->c_str() || *end != '\0' || !std::isfinite(budget) || budget <= 0.0) {
                    err += "helios-rendertest: --budget needs a positive number of seconds, got '" + *v + "'\n";
                    return 2;
                }
            }
            if (a == "--expect-scenes") expected = split(*v);
        } else if (a == "--update-goldens") {
            update = true;
        } else if (a == "--report") {
            report = true;
        } else if (a == "--report-only") {
            reportOnly = true;
        } else if (a == "--list") {
            list = true;
        } else if (a == "--validation") {
            validation = true;
        } else if (a == "--require-validation") {
            validation = requireValidation = true;
        } else if (a == "--validation-self-test") {
            selfTest = true;
        } else if (a == "--coverage") {
            coverage = true;
        } else if (a == "--seed-name-collision") {
            seedCollision = true;
        } else if (a == "--print-probe") {
            printProbe = true;
        } else if (a == "-h" || a == "--help") {
            out += kUsage;
            return 0;
        } else {
            err += "helios-rendertest: unknown argument '" + a + "'\n" + std::string(kUsage);
            return 2;
        }
    }

    if (printProbe) {
        for (const bool probe : {false, true}) {
            auto device = probe ? probeDevice() : createTestDevice(Backend::Vulkan, false, false);
            const char* which = probe ? "probe" : "default";
            if (!device) {
                err += std::format("helios-rendertest: {} device: {}\n", which, device.error().toString());
                return 1;
            }
            // The layer's own description when active (versions, the tool that answered, "enabled by the loader,
            // not requested"), for the win-gpu job's diagnostics.
            const rhi::Caps& caps = device.value()->caps();
            out += caps.has(rhi::CapBit::ValidationLayer)
                       ? std::format("{} device: Khronos validation active ({})\n", which, caps.validationLayer)
                       : std::format("{} device: Khronos validation inactive\n", which);
        }
        return 0;
    }
    if (selfTest) {
        if (auto device = openDevice(Backend::Vulkan, true, true); !device.device) {
            if (!device.skip.empty()) {
                out += "helios-rendertest: SKIP validation self-test (" + device.skip + ")\n";
                return 0;
            }
            err += "helios-rendertest: validation self-test: " + device.error + "\n";
            return 1;
        }
        auto result = runValidationSelfTest();
        if (!result) {
            err += "helios-rendertest: validation self-test: " + result.error().toString() + "\n";
            return 1;
        }
        out += std::format("validation layer: {}\nseeded barrier error reported by the layer ({} error(s)): {}\n",
                           result->layer, result->errors, result->message);
        return 0;
    }

    std::vector<std::unique_ptr<Scene>> scenes = createScenes();
    if (list) {
        for (const auto& s : scenes) {
            out += std::format("{:<10} {}x{}  {}\n", s->info().name, s->info().width, s->info().height, s->info().description);
        }
        return 0;
    }
    if (expected) {
        std::vector<std::string> actual;
        for (const auto& s : scenes) actual.emplace_back(s->info().name);
        if (actual != *expected) {
            std::string names;
            for (const auto& n : actual) names += (names.empty() ? "" : ",") + n;
            err += "helios-rendertest: the scene list is " + names + "; update the CMake test list\n";
            return 1;
        }
    }
    for (const std::string& name : only) {
        bool known = false;
        for (const auto& s : scenes) known |= s->info().name == name;
        if (!known) {
            err += "helios-rendertest: unknown scene '" + name + "' (see --list)\n";
            return 2;
        }
    }
    if (coverage) {
        std::vector<Scene*> counted;
        for (const auto& s : scenes) {
            if (only.empty() || only.count(std::string(s->info().name))) counted.push_back(s.get());
        }
        if (seedCollision && !seedNameCollision()) {
            err += "helios-rendertest: --seed-name-collision: cannot create the seeded pipeline\n";
            return 1;
        }
        auto report = measureCoverage(counted, goldenDir);
        if (!report) {
            err += "helios-rendertest: coverage: " + report.error().toString() + "\n";
            return 1;
        }
        out += std::format("RC-1 coverage: {} shipped entry point(s), {} shipped pipeline(s), {} scene(s)\n",
                           report->entryPoints.size(), report->pipelines.size(), counted.size());
        auto print = [&](std::string_view kind, const std::vector<CoverageItem>& items) {
            for (const CoverageItem& item : items) {
                std::string scenesText;
                for (const std::string& scene : item.scenes) scenesText += (scenesText.empty() ? "" : ", ") + scene;
                out += std::format("  {} {}: {}\n", kind, item.name, scenesText.empty() ? "NOT COVERED" : scenesText);
            }
        };
        print("entry point", report->entryPoints);
        print("pipeline", report->pipelines);
        for (const std::string& problem : report->problems) err += "helios-rendertest: coverage: " + problem + "\n";
        return report->ok() ? 0 : 1;
    }
    if (auto made = fs::createDirectories(outDir); !made) {
        err += "helios-rendertest: " + made.error().toString() + "\n";
        return 1;
    }

    bool ok = true;
    if (!reportOnly) {
        jobs::JobSystem jobSystem(jobs::JobSystemDesc{.workerCount = 3, .name = "Record"});
        for (Backend backend : backends) {
            TestDevice device = openDevice(backend, validation, requireValidation);
            if (!device.device) {
                if (!device.skip.empty()) {
                    out += "helios-rendertest: SKIP vulkan (" + device.skip + ")\n";
                    // Record the skip: an older passing result must not stand in for this run.
                    for (const auto& scene : scenes) {
                        if (!only.empty() && !only.count(std::string(scene->info().name))) continue;
                        SceneResult skipped;
                        skipped.scene = scene->info().name;
                        skipped.backend = backendName(backend);
                        skipped.status = "skip";
                        skipped.message = "no Vulkan device: " + device.skip;
                        (void)fs::createDirectories(outDir / skipped.backend);
                        (void)writeResult(skipped, outDir / skipped.backend / (skipped.scene + ".json"));
                    }
                    continue;
                }
                err += std::format("helios-rendertest: cannot create a {} device: {}\n", backendName(backend), device.error);
                ok = false;
                continue;
            }
            RunOptions options;
            options.backend = backend;
            options.goldenDir = goldenDir;
            options.outDir = outDir;
            options.updateGoldens = update;
            options.validation = validation;
            options.requireValidation = requireValidation;
            options.jobs = &jobSystem;
            for (auto& scene : scenes) {
                if (!only.empty() && !only.count(std::string(scene->info().name))) continue;
                const SceneResult r = runScene(*scene, *device.device, options);
                const std::string line =
                    backend == Backend::Vulkan
                        ? std::format("{:<7} {:<10} vulkan  mean ꟻLIP {:.5f}  max {:.4f}  {:6.0f} ms  {}  {}\n",
                                      r.status == "pass" ? "PASS" : (r.status == "updated" ? "UPDATED" : "FAIL"), r.scene,
                                      r.meanFlip, r.maxFlip, r.milliseconds, r.validation ? "validated" : "NOT validated",
                                      r.message.empty() ? r.goldenKey : r.message)
                        : std::format("{:<7} {:<10} null    trace {}  {:6.0f} ms  {}\n",
                                      r.status == "pass" ? "PASS" : (r.status == "updated" ? "UPDATED" : "FAIL"), r.scene,
                                      r.renderedTwiceIdentical ? "deterministic" : "NOT deterministic", r.milliseconds,
                                      r.message);
                (r.passed() ? out : err) += line;
                ok = ok && r.passed();
            }
        }
    }
    if (report || reportOnly) {
        std::string summary;
        // Only scenes that still exist: results of removed or renamed scenes are stale.
        std::vector<SceneResult> results = collectResults(outDir);
        std::erase_if(results, [&](const SceneResult& r) {
            for (const auto& s : scenes) {
                if (s->info().name == r.scene) return false;
            }
            return true;
        });
        const bool reportOk = writeReport(results, outDir, budget, summary);
        out += std::format("helios-rendertest: {} -> {}\n", summary, fs::pathToUtf8(outDir / "report.html"));
        ok = ok && reportOk;
    }
    return ok ? 0 : 1;
}

} // namespace helios::rendertest
