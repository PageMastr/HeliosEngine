#pragma once
// File system: whole-file read/write (atomic replace by default), File handles with positional
// reads, read-only memory mapping, directory listing and a FileWatcher that reports changes on
// poll() (ReadDirectoryChangesW on Windows, inotify on Linux, polling fallback everywhere).
//
// Paths: std::filesystem::path. Convert UTF-8 text with pathFromUtf8()/pathToUtf8() — never
// construct a path from a narrow std::string on Windows (that uses the ANSI code page).
//
// Threading: free functions are thread-safe (they only touch the OS). File and MappedFile objects
// may be used from one thread at a time, except File::readAt() and MappedFile::data(), which are
// safe concurrently.
// FileWatcher must be polled from a single thread.

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::fs {

using Path = std::filesystem::path;

/// UTF-8 text -> path (UTF-16 on Windows).
Path pathFromUtf8(std::string_view utf8);
/// path -> UTF-8 using the native separators.
std::string pathToUtf8(const Path& path);
/// path -> UTF-8 with '/' separators (stable for logs, keys and VFS mapping).
std::string pathToGenericUtf8(const Path& path);

Result<std::vector<u8>> readFile(const Path& path);
Result<std::string> readTextFile(const Path& path);

enum class WriteMode : u8 {
    Atomic, ///< Write a sibling temp file, flush it to disk, then atomically replace the target.
    Direct, ///< Truncate and write in place (faster, not crash-safe).
};

/// Writes `data` to `path`, creating parent directories is NOT done implicitly.
Result<void> writeFile(const Path& path, std::span<const u8> data, WriteMode mode = WriteMode::Atomic);
Result<void> writeTextFile(const Path& path, std::string_view text, WriteMode mode = WriteMode::Atomic);

bool exists(const Path& path) noexcept;
bool isFile(const Path& path) noexcept;
bool isDirectory(const Path& path) noexcept;
Result<u64> fileSize(const Path& path);
Result<std::filesystem::file_time_type> lastWriteTime(const Path& path);
Result<void> createDirectories(const Path& path);
/// Removes a file or an empty directory. Removing a missing path is an error (NotFound).
Result<void> remove(const Path& path);
/// Recursively removes; returns the number of entries removed (0 if missing).
Result<u64> removeAll(const Path& path);
/// Renames, atomically replacing an existing target file.
Result<void> rename(const Path& from, const Path& to);
/// Absolute path of the running executable.
Result<Path> executablePath();
/// Creates a new uniquely named directory under the system temp directory.
Result<Path> createUniqueTempDirectory(std::string_view prefix = "helios");

struct DirEntry {
    Path path;               ///< Full path.
    std::string relativePath; ///< UTF-8, '/'-separated, relative to the listed directory.
    bool isDirectory = false;
    u64 size = 0;            ///< File size (0 for directories).
};

struct ListOptions {
    bool recursive = false;
    bool includeFiles = true;
    bool includeDirectories = true;
    std::string extension; ///< If non-empty (e.g. ".json"), only files with this extension (case-insensitive).
};

/// Lists a directory; entries are sorted by relativePath for determinism.
Result<std::vector<DirEntry>> listDirectory(const Path& dir, const ListOptions& options = {});

enum class OpenMode : u8 {
    Read,      ///< Existing file, read-only.
    Write,     ///< Create or truncate, write-only.
    ReadWrite, ///< Create if missing, read + write, no truncation.
    Append,    ///< Create if missing, writes go to the end.
};

enum class SeekOrigin : u8 { Begin, Current, End };

/// Unbuffered OS file handle (CreateFileW / open). Move-only; closes on destruction.
class File {
public:
    File() noexcept = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    static Result<File> open(const Path& path, OpenMode mode);

    bool isOpen() const noexcept;
    void close() noexcept;

    /// Reads up to `size` bytes at the current position; returns bytes read (0 at end of file).
    Result<usize> read(void* buffer, usize size);
    /// Reads exactly `size` bytes or fails with EndOfFile.
    Result<void> readExact(void* buffer, usize size);
    /// Positional read (pread / ReadFile with an OVERLAPPED offset). Safe to call concurrently from
    /// several threads on the same File. Do not mix with read()/seek() concurrently: on Windows the
    /// file position ends up after the bytes read.
    Result<usize> readAt(u64 offset, void* buffer, usize size) const;
    /// Writes all bytes (loops over partial writes).
    Result<void> write(const void* data, usize size);
    Result<u64> seek(i64 offset, SeekOrigin origin = SeekOrigin::Begin);
    Result<u64> tell();
    Result<u64> size() const;
    /// Flushes OS buffers to the storage device (fsync / FlushFileBuffers).
    Result<void> sync();

    const Path& path() const noexcept { return m_path; }

private:
    std::intptr_t m_handle = -1;
    Path m_path;
};

/// Read-only memory mapping of a whole file (CreateFileMappingW / mmap). Empty files map to an
/// empty span. The mapping stays valid until close()/destruction even if the file is replaced.
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile();
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    static Result<MappedFile> open(const Path& path);

    bool isOpen() const noexcept { return m_open; }
    void close() noexcept;
    const u8* data() const noexcept { return static_cast<const u8*>(m_data); }
    usize size() const noexcept { return m_size; }
    std::span<const u8> bytes() const noexcept { return {data(), m_size}; }
    std::string_view text() const noexcept { return {static_cast<const char*>(m_data), m_size}; }

private:
    const void* m_data = nullptr;
    usize m_size = 0;
    std::intptr_t m_file = -1;
    std::intptr_t m_mapping = -1;
    bool m_open = false;
};

enum class FileAction : u8 {
    Added,
    Removed,
    Modified,
    RenamedOld, ///< Path before a rename (treat like Removed).
    RenamedNew, ///< Path after a rename (treat like Added; atomic saves arrive as this).
    Overflow,   ///< Events were lost (OS buffer overflow); rescan the directory. Path is empty.
};

std::string_view fileActionName(FileAction action) noexcept;

struct FileEvent {
    FileAction action = FileAction::Modified;
    std::string path; ///< UTF-8, '/'-separated, relative to the watched directory.
    friend bool operator==(const FileEvent&, const FileEvent&) = default;
};

enum class WatchBackend : u8 {
    Auto,    ///< Native if available, else polling.
    Native,  ///< Fail if the OS backend is unavailable.
    Polling, ///< Periodic directory scans (size + mtime comparison).
};

namespace detail {
/// Implementation interface for FileWatcher backends (internal).
class WatcherBackend {
public:
    virtual ~WatcherBackend() = default;
    /// Appends pending events; must not block.
    virtual void poll(std::vector<FileEvent>& out) = 0;
    virtual bool isNative() const noexcept = 0;
};
} // namespace detail

/// Watches a directory tree and reports changes when polled. Consecutive duplicate events for
/// the same path within one poll are coalesced. Not thread-safe: poll from one thread.
class FileWatcher {
public:
    FileWatcher();
    ~FileWatcher();
    FileWatcher(FileWatcher&&) noexcept;
    FileWatcher& operator=(FileWatcher&&) noexcept;
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /// Starts watching `directory` (must exist). `pollInterval` only applies to the polling backend
    /// (0 = rescan on every poll()).
    Result<void> start(const Path& directory, bool recursive = true, WatchBackend backend = WatchBackend::Auto,
                       std::chrono::milliseconds pollInterval = std::chrono::milliseconds(250));
    void stop();
    bool isWatching() const noexcept { return m_backend != nullptr; }
    /// True if the OS backend is active (false for polling or when stopped).
    bool isNative() const noexcept;
    const Path& directory() const noexcept { return m_directory; }

    /// Non-blocking; returns events that happened since the previous poll.
    std::vector<FileEvent> poll();
    /// Appends events to `out`; returns the number appended.
    usize poll(std::vector<FileEvent>& out);

private:
    std::unique_ptr<detail::WatcherBackend> m_backend;
    Path m_directory;
};

} // namespace helios::fs
