// helios-tool: the headless ToolsFramework CLI (07 §1.1). It links engine/toolsfw and the sample
// record types only (no EditorUI, no RHI), so it runs on build agents and in the headless preset.
// Every edit is a transaction with origin "cli", journaled like an editor session. See README.md.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "helios/core/cmdline.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/platform.h"
#include "helios/core/version.h"
#include "helios/toolsfw/toolsfw.h"
#include "helios/toolsfw/samples.h"

using namespace helios;

namespace {

constexpr std::string_view kUsage = R"(helios-tool - headless Helios ToolsFramework CLI

  helios-tool [--project-root=<dir>] [options] <verb> [arguments]

Verbs
  validate [--json]                 validate every record (exit 1 when there are errors)
  fmt [--check]                     rewrite record files in canonical JSONC (--check: exit 1 if any
                                    file is not canonical, change nothing)
  apply <doc> <path> <json>         set a property (doc = record name, path or GUID)
  apply --command=<id> [--args=<json>]
  apply --file=<commands.jsonl>     one {"command": id, "args": {...}} per line, one transaction
                                    (--no-save leaves files untouched: the edit is journal-only)
  undo [--steps=<n>]                revert the last n saved helios-tool transactions on this
                                    project (journal-only --no-save edits are not on the stack)
  redo [--steps=<n>]                re-apply the last n reverted ones
  journal list [--all]              this project's journals (unclean sessions only without --all)
  journal show <file|latest> [--json]
  journal verify <file>             record integrity (torn tail, clean end)
  journal replay <file|auto> [--save] [--ignore-source-changes] [--allow-other-project]
                                    crash recovery. A journal is untrusted input: each file
                                    it names must be a .hrec inside the project (no '..',
                                    absolute, drive, UNC or device path, no link out of it)
                                    and its header must name this project (--allow-other-
                                    project: it was renamed). One bad entry refuses the whole
                                    replay (exit 3); nothing is written
  run <script.luau>                 run an automation script (Editor.*, Record.*, Validate.*)
  commands [--json]                 list the command registry

Options
  --project-root=<dir>   project with records/<table>/*.hrec (default: current directory)
  --project=<name>       journal project name (default: the root directory's name)
  --user=<name>          transaction author (default: local)
  --journal-dir=<dir>    journal root (default: HELIOS_JOURNAL_DIR, then the user state dir)
  --no-journal           do not journal this run
  --log-level=<level>    trace|debug|info|warn|error (default warn)
  --version, --help

Exit codes: 0 ok, 1 validation or check failed, 2 usage error, 3 operation failed or conflicted.
)";

constexpr int kOk = 0;
constexpr int kCheckFailed = 1;
constexpr int kUsageError = 2;
constexpr int kFailed = 3;

int fail(int code, const std::string& message) {
    std::fprintf(stderr, "helios-tool: %s\n", message.c_str());
    return code;
}

void out(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

struct Context {
    const CommandLine* cl = nullptr;
    fs::Path root;
    std::string project;
    std::string user;
    fs::Path journalRoot;
    bool journal = true;
    std::vector<std::string> args;  ///< Positional arguments after the verb.
};

fs::Path journalRootOf(const Context& c) {
    return c.journalRoot.empty() ? tf::defaultJournalRoot() : c.journalRoot;
}

/// `lamportFloor`: the highest Lamport counter in the project's journals when the caller has
/// already read them (undo/redo); otherwise openFramework reads them once more to find it.
Result<std::unique_ptr<tf::Framework>> openFramework(const Context& c, bool journal, std::optional<u64> lamportFloor = std::nullopt) {
    tf::FrameworkConfig cfg;
    cfg.project = c.project;
    cfg.projectRoot = c.root;
    cfg.user = c.user;
    cfg.journal = journal && c.journal;
    cfg.journalRoot = c.journalRoot;
    if (cfg.journal) {
        // Each helios-tool run is its own session: continue the project's Lamport counter so
        // transaction ids (and the undo/redo targets that name them) never repeat across runs.
        // This holds for runs one after another; see the README for concurrent sessions.
        if (lamportFloor) {
            cfg.lamportFloor = *lamportFloor;
        } else {
            for (const tf::JournalSessionInfo& s : tf::listJournalSessions(journalRootOf(c), c.project, false)) {
                cfg.lamportFloor = std::max(cfg.lamportFloor, s.maxLamport);
            }
        }
    }
    HELIOS_TRY_ASSIGN(auto fw, tf::Framework::create(cfg));
    HELIOS_TRY(fw->openAll());
    return fw;
}

// ---- validate / fmt ----------------------------------------------------------------------------
int cmdValidate(const Context& c) {
    auto fw = openFramework(c, false);
    if (!fw) return fail(kFailed, std::format("{}", fw.error()));
    auto r = (*fw)->invoker(tf::Origin::Cli).invoke("validate.run");
    if (!r) return fail(kFailed, std::format("{}", r.error()));
    auto doc = refl::JsonDocument::parse(r->result);
    if (!doc) return fail(kFailed, std::format("{}", doc.error()));
    const i64 errors = tf::json::getInteger(doc->root(), "errors").value_or(0);
    if (c.cl->has("json")) {
        out(r->result + "\n");
    } else {
        for (refl::JsonValue issue : doc->root().get("issues").elements()) {
            out(std::format("{}: {}: {}: {}\n", tf::json::getString(issue, "file").value_or("?"),
                            tf::json::getString(issue, "severity").value_or("error"), tf::json::getString(issue, "path").value_or(""),
                            tf::json::getString(issue, "message").value_or("")));
        }
        out(std::format("{} document(s), {} error(s)\n", (*fw)->documents().documents().size(), errors));
    }
    return errors > 0 ? kCheckFailed : kOk;
}

int cmdFmt(const Context& c) {
    auto fw = openFramework(c, false);
    if (!fw) return fail(kFailed, std::format("{}", fw.error()));
    const bool check = c.cl->has("check");
    usize changed = 0;
    for (const tf::Document* d : (*fw)->documents().documents()) {
        // Read and write only inside the project, checked at the time of use like Framework::save.
        if (auto file = (*fw)->documents().confine(d->relativePath(), tf::PathOrigin::Untrusted); !file) {
            return fail(kFailed, std::format("{}", file.error()));
        }
        // The document's text is canonical; compare it with the bytes on disk.
        auto bytes = fs::readTextFile(d->path());
        if (!bytes) return fail(kFailed, std::format("{}", bytes.error()));
        if (*bytes == d->text()) continue;
        ++changed;
        out(std::format("{}{}\n", check ? "not canonical: " : "formatted: ", d->relativePath()));
        if (!check) {
            // Atomic like Framework::save: a crash mid-write never leaves a truncated record.
            if (auto r = fs::writeTextFile(d->path(), d->text(), fs::WriteMode::Atomic); !r) return fail(kFailed, std::format("{}", r.error()));
        }
    }
    if (check && changed > 0) return kCheckFailed;
    return kOk;
}

// ---- apply ---------------------------------------------------------------------------------------
int cmdApply(const Context& c) {
    auto created = openFramework(c, true);
    if (!created) return fail(kFailed, std::format("{}", created.error()));
    tf::Framework& fw = **created;
    tf::CommandInvoker& cli = fw.invoker(tf::Origin::Cli);
    std::vector<std::pair<std::string, std::string>> commands;  // id, args JSON
    if (auto file = c.cl->value("file")) {
        auto text = fs::readTextFile(fs::pathFromUtf8(*file));
        if (!text) return fail(kFailed, std::format("{}", text.error()));
        usize line = 0;
        std::string_view rest = *text;
        while (!rest.empty()) {
            const usize nl = rest.find('\n');
            std::string_view l = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);
            ++line;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.remove_suffix(1);
            if (l.empty() || l.starts_with("//")) continue;
            auto obj = tf::json::parseObject(l, std::format("{}:{}", *file, line));
            if (!obj) return fail(kUsageError, std::format("{}", obj.error()));
            const auto id = tf::json::getString(obj->root(), "command");
            if (!id) return fail(kUsageError, std::format("{}:{}: missing \"command\"", *file, line));
            const refl::JsonValue a = obj->root().get("args");
            commands.emplace_back(std::string(*id), a.isValid() ? tf::json::compact(a) : std::string());
        }
    } else if (auto id = c.cl->value("command")) {
        commands.emplace_back(std::string(*id), std::string(c.cl->getString("args", "")));
    } else if (c.args.size() == 3) {
        auto value = tf::json::normalize(c.args[2]);
        if (!value) return fail(kUsageError, std::format("value: {}", value.error()));
        commands.emplace_back("doc.setProperty", std::format(R"({{"doc":{},"path":{},"value":{}}})", tf::json::quote(c.args[0]),
                                                             tf::json::quote(c.args[1]), *value));
    } else {
        return fail(kUsageError, "apply: expected <doc> <path> <json>, --command=<id> or --file=<jsonl>");
    }
    if (commands.empty()) return fail(kUsageError, "apply: no commands");
    const bool group = commands.size() > 1;
    if (group) fw.beginGroup(tf::Origin::Cli, std::format("helios-tool apply ({} commands)", commands.size()));
    for (const auto& [id, args] : commands) {
        auto r = cli.invoke(id, args);
        if (!r) {
            if (group) fw.cancelGroup();
            return fail(kFailed, std::format("{}: {}", id, r.error()));
        }
        if (!r->result.empty() && r->result != "null") out(r->result + "\n");
    }
    if (group) {
        auto r = fw.endGroup();
        if (!r) return fail(kFailed, std::format("{}", r.error()));
    }
    if (fw.log().empty()) {
        out("no change\n");
        return kOk;
    }
    const tf::Transaction& tx = fw.log().back();
    out(std::format("{} {} ({} op(s))\n", tx.id.toString(), tx.label, tx.ops.size()));
    if (!c.cl->has("no-save")) {
        auto saved = fw.saveAll();
        if (!saved) return fail(kFailed, std::format("save: {}", saved.error()));
        out(std::format("saved {} file(s)\n", *saved));
    }
    return kOk;
}

// ---- undo / redo across processes ------------------------------------------------------------------
struct CliEntry {
    tf::Transaction tx;
    std::map<tf::DocId, std::string> files;  ///< The session's document files.
};

struct CliStacks {
    std::vector<CliEntry> done;
    std::vector<CliEntry> redo;
    u64 lamportFloor = 0;  ///< The highest Lamport counter in the project's journals.
};

/// Rebuilds helios-tool's linear undo stack from the project's journals (oldest first): Do
/// transactions push, Undo pops onto the redo stack, Redo moves back. Only transactions that
/// reached the files count: every document they touch must have a later "save" record in the
/// same session. A journal-only edit (`apply --no-save`, or a run whose save failed) never changed
/// the files, so its inverse could never apply and would block every older undo.
Result<CliStacks> cliStacks(const Context& c) {
    CliStacks stacks;
    std::vector<CliEntry>& done = stacks.done;
    std::vector<CliEntry>& redo = stacks.redo;
    for (const tf::JournalSessionInfo& s : tf::listJournalSessions(journalRootOf(c), c.project, false)) {
        stacks.lamportFloor = std::max(stacks.lamportFloor, s.maxLamport);
        HELIOS_TRY_ASSIGN(const tf::JournalScan scan, tf::readJournal(s.path));
        // saved[i]: record i is a transaction whose every document is saved later in the session.
        std::vector<bool> saved(scan.records.size(), false);
        std::vector<tf::DocId> savedLater;
        for (usize i = scan.records.size(); i-- > 0;) {
            const tf::JournalRecord& r = scan.records[i];
            if (r.kind == tf::JournalRecordKind::Save) {
                if (!tf::containsDoc(savedLater, r.doc)) savedLater.push_back(r.doc);
            } else if (r.kind == tf::JournalRecordKind::Tx) {
                const auto docs = r.tx.documents();
                saved[i] = std::all_of(docs.begin(), docs.end(), [&](const tf::DocId& d) { return tf::containsDoc(savedLater, d); });
            }
        }
        std::map<tf::DocId, std::string> files;
        for (usize i = 0; i < scan.records.size(); ++i) {
            const tf::JournalRecord& r = scan.records[i];
            if (r.kind == tf::JournalRecordKind::Open || r.kind == tf::JournalRecordKind::Save) {
                if (!r.file.empty()) files[r.doc] = r.file;
                continue;
            }
            if (r.kind != tf::JournalRecordKind::Tx || r.tx.origin != tf::Origin::Cli || !saved[i]) continue;
            for (const tf::Op& op : r.tx.ops) {
                if (!op.file.empty()) files[op.doc] = op.file;
            }
            switch (r.tx.kind) {
            case tf::TxKind::Do:
                done.push_back({r.tx, files});
                redo.clear();
                break;
            case tf::TxKind::Undo:
                if (done.empty() || done.back().tx.id != r.tx.target) {
                    return Error{ErrorCode::InvalidState, std::format("{}: undo of {} does not match the CLI history",
                                                                      fs::pathToUtf8(s.path.filename()), r.tx.target.toString())};
                }
                redo.push_back(std::move(done.back()));
                done.pop_back();
                break;
            case tf::TxKind::Redo:
                if (redo.empty() || redo.back().tx.id != r.tx.target) {
                    return Error{ErrorCode::InvalidState, std::format("{}: redo of {} does not match the CLI history",
                                                                      fs::pathToUtf8(s.path.filename()), r.tx.target.toString())};
                }
                done.push_back(std::move(redo.back()));
                redo.pop_back();
                break;
            }
        }
    }
    return stacks;
}

/// Applies `entry` (redo) or its inverse (undo) as one transaction and saves.
Result<tf::TxId> revert(tf::Framework& fw, const CliEntry& entry, bool undo) {
    auto b = fw.begin(tf::Origin::Cli, std::format("{} {}", undo ? "Undo" : "Redo", entry.tx.label));
    b->markRevert(undo ? tf::TxKind::Undo : tf::TxKind::Redo, entry.tx.id);
    std::map<tf::DocId, tf::DocId> remap;
    const auto local = [&](const tf::Op& op) -> Result<tf::DocId> {
        if (auto it = remap.find(op.doc); it != remap.end()) return it->second;
        const bool creates = (undo && op.kind == tf::OpKind::Destroy) || (!undo && op.kind == tf::OpKind::Create);
        if (creates) {
            remap[op.doc] = op.doc;
            return op.doc;
        }
        std::string file = op.file;
        if (file.empty()) {
            const auto f = entry.files.find(op.doc);
            if (f == entry.files.end()) return Error{ErrorCode::NotFound, "the journal does not name the document's file"};
            file = f->second;
        }
        // A journal names the file: project-relative only (Framework::open would also take an
        // absolute path under the root, which a journal never needs).
        HELIOS_TRY_ASSIGN(const tf::ProjectFile target, fw.documents().confine(file, tf::PathOrigin::Untrusted));
        HELIOS_TRY_ASSIGN(tf::Document * d, fw.open(fs::pathFromUtf8(target.relative)));
        remap[op.doc] = d->id();
        return d->id();
    };
    const auto step = [&](tf::Op op) -> Result<void> {
        HELIOS_TRY_ASSIGN(op.doc, local(op));
        return b->apply(op);
    };
    if (undo) {
        for (auto it = entry.tx.ops.rbegin(); it != entry.tx.ops.rend(); ++it) {
            if (auto r = step(it->inverse()); !r) {
                b->abort();
                return Error{r.error().code, std::format("cannot undo {} ({}): {}", entry.tx.id.toString(), entry.tx.label, r.error().message)};
            }
        }
    } else {
        for (const tf::Op& op : entry.tx.ops) {
            if (auto r = step(op); !r) {
                b->abort();
                return Error{r.error().code, std::format("cannot redo {} ({}): {}", entry.tx.id.toString(), entry.tx.label, r.error().message)};
            }
        }
    }
    HELIOS_TRY_ASSIGN(const tf::TxId id, b->commit());
    HELIOS_TRY(fw.saveAll());
    return id;
}

int cmdUndoRedo(const Context& c, bool undo) {
    if (!c.journal) return fail(kUsageError, "undo/redo read and write the journal (drop --no-journal)");
    const i64 steps = c.cl->getInt("steps", 1);
    if (steps < 1) return fail(kUsageError, "--steps must be at least 1");
    auto stacks = cliStacks(c);
    if (!stacks) return fail(kFailed, std::format("{}", stacks.error()));
    std::vector<CliEntry>& done = stacks->done;
    std::vector<CliEntry>& redo = stacks->redo;
    auto created = openFramework(c, true, stacks->lamportFloor);
    if (!created) return fail(kFailed, std::format("{}", created.error()));
    tf::Framework& fw = **created;
    for (i64 i = 0; i < steps; ++i) {
        std::vector<CliEntry>& from = undo ? done : redo;
        if (from.empty()) {
            if (i == 0) return fail(kFailed, undo ? "nothing to undo" : "nothing to redo");
            break;
        }
        const CliEntry entry = std::move(from.back());
        from.pop_back();
        auto r = revert(fw, entry, undo);
        if (!r) return fail(kFailed, std::format("{}", r.error()));
        out(std::format("{} {} -> {}\n", undo ? "undid" : "redid", entry.tx.id.toString(), r->toString()));
    }
    return kOk;
}

// ---- journal ---------------------------------------------------------------------------------------
Result<fs::Path> resolveJournal(const Context& c, std::string_view which, bool uncleanOnly) {
    if (which != "latest" && which != "auto") return fs::pathFromUtf8(which);
    const auto sessions = tf::listJournalSessions(journalRootOf(c), c.project, uncleanOnly);
    if (sessions.empty()) return Error{ErrorCode::NotFound, std::format("no {}journal for project '{}'", uncleanOnly ? "unclean " : "", c.project)};
    return sessions.back().path;
}

int cmdJournal(const Context& c) {
    if (c.args.empty()) return fail(kUsageError, "journal: expected list, show, verify or replay");
    const std::string& sub = c.args[0];
    if (sub == "list") {
        const auto sessions = tf::listJournalSessions(journalRootOf(c), c.project, !c.cl->has("all"));
        for (const tf::JournalSessionInfo& s : sessions) {
            out(std::format("{}  session={} user={} host={} pid={} tx={} {}{}\n", fs::pathToUtf8(s.path), tf::printable(s.header.session),
                            tf::printable(s.header.user), tf::printable(s.header.host), s.header.pid, s.txCount, s.clean ? "clean" : "UNCLEAN",
                            s.tornBytes ? std::format(" torn={}B", s.tornBytes) : std::string()));
        }
        if (sessions.empty()) out(std::format("no {}journals for project '{}'\n", c.cl->has("all") ? "" : "unclean ", c.project));
        return kOk;
    }
    if (c.args.size() < 2) return fail(kUsageError, std::format("journal {}: expected a journal file", sub));
    auto path = resolveJournal(c, c.args[1], sub == "replay");
    if (!path) return fail(kFailed, std::format("{}", path.error()));
    if (sub == "show" || sub == "verify") {
        auto scan = tf::readJournal(*path);
        if (!scan) return fail(kFailed, std::format("{}", scan.error()));
        if (sub == "verify") {
            out(std::format("{}: {} record(s), {} valid byte(s), {} torn byte(s), {}\n", fs::pathToUtf8(*path), scan->records.size(),
                            scan->validBytes, scan->tornBytes, scan->clean ? "clean end" : "no end record (crashed or running)"));
            return scan->tornBytes == 0 ? kOk : kCheckFailed;
        }
        const bool json = c.cl->has("json");
        out(std::format("# project={} session={} user={} host={} pid={}\n", tf::printable(scan->header.project),
                        tf::printable(scan->header.session), tf::printable(scan->header.user), tf::printable(scan->header.host), scan->header.pid));
        for (const tf::JournalRecord& r : scan->records) {
            if (json) {
                out(r.toJson() + "\n");
            } else if (r.kind == tf::JournalRecordKind::Tx) {
                out(std::format("@{} tx {} {} {} \"{}\" ({} op(s))\n", r.offset, tf::printable(r.tx.id.toString()), tf::txKindName(r.tx.kind),
                                tf::originName(r.tx.origin), tf::printable(r.tx.label), r.tx.ops.size()));
            } else {
                out(std::format("@{} {} {}\n", r.offset, tf::journalRecordKindName(r.kind), tf::printable(r.file)));
            }
        }
        return kOk;
    }
    if (sub == "replay") {
        auto created = openFramework(c, true);
        if (!created) return fail(kFailed, std::format("{}", created.error()));
        tf::RecoveryOptions options;
        options.ignoreSourceChanges = c.cl->has("ignore-source-changes");
        options.allowOtherProject = c.cl->has("allow-other-project");
        auto report = (*created)->recover(*path, options);
        if (!report) return fail(kFailed, std::format("{}", report.error()));
        out(std::format("replayed {} transaction(s), skipped {}{}\n", report->replayed, report->skipped,
                        report->clean ? " (the session had ended cleanly)" : ""));
        bool conflicts = false;
        for (const tf::RecoveredDocument& d : report->documents) {
            out(std::format("  {}: {} {}\n", d.file, tf::docRecoveryName(d.status), d.message));
            conflicts = conflicts || d.status == tf::DocRecovery::Conflict || d.status == tf::DocRecovery::SourceChanged;
        }
        if (c.cl->has("save")) {
            auto saved = (*created)->saveAll();
            if (!saved) return fail(kFailed, std::format("save: {}", saved.error()));
            out(std::format("saved {} file(s)\n", *saved));
        }
        return conflicts ? kFailed : kOk;
    }
    return fail(kUsageError, std::format("journal: unknown subcommand '{}'", sub));
}

// ---- run / commands ------------------------------------------------------------------------------------
int cmdRun(const Context& c) {
    if (c.args.empty()) return fail(kUsageError, "run: expected a script file");
    const fs::Path file = fs::pathFromUtf8(c.args[0]);
    auto source = fs::readTextFile(file);
    if (!source) return fail(kFailed, std::format("{}", source.error()));
    auto created = openFramework(c, true);
    if (!created) return fail(kFailed, std::format("{}", created.error()));
    auto automation = tf::Automation::create(**created);
    if (!automation) return fail(kFailed, std::format("{}", automation.error()));
    auto r = (*automation)->run(fs::pathToUtf8(file.filename()), *source);
    if (!r) return fail(kFailed, std::format("{}", r.error()));
    for (const std::string& line : r->log) out(line + "\n");
    if (!c.cl->has("no-save")) {
        auto saved = (*created)->saveAll();
        if (!saved) return fail(kFailed, std::format("save: {}", saved.error()));
        if (*saved) out(std::format("saved {} file(s)\n", *saved));
    }
    return kOk;
}

int cmdCommands(const Context& c) {
    auto fw = openFramework(c, false);
    if (!fw) return fail(kFailed, std::format("{}", fw.error()));
    const bool json = c.cl->has("json");
    if (json) out("[");
    bool first = true;
    for (const tf::CommandDesc* d : (*fw)->commands().commands()) {
        if (json) {
            out(std::format(R"({}{{"id":{},"label":{},"shortcut":{},"headless":{}}})", first ? "" : ",", tf::json::quote(d->id),
                            tf::json::quote(d->label), tf::json::quote(d->shortcut), d->headless ? "true" : "false"));
        } else {
            out(std::format("{:<24} {:<16} {}\n", d->id, d->shortcut, d->doc));
        }
        first = false;
    }
    if (json) out("]\n");
    return kOk;
}

int run(const CommandLine& cl) {
    if (cl.has("version")) {
        std::printf("helios-tool %s\n", version::kString);
        return kOk;
    }
    if (cl.has("help") || cl.has("h")) {
        std::fputs(kUsage.data(), stdout);
        return kOk;
    }
    if (cl.positional().empty()) {
        std::fputs(kUsage.data(), stderr);
        return kUsageError;
    }
    log::setLevel(log::Level::Warn);
    if (auto lvl = cl.value("log-level")) {
        if (auto l = log::parseLevel(*lvl)) log::setLevel(*l);
    }
    if (auto r = tf::samples::registerSampleTypes(); !r) return fail(kFailed, std::format("sample types: {}", r.error()));

    Context c;
    c.cl = &cl;
    c.root = cl.value("project-root") ? fs::pathFromUtf8(*cl.value("project-root")) : std::filesystem::current_path();
    c.project = std::string(cl.getString("project", ""));
    if (c.project.empty()) {
        const fs::Path norm = std::filesystem::absolute(c.root).lexically_normal();
        c.project = fs::pathToUtf8(norm.filename().empty() ? norm.parent_path().filename() : norm.filename());
        if (c.project.empty()) c.project = "project";
    }
    c.user = std::string(cl.getString("user", "local"));
    if (auto dir = cl.value("journal-dir")) c.journalRoot = fs::pathFromUtf8(*dir);
    c.journal = !cl.has("no-journal");
    const std::string verb = cl.positional()[0];
    c.args.assign(cl.positional().begin() + 1, cl.positional().end());

    if (verb == "validate") return cmdValidate(c);
    if (verb == "fmt") return cmdFmt(c);
    if (verb == "apply") return cmdApply(c);
    if (verb == "undo") return cmdUndoRedo(c, true);
    if (verb == "redo") return cmdUndoRedo(c, false);
    if (verb == "journal") return cmdJournal(c);
    if (verb == "run") return cmdRun(c);
    if (verb == "commands") return cmdCommands(c);
    return fail(kUsageError, std::format("unknown verb '{}' (--help lists them)", verb));
}

} // namespace

int main(int argc, char** argv) {
    const CommandLine cl = platform::kIsWindows ? CommandLine::fromProcess() : CommandLine::parse(argc, argv);
    return run(cl);
}
