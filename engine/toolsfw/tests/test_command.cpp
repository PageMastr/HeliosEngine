// Command bus: registration, argument validation, origin stamping, rollback, results, exposure.

#include <doctest/doctest.h>

#include "helios/toolsfw/json_util.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

TEST_CASE("cmd: built-in commands are registered with docs") {
    Fixture f("cmd_builtin");
    for (const char* id : {"doc.setProperty", "doc.remove", "doc.insertElement", "doc.moveElement", "doc.rename", "doc.create",
                           "doc.destroy", "doc.save", "doc.saveAll", "doc.revert", "doc.reload", "doc.open", "doc.close",
                           "edit.undo", "edit.redo", "selection.set", "selection.clear", "selection.back", "selection.forward",
                           "validate.run"}) {
        const CommandDesc* c = f.fw->commands().find(id);
        REQUIRE_MESSAGE(c != nullptr, id);
        CHECK_MESSAGE(!c->doc.empty(), id);
        CHECK_MESSAGE(!c->label.empty(), id);
        CHECK(c->headless);
    }
    CHECK(f.fw->commands().find("edit.undo")->shortcut == "Ctrl+Z");
    auto all = f.fw->commands().commands();
    CHECK(std::is_sorted(all.begin(), all.end(), [](const CommandDesc* a, const CommandDesc* b) { return a->id < b->id; }));
}

TEST_CASE("cmd: arguments are validated against the command's ArgDescs") {
    Fixture f("cmd_args");
    CommandInvoker& inv = f.fw->invoker(Origin::Cli);
    CHECK(inv.invoke("no.such.command").errorCode() == ErrorCode::NotFound);
    CHECK(inv.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass"})").errorCode() == ErrorCode::InvalidArgument);
    CHECK(inv.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": 3, "value": 1})").errorCode() == ErrorCode::InvalidArgument);
    CHECK(inv.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 1, "extra": 1})").errorCode() ==
          ErrorCode::InvalidArgument);
    CHECK(inv.invoke("doc.setProperty", "[1, 2]").errorCode() == ErrorCode::InvalidArgument);
    CHECK(inv.invoke("doc.setProperty", "{broken").errorCode() == ErrorCode::ParseError);
    CHECK(inv.invoke("doc.setProperty", R"({"doc": "hull/nope", "path": "mass", "value": 1})").errorCode() == ErrorCode::NotFound);
    CHECK(f.fw->log().empty());
}

TEST_CASE("cmd: the invoker stamps the origin; arguments cannot") {
    Fixture f("cmd_origin");
    for (const Origin o : {Origin::Ui, Origin::UiScripted, Origin::Luau, Origin::Rpc, Origin::Cli, Origin::Import, Origin::Collab}) {
        auto r = f.fw->invoker(o).invoke("doc.setProperty",
                                         std::format(R"({{"doc": "hull/frigate", "path": "mass", "value": {}}})", 13000 + static_cast<int>(o)));
        REQUIRE(r);
        REQUIRE_FALSE(r->tx.isNull());
        CHECK(f.fw->log().back().origin == o);
        CHECK(f.fw->log().back().id == r->tx);
    }
    // There is no origin argument to smuggle in.
    CHECK_FALSE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty",
                                                  R"({"doc": "hull/frigate", "path": "mass", "value": 1, "origin": "collab"})"));
    CHECK(originName(Origin::UiScripted) == "ui-scripted");
    CHECK(parseOrigin("luau") == Origin::Luau);
    CHECK_FALSE(parseOrigin("human"));
}

TEST_CASE("cmd: another input path's commands are refused while a group is open") {
    Fixture f("cmd_group_origin");
    f.fw->beginGroup(Origin::Ui, "Retune");
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 14000})"));
    // An RPC command would otherwise join the group and be committed with origin ui.
    auto rpc = f.fw->invoker(Origin::Rpc).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 12})");
    REQUIRE_FALSE(rpc);
    CHECK(rpc.errorCode() == ErrorCode::InvalidState);
    CHECK(f.get("handling/yawRate") == "30");
    REQUIRE(f.fw->endGroup());
    REQUIRE(f.fw->history().size() == 1);
    CHECK(f.fw->history()[0].tx.origin == Origin::Ui);
    CHECK(f.fw->history()[0].tx.ops.size() == 1);
    // Once the group has closed, the RPC command runs with its own origin.
    REQUIRE(f.fw->invoker(Origin::Rpc).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 12})"));
    CHECK(f.fw->log().back().origin == Origin::Rpc);
}

TEST_CASE("cmd: a failing command rolls back all of its edits") {
    Fixture f("cmd_rollback");
    const std::string before = f.doc().text();
    CommandDesc c;
    c.id = "test.twoThenFail";
    c.label = "Two then fail";
    c.execute = [](CommandContext& ctx) -> Result<void> {
        const Document* d = ctx.framework().documents().find("hull/frigate");
        HELIOS_TRY(ctx.tx().set(d->id(), "mass", "20000"));
        HELIOS_TRY(ctx.tx().remove(d->id(), "thrusters[0]"));
        return Error{ErrorCode::Cancelled, "changed my mind"};
    };
    REQUIRE(f.fw->commands().add(c));
    CHECK(f.fw->commands().add(c).errorCode() == ErrorCode::AlreadyExists);
    auto r = f.fw->invoker(Origin::Ui).invoke("test.twoThenFail");
    CHECK(r.errorCode() == ErrorCode::Cancelled);
    CHECK(f.doc().text() == before);
    CHECK(f.fw->history().empty());
    CHECK(f.fw->commands().remove("test.twoThenFail"));
    CHECK_FALSE(f.fw->commands().remove("test.twoThenFail"));
}

TEST_CASE("cmd: canExecute gates invoke") {
    Fixture f("cmd_can");
    CommandInvoker& inv = f.fw->invoker(Origin::Ui);
    CHECK_FALSE(inv.canExecute("edit.undo"));
    CHECK(inv.invoke("edit.undo").errorCode() == ErrorCode::InvalidState);
    REQUIRE(inv.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 13000})"));
    CHECK(inv.canExecute("edit.undo"));
    CHECK(inv.canExecute("edit.undo", R"({"doc": "hull/frigate"})"));
    auto u = inv.invoke("edit.undo");
    REQUIRE(u);
    CHECK_FALSE(u->tx.isNull());  // undo commits its own transaction
    CHECK(inv.canExecute("edit.redo"));
    CHECK_FALSE(inv.canExecute("no.such"));
}

TEST_CASE("cmd: document commands and results") {
    Fixture f("cmd_docs");
    CommandInvoker& inv = f.fw->invoker(Origin::Cli);
    auto created = inv.invoke("doc.create", R"({"type": "sample.ship.ShipHullDef", "file": "records/hull/wren.hrec",
                                                  "name": "hull/wren", "values": {"mass": 700}, "rid": 42})");
    REQUIRE(created);
    auto id = json::normalize(created->result);
    REQUIRE(id);
    CHECK(f.fw->documents().find("hull/wren") != nullptr);
    REQUIRE(inv.invoke("doc.insertElement", R"({"doc": "hull/wren", "path": "thrusters", "value": {"bone": "b"}})"));
    REQUIRE(inv.invoke("doc.insertElement", R"({"doc": "hull/wren", "path": "thrusters", "index": 0})"));
    REQUIRE(inv.invoke("doc.moveElement", R"({"doc": "hull/wren", "path": "thrusters[0]", "to": 1})"));
    REQUIRE(inv.invoke("doc.remove", R"({"doc": "hull/wren", "path": "thrusters[0]"})"));
    REQUIRE(inv.invoke("doc.rename", R"({"doc": "hull/wren", "name": "hull/wren_mk2"})"));
    CHECK(f.fw->documents().find("hull/wren_mk2") != nullptr);
    auto saved = inv.invoke("doc.saveAll");
    REQUIRE(saved);
    CHECK(saved->result == "1");
    CHECK(fs::exists(f.root / "project" / "records" / "hull" / "wren.hrec"));
    REQUIRE(inv.invoke("selection.set", R"({"doc": "hull/wren_mk2", "path": "mass"})"));
    CHECK(f.fw->selection().items().size() == 1);
    CHECK(f.fw->selection().items()[0].path == "mass");
    REQUIRE(inv.invoke("selection.back"));
    CHECK(f.fw->selection().empty());
    REQUIRE(inv.invoke("selection.forward"));
    REQUIRE(inv.invoke("selection.clear"));
    REQUIRE(inv.invoke("doc.destroy", R"({"doc": "hull/wren_mk2"})"));
    REQUIRE(inv.invoke("doc.save", R"({"doc": ")" + id->substr(1, id->size() - 2) + R"("})"));
    CHECK_FALSE(fs::exists(f.root / "project" / "records" / "hull" / "wren.hrec"));

    auto v = inv.invoke("validate.run");
    REQUIRE(v);
    auto doc = refl::JsonDocument::parse(v->result);
    REQUIRE(doc);
    CHECK(json::getInteger(doc->root(), "errors") == 0);
    CHECK(doc->root().get("issues").isArray());
}

TEST_CASE("cmd: exposure registry feeds the layout lint") {
    Fixture f("cmd_expose");
    CommandBus& bus = f.fw->commands();
    CommandDesc c;
    c.id = "ui.test";
    c.headless = false;
    c.execute = [](CommandContext&) -> Result<void> { return {}; };
    REQUIRE(bus.add(c));
    auto unexposed = bus.unexposedCommands();
    CHECK(std::find(unexposed.begin(), unexposed.end(), "ui.test") != unexposed.end());
    bus.markExposed("ui.test", "MainMenu/View");
    bus.markExposed("ui.test", "MainMenu/View");
    CHECK(bus.exposures("ui.test").size() == 1);
    unexposed = bus.unexposedCommands();
    CHECK(std::find(unexposed.begin(), unexposed.end(), "ui.test") == unexposed.end());
    // Headless commands are included on request; palette-only ones never are.
    auto all = bus.unexposedCommands(true);
    CHECK(std::find(all.begin(), all.end(), "edit.undo") != all.end());
    CHECK(std::find(all.begin(), all.end(), "doc.setProperty") == all.end());
}

} // namespace
