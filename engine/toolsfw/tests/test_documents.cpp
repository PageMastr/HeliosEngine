// Documents, the workspace, validation, selection, external edits and Luau automation.

#include <doctest/doctest.h>

#include <cmath>
#include <format>
#include <limits>

#include "helios/core/platform.h"

#include "helios/toolsfw/automation.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/samples.h"
#include "helios/toolsfw/validate.h"
#include "sample/ship.gen.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

TEST_CASE("documents: types come from @table directories; lookups by name, id and path") {
    ensureSampleTypes();
    const auto& types = refl::TypeRegistry::global();
    CHECK(recordTypeForPath(types, "records/hull/frigate.hrec") == types.find("sample.ship.ShipHullDef"));
    CHECK(recordTypeForPath(types, "hull/frigate.hrec") == types.find("sample.ship.ShipHullDef"));
    CHECK(recordTypeForPath(types, "records/loot/drone.hrec") == types.find("sample.ship.LootTableDef"));
    CHECK(recordTypeForPath(types, "records/nosuchtable/x.hrec") == nullptr);
    CHECK(recordTypeForPath(types, "frigate.hrec") == nullptr);

    Fixture f("docs_lookup");
    const Document& d = f.doc();
    CHECK(d.name() == "hull/frigate");
    CHECK(d.relativePath() == "records/hull/frigate.hrec");
    CHECK(f.fw->documents().find("hull/frigate") == &d);
    CHECK(f.fw->documents().find("records/hull/frigate.hrec") == &d);
    CHECK(f.fw->documents().find(d.id().toString()) == &d);
    CHECK(f.fw->documents().find(d.path().string()) == &d);
    CHECK(f.fw->documents().find("hull/nope") == nullptr);
    CHECK_FALSE(d.dirty());
    CHECK(d.text() == samples::sampleHullRecordText());
    // Opening the same file again returns the open document.
    auto again = f.fw->open("records/hull/frigate.hrec");
    REQUIRE(again);
    CHECK(*again == &d);
    CHECK(f.fw->open("records/hull/missing.hrec").errorCode() == ErrorCode::NotFound);
}

TEST_CASE("documents: path case follows the platform's file system") {
    Fixture f("docs_case");
    const auto& types = f.fw->types();
    auto other = f.fw->open("Records/Hull/FRIGATE.hrec");
    if constexpr (platform::kIsWindows) {
        // NTFS ignores case: the same file must not become a second document.
        REQUIRE(other);
        CHECK(*other == &f.doc());
        CHECK(recordTypeForPath(types, "Records/HULL/frigate.hrec") == types.find("sample.ship.ShipHullDef"));
    } else {
        // A case-sensitive file system has no such file.
        CHECK(other.errorCode() == ErrorCode::NotFound);
        CHECK(recordTypeForPath(types, "Records/HULL/frigate.hrec") == nullptr);
    }
    CHECK(f.fw->documents().size() == 1);
}

TEST_CASE("documents: a non-canonical file opens clean and saves canonical") {
    const fs::Path root = freshDir("docs_canonical");
    REQUIRE(writeProject(root, false));
    const fs::Path file = root / "records" / "hull" / "odd.hrec";
    REQUIRE(fs::writeTextFile(file, "// hand written\n{\"mass\": 1.5e3, \"$name\": \"hull/odd\", \"$rid\": 12, \"size\": \"Large\",}\n"));
    auto fw = Framework::create(deterministicConfig(root, {}, 1));
    REQUIRE(fw);
    auto opened = (*fw)->openAll();
    REQUIRE(opened);
    CHECK(*opened == 1);
    const Document* d = (*fw)->documents().find("hull/odd");
    REQUIRE(d);
    CHECK_FALSE(d->dirty());
    CHECK(d->text().find("\"mass\": 1500") != std::string::npos);
    REQUIRE((*fw)->save(d->id()));
    CHECK(fs::readTextFile(file).value() == d->text());
    REQUIRE(fs::writeTextFile(root / "records" / "hull" / "broken.hrec", "{\"mass\": }"));
    auto bad = Framework::create(deterministicConfig(root, {}, 1));
    REQUIRE(bad);
    CHECK_FALSE((*bad)->openAll());
    CHECK((*bad)->documents().find("hull/odd") != nullptr);  // the good file still opened
}

TEST_CASE("validate: ranges, finiteness, @max, keys and headers") {
    ensureSampleTypes();
    sample::ship::ShipHullDef h;
    h.mass = 50.0f;  // @range(100, 1e9)
    h.handling.yawRate = std::numeric_limits<f32>::infinity();
    sample::ship::ThrusterMount t;
    t.maxForce = -1.0f;  // @range(0, 1e8)
    for (int i = 0; i < 65; ++i) h.thrusters.add(Guid(1, static_cast<u64>(i + 1)), t);  // @max(64)
    h.thrusters.setKey(3, Guid(1, 1));                                                   // duplicate key
    sample::ship::Hardpoint hp;
    hp.slot = Name("a");
    h.hardpoints = {hp, hp};  // duplicate @keyed(slot)
    std::vector<Issue> issues;
    validateObject(refl::typeOf<sample::ship::ShipHullDef>(), &h, issues);
    auto count = [&](std::string_view rule) {
        return std::count_if(issues.begin(), issues.end(), [&](const Issue& i) { return i.rule == rule; });
    };
    CHECK(count("range") == 1 + 65);
    CHECK(count("finite") == 1);
    CHECK(count("max") == 1);
    CHECK(count("key") == 2);
    bool massIssue = false;
    for (const Issue& i : issues) massIssue |= i.path == "mass" && i.rule == "range";
    CHECK(massIssue);
    CHECK(formatIssue(issues.front()).find("error") != std::string::npos);
    std::vector<Issue> header;
    validateHeader(refl::RecordHeader{}, header);
    CHECK(header.size() == 2);
}

TEST_CASE("validate: each issue names the file of the document it was found in") {
    Fixture f("docs_validate_files");
    const refl::TypeInfo* type = f.fw->types().find("sample.ship.ShipHullDef");
    REQUIRE(type);
    refl::RecordHeader h;
    h.rid = 98;
    h.name = "hull/wren";
    auto c = f.fw->begin(Origin::Cli);
    auto wren = c->createRecord(*type, "records/hull/wren.hrec", h, R"({"mass": 500})");
    REQUIRE(wren);
    REQUIRE(c->commit());
    auto b = f.fw->begin(Origin::Cli, "Two hulls");
    REQUIRE(b->set(f.frigate, "mass", "13000"));
    REQUIRE(b->set(*wren, "mass", "1"));  // @range(100, 1e9)
    auto r = b->commit();
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("records/hull/wren.hrec") != std::string::npos);
    CHECK(r.error().message.find("records/hull/frigate.hrec") == std::string::npos);
}

TEST_CASE("selection: history, named sets and forgetting documents") {
    Selection s;
    const ObjRef a{Guid(1, 1), {}, ""};
    const ObjRef b{Guid(2, 2), {}, "mass"};
    s.set(a);
    s.set(b);
    s.add(a);
    CHECK(s.items().size() == 2);
    CHECK(s.primaryDocument() == b.doc);
    REQUIRE(s.back());
    CHECK(s.items().size() == 1);
    CHECK(s.items()[0] == b);
    REQUIRE(s.forward());
    CHECK(s.contains(a));
    s.saveSet("pair");
    s.clear();
    CHECK(s.empty());
    REQUIRE(s.recallSet("pair"));
    CHECK(s.items().size() == 2);
    CHECK_FALSE(s.recallSet("nope"));
    const u64 v = s.version();
    s.forgetDocument(a.doc);
    CHECK(s.version() > v);
    CHECK(s.items().size() == 1);
    s.remove(b);
    CHECK(s.empty());
    for (int i = 0; i < 100; ++i) s.set(ObjRef{Guid(3, static_cast<u64>(i + 1)), {}, ""});
    int backs = 0;
    while (s.back()) ++backs;
    CHECK(backs == static_cast<int>(Selection::kHistoryLimit));
}

TEST_CASE("external edits: reload is an import transaction; dirty documents merge three-way") {
    Fixture f("docs_external");
    const fs::Path file = f.doc().path();
    auto edit = [&](std::string_view from, std::string_view to) {
        std::string text = fs::readTextFile(file).value();
        const usize at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        REQUIRE(fs::writeTextFile(file, text));
    };
    SUBCASE("clean document") {
        edit("\"mass\": 12000", "\"mass\": 12345");
        auto tx = f.fw->reloadFromDisk(f.frigate);
        REQUIRE(tx);
        CHECK(f.get("mass") == "12345");
        CHECK_FALSE(f.doc().dirty());
        CHECK(f.fw->log().back().origin == Origin::Import);
        REQUIRE(f.fw->undo(Origin::Ui));
        CHECK(f.get("mass") == "12000");
        CHECK(f.doc().dirty());
    }
    SUBCASE("dirty document, non-overlapping changes merge") {
        REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 33})"));
        edit("\"mass\": 12000", "\"mass\": 12345");
        REQUIRE(f.fw->reloadFromDisk(f.frigate));
        CHECK(f.get("mass") == "12345");
        CHECK(f.get("handling/yawRate") == "33");
        CHECK(f.doc().dirty());
    }
    SUBCASE("dirty document, conflicting change is refused") {
        REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 15000})"));
        edit("\"mass\": 12000", "\"mass\": 12345");
        auto r = f.fw->reloadFromDisk(f.frigate);
        REQUIRE_FALSE(r);
        CHECK(r.errorCode() == ErrorCode::InvalidState);
        CHECK(r.error().message.find("mass") != std::string::npos);
        CHECK(f.get("mass") == "15000");
        // Revert takes the file's content wholesale (undoably).
        REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.revert", R"({"doc": "hull/frigate"})"));
        CHECK(f.get("mass") == "12345");
        CHECK_FALSE(f.doc().dirty());
    }
    SUBCASE("header edits on disk") {
        edit("\"$name\": \"hull/frigate\"", "\"$name\": \"hull/frigate_ext\"");
        REQUIRE(f.fw->reloadFromDisk(f.frigate));
        CHECK(f.doc().name() == "hull/frigate_ext");
    }
}

TEST_CASE("automation: Luau edits go through commands with origin luau") {
    Fixture f("docs_luau");
    auto a = Automation::create(*f.fw);
    REQUIRE(a);
    auto r = (*a)->run("tune", R"(
        Record.set("hull/frigate", "mass", "15000")
        Editor.transaction("Retune handling", function()
            Record.set("hull/frigate", "handling/yawRate", "12")
            Editor.cmd("doc.setProperty", '{"doc": "hull/frigate", "path": "handling/pitchRate", "value": 13}')
        end)
        Editor.log("mass is " .. Record.get("hull/frigate", "mass"))
        local found = Editor.find("frigate")
        print(#found, found[1])
        assert(Validate.run("hull/frigate") == 0)
    )");
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(f.get("mass") == "15000");
    CHECK(f.get("handling/pitchRate") == "13");
    REQUIRE(f.fw->history().size() == 2);
    CHECK(f.fw->history()[1].tx.label == "Retune handling");
    CHECK(f.fw->history()[1].tx.ops.size() == 2);
    for (const HistoryEntry& e : f.fw->history()) CHECK(e.tx.origin == Origin::Luau);
    REQUIRE(r->log.size() == 2);
    CHECK(r->log[0] == "mass is 15000");
    CHECK(r->log[1] == "1\thull/frigate");

    // An error inside Editor.transaction rolls the group back and surfaces the Luau message.
    const std::string before = f.doc().text();
    auto bad = (*a)->run("bad", R"(
        Editor.transaction("Broken", function()
            Record.set("hull/frigate", "mass", "16000")
            error("boom")
        end)
    )");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("boom") != std::string::npos);
    CHECK(f.doc().text() == before);
    CHECK_FALSE(f.fw->inGroup());
    // Validation failures come back as errors, the sandbox has no io.
    CHECK_FALSE((*a)->run("range", R"(Record.set("hull/frigate", "mass", "1"))"));
    CHECK_FALSE((*a)->run("io", R"(io.open("x"))"));
    CHECK((*a)->run("undo", R"(assert(Editor.undo()))"));
    CHECK(f.fw->log().back().origin == Origin::Luau);
}

TEST_CASE("automation: a caught nested Editor.transaction failure keeps the outer one atomic") {
    Fixture f("docs_luau_nested");
    auto a = Automation::create(*f.fw);
    REQUIRE(a);
    auto r = (*a)->run("nested", R"(
        Editor.transaction("outer", function()
            Record.set("hull/frigate", "mass", "15000")
            local ok = pcall(function()
                Editor.transaction("inner", function()
                    Record.set("hull/frigate", "handling/rollRate", "95")
                    error("x")
                end)
            end)
            assert(not ok)
            Record.set("hull/frigate", "handling/yawRate", "12")
        end))");
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK(f.get("mass") == "15000");
    CHECK(f.get("handling/yawRate") == "12");
    CHECK(f.get("handling/rollRate") != "95");  // the inner level rolled back
    REQUIRE(f.fw->history().size() == 1);       // one undo step, labelled by the outer transaction
    CHECK(f.fw->history()[0].tx.label == "outer");
    CHECK(f.fw->history()[0].tx.ops.size() == 2);
    CHECK_FALSE(f.fw->inGroup());
}

TEST_CASE("automation: runs reuse one VM module; a caller's open group survives a run") {
    Fixture f("docs_luau_runs");
    auto a = Automation::create(*f.fw);
    REQUIRE(a);
    for (int i = 0; i < 20; ++i) {
        auto r = (*a)->run(std::format("run{}", i), std::format("Editor.log('run {}')", i));
        REQUIRE(r);
        REQUIRE(r->log.size() == 1);
        CHECK(r->log[0] == std::format("run {}", i));
    }
    CHECK((*a)->loadedModules() == 1);
    // A failing first chunk does not wedge the module: the next run still runs.
    auto fresh = Automation::create(*f.fw);
    REQUIRE(fresh);
    CHECK_FALSE((*fresh)->run("bad", "error('first')"));
    auto second = (*fresh)->run("good", "Editor.log('second')");
    REQUIRE(second);
    CHECK(second->log.size() == 1);
    // A group the caller opened stays open; only the script's own levels are cancelled.
    f.fw->beginGroup(Origin::Luau, "Caller");
    CHECK_FALSE((*a)->run("killed", R"(Editor.transaction("t", function() error("boom") end))"));
    CHECK(f.fw->groupDepth() == 1);
    f.fw->cancelGroup();
}

} // namespace
