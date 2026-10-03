// Undo/redo: global and per-document histories, redo invalidation, gesture merging, groups and
// the history cap.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <vector>

#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/samples.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

TxId setValue(Framework& fw, const DocId& doc, std::string_view path, std::string_view json, std::string mergeKey = {}) {
    auto b = fw.begin(Origin::Cli, std::string("Set ") + std::string(path));
    b->setMergeKey(std::move(mergeKey));
    REQUIRE(b->set(doc, path, json));
    auto id = b->commit();
    REQUIRE(id);
    return *id;
}

DocId addWren(Fixture& f) {
    const refl::TypeInfo* type = f.fw->types().find("sample.ship.ShipHullDef");
    auto b = f.fw->begin(Origin::Cli);
    refl::RecordHeader h;
    h.rid = 99;
    h.name = "hull/wren";
    auto id = b->createRecord(*type, "records/hull/wren.hrec", h, R"({"mass": 500})");
    REQUIRE(id);
    REQUIRE(b->commit());
    REQUIRE(f.fw->save(*id));
    return *id;
}

TEST_CASE("history: global and per-document undo/redo") {
    Fixture f("hist_docs");
    const DocId wren = addWren(f);
    const std::string k0 = f.doc().text();
    setValue(*f.fw, f.frigate, "mass", "13000");
    setValue(*f.fw, wren, "mass", "600");
    setValue(*f.fw, f.frigate, "handling/yawRate", "31");
    setValue(*f.fw, wren, "mass", "700");

    CHECK(f.fw->undoLabel() == "Set mass");
    CHECK(f.fw->undoLabel(f.frigate) == "Set handling/yawRate");
    // Per-document undo skips the other document's later edits.
    REQUIRE(f.fw->undo(Origin::Cli, f.frigate));
    REQUIRE(f.fw->undo(Origin::Cli, f.frigate));
    CHECK(f.doc().text() == k0);
    CHECK(f.get("mass") == "12000");
    CHECK(f.fw->documents().find(wren)->text().find("\"mass\": 700") != std::string::npos);
    CHECK_FALSE(f.fw->canUndo(f.frigate));
    CHECK(f.fw->canUndo(wren));
    CHECK(f.fw->canRedo(f.frigate));
    CHECK_FALSE(f.fw->canRedo(wren));
    // Global undo now reverts wren's last edit.
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.fw->documents().find(wren)->text().find("\"mass\": 600") != std::string::npos);
    // Global redo re-applies in reverse undo order: wren first.
    REQUIRE(f.fw->redo(Origin::Cli));
    CHECK(f.fw->documents().find(wren)->text().find("\"mass\": 700") != std::string::npos);
    REQUIRE(f.fw->redo(Origin::Cli, f.frigate));
    CHECK(f.get("mass") == "13000");
}

TEST_CASE("history: a new edit drops only its documents' redo entries") {
    Fixture f("hist_redo");
    const DocId wren = addWren(f);
    setValue(*f.fw, f.frigate, "mass", "13000");
    setValue(*f.fw, wren, "mass", "600");
    REQUIRE(f.fw->undo(Origin::Cli, f.frigate));
    REQUIRE(f.fw->undo(Origin::Cli, wren));
    CHECK(f.fw->canRedo(f.frigate));
    CHECK(f.fw->canRedo(wren));
    setValue(*f.fw, f.frigate, "mass", "14000");
    CHECK_FALSE(f.fw->canRedo(f.frigate));
    CHECK(f.fw->canRedo(wren));
    CHECK(f.fw->redo(Origin::Cli));
    CHECK_FALSE(f.fw->redo(Origin::Cli));
}

TEST_CASE("history: undo and redo are journaled transactions with the input path's origin") {
    Fixture f("hist_log");
    const TxId edit = setValue(*f.fw, f.frigate, "mass", "13000");
    auto u = f.fw->undo(Origin::Luau);
    REQUIRE(u);
    auto r = f.fw->redo(Origin::Rpc);
    REQUIRE(r);
    const auto log = f.fw->log();
    REQUIRE(log.size() == 3);
    CHECK(log[1].kind == TxKind::Undo);
    CHECK(log[1].target == edit);
    CHECK(log[1].origin == Origin::Luau);
    CHECK(log[1].label == "Undo Set mass");
    CHECK(log[2].kind == TxKind::Redo);
    CHECK(log[2].origin == Origin::Rpc);
    CHECK(log[2].id.lamport > log[1].id.lamport);
}

TEST_CASE("history: continuous gestures merge into one undo step") {
    Fixture f("hist_merge");
    const std::string before = f.doc().text();
    for (int i = 1; i <= 5; ++i) setValue(*f.fw, f.frigate, "handling/yawRate", std::to_string(30 + i), "drag-1");
    CHECK(f.fw->history().size() == 1);
    CHECK(f.fw->history()[0].tx.ops.size() == 1);
    CHECK(f.fw->history()[0].tx.ops[0].before == "30");
    CHECK(f.fw->history()[0].tx.ops[0].after == "35");
    CHECK(f.fw->log().size() == 5);  // every commit is journaled on its own
    // A different key starts a new step.
    setValue(*f.fw, f.frigate, "handling/yawRate", "40", "drag-2");
    CHECK(f.fw->history().size() == 2);
    REQUIRE(f.fw->undo(Origin::Ui));
    REQUIRE(f.fw->undo(Origin::Ui));
    CHECK(f.doc().text() == before);

    // A gesture that returns to its start leaves nothing to undo.
    setValue(*f.fw, f.frigate, "mass", "12500", "drag-3");
    setValue(*f.fw, f.frigate, "mass", "12000", "drag-3");
    CHECK(f.doc().text() == before);
    for (const HistoryEntry& e : f.fw->history()) CHECK(e.tx.mergeKey != "drag-3");
    // After an undo the next commit never merges into an undone entry.
    setValue(*f.fw, f.frigate, "mass", "12600", "drag-4");
    REQUIRE(f.fw->undo(Origin::Ui));
    setValue(*f.fw, f.frigate, "mass", "12700", "drag-4");
    CHECK(f.fw->history().back().tx.ops[0].before == "12000");
}

TEST_CASE("history: groups fold commands into one undo step") {
    Fixture f("hist_group");
    const std::string before = f.doc().text();
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    f.fw->beginGroup(Origin::Ui, "Retune");
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 14000})"));
    f.fw->beginGroup(Origin::Ui, "inner");  // nested: joins the outer group
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 12})"));
    REQUIRE(f.fw->endGroup());
    // A failing command inside the group rolls back only its own ops.
    CHECK_FALSE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "nosuch", "value": 5})"));
    CHECK(f.get("mass") == "14000");
    CHECK(f.fw->history().empty());
    auto id = f.fw->endGroup();
    REQUIRE(id);
    REQUIRE(f.fw->history().size() == 1);
    CHECK(f.fw->history()[0].tx.label == "Retune");
    CHECK(f.fw->history()[0].tx.ops.size() == 2);
    CHECK_FALSE(f.fw->endGroup());
    REQUIRE(f.fw->undo(Origin::Ui));
    CHECK(f.doc().text() == before);

    // The group is validated as one transaction when it commits: one bad value rejects it all.
    f.fw->beginGroup(Origin::Ui, "Invalid");
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 5})"));
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 5})"));
    CHECK_FALSE(f.fw->endGroup());
    CHECK(f.doc().text() == before);

    f.fw->beginGroup(Origin::Ui, "Abandoned");
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 14000})"));
    CHECK_FALSE(f.fw->undo(Origin::Ui));  // not inside a group
    f.fw->cancelGroup();
    CHECK(f.doc().text() == before);
    CHECK_FALSE(f.fw->inGroup());
}

TEST_CASE("history: cancelling a nested group rolls back only its own edits") {
    Fixture f("hist_group_nested_cancel");
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    f.fw->beginGroup(Origin::Ui, "Outer");
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 15000})"));
    f.fw->beginGroup(Origin::Ui, "Inner");
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/rollRate", "value": 95})"));
    CHECK(f.fw->groupDepth() == 2);
    f.fw->cancelGroup();
    CHECK(f.fw->groupDepth() == 1);
    CHECK(f.get("handling/rollRate") != "95");
    CHECK(f.get("mass") == "15000");  // the outer level's edit stays
    REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 12})"));
    CHECK(f.fw->history().empty());   // still inside the outer group
    REQUIRE(f.fw->endGroup());
    CHECK_FALSE(f.fw->inGroup());
    REQUIRE(f.fw->history().size() == 1);
    CHECK(f.fw->history()[0].tx.label == "Outer");
    CHECK(f.fw->history()[0].tx.ops.size() == 2);
}

TEST_CASE("history: the byte cap drops the oldest entries") {
    const fs::Path root = freshDir("hist_cap");
    REQUIRE(writeProject(root));
    FrameworkConfig cfg = deterministicConfig(root, {}, 3);
    cfg.historyByteLimit = 16 * 1024;
    auto fw = Framework::create(cfg);
    REQUIRE(fw);
    auto doc = openSampleHull(**fw);
    REQUIRE(doc);
    for (int i = 0; i < 400; ++i) setValue(**fw, *doc, "mass", std::to_string(1000 + i));
    CHECK((*fw)->historyBytes() <= cfg.historyByteLimit);
    CHECK((*fw)->history().size() < 400);
    CHECK((*fw)->history().size() > 10);
    usize undone = 0;
    while ((*fw)->undo(Origin::Cli)) ++undone;
    CHECK(undone == (*fw)->history().size());
    CHECK((*fw)->documents().find(*doc)->text().find("\"mass\": 12000") == std::string::npos);
}

TEST_CASE("history: closing a document drops its entries") {
    Fixture f("hist_close");
    const DocId wren = addWren(f);
    setValue(*f.fw, wren, "mass", "600");
    setValue(*f.fw, f.frigate, "mass", "13000");
    CHECK_FALSE(f.fw->close(wren));  // dirty
    REQUIRE(f.fw->close(wren, true));
    CHECK(f.fw->documents().find(wren) == nullptr);
    for (const HistoryEntry& e : f.fw->history()) {
        for (const Op& op : e.tx.ops) CHECK(op.doc != wren);
    }
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.get("mass") == "12000");
}

TEST_CASE("history: edits of every origin share one history") {
    Fixture f("hist_origins");
    setValue(*f.fw, f.frigate, "mass", "13000");
    auto b = f.fw->begin(Origin::Collab);
    REQUIRE(b->set(f.frigate, "mass", "15000"));
    REQUIRE(b->commit());
    CHECK(f.fw->history().back().tx.origin == Origin::Collab);
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.get("mass") == "13000");
    REQUIRE(f.fw->undo(Origin::Cli));
    CHECK(f.get("mass") == "12000");
}

#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
constexpr bool kAssertBudget = true;
#else
constexpr bool kAssertBudget = false;
#endif

TEST_CASE("perf: a one-op commit costs <= 0.1 ms, flat over a 50,000-transaction session") {
    // framework.h's budget. The cost must not grow with the session's length: the first and the last
    // 1,000 of 50,000 commits are timed in batches of 100, and the medians are compared.
    Fixture f("hist_perf");
    constexpr int kCommits = 50'000;
    constexpr int kWindow = 1'000;
    constexpr int kBatch = 100;
    std::vector<f64> first;
    std::vector<f64> last;
    for (int i = 0; i < kCommits; i += kBatch) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < kBatch; ++k) {
            auto b = f.fw->begin(Origin::Ui, "Set mass");
            REQUIRE(b->set(f.frigate, "mass", std::to_string(1000 + ((i + k) % 30000))));
            REQUIRE(b->commit());
        }
        const f64 us = std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0).count() / kBatch;
        if (i < kWindow) first.push_back(us);
        if (i >= kCommits - kWindow) last.push_back(us);
    }
    const auto median = [](std::vector<f64> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const f64 a = median(first);
    const f64 b = median(last);
    MESSAGE(std::format("one-op commit: {:.1f} us (first 1,000), {:.1f} us (last 1,000 of {}), log {} transactions", a, b, kCommits,
                        f.fw->log().size()));
    CHECK(f.fw->log().size() == static_cast<usize>(kCommits));
    CHECK(b <= 3.0 * a);
    if (kAssertBudget) CHECK(b <= 100.0);
}

} // namespace
