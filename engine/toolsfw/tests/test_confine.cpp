// The journal is untrusted input: every path it names stays inside the project (Workspace::confine),
// and a journal of another project is refused. These are the regression cases of the round-5
// review of PR #40 (a crafted journal made `helios-tool journal replay --save` write, rewrite and
// delete files outside the project) plus the spelling and link rules behind them, and the output
// rule: no string from a journal reaches an error or a report with its control characters.

#include <doctest/doctest.h>

#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/core/process.h"
#include "helios/core/utf.h"
#include "platform/tf_os.h"
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

/// True if a terminal could take part of `text` as a control: a C0 or C1 control, DEL, or bytes
/// that are not UTF-8 (a lone 0x9B is the 8-bit CSI).
bool hasControl(std::string_view text) {
    for (usize i = 0; i < text.size();) {
        const usize start = i;
        const char32_t cp = decodeUtf8(text, i);
        if (cp == kReplacementChar && text.substr(start, i - start) != "\xEF\xBF\xBD") return true;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
    }
    return false;
}

/// A transaction author that sets the window title (OSC ... BEL) and the colour (CSI), then DEL
/// and the C1 CSI (U+009B). A journal's strings are well-formed UTF-8 (yyjson refuses a record
/// that is not), so a lone 0x9B byte is tested on printable() alone.
const std::string kEvil = "\x1b]0;pwned\x07\x1b[31m\x7f\xC2\x9B" "1m";
/// kEvil through printable().
constexpr std::string_view kEvilShown = R"(\x1b]0;pwned\x07\x1b[31m\x7f\u009b1m)";

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
             {"records/hull/Frigate.HREC", "records/hull/Frigate.HREC"},  // as openAll() lists them
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
             ".hrec", ".HREC", "records/x.hrec/", "records/x.hrecx", "records/x.hre", "records/xhrec",
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

    // A Destroy that names another file of the project than its document's. A legitimate journal
    // never holds one (TxBuilder::destroy names the document's own file), so the whole journal is
    // refused: before, only the frigate's replay stopped at a conflict, and the probe's Create
    // still replayed and was saved.
    REQUIRE(fs::writeTextFile(s.project / "records" / "hull" / "spare.hrec", s.original));
    craftJournal(s.journal, s.root / "other_file.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Destroy) op.file = "records/hull/spare.hrec";
        }
    });
    checkRefused(*s.restart(), s.root / "other_file.hjl", "the destroy op names 'records/hull/spare.hrec', but document");
    // The same for a Create that names a known document with another file (an undone Destroy
    // restores the document at its own file).
    DocId frigate;
    craftJournal(s.journal, s.root / "create_known.hjl", [&frigate](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Open && r.file == "records/hull/frigate.hrec") frigate = r.doc;
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.doc = frigate;
        }
    });
    checkRefused(*s.restart(), s.root / "create_known.hjl", "the create op names 'records/hull/probe.hrec', but document");
    CHECK(fs::exists(s.project / "records" / "hull" / "spare.hrec"));
    s.checkUntouched();

    // Control: the journal as written destroys the project's own frigate.
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

TEST_CASE("printable: escapes every control character a terminal could act on") {
    CHECK(printable("records/hull/frigate.hrec") == "records/hull/frigate.hrec");
    CHECK(printable("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x80 \xEF\xBF\xBD") == "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x9A\x80 \xEF\xBF\xBD");
    CHECK(printable(std::string("a\0b\tc\nd", 7)) == R"(a\x00b\x09c\x0ad)");
    CHECK(printable(kEvil) == kEvilShown);
    CHECK(printable("\xC2\x80\xC2\x9F\xC2\xA0") == "\\u0080\\u009f\xC2\xA0");  // C1 escaped, NBSP kept
    // Not UTF-8: a truncated sequence, an overlong '/', a UTF-16 surrogate, a stray continuation byte.
    CHECK(printable("\xE2\x82") == R"(\xe2\x82)");
    CHECK(printable("\xC0\xAF") == R"(\xc0\xaf)");
    CHECK(printable("\xED\xA0\x80") == R"(\xed\xa0\x80)");
    CHECK(printable("\x85x") == R"(\x85x)");
    CHECK(printable("\x9B" "2m") == R"(\x9b2m)");  // the 8-bit CSI
    CHECK(printable("abcdef", 3) == "abc...");
    for (const std::string& text : {kEvil, std::string("\xE2\x82\xC0\xAF\xFF\x80"), std::string(kEvilShown)}) {
        INFO(printable(text));
        CHECK_FALSE(hasControl(printable(text)));
        CHECK(printable(printable(text)) == printable(text));
    }
}

TEST_CASE("journal: a refusal and the replay report escape the journal's control characters") {
    // Round-1 review of PR #50: the refusal named a transaction by its raw id, json::quote let DEL
    // and C1 through in the project name, and the report quoted the journal's strings as they were.
    const Scenario s("confine_escapes");
    craftJournal(s.journal, s.root / "esc.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Tx) r.tx.id.user = kEvil;
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "../outside/evil.hrec";
        }
    });
    {
        auto refused = s.restart()->recover(s.root / "esc.hjl");
        REQUIRE_FALSE(refused);
        INFO(printable(refused.error().message));
        CHECK_FALSE(hasControl(refused.error().message));
        CHECK(refused.error().message.find(std::format("(transaction {}:", kEvilShown)) != std::string::npos);
    }
    s.checkUntouched();

    // The header's project, in the mismatch refusal and in listJournalSessions' warning.
    craftJournal(s.journal, s.root / "esc_project.hjl", [](JournalRecord&) {}, [](JournalHeader& h) { h.project = kEvil; });
    {
        auto refused = s.restart()->recover(s.root / "esc_project.hjl");
        REQUIRE_FALSE(refused);
        INFO(printable(refused.error().message));
        CHECK_FALSE(hasControl(refused.error().message));
        CHECK(refused.error().message.find(std::format("belongs to project \"{}\"", kEvilShown)) != std::string::npos);
    }
    const fs::Path journals = s.root / "journals";
    craftJournal(s.journal, journalDirectory(journals, "test-project") / "planted.hjl", [](JournalRecord&) {},
                 [](JournalHeader& h) { h.project = kEvil; });
    {
        std::mutex mutex;
        std::vector<std::string> warnings;
        // Only this sink, at Warn, for the call (test_main.cpp keeps the log at Error).
        const std::vector<std::shared_ptr<log::Sink>> savedSinks = log::sinks();
        const log::Level savedLevel = log::level();
        log::clearSinks();
        log::addSink(std::make_shared<log::CallbackSink>([&](const log::Record& r) {
            const std::lock_guard lock(mutex);
            warnings.emplace_back(r.message);
        }));
        log::setLevel(log::Level::Warn);
        const auto sessions = listJournalSessions(journals, "test-project", false);
        log::setLevel(savedLevel);
        log::clearSinks();
        for (const auto& saved : savedSinks) log::addSink(saved);
        CHECK(sessions.empty());
        const std::lock_guard lock(mutex);
        bool warned = false;
        for (const std::string& w : warnings) {
            INFO(printable(w));
            CHECK_FALSE(hasControl(w));
            warned = warned || w.find(std::format("belongs to project \"{}\"", kEvilShown)) != std::string::npos;
        }
        CHECK(warned);
    }

    // The report: the transaction's user and an op's property path, both from the journal.
    craftJournal(s.journal, s.root / "esc_path.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Tx) r.tx.id.user = kEvil;
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Set) op.path = "\x1b[31mmass";
        }
    });
    auto report = s.restart()->recover(s.root / "esc_path.hjl");
    REQUIRE_MESSAGE(report, (report ? std::string() : printable(report.error().message)));
    bool conflict = false;
    for (const RecoveredDocument& d : report->documents) {
        INFO(printable(d.message));
        CHECK_FALSE(hasControl(d.message));
        if (d.status != DocRecovery::Conflict) continue;
        conflict = true;
        CHECK(d.message.find(std::format("transaction {}:", kEvilShown)) != std::string::npos);
        CHECK(d.message.find(R"(\x1b[31mmass)") != std::string::npos);
    }
    CHECK(conflict);
}

TEST_CASE("journal: a FIFO in the project is refused at recovery and open, without blocking") {
    const Scenario s("confine_fifo");
    // A named pipe, made with the mkfifo tool rather than a POSIX call; Windows has no FIFO files.
    const fs::Path fifo = s.project / "records" / "hull" / "fifo.hrec";
    ProcessDesc mkfifo;
    mkfifo.executable = "mkfifo";
    mkfifo.searchPath = true;
    mkfifo.args = {fs::pathToUtf8(fifo)};
    mkfifo.stdinMode = StdioMode::Null;
    mkfifo.stdoutMode = StdioMode::Null;
    mkfifo.stderrMode = StdioMode::Null;
    auto made = runProcess(mkfifo);
    if (!made || made->exitCode != 0 || !fs::exists(fifo)) {
        MESSAGE("skipped: cannot create a FIFO here (no mkfifo)");
        return;
    }
    auto kind = os::entryKind(fifo);
    REQUIRE(kind);
    CHECK(*kind == os::EntryKind::Other);

    // Reading a FIFO blocks until a writer opens it: each of these would hang if it got that far.
    auto fw = s.restart();
    auto confined = fw->documents().confine("records/hull/fifo.hrec", PathOrigin::Untrusted);
    REQUIRE_FALSE(confined);
    CHECK(confined.error().message.find("'records/hull/fifo.hrec' is not a regular file or directory") != std::string::npos);
    const refl::TypeInfo* type = fw->types().find("sample.ship.ShipHullDef");
    CHECK(fw->open("records/hull/fifo.hrec", type).errorCode() == ErrorCode::InvalidArgument);
    if (makeFileLink(fifo, s.project / "records" / "hull" / "fifo_link.hrec")) {
        auto linked = fw->open("records/hull/fifo_link.hrec", type);
        REQUIRE_FALSE(linked);
        CHECK(linked.error().message.find("'records/hull/fifo_link.hrec' is not a regular file or directory") != std::string::npos);
    }
    CHECK(fw->documents().size() == 0);

    craftJournal(s.journal, s.root / "open_fifo.hjl", [](JournalRecord& r) {
        if (r.kind == JournalRecordKind::Open && r.file == "records/hull/frigate.hrec") r.file = "records/hull/fifo.hrec";
    });
    checkRefused(*s.restart(), s.root / "open_fifo.hjl", "'records/hull/fifo.hrec' is not a regular file or directory");
    craftJournal(s.journal, s.root / "create_fifo.hjl", [](JournalRecord& r) {
        for (Op& op : r.tx.ops) {
            if (op.kind == OpKind::Create) op.file = "records/hull/fifo.hrec";
        }
    });
    checkRefused(*s.restart(), s.root / "create_fifo.hjl", "'records/hull/fifo.hrec' is not a regular file or directory");
    s.checkUntouched();
}

} // namespace
