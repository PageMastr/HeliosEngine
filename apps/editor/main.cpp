// helios-editor: the Helios editor (07 §1). The process is edui::EditorHost (engine/editorui):
// the ImGui docking shell over the ToolsFramework, rendering through the Helios RHI. This file only
// parses the command line. See apps/editor/README.md.

#include <algorithm>
#include <cstdio>
#include <string>

#include "helios/core/cmdline.h"
#include "helios/core/crash.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/core/version.h"
#include "helios/editorui/editor_host.h"
#include "helios/toolsfw/samples.h"

using namespace helios;

namespace {

constexpr std::string_view kUsage = R"(helios-editor - Helios editor

  helios-editor [--project-root=<dir>] [options]

Project
  --project-root=<dir>   project whose records/<table>/*.hrec are opened (default: none)
  --project=<name>       project name for the journal and window title (default: the root's name)
  --user=<name>          author of this session's transactions (default: local)
  --recover=<file|auto>  replay a crash journal at start (auto = the newest unclean session)
  --journal-dir=<dir>    journal root (default: HELIOS_JOURNAL_DIR, then the user state dir)
  --no-journal           do not journal (scratch sessions)

Display
  --theme=<dark|light|high-contrast>   (default dark)
  --theme-file=<file>    a theme token file (engine/editorui/themes/*.jsonc format)
  --scale=<f>            UI scale, 1 = 100 % (default: the display's content scale)
  --width=<dip> --height=<dip>         window size at 100 % (default 1600x900)
  --pseudo-loc           pseudo-localized UI text (layout testing)

Automation
  --rpc=<name>           remote-control endpoint name (default helios-editor-<pid>; "-" = off)
  --test-mode            deterministic UI for helios-uitest: fixed clock and font, no OS input,
                         hidden window (add --present to show it), edits with origin ui-scripted
  --present              show the window in test mode
  --frames=<n>           exit after n frames; --screenshot=<png> saves the last one
  --validation           Vulkan validation layers
  --log-level=<level>    trace|debug|info|warn|error (default info)
  --crash-dir=<dir>      crash reports (default saved/crashes)
  --version, --help
)";

int run(const CommandLine& cl) {
    if (cl.has("help") || cl.has("h")) {
        std::fputs(kUsage.data(), stdout);
        return 0;
    }
    if (cl.has("version")) {
        std::printf("helios-editor %s\n", version::kString);
        return 0;
    }
    if (auto lvl = cl.value("log-level")) {
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    }
    if (auto r = installCrashHandler(fs::pathFromUtf8(cl.getString("crash-dir", "saved/crashes")), {.appName = "helios-editor"}); !r) {
        HELIOS_LOG_WARN("crash handler not installed: {}", r.error().message);
    }
    if (auto r = tf::samples::registerSampleTypes(); !r) {
        HELIOS_LOG_ERROR("sample types: {}", r.error());
        return 1;
    }

    edui::EditorConfig config;
    if (auto root = cl.value("project-root")) config.projectRoot = fs::pathFromUtf8(*root);
    config.project = std::string(cl.getString("project", ""));
    if (config.project.empty()) {
        config.project = config.projectRoot.empty() ? std::string("scratch") : fs::pathToUtf8(config.projectRoot.lexically_normal().filename());
        if (config.project.empty()) config.project = fs::pathToUtf8(config.projectRoot.lexically_normal().parent_path().filename());
        if (config.project.empty()) config.project = "project";
    }
    config.user = std::string(cl.getString("user", "local"));
    config.recover = std::string(cl.getString("recover", ""));
    if (auto dir = cl.value("journal-dir")) config.journalRoot = fs::pathFromUtf8(*dir);
    config.journal = !cl.has("no-journal");
    config.theme = std::string(cl.getString("theme", "dark"));
    if (auto file = cl.value("theme-file")) config.themeFile = fs::pathFromUtf8(*file);
    // Bounded in f64 (converting a double outside f32's range is undefined behaviour); create()
    // clamps it to the supported range. Not positive, NaN included: the display's scale.
    const f64 scale = cl.getFloat("scale", 0.0);
    config.scale = scale > 0.0 ? static_cast<f32>(std::min(scale, 100.0)) : 0.0f;
    config.width = static_cast<u32>(cl.getInt("width", 1600));
    config.height = static_cast<u32>(cl.getInt("height", 900));
    config.pseudoLoc = cl.has("pseudo-loc");
    config.rpcEndpoint = std::string(cl.getString("rpc", ""));
    config.testMode = cl.has("test-mode");
    config.present = !config.testMode || cl.has("present");
    config.maxFrames = static_cast<u64>(std::max<i64>(0, cl.getInt("frames", 0)));
    if (auto shot = cl.value("screenshot")) config.screenshot = fs::pathFromUtf8(*shot);
    config.validation = cl.has("validation");
    if (!config.screenshot.empty() && config.maxFrames == 0) {
        HELIOS_LOG_ERROR("--screenshot needs --frames=N (the last frame is saved)");
        return 1;
    }

    auto host = edui::EditorHost::create(config);
    if (!host) {
        HELIOS_LOG_ERROR("helios-editor: {}", host.error());
        return 1;
    }
    return (*host)->run();
}

} // namespace

int main(int argc, char** argv) {
    // On Windows, fromProcess() reads the full Unicode command line instead of ANSI argv.
    const CommandLine cl = platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv);
    return run(cl);
}
