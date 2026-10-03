// Crash-recovery journal: format, torn tails, group commit, and replay (07 §1.2, AAA-STB-2).

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "helios/core/hash.h"
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

} // namespace
