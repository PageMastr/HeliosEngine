// POSIX file I/O, memory mapping, atomic replace and the inotify directory watcher.

#include <cerrno>
#include <climits>
#include <cstring>
#include <unordered_map>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#endif

#include "platform/os.h"

namespace helios::os {

namespace {
int fdOf(NativeFile file) noexcept { return static_cast<int>(file); }
} // namespace

Result<NativeFile> fileOpen(const std::filesystem::path& path, fs::OpenMode mode) {
    int flags = O_CLOEXEC;
    switch (mode) {
    case fs::OpenMode::Read: flags |= O_RDONLY; break;
    case fs::OpenMode::Write: flags |= O_WRONLY | O_CREAT | O_TRUNC; break;
    case fs::OpenMode::ReadWrite: flags |= O_RDWR | O_CREAT; break;
    case fs::OpenMode::Append: flags |= O_WRONLY | O_CREAT | O_APPEND; break;
    }
    int fd;
    do {
        fd = ::open(path.c_str(), flags, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return lastError(std::format("open '{}'", path.string()));
    struct stat st{};
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        ::close(fd);
        return makeError(ErrorCode::InvalidArgument, "open '{}': is a directory", path.string());
    }
    return static_cast<NativeFile>(fd);
}

void fileClose(NativeFile file) noexcept { ::close(fdOf(file)); }

Result<usize> fileRead(NativeFile file, void* buffer, usize size) {
    for (;;) {
        const ssize_t n = ::read(fdOf(file), buffer, size);
        if (n >= 0) return static_cast<usize>(n);
        if (errno != EINTR) return lastError("read");
    }
}

Result<usize> fileReadAt(NativeFile file, u64 offset, void* buffer, usize size) {
    for (;;) {
        const ssize_t n = ::pread(fdOf(file), buffer, size, static_cast<off_t>(offset));
        if (n >= 0) return static_cast<usize>(n);
        if (errno != EINTR) return lastError("pread");
    }
}

Result<usize> fileWrite(NativeFile file, const void* data, usize size) {
    for (;;) {
        const ssize_t n = ::write(fdOf(file), data, size);
        if (n >= 0) return static_cast<usize>(n);
        if (errno != EINTR) return lastError("write");
    }
}

Result<u64> fileSeek(NativeFile file, i64 offset, fs::SeekOrigin origin) {
    const int whence = origin == fs::SeekOrigin::Begin ? SEEK_SET : origin == fs::SeekOrigin::Current ? SEEK_CUR : SEEK_END;
    const off_t pos = ::lseek(fdOf(file), static_cast<off_t>(offset), whence);
    if (pos < 0) return lastError("lseek");
    return static_cast<u64>(pos);
}

Result<u64> fileSize(NativeFile file) {
    struct stat st{};
    if (fstat(fdOf(file), &st) != 0) return lastError("fstat");
    return static_cast<u64>(st.st_size);
}

Result<void> fileSync(NativeFile file) {
    if (::fsync(fdOf(file)) != 0) return lastError("fsync");
    return {};
}

Result<void> atomicReplace(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (::rename(source.c_str(), target.c_str()) != 0) {
        return lastError(std::format("rename '{}' -> '{}'", source.string(), target.string()));
    }
    // Persist the directory entry too (best effort) so the replacement survives power loss.
    const std::filesystem::path parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd >= 0) {
        (void)::fsync(dirFd);
        ::close(dirFd);
    }
    return {};
}

Result<Mapping> mapFileReadOnly(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return lastError(std::format("open '{}'", path.string()));
    struct stat st{};
    if (fstat(fd, &st) != 0) {
        Error e = lastError("fstat");
        ::close(fd);
        return e;
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        return makeError(ErrorCode::InvalidArgument, "map '{}': is a directory", path.string());
    }
    Mapping mapping;
    mapping.size = static_cast<usize>(st.st_size);
    if (mapping.size > 0) {
        void* p = mmap(nullptr, mapping.size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            Error e = lastError(std::format("mmap '{}'", path.string()));
            ::close(fd);
            return e;
        }
        mapping.data = p;
    }
    ::close(fd); // the mapping keeps its own reference to the file
    return mapping;
}

void unmapFile(Mapping& mapping) noexcept {
    if (mapping.data && mapping.size > 0) munmap(const_cast<void*>(mapping.data), mapping.size);
    mapping = Mapping{};
}

Result<std::filesystem::path> executablePath() {
#if defined(__linux__)
    char buffer[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n < 0) return lastError("readlink /proc/self/exe");
    return std::filesystem::path(std::string(buffer, static_cast<usize>(n)));
#else
    return Error{ErrorCode::Unsupported, "executablePath is not implemented on this platform"};
#endif
}

// ---- inotify watcher --------------------------------------------------------------------------

#if defined(__linux__)
namespace {

class InotifyWatcher final : public fs::detail::WatcherBackend {
public:
    InotifyWatcher(int fd, std::filesystem::path root, bool recursive)
        : m_fd(fd), m_root(std::move(root)), m_recursive(recursive), m_buffer(64 * 1024) {}
    ~InotifyWatcher() override { ::close(m_fd); }

    bool addWatch(const std::string& rel) {
        const std::filesystem::path dir = rel.empty() ? m_root : m_root / fs::pathFromUtf8(rel);
        const u32 mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                         IN_DELETE_SELF | IN_EXCL_UNLINK | IN_ONLYDIR;
        const int wd = inotify_add_watch(m_fd, dir.c_str(), mask);
        if (wd < 0) return false;
        m_dirs[wd] = rel;
        return true;
    }

    /// Adds watches for `rel` and its subdirectories; optionally reports their existing contents
    /// (entries created before the watch existed would otherwise be missed). Returns false (errno
    /// set) if `rel` itself could not be watched.
    bool addTree(const std::string& rel, std::vector<fs::FileEvent>* reportExisting) {
        if (!addWatch(rel)) return false;
        const std::filesystem::path dir = rel.empty() ? m_root : m_root / fs::pathFromUtf8(rel);
        fs::ListOptions options;
        options.recursive = true;
        auto entries = fs::listDirectory(dir, options);
        if (!entries) return true;
        for (const fs::DirEntry& e : *entries) {
            const std::string child = rel.empty() ? e.relativePath : rel + "/" + e.relativePath;
            if (e.isDirectory) addWatch(child);
            if (reportExisting) reportExisting->push_back({fs::FileAction::Added, child});
        }
        return true;
    }

    void poll(std::vector<fs::FileEvent>& out) override {
        for (;;) {
            const ssize_t n = ::read(m_fd, m_buffer.data(), m_buffer.size());
            if (n <= 0) return; // EAGAIN: nothing pending
            usize offset = 0;
            while (offset + sizeof(inotify_event) <= static_cast<usize>(n)) {
                inotify_event ev;
                std::memcpy(&ev, m_buffer.data() + offset, sizeof(ev));
                const char* namePtr = m_buffer.data() + offset + sizeof(inotify_event);
                offset += sizeof(inotify_event) + ev.len;
                handle(ev, ev.len ? std::string(namePtr, ::strnlen(namePtr, ev.len)) : std::string(), out);
            }
        }
    }

    bool isNative() const noexcept override { return true; }

private:
    void handle(const inotify_event& ev, const std::string& name, std::vector<fs::FileEvent>& out) {
        if (ev.mask & IN_Q_OVERFLOW) {
            out.push_back({fs::FileAction::Overflow, {}});
            return;
        }
        if (ev.mask & IN_IGNORED) {
            m_dirs.erase(ev.wd);
            return;
        }
        auto it = m_dirs.find(ev.wd);
        if (it == m_dirs.end()) return;
        if (ev.mask & IN_DELETE_SELF) return; // parent reports the deletion
        const std::string path = it->second.empty() ? name : (name.empty() ? it->second : it->second + "/" + name);
        const bool isDir = (ev.mask & IN_ISDIR) != 0;
        if (ev.mask & IN_CREATE) {
            out.push_back({fs::FileAction::Added, path});
            if (isDir && m_recursive) addTree(path, &out);
        } else if (ev.mask & IN_DELETE) {
            out.push_back({fs::FileAction::Removed, path});
        } else if (ev.mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
            if (!isDir) out.push_back({fs::FileAction::Modified, path});
        } else if (ev.mask & IN_MOVED_FROM) {
            out.push_back({fs::FileAction::RenamedOld, path});
        } else if (ev.mask & IN_MOVED_TO) {
            out.push_back({fs::FileAction::RenamedNew, path});
            if (isDir && m_recursive) addTree(path, &out);
        }
    }

    int m_fd;
    std::filesystem::path m_root;
    bool m_recursive;
    std::vector<char> m_buffer;
    std::unordered_map<int, std::string> m_dirs; // watch descriptor -> relative dir
};

} // namespace
#endif

Result<std::unique_ptr<fs::detail::WatcherBackend>> createNativeWatcher(const std::filesystem::path& dir,
                                                                        bool recursive) {
#if defined(__linux__)
    const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return lastError("inotify_init1");
    auto watcher = std::make_unique<InotifyWatcher>(fd, dir, recursive);
    // A failed root watch (e.g. fs.inotify.max_user_watches exhausted) must be an error, so that
    // WatchBackend::Auto falls back to polling instead of silently reporting nothing.
    const bool watching = recursive ? watcher->addTree("", nullptr) : watcher->addWatch("");
    if (!watching) return lastError(std::format("inotify_add_watch '{}'", dir.string()));
    return std::unique_ptr<fs::detail::WatcherBackend>(std::move(watcher));
#else
    (void)dir;
    (void)recursive;
    return Error{ErrorCode::Unsupported, "no native file watcher on this platform"};
#endif
}

} // namespace helios::os
