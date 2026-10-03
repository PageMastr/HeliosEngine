#pragma once
// ToolsFramework (07 §1.2, ADR-009): the UI-less editor core shared by helios-editor, helios-tool
// and (later) helios-assetd. It owns the project's documents, the selection, the command bus, the
// transaction history (undo/redo) and the crash-recovery journal.
//
//   tf::FrameworkConfig cfg;
//   cfg.project = "my-game";
//   cfg.projectRoot = "content";
//   auto fw = tf::Framework::create(cfg).value();
//   fw->openAll();                                        // records/**/*.hrec
//   fw->invoker(tf::Origin::Cli).invoke("doc.setProperty",
//       R"({"doc": "hull/frigate", "path": "mass", "value": 15000})");
//   fw->undo(tf::Origin::Cli);                            // a new inverse transaction
//   fw->saveAll();
//
// Every mutation is a transaction (07 §0 rule 3): ops apply to the documents immediately while a
// TxBuilder collects them (so later ops see earlier ones), and the whole transaction commits or
// rolls back atomically. Commit runs the pre-commit hooks (schema ranges, `@max`, key integrity),
// bumps revisions, coalesces continuous gestures by mergeKey, records the transaction in the
// history and appends it to the journal before returning, then runs the post-commit hooks.
//
// Undo is a new inverse transaction (kind Undo), so the journal, remote clients and collaborators
// see it (07 §1.2). History is global with per-document views; it survives saves and is capped at
// `historyByteLimit` (512 MB): past the cap the oldest entries are dropped down to 7/8 of it.
//
// Budget: a commit, undo or redo costs O(its ops), independent of the session's length; a one-op
// edit stays ≤ 0.1 ms (RelWithDebInfo, the dev container), so 60 Hz gestures never approach a
// frame (`perf:` case in tests/test_history.cpp, which also checks that the cost stays flat over
// 50,000 commits).
//
// Threading: a Framework is owned by one thread (the editor's main thread, the tool's thread);
// every member must be called from it. The journal flusher and the RPC server's IO threads never
// touch documents: RPC requests are dispatched on the owner thread by RpcServer::pump().

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/reflect/registry.h"
#include "helios/toolsfw/command.h"
#include "helios/toolsfw/document.h"
#include "helios/toolsfw/journal.h"
#include "helios/toolsfw/selection.h"
#include "helios/toolsfw/transaction.h"

namespace helios::tf {

class Framework;
namespace detail {
struct FwAccess;
}

struct FrameworkConfig {
    /// Project name (journal directory, window titles).
    std::string project = "project";
    /// Project root: record files live under `<root>/records/<table>/...hrec`.
    fs::Path projectRoot;
    /// Types the documents may have (schema registrations).
    const refl::TypeRegistry* types = nullptr; ///< null = TypeRegistry::global()
    /// Author of this session's transactions (`TxId::user`).
    std::string user = "local";
    /// Journal every transaction (07 §1.2). Off for scratch frameworks in tests.
    bool journal = true;
    fs::Path journalRoot;   ///< Empty = defaultJournalRoot().
    std::string session;    ///< Empty = newSessionName().
    JournalOptions journalOptions;
    /// History cap (07 §1.2: 512 MB).
    u64 historyByteLimit = 512ull << 20;
    /// Transaction ids continue after this Lamport counter. helios-tool passes the highest one in
    /// the project's journals, so the ids of its separate processes never repeat.
    u64 lamportFloor = 0;
    /// Clock for Transaction::time (unix nanoseconds); null = the system clock.
    std::function<i64()> clock;
    /// Keys for new keyed-list elements; null = Guid::generate(). Tests pass a seeded generator.
    std::function<Guid()> newKey;
    /// Register the built-in doc.*, edit.*, selection.* and validate.* commands.
    bool builtinCommands = true;
};

/// Collects the ops of one transaction; see the header comment. Obtained from
/// Framework::begin() or CommandContext::tx(). Every call applies its op immediately; a failing
/// call leaves the documents unchanged.
class TxBuilder {
public:
    ~TxBuilder();
    TxBuilder(const TxBuilder&) = delete;
    TxBuilder& operator=(const TxBuilder&) = delete;

    /// Sets the value at `path` to the JSON `json` (map keys and optionals are created as needed).
    Result<void> set(const DocId& doc, std::string_view path, std::string_view json);
    /// Removes a map key, resets an optional or removes a list element (`list[#key]`, `list[3]`).
    Result<void> remove(const DocId& doc, std::string_view path);
    /// Inserts an element (JSON) into the list at `listPath` before `index` (index == size
    /// appends). Keyed lists get `key`, or a new key from FrameworkConfig::newKey.
    Result<void> insert(const DocId& doc, std::string_view listPath, u64 index, std::string_view elementJson,
                        std::optional<Guid> key = std::nullopt);
    /// Moves the element at `elementPath` (`list[#key]` or `list[i]`) to `toIndex`.
    Result<void> move(const DocId& doc, std::string_view elementPath, u64 toIndex);
    /// Reflection-based property diff (ADR-009): `mutate` edits a copy of the document's object;
    /// the diff between the copy and the document becomes this transaction's ops.
    Result<void> edit(const DocId& doc, const std::function<void(void* object)>& mutate);
    /// Creates a record document at the project-relative `file` (".hrec"). `json` is the initial
    /// field values (empty = the type's defaults). Returns the new document's id.
    Result<DocId> createRecord(const refl::TypeInfo& type, std::string_view file, const refl::RecordHeader& header,
                               std::string_view json = {});
    /// Removes a document (its file is deleted when the project is saved).
    Result<void> destroy(const DocId& doc);
    /// Applies a raw op (replay, collaboration, patches).
    Result<void> apply(const Op& op);

    void setLabel(std::string label) { m_label = std::move(label); }
    void setMergeKey(std::string key) { m_mergeKey = std::move(key); }
    /// Records this transaction as the Undo/Redo of `target` from an earlier session (helios-tool's
    /// cross-process undo): the journal and log carry the kind and target, so later sessions can
    /// rebuild the linear undo stack. In this session's history it is an ordinary entry.
    void markRevert(TxKind kind, TxId target) {
        m_kind = kind;
        m_target = std::move(target);
    }
    const std::string& label() const noexcept { return m_label; }
    const std::vector<Op>& ops() const noexcept { return m_ops; }
    bool empty() const noexcept { return m_ops.empty(); }
    Origin origin() const noexcept { return m_origin; }

    /// Commits (see Framework). An empty builder commits nothing and returns a null TxId.
    Result<TxId> commit();
    /// Rolls back every op applied so far.
    void abort();

private:
    friend class Framework;
    friend struct detail::FwAccess;
    TxBuilder(Framework& fw, Origin origin, std::string label) noexcept;
    Result<void> push(Op op);

    Framework* m_fw;
    Origin m_origin;
    std::string m_label;
    std::string m_mergeKey;
    TxKind m_kind = TxKind::Do;
    TxId m_target;
    std::vector<Op> m_ops;
    std::vector<std::pair<DocId, u64>> m_baseRev;
    bool m_done = false;
    bool m_replay = false;  ///< Undo/redo, recovery, external edits: no pre-commit hooks.
};

struct HistoryEntry {
    Transaction tx;     ///< A Do transaction (merged gestures are one entry).
    bool undone = false;
};

/// Result of a pre-commit hook; `ok == false` rejects the transaction with `message`.
using PreCommitHook = std::function<Result<void>(const Framework&, const Transaction&)>;
using PostCommitHook = std::function<void(const Framework&, const Transaction&)>;

enum class DocRecovery : u8 {
    Replayed,       ///< Unsaved transactions re-applied.
    UpToDate,       ///< Nothing to replay (saved after the last transaction).
    Created,        ///< A never-saved document restored from its snapshot.
    SourceChanged,  ///< The file changed after the session's last save; replay skipped.
    Missing,        ///< The file is gone; replay skipped.
    Conflict,       ///< An op failed its precondition; replay of this document stopped.
};

std::string_view docRecoveryName(DocRecovery status) noexcept;

struct RecoveredDocument {
    DocId doc;
    std::string file;
    DocRecovery status = DocRecovery::UpToDate;
    u64 transactions = 0;
    std::string message;
};

struct RecoveryReport {
    u64 replayed = 0;       ///< Transactions re-applied (partially counts as replayed).
    u64 skipped = 0;        ///< Transactions skipped (saved already, or their document skipped).
    u64 tornBytes = 0;
    bool clean = false;     ///< The journal ended with a clean shutdown (nothing was lost).
    std::vector<RecoveredDocument> documents;
};

struct RecoveryOptions {
    /// Replay even when a document's file changed after the session's last save (the ops'
    /// preconditions still guard the replay).
    bool ignoreSourceChanges = false;
};

/// A change the UI and remote clients may want to react to.
struct FrameworkEvent {
    enum class Kind : u8 { Committed, Undone, Redone, Opened, Closed, Saved, Reloaded, Destroyed, Created };
    Kind kind = Kind::Committed;
    DocId doc;          ///< Opened/Closed/Saved/Reloaded/Destroyed/Created.
    TxId tx;            ///< Committed/Undone/Redone.
};

class Framework {
public:
    static Result<std::unique_ptr<Framework>> create(const FrameworkConfig& config);
    ~Framework();
    Framework(const Framework&) = delete;
    Framework& operator=(const Framework&) = delete;

    const FrameworkConfig& config() const noexcept { return m_config; }
    const Workspace& documents() const noexcept { return *m_workspace; }
    const refl::TypeRegistry& types() const noexcept { return m_workspace->types(); }
    Selection& selection() noexcept { return m_selection; }
    const Selection& selection() const noexcept { return m_selection; }
    CommandBus& commands() noexcept { return m_commands; }
    const CommandBus& commands() const noexcept { return m_commands; }
    /// The invoker of an input path (see command.h).
    CommandInvoker& invoker(Origin origin) noexcept { return m_invokers[static_cast<usize>(origin)]; }
    /// The journal (null when journaling is off).
    JournalWriter* journal() const noexcept { return m_journal.get(); }

    // ---- documents ----------------------------------------------------------------------------
    /// Opens a record file (absolute or project-relative). The type comes from `type` or from the
    /// file's `records/<table>/` directory. Opening an open file returns the open document.
    Result<Document*> open(const fs::Path& file, const refl::TypeInfo* type = nullptr);
    /// Opens every `*.hrec` under `<root>/records`, sorted by path. Returns the number opened.
    Result<usize> openAll();
    /// Writes a dirty document's canonical text atomically (a destroyed one's file is removed).
    Result<void> save(const DocId& doc);
    Result<usize> saveAll();
    /// Closes a document. Refuses a dirty one unless `discard`. Its history entries are dropped.
    Result<void> close(const DocId& doc, bool discard = false);
    /// Brings a document in line with its file after an external edit (VS Code, `git pull`): an
    /// undoable transaction with origin Import. When the document has unsaved edits, the file's
    /// changes since the last load/save are merged three-way (07 §1.2); conflicting paths fail with
    /// InvalidState and nothing changes.
    Result<TxId> reloadFromDisk(const DocId& doc);

    // ---- transactions -------------------------------------------------------------------------
    /// Starts a transaction for `origin`. Commit it with TxBuilder::commit().
    std::unique_ptr<TxBuilder> begin(Origin origin, std::string label = {});
    /// Groups the transactions of every command until endGroup() into one undo step (Luau
    /// `Editor.transaction`, multi-command UI actions). Groups nest; the outermost commits, with
    /// the outermost origin and label. While a group is open, commands may run only through the
    /// invoker of the group's origin (07 §1.2: the origin comes from the input path).
    void beginGroup(Origin origin, std::string label);
    /// Closes the innermost level; closing the outermost commits the group.
    Result<TxId> endGroup();
    /// Cancels the innermost level: a nested level rolls back only the ops made since its
    /// beginGroup() and the enclosing levels stay open; the outermost level aborts the group.
    void cancelGroup();
    bool inGroup() const noexcept { return !m_groupMarks.empty(); }
    /// Open group levels (0 = none).
    usize groupDepth() const noexcept { return m_groupMarks.size(); }

    Result<TxId> undo(Origin origin, std::optional<DocId> doc = std::nullopt);
    Result<TxId> redo(Origin origin, std::optional<DocId> doc = std::nullopt);
    bool canUndo(std::optional<DocId> doc = std::nullopt) const;
    bool canRedo(std::optional<DocId> doc = std::nullopt) const;
    /// Label of the transaction undo()/redo() would revert/re-apply ("" if none).
    std::string undoLabel(std::optional<DocId> doc = std::nullopt) const;
    std::string redoLabel(std::optional<DocId> doc = std::nullopt) const;
    /// Every Do transaction still in the history, oldest first.
    std::span<const HistoryEntry> history() const noexcept { return m_history; }
    /// Every transaction committed in this session (Do, Undo, Redo), oldest first. Capped with
    /// the history (tests, the `tx.log` RPC and provenance reports).
    std::span<const Transaction> log() const noexcept { return m_log; }
    u64 historyBytes() const noexcept { return m_historyBytes; }

    void addPreCommitHook(PreCommitHook hook) { m_preHooks.push_back(std::move(hook)); }
    void addPostCommitHook(PostCommitHook hook) { m_postHooks.push_back(std::move(hook)); }
    /// Observers of FrameworkEvent (UI refresh, RPC notifications). Called on the owner thread.
    void addListener(std::function<void(const FrameworkEvent&)> listener) { m_listeners.push_back(std::move(listener)); }
    /// Incremented by every event (cheap change polling).
    u64 changeCounter() const noexcept { return m_changes; }

    // ---- recovery ------------------------------------------------------------------------------
    /// Replays an unclean journal of this project (07 §1.2): per document, verifies that its file
    /// still matches the session's last open/save hash and re-applies the transactions after it.
    /// Replayed transactions join this session's history (undoable) and journal.
    Result<RecoveryReport> recover(const fs::Path& journalFile, const RecoveryOptions& options = {});

    /// Current unix time from the configured clock.
    i64 now() const;

private:
    friend class TxBuilder;
    friend class CommandInvoker;
    friend struct detail::FwAccess;
    explicit Framework(const FrameworkConfig& config);
    Result<void> init();

    Result<TxId> commit(TxBuilder& builder);
    Result<void> applyOp(const Op& op);
    void rollback(std::vector<Op>& applied);
    Result<TxId> revert(Origin origin, std::optional<DocId> doc, bool redo);
    void pushLog(Transaction tx);
    void trimHistory();
    void emit(const FrameworkEvent& event);
    Result<void> journalRecord(const JournalRecord& record);
    Result<Document*> openInternal(const fs::Path& absolute, const refl::TypeInfo* type, bool journal);
    Guid newKey();

    FrameworkConfig m_config;
    std::unique_ptr<Workspace> m_workspace;
    Selection m_selection;
    CommandBus m_commands;
    std::array<CommandInvoker, kOriginCount> m_invokers;
    std::unique_ptr<JournalWriter> m_journal;
    std::vector<HistoryEntry> m_history;
    std::vector<Transaction> m_log;
    std::vector<usize> m_redoStack;  ///< Indices into m_history, most recently undone last.
    u64 m_historyBytes = 0;
    u64 m_logBytes = 0;              ///< Sum of byteSize() over m_log (kept, never re-summed).
    u64 m_lamport = 0;
    u64 m_changes = 0;
    bool m_lastCanMerge = false;     ///< The last history entry may absorb a same-mergeKey commit.
    std::vector<PreCommitHook> m_preHooks;
    std::vector<PostCommitHook> m_postHooks;
    std::vector<std::function<void(const FrameworkEvent&)>> m_listeners;
    std::unique_ptr<TxBuilder> m_group;
    std::vector<usize> m_groupMarks;  ///< Per open group level: m_group's op count at its beginGroup().
};

} // namespace helios::tf
