// The journal is untrusted input: every path it names stays inside the project (Workspace::confine),
// and a journal of another project is refused. These are the regression cases of the round-5
// review of PR #40 (a crafted journal made `helios-tool journal replay --save` write, rewrite and
// delete files outside the project) plus the spelling and link rules behind them.

#include <doctest/doctest.h>

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "helios/core/platform.h"
#include "test_util.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

/// Copies the journal `from` to `to` record by record, letting `edit` change each record and
/// `editHeader` the header. The end record is dropped, so the copy reads as a crashed session.
void craftJournal(const fs::Path& from, const fs::Path& to, const std::function<void(JournalRecord&)>& edit,
                  const std::function<void(JournalHeader&)>& editHeader = {}) {
    auto scan = readJournal(from);
    REQUIRE(scan);
    JournalHeader header = scan->header;
    if (editHeader) editHeader(header);
    (void)fs::remove(to);
    auto w = JournalWriter::create(to, header, {.fsync = false});
    REQUIRE(w);
    for (JournalRecord r : scan->records) {
        if (r.kind == JournalRecordKind::End) continue;
        edit(r);
        REQUIRE((*w)->append(r));
    }
    REQUIRE((*w)->close(false));
}

/// The review's scenario: `<root>/project` and a byte-identical second project `<root>/other`. A
/// journaled session sets the project's frigate mass to 13000 and creates records/hull/probe.hrec,
/// saving neither, and ends.
struct Scenario {
    fs::Path root;
    fs::Path project;
    fs::Path journal;
    fs::Path frigate;       ///< The project's frigate file.
    fs::Path otherFrigate;  ///< The other project's frigate file.
    std::string original;   ///< Both frigates' text.

    explicit Scenario(std::string_view name, bool destroyFrigate = false) {
        Fixture f(name, /*journal=*/true);
        root = f.root;
        project = root / "project";
        frigate = project / "records" / "hull" / "frigate.hrec";
        REQUIRE(writeProject(root / "other"));
        otherFrigate = root / "other" / "records" / "hull" / "frigate.hrec";
        original = fs::readTextFile(otherFrigate).value();
        CommandInvoker& ui = f.fw->invoker(Origin::Ui);
        REQUIRE(ui.invoke("doc.setProperty", R"({"doc": "hull/frigate", "path": "mass", "value": 13000})"));
        REQUIRE(ui.invoke("doc.create", R"({"type": "sample.ship.ShipHullDef", "file": "records/hull/probe.hrec", "name": "hull/probe"})"));
        if (destroyFrigate) REQUIRE(ui.invoke("doc.destroy", R"({"doc": "hull/frigate"})"));
        journal = f.fw->journal()->path();
    }

    /// A framework over the project (no journal), as after a restart.
    std::unique_ptr<Framework> restart(std::string_view project = "test-project") const {
        FrameworkConfig cfg = deterministicConfig(this->project, {}, 5);
        cfg.project = std::string(project);
        auto fw = Framework::create(cfg);
        REQUIRE(fw);
        return std::move(*fw);
    }

    /// Nothing outside the project was touched and the project's own files are as before.
    void checkUntouched() const {
        CHECK(fs::readTextFile(otherFrigate).value() == original);
        CHECK(fs::readTextFile(frigate).value() == original);
        CHECK_FALSE(fs::exists(root / "outside"));
        CHECK_FALSE(fs::exists(project / "records" / "hull" / "probe.hrec"));
    }
};

/// recover() must refuse the journal as a whole, naming `needle`, with nothing opened or applied;
/// a save afterwards then writes nothing.
void checkRefused(Framework& fw, const fs::Path& journal, std::string_view needle) {
    auto report = fw.recover(journal);
    CHECK_FALSE(report);  // not REQUIRE: without the rule, the save below shows what escapes
    if (!report) {
        CHECK(report.errorCode() == ErrorCode::InvalidArgument);
        INFO(report.error().message);
        CHECK(report.error().message.find(needle) != std::string::npos);
        CHECK(report.error().message.find("nothing was replayed") != std::string::npos);
    }
    CHECK(fw.documents().size() == 0);
    CHECK(fw.log().empty());
    auto saved = fw.saveAll();
    REQUIRE(saved);
    CHECK(*saved == 0);
}

/// Tries to create a directory symbolic link; false (with a message) where this platform or
/// account cannot (Windows without Developer Mode or SeCreateSymbolicLinkPrivilege).
bool makeDirectoryLink(const fs::Path& target, const fs::Path& link) {
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (ec) MESSAGE("skipped: cannot create a symbolic link here: " << ec.message());
    return !ec;
}

bool makeFileLink(const fs::Path& target, const fs::Path& link) {
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) MESSAGE("skipped: cannot create a symbolic link here: " << ec.message());
    return !ec;
}

TEST_CASE("confine: the spelling rule keeps every path inside the project") {
    Fixture f("confine_spelling");
    const Workspace& ws = f.fw->documents();
    for (const auto& [path, expected] : std::vector<std::pair<std::string, std::string>>{
             {"records/hull/frigate.hrec", "records/hull/frigate.hrec"},
             {"records\\hull\\frigate.hrec", "records/hull/frigate.hrec"},  // '\' separates on every platform
             {"./records//hull/./frigate.hrec", "records/hull/frigate.hrec"},
             {"x.hrec", "x.hrec"},
             {"records/hull/a b.hrec", "records/hull/a b.hrec"},
             {"records/console/conveyor.hrec", "records/console/conveyor.hrec"},  // not device names
             {"records/COM10.hrec", "records/COM10.hrec"},
             {"records/nul_drone.hrec", "records/nul_drone.hrec"},
             {".hidden/x.hrec", ".hidden/x.hrec"},
         }) {
        INFO(path);
        for (PathOrigin origin : {PathOrigin::Untrusted, PathOrigin::Caller}) {
            auto r = ws.confine(path, origin, PathCheck::Lexical);
            REQUIRE_MESSAGE(r, (r ? std::string() : r.error().message));
            CHECK(r->relative == expected);
            CHECK(r->absolute == ws.root() / fs::pathFromUtf8(expected));
        }
    }
    for (const std::string& path : std::vector<std::string>{
             "", ".", "records/", "../outside/evil.hrec", "..\\outside\\evil.hrec", "records/../../x.hrec",
             "records/..\\..\\x.hrec", "records/hull/../frigate.hrec",
             "/etc/x.hrec", "\\x.hrec", "C:/x.hrec", "c:x.hrec", "C:\\Windows\\x.hrec", "z:/x.hrec",
             "\\\\server\\share\\x.hrec", "//server/share/x.hrec", "\\\\?\\C:\\x.hrec", "\\\\.\\pipe\\x.hrec",
             "//?/c:/x.hrec", "records/hull/frigate.hrec:evil", "records/hull/frigate.hrec::$DATA",
             "records/hull/x.hrec.", "records/hull/x.hrec ", "records/.. /x.hrec", "records/... /x.hrec",
             "records/CON.hrec", "records/con/x.hrec", "records/Nul.hrec", "records/aux .hrec", "records/COM1.hrec",
             "records/lpt9.x.hrec", "records/COM\xC2\xB9.hrec", "records/CONIN$.hrec", "records/conout$/x.hrec",
             "records/prn.tar.hrec", "records/a?.hrec", "records/a*.hrec", "records/a|b.hrec", "records/a\"b.hrec",
             "records/a<b.hrec", "records/a>b.hrec", std::string("records/a\0b.hrec", 16), "records/a\x01.hrec",
             "records/a\x1b[31m.hrec", "records/a\xC2\x9B.hrec", "records/x.sh", "records/hull/evil.bat", "evil.sh",
             "records/x.HREC", ".hrec", "records/x.hrec/", "records/x.hrecx",
         }) {
        INFO("refused: " << printable(path));
        for (PathOrigin origin : {PathOrigin::Untrusted, PathOrigin::Caller}) {
            auto r = ws.confine(path, origin, PathCheck::Lexical);
            CHECK_FALSE(r);
            if (!r) CHECK(r.errorCode() == ErrorCode::InvalidArgument);
        }
    }
    // Refusals name the path, with control characters escaped (no terminal escape sequences).
    auto esc = ws.confine("records/a\x1b[31m.hrec", PathOrigin::Untrusted, PathCheck::Lexical);
    REQUIRE_FALSE(esc);
    CHECK(esc.error().message.find("records/a\\x1b[31m.hrec") != std::string::npos);
    CHECK(esc.error().message.find('\x1b') == std::string::npos);
    auto dots = ws.confine("../outside/evil.sh", PathOrigin::Untrusted, PathCheck::Lexical);
    REQUIRE_FALSE(dots);
    CHECK(dots.error().message.find("'../outside/evil.sh'") != std::string::npos);

    // A caller may also name a file by its absolute path under the root; a journal may not.
    const std::string inside = fs::pathToUtf8(ws.root() / "records" / "hull" / "frigate.hrec");
    auto abs = ws.confine(inside, PathOrigin::Caller, PathCheck::OnDisk);
    REQUIRE_MESSAGE(abs, (abs ? std::string() : abs.error().message));
    CHECK(abs->relative == "records/hull/frigate.hrec");
    CHECK_FALSE(ws.confine(inside, PathOrigin::Untrusted, PathCheck::Lexical));
    CHECK_FALSE(ws.confine(fs::pathToUtf8(ws.root()), PathOrigin::Caller, PathCheck::Lexical));
    CHECK_FALSE(ws.confine(fs::pathToUtf8(ws.root() / ".." / "other" / "x.hrec"), PathOrigin::Caller, PathCheck::Lexical));
    CHECK_FALSE(ws.confine(fs::pathToUtf8(ws.root().parent_path() / "project2" / "x.hrec"), PathOrigin::Caller,
                           PathCheck::Lexical));  // a sibling whose name starts like the root's
    if constexpr (platform::kIsWindows) {
        // The drive letter compares without case, as Windows does.
        std::string flipped = fs::pathToGenericUtf8(ws.root() / "records" / "hull" / "frigate.hrec");
        REQUIRE(flipped.size() > 2);
        REQUIRE(flipped[1] == ':');
        flipped[0] = static_cast<char>(flipped[0] >= 'a' ? flipped[0] - 'a' + 'A' : flipped[0] - 'A' + 'a');
        auto r = ws.confine(flipped, PathOrigin::Caller, PathCheck::OnDisk);
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().message));
        CHECK(r->relative == "records/hull/frigate.hrec");
    }
}

TEST_CASE("confine: open, createRecord and raw ops refuse files outside the project") {
    Fixture f("confine_api");
    REQUIRE(writeProject(f.root / "other"));
    const fs::Path otherFrigate = f.root / "other" / "records" / "hull" / "frigate.hrec";
    const refl::TypeInfo* type = f.fw->types().find("sample.ship.ShipHullDef");
    REQUIRE(type);
    // Framework::open with an explicit type used to open any file.
    CHECK(f.fw->open(otherFrigate, type).errorCode() == ErrorCode::InvalidArgument);
    CHECK(f.fw->open("../other/records/hull/frigate.hrec", type).errorCode() == ErrorCode::InvalidArgument);
    CHECK(f.fw->invoker(Origin::Rpc).invoke("doc.open", R"({"file": "../other/records/hull/frigate.hrec"})").errorCode() ==
          ErrorCode::InvalidArgument);
    // An absolute path under the root still opens (openAll() passes those).
    auto same = f.fw->open(f.root / "project" / "records" / "hull" / "frigate.hrec");
    REQUIRE(same);
    CHECK((*same)->id() == f.frigate);
    CHECK(f.fw->documents().size() == 1);

    refl::RecordHeader h;
    h.rid = 9;
    h.name = "hull/evil";
    auto b = f.fw->begin(Origin::Cli);
    CHECK_FALSE(b->createRecord(*type, "../outside/evil.hrec", h));
    CHECK_FALSE(b->createRecord(*type, fs::pathToUtf8(f.root / "outside" / "evil.hrec"), h));
    CHECK_FALSE(b->createRecord(*type, "records/hull/evil.sh", h));
    CHECK_FALSE(b->createRecord(*type, "C:/evil.hrec", h));
    // Raw ops (replay, collaboration, patches) get the same rule.
    Op create;
    create.kind = OpKind::Create;
    create.doc = Guid(7, 7);
    create.typeName = std::string(type->qualifiedName);
    create.after = f.doc().text();
    for (const char* file : {"../outside/evil.sh", "../outside/evil.hrec", "records/hull/evil.sh", "..\\outside\\evil.hrec",
                             "/tmp/evil.hrec", "C:/evil.hrec"}) {
        INFO(file);
        create.file = file;
        auto r = b->apply(create);
        CHECK(r.errorCode() == ErrorCode::InvalidArgument);
    }
    // A Destroy must name the document's own file (it is the file the save deletes).
    Op destroy;
    destroy.kind = OpKind::Destroy;
    destroy.doc = f.frigate;
    destroy.typeName = std::string(type->qualifiedName);
    destroy.before = f.doc().text();
    for (const char* file : {"../other/records/hull/frigate.hrec", "records/itm/scrap_plate.hrec", ""}) {
        INFO(file);
        destroy.file = file;
        CHECK(b->apply(destroy).errorCode() == ErrorCode::InvalidArgument);
    }
    CHECK(b->empty());
    b->abort();
    CHECK_FALSE(f.doc().destroyed());
    CHECK(f.fw->documents().size() == 1);
    CHECK_FALSE(fs::exists(f.root / "outside"));
    CHECK_FALSE(fs::exists(f.root / "project" / "records" / "hull" / "evil.sh"));
}

TEST_CASE("journal: recovery never reads or writes a file outside the project") {
    // The review's reproduction: the open record names another project's record and the Create
    // op a shell script outside the project.
    const Scenario s("confine_escape");
    craftJournal(s.journal, s.root / "crafted.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Open && r.file == "records/hull/frigate.hrec") r.file = "../other/records/hull/frigate.hrec";
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "../outside/evil.sh";
        }
    });
    checkRefused(*s.restart(), s.root / "crafted.hjl", "../other/records/hull/frigate.hrec");
    s.checkUntouched();

    // Only the Create op crafted: the frigate's legitimate edit is not replayed either.
    craftJournal(s.journal, s.root / "create_only.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "../outside/evil.sh";
        }
    });
    checkRefused(*s.restart(), s.root / "create_only.hjl", "../outside/evil.sh");
    s.checkUntouched();

    // An in-project Create of a file that is not a record.
    craftJournal(s.journal, s.root / "not_hrec.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "records/hull/evil.sh";
        }
    });
    checkRefused(*s.restart(), s.root / "not_hrec.hjl", "records/hull/evil.sh");
    s.checkUntouched();
    CHECK_FALSE(fs::exists(s.project / "records" / "hull" / "evil.sh"));

    // Control: the journal as written replays both documents.
    craftJournal(s.journal, s.root / "plain.hjl", [](JournalRecord&) {});
    auto fw = s.restart();
    auto report = fw->recover(s.root / "plain.hjl");
    REQUIRE_MESSAGE(report, (report ? std::string() : report.error().message));
    CHECK(report->replayed == 2);
    REQUIRE(fw->saveAll());
    CHECK(fs::readTextFile(s.frigate).value().find("\"mass\": 13000,") != std::string::npos);
    CHECK(fs::exists(s.project / "records" / "hull" / "probe.hrec"));
    CHECK(fs::readTextFile(s.otherFrigate).value() == s.original);
}

TEST_CASE("journal: recovery refuses absolute, drive-letter, UNC, device and backslash paths") {
    const Scenario s("confine_absolute");
    const std::vector<std::string> hostile = {
        fs::pathToGenericUtf8(s.otherFrigate),  // an existing file, by its absolute path
        fs::pathToUtf8(s.otherFrigate),
        "C:/Users/Public/frigate.hrec",
        "c:\\Users\\Public\\frigate.hrec",
        "C:frigate.hrec",
        "\\\\server\\share\\records\\hull\\frigate.hrec",
        "//server/share/records/hull/frigate.hrec",
        "\\\\?\\C:\\records\\hull\\frigate.hrec",
        "\\\\.\\C:\\records\\hull\\frigate.hrec",
        "..\\other\\records\\hull\\frigate.hrec",
        "records\\..\\..\\other\\records\\hull\\frigate.hrec",
        "records/hull/frigate.hrec:stream",
        "records/hull/NUL.hrec",
    };
    for (usize i = 0; i < hostile.size(); ++i) {
        INFO(hostile[i]);
        const fs::Path crafted = s.root / std::format("crafted{}.hjl", i);
        craftJournal(s.journal, crafted, [&](JournalRecord& r) {
            if (r.kind == JournalRecordKind::Open) r.file = hostile[i];
        });
        checkRefused(*s.restart(), crafted, "record 0 (open of document");
        // The same path in the Create op.
        const fs::Path crafted2 = s.root / std::format("crafted{}b.hjl", i);
        craftJournal(s.journal, crafted2, [&](JournalRecord& r) {
            for (Op& op : r.tx.ops) {
                if (op.kind == OpKind::Create) op.file = hostile[i];
            }
        });
        checkRefused(*s.restart(), crafted2, "(transaction tester:");
    }
    s.checkUntouched();
}

TEST_CASE("journal: a replayed Destroy never deletes a file outside the project") {
    const Scenario s("confine_destroy", /*destroyFrigate=*/true);
    // The open record and the Destroy both name the other project's record.
    craftJournal(s.journal, s.root / "crafted.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Open && r.file == "records/hull/frigate.hrec") r.file = "../other/records/hull/frigate.hrec";
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Destroy) op.file = "../other/records/hull/frigate.hrec";
        }
    });
    checkRefused(*s.restart(), s.root / "crafted.hjl", "../other/records/hull/frigate.hrec");
    // Only the Destroy names it.
    craftJournal(s.journal, s.root / "destroy_only.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Destroy) op.file = "../other/records/hull/frigate.hrec";
        }
    });
    checkRefused(*s.restart(), s.root / "destroy_only.hjl", "../other/records/hull/frigate.hrec");
    s.checkUntouched();

    // A Destroy that names another file of the project than its document's: that document's
    // replay stops at a conflict, and a save deletes nothing.
    REQUIRE(fs::writeTextFile(s.project / "records" / "hull" / "spare.hrec", s.original));
    craftJournal(s.journal, s.root / "other_file.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Destroy) op.file = "records/hull/spare.hrec";
        }
    });
    {
        auto fw = s.restart();
        auto report = fw->recover(s.root / "other_file.hjl");
        REQUIRE_MESSAGE(report, (report ? std::string() : report.error().message));
        bool conflict = false;
        for (const RecoveredDocument& d : report->documents) {
            if (d.file == "records/hull/frigate.hrec") conflict = d.status == DocRecovery::Conflict;
        }
        CHECK(conflict);
        REQUIRE(fw->saveAll());
    }
    CHECK(fs::exists(s.project / "records" / "hull" / "spare.hrec"));
    CHECK(fs::exists(s.frigate));
    CHECK(fs::readTextFile(s.otherFrigate).value() == s.original);

    // Control: the journal as written destroys the project's own frigate.
    REQUIRE(fs::writeTextFile(s.frigate, s.original));
    REQUIRE(fs::remove(s.project / "records" / "hull" / "probe.hrec"));
    craftJournal(s.journal, s.root / "plain.hjl", [](JournalRecord&) {});
    auto fw = s.restart();
    auto report = fw->recover(s.root / "plain.hjl");
    REQUIRE_MESSAGE(report, (report ? std::string() : report.error().message));
    REQUIRE(fw->saveAll());
    CHECK_FALSE(fs::exists(s.frigate));
    CHECK(fs::readTextFile(s.otherFrigate).value() == s.original);
}

TEST_CASE("journal: recovery refuses another project's journal unless allowed") {
    const Scenario s("confine_project");
    craftJournal(s.journal, s.root / "copy.hjl", [](JournalRecord&) {});
    {
        auto fw = s.restart("renamed-project");
        auto report = fw->recover(s.root / "copy.hjl");
        REQUIRE_FALSE(report);
        CHECK(report.errorCode() == ErrorCode::InvalidArgument);
        CHECK(report.error().message.find("\"test-project\"") != std::string::npos);
        CHECK(report.error().message.find("\"renamed-project\"") != std::string::npos);
        CHECK(fw->documents().size() == 0);
    }
    s.checkUntouched();
    // The explicit opt-in replays it; the paths are still confined.
    auto fw = s.restart("renamed-project");
    auto report = fw->recover(s.root / "copy.hjl", {.allowOtherProject = true});
    REQUIRE_MESSAGE(report, (report ? std::string() : report.error().message));
    CHECK(report->replayed == 2);
    craftJournal(s.journal, s.root / "crafted.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "../outside/evil.hrec";
        }
    });
    auto fw2 = s.restart("renamed-project");
    CHECK_FALSE(fw2->recover(s.root / "crafted.hjl", {.allowOtherProject = true}));
    CHECK(fw2->documents().size() == 0);

    // A journal whose header names another project is never listed as this project's session.
    const fs::Path journals = s.root / "journals";
    craftJournal(s.journal, journalDirectory(journals, "test-project") / "mine.hjl", [](JournalRecord&) {});
    craftJournal(s.journal, journalDirectory(journals, "test-project") / "planted.hjl", [](JournalRecord&) {},
                 [](JournalHeader& h) { h.project = "someone-else"; });
    const auto sessions = listJournalSessions(journals, "test-project", false);
    REQUIRE(sessions.size() == 1);
    CHECK(sessions[0].path.filename() == "mine.hjl");
}

TEST_CASE("journal: a link out of the project is refused at recovery, open, create and save") {
    const Scenario s("confine_links");
    const fs::Path outside = s.root / "outside";
    REQUIRE(fs::createDirectories(outside));
    REQUIRE(fs::writeTextFile(outside / "frigate.hrec", s.original));
    if (!makeDirectoryLink(outside, s.project / "records" / "evil")) return;

    auto fw = s.restart();
    auto opened = fw->open("records/evil/frigate.hrec");
    REQUIRE_FALSE(opened);
    CHECK(opened.error().message.find("'records/evil' is a link to") != std::string::npos);
    CHECK(opened.error().message.find("outside the project") != std::string::npos);
    const refl::TypeInfo* type = fw->types().find("sample.ship.ShipHullDef");
    refl::RecordHeader h;
    h.rid = 11;
    h.name = "hull/evil";
    {
        auto b = fw->begin(Origin::Cli);
        CHECK_FALSE(b->createRecord(*type, "records/evil/new.hrec", h));
    }
    CHECK(fw->documents().size() == 0);

    craftJournal(s.journal, s.root / "open_link.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Open && r.file == "records/hull/frigate.hrec") r.file = "records/evil/frigate.hrec";
    });
    checkRefused(*s.restart(), s.root / "open_link.hjl", "'records/evil' is a link to");
    craftJournal(s.journal, s.root / "create_link.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "records/evil/new.hrec";
        }
    });
    checkRefused(*s.restart(), s.root / "create_link.hjl", "'records/evil' is a link to");
    CHECK_FALSE(fs::exists(outside / "new.hrec"));
    CHECK(fs::readTextFile(outside / "frigate.hrec").value() == s.original);

    // A link to a file outside, and a link that does not resolve.
    if (makeFileLink(s.otherFrigate, s.project / "records" / "hull" / "linked.hrec")) {
        CHECK(s.restart()->open("records/hull/linked.hrec").errorCode() == ErrorCode::InvalidArgument);
    }
    if (makeDirectoryLink(s.root / "nowhere", s.project / "records" / "dangling")) {
        auto r = fw->documents().confine("records/dangling/x.hrec", PathOrigin::Untrusted);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find("does not resolve") != std::string::npos);
    }
    // A link that stays inside the project is fine.
    if (makeDirectoryLink(s.project / "records" / "hull", s.project / "records" / "alias")) {
        auto fw3 = s.restart();
        auto inside = fw3->open("records/alias/frigate.hrec", type);
        CHECK_MESSAGE(inside, (inside ? std::string() : inside.error().message));
    }

    // Time of use: a link that appears after the open is refused when the document is saved.
    {
        auto fw2 = s.restart();
        auto doc = openSampleHull(*fw2);
        REQUIRE(doc);
        std::error_code ec;
        std::filesystem::rename(s.project / "records" / "hull", s.project / "records" / "hull_moved", ec);
        REQUIRE_FALSE(ec);
        REQUIRE(makeDirectoryLink(outside, s.project / "records" / "hull"));
        auto b = fw2->begin(Origin::Ui, "Set mass");
        REQUIRE(b->set(*doc, "mass", "14000"));
        REQUIRE(b->commit());
        auto saved = fw2->save(*doc);
        REQUIRE_FALSE(saved);
        CHECK(saved.error().message.find("refusing to save") != std::string::npos);
        CHECK(fs::readTextFile(outside / "frigate.hrec").value() == s.original);
        CHECK(fw2->invoker(Origin::Ui).invoke("doc.reload", R"({"doc": "hull/frigate"})").errorCode() == ErrorCode::InvalidArgument);
    }
    CHECK(fs::readTextFile(s.otherFrigate).value() == s.original);
}

} // namespace
