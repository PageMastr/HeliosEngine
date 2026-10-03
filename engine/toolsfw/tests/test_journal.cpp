// Crash-recovery journal: format, torn tails, group commit, and replay (07 §1.2, AAA-STB-2).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "helios/core/hash.h"
#include "helios/toolsfw/automation.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

JournalHeader header(u32 pid = 1234) {
    JournalHeader h;
    h.project = "p";
    h.session = "s";
    h.user = "u";
    h.host = "h";
    h.pid = pid;
    h.created = 42;
    return h;
}

JournalRecord openRecord(u64 hash) {
    JournalRecord r;
    r.kind = JournalRecordKind::Open;
    r.doc = Guid(1, 2);
    r.file = "records/hull/frigate.hrec";
    r.typeName = "sample.ship.ShipHullDef";
    r.hash = hash;
    return r;
}

void setMass(Framework& fw, const DocId& doc, int mass) {
    auto b = fw.begin(Origin::Ui, "Set mass");
    REQUIRE(b->set(doc, "mass", std::to_string(mass)));
    REQUIRE(b->commit());
}

TEST_CASE("journal: records round-trip; the header is validated") {
    const fs::Path dir = freshDir("journal_format");
    const fs::Path path = dir / "a.hjl";
    {
        auto w = JournalWriter::create(path, header(), {.fsync = false});
        REQUIRE(w);
        REQUIRE((*w)->append(openRecord(0xabcdef)));
        JournalRecord tx;
        tx.kind = JournalRecordKind::Tx;
        tx.tx.id = {"u", 7};
        tx.tx.label = "L";
        tx.tx.origin = Origin::Luau;
        Op op;
        op.doc = Guid(1, 2);
        op.path = "mass";
        op.before = "1";
        op.after = "2";
        tx.tx.ops.push_back(op);
        REQUIRE((*w)->append(tx));
        CHECK(JournalWriter::create(path, header()).errorCode() == ErrorCode::AlreadyExists);
        REQUIRE((*w)->close(true));
        CHECK((*w)->close(true));  // idempotent
        CHECK_FALSE((*w)->append(tx));
    }
    auto scan = readJournal(path);
    REQUIRE(scan);
    CHECK(scan->header.project == "p");
    CHECK(scan->header.pid == 1234);
    CHECK(scan->clean);
    CHECK(scan->tornBytes == 0);
    REQUIRE(scan->records.size() == 3);
    CHECK(scan->records[0].kind == JournalRecordKind::Open);
    CHECK(scan->records[0].hash == 0xabcdef);
    CHECK(scan->records[1].tx.origin == Origin::Luau);
    CHECK(scan->records[1].tx.ops.size() == 1);
    CHECK(scan->records[2].kind == JournalRecordKind::End);

    REQUIRE(fs::writeTextFile(dir / "bad.hjl", "HJL1garbage"));
    CHECK(readJournal(dir / "bad.hjl").errorCode() == ErrorCode::Corrupt);
    REQUIRE(fs::writeTextFile(dir / "bad2.hjl", "NOPE12345678"));
    CHECK(readJournal(dir / "bad2.hjl").errorCode() == ErrorCode::Corrupt);
}

TEST_CASE("journal: a torn last record is ignored at every cut point") {
    const fs::Path dir = freshDir("journal_torn");
    const fs::Path path = dir / "a.hjl";
    {
        auto w = JournalWriter::create(path, header(), {.fsync = false});
        REQUIRE(w);
        for (int i = 0; i < 3; ++i) REQUIRE((*w)->append(openRecord(static_cast<u64>(i + 1))));
        REQUIRE((*w)->close(false));
    }
    auto full = fs::readFile(path);
    REQUIRE(full);
    auto scan = readJournal(path);
    REQUIRE(scan);
    REQUIRE(scan->records.size() == 3);
    const u64 lastStart = scan->records[2].offset;
    for (u64 cut = lastStart; cut < full->size(); ++cut) {
        std::vector<u8> bytes(full->begin(), full->begin() + static_cast<isize>(cut));
        REQUIRE(fs::writeFile(dir / "cut.hjl", bytes));
        auto s = readJournal(dir / "cut.hjl");
        REQUIRE(s);
        CHECK(s->records.size() == 2);
        CHECK(s->tornBytes == cut - lastStart);
        CHECK_FALSE(s->clean);
    }
    // A flipped byte inside a record stops the scan there (checksum).
    std::vector<u8> flipped = *full;
    flipped[static_cast<usize>(scan->records[1].offset + 12)] ^= 0x20;
    REQUIRE(fs::writeFile(dir / "flip.hjl", flipped));
    auto s = readJournal(dir / "flip.hjl");
    REQUIRE(s);
    CHECK(s->records.size() == 1);

    // reopen() truncates the torn tail and continues appending.
    std::vector<u8> torn(full->begin(), full->end() - 3);
    REQUIRE(fs::writeFile(dir / "reopen.hjl", torn));
    {
        auto w = JournalWriter::reopen(dir / "reopen.hjl", {.fsync = false});
        REQUIRE(w);
        REQUIRE((*w)->append(openRecord(99)));
        REQUIRE((*w)->close(true));
    }
    auto r = readJournal(dir / "reopen.hjl");
    REQUIRE(r);
    REQUIRE(r->records.size() == 4);
    CHECK(r->records[2].hash == 99);
    CHECK(r->clean);
    // Reopening a clean journal drops its end record.
    {
        auto w = JournalWriter::reopen(dir / "reopen.hjl", {.fsync = false});
        REQUIRE(w);
        REQUIRE((*w)->close(false));
    }
    auto again = readJournal(dir / "reopen.hjl");
    REQUIRE(again);
    CHECK(again->records.size() == 3);
    CHECK_FALSE(again->clean);
}

TEST_CASE("journal: group commit makes appends durable within the window") {
    const fs::Path dir = freshDir("journal_sync");
    auto w = JournalWriter::create(dir / "a.hjl", header(), {.flushInterval = std::chrono::milliseconds(20), .fsync = true});
    REQUIRE(w);
    const u64 initial = (*w)->syncCount();
    for (int i = 0; i < 50; ++i) REQUIRE((*w)->append(openRecord(static_cast<u64>(i))));
    // The flusher syncs once per window, not once per append.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((*w)->syncCount() == initial && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK((*w)->syncCount() > initial);
    CHECK((*w)->syncCount() < initial + 50);
    REQUIRE((*w)->close(true));
}

TEST_CASE("journal: session files, names and unclean detection") {
    const fs::Path root = freshDir("journal_sessions");
    CHECK(journalDirectory(root, "my project/../x") == root / "my_project_.._x");
    CHECK(journalDirectory(root, "..") == root / "project");
    const std::string name = newSessionName();
    CHECK(name.size() > 16);
    const fs::Path dir = journalDirectory(root, "proj");
    // A crashed session of a process that no longer exists.
    JournalHeader dead = header(0x7ffffff0u);
    dead.project = "proj";
    dead.host = "some-other-host";
    dead.created = 1;
    {
        auto w = JournalWriter::create(dir / "crashed.hjl", dead, {.fsync = false});
        REQUIRE(w);
        JournalRecord tx;
        tx.kind = JournalRecordKind::Tx;
        tx.tx.id = {"u", 1};
        REQUIRE((*w)->append(tx));
        REQUIRE((*w)->close(false));
    }
    JournalHeader ok = header();
    ok.created = 2;
    {
        auto w = JournalWriter::create(dir / "clean.hjl", ok, {.fsync = false});
        REQUIRE(w);
        REQUIRE((*w)->close(true));
    }
    auto all = listJournalSessions(root, "proj", false);
    CHECK(all.size() == 2);
    auto unclean = listJournalSessions(root, "proj", true);
    REQUIRE(unclean.size() == 1);
    CHECK(unclean[0].path.filename() == "crashed.hjl");
    CHECK(unclean[0].txCount == 1);
    CHECK(processIsRunning(1) == processIsRunning(1));  // callable; pid 1 exists on Linux, not on Windows
    CHECK_FALSE(processIsRunning(0));
}

TEST_CASE("journal: the framework journals opens, transactions, saves and a clean end") {
    Fixture f("journal_fw", /*journal=*/true);
    setMass(*f.fw, f.frigate, 13000);
    REQUIRE(f.fw->save(f.frigate));
    REQUIRE(f.fw->undo(Origin::Ui));
    const fs::Path path = f.fw->journal()->path();
    f.fw.reset();
    auto scan = readJournal(path);
    REQUIRE(scan);
    REQUIRE(scan->records.size() == 5);
    CHECK(scan->records[0].kind == JournalRecordKind::Open);
    CHECK(scan->records[1].kind == JournalRecordKind::Tx);
    CHECK(scan->records[2].kind == JournalRecordKind::Save);
    CHECK(scan->records[3].tx.kind == TxKind::Undo);
    CHECK(scan->records[4].kind == JournalRecordKind::End);
    CHECK(scan->clean);
}

TEST_CASE("journal: recovery replays unsaved transactions byte-identically") {
    const fs::Path root = freshDir("journal_recover");
    const fs::Path project = root / "project";
    REQUIRE(writeProject(project));
    std::string expected;
    fs::Path journalFile;
    {
        auto fw = Framework::create(deterministicConfig(project, root / "journal", 5, "crashed"));
        REQUIRE(fw);
        auto doc = openSampleHull(**fw);
        REQUIRE(doc);
        setMass(**fw, *doc, 13000);
        REQUIRE((*fw)->save(*doc));   // the replay base moves here
        setMass(**fw, *doc, 14000);
        auto b = (*fw)->begin(Origin::Luau);
        REQUIRE(b->insert(*doc, "thrusters", 1, R"({"bone": "aft"})"));
        REQUIRE(b->commit());
        REQUIRE((*fw)->undo(Origin::Ui));
        REQUIRE((*fw)->redo(Origin::Ui));
        // A record created and never saved.
        auto c = (*fw)->begin(Origin::Cli);
        refl::RecordHeader h;
        h.rid = 5;
        h.name = "hull/wren";
        REQUIRE(c->createRecord(*(*fw)->types().find("sample.ship.ShipHullDef"), "records/hull/wren.hrec", h, R"({"mass": 321})"));
        REQUIRE(c->commit());
        expected = (*fw)->documents().find(*doc)->text();
        journalFile = (*fw)->journal()->path();
        // Crash: the journal is never closed cleanly (no "end" record).
        (void)(*fw)->journal()->sync();
        const std::vector<u8> bytes = fs::readFile(journalFile).value();
        fw->reset();
        REQUIRE(fs::writeFile(journalFile, bytes));  // drop the end record the destructor wrote
    }
    auto fw = Framework::create(deterministicConfig(project, {}, 6, "recovering"));
    REQUIRE(fw);
    auto report = (*fw)->recover(journalFile);
    REQUIRE(report);
    CHECK_FALSE(report->clean);
    CHECK(report->replayed == 5);  // 14000, insert, undo, redo, create (13000 was saved)
    CHECK(report->skipped == 1);
    const Document* frigate = (*fw)->documents().find("hull/frigate");
    REQUIRE(frigate);
    CHECK(frigate->text() == expected);
    CHECK(frigate->dirty());
    const Document* wren = (*fw)->documents().find("hull/wren");
    REQUIRE(wren);
    CHECK(wren->text().find("\"mass\": 321") != std::string::npos);
    bool sawCreated = false;
    for (const RecoveredDocument& d : report->documents) {
        if (d.file == "records/hull/wren.hrec") {
            sawCreated = true;
            CHECK(d.status == DocRecovery::Created);
        } else {
            CHECK(d.status == DocRecovery::Replayed);
        }
    }
    CHECK(sawCreated);
    // The recovered edits are undoable.
    CHECK((*fw)->canUndo(frigate->id()));
}

TEST_CASE("journal: recovery refuses to replay onto a changed source file") {
    const fs::Path root = freshDir("journal_changed");
    const fs::Path project = root / "project";
    REQUIRE(writeProject(project));
    fs::Path journalFile;
    {
        auto fw = Framework::create(deterministicConfig(project, root / "journal", 7, "crashed"));
        REQUIRE(fw);
        auto doc = openSampleHull(**fw);
        REQUIRE(doc);
        setMass(**fw, *doc, 13000);
        journalFile = (*fw)->journal()->path();
    }
    // Someone edits the file after the crash.
    const fs::Path file = project / "records" / "hull" / "frigate.hrec";
    std::string text = fs::readTextFile(file).value();
    text.replace(text.find("12000"), 5, "11111");
    REQUIRE(fs::writeTextFile(file, text));

    auto fw = Framework::create(deterministicConfig(project, {}, 8, "recovering"));
    REQUIRE(fw);
    auto report = (*fw)->recover(journalFile);
    REQUIRE(report);
    CHECK(report->replayed == 0);
    REQUIRE(report->documents.size() == 1);
    CHECK(report->documents[0].status == DocRecovery::SourceChanged);
    CHECK((*fw)->documents().find("hull/frigate") == nullptr);
    // Forcing the replay still checks every op's precondition: mass is not 12000 any more.
    auto forced = (*fw)->recover(journalFile, {.ignoreSourceChanges = true});
    REQUIRE(forced);
    CHECK(forced->documents[0].status == DocRecovery::Conflict);
}

TEST_CASE("journal: cross-session reverts keep their kind and target through the journal and recovery") {
    const fs::Path root = freshDir("journal_revert");
    const fs::Path project = root / "project";
    REQUIRE(writeProject(project));
    fs::Path journalFile;
    const TxId target{"earlier-session", 7};
    {
        auto fw = Framework::create(deterministicConfig(project, root / "journal", 9, "reverting"));
        REQUIRE(fw);
        auto doc = openSampleHull(**fw);
        REQUIRE(doc);
        auto b = (*fw)->begin(Origin::Cli, "Undo Set mass");
        b->markRevert(TxKind::Undo, target);
        REQUIRE(b->set(*doc, "mass", "12500"));
        auto id = b->commit();
        REQUIRE(id);
        REQUIRE_FALSE((*fw)->log().empty());
        CHECK((*fw)->log().back().kind == TxKind::Undo);
        CHECK((*fw)->log().back().target == target);
        // In this session it is an ordinary, undoable history entry.
        CHECK((*fw)->canUndo());
        journalFile = (*fw)->journal()->path();
        (void)(*fw)->journal()->sync();
        const std::vector<u8> bytes = fs::readFile(journalFile).value();
        fw->reset();
        REQUIRE(fs::writeFile(journalFile, bytes));  // crash: no end record
    }
    auto scan = readJournal(journalFile);
    REQUIRE(scan);
    bool found = false;
    for (const JournalRecord& r : scan->records) {
        if (r.kind != JournalRecordKind::Tx) continue;
        found = true;
        CHECK(r.tx.kind == TxKind::Undo);
        CHECK(r.tx.target == target);
        CHECK(r.tx.origin == Origin::Cli);
    }
    CHECK(found);
    auto fw = Framework::create(deterministicConfig(project, {}, 10, "recovering"));
    REQUIRE(fw);
    auto report = (*fw)->recover(journalFile);
    REQUIRE(report);
    CHECK(report->replayed == 1);
    REQUIRE_FALSE((*fw)->log().empty());
    CHECK((*fw)->log().back().kind == TxKind::Undo);
    CHECK((*fw)->log().back().target == target);
}

TEST_CASE("journal: sessions created at the same instant list in a stable order") {
    const fs::Path root = freshDir("journal_ties");
    const fs::Path dir = journalDirectory(root, "proj");
    for (const char* name : {"b.hjl", "a.hjl", "c.hjl"}) {
        JournalHeader h = header();
        h.project = "proj";
        h.created = 5;
        auto w = JournalWriter::create(dir / name, h, {.fsync = false});
        REQUIRE(w);
        REQUIRE((*w)->close(true));
    }
    const auto sessions = listJournalSessions(root, "proj", false);
    REQUIRE(sessions.size() == 3);
    CHECK(sessions[0].path.filename() == "a.hjl");
    CHECK(sessions[1].path.filename() == "b.hjl");
    CHECK(sessions[2].path.filename() == "c.hjl");
    CHECK(currentProcessId() != 0);
}

/// Replaces `from` with `to` in the frigate's file (an external editor or `git pull`).
void editFile(const fs::Path& file, std::string_view from, std::string_view to) {
    std::string text = fs::readTextFile(file).value();
    const usize at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    REQUIRE(fs::writeTextFile(file, text));
}

TEST_CASE("journal: recovery after an external reload keeps the later unsaved edits") {
    Fixture f("reload_recover", /*journal=*/true);
    const fs::Path file = f.doc().path();
    editFile(file, "\"mass\": 12000", "\"mass\": 12345");
    REQUIRE(f.fw->reloadFromDisk(f.frigate));
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/yawRate", "value": 33})"));
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    auto fw2 = Framework::create(deterministicConfig(f.root / "project", {}, 5));
    REQUIRE(fw2);
    auto report = (*fw2)->recover(journal);
    REQUIRE(report);
    REQUIRE(report->documents.size() == 1);
    CHECK(report->documents[0].status == DocRecovery::Replayed);
    CHECK(report->replayed == 1);  // the yawRate edit; the reload itself is in the file
    const Document* d = (*fw2)->documents().find("records/hull/frigate.hrec");
    REQUIRE(d);
    CHECK(d->text() == expected);
    CHECK(d->dirty());
}

TEST_CASE("journal: recovery restores unsaved edits that a reload merged, then the later edits") {
    Fixture f("reload_merge_recover", /*journal=*/true);
    const fs::Path file = f.doc().path();
    // An unsaved local edit, then an external edit of another field: the reload merges them.
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 15000})"));
    editFile(file, "\"yawRate\": 30", "\"yawRate\": 31");
    REQUIRE(f.fw->reloadFromDisk(f.frigate));
    CHECK(f.get("mass") == "15000");
    CHECK(f.get("handling/yawRate") == "31");
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "handling/rollRate", "value": 95})"));
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    auto scan = readJournal(journal);
    REQUIRE(scan);
    const auto snapshotRecord = std::find_if(scan->records.begin(), scan->records.end(),
                                             [](const JournalRecord& r) { return r.kind == JournalRecordKind::Open && r.snapshot; });
    REQUIRE(snapshotRecord != scan->records.end());
    CHECK(snapshotRecord->hash == hash64(fs::readTextFile(file).value()));

    auto fw2 = Framework::create(deterministicConfig(f.root / "project", {}, 5));
    REQUIRE(fw2);
    auto report = (*fw2)->recover(journal);
    REQUIRE(report);
    REQUIRE(report->documents.size() == 1);
    CHECK(report->documents[0].status == DocRecovery::Replayed);
    CHECK(report->replayed == 2);  // the merged snapshot, then rollRate
    const Document* d = (*fw2)->documents().find("hull/frigate");
    REQUIRE(d);
    CHECK(d->text() == expected);
    // The restored state is one undo step on top of the file.
    REQUIRE((*fw2)->undo(Origin::Ui));
    REQUIRE((*fw2)->undo(Origin::Ui));
    CHECK(d->text() == fs::readTextFile(file).value());

    // A snapshot is never forced onto a file that changed after the reload.
    editFile(file, "\"yawRate\": 31", "\"yawRate\": 32");
    auto fw3 = Framework::create(deterministicConfig(f.root / "project", {}, 6));
    REQUIRE(fw3);
    auto forced = (*fw3)->recover(journal, {.ignoreSourceChanges = true});
    REQUIRE(forced);
    REQUIRE(forced->documents.size() == 1);
    CHECK(forced->documents[0].status == DocRecovery::SourceChanged);
    CHECK(forced->replayed == 0);
}

TEST_CASE("journal: default session names of one project never collide") {
    const fs::Path root = freshDir("journal_names");
    REQUIRE(writeProject(root / "project"));
    std::vector<std::unique_ptr<Framework>> fws;
    std::vector<std::string> sessions;
    for (int i = 0; i < 3; ++i) {
        // Opened back to back: usually within one second, so the default names would repeat.
        FrameworkConfig cfg = deterministicConfig(root / "project", root / "journal", 9, "");
        auto fw = Framework::create(cfg);
        REQUIRE(fw);
        sessions.push_back((*fw)->config().session);
        fws.push_back(std::move(*fw));
    }
    CHECK(sessions[0] != sessions[1]);
    CHECK(sessions[1] != sessions[2]);
    CHECK(sessions[0] != sessions[2]);
    // An explicit name must be free.
    FrameworkConfig taken = deterministicConfig(root / "project", root / "journal", 9, sessions[0]);
    CHECK(Framework::create(taken).errorCode() == ErrorCode::AlreadyExists);
}

TEST_CASE("journal: the Lamport floor continues transaction ids across sessions") {
    const fs::Path root = freshDir("journal_lamport");
    REQUIRE(writeProject(root / "project"));
    FrameworkConfig cfg = deterministicConfig(root / "project", root / "journal", 9, "first");
    cfg.lamportFloor = 41;
    auto fw = Framework::create(cfg);
    REQUIRE(fw);
    auto doc = openSampleHull(**fw);
    REQUIRE(doc);
    setMass(**fw, *doc, 13000);
    CHECK((*fw)->log().back().id.lamport == 42);
    fw->reset();
    const auto sessions = listJournalSessions(root / "journal", "test-project", false);
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].maxLamport == 42);
}

// ---- replay bases and open groups (review round 2 of #40) --------------------------------------
// A save, or the "open" record a reload journals, is a document's replay base. A group's ops are
// applied at once but journaled only when the outermost group ends, so a base journaled inside
// the group would precede ops that are already in it: recovery then failed with a conflict and
// dropped every later edit, or brought back a cancelled group's edits.

constexpr const char* kSetMass = R"({"doc": "hull/frigate", "path": "mass", "value": 15000})";
constexpr const char* kSetRoll = R"({"doc": "hull/frigate", "path": "handling/rollRate", "value": 95})";
constexpr const char* kSetPitch = R"({"doc": "hull/frigate", "path": "handling/pitchRate", "value": 44})";
constexpr const char* kSaveFrigate = R"({"doc": "hull/frigate"})";

TEST_CASE("journal: a save inside a group that edited the document is refused, and recovery keeps the later edits") {
    Fixture f("save_in_group", /*journal=*/true);
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    const fs::Path file = f.doc().path();
    const std::string onDisk = fs::readTextFile(file).value();
    f.fw->beginGroup(Origin::Ui, "Outer");
    REQUIRE(ui.invoke("doc.setProperty", kSetMass));
    CHECK(ui.invoke("doc.save", kSaveFrigate).errorCode() == ErrorCode::InvalidState);
    CHECK(f.fw->save(f.frigate).errorCode() == ErrorCode::InvalidState);
    CHECK(f.fw->saveAll().errorCode() == ErrorCode::InvalidState);
    CHECK(fs::readTextFile(file).value() == onDisk);  // nothing was written
    REQUIRE(f.fw->endGroup());
    REQUIRE(ui.invoke("doc.setProperty", kSetRoll));
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    const Recovered r = recoverFixture(f, journal);
    CHECK(r.status() == DocRecovery::Replayed);
    CHECK(r.report.replayed == 2);
    CHECK(r.text() == expected);  // before the fix: conflict at 'mass', rollRate 95 lost
}

TEST_CASE("journal: a save inside a group that has not edited the document stays a valid replay base") {
    Fixture f("save_before_group_edit", /*journal=*/true);
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    REQUIRE(ui.invoke("doc.setProperty", kSetMass));
    f.fw->beginGroup(Origin::Ui, "Outer");
    REQUIRE(ui.invoke("doc.save", kSaveFrigate));  // the group holds nothing of the frigate yet
    REQUIRE(ui.invoke("doc.setProperty", kSetRoll));
    REQUIRE(f.fw->endGroup());
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    const Recovered r = recoverFixture(f, journal);
    CHECK(r.status() == DocRecovery::Replayed);
    CHECK(r.report.replayed == 1);  // only the group: the mass edit is in the saved file
    CHECK(r.text() == expected);
}

TEST_CASE("journal: Editor.transaction cannot save a document it edited; recovery keeps the later edits") {
    Fixture f("save_in_luau_tx", /*journal=*/true);
    const fs::Path file = f.doc().path();
    const std::string onDisk = fs::readTextFile(file).value();
    auto a = Automation::create(*f.fw);
    REQUIRE(a);
    // An uncaught refusal fails the script and rolls the transaction back.
    auto failed = (*a)->run("save", R"(
        Editor.transaction("t", function()
            Record.set("hull/frigate", "mass", "15000")
            Editor.cmd("doc.save", '{"doc": "hull/frigate"}')
        end))");
    CHECK_FALSE(failed);
    CHECK(f.get("mass") == "12000");
    CHECK(fs::readTextFile(file).value() == onDisk);
    // A caught refusal keeps the transaction, which commits as one step.
    auto caught = (*a)->run("save_caught", R"(
        Editor.transaction("t", function()
            Record.set("hull/frigate", "mass", "15000")
            assert(not pcall(Editor.cmd, "doc.save", '{"doc": "hull/frigate"}'))
        end))");
    REQUIRE_MESSAGE(caught, (caught ? std::string() : caught.error().toString()));
    CHECK(fs::readTextFile(file).value() == onDisk);
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", kSetRoll));
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    a->reset();
    f.fw.reset();

    const Recovered r = recoverFixture(f, journal);
    CHECK(r.status() == DocRecovery::Replayed);
    CHECK(r.text() == expected);
}

TEST_CASE("journal: a reload inside a group is refused, and recovery keeps the edits in and after the group") {
    Fixture f("reload_in_group", /*journal=*/true);
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    editFile(f.doc().path(), "\"yawRate\": 30", "\"yawRate\": 31");
    f.fw->beginGroup(Origin::Ui, "Outer");
    REQUIRE(ui.invoke("doc.setProperty", kSetMass));
    CHECK(f.fw->reloadFromDisk(f.frigate).errorCode() == ErrorCode::InvalidState);
    CHECK(ui.invoke("doc.revert", kSaveFrigate).errorCode() == ErrorCode::InvalidState);
    CHECK(f.get("handling/yawRate") == "30");
    REQUIRE(ui.invoke("doc.setProperty", kSetPitch));
    REQUIRE(f.fw->endGroup());
    REQUIRE(f.fw->reloadFromDisk(f.frigate));  // after the group: merges the external yawRate
    CHECK(f.get("handling/yawRate") == "31");
    REQUIRE(ui.invoke("doc.setProperty", kSetRoll));
    const std::string expected = f.doc().text();
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    const Recovered r = recoverFixture(f, journal);
    CHECK(r.status() == DocRecovery::Replayed);
    CHECK(r.text() == expected);  // before the fix: conflict, rollRate and pitchRate lost
}

TEST_CASE("journal: cancelling a group never leaves its edits in a replay base") {
    Fixture f("reload_in_group_cancel", /*journal=*/true);
    CommandInvoker& ui = f.fw->invoker(Origin::Ui);
    const std::string original = f.doc().text();
    editFile(f.doc().path(), "\"yawRate\": 30", "\"yawRate\": 31");
    f.fw->beginGroup(Origin::Ui, "Outer");
    REQUIRE(ui.invoke("doc.setProperty", kSetMass));
    CHECK(f.fw->reloadFromDisk(f.frigate).errorCode() == ErrorCode::InvalidState);
    f.fw->cancelGroup();
    // The cancelled edit is gone, and the external yawRate 31 was never half-loaded into the group
    // (before the fix the cancel silently rolled it back as well).
    CHECK(f.doc().text() == original);
    const fs::Path journal = f.fw->journal()->path();
    f.fw.reset();

    // Nothing in the journal holds the cancelled mass: the file changed after the session's open,
    // so recovery reports it and replays nothing. Before the fix, the reload's snapshot brought
    // mass 15000 back.
    const Recovered r = recoverFixture(f, journal);
    CHECK(r.report.replayed == 0);
    CHECK(r.status() == DocRecovery::SourceChanged);
    CHECK(r.text().find("\"mass\": 15000") == std::string::npos);
}

TEST_CASE("journal: an uncommitted builder's edits block saving, closing and reloading their document") {
    Fixture f("pending_builder", /*journal=*/true);
    auto b = f.fw->begin(Origin::Ui, "Pending");
    REQUIRE(b->set(f.frigate, "mass", "15000"));
    CHECK(f.fw->save(f.frigate).errorCode() == ErrorCode::InvalidState);
    CHECK(f.fw->saveAll().errorCode() == ErrorCode::InvalidState);
    CHECK(f.fw->close(f.frigate, /*discard=*/true).errorCode() == ErrorCode::InvalidState);
    CHECK(f.fw->reloadFromDisk(f.frigate).errorCode() == ErrorCode::InvalidState);
    REQUIRE(b->commit());
    CHECK(f.fw->save(f.frigate));
    b.reset();
    // An aborted builder holds nothing.
    auto c = f.fw->begin(Origin::Ui, "Aborted");
    REQUIRE(c->set(f.frigate, "mass", "16000"));
    c->abort();
    CHECK(f.fw->reloadFromDisk(f.frigate));
}

TEST_CASE("journal: a save or reload whose replay base cannot be journaled reports it") {
    Fixture f("base_not_journaled", /*journal=*/true);
    const fs::Path file = f.doc().path();
    REQUIRE(f.fw->invoker(Origin::Ui).invoke("doc.setProperty", kSetMass));
    REQUIRE(f.fw->journal()->close(false));  // every later append fails
    auto saved = f.fw->save(f.frigate);
    REQUIRE_FALSE(saved);
    CHECK(saved.error().message.find("was saved") != std::string::npos);
    CHECK(fs::readTextFile(file).value() == f.doc().text());  // the file itself was written
    // A reload with no edit to apply still needs a new base (the file's bytes changed).
    REQUIRE(fs::writeTextFile(file, f.doc().text() + "\n"));
    auto reloaded = f.fw->reloadFromDisk(f.frigate);
    REQUIRE_FALSE(reloaded);
    CHECK(reloaded.error().message.find("was reloaded") != std::string::npos);
}

} // namespace
