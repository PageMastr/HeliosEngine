// Golden-file tests of the generators: tests/golden/golden.hschema (every declaration kind and
// type form) compiled against its committed lock must reproduce tests/golden/*.expected byte for
// byte. The same schema is also compiled into this test binary and the Go interop test by
// helios_schema(), so the golden outputs are known to build.
//
// After an intended generator change, regenerate and review the diff:
//   HELIOS_UPDATE_GOLDEN=1 schemac_tests -tc="golden*"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "golden.gen.h"
#include "golden.samples.gen.h"
#include "helios/reflect/reflect.h"
#include "test_util.h"

using namespace schemac_test;

namespace {

const std::string kGoldenDir = std::string(HELIOS_SCHEMAC_TEST_DIR) + "/golden";

std::optional<std::string> readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    // Tolerate CRLF checkouts (the .gitattributes next to the fixtures asks for LF).
    std::erase(text, '\r');
    return text;
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool updateGolden() {
    const char* v = std::getenv("HELIOS_UPDATE_GOLDEN");
    return v && *v && std::string_view(v) != "0";
}

/// First differing line, for a readable failure message.
std::string firstDifference(const std::string& expected, const std::string& actual) {
    std::istringstream a(expected), b(actual);
    std::string la, lb;
    for (usize line = 1;; ++line) {
        const bool ha = static_cast<bool>(std::getline(a, la));
        const bool hb = static_cast<bool>(std::getline(b, lb));
        if (!ha && !hb) return "identical";
        if (!ha || !hb || la != lb) {
            return "line " + std::to_string(line) + ":\n  expected: " + (ha ? la : "<end of file>") + "\n  actual:   " + (hb ? lb : "<end of file>");
        }
    }
}

struct GoldenOutput {
    const char* suffix;   ///< output path suffix
    const char* expected; ///< file in tests/golden
};

constexpr GoldenOutput kOutputs[] = {
    {"cpp/golden.gen.h", "golden.gen.h.expected"},
    {"cpp/golden.gen.cpp", "golden.gen.cpp.expected"},
    {"go/golden.go", "golden.go.expected"},
    {"go/helios_codecs.go", "helios_codecs.go.expected"},
    {"golden.schema.json", "golden.schema.json.expected"},
};

} // namespace

TEST_CASE("golden: generator output matches the committed expectations") {
    const auto source = readText(kGoldenDir + "/golden.hschema");
    REQUIRE(source);
    const auto lock = readText(kGoldenDir + "/golden.lock.jsonc");
    REQUIRE_MESSAGE(lock, "tests/golden/golden.lock.jsonc is created by building schemac_tests; commit it");

    MemoryFileSystem fs;
    fs.files["golden/golden.lock.jsonc"] = *lock;
    CompileOptions options;
    options.files = {"golden/golden.hschema"};
    options.includeDirs = {"golden"};
    options.lockPath = "golden/golden.lock.jsonc";
    options.emitCpp = true;
    options.emitGo = true;
    options.emitJson = true;
    options.cppOut = "cpp";
    options.goOut = "go";
    options.jsonOut = "golden.schema.json";
    auto c = compileFiles({{"golden/golden.hschema", *source}}, options, &fs);
    REQUIRE_MESSAGE(c->ok(), c->messages);
    CHECK_MESSAGE(c->diags.diagnostics().empty(), c->messages);
    CHECK_MESSAGE(!c->result.lockChanged, "the golden lock is stale; rebuild schemac_tests (which updates it) and commit it");

    for (const GoldenOutput& g : kOutputs) {
        const std::string* actual = c->output(g.suffix);
        REQUIRE_MESSAGE(actual, std::string(g.suffix));
        const std::string path = kGoldenDir + "/" + g.expected;
        if (updateGolden()) {
            writeText(path, *actual);
            MESSAGE("updated " << path);
            continue;
        }
        const auto expected = readText(path);
        REQUIRE_MESSAGE(expected, "missing " << path << " (run with HELIOS_UPDATE_GOLDEN=1)");
        const bool same = *expected == *actual; // (a bool, so doctest does not print both files)
        CHECK_MESSAGE(same, std::string(g.expected) << " differs at " << firstDifference(*expected, *actual)
                                                    << "\n(if intended: HELIOS_UPDATE_GOLDEN=1 schemac_tests -tc=\"golden*\")");
    }
}

TEST_CASE("golden: generation is deterministic and independent of declaration-irrelevant input") {
    const auto source = readText(kGoldenDir + "/golden.hschema");
    REQUIRE(source);
    CompileOptions options;
    options.files = {"golden/golden.hschema"};
    options.includeDirs = {"golden"};
    options.emitCpp = true;
    options.emitGo = true;
    options.emitJson = true;
    auto a = compileFiles({{"golden/golden.hschema", *source}}, options);
    // Comments and whitespace do not change the output.
    std::string reformatted;
    for (char ch : *source) {
        reformatted += ch;
        if (ch == '\n') reformatted += "   // noise\n\n";
    }
    auto b = compileFiles({{"golden/golden.hschema", reformatted}}, options);
    REQUIRE_MESSAGE(a->ok(), a->messages);
    REQUIRE_MESSAGE(b->ok(), b->messages);
    REQUIRE(a->result.outputs.size() == b->result.outputs.size());
    for (usize i = 0; i < a->result.outputs.size(); ++i) {
        CHECK(a->result.outputs[i].path == b->result.outputs[i].path);
        const bool same = a->result.outputs[i].content == b->result.outputs[i].content;
        CHECK_MESSAGE(same, a->result.outputs[i].path);
    }
}

TEST_CASE("golden: the generated golden types round-trip in C++") {
    using helios::refl::Duration;
    using helios::refl::JsonStyle;
    using helios::refl::ReadCtx;
    using helios::refl::TypeInfo;
    using helios::refl::typeOf;
    namespace walk = helios::refl::walk;
    for (const auto& s : golden::all::samples::kSamples) {
        const TypeInfo& t = s.type();
        INFO(t.qualifiedName);
        helios::refl::Value v(t);
        s.fill(v.data());
        helios::refl::Value back(t);
        ReadCtx ctx;
        REQUIRE(helios::refl::fromJson(t, back.data(), helios::refl::toJson(t, v.data()), ctx));
        CHECK(helios::refl::equals(t, v.data(), back.data()));
        std::vector<helios::u8> bytes, walked;
        helios::refl::encodeTagged(t, v.data(), bytes);
        walk::encodeTagged(t, v.data(), walked);
        CHECK(bytes == walked);
        helios::refl::Value fromBytes(t);
        REQUIRE(helios::refl::decodeTagged(t, fromBytes.data(), bytes));
        CHECK(helios::refl::equals(t, v.data(), fromBytes.data()));
    }
    // Spot checks of the declarations.
    golden::all::Scalars s{};
    CHECK(s.i64v == INT64_MIN);
    CHECK(s.u64v == UINT64_MAX);
    CHECK(s.s == "quote\" backslash\\ tab\t \xC3\xA9");
    CHECK(s.perm == (golden::all::Perm::Read | golden::all::Perm::Exec));
    CHECK(s.d == Duration::fromSeconds(90));
    CHECK(helios::refl::toJson(s, JsonStyle::Compact) == "{}");
    CHECK(golden::all::Limit == 16u);
    CHECK(golden::all::Cooldown == Duration::fromMillis(1500));
    golden::all::Pair p{1.0f, 2.0f};
    CHECK(helios::refl::toJson(p, JsonStyle::Compact) == R"({"x":1,"y":2})");
    CHECK(typeOf<golden::all::Plain>().fields[0].was[0] == "old_value");
    auto plain = helios::refl::fromJson<golden::all::Plain>(R"({"old_value": 7})");
    REQUIRE(plain);
    CHECK(plain->value == 7);
    // The current name wins over the @was alias in any member order (compiled codec, walker and Go agree).
    for (const char* text : {R"({"value": 9, "old_value": 7})", R"({"old_value": 7, "value": 9})"}) {
        INFO(text);
        auto both = helios::refl::fromJson<golden::all::Plain>(text);
        REQUIRE(both);
        CHECK(both->value == 9);
        auto doc = helios::refl::JsonDocument::parse(text);
        REQUIRE(doc);
        golden::all::Plain walked{};
        ReadCtx ctx;
        REQUIRE(walk::readJson(typeOf<golden::all::Plain>(), &walked, doc->root(), ctx));
        CHECK(walked.value == 9);
    }
}
