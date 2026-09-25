#pragma once
// Virtual file system: engine code addresses content by virtual paths ("/content/ships/hauler.json",
// "/saved/settings.cfg") resolved through mount points backed by providers (native directories,
// in-memory files, later pak/chunk containers via IMountProvider).
//
// * Several providers may be mounted at the same (or nested) mount points; the one with the
//   highest priority wins (ties: the most recent mount). Listing merges all layers.
// * Paths are normalized ('\' -> '/', '.' and '..' resolved, duplicate '/' removed) and sandboxed:
//   a path that would climb above "/" is rejected, as are ':' and NUL (drive letters, ADS).
//   Virtual paths are case-sensitive on every platform.
// * Writes go to the highest-priority writable mount covering the path.
//
// Threading: Vfs methods are thread-safe (mount table guarded by a reader/writer lock; providers
// are called outside the lock and must be thread-safe for concurrent reads). pollWatches() and the
// watch callbacks run on the calling thread; call it from one thread (usually the main loop).

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::fs {

/// Conventional engine mount points.
inline constexpr std::string_view kContentMount = "/content"; ///< Read-only game content (layered: base, DLC, mods, paks).
inline constexpr std::string_view kCacheMount = "/cache";     ///< Derived data / cooked cache (writable, disposable).
inline constexpr std::string_view kSavedMount = "/saved";     ///< User settings, saves, logs (writable, persistent).

struct VfsEntry {
    std::string path; ///< Full virtual path (from Vfs::list) or mount-relative path (from providers).
    bool isDirectory = false;
    u64 size = 0;
};

/// A mounted content source. Relative paths passed in are already normalized: '/'-separated, no
/// leading '/', no '.'/'..' components; "" denotes the mount root. Implementations must be
/// thread-safe for concurrent reads.
class IMountProvider {
public:
    virtual ~IMountProvider() = default;
    virtual Result<std::vector<u8>> readFile(std::string_view relPath) = 0;
    virtual bool exists(std::string_view relPath) = 0;
    virtual bool isDirectory(std::string_view relPath) = 0;
    /// Appends entries under relDir with paths relative to the mount root.
    virtual Result<void> list(std::string_view relDir, bool recursive, std::vector<VfsEntry>& out) = 0;
    virtual Result<void> writeFile(std::string_view relPath, std::span<const u8> data) {
        (void)relPath;
        (void)data;
        return Error{ErrorCode::Unsupported, "mount is read-only"};
    }
    virtual bool isReadOnly() const noexcept { return true; }
    /// Native directory backing the mount root, if any (enables watching).
    virtual std::optional<Path> nativeRoot() const { return std::nullopt; }
    virtual std::string describe() const = 0;
};

/// Mount backed by a native directory. Besides '..'/':' escapes it rejects path components that
/// Win32 does not treat as plain file names (DOS devices such as "con"/"nul.json", names ending
/// in '.' or ' '), on every platform, so content resolves identically on Windows and Linux.
class DirectoryMount final : public IMountProvider {
public:
    explicit DirectoryMount(Path root, bool readOnly = false);
    Result<std::vector<u8>> readFile(std::string_view relPath) override;
    bool exists(std::string_view relPath) override;
    bool isDirectory(std::string_view relPath) override;
    Result<void> list(std::string_view relDir, bool recursive, std::vector<VfsEntry>& out) override;
    Result<void> writeFile(std::string_view relPath, std::span<const u8> data) override;
    bool isReadOnly() const noexcept override { return m_readOnly; }
    std::optional<Path> nativeRoot() const override { return m_root; }
    std::string describe() const override;

private:
    Result<Path> resolve(std::string_view relPath) const;
    Path m_root;
    bool m_readOnly;
};

/// Mount holding files in memory (tests, generated content, patches). Writable.
class MemoryMount final : public IMountProvider {
public:
    void addFile(std::string_view relPath, std::vector<u8> data);
    void addFile(std::string_view relPath, std::string_view text);
    bool removeFile(std::string_view relPath);

    Result<std::vector<u8>> readFile(std::string_view relPath) override;
    bool exists(std::string_view relPath) override;
    bool isDirectory(std::string_view relPath) override;
    Result<void> list(std::string_view relDir, bool recursive, std::vector<VfsEntry>& out) override;
    Result<void> writeFile(std::string_view relPath, std::span<const u8> data) override;
    bool isReadOnly() const noexcept override { return false; }
    std::string describe() const override { return "memory"; }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<u8>, std::less<>> m_files;
};

using MountId = u32;

struct MountInfo {
    MountId id = 0;
    std::string mountPoint;
    i32 priority = 0;
    bool readOnly = true;
    std::string description;
};

struct VfsEvent {
    FileAction action = FileAction::Modified;
    std::string path; ///< Virtual path (for Overflow: the watched directory).
};

class Vfs {
public:
    using WatchId = u32;
    using WatchCallback = std::function<void(const VfsEvent&)>;

    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    /// Normalizes an absolute virtual path (see file comment). Errors: InvalidArgument (relative,
    /// illegal characters) or PermissionDenied (escapes the root).
    static Result<std::string> normalizePath(std::string_view path);

    Result<MountId> mount(std::string_view mountPoint, std::unique_ptr<IMountProvider> provider, i32 priority = 0);
    Result<MountId> mountDirectory(std::string_view mountPoint, const Path& directory, i32 priority = 0,
                                   bool readOnly = false);
    bool unmount(MountId id);
    std::vector<MountInfo> mounts() const;

    Result<std::vector<u8>> readFile(std::string_view path) const;
    Result<std::string> readTextFile(std::string_view path) const;
    Result<void> writeFile(std::string_view path, std::span<const u8> data);
    Result<void> writeTextFile(std::string_view path, std::string_view text);
    bool exists(std::string_view path) const;
    bool isDirectory(std::string_view path) const;
    /// Merged listing (highest priority wins per path), sorted by path. Mount points below `dir`
    /// appear as directories; a recursive listing also includes the contents of those mounts.
    Result<std::vector<VfsEntry>> list(std::string_view dir, bool recursive = false) const;
    /// Native file path the virtual path currently resolves to (tools/debugging only).
    Result<Path> resolveNativePath(std::string_view path) const;

    /// Watches a virtual directory across all native-directory mounts overlapping it. Callbacks run
    /// inside pollWatches(). Mounts added after watch() are not covered.
    Result<WatchId> watch(std::string_view dir, WatchCallback callback);
    void unwatch(WatchId id);
    /// Polls all watchers and dispatches callbacks; returns the number of events delivered.
    usize pollWatches();

private:
    struct Mount;
    struct Watch;
    struct Candidate;
    std::vector<Candidate> candidates(std::string_view normalizedPath) const;

    mutable std::shared_mutex m_mutex;
    std::vector<std::shared_ptr<Mount>> m_mounts;
    MountId m_nextMountId = 1;

    std::mutex m_watchMutex;
    std::vector<std::unique_ptr<Watch>> m_watches;
    WatchId m_nextWatchId = 1;
};

} // namespace helios::fs
