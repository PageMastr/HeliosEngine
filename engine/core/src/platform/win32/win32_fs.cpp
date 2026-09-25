// Win32 file I/O, memory mapping, atomic replace (ReplaceFileW / MoveFileExW) and the
// ReadDirectoryChangesW directory watcher.

#include "platform/win32/win32_common.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <vector>

#include "platform/os.h"

namespace helios::os {

namespace {

std::string displayPath(const std::filesystem::path& p) { return fs::pathToGenericUtf8(p); }

// Win32 I/O calls take DWORD sizes; stay well below 4 GiB per call.
constexpr usize kMaxIoChunk = 0x40000000;

} // namespace

Result<NativeFile> fileOpen(const std::filesystem::path& path, fs::OpenMode mode) {
    DWORD access = 0;
    DWORD disposition = 0;
    switch (mode) {
    case fs::OpenMode::Read:
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case fs::OpenMode::Write:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case fs::OpenMode::ReadWrite:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    case fs::OpenMode::Append:
        // FILE_APPEND_DATA without FILE_WRITE_DATA: every write goes to the end of the file.
        access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        disposition = OPEN_ALWAYS;
        break;
    }
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return lastError(std::format("open '{}'", displayPath(path)));
    return win32::fromHandle(h);
}

void fileClose(NativeFile file) noexcept { CloseHandle(win32::toHandle(file)); }

Result<usize> fileRead(NativeFile file, void* buffer, usize size) {
    DWORD read = 0;
    const DWORD chunk = static_cast<DWORD>(std::min(size, kMaxIoChunk));
    if (!ReadFile(win32::toHandle(file), buffer, chunk, &read, nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_BROKEN_PIPE) return usize(0);
        return win32::makeWin32Error(code, "ReadFile");
    }
    return static_cast<usize>(read);
}

Result<usize> fileReadAt(NativeFile file, u64 offset, void* buffer, usize size) {
    // Synchronous handle + OVERLAPPED offset: positional read (the file pointer ends up after it).
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read = 0;
    const DWORD chunk = static_cast<DWORD>(std::min(size, kMaxIoChunk));
    if (!ReadFile(win32::toHandle(file), buffer, chunk, &read, &ov)) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF) return usize(0);
        return win32::makeWin32Error(code, "ReadFile(offset)");
    }
    return static_cast<usize>(read);
}

Result<usize> fileWrite(NativeFile file, const void* data, usize size) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min(size, kMaxIoChunk));
    if (!WriteFile(win32::toHandle(file), data, chunk, &written, nullptr)) return lastError("WriteFile");
    return static_cast<usize>(written);
}

Result<u64> fileSeek(NativeFile file, i64 offset, fs::SeekOrigin origin) {
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    LARGE_INTEGER position;
    const DWORD method = origin == fs::SeekOrigin::Begin     ? FILE_BEGIN
                         : origin == fs::SeekOrigin::Current ? FILE_CURRENT
                                                             : FILE_END;
    if (!SetFilePointerEx(win32::toHandle(file), distance, &position, method)) return lastError("SetFilePointerEx");
    return static_cast<u64>(position.QuadPart);
}

Result<u64> fileSize(NativeFile file) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(win32::toHandle(file), &size)) return lastError("GetFileSizeEx");
    return static_cast<u64>(size.QuadPart);
}

Result<void> fileSync(NativeFile file) {
    if (!FlushFileBuffers(win32::toHandle(file))) return lastError("FlushFileBuffers");
    return {};
}

Result<void> atomicReplace(const std::filesystem::path& source, const std::filesystem::path& target) {
    // ReplaceFileW keeps the target's identity/attributes and is the documented way to swap in a
    // freshly written file; it requires an existing target, so fall back to MoveFileExW.
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (ReplaceFileW(target.c_str(), source.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            return {};
        }
    }
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return lastError(std::format("replace '{}' with '{}'", displayPath(target), displayPath(source)));
    }
    return {};
}

Result<Mapping> mapFileReadOnly(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return lastError(std::format("open '{}'", displayPath(path)));
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        Error e = lastError("GetFileSizeEx");
        CloseHandle(file);
        return e;
    }
    Mapping mapping;
    mapping.size = static_cast<usize>(size.QuadPart);
    if (mapping.size == 0) { // CreateFileMapping rejects empty files
        CloseHandle(file);
        return mapping;
    }
    HANDLE section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!section) {
        Error e = lastError(std::format("CreateFileMapping '{}'", displayPath(path)));
        CloseHandle(file);
        return e;
    }
    const void* view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        Error e = lastError(std::format("MapViewOfFile '{}'", displayPath(path)));
        CloseHandle(section);
        CloseHandle(file);
        return e;
    }
    // The view keeps the section and file alive; the handles are not needed any more.
    CloseHandle(section);
    CloseHandle(file);
    mapping.data = view;
    return mapping;
}

void unmapFile(Mapping& mapping) noexcept {
    if (mapping.data) UnmapViewOfFile(mapping.data);
    mapping = Mapping{};
}

Result<std::filesystem::path> executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) return lastError("GetModuleFileNameW");
        if (len < buffer.size()) {
            buffer.resize(len);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

// ---- ReadDirectoryChangesW watcher ------------------------------------------------------------

namespace {

class Win32Watcher final : public fs::detail::WatcherBackend {
public:
    Win32Watcher(HANDLE dir, HANDLE event, std::filesystem::path root, bool recursive)
        : m_dir(dir), m_event(event), m_root(std::move(root)), m_recursive(recursive), m_buffer(64 * 1024 / sizeof(DWORD)) {}

    ~Win32Watcher() override {
        if (m_pending) {
            CancelIoEx(m_dir, &m_overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(m_dir, &m_overlapped, &bytes, TRUE); // wait for the cancellation
        }
        CloseHandle(m_event);
        CloseHandle(m_dir);
    }

    bool issue() {
        std::memset(&m_overlapped, 0, sizeof(m_overlapped));
        m_overlapped.hEvent = m_event;
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                             FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
        m_pending = ReadDirectoryChangesW(m_dir, m_buffer.data(), static_cast<DWORD>(m_buffer.size() * sizeof(DWORD)),
                                          m_recursive ? TRUE : FALSE, filter, nullptr, &m_overlapped, nullptr) != 0;
        return m_pending;
    }

    void poll(std::vector<fs::FileEvent>& out) override {
        while (m_pending) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(m_dir, &m_overlapped, &bytes, FALSE)) {
                const DWORD code = GetLastError();
                if (code == ERROR_IO_INCOMPLETE) return; // nothing new yet
                m_pending = false;
                out.push_back({fs::FileAction::Overflow, {}});
                issue();
                return;
            }
            if (bytes == 0) {
                out.push_back({fs::FileAction::Overflow, {}}); // buffer overflow: changes were lost
            } else {
                parse(bytes, out);
            }
            issue();
        }
    }

    bool isNative() const noexcept override { return true; }

private:
    void parse(DWORD bytes, std::vector<fs::FileEvent>& out) {
        const auto* base = reinterpret_cast<const std::byte*>(m_buffer.data());
        usize offset = 0;
        for (;;) {
            FILE_NOTIFY_INFORMATION info;
            std::memcpy(&info, base + offset, sizeof(info) - sizeof(info.FileName));
            const auto* name = reinterpret_cast<const wchar_t*>(base + offset + offsetof(FILE_NOTIFY_INFORMATION, FileName));
            std::string rel = win32::narrow(std::wstring_view(name, info.FileNameLength / sizeof(wchar_t)));
            std::replace(rel.begin(), rel.end(), '\\', '/');
            fs::FileAction action = fs::FileAction::Modified;
            bool emit = true;
            switch (info.Action) {
            case FILE_ACTION_ADDED: action = fs::FileAction::Added; break;
            case FILE_ACTION_REMOVED: action = fs::FileAction::Removed; break;
            case FILE_ACTION_RENAMED_OLD_NAME: action = fs::FileAction::RenamedOld; break;
            case FILE_ACTION_RENAMED_NEW_NAME: action = fs::FileAction::RenamedNew; break;
            case FILE_ACTION_MODIFIED: {
                // Directories report "modified" whenever their contents change; skip those.
                const DWORD attrs = GetFileAttributesW((m_root / fs::pathFromUtf8(rel)).c_str());
                emit = attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
                break;
            }
            default: emit = false; break;
            }
            if (emit) out.push_back({action, std::move(rel)});
            if (info.NextEntryOffset == 0 || offset + info.NextEntryOffset >= bytes) break;
            offset += info.NextEntryOffset;
        }
    }

    HANDLE m_dir;
    HANDLE m_event;
    std::filesystem::path m_root;
    bool m_recursive;
    bool m_pending = false;
    OVERLAPPED m_overlapped{};
    std::vector<DWORD> m_buffer; // DWORD-aligned as ReadDirectoryChangesW requires
};

} // namespace

Result<std::unique_ptr<fs::detail::WatcherBackend>> createNativeWatcher(const std::filesystem::path& dir,
                                                                        bool recursive) {
    HANDLE handle = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return lastError(std::format("open directory '{}'", displayPath(dir)));
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        Error e = lastError("CreateEventW");
        CloseHandle(handle);
        return e;
    }
    auto watcher = std::make_unique<Win32Watcher>(handle, event, dir, recursive);
    if (!watcher->issue()) return lastError(std::format("ReadDirectoryChangesW '{}'", displayPath(dir)));
    return std::unique_ptr<fs::detail::WatcherBackend>(std::move(watcher));
}

} // namespace helios::os
