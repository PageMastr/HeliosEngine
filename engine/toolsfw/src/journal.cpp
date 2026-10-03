#include "helios/toolsfw/journal.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

#include "helios/core/hash.h"
#include "helios/core/platform.h"
#include "helios/toolsfw/json_util.h"

#include "platform/tf_os.h"

namespace helios::tf {

namespace {

constexpr u32 kMaxHeaderBytes = 1u << 20;

void putU32(std::string& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

u32 getU32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u32 recordCheck(std::string_view payload) noexcept {
    return static_cast<u32>(hash64(payload.data(), payload.size(), payload.size()));
}

std::string headerJson(const JournalHeader& h) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginObject();
    w.key("version");
    w.unsignedInteger(h.version);
    w.key("project");
    w.string(h.project);
    w.key("session");
    w.string(h.session);
    w.key("user");
    w.string(h.user);
    w.key("host");
    w.string(h.host);
    w.key("pid");
    w.unsignedInteger(h.pid);
    w.key("created");
    w.integer(h.created);
    w.endObject();
    return w.take();
}

Result<JournalHeader> parseHeader(std::string_view text) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(text, "<journal header>"));
    const refl::JsonValue v = doc.root();
    if (!v.isObject()) return Error{ErrorCode::Corrupt, "journal header is not an object"};
    JournalHeader h;
    h.version = static_cast<u32>(json::getInteger(v, "version").value_or(0));
    if (h.version != kJournalVersion) {
        return Error{ErrorCode::VersionMismatch, std::format("journal version {} (expected {})", h.version, kJournalVersion)};
    }
    h.project = std::string(json::getString(v, "project").value_or(""));
    h.session = std::string(json::getString(v, "session").value_or(""));
    h.user = std::string(json::getString(v, "user").value_or(""));
    h.host = std::string(json::getString(v, "host").value_or(""));
    h.pid = static_cast<u32>(json::getInteger(v, "pid").value_or(0));
    h.created = json::getInteger(v, "created").value_or(0);
    return h;
}

std::string frame(std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 8);
    putU32(out, static_cast<u32>(payload.size()));
    putU32(out, recordCheck(payload));
    out.append(payload);
    return out;
}

} // namespace

std::string_view journalRecordKindName(JournalRecordKind kind) noexcept {
    switch (kind) {
    case JournalRecordKind::Open: return "open";
    case JournalRecordKind::Save: return "save";
    case JournalRecordKind::Close: return "close";
    case JournalRecordKind::Tx: return "tx";
    case JournalRecordKind::End: return "end";
    }
    return "unknown";
}

std::string JournalRecord::toJson() const {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginObject();
    w.key("t");
    w.string(journalRecordKindName(kind));
    switch (kind) {
    case JournalRecordKind::Open:
    case JournalRecordKind::Save:
        w.key("doc");
        w.string(doc.toString());
        w.key("file");
        w.string(file);
        if (kind == JournalRecordKind::Open) {
            w.key("type");
            w.string(typeName);
        }
        w.key("hash");
        w.string(hashHex(hash));
        if (snapshot) {
            w.key("snapshot");
            w.string(*snapshot);
        }
        break;
    case JournalRecordKind::Close:
        w.key("doc");
        w.string(doc.toString());
        break;
    case JournalRecordKind::Tx:
        w.key("tx");
        tx.writeJson(w);
        break;
    case JournalRecordKind::End: break;
    }
    w.endObject();
    return w.take();
}

Result<JournalRecord> JournalRecord::fromJson(std::string_view payload) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(payload, "<journal record>"));
    const refl::JsonValue v = doc.root();
    const auto t = json::getString(v, "t");
    if (!t) return Error{ErrorCode::Corrupt, "journal record without \"t\""};
    JournalRecord r;
    if (*t == "open") {
        r.kind = JournalRecordKind::Open;
    } else if (*t == "save") {
        r.kind = JournalRecordKind::Save;
    } else if (*t == "close") {
        r.kind = JournalRecordKind::Close;
    } else if (*t == "tx") {
        r.kind = JournalRecordKind::Tx;
    } else if (*t == "end") {
        r.kind = JournalRecordKind::End;
    } else {
        return Error{ErrorCode::Corrupt, "unknown journal record '" + std::string(*t) + "'"};
    }
    if (r.kind == JournalRecordKind::Open || r.kind == JournalRecordKind::Save || r.kind == JournalRecordKind::Close) {
        const auto d = json::getString(v, "doc");
        if (!d) return Error{ErrorCode::Corrupt, "journal record without \"doc\""};
        HELIOS_TRY_ASSIGN(r.doc, Guid::parse(*d));
    }
    if (r.kind == JournalRecordKind::Open || r.kind == JournalRecordKind::Save) {
        r.file = std::string(json::getString(v, "file").value_or(""));
        r.typeName = std::string(json::getString(v, "type").value_or(""));
        const auto hash = parseHashHex(json::getString(v, "hash").value_or(""));
        if (!hash) return Error{ErrorCode::Corrupt, "journal record with a bad \"hash\""};
        r.hash = *hash;
        if (const auto snap = json::getString(v, "snapshot")) r.snapshot = std::string(*snap);
    }
    if (r.kind == JournalRecordKind::Tx) {
        HELIOS_TRY_ASSIGN(r.tx, Transaction::fromJson(v.get("tx")));
    }
    return r;
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------
Result<JournalScan> readJournal(const fs::Path& path) {
    HELIOS_TRY_ASSIGN(const std::vector<u8> bytes, fs::readFile(path));
    JournalScan scan;
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kJournalMagic, 4) != 0) {
        return Error{ErrorCode::Corrupt, std::format("{}: not a Helios journal", fs::pathToGenericUtf8(path))};
    }
    const u32 headerBytes = getU32(bytes.data() + 4);
    if (headerBytes > kMaxHeaderBytes || 8ull + headerBytes > bytes.size()) {
        return Error{ErrorCode::Corrupt, std::format("{}: truncated journal header", fs::pathToGenericUtf8(path))};
    }
    HELIOS_TRY_ASSIGN(scan.header, parseHeader(std::string_view(reinterpret_cast<const char*>(bytes.data()) + 8, headerBytes)));
    u64 pos = 8ull + headerBytes;
    while (pos < bytes.size()) {
        const u64 remaining = bytes.size() - pos;
        if (remaining < 8) break;
        const u32 len = getU32(bytes.data() + pos);
        const u32 check = getU32(bytes.data() + pos + 4);
        if (len > kJournalMaxRecordBytes || remaining - 8 < len) break;
        const std::string_view payload(reinterpret_cast<const char*>(bytes.data()) + pos + 8, len);
        if (recordCheck(payload) != check) break;
        auto rec = JournalRecord::fromJson(payload);
        if (!rec) {
            scan.warnings.push_back(std::format("record at offset {}: {}", pos, rec.error().message));
            break;
        }
        rec->offset = pos;
        scan.clean = rec->kind == JournalRecordKind::End;
        scan.records.push_back(std::move(*rec));
        pos += 8ull + len;
    }
    scan.validBytes = pos;
    scan.tornBytes = bytes.size() - pos;
    return scan;
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------
JournalWriter::JournalWriter(fs::Path path, fs::File file, JournalHeader header, const JournalOptions& options, u64 bytes)
    : m_path(std::move(path)), m_file(std::move(file)), m_header(std::move(header)), m_options(options), m_bytes(bytes) {
    if (m_options.fsync) m_flusher = std::thread([this] { flusherMain(); });
}

JournalWriter::~JournalWriter() {
    // A writer destroyed without close() is a crash-like end (no "end" record), but the data is
    // still made durable.
    if (!m_closed) {
        if (auto r = close(false); !r) HELIOS_LOG_ERROR(LogTools, "journal {}: {}", fs::pathToGenericUtf8(m_path), r.error());
    }
}

Result<std::unique_ptr<JournalWriter>> JournalWriter::create(const fs::Path& path, const JournalHeader& header,
                                                            const JournalOptions& options) {
    if (fs::exists(path)) return Error{ErrorCode::AlreadyExists, std::format("{} exists", fs::pathToGenericUtf8(path))};
    HELIOS_TRY(fs::createDirectories(path.parent_path()));
    HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(path, fs::OpenMode::Append));
    const std::string json = headerJson(header);
    std::string head(kJournalMagic, 4);
    putU32(head, static_cast<u32>(json.size()));
    head += json;
    HELIOS_TRY(file.write(head.data(), head.size()));
    if (options.fsync) HELIOS_TRY(file.sync());
    return std::unique_ptr<JournalWriter>(new JournalWriter(path, std::move(file), header, options, head.size()));
}

Result<std::unique_ptr<JournalWriter>> JournalWriter::reopen(const fs::Path& path, const JournalOptions& options) {
    HELIOS_TRY_ASSIGN(const JournalScan scan, readJournal(path));
    u64 keep = scan.validBytes;
    if (!scan.records.empty() && scan.records.back().kind == JournalRecordKind::End) keep = scan.records.back().offset;
    if (keep != scan.validBytes || scan.tornBytes != 0) {
        HELIOS_TRY_ASSIGN(std::vector<u8> bytes, fs::readFile(path));
        bytes.resize(static_cast<usize>(keep));
        HELIOS_TRY(fs::writeFile(path, bytes, fs::WriteMode::Atomic));
    }
    HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(path, fs::OpenMode::Append));
    auto w = std::unique_ptr<JournalWriter>(new JournalWriter(path, std::move(file), scan.header, options, keep));
    w->m_records = scan.records.size() - (keep != scan.validBytes && !scan.records.empty() ? 1 : 0);
    return w;
}

Result<void> JournalWriter::append(std::string_view payloadJson) {
    if (payloadJson.size() > kJournalMaxRecordBytes) return Error{ErrorCode::LimitExceeded, "journal record too large"};
    const std::string bytes = frame(payloadJson);
    std::lock_guard lock(m_mutex);
    if (m_closed) return Error{ErrorCode::InvalidState, "journal is closed"};
    // One write call: a crash can only tear this record, never an earlier one.
    HELIOS_TRY(m_file.write(bytes.data(), bytes.size()));
    ++m_records;
    m_bytes += bytes.size();
    if (m_options.fsync) {
        m_dirty = true;
        m_cv.notify_all();
    }
    return {};
}

Result<void> JournalWriter::sync() {
    std::lock_guard lock(m_mutex);
    if (m_closed) return {};
    HELIOS_TRY(m_file.sync());
    m_dirty = false;
    ++m_syncs;
    return {};
}

u64 JournalWriter::syncCount() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_syncs;
}

void JournalWriter::flusherMain() {
    std::unique_lock lock(m_mutex);
    while (!m_stop) {
        m_cv.wait(lock, [&] { return m_dirty || m_stop; });
        if (m_stop) break;
        // Group commit: gather the appends of one window, then make them durable together.
        m_cv.wait_for(lock, m_options.flushInterval, [&] { return m_stop; });
        if (!m_dirty || m_closed) continue;
        if (auto r = m_file.sync(); !r) {
            HELIOS_LOG_ERROR(LogTools, "journal {}: sync failed: {}", fs::pathToGenericUtf8(m_path), r.error());
        }
        m_dirty = false;
        ++m_syncs;
    }
}

Result<void> JournalWriter::close(bool clean) {
    {
        std::lock_guard lock(m_mutex);
        if (m_closed) return {};
    }
    Result<void> result;
    if (clean) {
        JournalRecord end;
        end.kind = JournalRecordKind::End;
        result = append(end);
    }
    {
        std::lock_guard lock(m_mutex);
        if (m_closed) return result;
        m_stop = true;
        m_cv.notify_all();
    }
    if (m_flusher.joinable()) m_flusher.join();
    std::lock_guard lock(m_mutex);
    if (m_options.fsync && m_dirty) {
        if (auto r = m_file.sync(); !r && result) result = std::move(r);
        ++m_syncs;
    }
    m_dirty = false;
    m_closed = true;
    m_file.close();
    return result;
}

// ---------------------------------------------------------------------------------------------
// Locations and sessions
// ---------------------------------------------------------------------------------------------
namespace {
std::string env(const char* name) {
    return os::environment(name);
}
} // namespace

fs::Path defaultJournalRoot() {
    if (const std::string o = env("HELIOS_JOURNAL_DIR"); !o.empty()) return fs::pathFromUtf8(o);
    if constexpr (platform::kIsWindows) {
        if (const std::string local = env("LOCALAPPDATA"); !local.empty()) {
            return fs::pathFromUtf8(local) / "Helios" / "journal";
        }
    } else {
        if (const std::string state = env("XDG_STATE_HOME"); !state.empty()) {
            return fs::pathFromUtf8(state) / "helios" / "journal";
        }
        if (const std::string home = env("HOME"); !home.empty()) {
            return fs::pathFromUtf8(home) / ".local" / "state" / "helios" / "journal";
        }
    }
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "helios" / "journal";
}

fs::Path journalDirectory(const fs::Path& root, std::string_view project) {
    std::string safe;
    for (char c : project) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty() || safe == "." || safe == "..") safe = "project";
    return root / fs::pathFromUtf8(safe);
}

std::string newSessionName() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto day = floor<days>(now);
    const year_month_day ymd{day};
    const hh_mm_ss hms{floor<seconds>(now - day)};
    return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()), hms.hours().count(), hms.minutes().count(),
                       hms.seconds().count(), os::currentProcessId());
}

bool processIsRunning(u32 pid) noexcept {
    return os::processIsRunning(pid);
}

u32 currentProcessId() noexcept {
    return os::currentProcessId();
}

std::vector<JournalSessionInfo> listJournalSessions(const fs::Path& root, std::string_view project, bool uncleanOnly) {
    std::vector<JournalSessionInfo> out;
    const fs::Path dir = journalDirectory(root, project);
    if (!fs::isDirectory(dir)) return out;
    fs::ListOptions opts;
    opts.includeDirectories = false;
    opts.extension = ".hjl";
    auto entries = fs::listDirectory(dir, opts);
    if (!entries) return out;
    const std::string host = os::hostName();
    const u32 self = os::currentProcessId();
    for (const fs::DirEntry& e : *entries) {
        auto scan = readJournal(e.path);
        if (!scan) {
            HELIOS_LOG_WARN(LogTools, "skipping {}: {}", e.relativePath, scan.error());
            continue;
        }
        JournalSessionInfo info;
        info.path = e.path;
        info.header = scan->header;
        info.clean = scan->clean;
        info.tornBytes = scan->tornBytes;
        info.txCount = static_cast<u64>(std::count_if(scan->records.begin(), scan->records.end(),
                                                      [](const JournalRecord& r) { return r.kind == JournalRecordKind::Tx; }));
        if (uncleanOnly) {
            if (info.clean) continue;
            const bool thisHost = info.header.host == host;
            if (thisHost && (info.header.pid == self || os::processIsRunning(info.header.pid))) continue;
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const JournalSessionInfo& a, const JournalSessionInfo& b) {
        if (a.header.created != b.header.created) return a.header.created < b.header.created;
        return a.path < b.path;
    });
    return out;
}

} // namespace helios::tf
