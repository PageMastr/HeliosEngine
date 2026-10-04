// Fuzz target: the `.hrdb` loader (02 §3.7: "loaders validate bounds and are fuzzed").
//
// Each input runs twice: as given, and with its checksums recomputed (hrdb::seal), so mutated fields
// reach the validators behind the header and content checksums. Properties: openBytes() returns a
// Result (no crash, sanitizer report or out-of-bounds read); for a database that opened, every record
// is found by id and by name at its own index, decodes into an object of its type, and every cooked
// value can be read through the views (HXL bytecode is decoded too, which verifies it); every tag whose
// name the cook carries is found by name at its index. The seeds (fuzz/corpus/hrdb_loader, written by
// `--make-seeds`) are cooks of small projects of the test types: every encoding, inheritance, tags,
// formulas, both audiences.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "helios/core/log.h"
#include "helios/hxl/program.h"
#include "helios/records/records.h"
#include "helios/reflect/serialize.h"
#include "records_test.gen.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::records;

constexpr usize kMaxInput = 1 * kMiB;

[[noreturn]] void fail() { std::abort(); }

const refl::TypeRegistry& registry() {
    static refl::TypeRegistry* reg = [] {
        auto* r = new refl::TypeRegistry();
        if (!::test::records::registerRecordsTestTypes(*r)) fail();
        return r;
    }();
    return *reg;
}

u64 g_sink = 0;

void touch(const ValueView& v) {
    switch (v.enc()) {
    case Enc::Bool: g_sink += v.asBool(); return;
    case Enc::Int:
    case Enc::EntityId:
    case Enc::NetHandle:
    case Enc::Duration:
    case Enc::RecordRef: g_sink += v.asUInt(); return;
    case Enc::F32:
    case Enc::F64: g_sink += v.asFloat() > 0.0 ? 1 : 0; return;
    case Enc::Text: g_sink += v.asText().size(); return;
    case Enc::Hxl: {
        g_sink += v.asText().size();
        if (!v.hxlBytecode().empty()) g_sink += hxl::Program::decode(v.hxlBytecode()).ok() ? 1 : 0;
        return;
    }
    case Enc::TagSet:
        for (const TagIndex t : v.tags()) g_sink += t;
        return;
    case Enc::Guid: g_sink += v.asGuid().low; return;
    case Enc::Struct:
        for (const CookedLayout::Field& f : v.layout().fields) touch(v.field(f));
        return;
    case Enc::List:
    case Enc::KeyedList:
    case Enc::Set:
    case Enc::Array:
        for (usize i = 0; i < v.size(); ++i) {
            touch(v[i]);
            if (v.enc() == Enc::KeyedList) g_sink += v.keyAt(i).high;
        }
        return;
    case Enc::Map:
        for (usize i = 0; i < v.size(); ++i) {
            touch(v.mapKey(i));
            touch(v.mapValue(i));
        }
        return;
    case Enc::Optional:
        if (v.hasValue()) touch(v.value());
        return;
    case Enc::Variant: touch(v.alternativeValue()); return;
    }
}

void exercise(const u8* data, usize size) {
    auto db = RecordDb::openBytes(std::span<const u8>(data, size), registry());
    if (!db) return;
    for (usize i = 0; i < db->recordCount(); ++i) {
        const RecordView r = db->record(i);
        if (!r || db->find(r.id).id != r.id || db->findByName(r.name).id != r.id) fail();
        touch(r.value);
        refl::Value obj(*r.type);
        if (!db->decode(r, obj.data())) fail();
    }
    for (usize i = 0; i < db->tagCount(); ++i) {
        const TagView t = db->tag(static_cast<TagIndex>(i));
        if (!t.withheld && db->findTag(t.name) != i) fail();
    }
}

// Logging is quiet while fuzzing (the driver restores it for its summary).
const bool g_quietLogs = (log::setLevel(log::Level::Error), true);

SourceRecord src(std::string path, std::string_view type, std::string text) {
    const refl::TypeInfo* t = registry().find(type);
    if (!t) fail();
    return SourceRecord{std::move(path), t, std::move(text)};
}

void addCook(std::vector<std::vector<uint8_t>>& out, const std::vector<SourceRecord>& sources) {
    auto cooked = cook(sources);
    if (!cooked) fail();
    out.push_back(cooked->client);
    out.push_back(cooked->server);
}

} // namespace

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    // An empty project, then one record of each test type with every encoding set, then inheritance.
    addCook(out, {});
    addCook(out, {
        src("part.hrec", "test.records.PartDef", R"({"$rid": 1, "$name": "p/1", "name": "loc:p1", "mass": 2.5})"),
        src("loot.hrec", "test.records.LootDef", R"({"$rid": 2, "$name": "l/1", "entries": ["a", "b"], "secret": "s", "weight": 1})"),
        src("skin.hrec", "test.records.SkinDef", R"({"$rid": 3, "$name": "k/1", "palette": [[1, 0, 0, 1]]})"),
        src("tree.hrec", "test.records.TreeDef",
            R"({"$rid": 4, "$name": "t/1", "root": {"value": 1, "kids": [{"value": 2, "kids": [{"value": 3}]}, {"value": 4}]}})"),
        src("ship.hrec", "test.records.ShipDef", R"j({
  "$rid": 5, "$name": "s/1", "name": "loc:ship", "grade": "High", "perms": ["Read", "Admin"], "mass": 3.5, "hp": -1e300,
  "i8v": -1, "i16v": -2, "i32v": -3, "i64v": -4, "u8v": 5, "u16v": 6, "u32v": 7, "u64v": 8, "flag": true, "label": "label é",
  "ident": "ident", "guid": "guid:01020304-0506-4708-890a-0b0c0d0e0f10", "owner": "ent:9", "net": 10, "cooldown": "11ms",
  "at": 12, "pos": [1, 2, 3], "rot": [0, 0, 0, 1], "tint": [1, 1, 1, 1], "icon": "guid:01020304-0506-4708-890a-0b0c0d0e0f11",
  "tags": ["A.B", "A.C.D"], "query": "all(A)", "formula": "attr(self, X) * 2 + select(tag(self, A.B), 1, 0)",
  "mounts": [{"$key": "00000000-0000-4000-8000-000000000001", "bone": "m", "force": 1}],
  "slots": [{"slot": "x", "grade": "Low", "weight": 1}], "extras": ["e"], "parts": [1], "attrs": {"k": 1, "j": 2},
  "labels": ["z", "y"], "nums": [1, 2, 3], "maybe": 4, "handling": {"pitch": 1, "yaw": 2, "roll": 3},
  "mode": {"Warp": {"target": "t", "spool": "1s"}}, "nested": [[1], [], [2, 3]], "drop": 2, "skin": 3, "hudColor": [0, 1, 0, 1],
  "loot": 2, "aiNotes": "n", "aiHints": {"h": 1}, "serverTags": ["S.T"], "threat": "attr(self, T)", "secretCode": 13
})j"),
        src("child.hrec", "test.records.ShipDef",
            R"({"$rid": 6, "$name": "s/2", "$parent": "s/1", "mass": 1, "extras": ["f"], "mode": "Idle", "maybe": null})"),
    });
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInput) return 0;
    exercise(data, size);
    std::vector<u8> resealed(data, data + size);
    hrdb::seal(resealed);
    exercise(resealed.data(), resealed.size());
    return 0;
}
