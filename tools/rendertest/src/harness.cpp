// helios-rendertest harness (see harness.h).

#include "harness.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>

#include "helios/core/fs.h"
#include "helios/core/jobs.h"
#include "helios/core/time.h"
#include "helios/render/flip.h"
#include "helios/render/image.h"
#include "helios/render/render_graph.h"
#include "helios/rhi/null_device.h"
#include "helios/rhi/utils.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include <yyjson.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace helios::rendertest {

using namespace helios::render;

namespace {

constexpr rhi::Format kOutputFormat = rhi::Format::RGBA8Unorm;

/// "llvmpipe (LLVM 19.1.7, 256 bits)" -> "llvmpipe".
std::string driverKey(const rhi::AdapterInfo& adapter) {
    std::string key;
    for (char c : adapter.driverName.empty() ? adapter.name : adapter.driverName) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!key.empty() && key.back() != '-') {
            key += '-';
        }
    }
    while (!key.empty() && key.back() == '-') key.pop_back();
    return key.empty() ? std::string("unknown") : key;
}

std::string normalizedText(std::string text) {
    std::erase(text, '\r');
    return text;
}

struct Capture {
    ImageRgba8 image;   // Vulkan
    std::string trace;  // Null: plan dump + command stream of the captured frame
};

/// Renders warmup + 1 frames into a fresh output and pool; captures the last frame.
Result<Capture> renderOnce(Scene& scene, rhi::Device& device, jobs::JobSystem* jobs) {
    const SceneInfo& info = scene.info();
    rhi::NullDevice* null = rhi::NullDevice::from(device);
    HELIOS_TRY_ASSIGN(rhi::TextureH output,
                      (device.createTexture(rhi::TextureDesc::tex2D(
                          kOutputFormat, info.width, info.height,
                          rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc, "Output"))));
    const rhi::TextureDesc outputDesc = device.textureDesc(output);
    Capture capture;
    {
        RgResourcePool pool(device);
        rhi::TimelinePoint done;
        std::string plan;
        for (u32 frame = 0; frame <= info.warmupFrames; ++frame) {
            HELIOS_TRY(device.beginFrame());
            if (null && frame == info.warmupFrames) null->clearTrace();
            RenderGraph graph(info.name);
            const RgTexture out = graph.importTexture(
                "Output", output, outputDesc,
                {.initialState = frame == 0 ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySource,
                 .finalState = rhi::ResourceState::CopySource});
            scene.addPasses(graph, out, frame);
            HELIOS_TRY(graph.compile({.maxCommandLists = 6}));
            HELIOS_TRY_ASSIGN(RgExecuteResult result, graph.execute(device, pool, {.jobs = jobs}));
            if (graph.contextErrorCount() != 0) {
                return Error{ErrorCode::InvalidState, "a pass resolved a resource it did not declare"};
            }
            done = result.graphics();
            plan = rgDumpPlan(graph.executedPlan());
        }
        if (null) {
            capture.trace = plan + "--- Null trace ---\n" + null->trace();
        } else {
            HELIOS_TRY_ASSIGN(std::vector<u8> bytes,
                              rhi::readbackTexture(device, output, rhi::ResourceState::CopySource, 0, 0, done));
            capture.image = ImageRgba8(info.width, info.height);
            if (bytes.size() != capture.image.pixels.size()) return Error{ErrorCode::Corrupt, "readback size mismatch"};
            capture.image.pixels = std::move(bytes);
        }
        HELIOS_TRY(device.waitIdle());
    }
    device.destroy(output);
    return capture;
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string htmlEscape(std::string_view s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

} // namespace

std::string_view backendName(Backend backend) noexcept { return backend == Backend::Vulkan ? "vulkan" : "null"; }

Result<std::unique_ptr<rhi::Device>> createTestDevice(Backend backend, bool validation) {
    rhi::DeviceDesc desc;
    desc.appName = "helios-rendertest";
    desc.backend = backend == Backend::Vulkan ? rhi::Backend::Vulkan : rhi::Backend::Null;
    desc.enableSwapchain = false;  // offscreen only: no window system needed (CI, containers)
    desc.validation = validation;
    desc.adapterPreference = rhi::AdapterPreference::Software;
    return rhi::Device::create(desc);
}

SceneResult runScene(Scene& scene, rhi::Device& device, const RunOptions& options) {
    const SceneInfo& info = scene.info();
    SceneResult r;
    r.scene = info.name;
    r.backend = backendName(options.backend);
    r.maxMeanFlip = info.maxMeanFlip;
    r.maxAllowedFlip = info.maxFlip;
    const Stopwatch timer;
    const std::filesystem::path outDir = options.outDir / r.backend;
    (void)fs::createDirectories(outDir);
    auto fail = [&](std::string message) {
        r.status = "fail";
        r.message = std::move(message);
        r.milliseconds = timer.elapsedMillis();
        (void)writeResult(r, outDir / (r.scene + ".json"));
        return r;
    };
    const u64 validationBefore = device.validationErrorCount();
    Result<Capture> first = Error{ErrorCode::Unknown, "not run"};
    Result<Capture> second = Error{ErrorCode::Unknown, "not run"};
    if (options.backend == Backend::Null) {
        // The Null trace carries timeline values: each render gets a fresh device, so the stream
        // does not depend on which scenes ran before (goldens are per scene).
        for (int run = 0; run < 2; ++run) {
            auto fresh = createTestDevice(Backend::Null, false);
            if (!fresh) return fail("Null device: " + fresh.error().toString());
            if (auto init = scene.init(*fresh.value(), kOutputFormat); !init) return fail("init: " + init.error().toString());
            (run == 0 ? first : second) = renderOnce(scene, *fresh.value(), run == 0 ? nullptr : options.jobs);
            scene.destroy(*fresh.value());
            (void)fresh.value()->waitIdle();
            if (const u64 errors = fresh.value()->validationErrorCount(); errors != 0) {
                std::string text;
                if (rhi::NullDevice* null = rhi::NullDevice::from(*fresh.value())) {
                    for (const std::string& e : null->validationErrors()) text += "\n  " + e;
                }
                return fail(std::format("{} Null validation error(s):{}", errors, text));
            }
        }
    } else {
        // Render twice on one device: serial recording, then parallel recording on the job system.
        if (auto init = scene.init(device, kOutputFormat); !init) return fail("init: " + init.error().toString());
        first = renderOnce(scene, device, nullptr);
        if (first) second = renderOnce(scene, device, options.jobs);
        scene.destroy(device);
        (void)device.waitIdle();
    }
    if (!first) return fail("render: " + first.error().toString());
    if (!second) return fail("second render: " + second.error().toString());
    if (const u64 errors = device.validationErrorCount() - validationBefore; errors != 0) {
        return fail(std::format("{} validation error(s)", errors));
    }

    if (options.backend == Backend::Null) {
        r.goldenKey = "null";
        r.renderedTwiceIdentical = first->trace == second->trace;
        const std::filesystem::path golden = options.goldenDir / "null" / (r.scene + ".txt");
        r.actualFile = "null/" + r.scene + ".trace.txt";
        r.goldenFile = fs::pathToGenericUtf8(golden);
        (void)fs::writeTextFile(options.outDir / r.actualFile, first->trace);
        if (!r.renderedTwiceIdentical) {
            (void)fs::writeTextFile(options.outDir / ("null/" + r.scene + ".second.trace.txt"), second->trace);
            return fail("the two renders produced different command streams");
        }
        if (options.updateGoldens) {
            (void)fs::createDirectories(golden.parent_path());
            if (auto w = fs::writeTextFile(golden, first->trace); !w) return fail("cannot write golden: " + w.error().toString());
            r.status = "updated";
        } else {
            auto expected = fs::readTextFile(golden);
            if (!expected) return fail("missing golden " + fs::pathToUtf8(golden) + " (run with --update-goldens)");
            if (normalizedText(expected.value()) != first->trace) {
                // Report the first differing line.
                const std::string e = normalizedText(expected.value());
                usize line = 1, i = 0;
                while (i < e.size() && i < first->trace.size() && e[i] == first->trace[i]) {
                    if (e[i] == '\n') ++line;
                    ++i;
                }
                return fail(std::format("trace differs from the golden at line {} (actual: {})", line, r.actualFile));
            }
            r.status = "pass";
        }
        r.milliseconds = timer.elapsedMillis();
        (void)writeResult(r, outDir / (r.scene + ".json"));
        return r;
    }

    // Vulkan: goldens per driver, falling back to the lavapipe set that CI pins.
    const rhi::AdapterInfo& adapter = device.caps().adapter;
    r.adapter = std::format("{} ({} {})", adapter.name, adapter.driverName, adapter.driverInfo);
    r.renderedTwiceIdentical = first->image == second->image;
    const ImageRgba8& actual = first->image;
    r.actualFile = "vulkan/" + r.scene + ".png";
    (void)writePng(options.outDir / r.actualFile, actual);
    if (!r.renderedTwiceIdentical) {
        (void)writePng(options.outDir / ("vulkan/" + r.scene + ".second.png"), second->image);
        return fail(std::format("the two renders differ in {} pixels (missing barrier?)",
                                countDifferentPixels(first->image, second->image)));
    }
    r.goldenKey = "vulkan-" + driverKey(adapter);
    std::filesystem::path golden = options.goldenDir / r.goldenKey / (r.scene + ".png");
    if (options.updateGoldens) {
        if (auto w = writePng(golden, actual); !w) return fail("cannot write golden: " + w.error().toString());
        r.goldenFile = fs::pathToGenericUtf8(golden);
        r.status = "updated";
        r.milliseconds = timer.elapsedMillis();
        (void)writeResult(r, outDir / (r.scene + ".json"));
        return r;
    }
    if (!fs::exists(golden)) {
        r.goldenKey = "vulkan-llvmpipe";
        golden = options.goldenDir / r.goldenKey / (r.scene + ".png");
    }
    r.goldenFile = fs::pathToGenericUtf8(golden);
    auto expected = readPng(golden);
    if (!expected) return fail("missing golden " + fs::pathToUtf8(golden) + " (run with --update-goldens)");
    (void)writePng(options.outDir / ("vulkan/" + r.scene + ".golden.png"), expected.value());
    auto flip = computeFlip(expected.value(), actual);
    if (!flip) return fail("ꟻLIP: " + flip.error().toString());
    r.meanFlip = flip->mean;
    r.maxFlip = flip->max;
    r.differentPixels = countDifferentPixels(expected.value(), actual);
    r.flipFile = "vulkan/" + r.scene + ".flip.png";
    (void)writePng(options.outDir / r.flipFile, flipErrorImage(flip.value()));
    if (r.meanFlip > info.maxMeanFlip || r.maxFlip > info.maxFlip) {
        return fail(std::format("ꟻLIP mean {:.5f} (limit {}) max {:.4f} (limit {})", r.meanFlip, info.maxMeanFlip,
                                r.maxFlip, info.maxFlip));
    }
    r.status = "pass";
    r.milliseconds = timer.elapsedMillis();
    (void)writeResult(r, outDir / (r.scene + ".json"));
    return r;
}

Result<void> writeResult(const SceneResult& r, const std::filesystem::path& file) {
    std::string json = "{\n";
    auto str = [&](std::string_view key, std::string_view value) {
        json += std::format("  \"{}\": \"{}\",\n", key, jsonEscape(value));
    };
    str("scene", r.scene);
    str("backend", r.backend);
    str("status", r.status);
    str("message", r.message);
    str("adapter", r.adapter);
    str("goldenKey", r.goldenKey);
    str("actualFile", r.actualFile);
    str("goldenFile", r.goldenFile);
    str("flipFile", r.flipFile);
    json += std::format("  \"meanFlip\": {},\n  \"maxFlip\": {},\n  \"maxMeanFlip\": {},\n  \"maxAllowedFlip\": {},\n",
                        r.meanFlip, r.maxFlip, r.maxMeanFlip, r.maxAllowedFlip);
    json += std::format("  \"renderedTwiceIdentical\": {},\n  \"differentPixels\": {},\n  \"milliseconds\": {}\n}}\n",
                        r.renderedTwiceIdentical ? "true" : "false", r.differentPixels, r.milliseconds);
    return fs::writeTextFile(file, json);
}

Result<SceneResult> readResult(const std::filesystem::path& file) {
    HELIOS_TRY_ASSIGN(std::string text, fs::readTextFile(file));
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) return Error{ErrorCode::ParseError, fs::pathToUtf8(file) + ": invalid JSON"};
    yyjson_val* root = yyjson_doc_get_root(doc);
    auto getStr = [&](const char* key) {
        yyjson_val* v = yyjson_obj_get(root, key);
        const char* s = v ? yyjson_get_str(v) : nullptr;
        return s ? std::string(s) : std::string();
    };
    auto getNum = [&](const char* key) {
        yyjson_val* v = yyjson_obj_get(root, key);
        return v ? yyjson_get_num(v) : 0.0;
    };
    SceneResult r;
    r.scene = getStr("scene");
    r.backend = getStr("backend");
    r.status = getStr("status");
    r.message = getStr("message");
    r.adapter = getStr("adapter");
    r.goldenKey = getStr("goldenKey");
    r.actualFile = getStr("actualFile");
    r.goldenFile = getStr("goldenFile");
    r.flipFile = getStr("flipFile");
    r.meanFlip = getNum("meanFlip");
    r.maxFlip = getNum("maxFlip");
    r.maxMeanFlip = getNum("maxMeanFlip");
    r.maxAllowedFlip = getNum("maxAllowedFlip");
    yyjson_val* twice = yyjson_obj_get(root, "renderedTwiceIdentical");
    r.renderedTwiceIdentical = twice && yyjson_get_bool(twice);
    r.differentPixels = static_cast<u64>(getNum("differentPixels"));
    r.milliseconds = getNum("milliseconds");
    yyjson_doc_free(doc);
    if (r.scene.empty() || r.status.empty()) return Error{ErrorCode::ParseError, fs::pathToUtf8(file) + ": incomplete result"};
    return r;
}

std::vector<SceneResult> collectResults(const std::filesystem::path& outDir) {
    std::vector<SceneResult> results;
    for (std::string_view backend : {"null", "vulkan"}) {
        const std::filesystem::path dir = outDir / backend;
        auto entries = fs::listDirectory(dir);
        if (!entries) continue;
        std::vector<std::filesystem::path> files;
        for (const auto& e : entries.value()) {
            if (e.path.extension() == ".json") files.push_back(e.path);
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            if (auto r = readResult(f); r.ok()) results.push_back(std::move(r).value());
        }
    }
    return results;
}

bool writeReport(const std::vector<SceneResult>& results, const std::filesystem::path& outDir, f64 budgetSeconds,
                 std::string& summary) {
    u32 failed = 0;
    f64 totalMs = 0.0;
    for (const SceneResult& r : results) {
        failed += r.passed() ? 0u : 1u;
        totalMs += r.milliseconds;
    }
    const bool overBudget = totalMs > budgetSeconds * 1000.0;
    summary = std::format("{} result(s), {} failed, {:.1f} s total (budget {:.0f} s){}", results.size(), failed,
                          totalMs / 1000.0, budgetSeconds, overBudget ? " — OVER BUDGET" : "");

    std::string md = "# helios-rendertest report\n\n" + summary + "\n\n";
    md += "Golden policy (03 §8.4): ꟻLIP mean <= 0.01 plus a per-scene maximum; every scene is rendered twice "
          "(serial and parallel recording) and must be bit-identical. Null runs compare command-stream traces.\n\n";
    md += "| Scene | Backend | Status | mean ꟻLIP | max ꟻLIP | twice identical | pixels != golden | ms | Notes |\n";
    md += "|---|---|---|---|---|---|---|---|---|\n";
    std::string rows;
    for (const SceneResult& r : results) {
        md += std::format("| {} | {} | {} | {:.5f} | {:.4f} | {} | {} | {:.0f} | {} |\n", r.scene, r.backend, r.status,
                          r.meanFlip, r.maxFlip, r.renderedTwiceIdentical ? "yes" : "NO", r.differentPixels,
                          r.milliseconds, r.message.empty() ? r.goldenKey : r.message);
        std::string images;
        if (r.backend == "vulkan") {
            auto img = [&](const std::string& file, std::string_view label) {
                if (file.empty()) return;
                images += std::format("<figure><img src=\"{}\" alt=\"{}\"><figcaption>{}</figcaption></figure>",
                                      htmlEscape(file), label, label);
            };
            img(r.actualFile, "actual");
            img(r.goldenFile.empty() ? std::string() : "vulkan/" + r.scene + ".golden.png", "golden");
            img(r.flipFile, "ꟻLIP error");
        } else if (!r.actualFile.empty()) {
            images = std::format("<a href=\"{}\">trace</a>", htmlEscape(r.actualFile));
        }
        rows += std::format("<tr class=\"{}\"><td>{}</td><td>{}</td><td>{}</td><td>{:.5f}</td><td>{:.4f}</td><td>{}</td>"
                            "<td>{}</td><td>{:.0f}</td><td>{}</td><td class=\"imgs\">{}</td></tr>\n",
                            r.passed() ? "ok" : "bad", htmlEscape(r.scene), r.backend, r.status, r.meanFlip, r.maxFlip,
                            r.renderedTwiceIdentical ? "yes" : "NO", r.differentPixels, r.milliseconds,
                            htmlEscape(r.message.empty() ? r.adapter : r.message), images);
    }
    const std::string html = std::format(R"(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>helios-rendertest report</title>
<style>
body {{ font: 14px/1.4 system-ui, sans-serif; margin: 24px; background: #fafafa; color: #1d1d1f; }}
table {{ border-collapse: collapse; }} td, th {{ border: 1px solid #ccc; padding: 4px 8px; vertical-align: top; }}
tr.ok td:nth-child(3) {{ color: #1a7f37; font-weight: 600; }} tr.bad td:nth-child(3) {{ color: #cf222e; font-weight: 600; }}
figure {{ display: inline-block; margin: 0 6px 0 0; }} figure img {{ width: 240px; image-rendering: pixelated; border: 1px solid #999; }}
figcaption {{ font-size: 12px; color: #555; }}
</style></head><body>
<h1>helios-rendertest</h1><p>{}</p>
<p>Golden policy (03 §8.4): ꟻLIP mean &le; 0.01 plus a per-scene maximum; each scene is rendered twice (serial and
parallel command recording) and must be bit-identical. Null runs compare command-stream traces.</p>
<table><tr><th>Scene</th><th>Backend</th><th>Status</th><th>mean ꟻLIP</th><th>max ꟻLIP</th><th>twice identical</th>
<th>pixels &ne; golden</th><th>ms</th><th>Adapter / message</th><th>Images</th></tr>
{}</table></body></html>
)",
                                         htmlEscape(summary), rows);
    (void)fs::writeTextFile(outDir / "report.md", md);
    (void)fs::writeTextFile(outDir / "report.html", html);
    return failed == 0 && !overBudget;
}

} // namespace helios::rendertest
