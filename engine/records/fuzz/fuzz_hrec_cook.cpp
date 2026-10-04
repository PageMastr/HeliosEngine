// Fuzz target: the `.hrec` side of the records cook (02 §3.3, §3.7): JSONC parsing through reflect,
// the `$` header, `$parent` inheritance with its keyed and appending merges, the reference, tag, enum
// and formula checks, the AAA-SEC-4 layout rules and the encoders.
//
// Input: one selector byte (the record type: ShipDef, TreeDef, PartDef or LootDef), then the JSONC text
// of one record file. It is cooked together with fixed records (a part, a server-only loot table, a
// client-only skin and a ship template the input may name as its `$parent`). Properties: cook() returns a
// Result (no crash, sanitizer report or unbounded work); when it succeeds, both cooks open with the loader
// (the cooker never writes what the loader refuses) and every record decodes. Seeds (fuzz/corpus/hrec_cook,
// `--make-seeds`) are the test project's records.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/records/records.h"
#include "helios/reflect/serialize.h"
#include "records_test.gen.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::records;

constexpr usize kMaxInput = 64 * kKiB;

[[noreturn]] void fail() { std::abort(); }

const refl::TypeRegistry& registry() {
    static refl::TypeRegistry* reg = [] {
        auto* r = new refl::TypeRegistry();
        if (!::test::records::registerRecordsTestTypes(*r)) fail();
        return r;
    }();
    return *reg;
}

constexpr const char* kTypes[] = {"test.records.ShipDef", "test.records.TreeDef", "test.records.PartDef", "test.records.LootDef"};

const refl::TypeInfo& type(std::string_view name) {
    const refl::TypeInfo* t = registry().find(name);
    if (!t) fail();
    return *t;
}

std::vector<SourceRecord> fixedSources() {
    return {
        {"part.hrec", &type("test.records.PartDef"), R"({"$rid": 1, "$name": "p/1", "name": "loc:p1", "mass": 2.5})"},
        {"loot.hrec", &type("test.records.LootDef"), R"({"$rid": 2, "$name": "l/1", "entries": ["a"], "secret": "s"})"},
        {"skin.hrec", &type("test.records.SkinDef"), R"({"$rid": 3, "$name": "k/1", "palette": [[1, 0, 0, 1]]})"},
        {"base.hrec", &type("test.records.ShipDef"), R"j({
  "$rid": 5, "$name": "ship/base", "mass": 3.5, "tags": ["A.B"], "formula": "attr(self, X) * 2",
  "mounts": [{"$key": "00000000-0000-4000-8000-000000000001", "bone": "m", "force": 1}],
  "slots": [{"slot": "x", "grade": "Low", "weight": 1}], "extras": ["e"], "parts": [1], "attrs": {"k": 1},
  "labels": ["z", "y"], "mode": {"Warp": {"target": "t"}}, "nested": [[1], []], "drop": 2, "skin": 3, "loot": 2,
  "aiHints": {"h": 1}, "serverTags": ["S.T"], "threat": "attr(self, T)"})j"},
    };
}

// Logging is quiet while fuzzing (the driver restores it for its summary).
const bool g_quietLogs = (log::setLevel(log::Level::Error), true);

void loadAll(std::span<const u8> bytes) {
    auto db = RecordDb::openBytes(bytes, registry());
    if (!db) fail(); // the cooker wrote something its own loader refuses
    for (usize i = 0; i < db->recordCount(); ++i) {
        const RecordView r = db->record(i);
        refl::Value obj(*r.type);
        if (!db->decode(r, obj.data())) fail();
    }
}

} // namespace

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    auto add = [&](uint8_t selector, std::string_view text) {
        std::vector<uint8_t> in{selector};
        in.insert(in.end(), text.begin(), text.end());
        out.push_back(std::move(in));
    };
    add(0, R"({"$rid": 9, "$name": "ship/child", "$parent": "ship/base", "mass": 1, "extras": ["f"], "maybe": null,
  "mounts": [{"$key": "00000000-0000-4000-8000-000000000001", "force": 2}, {"$key": "00000000-0000-4000-8000-000000000002"}],
  "slots": [{"slot": "x", "weight": 3}, {"slot": "y"}], "handling": {"yaw": 2}, "mode": "Idle"})");
    add(0, R"j({"$rid": 10, "$name": "ship/full", "name": "loc:s", "grade": "High", "perms": ["Read", "Admin"], "hp": 1e300,
  "i8v": -1, "u64v": 18446744073709551615, "flag": true, "label": "x", "ident": "i", "guid": "guid:01020304-0506-4708-890a-0b0c0d0e0f10",
  "owner": "ent:9", "net": 10, "cooldown": "11ms", "at": 12, "pos": [1, 2, 3], "rot": [0, 0, 0, 1], "tint": [1, 1, 1, 1],
  "tags": ["Q.R"], "query": "all(Q) none(A.B)", "formula": "formula F(a, b) = attr(a, X) + select(tag(b, Q.R), 1, 0)",
  "nums": [1, 2, 3], "roster": {"seats": [{"$key": "00000000-0000-4000-8000-000000000003"}]}, "secretCode": 7})j");
    add(1, R"({"$rid": 11, "$name": "tree/t", "root": {"value": 1, "kids": [{"value": 2, "kids": [{"value": 3}]}]}})");
    add(2, R"({"$rid": 12, "$name": "part/q", "mass": 4})");
    add(3, R"({"$rid": 13, "$name": "loot/m", "entries": ["x", "y"], "weight": 0.5})");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > kMaxInput) return 0;
    std::vector<SourceRecord> sources = fixedSources();
    sources.push_back(SourceRecord{"fuzz.hrec", &type(kTypes[data[0] % 4]), std::string(reinterpret_cast<const char*>(data + 1), size - 1)});
    auto out = cook(sources);
    if (!out) return 0;
    loadAll(out->client);
    loadAll(out->server);
    return 0;
}
