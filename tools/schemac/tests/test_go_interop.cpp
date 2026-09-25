// Cross-language compatibility (ADR-004): the Go packages generated from schemas/sample and
// tests/golden must produce byte-identical tagged binary to the C++ codecs and read each other's
// JSON. The test writes the C++ encoding of every sample value to testdata/cpp_vectors.json in a
// temporary Go module holding the generated package, runs `go test` (vet + the generated round
// trip, corruption and TestHeliosCppVectors tests), then decodes the Go output
// (testdata/go_vectors.json) with the C++ codecs.
//
// Skipped (with a message) when no Go toolchain was found at configure time
// (HELIOS_GO_EXECUTABLE) or HELIOS_SKIP_GO=1. Runs offline (GOPROXY=off, GOTOOLCHAIN=local).

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "golden.gen.h"
#include "golden.samples.gen.h"
#include "helios/reflect/reflect.h"
#include "sample/common.samples.gen.h"
#include "sample/items.samples.gen.h"
#include "sample/ship.samples.gen.h"

using namespace helios;
using namespace helios::refl;
namespace fs = std::filesystem;

namespace {

struct Sample {
    const TypeInfo* type;
    void (*fill)(void*);
};

template <class Table>
void addSamples(std::vector<Sample>& out, const Table& table) {
    for (const auto& s : table) out.push_back({&s.type(), s.fill});
}

std::string hexOf(std::span<const u8> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (u8 b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::vector<u8> unhex(std::string_view text) {
    std::vector<u8> out;
    auto nibble = [](char c) -> int { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (usize i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<u8>(nibble(text[i]) << 4 | nibble(text[i + 1])));
    return out;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string quoteArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

/// Runs a command (arguments quoted), output to `log`. Returns the exit status.
int run(const std::vector<std::string>& args, const fs::path& log) {
    std::string cmd;
    for (const std::string& a : args) cmd += (cmd.empty() ? "" : " ") + quoteArg(a);
    cmd += " > " + quoteArg(log.string()) + " 2>&1";
#ifdef _WIN32
    cmd = "\"" + cmd + "\""; // cmd.exe strips one level of quotes
#endif
    return std::system(cmd.c_str());
}

bool goAvailable() {
    const char* skip = std::getenv("HELIOS_SKIP_GO");
    if (skip && *skip && std::string_view(skip) != "0") return false;
    const std::string go = HELIOS_GO_EXECUTABLE;
    return !go.empty() && fs::exists(go);
}

/// Copies the generated package into a fresh module, writes the C++ vectors, runs go vet + go test
/// and checks the Go vectors in C++.
void checkPackage(const std::string& name, const fs::path& goDir, const std::vector<Sample>& samples,
                  const std::string& extraGoTest = {}) {
    INFO("Go package " << name);
    const fs::path work = fs::path(HELIOS_TEST_WORK_DIR) / ("go_" + name);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "testdata");
    usize copied = 0;
    for (const auto& entry : fs::directory_iterator(goDir)) {
        if (entry.path().extension() == ".go") {
            fs::copy_file(entry.path(), work / entry.path().filename(), fs::copy_options::overwrite_existing);
            ++copied;
        }
    }
    REQUIRE(copied >= 4);
    {
        std::ofstream mod(work / "go.mod", std::ios::binary);
        mod << "module heliosgen\n\ngo 1.22\n";
    }
    if (!extraGoTest.empty()) {
        std::ofstream extra(work / "extra_test.go", std::ios::binary);
        extra << extraGoTest;
    }

    // C++ vectors.
    std::map<std::string, std::pair<std::vector<u8>, const Sample*>> cpp;
    JsonWriter w;
    w.beginObject();
    w.key("vectors");
    w.beginArray();
    for (const Sample& s : samples) {
        Value v(*s.type);
        s.fill(v.data());
        std::vector<u8> bytes;
        encodeTagged(*s.type, v.data(), bytes);
        w.beginObject();
        w.key("type");
        w.string(s.type->qualifiedName);
        w.key("tagged");
        w.string(hexOf(bytes));
        w.key("json");
        w.raw(toJson(*s.type, v.data(), JsonStyle::Compact));
        w.endObject();
        cpp[std::string(s.type->qualifiedName)] = {std::move(bytes), &s};
    }
    w.endArray();
    w.endObject();
    {
        std::ofstream out(work / "testdata" / "cpp_vectors.json", std::ios::binary);
        out << w.take();
    }

    const std::vector<std::string> env = {HELIOS_CMAKE_COMMAND, "-E", "chdir", work.string(), HELIOS_CMAKE_COMMAND, "-E", "env",
                                          "GOFLAGS=", "GOPROXY=off", "GOWORK=off", "GOTOOLCHAIN=local", "GO111MODULE=on",
                                          HELIOS_GO_EXECUTABLE};
    auto withEnv = [&](std::initializer_list<std::string> tail) {
        std::vector<std::string> args = env;
        args.insert(args.end(), tail);
        return args;
    };
    const fs::path vetLog = work / "vet.log";
    const int vet = run(withEnv({"vet", "."}), vetLog);
    CHECK_MESSAGE(vet == 0, "go vet failed:\n" << readFile(vetLog));
    const fs::path testLog = work / "test.log";
    const int test = run(withEnv({"test", "-count=1", "-v", "."}), testLog);
    const std::string log = readFile(testLog);
    REQUIRE_MESSAGE(test == 0, "go test failed:\n" << log);
    CHECK_MESSAGE(log.find("--- PASS: TestHeliosCppVectors") != std::string::npos, log);
    CHECK(log.find("--- SKIP") == std::string::npos);

    // Go vectors, checked in C++.
    const std::string goText = readFile(work / "testdata" / "go_vectors.json");
    auto doc = JsonDocument::parse(goText);
    REQUIRE_MESSAGE(doc, goText);
    usize checked = 0;
    for (const JsonValue vec : doc->root().get("vectors").elements()) {
        const std::string type(vec.get("type").asString());
        INFO(type);
        auto it = cpp.find(type);
        REQUIRE(it != cpp.end());
        const Sample& s = *it->second.second;
        const std::vector<u8> goBytes = unhex(vec.get("tagged").asString());
        CHECK(goBytes == it->second.first); // byte-identical
        Value want(*s.type);
        s.fill(want.data());
        Value fromBytes(*s.type);
        auto r = decodeTagged(*s.type, fromBytes.data(), goBytes);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        CHECK(equals(*s.type, want.data(), fromBytes.data()));
        // Go's encoding/json output (compact, Go float formatting, \u escapes) reads back in C++.
        Value fromJsonValue(*s.type);
        ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
        auto j = readJson(*s.type, fromJsonValue.data(), vec.get("json"), ctx);
        REQUIRE_MESSAGE(j, (j ? std::string() : j.error().toString()));
        CHECK(equals(*s.type, want.data(), fromJsonValue.data()));
        ++checked;
    }
    CHECK(checked == samples.size());
}

} // namespace

TEST_CASE("go interop: generated Go matches the C++ codecs byte for byte") {
    if (!goAvailable()) {
        MESSAGE("skipped: no Go toolchain (configure found none, or HELIOS_SKIP_GO=1)");
        return;
    }
    std::vector<Sample> sample;
    addSamples(sample, sample::common::samples::kSamples);
    addSamples(sample, sample::ship::samples::kSamples);
    addSamples(sample, sample::items::samples::kSamples);
    checkPackage("sample", HELIOS_SAMPLE_GO_DIR, sample);

    std::vector<Sample> golden;
    addSamples(golden, golden::all::samples::kSamples);
    // Hand-written Go checks on the golden package: @was keys and the reference prefixes.
    const std::string extra = R"(package all

import (
	"encoding/json"
	"errors"
	"os"
	"testing"
)

// Go writes nil slices, maps and TagSets as null; the C++ test reads this file back.
func TestWriteNullContainers(t *testing.T) {
	c := NewContainers()
	c.Me = map[Color][]uint64{ColorRed: nil, ColorBlue: {7}}
	c.M = map[string]Scalars{"k": NewScalars()}
	b, err := json.Marshal(c)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile("testdata/go_nulls.json", b, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestHandWritten(t *testing.T) {
	var p Plain
	if err := json.Unmarshal([]byte(`{"old_value": 7}`), &p); err != nil || p.Value != 7 {
		t.Fatalf("@was key: %v %+v", err, p)
	}
	if err := json.Unmarshal([]byte(`{"old_value": 7, "value": 9}`), &p); err != nil || p.Value != 9 {
		t.Fatalf("current key wins: %v %+v", err, p)
	}
	if err := json.Unmarshal([]byte(`{"value": 9, "old_value": 7}`), &p); err != nil || p.Value != 9 {
		t.Fatalf("current key wins (either order): %v %+v", err, p)
	}
	// Tagged strings must be valid UTF-8 (same rule as the C++ reader).
	sc := NewScalars()
	sc.S = "ok \xc3\xa9"
	var decoded Scalars
	if err := decoded.UnmarshalHelios(sc.MarshalHelios()); err != nil || decoded.S != sc.S {
		t.Fatalf("valid UTF-8: %v %q", err, decoded.S)
	}
	for _, bad := range []string{"\xff", "a\xc3", "\xed\xa0\x80", "\xc0\xaf"} {
		sc.S = bad
		if err := decoded.UnmarshalHelios(sc.MarshalHelios()); !errors.Is(err, ErrCorrupt) {
			t.Fatalf("invalid UTF-8 %q accepted: %v", bad, err)
		}
	}
	// Durations in seconds must fit int64 nanoseconds (the C++ reader rejects the same values).
	var d Duration
	if err := json.Unmarshal([]byte(`1.5`), &d); err != nil || d != Duration(1500000000) {
		t.Fatalf("seconds: %v %d", err, d)
	}
	if err := json.Unmarshal([]byte(`1e10`), &d); err == nil {
		t.Fatalf("out-of-range seconds accepted: %d", d)
	}
	// Sets encode canonically (sorted, no duplicates), like a C++ std::set.
	dup := NewContainers()
	dup.St = []string{"b", "a", "b"}
	unique := NewContainers()
	unique.St = []string{"a", "b"}
	if string(dup.MarshalHelios()) != string(unique.MarshalHelios()) {
		t.Fatalf("set with duplicates encodes differently")
	}
	// TagSets too, even when built out of order.
	tags := NewScalars()
	tags.Tags = TagSet{"b.x", "a.y", "b.x"}
	sortedTags := NewScalars()
	sortedTags.Tags = TagSet{"a.y", "b.x"}
	if string(tags.MarshalHelios()) != string(sortedTags.MarshalHelios()) {
		t.Fatalf("unsorted TagSet encodes differently")
	}
	// GUID text forms accepted by helios::Guid::parse.
	g, err := ParseGuid("{00112233-4455-6677-8899-aabbccddeeff}")
	if err != nil || g.String() != "00112233-4455-6677-8899-aabbccddeeff" {
		t.Fatalf("braced GUID: %v %s", err, g)
	}
	c := NewChat()
	c.From = 42
	b, _ := json.Marshal(c)
	if string(b) != `{"from":"ent:42","text":""}` {
		t.Fatalf("EntityId JSON: %s", b)
	}
	var back Chat
	if err := json.Unmarshal([]byte(`{"from": 42}`), &back); err != nil || back.From != 42 {
		t.Fatalf("bare EntityId: %v", err)
	}
	var th ThingDef
	if err := json.Unmarshal([]byte(`{"label": "ui.name"}`), &th); err != nil || th.Label != "ui.name" {
		t.Fatalf("bare LocString: %v %q", err, th.Label)
	}
	if b, _ := json.Marshal(th.Label); string(b) != `"loc:ui.name"` {
		t.Fatalf("LocString JSON: %s", b)
	}
}
)";
    checkPackage("golden", HELIOS_GOLDEN_GO_DIR, golden, extra);

    // Go's encoding/json writes empty (nil) slices, maps and TagSets as null; the C++ readers
    // (compiled and walker) take null as an empty container.
    const std::string nulls = readFile(fs::path(HELIOS_TEST_WORK_DIR) / "go_golden" / "testdata" / "go_nulls.json");
    INFO(nulls);
    REQUIRE(nulls.find("null") != std::string::npos);
    auto compiled = fromJson<golden::all::Containers>(nulls);
    REQUIRE_MESSAGE(compiled, (compiled ? std::string() : compiled.error().toString()));
    CHECK(compiled->me.at(golden::all::Color::Red).empty());
    CHECK(compiled->me.at(golden::all::Color::Blue) == std::vector<u64>{7});
    CHECK(compiled->m.at("k") == golden::all::Scalars{});
    auto doc = JsonDocument::parse(nulls);
    REQUIRE(doc);
    golden::all::Containers walked{};
    ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
    auto r = walk::readJson(typeOf<golden::all::Containers>(), &walked, doc->root(), ctx);
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(walked == *compiled);

}
