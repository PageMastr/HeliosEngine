#include "helios/core/vfs.h"

#include <algorithm>

#include "helios/core/log.h"

namespace helios::fs {

namespace {

/// Mount-relative path of `path` under `mountPoint`, if it is inside it.
std::optional<std::string_view> relativeTo(std::string_view mountPoint, std::string_view path) noexcept {
    if (mountPoint == "/") return path.substr(1);
    if (path == mountPoint) return std::string_view();
    if (path.size() > mountPoint.size() && path.starts_with(mountPoint) && path[mountPoint.size()] == '/') {
        return path.substr(mountPoint.size() + 1);
    }
    return std::nullopt;
}

std::string joinVirtual(std::string_view base, std::string_view rel) {
    if (rel.empty()) return std::string(base);
    std::string out(base);
    if (out.empty() || out.back() != '/') out.push_back('/');
    out.append(rel);
    return out;
}

/// Validates a provider-relative path (defense in depth for providers used without a Vfs).
bool isSafeRelative(std::string_view rel) noexcept {
    if (rel.empty()) return true;
    if (rel.front() == '/' || rel.back() == '/') return false;
    usize start = 0;
    while (start <= rel.size()) {
        const usize end = std::min(rel.find('/', start), rel.size());
        const std::string_view part = rel.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    for (const char c : rel) {
        if (c == '\\' || c == ':' || c == '\0') return false;
    }
    return true;
}

Error unsafePath(std::string_view rel) {
    return makeError(ErrorCode::InvalidArgument, "unsafe mount-relative path '{}'", rel);
}

bool iequalsAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        if (x != b[i]) return false; // b is lowercase
    }
    return true;
}

/// Components that Win32 maps to something other than a file with that name: DOS device names
/// (CON, NUL, COM1, ... also with an extension, e.g. "nul.json") and names ending in '.' or ' '
/// (silently stripped, so "a." aliases "a"). Rejected on every platform so a native-directory mount
/// resolves identically on Windows and Linux and never opens a device (reading "con" would block
/// on console input).
bool isNonPortableComponent(std::string_view part) noexcept {
    if (part.empty()) return false;
    if (part.back() == '.' || part.back() == ' ') return true;
    std::string_view stem = part.substr(0, part.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    if (stem.size() == 3) {
        return iequalsAscii(stem, "con") || iequalsAscii(stem, "prn") || iequalsAscii(stem, "aux") ||
               iequalsAscii(stem, "nul");
    }
    // COM0-9 / LPT0-9 and the superscript forms COM¹²³ / LPT¹²³ (UTF-8 C2 B9, C2 B2, C2 B3).
    const bool digitSuffix = stem.size() == 4 && stem[3] >= '0' && stem[3] <= '9';
    const bool superscriptSuffix = stem.size() == 5 && static_cast<u8>(stem[3]) == 0xC2 &&
                                   (static_cast<u8>(stem[4]) == 0xB9 || static_cast<u8>(stem[4]) == 0xB2 ||
                                    static_cast<u8>(stem[4]) == 0xB3);
    if (digitSuffix || superscriptSuffix) {
        return iequalsAscii(stem.substr(0, 3), "com") || iequalsAscii(stem.substr(0, 3), "lpt");
    }
    return iequalsAscii(stem, "conin$") || iequalsAscii(stem, "conout$");
}

bool hasNonPortableComponent(std::string_view rel) noexcept {
    usize start = 0;
    while (start < rel.size()) {
        const usize end = std::min(rel.find('/', start), rel.size());
        if (isNonPortableComponent(rel.substr(start, end - start))) return true;
        start = end + 1;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// DirectoryMount
// ---------------------------------------------------------------------------------------------

DirectoryMount::DirectoryMount(Path root, bool readOnly) : m_root(std::move(root)), m_readOnly(readOnly) {}

Result<Path> DirectoryMount::resolve(std::string_view relPath) const {
    if (!isSafeRelative(relPath) || hasNonPortableComponent(relPath)) return unsafePath(relPath);
    return relPath.empty() ? m_root : m_root / pathFromUtf8(relPath);
}

Result<std::vector<u8>> DirectoryMount::readFile(std::string_view relPath) {
    HELIOS_TRY_ASSIGN(const Path path, resolve(relPath));
    return fs::readFile(path);
}

bool DirectoryMount::exists(std::string_view relPath) {
    auto path = resolve(relPath);
    return path && fs::exists(*path);
}

bool DirectoryMount::isDirectory(std::string_view relPath) {
    auto path = resolve(relPath);
    return path && fs::isDirectory(*path);
}

Result<void> DirectoryMount::list(std::string_view relDir, bool recursive, std::vector<VfsEntry>& out) {
    HELIOS_TRY_ASSIGN(const Path dir, resolve(relDir));
    ListOptions options;
    options.recursive = recursive;
    HELIOS_TRY_ASSIGN(auto entries, listDirectory(dir, options));
    for (DirEntry& e : entries) {
        VfsEntry v;
        v.path = relDir.empty() ? std::move(e.relativePath) : joinVirtual(relDir, e.relativePath);
        v.isDirectory = e.isDirectory;
        v.size = e.size;
        out.push_back(std::move(v));
    }
    return {};
}

Result<void> DirectoryMount::writeFile(std::string_view relPath, std::span<const u8> data) {
    if (m_readOnly) return Error{ErrorCode::PermissionDenied, "mount is read-only: " + describe()};
    HELIOS_TRY_ASSIGN(const Path path, resolve(relPath));
    if (path.has_parent_path()) HELIOS_TRY(createDirectories(path.parent_path()));
    return fs::writeFile(path, data, WriteMode::Atomic);
}

std::string DirectoryMount::describe() const { return "dir:" + pathToGenericUtf8(m_root); }

// ---------------------------------------------------------------------------------------------
// MemoryMount
// ---------------------------------------------------------------------------------------------

void MemoryMount::addFile(std::string_view relPath, std::vector<u8> data) {
    std::lock_guard lock(m_mutex);
    m_files[std::string(relPath)] = std::move(data);
}

void MemoryMount::addFile(std::string_view relPath, std::string_view text) {
    addFile(relPath, std::vector<u8>(text.begin(), text.end()));
}

bool MemoryMount::removeFile(std::string_view relPath) {
    std::lock_guard lock(m_mutex);
    auto it = m_files.find(relPath);
    if (it == m_files.end()) return false;
    m_files.erase(it);
    return true;
}

Result<std::vector<u8>> MemoryMount::readFile(std::string_view relPath) {
    std::lock_guard lock(m_mutex);
    auto it = m_files.find(relPath);
    if (it == m_files.end()) return makeError(ErrorCode::NotFound, "memory file '{}' not found", relPath);
    return it->second;
}

bool MemoryMount::exists(std::string_view relPath) {
    std::lock_guard lock(m_mutex);
    if (m_files.find(relPath) != m_files.end()) return true;
    if (relPath.empty()) return true;
    const std::string prefix = std::string(relPath) + "/";
    auto it = m_files.lower_bound(prefix);
    return it != m_files.end() && it->first.starts_with(prefix);
}

bool MemoryMount::isDirectory(std::string_view relPath) {
    if (relPath.empty()) return true;
    std::lock_guard lock(m_mutex);
    const std::string prefix = std::string(relPath) + "/";
    auto it = m_files.lower_bound(prefix);
    return it != m_files.end() && it->first.starts_with(prefix);
}

Result<void> MemoryMount::list(std::string_view relDir, bool recursive, std::vector<VfsEntry>& out) {
    std::lock_guard lock(m_mutex);
    const std::string prefix = relDir.empty() ? std::string() : std::string(relDir) + "/";
    std::vector<std::string> dirs;
    bool any = relDir.empty();
    for (auto it = m_files.lower_bound(prefix); it != m_files.end() && it->first.starts_with(prefix); ++it) {
        any = true;
        const std::string_view rest = std::string_view(it->first).substr(prefix.size());
        usize slash = rest.find('/');
        if (!recursive && slash != std::string_view::npos) {
            dirs.push_back(prefix + std::string(rest.substr(0, slash)));
            continue;
        }
        while (slash != std::string_view::npos) { // intermediate directories (recursive)
            dirs.push_back(prefix + std::string(rest.substr(0, slash)));
            slash = rest.find('/', slash + 1);
        }
        out.push_back(VfsEntry{it->first, false, static_cast<u64>(it->second.size())});
    }
    if (!any) return makeError(ErrorCode::NotFound, "memory directory '{}' not found", relDir);
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
    for (std::string& d : dirs) out.push_back(VfsEntry{std::move(d), true, 0});
    return {};
}

Result<void> MemoryMount::writeFile(std::string_view relPath, std::span<const u8> data) {
    if (!isSafeRelative(relPath) || relPath.empty()) return unsafePath(relPath);
    addFile(relPath, std::vector<u8>(data.begin(), data.end()));
    return {};
}

// ---------------------------------------------------------------------------------------------
// Vfs
// ---------------------------------------------------------------------------------------------

struct Vfs::Mount {
    MountId id = 0;
    std::string point;
    i32 priority = 0;
    std::shared_ptr<IMountProvider> provider;
};

struct Vfs::Candidate {
    std::shared_ptr<Mount> mount;
    std::string rel;
};

struct Vfs::Watch {
    struct Source {
        FileWatcher watcher;
        std::string virtualBase;
    };
    WatchId id = 0;
    WatchCallback callback;
    std::string dir;
    std::vector<std::unique_ptr<Source>> sources;
};

Vfs::Vfs() = default;
Vfs::~Vfs() = default;

Result<std::string> Vfs::normalizePath(std::string_view path) {
    if (path.empty() || (path.front() != '/' && path.front() != '\\')) {
        return makeError(ErrorCode::InvalidArgument, "virtual path '{}' must be absolute (start with '/')", path);
    }
    std::vector<std::string_view> parts;
    usize i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
        usize j = i;
        while (j < path.size() && path[j] != '/' && path[j] != '\\') {
            if (path[j] == ':' || path[j] == '\0') {
                return makeError(ErrorCode::InvalidArgument, "virtual path '{}' contains an illegal character", path);
            }
            ++j;
        }
        const std::string_view part = path.substr(i, j - i);
        i = j;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) {
                return makeError(ErrorCode::PermissionDenied, "virtual path '{}' escapes the root", path);
            }
            parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const std::string_view part : parts) {
        out.push_back('/');
        out.append(part);
    }
    if (out.empty()) out = "/";
    return out;
}

Result<MountId> Vfs::mount(std::string_view mountPoint, std::unique_ptr<IMountProvider> provider, i32 priority) {
    if (!provider) return Error{ErrorCode::InvalidArgument, "Vfs::mount: null provider"};
    HELIOS_TRY_ASSIGN(std::string point, normalizePath(mountPoint));
    auto m = std::make_shared<Mount>();
    m->point = std::move(point);
    m->priority = priority;
    m->provider = std::shared_ptr<IMountProvider>(std::move(provider));
    std::unique_lock lock(m_mutex);
    m->id = m_nextMountId++;
    HELIOS_LOG_DEBUG(LogFs, "Vfs: mounted {} at {} (priority {})", m->provider->describe(), m->point, priority);
    m_mounts.push_back(m);
    return m->id;
}

Result<MountId> Vfs::mountDirectory(std::string_view mountPoint, const Path& directory, i32 priority, bool readOnly) {
    if (!fs::isDirectory(directory)) {
        return makeError(ErrorCode::NotFound, "Vfs::mountDirectory: '{}' is not a directory",
                         pathToGenericUtf8(directory));
    }
    return mount(mountPoint, std::make_unique<DirectoryMount>(directory, readOnly), priority);
}

bool Vfs::unmount(MountId id) {
    std::unique_lock lock(m_mutex);
    const auto removed = std::erase_if(m_mounts, [id](const std::shared_ptr<Mount>& m) { return m->id == id; });
    return removed > 0;
}

std::vector<MountInfo> Vfs::mounts() const {
    std::shared_lock lock(m_mutex);
    std::vector<MountInfo> out;
    for (const auto& m : m_mounts) {
        out.push_back(MountInfo{m->id, m->point, m->priority, m->provider->isReadOnly(), m->provider->describe()});
    }
    return out;
}

std::vector<Vfs::Candidate> Vfs::candidates(std::string_view normalizedPath) const {
    std::vector<Candidate> out;
    {
        std::shared_lock lock(m_mutex);
        for (const auto& m : m_mounts) {
            if (auto rel = relativeTo(m->point, normalizedPath)) out.push_back(Candidate{m, std::string(*rel)});
        }
    }
    // Highest priority first; among equals the most recent mount (highest id) wins.
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.mount->priority != b.mount->priority) return a.mount->priority > b.mount->priority;
        return a.mount->id > b.mount->id;
    });
    return out;
}

Result<std::vector<u8>> Vfs::readFile(std::string_view path) const {
    HELIOS_TRY_ASSIGN(const std::string norm, normalizePath(path));
    for (const Candidate& c : candidates(norm)) {
        if (c.rel.empty() || !c.mount->provider->exists(c.rel) || c.mount->provider->isDirectory(c.rel)) continue;
        return c.mount->provider->readFile(c.rel);
    }
    return makeError(ErrorCode::NotFound, "Vfs: '{}' not found", norm);
}

Result<std::string> Vfs::readTextFile(std::string_view path) const {
    HELIOS_TRY_ASSIGN(auto bytes, readFile(path));
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Result<void> Vfs::writeFile(std::string_view path, std::span<const u8> data) {
    HELIOS_TRY_ASSIGN(const std::string norm, normalizePath(path));
    for (const Candidate& c : candidates(norm)) {
        if (c.rel.empty() || c.mount->provider->isReadOnly()) continue;
        return c.mount->provider->writeFile(c.rel, data);
    }
    return makeError(ErrorCode::PermissionDenied, "Vfs: no writable mount for '{}'", norm);
}

Result<void> Vfs::writeTextFile(std::string_view path, std::string_view text) {
    return writeFile(path, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool Vfs::exists(std::string_view path) const {
    auto norm = normalizePath(path);
    if (!norm) return false;
    for (const Candidate& c : candidates(*norm)) {
        if (c.mount->provider->exists(c.rel)) return true;
    }
    return isDirectory(*norm);
}

bool Vfs::isDirectory(std::string_view path) const {
    auto norm = normalizePath(path);
    if (!norm) return false;
    if (*norm == "/") return true;
    for (const Candidate& c : candidates(*norm)) {
        if (c.rel.empty() || c.mount->provider->isDirectory(c.rel)) return true;
    }
    // Ancestors of mount points are implicit directories ("/content" for a mount at "/content/dlc").
    std::shared_lock lock(m_mutex);
    const std::string prefix = *norm + "/";
    return std::any_of(m_mounts.begin(), m_mounts.end(),
                       [&](const std::shared_ptr<Mount>& m) { return m->point.starts_with(prefix); });
}

Result<std::vector<VfsEntry>> Vfs::list(std::string_view dir, bool recursive) const {
    HELIOS_TRY_ASSIGN(const std::string norm, normalizePath(dir));
    const std::string prefix = norm == "/" ? "/" : norm + "/";
    // Sources: mounts containing `dir`, plus (recursive only) mounts nested below it, listed from
    // their root: "/" or "/content" with DLC mounted at "/content/dlc" must include those files.
    // Highest priority first (ties: newest mount), the order readFile() resolves in, so the first
    // entry recorded for a path is the one that wins.
    std::vector<Candidate> sources = candidates(norm);
    if (recursive) {
        std::shared_lock lock(m_mutex);
        for (const auto& m : m_mounts) {
            if (m->point.size() > prefix.size() && m->point.starts_with(prefix)) sources.push_back(Candidate{m, {}});
        }
    }
    std::stable_sort(sources.begin(), sources.end(), [](const Candidate& a, const Candidate& b) {
        if (a.mount->priority != b.mount->priority) return a.mount->priority > b.mount->priority;
        return a.mount->id > b.mount->id;
    });
    std::map<std::string, VfsEntry, std::less<>> merged;
    bool found = false;
    for (const Candidate& c : sources) {
        if (!c.mount->provider->isDirectory(c.rel)) continue;
        std::vector<VfsEntry> entries;
        if (!c.mount->provider->list(c.rel, recursive, entries)) continue;
        found = true;
        for (VfsEntry& e : entries) {
            std::string vpath = joinVirtual(c.mount->point, e.path);
            if (!merged.contains(vpath)) {
                e.path = vpath;
                merged.emplace(std::move(vpath), std::move(e));
            }
        }
    }
    // Mount points below `dir` show up as directories.
    {
        std::shared_lock lock(m_mutex);
        for (const auto& m : m_mounts) {
            if (m->point.size() <= prefix.size() || !m->point.starts_with(prefix)) continue;
            found = true;
            const std::string_view rest = std::string_view(m->point).substr(prefix.size());
            usize slash = rest.find('/');
            for (;;) {
                const std::string vpath = prefix + std::string(rest.substr(0, slash));
                if (!merged.contains(vpath)) merged.emplace(vpath, VfsEntry{vpath, true, 0});
                if (!recursive || slash == std::string_view::npos) break;
                slash = rest.find('/', slash + 1);
            }
        }
    }
    if (!found) return makeError(ErrorCode::NotFound, "Vfs: directory '{}' not found", norm);
    std::vector<VfsEntry> out;
    out.reserve(merged.size());
    for (auto& [path, entry] : merged) out.push_back(std::move(entry));
    return out;
}

Result<Path> Vfs::resolveNativePath(std::string_view path) const {
    HELIOS_TRY_ASSIGN(const std::string norm, normalizePath(path));
    for (const Candidate& c : candidates(norm)) {
        auto root = c.mount->provider->nativeRoot();
        if (!root || !c.mount->provider->exists(c.rel)) continue;
        return c.rel.empty() ? *root : *root / pathFromUtf8(c.rel);
    }
    return makeError(ErrorCode::NotFound, "Vfs: '{}' has no native backing file", norm);
}

Result<Vfs::WatchId> Vfs::watch(std::string_view dir, WatchCallback callback) {
    HELIOS_TRY_ASSIGN(const std::string norm, normalizePath(dir));
    auto w = std::make_unique<Watch>();
    w->callback = std::move(callback);
    w->dir = norm;
    std::vector<std::shared_ptr<Mount>> mounts;
    {
        std::shared_lock lock(m_mutex);
        mounts = m_mounts;
    }
    const std::string prefix = norm == "/" ? "/" : norm + "/";
    for (const auto& m : mounts) {
        auto root = m->provider->nativeRoot();
        if (!root) continue;
        Path nativeDir;
        std::string virtualBase;
        if (auto rel = relativeTo(m->point, norm)) { // watched dir is inside this mount
            nativeDir = rel->empty() ? *root : *root / pathFromUtf8(*rel);
            virtualBase = norm;
        } else if (m->point.starts_with(prefix)) { // mount lives below the watched dir
            nativeDir = *root;
            virtualBase = m->point;
        } else {
            continue;
        }
        if (!fs::isDirectory(nativeDir)) continue;
        auto source = std::make_unique<Watch::Source>();
        auto started = source->watcher.start(nativeDir, true);
        if (!started) {
            HELIOS_LOG_WARN(LogFs, "Vfs::watch: cannot watch '{}': {}", pathToGenericUtf8(nativeDir),
                            started.error().toString());
            continue;
        }
        source->virtualBase = std::move(virtualBase);
        w->sources.push_back(std::move(source));
    }
    if (w->sources.empty()) return makeError(ErrorCode::Unsupported, "Vfs::watch: no watchable mount covers '{}'", norm);
    std::lock_guard lock(m_watchMutex);
    w->id = m_nextWatchId++;
    const WatchId id = w->id;
    m_watches.push_back(std::move(w));
    return id;
}

void Vfs::unwatch(WatchId id) {
    std::lock_guard lock(m_watchMutex);
    std::erase_if(m_watches, [id](const std::unique_ptr<Watch>& w) { return w->id == id; });
}

usize Vfs::pollWatches() {
    std::vector<std::pair<WatchCallback, VfsEvent>> pending;
    {
        std::lock_guard lock(m_watchMutex);
        for (auto& w : m_watches) {
            for (auto& source : w->sources) {
                for (FileEvent& e : source->watcher.poll()) {
                    VfsEvent ve;
                    ve.action = e.action;
                    ve.path = e.action == FileAction::Overflow ? w->dir : joinVirtual(source->virtualBase, e.path);
                    pending.emplace_back(w->callback, std::move(ve));
                }
            }
        }
    }
    // Callbacks run without the lock so they may call watch()/unwatch().
    for (auto& [callback, event] : pending) callback(event);
    return pending.size();
}

} // namespace helios::fs
