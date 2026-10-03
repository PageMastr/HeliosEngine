// Transactions: property-path ops, exact inverses, rollback, conflicts and their JSON form.

#include <doctest/doctest.h>

#include "helios/reflect/path.h"
#include "helios/toolsfw/json_util.h"
#include "sample/ship.gen.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

const sample::ship::ShipHullDef& hull(const Fixture& f) {
    return *static_cast<const sample::ship::ShipHullDef*>(f.doc().object());
}

/// Commits one edit, then checks undo restores the exact bytes and redo the edited ones.
template <class Fn>
void roundTrip(Fixture& f, Fn&& edit) {
    const std::string before = f.doc().text();
    auto b = f.fw->begin(Origin::Cli, "edit");
    edit(*b);
    auto id = b->commit();
    REQUIRE(id);
    REQUIRE_FALSE(id->isNull());
    const std::string after = f.doc().text();
    CHECK(after != before);
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.doc().text() == before);
    REQUIRE(f.fw->redo(Origin::Cli));
    CHECK(f.doc().text() == after);
}

TEST_CASE("tx: set applies and undo restores the exact bytes") {
    Fixture f("tx_set");
    const std::string before = f.doc().text();
    auto b = f.fw->begin(Origin::Cli, "Set mass");
    REQUIRE(b->set(f.frigate, "mass", "15000.5"));
    CHECK(f.get("mass") == "15000.5");
    auto id = b->commit();
    REQUIRE(id);
    CHECK(!id->isNull());
    CHECK(f.doc().dirty());
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.doc().text() == before);
    CHECK_FALSE(f.doc().dirty());
    REQUIRE(f.fw->redo(Origin::Cli));
    CHECK(f.get("mass") == "15000.5");
}

TEST_CASE("tx: values are stored canonically") {
    Fixture f("tx_canonical");
    auto b = f.fw->begin(Origin::Cli);
    REQUIRE(b->set(f.frigate, "mass", "  15000.000 "));
    REQUIRE(b->ops().size() == 1);
    CHECK(b->ops()[0].after == "15000");
    CHECK(b->ops()[0].before == "12000");
    REQUIRE(b->commit());
}

TEST_CASE("tx: map keys, optionals and enums round-trip") {
    Fixture f("tx_map");
    SUBCASE("new map key") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "aiHints[aggression]", "0.5")); });
        CHECK(f.get("aiHints[aggression]") == "0.5");
    }
    SUBCASE("remove map key") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.remove(f.frigate, "aiHints[preferredRange]")); });
        CHECK(hull(f).aiHints.size() == 1);
    }
    SUBCASE("engage and reset an optional") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "lootTable", "4242")); });
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "lootTable", "null")); });
    }
    SUBCASE("enum by name") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "size", "\"Capital\"")); });
        CHECK(hull(f).size == sample::ship::ShipSize::Capital);
    }
    SUBCASE("nested struct field") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "handling/rollRate", "-12.125")); });
        CHECK(hull(f).handling.rollRate == -12.125f);
    }
}

TEST_CASE("tx: keyed-list elements keep their index and key through undo") {
    Fixture f("tx_keyed");
    const Guid key(0x1111222233334444ull, 0x8555666677778888ull);
    SUBCASE("insert in the middle") {
        roundTrip(f, [&](TxBuilder& b) {
            REQUIRE(b.insert(f.frigate, "thrusters", 1, R"({"bone": "aft", "maxForce": 1000})", key));
        });
        REQUIRE(hull(f).thrusters.size() == 4);
        CHECK(hull(f).thrusters.keyAt(1) == key);
        CHECK(hull(f).thrusters[1].bone.view() == "aft");
    }
    SUBCASE("remove the middle element") {
        roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.remove(f.frigate, "thrusters[#66726967617465008000000000000002]")); });
    }
    SUBCASE("remove by index") {
        const std::string before = f.doc().text();
        auto b = f.fw->begin(Origin::Cli);
        REQUIRE(b->remove(f.frigate, "thrusters[1]"));
        REQUIRE(b->ops().size() == 1);
        CHECK(b->ops()[0].kind == OpKind::Remove);
        CHECK(b->ops()[0].index == 1);
        CHECK(b->ops()[0].key == refl::keyedKeyText(Guid(0x6672696761746500ull, 0x8000000000000002ull)));
        b->abort();
        CHECK(f.doc().text() == before);
    }
    SUBCASE("move") {
        roundTrip(f, [&](TxBuilder& b) {
            REQUIRE(b.move(f.frigate, "thrusters[#" + refl::keyedKeyText(Guid(0x6672696761746500ull, 0x8000000000000001ull)) + "]", 2));
        });
        CHECK(hull(f).thrusters[2].bone.view() == "thruster_main");
    }
    SUBCASE("edit a field of an element") {
        roundTrip(f, [&](TxBuilder& b) {
            REQUIRE(b.set(f.frigate, "thrusters[#" + refl::keyedKeyText(Guid(0x6672696761746500ull, 0x8000000000000003ull)) + "]/maxForce", "12"));
        });
    }
    SUBCASE("a reordering diff sets the whole list") {
        auto b = f.fw->begin(Origin::Cli);
        REQUIRE(b->edit(f.frigate, [](void* obj) {
            auto* h = static_cast<sample::ship::ShipHullDef*>(obj);
            refl::KeyedList<sample::ship::ThrusterMount> r;
            for (usize i = h->thrusters.size(); i-- > 0;) r.add(h->thrusters.keyAt(i), h->thrusters[i]);
            h->thrusters = std::move(r);
        }));
        REQUIRE(b->ops().size() == 1);
        CHECK(b->ops()[0].path == "thrusters");
        b->abort();
    }
}

TEST_CASE("tx: @keyed(field) lists address elements by their key field") {
    Fixture f("tx_keyed_field");
    roundTrip(f, [&](TxBuilder& b) {
        REQUIRE(b.insert(f.frigate, "hardpoints", 0, R"({"slot": "ventral", "size": "Large", "offset": [0, -1, 0]})"));
    });
    CHECK(hull(f).hardpoints[0].slot.view() == "ventral");
    roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "hardpoints[#dorsal]/size", "\"Capital\"")); });
    roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.remove(f.frigate, "hardpoints[#nose]")); });
    auto b = f.fw->begin(Origin::Cli);
    // A second element with an existing key is a conflict, not a silent duplicate.
    CHECK_FALSE(b->insert(f.frigate, "hardpoints", 0, R"({"slot": "dorsal"})"));
    b->abort();
}

TEST_CASE("tx: reflection diff (edit) produces exact, minimal ops") {
    Fixture f("tx_edit");
    auto b = f.fw->begin(Origin::Cli, "Tune");
    REQUIRE(b->edit(f.frigate, [](void* obj) {
        auto* h = static_cast<sample::ship::ShipHullDef*>(obj);
        h->mass = 9000.0f;
        h->handling.yawRate = 33.0f;
        h->thrusters.erase(1);
        h->aiHints.erase(helios::Name("retreatHealth"));
    }));
    CHECK(b->ops().size() == 4);
    const std::string edited = f.doc().text();
    REQUIRE(b->commit());
    REQUIRE(f.fw->undo(Origin::Cli));
    REQUIRE(f.fw->redo(Origin::Cli));
    CHECK(f.doc().text() == edited);
}

TEST_CASE("tx: header edits ($name, $comment) are transactions; $rid is immutable") {
    Fixture f("tx_header");
    roundTrip(f, [&](TxBuilder& b) { REQUIRE(b.set(f.frigate, "$name", "\"hull/frigate_mk2\"")); });
    CHECK(f.fw->documents().find("hull/frigate_mk2") != nullptr);
    CHECK(f.fw->documents().find("hull/frigate") == nullptr);
    auto b = f.fw->begin(Origin::Cli);
    CHECK_FALSE(b->set(f.frigate, "$rid", "7"));
    CHECK_FALSE(b->set(f.frigate, "$name", "\"\""));
    CHECK_FALSE(b->set(f.frigate, "$name", "3"));
    CHECK(b->empty());
}

TEST_CASE("tx: abort and failures leave documents unchanged") {
    Fixture f("tx_abort");
    const std::string before = f.doc().text();
    {
        auto b = f.fw->begin(Origin::Cli);
        REQUIRE(b->set(f.frigate, "mass", "20000"));
        REQUIRE(b->remove(f.frigate, "thrusters[0]"));
        b->abort();
    }
    CHECK(f.doc().text() == before);
    {
        auto b = f.fw->begin(Origin::Cli);
        REQUIRE(b->set(f.frigate, "mass", "20000"));
        // Destroying the builder without commit rolls back.
    }
    CHECK(f.doc().text() == before);
    auto b = f.fw->begin(Origin::Cli);
    CHECK_FALSE(b->set(f.frigate, "nosuchfield", "1"));
    CHECK_FALSE(b->set(f.frigate, "mass", "\"heavy\""));
    CHECK_FALSE(b->set(f.frigate, "thrusters[#ffffffff]/maxForce", "1"));
    CHECK_FALSE(b->insert(f.frigate, "thrusters", 99, "{}"));
    CHECK_FALSE(b->move(f.frigate, "thrusters[0]", 7));
    CHECK(b->empty());
    CHECK(f.doc().text() == before);
    CHECK(f.fw->history().empty());
}

TEST_CASE("tx: ops check their preconditions (conflicts never half-apply)") {
    Fixture f("tx_conflict");
    const std::string before = f.doc().text();
    auto b = f.fw->begin(Origin::Collab);
    Op op;
    op.kind = OpKind::Set;
    op.doc = f.frigate;
    op.path = "mass";
    op.before = "1";  // wrong
    op.after = "2";
    auto r = b->apply(op);
    REQUIRE_FALSE(r);
    CHECK(r.errorCode() == ErrorCode::InvalidState);
    Op rm;
    rm.kind = OpKind::Remove;
    rm.doc = f.frigate;
    rm.path = "thrusters";
    rm.index = 0;
    rm.key = "00000000000000000000000000000000";
    rm.before = "{}";
    CHECK(b->apply(rm).errorCode() == ErrorCode::InvalidState);
    CHECK(f.doc().text() == before);
}

TEST_CASE("tx: the pre-commit hook rejects out-of-range values and rolls back") {
    Fixture f("tx_validate");
    const std::string before = f.doc().text();
    auto b = f.fw->begin(Origin::Cli);
    REQUIRE(b->set(f.frigate, "handling/yawRate", "10"));
    REQUIRE(b->set(f.frigate, "mass", "50"));  // @range(100, 1e9)
    auto r = b->commit();
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("range") != std::string::npos);
    CHECK(f.doc().text() == before);
    CHECK(f.fw->history().empty());
}

TEST_CASE("tx: create and destroy records are undoable") {
    Fixture f("tx_create");
    const fs::Path file = f.root / "project" / "records" / "hull" / "wren.hrec";
    const refl::TypeInfo* type = f.fw->types().find("sample.ship.ShipHullDef");
    REQUIRE(type);
    refl::RecordHeader h;
    h.rid = 77;
    h.name = "hull/wren";
    auto b = f.fw->begin(Origin::Cli, "New Wren");
    auto id = b->createRecord(*type, "records/hull/wren.hrec", h, R"({"mass": 900})");
    REQUIRE(id);
    REQUIRE(b->commit());
    const Document* wren = f.fw->documents().find("hull/wren");
    REQUIRE(wren);
    CHECK(wren->dirty());
    CHECK_FALSE(fs::exists(file));
    REQUIRE(f.fw->save(*id));
    CHECK(fs::exists(file));
    CHECK_FALSE(wren->dirty());

    REQUIRE(f.fw->undo(Origin::Cli));  // destroys it
    CHECK(f.fw->documents().find("hull/wren") == nullptr);
    CHECK(wren->destroyed());
    CHECK(wren->dirty());  // the file still exists
    REQUIRE(f.fw->save(*id));
    CHECK_FALSE(fs::exists(file));
    REQUIRE(f.fw->redo(Origin::Cli));
    CHECK(f.fw->documents().find("hull/wren") != nullptr);
    CHECK(wren->dirty());
    REQUIRE(f.fw->saveAll());
    CHECK(fs::exists(file));

    // Destroying the open Frigate and undoing it restores the same bytes.
    const std::string frigate = f.doc().text();
    auto d = f.fw->begin(Origin::Cli);
    REQUIRE(d->destroy(f.frigate));
    REQUIRE(d->commit());
    CHECK(f.doc().destroyed());
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK_FALSE(f.doc().destroyed());
    CHECK(f.doc().text() == frigate);

    auto dup = f.fw->begin(Origin::Cli);
    CHECK_FALSE(dup->createRecord(*type, "records/hull/frigate.hrec", h));  // file exists
    CHECK_FALSE(dup->createRecord(*type, "../outside.hrec", h));
    CHECK_FALSE(dup->createRecord(*type, "records/hull/x.json", h));
}

TEST_CASE("tx: a failing raw Create onto a destroyed document changes nothing") {
    Fixture f("tx_create_restore");
    const std::string snapshot = f.doc().text();
    {
        auto b = f.fw->begin(Origin::Ui, "delete");
        REQUIRE(b->destroy(f.frigate));
        REQUIRE(b->commit());
    }
    Op op;
    op.kind = OpKind::Create;
    op.doc = f.frigate;
    op.typeName = std::string(f.doc().type().qualifiedName);
    op.file = "records/hull/frigate.hrec";
    const u64 revision = f.doc().revision();
    for (const std::string& bad : {snapshot + "\n\n",  // not canonical
                                   [&] {
                                       std::string t = snapshot;  // another record's $rid
                                       const usize at = t.find("\"$rid\": ");
                                       REQUIRE(at != std::string::npos);
                                       t[at + 8] = t[at + 8] == '4' ? '3' : '4';
                                       return t;
                                   }()}) {
        op.after = bad;
        auto b = f.fw->begin(Origin::Ui, "raw");
        CHECK_FALSE(b->apply(op));
        b->abort();
        CHECK(f.doc().destroyed());
        CHECK(f.doc().revision() == revision);
    }
    // The Destroy is still the newest history entry, and undoing it restores the same bytes.
    REQUIRE(f.fw->undo(Origin::Ui));
    CHECK_FALSE(f.doc().destroyed());
    CHECK(f.doc().text() == snapshot);
}

TEST_CASE("tx: ops and transactions round-trip through JSON; inverses are involutions") {
    Fixture f("tx_json");
    auto b = f.fw->begin(Origin::Ui, "Several");
    b->setMergeKey("gesture-1");
    REQUIRE(b->set(f.frigate, "mass", "13000"));
    REQUIRE(b->insert(f.frigate, "thrusters", 0, R"({"bone": "x"})"));
    REQUIRE(b->move(f.frigate, "thrusters[0]", 2));
    REQUIRE(b->remove(f.frigate, "aiHints[retreatHealth]"));
    REQUIRE(b->set(f.frigate, "$comment", "\"edited\""));
    REQUIRE(b->commit());
    const Transaction& tx = f.fw->log().back();
    CHECK(tx.origin == Origin::Ui);
    CHECK(tx.mergeKey == "gesture-1");
    CHECK(tx.afterHash.at(f.frigate) == f.doc().contentHash());
    auto back = Transaction::fromJson(tx.toJson());
    REQUIRE(back);
    CHECK(back->toJson() == tx.toJson());
    CHECK(back->ops == tx.ops);
    for (const Op& op : tx.ops) CHECK(op.inverse().inverse() == op);
    CHECK_FALSE(Transaction::fromJson("{\"id\":1}"));
    CHECK_FALSE(Transaction::fromJson("not json"));
}

} // namespace
