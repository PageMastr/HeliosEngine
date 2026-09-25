// helios-rendertest command line (see tools/rendertest/README.md).

#include "cli.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <set>

#include "harness.h"
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
  --expect-scenes <list> comma-separated scene names; fail if the built-in list differs
  -h, --help             this text

environment: HELIOS_SKIP_GPU_TESTS=1 skips (passes) Vulkan scenes when no Vulkan device can be created;
HELIOS_RHI_ADAPTER selects an adapter (index or name substring).
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
        } else if (a == "-h" || a == "--help") {
            out += kUsage;
            return 0;
        } else {
            err += "helios-rendertest: unknown argument '" + a + "'\n" + std::string(kUsage);
            return 2;
        }
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
    if (auto made = fs::createDirectories(outDir); !made) {
        err += "helios-rendertest: " + made.error().toString() + "\n";
        return 1;
    }

    bool ok = true;
    if (!reportOnly) {
        jobs::JobSystem jobSystem(jobs::JobSystemDesc{.workerCount = 3, .name = "Record"});
        for (Backend backend : backends) {
            auto device = createTestDevice(backend, validation);
            if (!device) {
                if (backend == Backend::Vulkan && envIs("HELIOS_SKIP_GPU_TESTS", "1")) {
                    out += "helios-rendertest: SKIP vulkan (" + device.error().toString() + ")\n";
                    // Record the skip: an older passing result must not stand in for this run.
                    for (const auto& scene : scenes) {
                        if (!only.empty() && !only.count(std::string(scene->info().name))) continue;
                        SceneResult skipped;
                        skipped.scene = scene->info().name;
                        skipped.backend = backendName(backend);
                        skipped.status = "skip";
                        skipped.message = "no Vulkan device: " + device.error().toString();
                        (void)fs::createDirectories(outDir / skipped.backend);
                        (void)writeResult(skipped, outDir / skipped.backend / (skipped.scene + ".json"));
                    }
                    continue;
                }
                err += std::format("helios-rendertest: cannot create a {} device: {} (set HELIOS_SKIP_GPU_TESTS=1 to skip)\n",
                                   backendName(backend), device.error().toString());
                ok = false;
                continue;
            }
            RunOptions options;
            options.backend = backend;
            options.goldenDir = goldenDir;
            options.outDir = outDir;
            options.updateGoldens = update;
            options.validation = validation;
            options.jobs = &jobSystem;
            for (auto& scene : scenes) {
                if (!only.empty() && !only.count(std::string(scene->info().name))) continue;
                const SceneResult r = runScene(*scene, *device.value(), options);
                const std::string line =
                    backend == Backend::Vulkan
                        ? std::format("{:<7} {:<10} vulkan  mean ꟻLIP {:.5f}  max {:.4f}  {:6.0f} ms  {}\n",
                                      r.status == "pass" ? "PASS" : (r.status == "updated" ? "UPDATED" : "FAIL"), r.scene,
                                      r.meanFlip, r.maxFlip, r.milliseconds, r.message.empty() ? r.goldenKey : r.message)
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
