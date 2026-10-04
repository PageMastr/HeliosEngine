#include "helios/toolsfw/document.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "helios/core/hash.h"
#include "helios/core/platform.h"
#include "helios/reflect/type_info.h"

#include "doc_access.h"
#include "platform/tf_os.h"

namespace helios::tf {

std::string_view docKindName(DocKind kind) noexcept {
    switch (kind) {
    case DocKind::Record: return "record";
    }
    return "unknown";
}

Document::Document(DocId id, const refl::TypeInfo& type, refl::RecordHeader header, fs::Path path)
    : m_id(id), m_type(&type), m_value(type), m_header(std::move(header)), m_path(std::move(path)) {}

Document::~Document() = default;

void DocAccess::setBase(Document& d, std::string text) {
    d.m_savedHash = hash64(text);
    d.m_baseText = std::move(text);
}

void Document::touch() noexcept {
    ++m_revision;
}

const std::string& Document::text() const {
    if (m_cacheRevision != m_revision) {
        m_textCache = recordText(*m_type, m_value.data(), m_header);
        m_hashCache = hash64(m_textCache);
        m_cacheRevision = m_revision;
    }
    return m_textCache;
}

u64 Document::contentHash() const {
    (void)text();
    return m_hashCache;
}

std::string recordText(const refl::TypeInfo& type, const void* object, const refl::RecordHeader& header) {
    return refl::writeRecord(type, object, header);
}

namespace {

/// File-name equality as the platform's file system sees it: Windows (NTFS and the Win32 APIs)
/// ignores case, so "Records/Hull/Frigate.hrec" is the same file as "records/hull/frigate.hrec"
/// there. Only ASCII letters are folded (record paths are ASCII by convention).
bool sameName(std::string_view a, std::string_view b) noexcept {
    if constexpr (!platform::kIsWindows) {
        return a == b;
    } else {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            if (fold(a[i]) != fold(b[i])) return false;
        }
        return true;
    }
}

bool samePath(const fs::Path& a, const fs::Path& b) {
    if constexpr (!platform::kIsWindows) {
        return a == b;
    } else {
        return sameName(fs::pathToGenericUtf8(a), fs::pathToGenericUtf8(b));
    }
}

/// `path` relative to `root` ('/'-separated, "" for the root itself), or nullopt when `path` is
/// not under `root`. Both are compared as normalized generic strings, without ASCII case on
/// Windows (sameName), so `c:\proj\x` is under `C:/proj`.
std::optional<std::string> underRoot(const fs::Path& root, const fs::Path& path) {
    std::string r = fs::pathToGenericUtf8(root.lexically_normal());
    const std::string p = fs::pathToGenericUtf8(path.lexically_normal());
    if (r.empty()) return std::nullopt;
    if (r.size() > 1 && r.back() == '/') r.pop_back();  // "C:/proj/" -> "C:/proj" (but keep "/")
    if (p.size() < r.size() || !sameName(std::string_view(p).substr(0, r.size()), r)) return std::nullopt;
    std::string_view rest = std::string_view(p).substr(r.size());
    if (rest.empty()) return std::string();
    if (r.back() != '/') {
        if (rest.front() != '/') return std::nullopt;  // "/proj2" is not under "/proj"
        rest.remove_prefix(1);
    }
    return std::string(rest);
}

/// A path for an error message: quoted, control characters escaped (a hostile journal must not
/// write terminal escape sequences through an error), and cut after 200 bytes.
std::string shown(std::string_view path) {
    return "'" + printable(path, 200) + "'";
}

/// CON, PRN, AUX, NUL, COM0-9, LPT0-9 (and COM/LPT with a superscript 1-3), CONIN$ and CONOUT$:
/// names Win32 maps to devices in every directory, with any extension ("NUL.hrec", "con .x").
bool isWindowsDeviceName(std::string_view component) {
    std::string_view stem = component.substr(0, component.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);
    std::string upper(stem);
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    static constexpr std::array<std::string_view, 6> kNames = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    if (std::find(kNames.begin(), kNames.end(), upper) != kNames.end()) return true;
    if (!upper.starts_with("COM") && !upper.starts_with("LPT")) return false;
    const std::string_view digit = std::string_view(upper).substr(3);
    if (digit.size() == 1) return digit[0] >= '0' && digit[0] <= '9';
    return digit == "\xC2\xB9" || digit == "\xC2\xB2" || digit == "\xC2\xB3";  // superscript 1, 2, 3
}

constexpr std::string_view kRecordExtension = ".hrec";

} // namespace

bool sameRelativePath(std::string_view a, std::string_view b) noexcept {
    return sameName(a, b);
}

const refl::TypeInfo* recordTypeForPath(const refl::TypeRegistry& types, std::string_view relativePath) {
    // "records/<table>/..." -> <table>. Accept a path that starts at the table directory too.
    std::string_view rest = relativePath;
    if (rest.size() >= 8 && sameName(rest.substr(0, 8), "records/")) rest.remove_prefix(8);
    const usize slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return nullptr;
    const std::string_view table = rest.substr(0, slash);
    for (const refl::TypeInfo* t : types.types()) {
        if (t->decl != refl::DeclKind::Record) continue;
        if (const auto* tbl = t->attr<refl::attrs::Table>(); tbl && sameName(tbl->name, table)) return t;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------------------------
Workspace::Workspace(const refl::TypeRegistry& types) : m_types(&types) {}
Workspace::~Workspace() = default;

void Workspace::setRoot(fs::Path root) {
    std::error_code ec;
    fs::Path abs = std::filesystem::absolute(root, ec);
    m_root = (ec ? root : abs).lexically_normal();
}

Document* Workspace::find(const DocId& id) const noexcept {
    for (Document* d : m_order) {
        if (d->id() == id) return d;
    }
    return nullptr;
}

Document* Workspace::find(std::string_view key) const {
    if (key.empty()) return nullptr;
    for (Document* d : m_order) {
        if (!d->destroyed() && (d->name() == key || d->relativePath() == key)) return d;
    }
    if (auto g = Guid::parse(key)) {
        if (Document* d = find(*g)) return d;
    }
    return findByPath(fs::pathFromUtf8(key));
}

Document* Workspace::findByPath(const fs::Path& path) const {
    if (path.empty()) return nullptr;
    fs::Path abs = path.is_absolute() ? path : m_root / path;
    abs = abs.lexically_normal();
    for (Document* d : m_order) {
        if (!d->destroyed() && !d->path().empty() && samePath(d->path(), abs)) return d;
    }
    return nullptr;
}

Result<ProjectFile> Workspace::confine(std::string_view path, PathOrigin origin, PathCheck check) const {
    const auto refuse = [&](std::string_view why) {
        return Error{ErrorCode::InvalidArgument, std::format("{}: {}", shown(path), why)};
    };
    if (path.empty()) return refuse("empty path");
    // A caller may name a file by its absolute path; only the part below the root goes on.
    std::string belowRoot;
    std::string_view rel = path;
    if (origin == PathOrigin::Caller) {
        const fs::Path p = fs::pathFromUtf8(path);
        if (p.is_absolute()) {
            auto under = m_root.empty() ? std::nullopt : underRoot(m_root, p);
            if (!under) return refuse("outside the project");
            belowRoot = std::move(*under);
            rel = belowRoot;
            if (rel.empty()) return refuse("names the project root, not a file");
        }
    }
    if (rel.front() == '/' || rel.front() == '\\') {
        return refuse("an absolute, UNC or device path; journal and op paths are relative to the project root");
    }
    if (rel.back() == '/' || rel.back() == '\\') return refuse("ends in a separator, so it names a directory");
    // The spelling rule is the same on every platform: a journal written on Linux may be replayed
    // on Windows, so whatever Windows would read differently is refused everywhere.
    std::vector<std::string_view> parts;
    usize start = 0;
    for (usize i = 0; i <= rel.size(); ++i) {
        if (i < rel.size()) {
            const auto c = static_cast<unsigned char>(rel[i]);
            const auto next = i + 1 < rel.size() ? static_cast<unsigned char>(rel[i + 1]) : 0;
            if (c < 0x20 || c == 0x7F || (c == 0xC2 && next >= 0x80 && next <= 0x9F)) return refuse("a control character");
            if (std::string_view(R"(<>:"|?*)").find(static_cast<char>(c)) != std::string_view::npos) {
                return refuse(std::format("'{}' is not allowed (drive letters, NTFS streams, wildcards)", static_cast<char>(c)));
            }
            if (c != '/' && c != '\\') continue;
        }
        const std::string_view part = rel.substr(start, i - start);
        start = i + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return refuse("a '..' component (project paths never leave their directory)");
        if (part.back() == '.' || part.back() == ' ') {
            return refuse(std::format("{} ends in a dot or a space, which Windows drops", shown(part)));
        }
        if (isWindowsDeviceName(part)) return refuse(std::format("{} is a reserved device name on Windows", shown(part)));
        parts.push_back(part);
    }
    if (parts.empty()) return refuse("names no file");
    // ASCII case aside, as openAll() lists record files (fs::listDirectory compares extensions so).
    const std::string_view last = parts.back();
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (last.size() <= kRecordExtension.size() ||
        !std::equal(kRecordExtension.begin(), kRecordExtension.end(), last.end() - static_cast<isize>(kRecordExtension.size()),
                    [&](char a, char b) { return a == lower(b); })) {
        return refuse("record files end in .hrec");
    }
    ProjectFile out;
    out.absolute = m_root;
    for (const std::string_view part : parts) {
        if (!out.relative.empty()) out.relative += '/';
        out.relative += part;
        out.absolute /= fs::pathFromUtf8(part);
    }
    if (check == PathCheck::Lexical) return out;

    // On disk: walk from the root without following anything, and resolve each link found. A
    // missing component ends the walk: below it nothing exists, and what a save creates there is
    // a real directory or file.
    const fs::Path base = m_root.empty() ? fs::Path(".") : m_root;
    fs::Path cur = base;
    std::string prefix;
    std::optional<fs::Path> realRoot;  // resolved once, when the first link needs comparing
    for (const std::string_view part : parts) {
        cur /= fs::pathFromUtf8(part);
        if (!prefix.empty()) prefix += '/';
        prefix += part;
        auto kind = os::entryKind(cur);
        if (!kind) return Error{kind.error().code, std::format("{}: {}", shown(path), kind.error().message)};
        if (*kind == os::EntryKind::Missing) break;
        if (*kind == os::EntryKind::Link) {
            auto target = os::finalPath(cur);
            if (!target) return refuse(std::format("{} is a link that does not resolve ({})", shown(prefix), target.error().message));
            if (!realRoot) {
                auto r = os::finalPath(base);
                if (!r) return Error{r.error().code, std::format("{}: the project root: {}", shown(path), r.error().message)};
                realRoot = std::move(*r);
            }
            if (!underRoot(*realRoot, *target)) {
                return refuse(std::format("{} is a link to {}, outside the project", shown(prefix), shown(fs::pathToGenericUtf8(*target))));
            }
            // What the link leads to. A fully resolved path that still reports Link is a Windows
            // reparse point that is not a name surrogate (a OneDrive placeholder, a deduplicated
            // file): an ordinary file or directory for this purpose.
            kind = os::entryKind(*target);
            if (!kind) return Error{kind.error().code, std::format("{}: {}", shown(path), kind.error().message)};
        }
        if (*kind == os::EntryKind::Other) return refuse(std::format("{} is not a regular file or directory", shown(prefix)));
    }
    return out;
}

} // namespace helios::tf
