// The Luau type-definition stub (defs/helios.d.luau): it must parse with declaration syntax and
// declare every built-in API name the VM registers (the TOOL-7 doc-coverage idea applied to Luau).

#include <fstream>
#include <sstream>

#include "Luau/Parser.h"
#include "script_test_util.h"

using namespace helios;
using namespace helios::script;
using helios::script::test::Harness;

namespace {

std::string readDefs() {
    std::ifstream in(HELIOS_SCRIPT_DEFS_PATH, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Position of `word` as a whole identifier at or after `from`, or npos.
usize findWord(const std::string& text, std::string_view word, usize from = 0) {
    for (usize pos = text.find(word, from); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        const bool startOk = pos == 0 || !isIdentChar(text[pos - 1]);
        const usize end = pos + word.size();
        const bool endOk = end >= text.size() || !isIdentChar(text[end]);
        if (startOk && endOk) return pos;
    }
    return std::string::npos;
}

} // namespace

TEST_CASE("defs: helios.d.luau parses with declaration syntax") {
    const std::string src = readDefs();
    REQUIRE_FALSE(src.empty());
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseOptions options;
    options.allowDeclarationSyntax = true;
    const Luau::ParseResult result = Luau::Parser::parse(src.data(), src.size(), names, allocator, options);
    for (const Luau::ParseError& e : result.errors) {
        FAIL_CHECK("helios.d.luau:", e.getLocation().begin.line + 1, ": ", e.what());
    }
    CHECK(result.errors.empty());
    CHECK(result.root != nullptr);
}

TEST_CASE("defs: every built-in API name is declared in helios.d.luau") {
    const std::string src = readDefs();
    Harness h;
    const std::vector<std::string> manifest = h.vm->apiManifest();
    CHECK(manifest.size() >= 20);
    for (const std::string& name : manifest) {
        INFO(name);
        const usize sep = name.find_first_of(".:");
        if (sep == std::string::npos) {
            CHECK(findWord(src, name) != std::string::npos);
            continue;
        }
        const std::string owner = name.substr(0, sep);
        const std::string member = name.substr(sep + 1);
        const usize ownerPos = findWord(src, owner);
        REQUIRE(ownerPos != std::string::npos);
        CHECK(findWord(src, member, ownerPos) != std::string::npos);
    }
}
