// `helios-cook check`: the project file, provenance and layout of a project's content (content_check.h).

#include "content_check.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <system_error>

#include "helios/assetpipe/meta.h"
#include "helios/core/guid.h"
#include "helios/core/process.h"
#include "helios/core/vfs.h"
#include "helios/reflect/json.h"

namespace helios::cook {
namespace {

using refl::JsonDocument;
using refl::JsonValue;

constexpr std::string_view kProjectFileName = "helios.project.jsonc";
constexpr u64 kProjectVersion = 0;
/// Project files, containers and entities are small; a larger one is refused before it is parsed.
constexpr u64 kMaxDocumentBytes = 1 * kMiB;
/// Build targets that enable gems (02 §1.3), sorted.
constexpr std::string_view kTargets[] = {"cellserver", "client", "editor", "gateway",
                                         "launcher", "tools", "voice"};
/// URI schemes a product may not register (08 §2.10.1), besides `helios-*` and `ms-*`.
constexpr std::string_view kReservedSchemes[] = {"com.epicgames.launcher", "data", "discord", "file", "ftp",
                                                 "helios", "http", "https", "javascript", "mailto", "steam"};
/// Frame kinds a zone's root frame or a container's parent frame may have (02 §5.1: galaxy, system, body,
/// grid, interior; a zone simulates one system at most, since f64 covers a single system).
constexpr std::string_view kZoneFrameKinds[] = {"system", "body", "grid"};
constexpr std::string_view kParentFrameKinds[] = {"system", "body", "grid", "interior"};
/// Binary sources, stored in Git LFS (07 §1.7): the importer id (also the extension) and a name for messages.
struct BinaryType {
    std::string_view id;
    std::string_view name;
};
constexpr BinaryType kBinaryTypes[] = {{"exr", "OpenEXR"}, {"glb", "glTF 2.0 binary"}, {"png", "PNG"}};
/// The Git LFS pointer spec caps a pointer file at 1024 bytes (git-lfs writes about 130).
constexpr u64 kMaxLfsPointerBytes = 1024;
/// The cook's output folders at a content root's top, which .gitignore keeps out of git.
constexpr std::string_view kCookOutputs[] = {".cooked", ".cache"};

bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isAlpha(char c) noexcept { return isLower(c) || (c >= 'A' && c <= 'Z'); }

template <class Range>
bool contains(const Range& range, std::string_view s) {
    return std::find(std::begin(range), std::end(range), s) != std::end(range);
}

/// `[a-z][a-z0-9<extra>]*`, `minLen`..`maxLen` characters.
bool isIdent(std::string_view s, usize minLen, usize maxLen, std::string_view extra) {
    if (s.size() < minLen || s.size() > maxLen || !isLower(s[0])) return false;
    return std::all_of(s.begin() + 1, s.end(), [&](char c) {
        return isLower(c) || isDigit(c) || extra.find(c) != std::string_view::npos;
    });
}

/// MAJOR.MINOR.PATCH without leading zeros (semver 2.0.0 §2). Pre-release and build tags are not used yet.
bool isSemver(std::string_view s) {
    int parts = 0;
    usize i = 0;
    while (true) {
        const usize start = i;
        while (i < s.size() && isDigit(s[i])) ++i;
        if (i == start || i - start > 9 || (s[start] == '0' && i - start > 1)) return false;
        ++parts;
        if (i == s.size()) return parts == 3;
        if (s[i] != '.' || parts == 3) return false;
        ++i;
    }
}

/// "<kind>:<name>" with a kind from `kinds` and a lower-case identifier name.
bool isFrameRef(std::string_view s, std::span<const std::string_view> kinds) {
    const usize colon = s.find(':');
    return colon != std::string_view::npos && contains(kinds, s.substr(0, colon)) &&
           isIdent(s.substr(colon + 1), 1, 64, "_");
}

/// A GUID in canonical text (lower case, dashed) other than nil.
std::optional<Guid> canonicalGuid(std::string_view s) {
    auto g = Guid::parse(s);
    if (!g || g->isNil() || g->toString() != s) return std::nullopt;
    return *g;
}

/// The extension of `name` as written, from its last '.' ("" without one).
std::string_view rawExtension(std::string_view name) {
    const usize slash = name.rfind('/');
    const usize dot = name.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return name.substr(dot);
}

std::string lowerExtension(std::string_view name) {
    std::string ext(rawExtension(name));
    for (char& c : ext) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return ext;
}

const BinaryType* binaryType(std::string_view lowerExt) {
    for (const BinaryType& t : kBinaryTypes) {
        if (lowerExt.size() == t.id.size() + 1 && lowerExt[0] == '.' && lowerExt.substr(1) == t.id) return &t;
    }
    return nullptr;
}

/// `.cooked`, `.cache` or a path inside them (relative to a content root).
bool isCookOutput(std::string_view rel) {
    return std::any_of(std::begin(kCookOutputs), std::end(kCookOutputs), [&](std::string_view dir) {
        return rel == dir || (rel.starts_with(dir) && rel.size() > dir.size() && rel[dir.size()] == '/');
    });
}

/// A Git LFS pointer as git-lfs writes it (pointer spec v1): exactly the version line, `oid sha256:` with 64
/// lower-case hex digits and `size` with a decimal byte count, each ending in LF. Anything else, extension
/// keys included, is not one (fail closed).
bool isLfsPointer(std::string_view t) {
    constexpr std::string_view kVersion = "version https://git-lfs.github.com/spec/v1\n";
    constexpr std::string_view kOid = "oid sha256:";
    constexpr std::string_view kSize = "size ";
    if (!t.starts_with(kVersion)) return false;
    t.remove_prefix(kVersion.size());
    if (!t.starts_with(kOid) || t.size() < kOid.size() + 65 || t[kOid.size() + 64] != '\n') return false;
    const std::string_view hex = t.substr(kOid.size(), 64);
    if (!std::all_of(hex.begin(), hex.end(), [](char c) { return isDigit(c) || (c >= 'a' && c <= 'f'); }))
        return false;
    t.remove_prefix(kOid.size() + 65);
    if (!t.starts_with(kSize)) return false;
    t.remove_prefix(kSize.size());
    usize digits = 0;
    while (digits < t.size() && isDigit(t[digits])) ++digits;
    return digits > 0 && digits <= 19 && (t[0] != '0' || digits == 1) && t.substr(digits) == "\n";
}

/// Whether `head` (the first bytes of a file of `fileSize` bytes) starts like a file of binary type `id`.
bool matchesFormat(std::string_view id, std::span<const u8> head, u64 fileSize) {
    const auto u32At = [&](usize at) {
        return static_cast<u32>(head[at]) | static_cast<u32>(head[at + 1]) << 8 |
               static_cast<u32>(head[at + 2]) << 16 | static_cast<u32>(head[at + 3]) << 24;
    };
    if (id == "glb") // glTF 2.0 §4.4.3: magic "glTF", container version 2, total length
        return head.size() >= 12 && u32At(0) == 0x46546C67u && u32At(4) == 2 && u32At(8) == fileSize;
    if (id == "png") { // ISO/IEC 15948 §5.2: the 8-byte signature
        constexpr u8 kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        return head.size() >= 8 && std::equal(std::begin(kSignature), std::end(kSignature), head.begin());
    }
    if (id == "exr") // OpenEXR: magic number 20000630, file format version 2 in the low byte
        return head.size() >= 8 && u32At(0) == 20000630u && (u32At(4) & 0xFFu) == 2;
    return false;
}

std::vector<std::string_view> components(std::string_view rel) {
    std::vector<std::string_view> out;
    usize start = 0;
    while (start <= rel.size()) {
        const usize slash = rel.find('/', start);
        const usize end = slash == std::string_view::npos ? rel.size() : slash;
        out.push_back(rel.substr(start, end - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return out;
}

/// Findings about one file, with the JSON path of the value in the message.
struct Report {
    std::string file;
    std::vector<Finding>* out;
    void operator()(std::string_view where, std::string message) const {
        out->push_back(
            Finding{file, where.empty() ? std::move(message) : std::format("{}: {}", where, message)});
    }
};

std::string join(std::string_view where, std::string_view key) {
    return where.empty() ? std::string(key) : std::format("{}.{}", where, key);
}

/// Reports every key of `obj` not in `known`, and every key that appears twice (yyjson keeps both).
void checkKeys(JsonValue obj, std::string_view where, std::initializer_list<std::string_view> known,
               const Report& r) {
    std::set<std::string_view> seen;
    for (const refl::JsonMember& m : obj.members()) {
        if (!seen.insert(m.key).second) {
            r(join(where, m.key), "duplicate key");
        } else if (!contains(known, m.key)) {
            r(join(where, m.key),
              "unknown key (this version of helios-cook does not check it, so it fails closed)");
        }
    }
}

/// The member `key` of object `obj` with JSON type `type` (and its JSON path), or an invalid value after
/// reporting it missing (when `required`) or of the wrong type.
JsonValue member(JsonValue obj, std::string_view where, std::string_view key, refl::JsonType type,
                 bool required, const Report& r) {
    const JsonValue v = obj.get(key);
    if (!v.isValid()) {
        if (required) r(join(where, key), "missing");
        return {};
    }
    if (v.type() != type) {
        r(join(where, key), std::format("expected {}, got {}", refl::jsonTypeName(type), v.typeName()));
        return {};
    }
    return v;
}

Result<std::string> readSmallText(const fs::Path& path, u64 size) {
    if (size > kMaxDocumentBytes)
        return makeError(ErrorCode::LimitExceeded, "{} bytes is above the {}-byte limit", size,
                         kMaxDocumentBytes);
    return fs::readTextFile(path);
}

// --------------------------------------------------------------------------------------------------------
// The project file
// --------------------------------------------------------------------------------------------------------

void checkGems(const fs::Path& projectRoot, JsonValue gems, const Report& r) {
    std::set<std::string_view> targets;
    for (const refl::JsonMember& t : gems.members()) {
        const std::string where = join("gems", t.key);
        if (!targets.insert(t.key).second) { // yyjson keeps both members, so readers could disagree
            r(where, "duplicate key");
            continue;
        }
        if (!contains(kTargets, t.key)) {
            r(where,
              "not a build target (02 §1.3: cellserver, client, editor, gateway, launcher, tools, voice)");
            continue;
        }
        if (!t.value.isArray()) {
            r(where, std::format("expected array, got {}", t.value.typeName()));
            continue;
        }
        std::set<std::string_view> names;
        for (const JsonValue g : t.value.elements()) {
            const std::string_view name = g.asString();
            if (!g.isString() || !isIdent(name, 1, 64, "_-")) {
                r(where, "a gem name is a string of [a-z0-9_-] that starts with a letter");
            } else if (!names.insert(name).second) {
                r(where, std::format("gem '{}' is listed twice", name));
            } else if (!fs::isFile(projectRoot / "gems" / fs::pathFromUtf8(name) / "gem.jsonc")) {
                r(where, std::format("gem '{}' has no gems/{}/gem.jsonc (02 §1.2)", name, name));
            }
        }
    }
}

void checkSchemas(const fs::Path& projectRoot, JsonValue schemas, const Report& r) {
    checkKeys(schemas, "schemas", {"native"}, r);
    const JsonValue native = member(schemas, "schemas", "native", refl::JsonType::Array, false, r);
    std::set<std::string_view> seen;
    for (const JsonValue p : native.elements()) {
        const std::string_view name = p.asString();
        if (!p.isString() || !isIdent(name, 1, 64, "_")) {
            r("schemas.native", "a package name is a string of [a-z0-9_] that starts with a letter");
            continue;
        }
        if (!seen.insert(name).second) {
            r("schemas.native", std::format("package '{}' is listed twice", name));
            continue;
        }
        fs::ListOptions opts;
        opts.recursive = true;
        opts.includeDirectories = false;
        opts.extension = ".hschema";
        const auto files = fs::listDirectory(projectRoot / "schemas" / fs::pathFromUtf8(name), opts);
        if (!files || files->empty())
            r("schemas.native",
              std::format("package '{}' has no .hschema files in schemas/{}/ (02 §3.8)", name, name));
    }
}

std::vector<std::string> checkContentRoots(const fs::Path& projectRoot, JsonValue roots, const Report& r) {
    std::vector<std::string> out;
    if (roots.size() == 0) r("contentRoots", "a project has at least one content root");
    for (const JsonValue v : roots.elements()) {
        const std::string_view root = v.asString();
        if (!v.isString()) {
            r("contentRoots", std::format("expected string, got {}", v.typeName()));
        } else if (auto ok = assetpipe::checkProjectPath(root); !ok) {
            r("contentRoots", ok.error().message);
        } else if (!fs::isDirectory(projectRoot / fs::pathFromUtf8(root))) {
            r("contentRoots", std::format("'{}' is not a directory", root));
        } else if (std::find(out.begin(), out.end(), root) != out.end()) {
            r("contentRoots", std::format("'{}' is listed twice", root));
        } else {
            out.emplace_back(root);
        }
    }
    // A root inside another would put its files under both: two scans, and records cooked twice.
    for (const std::string& a : out) {
        for (const std::string& b : out) {
            if (b.size() > a.size() && b.starts_with(a) && b[a.size()] == '/')
                r("contentRoots", std::format("'{}' is inside '{}'", b, a));
        }
    }
    return out;
}

std::vector<ZoneDecl> checkZones(JsonValue zones, const Report& r) {
    std::vector<ZoneDecl> out;
    std::set<std::string> names;
    std::set<u64> ids;
    usize index = 0;
    for (const JsonValue z : zones.elements()) {
        const std::string where = std::format("zones[{}]", index++);
        if (!z.isObject()) {
            r(where, std::format("expected object, got {}", z.typeName()));
            continue;
        }
        checkKeys(z, where, {"name", "id", "tickHz", "frame"}, r);
        ZoneDecl d;
        bool ok = true;
        if (const JsonValue v = member(z, where, "name", refl::JsonType::String, true, r); v.isValid()) {
            d.name = std::string(v.asString());
            if (!isIdent(d.name, 1, 32, "_")) {
                r(join(where, "name"),
                  "a zone name is 1-32 characters of [a-z0-9_] that start with a letter");
                ok = false;
            } else if (!names.insert(d.name).second) {
                r(join(where, "name"), std::format("zone '{}' is declared twice", d.name));
                ok = false;
            }
        } else {
            ok = false;
        }
        if (const JsonValue v = member(z, where, "id", refl::JsonType::Number, true, r); v.isValid()) {
            u64 id = 0;
            if (!v.getU64(id) || id == 0 || id > 0xFFFF'FFFFull) {
                r(join(where, "id"), "a zone id is an integer in 1..4294967295");
            } else if (!ids.insert(id).second) {
                r(join(where, "id"), std::format("zone id {} is used twice", id));
            } else {
                d.id = static_cast<u32>(id);
            }
        }
        if (const JsonValue v = member(z, where, "tickHz", refl::JsonType::Number, true, r); v.isValid()) {
            u64 hz = 0;
            if (!v.getU64(hz) || hz < 1 || hz > 60) {
                r(join(where, "tickHz"), "a tick rate is an integer in 1..60 Hz (ADR-007)");
            } else {
                d.tickHz = static_cast<u32>(hz);
            }
        }
        if (const JsonValue v = member(z, where, "frame", refl::JsonType::String, true, r); v.isValid()) {
            d.frame = std::string(v.asString());
            if (!isFrameRef(d.frame, kZoneFrameKinds))
                r(join(where, "frame"),
                  "a zone's root frame is \"system:<name>\", \"body:<name>\" or \"grid:<name>\" (02 §5.1)");
        }
        if (ok) out.push_back(std::move(d));
    }
    return out;
}

void checkProduct(JsonValue product, const Report& r) {
    constexpr std::string_view kWhere = "product";
    checkKeys(product, kWhere, {"productId", "displayName", "installName", "uriScheme", "version"}, r);
    if (const JsonValue v = member(product, kWhere, "productId", refl::JsonType::String, true, r);
        v.isValid()) {
        const std::string_view id = v.asString();
        if (!isIdent(id, 3, 32, "-"))
            r("product.productId", "a product id matches ^[a-z][a-z0-9-]{2,31}$ (08 §2.10.1)");
        else if (id.starts_with("helios"))
            r("product.productId",
              "ids that start with 'helios' are the engine's own products (08 §2.10.1)");
    }
    if (const JsonValue v = member(product, kWhere, "displayName", refl::JsonType::Object, true, r);
        v.isValid()) {
        if (v.size() == 0) r("product.displayName", "needs one entry per shipped language, at least one");
        std::set<std::string_view> languages;
        for (const refl::JsonMember& m : v.members()) {
            const std::string where = join("product.displayName", m.key);
            if (!languages.insert(m.key).second) {
                r(where, "duplicate key");
                continue;
            }
            const bool tag = m.key.size() >= 2 && m.key.size() <= 35 &&
                             std::all_of(m.key.begin(), m.key.end(),
                                         [](char c) { return isAlpha(c) || isDigit(c) || c == '-'; }) &&
                             isLower(m.key[0]) && isLower(m.key[1]);
            if (!tag) r(where, "not a language tag (BCP 47, e.g. \"en\", \"pt-BR\")");
            if (!m.value.isString() || m.value.asString().empty())
                r(where, "a display name is a non-empty string");
        }
    }
    if (const JsonValue v = member(product, kWhere, "installName", refl::JsonType::String, true, r);
        v.isValid()) {
        const std::string_view name = v.asString();
        const bool form =
            name.size() >= 3 && name.size() <= 24 && isAlpha(name[0]) &&
            std::all_of(name.begin(), name.end(), [](char c) { return isAlpha(c) || isDigit(c); });
        std::string lower(name);
        for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (!form)
            r("product.installName", "an install name matches ^[A-Za-z][A-Za-z0-9]{2,23}$ (08 §2.10.1)");
        else if (lower.starts_with("helios"))
            r("product.installName",
              "names that start with 'helios' are the engine's own products (08 §2.10.1)");
        else if (fs::isNonPortableComponent(name))
            r("product.installName", "a Windows reserved device name (08 §2.10.1)");
    }
    if (const JsonValue v = member(product, kWhere, "uriScheme", refl::JsonType::String, true, r);
        v.isValid()) {
        const std::string_view s = v.asString();
        if (!isIdent(s, 1, 64, "+.-"))
            r("product.uriScheme", "a URI scheme is lower case and matches RFC 3986's ALPHA *( ALPHA / DIGIT "
                                   "/ \"+\" / \"-\" / \".\" )");
        else if (contains(kReservedSchemes, s) || s.starts_with("helios-") || s.starts_with("ms-"))
            r("product.uriScheme", std::format("'{}' is reserved (08 §2.10.1)", s));
    }
    if (const JsonValue v = member(product, kWhere, "version", refl::JsonType::String, true, r);
        v.isValid()) {
        if (!isSemver(v.asString())) r("product.version", "expected MAJOR.MINOR.PATCH");
    }
}

// --------------------------------------------------------------------------------------------------------
// Content
// --------------------------------------------------------------------------------------------------------

struct Doc {
    std::string rel; ///< Relative to its content root.
    fs::Path path;
    u64 size = 0;
};

/// Parses a container or entity; reports and returns an invalid value when it cannot.
std::optional<JsonDocument> parseDoc(const Doc& d, const Report& r) {
    auto text = readSmallText(d.path, d.size);
    if (!text) {
        r("", text.error().message);
        return std::nullopt;
    }
    auto doc = JsonDocument::parse(*text, d.rel.substr(d.rel.rfind('/') + 1));
    if (!doc) {
        r("", doc.error().message);
        return std::nullopt;
    }
    if (!doc->root().isObject()) {
        r("", std::format("expected an object, got {}", doc->root().typeName()));
        return std::nullopt;
    }
    return std::move(*doc);
}

/// `zones/<zone>/…/<name>.hcont` (02 §5.6).
void checkContainer(const Doc& d, std::string_view zone, const std::map<std::string, Guid>& metaGuids,
                    const Report& r) {
    const auto doc = parseDoc(d, r);
    if (!doc) return;
    const JsonValue root = doc->root();
    std::optional<Guid> guid;
    if (const JsonValue v = member(root, "", "$container", refl::JsonType::String, true, r); v.isValid()) {
        const std::string_view text = v.asString();
        if (text.starts_with("guid:")) guid = canonicalGuid(text.substr(5));
        if (!guid)
            r("$container", "expected \"guid:<GUID>\" with the GUID in canonical lower case (02 §5.6)");
    }
    const std::string_view file = std::string_view(d.rel).substr(d.rel.rfind('/') + 1);
    const std::string_view stem = file.substr(0, file.size() - std::string_view(".hcont").size());
    if (const JsonValue v = member(root, "", "name", refl::JsonType::String, true, r); v.isValid()) {
        if (v.asString() != stem)
            r("name", std::format("'{}' differs from the file name's '{}'", v.asString(), stem));
    }
    if (const JsonValue frame = member(root, "", "frame", refl::JsonType::Object, true, r); frame.isValid()) {
        if (const JsonValue v = member(frame, "frame", "parent", refl::JsonType::String, true, r);
            v.isValid()) {
            if (!isFrameRef(v.asString(), kParentFrameKinds))
                r("frame.parent",
                  "expected \"<kind>:<name>\" with kind system, body, grid or interior (02 §5.1)");
        }
    }
    if (const JsonValue s = member(root, "", "streaming", refl::JsonType::Object, true, r); s.isValid()) {
        if (const JsonValue v = member(s, "streaming", "group", refl::JsonType::String, true, r);
            v.isValid() && !zone.empty() && v.asString() != std::format("zone.{}", zone)) {
            r("streaming.group",
              std::format("'{}' is not its zone's group, 'zone.{}' (02 §5.5-5.6)", v.asString(), zone));
        }
    }
    if (const auto m = metaGuids.find(d.rel); guid && m != metaGuids.end() && m->second != *guid)
        r("", std::format("its sidecar's GUID {} differs from $container {}: one document, one identity",
                          m->second, *guid));
}

/// `<dir>/<container>.entities/<GUID>.hent` next to `<dir>/<container>.hcont` (02 §5.6, OFPA).
void checkEntity(const Doc& d, const std::set<std::string>& docs,
                 const std::map<std::string, Guid>& metaGuids, const Report& r) {
    const usize slash = d.rel.rfind('/');
    const std::string_view file = std::string_view(d.rel).substr(slash + 1);
    const std::string_view stem = file.substr(0, file.size() - std::string_view(".hent").size());
    const std::string_view dir =
        slash == std::string::npos ? std::string_view() : std::string_view(d.rel).substr(0, slash);
    constexpr std::string_view kEntities = ".entities";
    const usize dirSlash = dir.rfind('/');
    const std::string_view dirName = dirSlash == std::string_view::npos ? dir : dir.substr(dirSlash + 1);
    if (!dirName.ends_with(kEntities) || dirName.size() == kEntities.size()) {
        r("", "an entity sits in its container's <name>.entities/ folder (02 §5.6)");
    } else {
        const std::string container = std::format("{}.hcont", dir.substr(0, dir.size() - kEntities.size()));
        if (!docs.contains(container)) {
            r("", std::format("its container {} does not exist next to its folder",
                              std::string_view(container).substr(container.rfind('/') + 1)));
        }
    }
    const std::optional<Guid> guid = canonicalGuid(stem);
    if (!guid) r("", "an entity's file name is its GUID in canonical lower case, then .hent (02 §5.6)");
    const auto doc = parseDoc(d, r);
    if (!doc) return;
    if (const JsonValue v = member(doc->root(), "", "$entity", refl::JsonType::String, true, r);
        v.isValid()) {
        if (v.asString() != stem)
            r("$entity", std::format("'{}' differs from the file name's GUID '{}'", v.asString(), stem));
    }
    if (const auto m = metaGuids.find(d.rel); guid && m != metaGuids.end() && m->second != *guid)
        r("", std::format("its sidecar's GUID {} differs from the entity's {}: one document, one identity",
                          m->second, *guid));
}

/// A binary source's file (07 §1.7): a Git LFS pointer, as a checkout without the LFS objects has (CI), or
/// a file that starts like its type, so that a renamed file of another kind cannot pass for it. Reads at
/// most kMaxLfsPointerBytes.
void checkBinarySource(const Doc& d, const BinaryType& type, ContentStats& stats, const Report& r) {
    auto file = fs::File::open(d.path, fs::OpenMode::Read);
    if (!file) {
        r("", file.error().message);
        return;
    }
    std::array<u8, kMaxLfsPointerBytes> head{};
    const usize want = static_cast<usize>(std::min<u64>(d.size, head.size()));
    usize got = 0;
    while (got < want) {
        auto n = file->read(head.data() + got, want - got);
        if (!n) {
            r("", n.error().message);
            return;
        }
        if (*n == 0) break;
        got += *n;
    }
    const std::string_view text(reinterpret_cast<const char*>(head.data()), got);
    if (d.size <= kMaxLfsPointerBytes && isLfsPointer(text)) {
        ++stats.lfsPointers;
        return;
    }
    if (!matchesFormat(type.id, std::span<const u8>(head.data(), got), d.size))
        r("", std::format("neither a Git LFS pointer nor a {} file: a binary source is its format's bytes, "
                          "stored in Git LFS (07 §1.7)",
                          type.name));
}

/// Runs `git -C <dir> <args...>` with `input` on its standard input; returns its standard output, or why it
/// could not run or failed.
Result<std::string> runGit(const fs::Path& dir, std::initializer_list<std::string_view> args,
                           std::string_view input = {}) {
    ProcessDesc desc;
    desc.executable = fs::pathFromUtf8("git");
    desc.searchPath = true;
    desc.args = {"-C", fs::pathToUtf8(dir)};
    for (const std::string_view a : args) desc.args.emplace_back(a);
    HELIOS_TRY_ASSIGN(ProcessOutput out, runProcess(std::move(desc), input));
    if (out.exitCode != 0) {
        std::string_view err = out.err;
        while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.remove_suffix(1);
        return makeError(ErrorCode::IoError, "git {} exited with {}: {}", *args.begin(), out.exitCode, err);
    }
    return std::move(out.out);
}

/// Git's view of a content root (07 §1.7), when it lies in a git work tree: nothing tracked under the cook's
/// output folders (the scan skips them because git ignores them; a force-added file there would escape it),
/// no tracked symbolic link or submodule, and every tracked binary source stored as a Git LFS pointer
/// rather than as a blob of its bytes. Blob sizes are read first, so a large binary is never loaded.
void checkGit(const fs::Path& root, const std::string& rootRel, const ContentOptions& options,
              ContentStats& stats, std::vector<Finding>& findings) {
    const auto report = [&](std::string_view rel, std::string message) {
        findings.push_back(Finding{std::format("{}/{}", rootRel, rel), std::move(message)});
    };
    const auto inside = runGit(root, {"rev-parse", "--is-inside-work-tree"});
    if (!inside || !(*inside == "true\n" || *inside == "true\r\n")) {
        if (options.requireGit)
            findings.push_back(Finding{
                rootRel, std::format("not checked against git ({}): --require-git needs the content root in a "
                                     "git work tree and git on PATH",
                                     inside ? "not inside a work tree" : inside.error().message)});
        return;
    }
    const auto listing = runGit(root, {"ls-files", "--stage", "-z", "--", "."});
    if (!listing) {
        findings.push_back(Finding{rootRel, listing.error().message});
        return;
    }
    ++stats.gitRoots;
    struct Tracked {
        std::string_view blob;
        std::string_view path;
    };
    std::vector<Tracked> binaries;
    std::string_view rest = *listing;
    while (!rest.empty()) {
        // "<mode> <object> <stage>\t<path>\0", the path relative to the content root.
        const usize end = std::min(rest.find('\0'), rest.size());
        const std::string_view entry = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        const usize tab = entry.find('\t');
        const usize space = entry.find(' ');
        if (tab == std::string_view::npos || space == std::string_view::npos || space > tab) {
            findings.push_back(Finding{rootRel, std::format("unexpected git ls-files output '{}'", entry)});
            continue;
        }
        const std::string_view mode = entry.substr(0, space);
        const std::string_view object = entry.substr(space + 1, entry.find(' ', space + 1) - space - 1);
        const std::string_view path = entry.substr(tab + 1);
        if (isCookOutput(path)) {
            report(path, "tracked by git in the cook's output folder, which the provenance scan skips: cook "
                         "output is not content (git rm --cached it)");
        } else if (mode == "120000") {
            report(path, "tracked by git as a symbolic link: content is plain files (07 §1.7)");
        } else if (mode != "100644" && mode != "100755") {
            report(path, std::format("tracked by git with mode {} (a submodule?): content is plain files", mode));
        } else if (binaryType(lowerExtension(path))) {
            binaries.push_back({object, path});
        }
    }
    if (binaries.empty()) return;

    std::string request;
    for (const Tracked& t : binaries) request += std::format("{}\n", t.blob);
    const auto sizes = runGit(root, {"cat-file", "--batch-check"}, request);
    if (!sizes) {
        findings.push_back(Finding{rootRel, sizes.error().message});
        return;
    }
    // "<object> blob <size>\n" per request line, in order.
    std::vector<const Tracked*> small;
    request.clear();
    rest = *sizes;
    for (const Tracked& t : binaries) {
        const usize nl = std::min(rest.find('\n'), rest.size());
        const std::string_view line = rest.substr(0, nl);
        rest.remove_prefix(std::min(nl + 1, rest.size()));
        const usize sp = line.rfind(' ');
        u64 size = 0;
        const std::string_view digits = sp == std::string_view::npos ? std::string_view() : line.substr(sp + 1);
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), size);
        if (!line.starts_with(t.blob) || line.find(" blob ") == std::string_view::npos || digits.empty() ||
            parsed.ptr != digits.data() + digits.size()) {
            report(t.path, std::format("git cat-file cannot read its blob ('{}')", line));
        } else if (size > kMaxLfsPointerBytes) {
            report(t.path, std::format("git stores it as a {}-byte blob, not as a Git LFS pointer: binary sources "
                                       "are Git LFS objects (07 §1.7; content/.gitattributes)",
                                       size));
        } else {
            small.push_back(&t);
            request += std::format("{}\n", t.blob);
        }
    }
    if (small.empty()) return;
    const auto blobs = runGit(root, {"cat-file", "--batch"}, request);
    if (!blobs) {
        findings.push_back(Finding{rootRel, blobs.error().message});
        return;
    }
    // "<object> blob <size>\n<content>\n" per request line, in order.
    rest = *blobs;
    for (const Tracked* t : small) {
        const usize nl = rest.find('\n');
        const usize sp = nl == std::string_view::npos ? nl : rest.rfind(' ', nl);
        u64 size = 0;
        if (sp == std::string_view::npos ||
            std::from_chars(rest.data() + sp + 1, rest.data() + nl, size).ptr != rest.data() + nl ||
            rest.size() - nl - 1 < size + 1) {
            report(t->path, "git cat-file --batch output ended early");
            return;
        }
        const std::string_view content = rest.substr(nl + 1, static_cast<usize>(size));
        rest.remove_prefix(nl + 1 + static_cast<usize>(size) + 1);
        if (isLfsPointer(content))
            ++stats.gitLfs;
        else
            report(t->path, "git stores it as a blob that is not a Git LFS pointer: binary sources are Git LFS "
                            "objects (07 §1.7; content/.gitattributes)");
    }
}

} // namespace

Result<assetpipe::ImporterRegistry> contentTypes() {
    assetpipe::ImporterRegistry types;
    HELIOS_TRY(types.add({.id = "hrec", .version = 1, .extensions = {".hrec"}}));
    HELIOS_TRY(types.add({.id = "hcont", .version = 1, .extensions = {".hcont"}}));
    HELIOS_TRY(types.add({.id = "hent", .version = 1, .extensions = {".hent"}}));
    HELIOS_TRY(types.add({.id = "md", .version = 1, .extensions = {".md"}}));
    for (const BinaryType& t : kBinaryTypes)
        HELIOS_TRY(types.add({.id = std::string(t.id), .version = 1, .extensions = {std::format(".{}", t.id)}}));
    return types;
}

ProjectFile checkProjectFile(const fs::Path& projectRoot, std::vector<Finding>& findings) {
    const Report r{std::string(kProjectFileName), &findings};
    ProjectFile project;
    const fs::Path path = projectRoot / fs::pathFromUtf8(kProjectFileName);
    if (!fs::isFile(path)) {
        r("", "missing: a project has its project file at its root (09 §2.7.2)");
        return project;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    auto text = readSmallText(path, ec ? 0 : static_cast<u64>(size));
    if (!text) {
        r("", text.error().message);
        return project;
    }
    auto doc = JsonDocument::parse(*text, kProjectFileName);
    if (!doc) {
        r("", doc.error().message);
        return project;
    }
    const JsonValue root = doc->root();
    if (!root.isObject()) {
        r("", std::format("expected an object, got {}", root.typeName()));
        return project;
    }
    checkKeys(
        root, "",
        {"$project", "engineVersion", "gems", "channels", "schemas", "contentRoots", "zones", "product"}, r);
    if (const JsonValue v = member(root, "", "$project", refl::JsonType::Number, true, r); v.isValid()) {
        u64 version = 0;
        if (!v.getU64(version))
            r("$project", "expected a format version");
        else if (version > kProjectVersion)
            r("$project",
              std::format("format version {} is newer than this build's ({})", version, kProjectVersion));
    }
    if (const JsonValue v = member(root, "", "engineVersion", refl::JsonType::String, true, r); v.isValid()) {
        if (!isSemver(v.asString())) r("engineVersion", "expected MAJOR.MINOR.PATCH (09 §2.7.2)");
    }
    if (const JsonValue v = member(root, "", "gems", refl::JsonType::Object, false, r); v.isValid())
        checkGems(projectRoot, v, r);
    if (const JsonValue v = member(root, "", "channels", refl::JsonType::Array, true, r); v.isValid()) {
        if (v.size() == 0) r("channels", "a project publishes to at least one channel");
        std::set<std::string_view> seen;
        for (const JsonValue c : v.elements()) {
            if (!c.isString() || !isIdent(c.asString(), 1, 32, "-"))
                r("channels", "a channel name is 1-32 characters of [a-z0-9-] that start with a letter");
            else if (!seen.insert(c.asString()).second)
                r("channels", std::format("channel '{}' is listed twice", c.asString()));
        }
    }
    if (const JsonValue v = member(root, "", "schemas", refl::JsonType::Object, false, r); v.isValid())
        checkSchemas(projectRoot, v, r);
    if (const JsonValue v = member(root, "", "contentRoots", refl::JsonType::Array, true, r); v.isValid())
        project.contentRoots = checkContentRoots(projectRoot, v, r);
    if (const JsonValue v = member(root, "", "zones", refl::JsonType::Array, true, r); v.isValid())
        project.zones = checkZones(v, r);
    if (const JsonValue v = member(root, "", "product", refl::JsonType::Object, true, r); v.isValid())
        checkProduct(v, r);
    return project;
}

ContentStats checkContent(const fs::Path& projectRoot, const ProjectFile& project,
                          const assetpipe::ImporterRegistry& types, std::vector<Finding>& findings,
                          const ContentOptions& options) {
    ContentStats stats;
    std::set<std::string_view> zones;
    for (const ZoneDecl& z : project.zones) zones.insert(z.name);
    for (const std::string& rootRel : project.contentRoots) {
        const fs::Path root = projectRoot / fs::pathFromUtf8(rootRel);
        const auto at = [&](std::string_view rel) { return std::format("{}/{}", rootRel, rel); };
        const auto report = [&](std::string_view rel, std::string message) {
            findings.push_back(Finding{at(rel), std::move(message)});
        };
        fs::ListOptions opts;
        opts.recursive = true; // directories too: the listing shows a linked one but does not enter it
        auto listing = fs::listDirectory(root, opts);
        if (!listing) {
            findings.push_back(Finding{rootRel, listing.error().message});
            continue;
        }
        std::vector<Doc> docs;
        std::set<std::string> hidden;
        for (const fs::DirEntry& e : *listing) {
            const std::string& rel = e.relativePath;
            // The cook outputs that .gitignore keeps out of git (/content/.cooked/, /content/.cache/); a
            // tracked file there is checkGit's.
            if (isCookOutput(rel)) continue;
            // The scan and the records cook do not follow a link into a directory, and a link (or junction)
            // can point outside the project: everything here is a plain file or directory.
            std::error_code ec;
            const std::filesystem::file_status st = std::filesystem::symlink_status(e.path, ec);
            if (ec || !(std::filesystem::is_regular_file(st) || std::filesystem::is_directory(st))) {
                report(rel, "a symbolic link, junction or special file: content is plain files and directories, "
                            "which the provenance scan reads (replace it with what it points to)");
                continue;
            }
            const std::vector<std::string_view> parts = components(rel);
            bool skip = false;
            for (usize i = 0; i < parts.size() && !skip; ++i) {
                if (parts[i].empty() || parts[i][0] != '.') continue;
                skip = true;
                // The root's own git settings are not content. Deeper ones are refused: they could mark a
                // content file binary or -text behind the root's rules.
                if (parts.size() == 1 && parts[0] == ".gitattributes") break;
                const std::string prefix(std::string_view(rel).substr(
                    0, static_cast<usize>(parts[i].data() - rel.data()) + parts[i].size()));
                if (hidden.insert(prefix).second) {
                    report(
                        prefix,
                        "hidden, so the provenance scan skips it while the cook can still read it: content "
                        "may not hide (rename or remove it)");
                }
            }
            if (skip || e.isDirectory) continue;
            if (lowerExtension(rel) == assetpipe::kMetaExtension) continue; // scanMetas checks sidecars
            ++stats.files;
            if (const std::string_view ext = rawExtension(rel); ext != lowerExtension(rel)) {
                report(rel, std::format("extension '{}' is not lower case: the content root's .gitattributes "
                                        "patterns (LF text, Git LFS) match lower case only",
                                        ext));
            }
            if (!types.forFile(rel)) {
                const std::string ext = lowerExtension(rel);
                report(rel, std::format(
                                "not a content document type the cook knows ({}): no importer claims it, so "
                                "it can carry no provenance (01 §5.2)",
                                ext.empty() ? "no extension" : ext));
                continue;
            }
            docs.push_back(Doc{rel, e.path, e.size});
        }

        // Provenance: engine/assetpipe's sidecar scan (01 §5.2, 07 T24).
        auto scan = assetpipe::scanMetas(root, types);
        if (!scan) {
            findings.push_back(Finding{rootRel, scan.error().message});
            continue;
        }
        for (const assetpipe::MetaProblem& p : scan->problems) {
            // parseMeta names the sidecar first; the finding's path already does.
            std::string_view message = p.message;
            if (message.starts_with(p.path) && message.substr(p.path.size()).starts_with(": "))
                message.remove_prefix(p.path.size() + 2);
            report(p.path, std::string(message));
        }
        std::map<std::string, Guid> metaGuids;
        for (const assetpipe::ScannedAsset& a : scan->assets) metaGuids.emplace(a.path, a.meta.guid);
        stats.withMeta += scan->assets.size();

        // Layout: 07 §1.8.2's collab.scope, records under records/, and containers and entities (02 §5.6).
        std::set<std::string> docPaths;
        for (const Doc& d : docs) docPaths.insert(d.rel);
        std::set<std::string> unknownZones;
        for (const Doc& d : docs) {
            const std::string ext = lowerExtension(d.rel);
            const bool spatial = ext == ".hcont" || ext == ".hent";
            const std::vector<std::string_view> parts = components(d.rel);
            std::string_view zone;
            if (parts[0] == "zones") {
                if (parts.size() < 3) {
                    report(d.rel, "directly in zones/: a zone's documents go in zones/<zone>/ (07 §1.8.2)");
                } else if (!zones.contains(parts[1])) {
                    if (unknownZones.insert(std::string(parts[1])).second)
                        report(std::format("zones/{}", parts[1]),
                               "not a zone of helios.project.jsonc: a zone folder holds one declared zone's "
                               "spatial documents (07 §1.8.2)");
                } else {
                    zone = parts[1];
                }
                if (!spatial && parts.size() >= 3)
                    report(
                        d.rel,
                        std::format("a {} document in a zone folder: only spatial documents (.hcont, .hent) "
                                    "live under zones/<zone>/ (07 §1.8.2 collab.scope)",
                                    ext));
            } else if (spatial) {
                report(d.rel,
                       "a spatial document outside zones/<zone>/: containers and their entities live in "
                       "their zone's folder (07 §1.8.2 collab.scope)");
            }
            if (ext == ".hrec" && parts[0] != "records")
                report(d.rel, "a record outside records/<table>/, which the records cook never reads");
            const Report r{at(d.rel), &findings};
            if (ext == ".hcont") {
                checkContainer(d, zone, metaGuids, r);
                ++stats.containers;
            } else if (ext == ".hent") {
                checkEntity(d, docPaths, metaGuids, r);
                ++stats.entities;
            } else if (const BinaryType* type = binaryType(ext)) {
                checkBinarySource(d, *type, stats, r);
                ++stats.binaries;
            }
        }
        checkGit(root, rootRel, options, stats, findings);
    }
    return stats;
}

} // namespace helios::cook
