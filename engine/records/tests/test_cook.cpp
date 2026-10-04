// The records cook: round trips, inheritance (02 §3.3), references, formulas, tags, determinism.
#include <format>
#include <random>

#include "helios/core/hash.h"
#include "helios/gameplay/tags.h"
#include "helios/hxl/hxl.h"
#include "test_util.h"

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;
using namespace ::test::records;

namespace {

constexpr refl::RecordId kBase = 4390381077356775243ull;
constexpr refl::RecordId kSkiff = 8721150164252052334ull;
constexpr refl::RecordId kSkiffMk2 = 872142931993027468ull;
constexpr refl::RecordId kWren = 5505769937651519472ull;
constexpr refl::RecordId kHullPlate = 499464797028555293ull;

const ShipDef& ship(const refl::Value& v) { return *static_cast<const ShipDef*>(v.data()); }

TEST_CASE("cook: the test project cooks, and records without $parent read back as refl::readRecord reads them") {
    const std::vector<SourceRecord> sources = projectSources();
    REQUIRE(sources.size() == 11);
    const CookOutput out = cookProject();
    CHECK(out.stats.records == 11);
    CHECK(out.stats.clientRecords == 10); // all but the server-only loot table
    CHECK(out.stats.serverRecords == 10); // all but the client-only skin
    CHECK(out.stats.inherited == 2);
    CHECK(out.stats.formulas == 4);
    const RecordDb client = open(out.client);
    const RecordDb server = open(out.server);
    CHECK(client.audience() == CookAudience::Client);
    CHECK(server.audience() == CookAudience::Server);
    CHECK(client.recordCount() == 10);
    CHECK(server.recordCount() == 10);
    for (const SourceRecord& s : sources) {
        refl::Value expected(*s.type);
        refl::RecordHeader header;
        refl::ReadCtx ctx;
        REQUIRE(refl::readRecord(*s.type, expected.data(), s.text, header, ctx).ok());
        if (!header.parent.empty()) continue;
        for (const RecordDb* db : {&client, &server}) {
            const RecordView r = db->find(header.rid);
            if (excludesType(*s.type, db->audience())) {
                CHECK_FALSE(r);
                continue;
            }
            REQUIRE_MESSAGE(r, s.path);
            CHECK(r.name == header.name);
            CHECK(r.type == s.type);
            CHECK(db->findByName(header.name).id == header.rid);
            refl::Value want(*s.type);
            s.type->ops->copy(want.data(), expected.data());
            strip(*s.type, want.data(), db->audience());
            const refl::Value got = decoded(*db, r);
            CHECK_MESSAGE(refl::equals(*s.type, got.data(), want.data()),
                          s.path << " (" << cookAudienceName(db->audience()) << ")\n got: " << refl::toJson(*s.type, got.data())
                                 << "\nwant: " << refl::toJson(*s.type, want.data()));
        }
    }
    CHECK_FALSE(client.findByName("ship/nope"));
    CHECK_FALSE(client.find(12345));
}

TEST_CASE("cook: $parent inheritance overrides fields, merges structs and keyed lists, appends @merge(append)") {
    const CookOutput out = cookProject();
    const RecordDb db = open(out.server);
    const refl::Value baseV = decoded(db, db.find(kBase));
    const refl::Value kV = decoded(db, db.find(kSkiff));
    const ShipDef& base = ship(baseV);
    const ShipDef& k = ship(kV);
    CHECK(k.name.key == "ship.skiff.name");
    CHECK(k.mass == 9000.5f);                                  // override
    CHECK(k.label == base.label);                              // inherited
    CHECK(k.tags == base.tags);
    CHECK(k.handling.pitch == 45.0f);                          // nested struct: merged field by field
    CHECK(k.handling.yaw == 60.0f);
    CHECK(k.handling.roll == 90.0f);
    // Keyed list by $key: ...02 merged (force overridden, bone and dir inherited), ...03 appended.
    REQUIRE(k.mounts.size() == 3);
    CHECK(k.mounts.keyAt(0) == base.mounts.keyAt(0));
    CHECK(k.mounts[0].force == 250000.0f);
    CHECK(k.mounts[1].bone == Name("thruster_left"));
    CHECK(k.mounts[1].force == 45000.0f);
    CHECK(k.mounts[1].dir == Vec3(-1.0f, 0.0f, 0.0f));
    CHECK(k.mounts.keyAt(2) == *Guid::parse("6b657374-6d6f-756e-8000-000000000003"));
    CHECK(k.mounts[2].bone == Name("thruster_right"));
    CHECK(k.mounts[2].force == 10.0f); // the element default
    // @keyed(slot): dorsal merged, ventral appended.
    REQUIRE(k.slots.size() == 3);
    CHECK(k.slots[0].slot == Name("nose"));
    CHECK(k.slots[1].slot == Name("dorsal"));
    CHECK(k.slots[1].grade == Grade::Mid);
    CHECK(k.slots[1].weight == 5.0f);
    CHECK(k.slots[2].slot == Name("ventral"));
    CHECK(k.slots[2].grade == Grade::High);
    // @merge(append) appends; plain lists, maps, variants and optionals replace.
    CHECK(k.extras == std::vector<Name>{Name("base_extra"), Name("skiff_extra")});
    CHECK(k.nested == std::vector<std::vector<i32>>{{9}});
    CHECK(k.attrs == std::map<Name, f64>{{Name("Speed"), 140.0}});
    CHECK(k.mode.index() == 0);
    CHECK_FALSE(k.maybe.has_value());
    CHECK(k.aiNotes == base.aiNotes); // server fields inherit too

    const refl::Value mk2V = decoded(db, db.find(kSkiffMk2));
    const ShipDef& mk2 = ship(mk2V);
    CHECK(mk2.extras == std::vector<Name>{Name("base_extra"), Name("skiff_extra"), Name("mk2_extra")});
    CHECK(mk2.label == "Skiff Mk II");
    CHECK(mk2.mass == 9000.5f); // from skiff, two levels up the chain
    CHECK(mk2.handling.yaw == 60.0f);
    REQUIRE(mk2.mode.index() == 1);
    CHECK(std::get<1>(mk2.mode).speed == 100.0f);
}

TEST_CASE("cook: source and inheritance errors are reported together, deterministically") {
    const refl::TypeInfo& part = type("test.records.PartDef");
    const refl::TypeInfo& loot = type("test.records.LootDef");
    std::vector<SourceRecord> s = {
        source("a/unknown_parent.hrec", part, R"({"$rid": 11, "$name": "p/a", "$parent": "p/missing"})"),
        source("b/cycle1.hrec", part, R"({"$rid": 12, "$name": "p/c1", "$parent": "p/c2"})"),
        source("b/cycle2.hrec", part, R"({"$rid": 13, "$name": "p/c2", "$parent": "p/c1"})"),
        source("b/cycle_child.hrec", part, R"({"$rid": 14, "$name": "p/cc", "$parent": "p/c1"})"),
        source("c/self.hrec", part, R"({"$rid": 15, "$name": "p/self", "$parent": "p/self"})"),
        source("d/loot.hrec", loot, R"({"$rid": 16, "$name": "l/one"})"),
        source("d/mistyped_parent.hrec", part, R"({"$rid": 17, "$name": "p/m", "$parent": "l/one"})"),
        source("e/dup_rid.hrec", part, R"({"$rid": 16, "$name": "p/dup"})"),
        source("e/dup_name.hrec", part, R"({"$rid": 18, "$name": "l/one"})"),
        source("f/no_rid.hrec", part, R"({"$name": "p/norid"})"),
        source("f/bad_rid.hrec", part, R"({"$rid": 9223372036854775808, "$name": "p/badrid"})"),
        source("f/no_name.hrec", part, R"({"$rid": 19})"),
        source("g/unknown_field.hrec", part, R"({"$rid": 20, "$name": "p/uf", "masss": 2})"),
        source("g/unknown_meta.hrec", part, R"({"$rid": 21, "$name": "p/um", "$type": "x"})"),
        source("g/bad_json.hrec", part, R"({"$rid": 22, "$name": )"),
        source("g/bad_value.hrec", part, R"({"$rid": 23, "$name": "p/bv", "mass": "heavy"})"),
        source("h/ok.hrec", part, R"({"$rid": 24, "$name": "p/ok", "mass": 3})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    CHECK(hasDiag(d, "a/unknown_parent.hrec", "$parent 'p/missing' does not exist"));
    CHECK(hasDiag(d, "b/cycle1.hrec", "$parent cycle: 'p/c1' -> 'p/c2' -> 'p/c1'"));
    CHECK(hasDiag(d, "b/cycle2.hrec", "$parent cycle"));
    CHECK(hasDiag(d, "b/cycle_child.hrec", "$parent 'p/c1' has errors"));
    CHECK(hasDiag(d, "c/self.hrec", "$parent cycle: 'p/self' -> 'p/self'"));
    CHECK(hasDiag(d, "d/mistyped_parent.hrec", "$parent 'l/one' is a test.records.LootDef, not a test.records.PartDef"));
    CHECK(hasDiag(d, "e/dup_rid.hrec", "$rid 16 is already used by d/loot.hrec"));
    CHECK(hasDiag(d, "e/dup_name.hrec", "$name 'l/one' is already used by d/loot.hrec"));
    CHECK(hasDiag(d, "f/no_rid.hrec", "no $rid"));
    CHECK(hasDiag(d, "f/bad_rid.hrec", "$rid must be a non-zero 63-bit integer"));
    CHECK(hasDiag(d, "f/no_name.hrec", "no $name"));
    CHECK(hasDiag(d, "g/unknown_field.hrec", "masss"));
    CHECK(hasDiag(d, "g/unknown_meta.hrec", "$type"));
    CHECK(hasDiag(d, "g/bad_json.hrec", ""));
    CHECK(hasDiag(d, "g/bad_value.hrec", "mass"));
    CHECK_FALSE(std::any_of(d.begin(), d.end(), [](const CookDiagnostic& x) { return x.path == "h/ok.hrec"; }));
    // Sorted by path, and identical whatever the input order.
    CHECK(std::is_sorted(d.begin(), d.end(), [](const CookDiagnostic& a, const CookDiagnostic& b) { return a.path < b.path; }));
    std::mt19937 rng(7);
    for (int round = 0; round < 3; ++round) {
        std::shuffle(s.begin(), s.end(), rng);
        CHECK(cookErrors(s) == d);
    }
    // Unknown fields are warnings when strictUnknownFields is off.
    CookOptions lax;
    lax.strictUnknownFields = false;
    const std::vector<SourceRecord> one = {source("x.hrec", part, R"({"$rid": 30, "$name": "p/x", "masss": 2})")};
    CHECK(cook(one, lax).ok());
}

TEST_CASE("cook: keyed-list merges reject duplicate and missing keys") {
    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    const std::vector<SourceRecord> s = {
        source("dup_key.hrec", shipT,
               R"({"$rid": 40, "$name": "s/1", "mounts": [{"$key": "00000000-0000-4000-8000-000000000001"},
                                                          {"$key": "00000000-0000-4000-8000-000000000001"}]})"),
        source("no_key.hrec", shipT, R"({"$rid": 41, "$name": "s/2", "mounts": [{"bone": "x"}]})"),
        // A keyed list inside a replaced value is read whole, and still needs its keys.
        source("nested_no_key.hrec", shipT, R"({"$rid": 45, "$name": "s/6", "roster": {"seats": [{"bone": "x"}]}})"),
        source("dup_slot.hrec", shipT, R"({"$rid": 42, "$name": "s/3", "slots": [{"slot": "a"}, {"slot": "a"}]})"),
        source("no_slot.hrec", shipT, R"({"$rid": 43, "$name": "s/4", "slots": [{"weight": 1}]})"),
        source("bad_enum.hrec", shipT, R"({"$rid": 44, "$name": "s/5", "grade": 7, "perms": 64})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    CHECK(hasDiag(d, "dup_key.hrec", "duplicate $key"));
    CHECK(hasDiag(d, "no_key.hrec", "mounts[0]: element has no $key"));
    CHECK(hasDiag(d, "nested_no_key.hrec", "roster.seats[0]: element has no $key"));
    CHECK(hasDiag(d, "dup_slot.hrec", "duplicate slot"));
    CHECK(hasDiag(d, "no_slot.hrec", "element has no 'slot'"));
    CHECK(hasDiag(d, "bad_enum.hrec", "grade: 7 is not a declared value of test.records.Grade"));
    CHECK(hasDiag(d, "bad_enum.hrec", "perms: 64 is not a declared value of test.records.Perm"));
}

TEST_CASE("cook: references must resolve to a record of the declared type") {
    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    const refl::TypeInfo& part = type("test.records.PartDef");
    const refl::TypeInfo& loot = type("test.records.LootDef");
    const std::vector<SourceRecord> s = {
        source("part.hrec", part, R"({"$rid": 50, "$name": "p/1"})"),
        source("loot.hrec", loot, R"({"$rid": 51, "$name": "l/1"})"),
        source("ship.hrec", shipT, R"({"$rid": 52, "$name": "s/1", "parts": [50, 999, 51], "loot": 50})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    CHECK(hasDiag(d, "ship.hrec", "parts[1]: references record 999, which does not exist"));
    CHECK(hasDiag(d, "ship.hrec", "parts[2]: references record 51 ('l/1'), a test.records.LootDef, where a test.records.PartDef is expected"));
    CHECK(hasDiag(d, "ship.hrec", "loot: references record 50 ('p/1'), a test.records.PartDef, where a test.records.LootDef is expected"));
    CHECK(d.size() == 3);
}

TEST_CASE("cook: HxlExpr values compile to HXL bytecode (engine/hxl)") {
    const CookOutput out = cookProject();
    const RecordDb db = open(out.client);
    const ValueView base = db.find(kBase).value;
    const ValueView f = base.field("formula");
    REQUIRE(f.isValid());
    CHECK(f.asText() == "attr(self, Hull.Mass) * 2 + 1");
    auto program = hxl::Program::decode(f.hxlBytecode());
    REQUIRE(program.ok());
    auto expected = hxl::compile("attr(self, Hull.Mass) * 2 + 1");
    REQUIRE(expected.ok());
    CHECK(*program == *expected);
    hxl::MapEnv env;
    env.attrs["self"]["Hull.Mass"] = 10.0;
    env.bind(*program);
    hxl::Value v;
    REQUIRE(hxl::eval(*program, env, v) == hxl::Status::Ok);
    CHECK(v.number == 21.0);
    // The `formula Name(a, b) = …` form names its own parameters.
    auto wren = hxl::Program::decode(db.find(kWren).value.field("formula").hxlBytecode());
    REQUIRE(wren.ok());
    CHECK(wren->params() == std::vector<std::string>{"pilot", "hull"});
    // An empty expression cooks as no program.
    CHECK(db.find(kHullPlate).type->qualifiedName == "test.records.PartDef");
    CHECK(db.find(kSkiff).value.field("query").asText() == "all(Ship.Class) none(State.Docked)");

    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    const std::vector<SourceRecord> bad = {
        source("bad.hrec", shipT, R"({"$rid": 60, "$name": "s/1", "formula": "attr(self, X) +"})"),
        source("param.hrec", shipT, R"j({"$rid": 61, "$name": "s/2", "formula": "attr(target, X)"})j"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(bad);
    INFO(dump(d));
    CHECK(hasDiag(d, "bad.hrec", "formula: formula does not compile: E_SYNTAX 1:16"));
    CHECK(hasDiag(d, "param.hrec", "E_UNKNOWN_NAME"));
    CookOptions opts;
    opts.hxlParams = {"target"};
    CHECK(cook(std::span<const SourceRecord>(bad).subspan(1), opts).ok());
}

TEST_CASE("cook: tags compile to the TagIndex table gameplay::TagRegistry assigns") {
    const CookOutput out = cookProject();
    const RecordDb server = open(out.server);
    const RecordDb client = open(out.client);
    REQUIRE(server.tagCount() == client.tagCount());
    CHECK(server.tagCount() == out.stats.tags);
    // The same declarations through gameplay's registry: TagDef records plus the tags TagSets use.
    gameplay::TagRegistry::Builder b;
    REQUIRE(b.add("Ship.Class.Frigate", gameplay::Audience::All).ok());
    REQUIRE(b.add("State.Docked", gameplay::Audience::Owner).ok());
    // TagSet values, then the tags of tag queries and of formulas (tag(self, …)).
    for (const char* t : {"Ship.Role.Escort", "Ship.Class.Scout", "Sec4Sentinel.Server.Tag", "Ship.Class", "Ship.Role.Scout",
                          "Faction.Neutral", "Ship.Mod.Afterburner"}) {
        REQUIRE(b.add(t).ok());
    }
    auto reg = b.build();
    REQUIRE(reg.ok());
    REQUIRE((*reg)->size() == server.tagCount());
    for (usize i = 0; i < server.tagCount(); ++i) {
        const TagView t = server.tag(static_cast<TagIndex>(i));
        const gameplay::TagInfo& g = (*reg)->info(static_cast<gameplay::TagIndex>(i));
        CHECK(t.name == g.name);
        CHECK(t.parent == g.parent);
        CHECK(t.subtreeEnd == g.subtreeEnd);
        CHECK(t.depth == g.depth);
        CHECK(t.declared == g.declared);
        CHECK(t.audience == static_cast<u8>(g.replicate));
        CHECK(server.findTag(t.name) == i);
    }
    // TagSet values hold indices; decoding gives the names back.
    const ValueView tags = server.find(kWren).value.field("tags");
    REQUIRE(tags.tags().size() == 2);
    CHECK(server.tag(tags.tags()[0]).name == "Ship.Class.Scout");
    CHECK(server.tag(tags.tags()[1]).name == "State.Docked");
    CHECK(server.tag(server.findTag("Ship.Class.Frigate")).audience == 2);
    CHECK(server.tag(server.findTag("Ship")).declared == false);
    CHECK(server.findTag("Ship.Mod.Afterburner") != kNoTag); // only a formula names it
    CHECK(server.findTag("Faction.Neutral") != kNoTag);      // only a tag query names it

    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    const refl::TypeInfo& tagT = type("helios.gameplay.TagDef");
    const std::vector<SourceRecord> bad = {
        source("t1.hrec", tagT, R"({"$rid": 70, "$name": "t/1", "tag": "A.B", "replicate": "All"})"),
        source("t2.hrec", tagT, R"({"$rid": 71, "$name": "t/2", "tag": "A.B", "replicate": "Owner"})"),
        source("s.hrec", shipT, R"({"$rid": 72, "$name": "s/1", "tags": ["Ok.Tag", "9bad", "a..b"]})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(bad);
    INFO(dump(d));
    CHECK(hasDiag(d, "t2.hrec", "tag 'A.B' is declared twice with different audiences"));
    CHECK(hasDiag(d, "s.hrec", "tags: invalid tag name '9bad'"));
    CHECK(hasDiag(d, "s.hrec", "tags: invalid tag name 'a..b'"));
}

TEST_CASE("cook: identical inputs give byte-identical cooks, pinned by a golden hash") {
    std::vector<SourceRecord> sources = projectSources();
    const CookOutput a = cookProject();
    std::mt19937 rng(11);
    std::shuffle(sources.begin(), sources.end(), rng);
    auto b = cook(sources);
    REQUIRE(b.ok());
    CHECK(a.client == b->client);
    CHECK(a.server == b->server);
    CHECK(a.client.size() % hrdb::kAlignment == 0);
    // The golden values pin the format: any change to the layout rules, the encoders or the tables shows
    // up here (and must bump hrdb::kFormatVersion when old files would be misread). The same values must
    // come out of every toolchain (GCC and Clang locally, MSVC and clang-cl in CI).
    MESSAGE("client ", a.client.size(), " bytes, xxh3 ", std::format("{:#018x}", hash64(a.client.data(), a.client.size())));
    MESSAGE("server ", a.server.size(), " bytes, xxh3 ", std::format("{:#018x}", hash64(a.server.data(), a.server.size())));
    CHECK(hash64(a.client.data(), a.client.size()) == 0x0174baa4ac0f45bfull);
    CHECK(hash64(a.server.data(), a.server.size()) == 0xe66e0b6f3c9c3afdull);
}

TEST_CASE("cook: lists nested deeper than hrdb::kMaxNesting are refused; the limit itself cooks and loads") {
    const refl::TypeInfo& treeT = type("test.records.TreeDef");
    auto tree = [](u32 depth) {
        std::string json = R"({"value": 0})";
        for (u32 i = 0; i < depth; ++i) json = R"({"value": 1, "kids": [)" + json + "]}";
        return json;
    };
    const std::vector<SourceRecord> ok = {source("ok.hrec", treeT, R"({"$rid": 80, "$name": "t/ok", "root": )" + tree(hrdb::kMaxNesting) + "}")};
    auto out = cook(ok);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    const RecordDb db = open(out->server);
    const refl::Value v = decoded(db, db.find(80));
    const auto& t = *static_cast<const TreeDef*>(v.data());
    const TreeNode* n = &t.root;
    u32 depth = 0;
    while (!n->kids.empty()) {
        n = &n->kids[0];
        ++depth;
    }
    CHECK(depth == hrdb::kMaxNesting);
    const std::vector<SourceRecord> deep = {source("deep.hrec", treeT, R"({"$rid": 81, "$name": "t/deep", "root": )" + tree(hrdb::kMaxNesting + 1) + "}")};
    const std::vector<CookDiagnostic> d = cookErrors(deep);
    CHECK(hasDiag(d, "deep.hrec", "nest deeper than 32"));
}

TEST_CASE("cook: collectSources types files by table; writeCookOutput and openFile round-trip") {
    const fs::Path work = fs::pathFromUtf8(HELIOS_RECORDS_TEST_WORK_DIR) / "collect";
    (void)fs::removeAll(work);
    REQUIRE(fs::createDirectories(work / "records" / "part").ok());
    REQUIRE(fs::createDirectories(work / "records" / "nope").ok());
    REQUIRE(fs::writeTextFile(work / "records" / "part" / "a.hrec", R"({"$rid": 90, "$name": "p/a"})").ok());
    REQUIRE(fs::writeTextFile(work / "records" / "nope" / "b.hrec", R"({"$rid": 91, "$name": "p/b"})").ok());
    auto bad = collectSources(work, registry());
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("records/nope/b.hrec: no record type has @table(\"nope\")") != std::string::npos);
    REQUIRE(fs::remove(work / "records" / "nope" / "b.hrec").ok());
    auto sources = collectSources(work, registry());
    REQUIRE(sources.ok());
    REQUIRE(sources->size() == 1);
    CHECK((*sources)[0].path == "records/part/a.hrec");
    CHECK((*sources)[0].type == &type("test.records.PartDef"));
    auto out = cook(*sources);
    REQUIRE(out.ok());
    REQUIRE(writeCookOutput(*out, work / "cooked").ok());
    auto db = RecordDb::openFile(work / "cooked" / fs::pathFromUtf8(kClientDbFile), registry(), {CookAudience::Client});
    REQUIRE(db.ok());
    CHECK(db->findByName("p/a").id == 90);
    auto wrong = RecordDb::openFile(work / "cooked" / fs::pathFromUtf8(kServerDbFile), registry(), {CookAudience::Client});
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message.find("this is the server cook, expected the client cook") != std::string::npos);
    CHECK(collectSources(work / "missing", registry()).errorCode() == ErrorCode::NotFound);
}

} // namespace
