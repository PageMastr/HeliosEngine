#pragma once
// Crash-recovery journal (07 §1.2, AAA-STB-2): every committed transaction is appended to
// `%LOCALAPPDATA%\Helios\journal\<project>\<session>.hjl` (`$XDG_STATE_HOME/helios/journal/...`,
// default `~/.local/state/helios/journal`, on Linux).
//
// File format (`.hjl`, little-endian):
//   header  "HJL1" | u32 headerBytes | header JSON {"version":1,"project","session","user","host",
//           "pid","created"}
//   record  u32 payloadBytes | u32 check | payload (compact JSON, one object)
//           check = low 32 bits of XXH3-64(payload, seed = payloadBytes)
// Records: {"t":"open","doc","file","type","hash"[,"snapshot"]}, {"t":"save","doc","file","hash"},
// {"t":"close","doc"}, {"t":"tx", ...Transaction::toJson()}, {"t":"end"} (clean shutdown).
//
// Durability: append() hands the record to the OS before it returns (one write call), so a
// process crash or `kill -9` loses at most the transaction being committed at that moment. A
// flusher thread makes the file durable against power loss with fsync/FlushFileBuffers at most
// every `flushInterval` (group commit, default 50 ms). A torn last record (the crash hit
// mid-write) fails its length or check and is ignored by readJournal(); a writer that reopens a
// journal truncates it first.
//
// Threading: JournalWriter::append/close may be called from one thread at a time (the Framework
// owner); the flusher runs on its own thread. readJournal() is thread-safe.

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/toolsfw/transaction.h"

namespace helios::tf {

inline constexpr char kJournalMagic[4] = {'H', 'J', 'L', '1'};
inline constexpr u32 kJournalVersion = 1;
/// Largest record accepted by the reader (a larger length field is treated as a torn tail).
inline constexpr u32 kJournalMaxRecordBytes = 256u << 20;

struct JournalHeader {
    u32 version = kJournalVersion;
    std::string project;
    std::string session;
    std::string user;
    std::string host;
    u32 pid = 0;
    i64 created = 0; ///< Unix nanoseconds.
};

enum class JournalRecordKind : u8 { Open, Save, Close, Tx, End };

std::string_view journalRecordKindName(JournalRecordKind kind) noexcept;

struct JournalRecord {
    JournalRecordKind kind = JournalRecordKind::Tx;
    DocId doc;                           ///< Open/Save/Close.
    std::string file;                    ///< Open/Save: project-relative source file.
    std::string typeName;                ///< Open: qualified record type.
    u64 hash = 0;                        ///< Open: source hash when opened; Save: hash written.
    /// Open written after a reload whose merge kept unsaved local edits: the merged record text
    /// (the file holds `hash`'s bytes; recovery restores this text before later transactions).
    std::optional<std::string> snapshot;
    Transaction tx;                      ///< Tx.
    u64 offset = 0;                      ///< Byte offset of the record in the file.

    /// Compact JSON payload.
    std::string toJson() const;
    static Result<JournalRecord> fromJson(std::string_view payload);
};

struct JournalScan {
    JournalHeader header;
    std::vector<JournalRecord> records;
    u64 validBytes = 0;  ///< Header plus every intact record.
    u64 tornBytes = 0;   ///< Trailing bytes that do not form an intact record (ignored).
    bool clean = false;  ///< Ends with an "end" record.
    std::vector<std::string> warnings;
};

/// Reads a journal. Fails only when the file cannot be read or its header is missing or corrupt;
/// a torn or corrupt tail ends the scan (counted in tornBytes).
Result<JournalScan> readJournal(const fs::Path& path);

struct JournalOptions {
    /// Group-commit window: the flusher makes appended records durable at most this long after
    /// the append (07 §1.2: ≤ 50 ms).
    std::chrono::milliseconds flushInterval{50};
    /// false: skip fsync (tests and tools that only need crash, not power-loss, durability).
    bool fsync = true;
};

class JournalWriter {
public:
    /// Creates a new journal (fails with AlreadyExists if `path` exists). Creates parent directories.
    static Result<std::unique_ptr<JournalWriter>> create(const fs::Path& path, const JournalHeader& header,
                                                         const JournalOptions& options = {});
    /// Opens an existing journal for appending: validates the header, truncates a torn tail and
    /// drops a trailing "end" record so the session can continue.
    static Result<std::unique_ptr<JournalWriter>> reopen(const fs::Path& path, const JournalOptions& options = {});
    ~JournalWriter();
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    /// Appends one record (see the header comment for the durability guarantee).
    Result<void> append(std::string_view payloadJson);
    Result<void> append(const JournalRecord& record) { return append(record.toJson()); }
    /// Writes pending data to stable storage now.
    Result<void> sync();
    /// Appends "end" when `clean`, syncs and closes. Idempotent.
    Result<void> close(bool clean = true);

    const fs::Path& path() const noexcept { return m_path; }
    const JournalHeader& header() const noexcept { return m_header; }
    u64 recordsWritten() const noexcept { return m_records; }
    u64 bytesWritten() const noexcept { return m_bytes; }
    /// Completed fsync calls (tests of the group-commit window).
    u64 syncCount() const noexcept;

private:
    JournalWriter(fs::Path path, fs::File file, JournalHeader header, const JournalOptions& options, u64 bytes);
    void flusherMain();

    fs::Path m_path;
    fs::File m_file;
    JournalHeader m_header;
    JournalOptions m_options;
    u64 m_records = 0;
    u64 m_bytes = 0;
    bool m_closed = false;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_dirty = false;
    bool m_stop = false;
    u64 m_syncs = 0;
    std::thread m_flusher;
};

/// `%LOCALAPPDATA%\Helios\journal` on Windows, `$XDG_STATE_HOME/helios/journal` (or
/// `~/.local/state/helios/journal`) elsewhere. HELIOS_JOURNAL_DIR overrides both.
fs::Path defaultJournalRoot();
/// `<root>/<project>`; the project name is sanitized to a file-name-safe form.
fs::Path journalDirectory(const fs::Path& root, std::string_view project);
/// A new session name: `<yyyymmdd-hhmmss>-<pid>` (UTC). Framework appends `-2`, `-3`, ... when
/// that journal already exists (two sessions of one process within a second).
std::string newSessionName();

struct JournalSessionInfo {
    fs::Path path;
    JournalHeader header;
    bool clean = false;
    u64 txCount = 0;
    u64 tornBytes = 0;
    u64 maxLamport = 0;  ///< Highest transaction Lamport counter in the session.
};

/// The journals of a project, oldest first. `uncleanOnly` keeps sessions without an "end" record
/// whose process is no longer running (the editor offers to replay those after a crash). A journal
/// whose header names another project is skipped (with a warning).
std::vector<JournalSessionInfo> listJournalSessions(const fs::Path& root, std::string_view project, bool uncleanOnly);

/// True when a process with this id is running on this machine (used to tell a crashed session
/// from a live one).
bool processIsRunning(u32 pid) noexcept;
/// This process's id (journal headers, default endpoint names such as helios-editor-<pid>).
u32 currentProcessId() noexcept;

} // namespace helios::tf
