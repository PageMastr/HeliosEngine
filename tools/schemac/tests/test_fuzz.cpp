// Robustness of helios-schemac against malformed input (schemas and the lock are untrusted text):
// deterministic mutations of real schemas and of a real lock must yield diagnostics, never a crash,
// hang or sanitizer report. The generators run on every mutant that still compiles.

#include <fstream>
#include <sstream>

#include "helios/core/random.h"
#include "test_util.h"

using namespace schemac_test;

namespace {

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// One random edit: overwrite a byte with a syntax character, delete or duplicate a span, or insert
/// a construct that opens nesting or triggers a lint.
std::string mutate(std::string text, SplitMix64& rng) {
    static constexpr std::string_view kChars = "{}[]()<>;:,.@=?\"/\\*\n\t abcXYZ019-_#";
    static constexpr std::string_view kInserts[] = {
        "struct ",   "list<",        "map<u8, ",     "variant { A; B { x: u8 } }", "enum { A, B }", "server { ",
        "fn go();",  "scriptlib L {", "@was(\"\")",  "@range(0, inf)",            "@keyed",        "u8[",
        "?",         "= [",          "/*",           "///",                       "\"",            "import \"x.hschema\";",
        "alias Q = Q;", "record R {}", "@max(1_000)", "client->server",           "}",             "{ a: ",
    };
    if (text.empty()) return text;
    const auto pick = [&](usize n) { return static_cast<usize>(rng.next() % n); };
    for (usize edits = 1 + pick(3); edits > 0; --edits) {
        const usize at = pick(text.size());
        switch (pick(4)) {
        case 0: text[at] = kChars[pick(kChars.size())]; break;
        case 1: text.erase(at, 1 + pick(12)); break;
        case 2: text.insert(at, text.substr(at, 1 + pick(40))); break;
        default: text.insert(at, kInserts[pick(std::size(kInserts))]); break;
        }
        if (text.empty()) break;
    }
    return text;
}

} // namespace

TEST_CASE("fuzz: mutated schemas produce diagnostics, never crashes") {
    const std::string golden = readText(std::string(HELIOS_SCHEMAC_TEST_DIR) + "/golden/golden.hschema");
    const std::string common = readText(std::string(HELIOS_SOURCE_DIR) + "/schemas/sample/common.hschema");
    const std::string ship = readText(std::string(HELIOS_SOURCE_DIR) + "/schemas/sample/ship.hschema");
    REQUIRE(!golden.empty());
    REQUIRE(!ship.empty());
    constexpr int kSchemaMutants = 2000;
    SplitMix64 rng(0x5eed5c4e11a);
    usize compiled = 0;
    for (int i = 0; i < kSchemaMutants; ++i) {
        CompileOptions options;
        options.emitCpp = true;
        options.emitGo = true;
        options.emitJson = true;
        options.samples = true;
        std::unique_ptr<Compiled> c;
        if (i % 2 == 0) {
            c = compileText(mutate(golden, rng), options);
        } else {
            options.files = {"schemas/sample/ship.hschema"};
            c = compileFiles({{"schemas/sample/ship.hschema", mutate(ship, rng)}, {"schemas/sample/common.hschema", common}}, options);
        }
        if (c->ok()) {
            ++compiled;
            CHECK_FALSE(c->result.outputs.empty());
        } else {
            CHECK(c->diags.hasErrors()); // a failed compile always says why
        }
    }
    MESSAGE(compiled << " of " << kSchemaMutants << " mutants still compiled");
}

TEST_CASE("fuzz: mutated locks are rejected or applied, never crash") {
    const std::string schema = readText(std::string(HELIOS_SCHEMAC_TEST_DIR) + "/golden/golden.hschema");
    const std::string lock = readText(std::string(HELIOS_SCHEMAC_TEST_DIR) + "/golden/golden.lock.jsonc");
    REQUIRE(!lock.empty());
    SplitMix64 rng(0x10cc);
    for (int i = 0; i < 600; ++i) {
        MemoryFileSystem fs;
        fs.files["golden/golden.lock.jsonc"] = mutate(lock, rng);
        CompileOptions options;
        options.files = {"golden/golden.hschema"};
        options.includeDirs = {"golden"};
        options.lockPath = "golden/golden.lock.jsonc";
        options.emitCpp = true;
        auto c = compileFiles({{"golden/golden.hschema", schema}}, options, &fs);
        if (!c->ok()) CHECK(c->diags.hasErrors());
    }
}
