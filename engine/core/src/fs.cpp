#include "helios/core/fs.h"

#include <algorithm>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "helios/core/guid.h"
#include "helios/core/log.h"
#include "helios/core/time.h"
#include "platform/os.h"

namespace helios::fs {

namespace {

ErrorCode mapErrorCode(const std::error_code& ec) noexcept {
    if (ec == std::errc::no_such_file_or_directory) return ErrorCode::NotFound;
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
        return ErrorCode::PermissionDenied;
    }
    if (ec == std::errc::file_exists) return ErrorCode::AlreadyExists;
    if (ec == std::errc::directory_not_empty) return ErrorCode::InvalidState;
    if (ec == std::errc::not_a_directory || ec == std::errc::is_a_directory) return ErrorCode::InvalidArgument;
    return ErrorCode::IoError;
}

Error fsError(const std::error_code& ec, std::string_view what, const Path& path) {
    return Error{mapErrorCode(ec), std::format("{} '{}': {}", what, pathToGenericUtf8(path), ec.message())};
}

char asciiLower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

} // namespace

Path pathFromUtf8(std::string_view utf8) {
    return Path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const Path& path) {
    const std::u8string s = path.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string pathToGenericUtf8(const Path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------------------------
// Whole-file helpers
// ---------------------------------------------------------------------------------------------

Result<std::vector<u8>> readFile(const Path& path) {
    HELIOS_TRY_ASSIGN(File file, File::open(path, OpenMode::Read));
    HELIOS_TRY_ASSIGN(const u64 size, file.size());
    std::vector<u8> data(static_cast<usize>(size));
    usize total = 0;
    while (total < data.size()) {
        HELIOS_TRY_ASSIGN(const usize got, file.read(data.data() + total, data.size() - total));
        if (got == 0) { // shrank while reading
            data.resize(total);
            return data;
        }
        total += got;
    }
    // Keep reading until EOF rather than trusting the size (the file may be growing, and /proc-style
    // files report 0). Probe through a small buffer: growing `data` here would allocate and zero
    // twice the file size just to observe EOF on an exactly-sized file (e.g. a 1 GiB pak).
    u8 probe[4096];
    for (;;) {
        HELIOS_TRY_ASSIGN(const usize got, file.read(probe, sizeof(probe)));
        if (got == 0) break;
        data.insert(data.end(), probe, probe + got);
    }
    return data;
}

Result<std::string> readTextFile(const Path& path) {
    HELIOS_TRY_ASSIGN(auto bytes, readFile(path));
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Result<void> writeFile(const Path& path, std::span<const u8> data, WriteMode mode) {
    if (mode == WriteMode::Direct) {
        HELIOS_TRY_ASSIGN(File file, File::open(path, OpenMode::Write));
        return file.write(data.data(), data.size());
    }
    // Atomic: unique sibling temp file (same volume) -> flush to disk -> atomic replace.
    Path temp = path;
    temp += pathFromUtf8(".tmp-" + Guid::generate().toString());
    {
        auto file = File::open(temp, OpenMode::Write);
        if (!file) return std::move(file).error();
        Result<void> written = file->write(data.data(), data.size());
        if (written) written = file->sync();
        file->close();
        if (!written) {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return written;
        }
    }
    Result<void> replaced = os::atomicReplace(temp, path);
    if (!replaced) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    }
    return replaced;
}

Result<void> writeTextFile(const Path& path, std::string_view text, WriteMode mode) {
    return writeFile(path, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()), mode);
}

bool exists(const Path& path) noexcept {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool isFile(const Path& path) noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool isDirectory(const Path& path) noexcept {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

Result<u64> fileSize(const Path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return fsError(ec, "file_size", path);
    return static_cast<u64>(size);
}

Result<std::filesystem::file_time_type> lastWriteTime(const Path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return fsError(ec, "last_write_time", path);
    return time;
}

Result<void> createDirectories(const Path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return fsError(ec, "create_directories", path);
    return {};
}

Result<void> remove(const Path& path) {
    std::error_code ec;
    if (!std::filesystem::remove(path, ec)) {
        if (!ec) return Error{ErrorCode::NotFound, std::format("remove '{}': no such file", pathToGenericUtf8(path))};
        return fsError(ec, "remove", path);
    }
    return {};
}

Result<u64> removeAll(const Path& path) {
    std::error_code ec;
    const auto count = std::filesystem::remove_all(path, ec);
    if (ec) return fsError(ec, "remove_all", path);
    return static_cast<u64>(count);
}

Result<void> rename(const Path& from, const Path& to) { return os::atomicReplace(from, to); }

Result<Path> executablePath() { return os::executablePath(); }

Result<Path> createUniqueTempDirectory(std::string_view prefix) {
    std::error_code ec;
    const Path base = std::filesystem::temp_directory_path(ec);
    if (ec) return fsError(ec, "temp_directory_path", Path());
    const Path dir = base / pathFromUtf8(std::string(prefix) + "-" + Guid::generate().toString());
    if (!std::filesystem::create_directories(dir, ec) || ec) return fsError(ec, "create_directories", dir);
    return dir;
}

Result<std::vector<DirEntry>> listDirectory(const Path& dir, const ListOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return Error{ErrorCode::NotFound, std::format("listDirectory '{}': not a directory", pathToGenericUtf8(dir))};
    }
    std::vector<DirEntry> out;
    auto visit = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entryEc;
        const bool isDir = entry.is_directory(entryEc);
        if (isDir ? !options.includeDirectories : !options.includeFiles) return;
        if (!isDir && !options.extension.empty() &&
            !iequals(pathToUtf8(entry.path().extension()), options.extension)) {
            return;
        }
        DirEntry e;
        e.path = entry.path();
        e.relativePath = pathToGenericUtf8(entry.path().lexically_relative(dir));
        e.isDirectory = isDir;
        e.size = isDir ? 0 : static_cast<u64>(entry.file_size(entryEc));
        if (entryEc) e.size = 0;
        out.push_back(std::move(e));
    };
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    if (options.recursive) {
        std::filesystem::recursive_directory_iterator it(dir, opts, ec), end;
        if (ec) return fsError(ec, "listDirectory", dir);
        for (; it != end; it.increment(ec)) {
            if (ec) return fsError(ec, "listDirectory", dir);
            visit(*it);
        }
    } else {
        std::filesystem::directory_iterator it(dir, opts, ec), end;
        if (ec) return fsError(ec, "listDirectory", dir);
        for (; it != end; it.increment(ec)) {
            if (ec) return fsError(ec, "listDirectory", dir);
            visit(*it);
        }
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.relativePath < b.relativePath; });
    return out;
}

// ---------------------------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------------------------

File::~File() { close(); }

File::File(File&& other) noexcept
    : m_handle(std::exchange(other.m_handle, os::kInvalidFile)), m_path(std::move(other.m_path)) {}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = std::exchange(other.m_handle, os::kInvalidFile);
        m_path = std::move(other.m_path);
    }
    return *this;
}

Result<File> File::open(const Path& path, OpenMode mode) {
    HELIOS_TRY_ASSIGN(const os::NativeFile handle, os::fileOpen(path, mode));
    File file;
    file.m_handle = handle;
    file.m_path = path;
    return file;
}

bool File::isOpen() const noexcept { return m_handle != os::kInvalidFile; }

void File::close() noexcept {
    if (m_handle != os::kInvalidFile) {
        os::fileClose(m_handle);
        m_handle = os::kInvalidFile;
    }
}

Result<usize> File::read(void* buffer, usize size) {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::read on a closed file"};
    return os::fileRead(m_handle, buffer, size);
}

Result<void> File::readExact(void* buffer, usize size) {
    auto* dst = static_cast<u8*>(buffer);
    usize total = 0;
    while (total < size) {
        HELIOS_TRY_ASSIGN(const usize got, read(dst + total, size - total));
        if (got == 0) {
            return makeError(ErrorCode::EndOfFile, "'{}': unexpected end of file ({} of {} bytes)",
                             pathToGenericUtf8(m_path), total, size);
        }
        total += got;
    }
    return {};
}

Result<usize> File::readAt(u64 offset, void* buffer, usize size) const {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::readAt on a closed file"};
    return os::fileReadAt(m_handle, offset, buffer, size);
}

Result<void> File::write(const void* data, usize size) {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::write on a closed file"};
    const auto* src = static_cast<const u8*>(data);
    usize total = 0;
    while (total < size) {
        HELIOS_TRY_ASSIGN(const usize put, os::fileWrite(m_handle, src + total, size - total));
        if (put == 0) return makeError(ErrorCode::IoError, "'{}': write made no progress", pathToGenericUtf8(m_path));
        total += put;
    }
    return {};
}

Result<u64> File::seek(i64 offset, SeekOrigin origin) {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::seek on a closed file"};
    return os::fileSeek(m_handle, offset, origin);
}

Result<u64> File::tell() { return seek(0, SeekOrigin::Current); }

Result<u64> File::size() const {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::size on a closed file"};
    return os::fileSize(m_handle);
}

Result<void> File::sync() {
    if (!isOpen()) return Error{ErrorCode::InvalidState, "File::sync on a closed file"};
    return os::fileSync(m_handle);
}

// ---------------------------------------------------------------------------------------------
// MappedFile
// ---------------------------------------------------------------------------------------------

MappedFile::~MappedFile() { close(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
      m_file(std::exchange(other.m_file, -1)), m_mapping(std::exchange(other.m_mapping, -1)),
      m_open(std::exchange(other.m_open, false)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        m_data = std::exchange(other.m_data, nullptr);
        m_size = std::exchange(other.m_size, 0);
        m_file = std::exchange(other.m_file, -1);
        m_mapping = std::exchange(other.m_mapping, -1);
        m_open = std::exchange(other.m_open, false);
    }
    return *this;
}

Result<MappedFile> MappedFile::open(const Path& path) {
    HELIOS_TRY_ASSIGN(const os::Mapping mapping, os::mapFileReadOnly(path));
    MappedFile file;
    file.m_data = mapping.data;
    file.m_size = mapping.size;
    file.m_file = mapping.file;
    file.m_mapping = mapping.mapping;
    file.m_open = true;
    return file;
}

void MappedFile::close() noexcept {
    if (!m_open) return;
    os::Mapping mapping;
    mapping.data = m_data;
    mapping.size = m_size;
    mapping.file = m_file;
    mapping.mapping = m_mapping;
    os::unmapFile(mapping);
    m_data = nullptr;
    m_size = 0;
    m_file = -1;
    m_mapping = -1;
    m_open = false;
}

// ---------------------------------------------------------------------------------------------
// FileWatcher
// ---------------------------------------------------------------------------------------------

std::string_view fileActionName(FileAction action) noexcept {
    switch (action) {
    case FileAction::Added: return "Added";
    case FileAction::Removed: return "Removed";
    case FileAction::Modified: return "Modified";
    case FileAction::RenamedOld: return "RenamedOld";
    case FileAction::RenamedNew: return "RenamedNew";
    case FileAction::Overflow: return "Overflow";
    }
    return "Unknown";
}

namespace {

/// Portable fallback: periodic scans compared by (size, mtime). Cannot see changes that keep both
/// size and timestamp identical within the filesystem's timestamp granularity.
class PollingWatcher final : public detail::WatcherBackend {
public:
    PollingWatcher(Path root, bool recursive, std::chrono::milliseconds interval)
        : m_root(std::move(root)), m_recursive(recursive),
          m_intervalNanos(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count())) {
        scan(m_snapshot);
        m_lastScan = monotonicNanos();
    }

    void poll(std::vector<FileEvent>& out) override {
        const u64 now = monotonicNanos();
        if (now - m_lastScan < m_intervalNanos) return;
        m_lastScan = now;
        Snapshot current;
        scan(current);
        std::vector<FileEvent> events;
        for (const auto& [path, info] : m_snapshot) {
            if (!current.contains(path)) events.push_back({FileAction::Removed, path});
        }
        for (const auto& [path, info] : current) {
            auto it = m_snapshot.find(path);
            if (it == m_snapshot.end()) {
                events.push_back({FileAction::Added, path});
            } else if (!info.isDirectory && (info.size != it->second.size || info.mtime != it->second.mtime)) {
                events.push_back({FileAction::Modified, path});
            }
        }
        std::sort(events.begin(), events.end(), [](const FileEvent& a, const FileEvent& b) {
            return a.path != b.path ? a.path < b.path : a.action < b.action;
        });
        out.insert(out.end(), events.begin(), events.end());
        m_snapshot = std::move(current);
    }

    bool isNative() const noexcept override { return false; }

private:
    struct Info {
        bool isDirectory = false;
        u64 size = 0;
        std::filesystem::file_time_type::rep mtime = 0;
    };
    using Snapshot = std::unordered_map<std::string, Info>;

    void scan(Snapshot& out) const {
        ListOptions options;
        options.recursive = m_recursive;
        auto entries = listDirectory(m_root, options);
        if (!entries) return;
        for (const DirEntry& e : *entries) {
            Info info;
            info.isDirectory = e.isDirectory;
            info.size = e.size;
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(e.path, ec);
            info.mtime = ec ? 0 : t.time_since_epoch().count();
            out.emplace(e.relativePath, info);
        }
    }

    Path m_root;
    bool m_recursive;
    u64 m_intervalNanos;
    u64 m_lastScan = 0;
    Snapshot m_snapshot;
};

} // namespace

FileWatcher::FileWatcher() = default;
FileWatcher::~FileWatcher() = default;
FileWatcher::FileWatcher(FileWatcher&&) noexcept = default;
FileWatcher& FileWatcher::operator=(FileWatcher&&) noexcept = default;

Result<void> FileWatcher::start(const Path& directory, bool recursive, WatchBackend backend,
                                std::chrono::milliseconds pollInterval) {
    stop();
    if (!isDirectory(directory)) {
        return makeError(ErrorCode::NotFound, "FileWatcher: '{}' is not a directory", pathToGenericUtf8(directory));
    }
    if (backend != WatchBackend::Polling) {
        auto native = os::createNativeWatcher(directory, recursive);
        if (native) {
            m_backend = std::move(native).value();
        } else if (backend == WatchBackend::Native) {
            return std::move(native).error();
        } else {
            HELIOS_LOG_WARN(LogFs, "Native file watching unavailable ({}); falling back to polling",
                            native.error().toString());
        }
    }
    if (!m_backend) m_backend = std::make_unique<PollingWatcher>(directory, recursive, pollInterval);
    m_directory = directory;
    return {};
}

void FileWatcher::stop() {
    m_backend.reset();
    m_directory.clear();
}

bool FileWatcher::isNative() const noexcept { return m_backend && m_backend->isNative(); }

std::vector<FileEvent> FileWatcher::poll() {
    std::vector<FileEvent> events;
    poll(events);
    return events;
}

usize FileWatcher::poll(std::vector<FileEvent>& out) {
    if (!m_backend) return 0;
    std::vector<FileEvent> raw;
    m_backend->poll(raw);
    // Coalesce duplicates (editors and inotify emit bursts of Modified for one save).
    std::unordered_set<std::string> seen;
    const usize before = out.size();
    for (FileEvent& e : raw) {
        std::string key;
        key.reserve(e.path.size() + 2);
        key.push_back(static_cast<char>('0' + static_cast<int>(e.action)));
        key.push_back('|');
        key.append(e.path);
        if (seen.insert(std::move(key)).second) out.push_back(std::move(e));
    }
    return out.size() - before;
}

} // namespace helios::fs
