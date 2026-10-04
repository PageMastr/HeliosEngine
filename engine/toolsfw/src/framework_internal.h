#pragma once
// Framework internals shared by framework.cpp, recovery.cpp, command.cpp and builtin_commands.cpp.

#include <vector>

#include "helios/toolsfw/framework.h"

namespace helios::tf::detail {

struct FwAccess {
    static Workspace& workspace(Framework& fw) noexcept { return *fw.m_workspace; }
    static TxBuilder* group(Framework& fw) noexcept { return fw.m_group.get(); }
    static usize opCount(const TxBuilder& b) noexcept { return b.m_ops.size(); }
    static void setReplay(TxBuilder& b, bool replay) noexcept { b.m_replay = replay; }
    static void setLamportAtLeast(Framework& fw, u64 lamport) noexcept {
        if (fw.m_lamport < lamport) fw.m_lamport = lamport;
    }

    /// Applies the ops that turn `d` into `target` (a full object of d's type) to `tx`.
    static Result<void> applyDiff(TxBuilder& tx, Document& d, const void* target);
    /// Rolls back `tx`'s ops beyond the first `count`.
    static void truncate(TxBuilder& tx, usize count);
    static Result<TxId> finishCommit(Framework& fw, TxBuilder& b, Transaction tx);
    static void eraseHistoryEntries(Framework& fw, std::vector<usize> indices);
    /// Reads and opens `file` (already through Workspace::confine) as a document with id `id`.
    static Result<Document*> openDocument(Framework& fw, const ProjectFile& file, const refl::TypeInfo* type, bool journal,
                                          DocId id);
    /// Brings `d` to the record text `text` (a reload's journaled snapshot) as one Import
    /// transaction; nothing changes when it fails.
    static Result<void> restoreSnapshot(Framework& fw, Document& d, std::string_view text);
    /// External edit (3-way merge) or, with discardLocal, a revert to the file.
    static Result<TxId> syncWithDisk(Framework& fw, const DocId& id, bool discardLocal);
    static Result<void> journal(Framework& fw, const JournalRecord& record) { return fw.journalRecord(record); }
    static void emit(Framework& fw, const FrameworkEvent& e) { fw.emit(e); }
};

/// Registers the built-in commands (builtin_commands.cpp).
Result<void> registerBuiltinCommands(Framework& fw);

} // namespace helios::tf::detail
