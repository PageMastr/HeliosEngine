// helios-cook: the cook CLI (07 §1.10 names it beside helios-tool and helios-assetd). v0 (WP-0.8) has one
// verb, `records`: `.hrec` sources → records.client.hrdb + records.server.hrdb (engine/records). It
// links the sample record types only, like helios-tool, until projects register their own (07 §1.10).
// See README.md.

#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/core/platform_init.h"
#include "helios/core/version.h"
#include "helios/records/cook.h"
#include "helios/toolsfw/samples.h"

using namespace helios;

namespace {

constexpr std::string_view kUsage = R"(helios-cook - Helios cook CLI

  helios-cook <verb> [options]

Verbs
  records    cook records/<table>/**/*.hrec into records.client.hrdb and records.server.hrdb
             (02 §3.3: $parent inheritance, reference, tag and formula checks, the client/server
             split with its AAA-SEC-4 rules)

Options
  --project-root=<dir>   project with records/<table>/*.hrec (default: current directory)
  --out=<dir>            output directory (default: <project-root>/cooked)
  --quiet                print errors only
  --log-level=<level>    trace|debug|info|warn|error (default warn)
  --version, --help

Exit codes: 0 ok, 1 the cook found errors (listed on stderr), 2 usage error, 3 I/O or setup failure.
)";

constexpr int kOk = 0;
constexpr int kCookErrors = 1;
constexpr int kUsageError = 2;
constexpr int kFailed = 3;

int fail(int code, const std::string& message) {
    std::fprintf(stderr, "helios-cook: %s\n", message.c_str());
    return code;
}

int cmdRecords(const CommandLine& cl) {
    const fs::Path root = cl.value("project-root") ? fs::pathFromUtf8(*cl.value("project-root")) : std::filesystem::current_path();
    const fs::Path outDir = cl.value("out") ? fs::pathFromUtf8(*cl.value("out")) : root / "cooked";
    auto sources = records::collectSources(root, refl::TypeRegistry::global());
    if (!sources) return fail(kCookErrors, std::format("{}", sources.error().message));
    std::vector<records::CookDiagnostic> diags;
    auto out = records::cook(*sources, {}, &diags);
    if (!out) {
        std::string text;
        for (const records::CookDiagnostic& d : diags) text += std::format("{}{}{}\n", d.path, d.path.empty() ? "" : ": ", d.message);
        std::fputs(text.c_str(), stderr);
        return fail(kCookErrors, std::format("{} error(s); nothing written", diags.size()));
    }
    if (auto r = records::writeCookOutput(*out, outDir); !r) return fail(kFailed, std::format("{}", r.error()));
    if (!cl.has("quiet")) {
        const records::CookStats& st = out->stats;
        std::string text = std::format("cooked {} records ({} inherit), {} tags ({} withheld from the client), {} formulas\n",
                                       st.records, st.inherited, st.tags, st.withheldTags, st.formulas);
        text += std::format("  {}: {} records, {} bytes\n", fs::pathToGenericUtf8(outDir / fs::pathFromUtf8(records::kClientDbFile)),
                            st.clientRecords, out->client.size());
        text += std::format("  {}: {} records, {} bytes\n", fs::pathToGenericUtf8(outDir / fs::pathFromUtf8(records::kServerDbFile)),
                            st.serverRecords, out->server.size());
        std::fputs(text.c_str(), stdout);
    }
    return kOk;
}

int run(const CommandLine& cl) {
    if (cl.has("version")) {
        std::printf("helios-cook %s\n", version::kString);
        return kOk;
    }
    if (cl.has("help") || cl.has("h")) {
        std::fputs(kUsage.data(), stdout);
        return kOk;
    }
    if (cl.positional().size() != 1) {
        std::fputs(kUsage.data(), stderr);
        return kUsageError;
    }
    log::setLevel(log::Level::Warn);
    if (auto lvl = cl.value("log-level")) {
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    }
    if (auto r = tf::samples::registerSampleTypes(); !r) return fail(kFailed, std::format("sample types: {}", r.error()));
    const std::string& verb = cl.positional()[0];
    if (verb == "records") return cmdRecords(cl);
    return fail(kUsageError, std::format("unknown verb '{}' (--help lists them)", verb));
}

} // namespace

int main(int argc, char** argv) {
    helios::core::platformInit(); // stops unless the CPU gate ran and passed (02 §1.1)
    const CommandLine cl = platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv);
    return run(cl);
}
