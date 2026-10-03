// Crash recovery: replaying an unclean journal (07 §1.2, AAA-STB-2).
//
// Per document the session's last "open" or "save" record is the replay base: the file on disk
// must still hash to what that record says (the session wrote or read exactly those bytes), and
// only the transactions after it are re-applied. A document created in the session and never
// saved comes back from the snapshot in its Create op. Each journaled transaction (do, undo and
// redo alike) is replayed as one new transaction with its original ops, label, origin and merge
// key, so the recovered edits are undoable and journaled again in the new session.

#include <algorithm>
#include <format>
#include <map>

#include "helios/core/hash.h"
#include "helios/toolsfw/framework.h"

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
    std::string file;
    std::string typeName;
    u64 hash = 0;          ///< Hash the file must have (last open/save).
    usize baseRecord = 0;  ///< Index of that record.
    bool closed = false;
    bool active = false;   ///< Replay enabled.
    usize report = 0;      ///< Index into RecoveryReport::documents.
    DocId localId;         ///< Id in this framework (differs when the file was already open).
};

} // namespace

Result<RecoveryReport> Framework::recover(const fs::Path& journalFile, const RecoveryOptions& options) {
    if (m_group) return Error{ErrorCode::InvalidState, "cannot recover inside a transaction group"};
    HELIOS_TRY_ASSIGN(const JournalScan scan, readJournal(journalFile));
    RecoveryReport report;
    report.tornBytes = scan.tornBytes;
    report.clean = scan.clean;

    // Pass 1: the replay base of every document.
    std::map<DocId, DocState> docs;
    u64 maxLamport = 0;
    for (usize i = 0; i < scan.records.size(); ++i) {
        const JournalRecord& r = scan.records[i];
        switch (r.kind) {
        case JournalRecordKind::Open: {
            DocState& s = docs[r.doc];
            s.file = r.file;
            s.typeName = r.typeName;
            s.hash = r.hash;
            s.baseRecord = i;
            s.closed = false;
            break;
        }
        case JournalRecordKind::Save: {
            DocState& s = docs[r.doc];
            s.file = r.file;
            s.hash = r.hash;
            s.baseRecord = i;
            break;
        }
        case JournalRecordKind::Close: docs[r.doc].closed = true; break;
        case JournalRecordKind::Tx:
            maxLamport = std::max(maxLamport, r.tx.id.lamport);
            for (const Op& op : r.tx.ops) {
                if (op.kind == OpKind::Create && !docs.contains(op.doc)) {
                    // Created in the session: no file yet; the Create op itself is the base.
                    DocState& s = docs[op.doc];
                    s.file = op.file;
                    s.typeName = op.typeName;
                    s.hash = 0;
                    s.baseRecord = ~usize{0};  // replay from the start: the Create op recreates it
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
        rd.file = s.file;
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
        const fs::Path abs = m_workspace->absolute(s.file);
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
        if (hash64(*bytes) != s.hash && !options.ignoreSourceChanges) {
            rd.status = DocRecovery::SourceChanged;
            rd.message = "the file changed after the session last read or saved it";
            report.documents.push_back(std::move(rd));
            continue;
        }
        Document* open = m_workspace->findByPath(abs);
        if (!open) {
            const refl::TypeInfo* type = s.typeName.empty() ? nullptr : m_workspace->types().find(s.typeName);
            auto opened = detail::FwAccess::openDocument(*this, abs, type, true, id);
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
        s.active = true;
        rd.status = DocRecovery::UpToDate;
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
    return report;
}

} // namespace helios::tf
