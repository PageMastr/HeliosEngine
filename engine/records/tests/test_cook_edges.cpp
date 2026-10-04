// The records cook at its edges: merges onto non-empty defaults, duplicate source paths and TypeIds, and
// the tag-table hash both cooks share.
#include <algorithm>
#include <format>

#include "test_util.h"

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;

// helios-lint: outside-anon-namespace begin (hand-built mergedef types and their TypeOf<> specializations)
namespace mergedef {
/// Builder types with non-empty container defaults (schemac forbids those; builder types may have them).
struct Item {
    Name id;
    i32 v = 0;
    std::vector<i32> inner = {7};
};
struct Rec {
    std::vector<i32> app = {1, 2};
    std::vector<Item> items = {Item{Name("a"), 1, {7}}};
};
} // namespace mergedef

HELIOS_REFLECT_TYPE(mergedef::Item);
HELIOS_REFLECT_TYPE(mergedef::Rec);

const helios::refl::TypeInfo& helios::refl::TypeOf<mergedef::Item>::get() noexcept {
    static const TypeInfo* info = StructBuilder<mergedef::Item>("mergedef.Item")
                                      .field("id", &mergedef::Item::id)
                                      .field("v", &mergedef::Item::v)
                                      .field("inner", &mergedef::Item::inner, {.attrs = {BuilderAttr{"merge", {{"", "append"}}}}})
                                      .build();
    return *info;
}
const helios::refl::TypeInfo& helios::refl::TypeOf<mergedef::Rec>::get() noexcept {
    static const TypeInfo* info = StructBuilder<mergedef::Rec>("mergedef.Rec", DeclKind::Record)
                                      .field("app", &mergedef::Rec::app, {.attrs = {BuilderAttr{"merge", {{"", "append"}}}}})
                                      .field("items", &mergedef::Rec::items, {.keyedBy = "id"})
                                      .build();
    return *info;
}
// helios-lint: outside-anon-namespace end

namespace {

const refl::TypeRegistry& mergeRegistry() {
    static refl::TypeRegistry* reg = [] {
        auto* r = new refl::TypeRegistry();
        REQUIRE(r->add(refl::typeOf<mergedef::Item>()).ok());
        REQUIRE(r->add(refl::typeOf<mergedef::Rec>()).ok());
        return r;
    }();
    return *reg;
}

const mergedef::Rec& rec(const refl::Value& v) { return *static_cast<const mergedef::Rec*>(v.data()); }

TEST_CASE("cook: keyed and @merge(append) lists merge only into inherited data, never into a default") {
    const refl::TypeInfo& t = refl::typeOf<mergedef::Rec>();
    const std::vector<SourceRecord> s = {
        source("a.hrec", t, R"({"$rid": 200, "$name": "m/base", "app": [3], "items": [{"id": "b", "v": 2, "inner": [8]}]})"),
        source("b.hrec", t, R"({"$rid": 201, "$name": "m/child", "$parent": "m/base", "app": [4],
          "items": [{"id": "b", "inner": [9]}, {"id": "c", "inner": [5]}]})"),
        source("c.hrec", t, R"({"$rid": 202, "$name": "m/plain"})"),
        source("d.hrec", t, R"({"$rid": 203, "$name": "m/plain_child", "$parent": "m/plain", "app": [4]})"),
    };
    auto out = cook(s);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    const RecordDb db = open(out->server, mergeRegistry());

    // Without $parent the cook reads what refl::readRecord reads: the lists replace their defaults.
    const refl::Value base = decoded(db, db.find(200));
    refl::Value want(t);
    refl::RecordHeader header;
    refl::ReadCtx ctx;
    REQUIRE(refl::readRecord(t, want.data(), s[0].text, header, ctx).ok());
    CHECK(refl::equals(t, base.data(), want.data()));
    CHECK(rec(base).app == std::vector<i32>{3});
    REQUIRE(rec(base).items.size() == 1);
    CHECK(rec(base).items[0].id == Name("b"));
    CHECK(rec(base).items[0].inner == std::vector<i32>{8});

    // With $parent they merge into the parent's lists; an element the child adds starts from defaults
    // that its own lists replace.
    const refl::Value child = decoded(db, db.find(201));
    CHECK(rec(child).app == std::vector<i32>({3, 4}));
    REQUIRE(rec(child).items.size() == 2);
    CHECK(rec(child).items[0].v == 2);
    CHECK(rec(child).items[0].inner == std::vector<i32>({8, 9}));
    CHECK(rec(child).items[1].id == Name("c"));
    CHECK(rec(child).items[1].inner == std::vector<i32>{5});

    // A parent that never set the list passes its default down, and a child appends to that.
    CHECK(rec(decoded(db, db.find(202))).app == std::vector<i32>({1, 2}));
    CHECK(rec(decoded(db, db.find(203))).app == std::vector<i32>({1, 2, 4}));
}

TEST_CASE("cook: two sources with one path are refused, whatever their order") {
    const refl::TypeInfo& part = type("test.records.PartDef");
    std::vector<SourceRecord> s = {
        source("records/part/a.hrec", part, R"({"$rid": 220, "$name": "p/a"})"),
        source("records/part/a.hrec", part, R"({"$rid": 221, "$name": "p/b"})"),
        source("records/part/c.hrec", part, R"({"$rid": 222, "$name": "p/c"})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    REQUIRE(d.size() == 1);
    CHECK(d[0] == CookDiagnostic{"records/part/a.hrec", "another source has the same path"});
    std::reverse(s.begin(), s.end());
    CHECK((cookErrors(s) == d));
}

TEST_CASE("cook: two record types with one TypeId are refused with a diagnostic") {
    // A copy of PartDef under another name keeps its TypeId (process lifetime, like every TypeInfo).
    auto* twin = new refl::TypeInfo(type("test.records.PartDef"));
    twin->qualifiedName = "test.records.PartTwin";
    const std::vector<SourceRecord> s = {
        source("part.hrec", type("test.records.PartDef"), R"({"$rid": 230, "$name": "p/1"})"),
        source("twin.hrec", *twin, R"({"$rid": 231, "$name": "p/2"})"),
    };
    const std::vector<CookDiagnostic> d = cookErrors(s);
    INFO(dump(d));
    REQUIRE(d.size() == 1);
    CHECK(d[0] == CookDiagnostic{"twin.hrec", std::format("record type 'test.records.PartTwin' has TypeId {}, which record type "
                                                          "'test.records.PartDef' also has",
                                                          twin->id)});
}

TEST_CASE("cook: both cooks carry one tag-table hash, which changes with the client's view of the table only") {
    const CookOutput project = cookProject();
    const RecordDb c = open(project.client);
    const RecordDb s = open(project.server);
    CHECK(c.tagTableHash() != 0);
    CHECK(c.tagTableHash() == s.tagTableHash());

    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    auto hashes = [&](std::string_view tags, std::string_view serverTags) {
        const std::vector<SourceRecord> src = {source(
            "s.hrec", shipT, std::format(R"({{"$rid": 240, "$name": "s/1", "tags": [{}], "serverTags": [{}]}})", tags, serverTags))};
        auto out = cook(src);
        REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
        const u64 ch = open(out->client).tagTableHash();
        CHECK(ch == open(out->server).tagTableHash());
        return ch;
    };
    const u64 h = hashes(R"("Vis.A")", R"("Srv.X")");
    CHECK(hashes(R"("Vis.B")", R"("Srv.X")") != h); // another client-visible name
    CHECK(hashes(R"("Vis.A", "Vis.C")", R"("Srv.X")") != h); // one more slot
    // A withheld name is not part of the hash (it would let a client confirm a guess), as long as the
    // numbering of the slots stays the same.
    CHECK(hashes(R"("Vis.A")", R"("Srv.Y")") == h);
    CHECK(hashes(R"("Vis.A")", R"("Vis.A")") != h); // Srv.X gone: fewer slots
}

} // namespace
