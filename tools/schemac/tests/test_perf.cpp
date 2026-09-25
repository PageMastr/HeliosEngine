// 02 §3.5: regenerating a schema set of 2,000 types takes ≤ 1 s. The budget is asserted in
// optimized builds without sanitizers; other builds only report the time.

#include <chrono>
#include <format>

#include "test_util.h"

using namespace schemac_test;

namespace {

/// ~2,000 types over 20 files: per entry an enum, a struct with an inline struct holding a variant
/// (+2 alternatives), a record and a replicated component with a server part (9 types).
std::map<std::string, std::string> bigSchemaSet() {
    std::map<std::string, std::string> files;
    for (int f = 0; f < 20; ++f) {
        std::string text = std::format("package perf.p{};\n", f);
        if (f > 0) text += std::format("import \"perf/f{}.hschema\";\n", f - 1);
        for (int i = 0; i < 12; ++i) {
            const std::string n = std::format("T{}_{}", f, i);
            text += std::format("enum {}Kind : u8 {{ A; B; C; D }}\n", n);
            text += std::format("/// Struct {}.\nstruct {} {{\n", n, n);
            text += "  a: u32 = 7 @range(0, 100)\n  b: string\n  c: list<f32>\n  d: map<Name, i64>\n";
            text += std::format("  k: {}Kind = C\n  v: vec3f = [0, 0, 1]\n  o: u64?\n", n);
            text += "  inner: { x: f32; y: f32; z: variant { P; Q { w: u16 } } }\n";
            if (f > 0) text += std::format("  prev: perf.p{}.T{}_{}\n", f - 1, f - 1, i);
            text += "}\n";
            text += std::format("record {}Def {{ name: LocString; value: f64 = 1.5; t: {} }}\n", n, n);
            text += std::format("component {}C replicate(all) {{ hp: f32; pos: WorldPos; server {{ secret: u32 }} }}\n", n);
        }
        files.emplace(std::format("schemas/perf/f{}.hschema", f), std::move(text));
    }
    return files;
}

} // namespace

TEST_CASE("perf: 2,000 types regenerate within the 02 §3.5 budget") {
    const auto files = bigSchemaSet();
    CompileOptions options;
    for (const auto& [path, text] : files) options.files.push_back(path);
    options.emitCpp = true;
    options.emitGo = false; // one Go package per schema set; the 20 files share names here
    options.emitJson = true;
    options.lockPath = "schemas/perf/schema.lock.jsonc";

    // First run creates the lock; the timed run is the incremental rebuild (lock present).
    MemoryFileSystem fs;
    auto first = compileFiles(files, options, &fs);
    REQUIRE_MESSAGE(first->ok(), first->messages.substr(0, 2000));
    fs.files[options.lockPath] = first->result.lockText;
    usize types = 0;
    for (const Decl* d : first->schema().decls) types += d->isLockable() ? 1 : 0;
    CHECK(types >= 2000);

    const auto start = std::chrono::steady_clock::now();
    auto again = compileFiles(files, options, &fs);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE(again->ok());
    CHECK_FALSE(again->result.lockChanged);
    usize bytes = 0;
    for (const OutputFile& o : again->result.outputs) bytes += o.content.size();
    MESSAGE(std::format("{} types, {} outputs ({} KiB) in {:.3f} s", types, again->result.outputs.size(), bytes / 1024, elapsed));
#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
    CHECK(elapsed <= 1.0);
#endif
}
