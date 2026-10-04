// records_fixture_cook: `helios-cook records` over the engine/records test types (test only, never
// shipped). The sample schemas helios-cook links have no `server {}` field inside a shared struct, so the
// AAA-SEC-4 lint over nested plantings (lint_records_sec4_nested) cooks tests/data/project with this.
//
//   records_fixture_cook records --project-root=<dir> --out=<dir>
//
// Exit codes as helios-cook's: 0 ok, 1 the cook found errors (listed on stderr), 2 usage error, 3 I/O or
// setup failure.

#include <cstdio>
#include <format>
#include <string>

#include "gameplay/tags.gen.h"
#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/platform.h"
#include "helios/records/cook.h"
#include "records_test.gen.h"

using namespace helios;

namespace {

int fail(int code, const std::string& message) {
    std::fprintf(stderr, "records_fixture_cook: %s\n", message.c_str());
    return code;
}

int run(const CommandLine& cl) {
    if (cl.positional().size() != 1 || cl.positional()[0] != "records" || !cl.value("project-root") || !cl.value("out")) {
        return fail(2, "usage: records_fixture_cook records --project-root=<dir> --out=<dir>");
    }
    refl::TypeRegistry registry;
    if (auto r = ::test::records::registerRecordsTestTypes(registry); !r) return fail(3, std::format("{}", r.error()));
    if (auto r = gameplay::registerTagsTypes(registry); !r) return fail(3, std::format("{}", r.error()));
    auto sources = records::collectSources(fs::pathFromUtf8(*cl.value("project-root")), registry);
    if (!sources) return fail(1, sources.error().message);
    std::vector<records::CookDiagnostic> diags;
    auto out = records::cook(*sources, {}, &diags);
    if (!out) {
        for (const records::CookDiagnostic& d : diags) std::fprintf(stderr, "%s: %s\n", d.path.c_str(), d.message.c_str());
        return fail(1, std::format("{} error(s); nothing written", diags.size()));
    }
    if (auto r = records::writeCookOutput(*out, fs::pathFromUtf8(*cl.value("out"))); !r) return fail(3, std::format("{}", r.error()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return run(platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv));
}
