// Crash recovery: replaying an unclean journal (07 §1.2, AAA-STB-2).
//
// Per document the session's last "open" or "save" record is the replay base: the file on disk
// must still hash to what that record says (the session wrote or read exactly those bytes), and
// only the transactions after it are re-applied. A reload of an external edit journals a new
// "open"; when unsaved local edits survived its merge, that record carries the merged text as a
// snapshot, which is restored (one transaction) before the later transactions. A document created
// in the session and never saved comes back from the snapshot in its Create op. Each journaled transaction (do, undo and
// redo alike) is replayed as one new transaction with its original ops, label, origin and merge
// key, so the recovered edits are undoable and journaled again in the new session.
//
// The journal is untrusted input (a file the user was handed, or one planted in the journal
// directory): every path in it goes through Workspace::confine before anything is read or
// applied, and a journal of another project is refused. One bad entry refuses the whole replay.
// Every string from the journal that reaches an error or a report message goes through
// printable(), so a crafted journal cannot write terminal escape sequences through either.

#include <algorithm>
#include <format>
#include <map>
#include <optional>

#include "helios/core/hash.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"

#include "doc_access.h"
#include "framework_internal.h"

namespace helios::tf {

std::string_view docRecoveryName(DocRecovery status) noexcept {
    switch (status) {
    case DocRecovery::Replayed: return "replayed";
    case DocRecovery::UpToDate: return "up-to-date";
    case DocRecovery::Created: return "created";
    case DocRecovery::SourceChanged: return "source-changed";
    case DocRecovery::Missing: return "missing";
    case DocRecovery::Conflict: return "conflict";
    }
    return "unknown";
}

namespace {

struct DocState {
    ProjectFile file;      ///< The journaled file, through Workspace::confine.
    std::string typeName;
    u64 hash = 0;          ///< Hash the file must have (last open/save).
    usize baseRecord = 0;  ///< Index of that record.
    std::optional<std::string> snapshot;  ///< The base's text when it differs from the file (merged reload).
    bool closed = false;
    bool active = false;   ///< Replay enabled.
    usize report = 0;      ///< Index into RecoveryReport::documents.
    DocId localId;         ///< Id in this framework (differs when the file was already open).
};

/// "record 3 (open of <doc>) at offset 812" for refusals. A transaction id's user is any string
/// the journal holds.
std::string describe(const JournalRecord& r, usize index) {
    if (r.kind == JournalRecordKind::Tx) {
        return std::format("record {} (transaction {}) at offset {}", index, printable(r.tx.id.toString(), 200), r.offset);
    }
    return std::format("record {} ({} of document {}) at offset {}", index, journalRecordKindName(r.kind), r.doc, r.offset);
}

} // namespace

Result<void> detail::FwAccess::restoreSnapshot(Framework& fw, Document& d, std::string_view text) {
    const refl::TypeInfo& type = d.type();
    refl::Value target(type);
    refl::RecordHeader header;
    refl::ReadCtx ctx;
    HELIOS_TRY(refl::readRecord(type, target.data(), text, header, ctx));
    if (header.rid != d.header().rid) return Error{ErrorCode::InvalidState, "$rid differs from the file's"};
    auto b = fw.begin(Origin::Import, "Recover unsaved edits: " + d.name());
    b->m_replay = true;
    HELIOS_TRY(b->set(d.id(), "$name", json::quote(header.name)));
    HELIOS_TRY(b->set(d.id(), "$parent", json::quote(header.parent)));
    HELIOS_TRY(b->set(d.id(), "$comment", json::quote(header.comment)));
    HELIOS_TRY(applyDiff(*b, d, target.data()));
    if (d.text() != text) return Error{ErrorCode::InvalidArgument, "the snapshot is not canonical"};
    HELIOS_TRY(b->commit());
    return {};
}

Result<RecoveryReport> Framework::recover(const fs::Path& journalFile, const RecoveryOptions& options) {
    if (m_group) return Error{ErrorCode::InvalidState, "cannot recover inside a transaction group"};
    const std::string journalName = printable(fs::pathToGenericUtf8(journalFile));
    auto scanned = readJournal(journalFile);
    if (!scanned) return Error{scanned.error().code, printable(scanned.error().message)};
    const JournalScan scan = std::move(*scanned);
    if (scan.header.project != m_config.project && !options.allowOtherProject) {
        // json::quote escapes only C0, so the names go through printable() instead.
        return Error{ErrorCode::InvalidArgument,
                     std::format("{}: the journal belongs to project \"{}\", not \"{}\"; nothing was replayed (a renamed "
                                 "project needs RecoveryOptions::allowOtherProject, helios-tool --allow-other-project)",
                                 journalName, printable(scan.header.project, 200), printable(m_config.project, 200))};
    }
    RecoveryReport report;
    report.tornBytes = scan.tornBytes;
    report.clean = scan.clean;

    // Pass 1: the replay base of every document, and the project-confinement rule on every file
    // the journal names. It reads no file's contents and changes nothing, so a refusal leaves no
    // trace.
    const auto confined = [&](const std::string& file, const JournalRecord& r, usize index) -> Result<ProjectFile> {
        auto checked = m_workspace->confine(file, PathOrigin::Untrusted, PathCheck::OnDisk);
        if (!checked) {
            return Error{checked.error().code, std::format("{}: {}: {}; nothing was replayed", journalName, describe(r, index),
                                                           printable(checked.error().message))};
        }
        return checked;
    };
    std::map<DocId, DocState> docs;
    u64 maxLamport = 0;
    for (usize i = 0; i < scan.records.size(); ++i) {
        const JournalRecord& r = scan.records[i];
        switch (r.kind) {
        case JournalRecordKind::Open: {
            HELIOS_TRY_ASSIGN(ProjectFile file, confined(r.file, r, i));
            DocState& s = docs[r.doc];
            s.file = std::move(file);
            s.typeName = r.typeName;
            s.hash = r.hash;
            s.baseRecord = i;
            s.snapshot = r.snapshot;
            s.closed = false;
            break;
        }
        case JournalRecordKind::Save: {
            HELIOS_TRY_ASSIGN(ProjectFile file, confined(r.file, r, i));
            DocState& s = docs[r.doc];
            s.file = std::move(file);
            s.hash = r.hash;
            s.baseRecord = i;
            s.snapshot.reset();
            break;
        }
        case JournalRecordKind::Close: docs[r.doc].closed = true; break;
        case JournalRecordKind::Tx:
            maxLamport = std::max(maxLamport, r.tx.id.lamport);
            for (const Op& op : r.tx.ops) {
                if (op.kind != OpKind::Create && op.kind != OpKind::Destroy) continue;
                HELIOS_TRY_ASSIGN(ProjectFile file, confined(op.file, r, i));
                const auto known = docs.find(op.doc);
                if (known == docs.end()) {
                    if (op.kind == OpKind::Create) {
                        // Created in the session: no file yet; the Create op itself is the base.
                        DocState& s = docs[op.doc];
                        s.file = std::move(file);
                        s.typeName = op.typeName;
                        s.hash = 0;
                        s.baseRecord = ~usize{0};  // replay from the start: the Create op recreates it
                    }
                    continue;
                }
                // A document keeps its file: TxBuilder::destroy names the document's own file, and
                // undoing it restores the document there. Another file is a crafted entry, and the
                // whole journal is refused (applyOp would refuse only this document's replay, and
                // the others would still replay and be saved).
                const std::string& own = known->second.file.relative;
                if (!own.empty() && !sameRelativePath(file.relative, own)) {
                    return Error{ErrorCode::InvalidArgument,
                                 std::format("{}: {}: the {} op names '{}', but document {} is '{}'; nothing was replayed",
                                             journalName, describe(r, i), opKindName(op.kind), file.relative, op.doc, own)};
                }
            }
            break;
        case JournalRecordKind::End: break;
        }
    }

    // Pass 2: bring each document to its base.
    for (auto& [id, s] : docs) {
        RecoveredDocument rd;
        rd.doc = id;
        rd.file = s.file.relative;
        s.report = report.documents.size();
        s.localId = id;
        if (s.closed) {
            rd.status = DocRecovery::UpToDate;
            rd.message = "closed in the session";
            report.documents.push_back(std::move(rd));
            continue;
        }
        if (s.baseRecord == ~usize{0}) {
            // Created in the session and never saved: the replay recreates it. Its file must not
            // exist (else the create would refuse to clobber it).
            s.active = true;
            rd.status = DocRecovery::Created;
            report.documents.push_back(std::move(rd));
            continue;
        }
        const fs::Path& abs = s.file.absolute;
        if (s.hash == 0) {
            // Saved as destroyed: the file was deleted; nothing to base a replay on.
            rd.status = fs::exists(abs) ? DocRecovery::SourceChanged : DocRecovery::UpToDate;
            report.documents.push_back(std::move(rd));
            continue;
        }
        auto bytes = fs::readTextFile(abs);
        if (!bytes) {
            rd.status = DocRecovery::Missing;
            rd.message = bytes.error().toString();
            report.documents.push_back(std::move(rd));
            continue;
        }
        const bool sourceChanged = hash64(*bytes) != s.hash;
        if (sourceChanged && (!options.ignoreSourceChanges || s.snapshot)) {
            // A snapshot is a whole-document state with no per-op preconditions: restoring it over
            // a changed file would silently revert that change, so it is never forced.
            rd.status = DocRecovery::SourceChanged;
            rd.message = "the file changed after the session last read or saved it";
            report.documents.push_back(std::move(rd));
            continue;
        }
        Document* open = m_workspace->findByPath(abs);
        if (!open) {
            const refl::TypeInfo* type = s.typeName.empty() ? nullptr : m_workspace->types().find(s.typeName);
            auto opened = detail::FwAccess::openDocument(*this, s.file, type, true, id);
            if (!opened) {
                rd.status = DocRecovery::Missing;
                rd.message = opened.error().toString();
                report.documents.push_back(std::move(rd));
                continue;
            }
            open = *opened;
        } else if (open->dirty()) {
            rd.status = DocRecovery::Conflict;
            rd.message = "the document is open with unsaved changes";
            report.documents.push_back(std::move(rd));
            continue;
        }
        s.localId = open->id();
        if (s.snapshot) {
            if (auto restored = detail::FwAccess::restoreSnapshot(*this, *open, *s.snapshot); !restored) {
                rd.status = DocRecovery::Conflict;
                rd.message = "unsaved edits merged at a reload: " + restored.error().message;
                report.documents.push_back(std::move(rd));
                continue;
            }
            ++report.replayed;
            ++rd.transactions;
        }
        s.active = true;
        rd.status = s.snapshot ? DocRecovery::Replayed : DocRecovery::UpToDate;
        report.documents.push_back(std::move(rd));
    }

    // Pass 3: replay.
    for (usize i = 0; i < scan.records.size(); ++i) {
        const JournalRecord& r = scan.records[i];
        if (r.kind != JournalRecordKind::Tx) continue;
        auto b = begin(r.tx.origin, r.tx.label);
        detail::FwAccess::setReplay(*b, true);
        b->setMergeKey(r.tx.mergeKey);
        // Keep what the transaction was (an undo stays an undo of its target in the new journal).
        if (r.tx.kind != TxKind::Do) b->markRevert(r.tx.kind, r.tx.target);
        bool any = false;
        bool failed = false;
        std::vector<DocId> touched;
        for (const Op& op : r.tx.ops) {
            auto it = docs.find(op.doc);
            if (it == docs.end() || !it->second.active) continue;
            DocState& s = it->second;
            if (s.baseRecord != ~usize{0} && i < s.baseRecord) continue;  // already on disk
            Op local = op;
            local.doc = s.localId;
            if (!containsDoc(touched, op.doc)) touched.push_back(op.doc);
            if (auto res = b->apply(local); !res) {
                failed = true;
                for (const DocId& d : touched) {
                    DocState& ds = docs[d];
                    ds.active = false;
                    RecoveredDocument& rd = report.documents[ds.report];
                    rd.status = DocRecovery::Conflict;
                    rd.message = std::format("transaction {}: {}", r.tx.id.toString(), res.error().message);
                }
                break;
            }
            any = true;
        }
        if (failed) {
            b->abort();
            ++report.skipped;
            continue;
        }
        if (!any) {
            b->abort();
            ++report.skipped;
            continue;
        }
        HELIOS_TRY(b->commit());
        ++report.replayed;
        for (const DocId& d : touched) {
            RecoveredDocument& rd = report.documents[docs[d].report];
            ++rd.transactions;
            if (rd.status == DocRecovery::UpToDate) rd.status = DocRecovery::Replayed;
        }
    }
    detail::FwAccess::setLamportAtLeast(*this, maxLamport);
    // The messages quote the journal (a transaction's user, an op's property path or type name)
    // and the record files: escaped once here for every caller (helios-tool, the editor's log).
    for (RecoveredDocument& d : report.documents) d.message = printable(d.message);
    return report;
}

} // namespace helios::tf
