// `.meta` sidecars v0 (see meta.h).

#include "helios/assetpipe/meta.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <new>
#include <unordered_map>

#include "helios/asset/asset_id.h"
#include "helios/core/log.h"
#include "helios/core/vfs.h"
#include "helios/reflect/json.h"
#include "helios/reflect/serialize.h"

namespace helios::assetpipe {

namespace {

using refl::JsonValue;
using refl::ReadCtx;

constexpr std::array<std::string_view, 9> kLicences = {
    "MIT", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "Zlib", "BSL-1.0", "ISC", "PostgreSQL", "CC0-1.0"};
constexpr std::string_view kFontLicence = "OFL-1.1";

char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

std::string foldCase(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lowerAscii(c);
    return out;
}

bool blank(std::string_view s) noexcept {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

/// A path or label for an error message: quoted, control characters escaped, at most 120 bytes.
std::string shown(std::string_view s) {
    std::string out = "'";
    for (const char ch : s.substr(0, 120)) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) {
            out += std::format("\\x{:02x}", c);
        } else {
            out += ch;
        }
    }
    if (s.size() > 120) out += "...";
    out += "'";
    return out;
}

Error prefixed(std::string_view prefix, const Error& e) {
    return Error{e.code, std::string(prefix) + ": " + e.message};
}

Error fieldError(ErrorCode code, std::string_view field, std::string_view message) {
    return Error{code, std::format("{}: {}", field, message)};
}

std::string_view fileName(std::string_view path) noexcept {
    const usize slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view parentOf(std::string_view path) noexcept {
    const usize slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
}

fs::Path absolute(const fs::Path& root, std::string_view rel) {
    return root / fs::pathFromUtf8(rel);
}

// ---------------------------------------------------------------------------------------------
// Settings through the importer's reflected type
// ---------------------------------------------------------------------------------------------

/// A default-constructed object of a reflected type, destroyed with it.
class TypedObject {
public:
    explicit TypedObject(const refl::TypeInfo& type)
        : m_type(type), m_align(std::max<usize>(type.align, alignof(std::max_align_t))),
          m_ptr(::operator new(std::max<usize>(type.size, 1), std::align_val_t(m_align))) {
        type.ops->construct(m_ptr);
    }
    ~TypedObject() {
        m_type.ops->destruct(m_ptr);
        ::operator delete(m_ptr, std::align_val_t(m_align));
    }
    TypedObject(const TypedObject&) = delete;
    TypedObject& operator=(const TypedObject&) = delete;
    void* get() const noexcept { return m_ptr; }

private:
    const refl::TypeInfo& m_type;
    usize m_align;
    void* m_ptr;
};

/// Reads a settings object through the importer's type (unknown fields are errors under `ctx`, which is
/// strict) and writes it with `out`.
Result<void> writeSettingsValue(const ImporterInfo& importer, JsonValue value, ReadCtx& ctx,
                                refl::JsonWriter& out) {
    if (!value.isObject()) return ctx.typeError("object", value);
    if (!importer.settings) {
        if (value.size() != 0) return ctx.error(std::format("importer '{}' takes no settings", importer.id));
        out.beginObject();
        out.endObject();
        return {};
    }
    TypedObject object(*importer.settings);
    HELIOS_TRY(refl::readJson(*importer.settings, object.get(), value, ctx));
    refl::writeJson(*importer.settings, object.get(), out);
    return {};
}

Result<std::string> resolveSettingsValue(const ImporterInfo& importer, JsonValue value, ReadCtx& ctx) {
    refl::JsonWriter out(refl::JsonStyle::Compact);
    HELIOS_TRY(writeSettingsValue(importer, value, ctx, out));
    return out.take();
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------

/// Every object in the document has unique keys (yyjson keeps duplicates; a sidecar must not mean two
/// things). The document is at most refl::kMaxJsonDepth deep, which bounds the recursion.
Result<void> checkDuplicateKeys(JsonValue value, ReadCtx& ctx) {
    if (value.isArray()) {
        usize i = 0;
        for (const JsonValue e : value.elements()) {
            ReadCtx::Scope scope(ctx, i++);
            HELIOS_TRY(checkDuplicateKeys(e, ctx));
        }
    } else if (value.isObject()) {
        std::vector<std::string_view> keys;
        for (const auto m : value.members()) keys.push_back(m.key);
        std::sort(keys.begin(), keys.end());
        if (const auto dup = std::adjacent_find(keys.begin(), keys.end()); dup != keys.end()) {
            return ctx.error(std::format("duplicate key {}", shown(*dup)));
        }
        for (const auto m : value.members()) {
            ReadCtx::Scope scope(ctx, m.key);
            HELIOS_TRY(checkDuplicateKeys(m.value, ctx));
        }
    }
    return {};
}

Result<void> checkKnownKeys(JsonValue object, std::span<const std::string_view> known, ReadCtx& ctx) {
    for (const auto m : object.members()) {
        if (std::find(known.begin(), known.end(), m.key) == known.end()) {
            ReadCtx::Scope scope(ctx, m.key);
            return ctx.error(
                "unknown field (a sidecar fails closed: check the spelling or the $meta version)");
        }
    }
    return {};
}

/// The string member `key` of `object`: empty when absent and optional, an error when absent and required.
Result<std::string> readString(JsonValue object, std::string_view key, bool required, ReadCtx& ctx) {
    const JsonValue v = object.get(key);
    if (!v.isValid()) {
        if (required) return ctx.error(std::format("missing required field '{}'", key));
        return std::string();
    }
    ReadCtx::Scope scope(ctx, key);
    if (!v.isString()) return ctx.typeError("string", v);
    return std::string(v.asString());
}

Result<std::vector<std::string>> readStringList(JsonValue object, std::string_view key, ReadCtx& ctx) {
    std::vector<std::string> out;
    const JsonValue v = object.get(key);
    if (!v.isValid()) return out;
    ReadCtx::Scope scope(ctx, key);
    if (!v.isArray()) return ctx.typeError("array of strings", v);
    usize i = 0;
    for (const JsonValue e : v.elements()) {
        ReadCtx::Scope at(ctx, i++);
        if (!e.isString()) return ctx.typeError("string", e);
        out.emplace_back(e.asString());
    }
    return out;
}

Result<u64> readUnsigned(JsonValue object, std::string_view key, ReadCtx& ctx) {
    const JsonValue v = object.get(key);
    if (!v.isValid()) return ctx.error(std::format("missing required field '{}'", key));
    ReadCtx::Scope scope(ctx, key);
    u64 out = 0;
    if (!v.getU64(out)) return ctx.typeError("non-negative integer", v);
    return out;
}

Result<Origin> parseOrigin(std::string_view text, ReadCtx& ctx) {
    for (const Origin o : {Origin::Original, Origin::Commissioned, Origin::Cc0, Origin::AiAssisted}) {
        if (text == originName(o)) return o;
    }
    ReadCtx::Scope scope(ctx, "origin");
    return ctx.error(
        std::format("{} is not an origin (original, commissioned, cc0, ai-assisted)", shown(text)));
}

Result<Provenance> readProvenance(JsonValue object, ReadCtx& ctx) {
    if (!object.isObject()) return ctx.typeError("object", object);
    static constexpr std::array<std::string_view, 7> kKeys = {"origin", "author", "licence", "url",
                                                              "rights", "ai",     "notes"};
    HELIOS_TRY(checkKnownKeys(object, kKeys, ctx));
    Provenance p;
    HELIOS_TRY_ASSIGN(const std::string origin, readString(object, "origin", true, ctx));
    HELIOS_TRY_ASSIGN(p.origin, parseOrigin(origin, ctx));
    HELIOS_TRY_ASSIGN(p.author, readString(object, "author", true, ctx));
    HELIOS_TRY_ASSIGN(p.licence, readString(object, "licence", true, ctx));
    HELIOS_TRY_ASSIGN(p.url, readString(object, "url", false, ctx));
    HELIOS_TRY_ASSIGN(p.rights, readString(object, "rights", false, ctx));
    HELIOS_TRY_ASSIGN(p.notes, readString(object, "notes", false, ctx));
    if (const JsonValue ai = object.get("ai"); ai.isValid()) {
        ReadCtx::Scope scope(ctx, "ai");
        if (!ai.isObject()) return ctx.typeError("object", ai);
        static constexpr std::array<std::string_view, 4> kAiKeys = {"tool", "model", "prompt", "inputs"};
        HELIOS_TRY(checkKnownKeys(ai, kAiKeys, ctx));
        AiProvenance a;
        HELIOS_TRY_ASSIGN(a.tool, readString(ai, "tool", true, ctx));
        HELIOS_TRY_ASSIGN(a.model, readString(ai, "model", true, ctx));
        HELIOS_TRY_ASSIGN(a.prompt, readString(ai, "prompt", true, ctx));
        HELIOS_TRY_ASSIGN(a.inputs, readStringList(ai, "inputs", ctx));
        p.ai = std::move(a);
    }
    return p;
}

Result<AssetMeta> readMeta(JsonValue root, const ImporterRegistry& importers, ReadCtx& ctx) {
    if (!root.isObject()) return ctx.typeError("object", root);
    HELIOS_TRY(checkDuplicateKeys(root, ctx));
    static constexpr std::array<std::string_view, 8> kKeys = {
        "$meta", "guid", "importer", "importerVersion", "settings", "labels", "source", "provenance"};
    HELIOS_TRY(checkKnownKeys(root, kKeys, ctx));

    HELIOS_TRY_ASSIGN(const u64 format, readUnsigned(root, "$meta", ctx));
    if (format != kMetaVersion) {
        return fieldError(ErrorCode::VersionMismatch, "$meta",
                          std::format("sidecar format {} is not this build's ({})", format, kMetaVersion));
    }
    AssetMeta meta;
    HELIOS_TRY_ASSIGN(const std::string guidText, readString(root, "guid", true, ctx));
    {
        auto guid = Guid::parse(guidText);
        if (!guid)
            return fieldError(ErrorCode::ParseError, "guid",
                              std::format("{} is not a GUID", shown(guidText)));
        if (guid->toString() != guidText) {
            return fieldError(
                ErrorCode::ParseError, "guid",
                std::format("{} is not in canonical form; write \"{}\"", shown(guidText), *guid));
        }
        meta.guid = *guid;
    }
    HELIOS_TRY_ASSIGN(meta.importer, readString(root, "importer", true, ctx));
    const ImporterInfo* importer = importers.find(meta.importer);
    if (!importer) {
        return fieldError(ErrorCode::NotFound, "importer",
                          std::format("{} is not a registered importer", shown(meta.importer)));
    }
    HELIOS_TRY_ASSIGN(const u64 version, readUnsigned(root, "importerVersion", ctx));
    if (version > 0xFFFFFFFFull)
        return fieldError(ErrorCode::ParseError, "importerVersion", "above 4294967295");
    meta.importerVersion = static_cast<u32>(version);

    if (const JsonValue settings = root.get("settings"); settings.isValid()) {
        ReadCtx::Scope scope(ctx, "settings");
        HELIOS_TRY_ASSIGN(meta.settings, resolveSettingsValue(*importer, settings, ctx));
    } else {
        HELIOS_TRY_ASSIGN(meta.settings, resolveSettings(*importer, "{}"));
    }
    HELIOS_TRY_ASSIGN(meta.labels, readStringList(root, "labels", ctx));
    std::sort(meta.labels.begin(), meta.labels.end());
    meta.labels.erase(std::unique(meta.labels.begin(), meta.labels.end()), meta.labels.end());
    HELIOS_TRY_ASSIGN(meta.source, readString(root, "source", false, ctx));

    const JsonValue provenance = root.get("provenance");
    if (!provenance.isValid())
        return ctx.error("missing required field 'provenance' (01 §5.2: every asset records it)");
    {
        ReadCtx::Scope scope(ctx, "provenance");
        HELIOS_TRY_ASSIGN(meta.provenance, readProvenance(provenance, ctx));
    }
    HELIOS_TRY(validateMeta(meta, importers));
    return meta;
}

// ---------------------------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------------------------

Result<void> checkText(std::string_view field, std::string_view value) {
    if (blank(value)) return fieldError(ErrorCode::InvalidArgument, field, "must not be empty");
    return {};
}

Result<void> checkProvenance(const Provenance& p, const ImporterInfo& importer) {
    HELIOS_TRY(checkText("provenance.author", p.author));
    if (auto ok = checkLicence(p.licence, importer.fonts); !ok)
        return prefixed("provenance.licence", ok.error());
    switch (p.origin) {
    case Origin::Original: break;
    case Origin::Commissioned:
        if (blank(p.rights)) {
            return fieldError(ErrorCode::InvalidArgument, "provenance.rights",
                              "a commissioned asset records how its rights were assigned");
        }
        break;
    case Origin::Cc0:
        if (blank(p.url)) {
            return fieldError(ErrorCode::InvalidArgument, "provenance.url",
                              "a cc0 asset records where it was obtained");
        }
        if (p.licence != "CC0-1.0") {
            return fieldError(ErrorCode::InvalidArgument, "provenance.licence",
                              std::format("a cc0 asset keeps licence 'CC0-1.0', not {}", shown(p.licence)));
        }
        break;
    case Origin::AiAssisted:
        if (!p.ai) {
            return fieldError(ErrorCode::InvalidArgument, "provenance.ai",
                              "an ai-assisted asset records the tool, model, prompt and inputs (09 §4.2)");
        }
        break;
    default:
        return fieldError(ErrorCode::InvalidArgument, "provenance.origin",
                          std::format("unknown origin {}", static_cast<u32>(p.origin)));
    }
    if (p.ai) {
        if (p.origin != Origin::AiAssisted) {
            return fieldError(ErrorCode::InvalidArgument, "provenance.ai",
                              "only an ai-assisted asset has 'ai'");
        }
        HELIOS_TRY(checkText("provenance.ai.tool", p.ai->tool));
        HELIOS_TRY(checkText("provenance.ai.model", p.ai->model));
        HELIOS_TRY(checkText("provenance.ai.prompt", p.ai->prompt));
        for (usize i = 0; i < p.ai->inputs.size(); ++i) {
            HELIOS_TRY(checkText(std::format("provenance.ai.inputs[{}]", i), p.ai->inputs[i]));
        }
    }
    return {};
}

Result<void> checkLabels(const std::vector<std::string>& labels) {
    if (labels.size() > kMaxLabels) {
        return fieldError(ErrorCode::LimitExceeded, "labels",
                          std::format("{} labels (at most {})", labels.size(), kMaxLabels));
    }
    for (usize i = 0; i < labels.size(); ++i) {
        const std::string& l = labels[i];
        const std::string field = std::format("labels[{}]", i);
        if (l.empty() || l.size() > kMaxLabelBytes || blank(l)) {
            return fieldError(ErrorCode::InvalidArgument, field,
                              std::format("{}: a label is 1-{} bytes", shown(l), kMaxLabelBytes));
        }
        for (const char ch : l) {
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) {
                return fieldError(ErrorCode::InvalidArgument, field,
                                  std::format("{} has a control character", shown(l)));
            }
        }
        if (i > 0 && !(labels[i - 1] < l)) {
            return fieldError(ErrorCode::InvalidArgument, "labels", "must be sorted and unique");
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------

void writeOptionalString(refl::JsonWriter& w, std::string_view key, std::string_view value) {
    if (value.empty()) return;
    w.key(key);
    w.string(value);
}

Result<std::string> writeMetaText(const AssetMeta& meta, const ImporterInfo& importer) {
    refl::JsonWriter w(refl::JsonStyle::Pretty);
    w.beginObject();
    w.key("$meta");
    w.unsignedInteger(kMetaVersion);
    w.key("guid");
    w.string(meta.guid.toString());
    w.key("importer");
    w.string(meta.importer);
    w.key("importerVersion");
    w.unsignedInteger(meta.importerVersion);
    if (meta.settings != "{}") {
        // Through the importer's type again, so the layout is reflect's canonical one (02 §3.7).
        HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(meta.settings, "settings"));
        ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
        ReadCtx::Scope scope(ctx, "settings");
        w.key("settings");
        HELIOS_TRY(writeSettingsValue(importer, doc.root(), ctx, w));
    }
    if (!meta.labels.empty()) {
        w.key("labels");
        w.beginArray(true);
        for (const std::string& l : meta.labels) w.string(l);
        w.endArray();
    }
    writeOptionalString(w, "source", meta.source);
    const Provenance& p = meta.provenance;
    w.key("provenance");
    w.beginObject();
    w.key("origin");
    w.string(originName(p.origin));
    w.key("author");
    w.string(p.author);
    w.key("licence");
    w.string(p.licence);
    writeOptionalString(w, "url", p.url);
    writeOptionalString(w, "rights", p.rights);
    if (p.ai) {
        w.key("ai");
        w.beginObject();
        w.key("tool");
        w.string(p.ai->tool);
        w.key("model");
        w.string(p.ai->model);
        w.key("prompt");
        w.string(p.ai->prompt);
        if (!p.ai->inputs.empty()) {
            w.key("inputs");
            w.beginArray(true);
            for (const std::string& in : p.ai->inputs) w.string(in);
            w.endArray();
        }
        w.endObject();
    }
    writeOptionalString(w, "notes", p.notes);
    w.endObject();
    w.endObject();
    return w.take();
}

/// The "guid" of a sidecar that may not validate, so saveMeta() can still refuse to change it.
std::optional<Guid> peekGuid(std::string_view text) {
    auto doc = refl::JsonDocument::parse(text);
    if (!doc) return std::nullopt;
    const JsonValue g = doc->root().get("guid");
    if (!g.isString()) return std::nullopt;
    auto guid = Guid::parse(g.asString());
    if (!guid || guid->isNil()) return std::nullopt;
    return *guid;
}

Result<std::string> readSidecar(const fs::Path& path) {
    HELIOS_TRY_ASSIGN(const u64 size, fs::fileSize(path));
    if (size > kMaxMetaBytes) {
        return makeError(ErrorCode::LimitExceeded, "{}: {} bytes (a sidecar is at most {})",
                         fs::pathToGenericUtf8(path), size, kMaxMetaBytes);
    }
    return fs::readTextFile(path);
}

/// Names in `dir` (a project-relative directory, "" for the root) that equal `name` ignoring ASCII case.
std::vector<std::string> namesIgnoringCase(const fs::Path& root, std::string_view dir,
                                           std::string_view name) {
    std::vector<std::string> out;
    const fs::Path abs = dir.empty() ? root : absolute(root, dir);
    if (!fs::isDirectory(abs)) return out;
    auto listing = fs::listDirectory(abs);
    if (!listing) return out;
    for (const fs::DirEntry& e : *listing) {
        if (equalsIgnoringCase(e.relativePath, name)) out.push_back(e.relativePath);
    }
    return out;
}

std::string joinRel(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    return std::string(dir) + "/" + std::string(name);
}

/// A temporary sibling name for the two-step rename of a case-only change.
std::string movingName(std::string_view path) {
    return std::string(path) + ".moving-" + Guid::generate().toString();
}

Result<void> renameRel(const fs::Path& root, std::string_view from, std::string_view to, bool viaTemp) {
    const fs::Path a = absolute(root, from);
    const fs::Path b = absolute(root, to);
    if (!viaTemp) return fs::rename(a, b);
    const fs::Path tmp = absolute(root, movingName(to));
    HELIOS_TRY(fs::rename(a, tmp));
    if (auto r = fs::rename(tmp, b); !r) {
        (void)fs::rename(tmp, a);
        return r;
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

std::string_view originName(Origin origin) noexcept {
    switch (origin) {
    case Origin::Original: return "original";
    case Origin::Commissioned: return "commissioned";
    case Origin::Cc0: return "cc0";
    case Origin::AiAssisted: return "ai-assisted";
    }
    return "unknown";
}

std::span<const std::string_view> allowedLicences() noexcept {
    return kLicences;
}

Result<void> checkLicence(std::string_view spdx, bool fontImporter) {
    if (std::find(kLicences.begin(), kLicences.end(), spdx) != kLicences.end()) return {};
    if (spdx == kFontLicence) {
        if (fontImporter) return {};
        return makeError(ErrorCode::InvalidArgument, "{} is allowed for font assets only (ADR-010)",
                         shown(spdx));
    }
    for (const std::string_view id : kLicences) {
        if (equalsIgnoringCase(id, spdx)) {
            return makeError(ErrorCode::InvalidArgument, "{}: write the SPDX id '{}'", shown(spdx), id);
        }
    }
    if (equalsIgnoringCase(kFontLicence, spdx)) {
        return makeError(ErrorCode::InvalidArgument, "{}: write the SPDX id '{}' (fonts only)", shown(spdx),
                         kFontLicence);
    }
    return makeError(
        ErrorCode::InvalidArgument,
        "{} is not an allowed licence: 01 §5.2 allows MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Zlib, "
        "BSL-1.0, ISC, PostgreSQL and CC0-1.0 (and OFL-1.1 for fonts, ADR-010)",
        shown(spdx));
}

Result<void> checkProjectPath(std::string_view path) {
    const auto refuse = [&](std::string_view why) {
        return Error{ErrorCode::InvalidArgument, std::format("{}: {}", shown(path), why)};
    };
    if (path.empty()) return refuse("empty path");
    if (path.front() == '/')
        return refuse("an absolute path; project paths are relative to the project root");
    if (path.back() == '/') return refuse("ends in '/', so it names a directory");
    usize start = 0;
    for (usize i = 0; i <= path.size(); ++i) {
        if (i < path.size()) {
            const auto c = static_cast<unsigned char>(path[i]);
            const auto next = i + 1 < path.size() ? static_cast<unsigned char>(path[i + 1]) : 0;
            if (c < 0x20 || c == 0x7F || (c == 0xC2 && next >= 0x80 && next <= 0x9F))
                return refuse("a control character");
            if (c == '\\') return refuse("'\\' is not a separator here; write '/'");
            if (std::string_view(R"(<>:"|?*)").find(static_cast<char>(c)) != std::string_view::npos) {
                return refuse(
                    std::format("'{}' is not allowed in a Windows file name", static_cast<char>(c)));
            }
            if (c != '/') continue;
        }
        const std::string_view part = path.substr(start, i - start);
        start = i + 1;
        if (part.empty()) return refuse("an empty component ('//')");
        if (part == "." || part == "..") return refuse("a '.' or '..' component");
        if (part.size() > 255)
            return refuse("a component longer than 255 bytes (NTFS allows 255 UTF-16 units)");
        if (fs::isNonPortableComponent(part)) {
            return refuse(std::format("{} is a Windows device name or ends in '.' or ' '", shown(part)));
        }
    }
    return {};
}

Result<std::string> resolveSettings(const ImporterInfo& importer, std::string_view settingsJson) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(settingsJson, "settings"));
    ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
    ReadCtx::Scope scope(ctx, "settings");
    HELIOS_TRY(checkDuplicateKeys(doc.root(), ctx));
    return resolveSettingsValue(importer, doc.root(), ctx);
}

Result<void> validateMeta(const AssetMeta& meta, const ImporterRegistry& importers) {
    if (meta.guid.isNil()) return fieldError(ErrorCode::InvalidArgument, "guid", "the nil GUID");
    const ImporterInfo* importer = importers.find(meta.importer);
    if (!importer) {
        return fieldError(ErrorCode::NotFound, "importer",
                          std::format("{} is not a registered importer", shown(meta.importer)));
    }
    if (meta.importerVersion == 0)
        return fieldError(ErrorCode::InvalidArgument, "importerVersion", "versions start at 1");
    if (meta.importerVersion > importer->version) {
        return fieldError(ErrorCode::VersionMismatch, "importerVersion",
                          std::format("written for '{}' version {}, but this build has version {}",
                                      importer->id, meta.importerVersion, importer->version));
    }
    auto canonical = resolveSettings(*importer, meta.settings);
    if (!canonical) return canonical.error();
    if (*canonical != meta.settings) {
        return fieldError(ErrorCode::InvalidArgument, "settings",
                          std::format("not canonical: {} (resolveSettings() gives it)", *canonical));
    }
    HELIOS_TRY(checkLabels(meta.labels));
    if (!meta.source.empty()) {
        if (auto ok = checkProjectPath(meta.source); !ok) return prefixed("source", ok.error());
    }
    return checkProvenance(meta.provenance, *importer);
}

Result<AssetMeta> parseMeta(std::string_view text, const ImporterRegistry& importers,
                            std::string_view sourceName) {
    if (text.size() > kMaxMetaBytes) {
        return makeError(ErrorCode::LimitExceeded, "{}: {} bytes (a sidecar is at most {})", sourceName,
                         text.size(), kMaxMetaBytes);
    }
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(text, sourceName));
    ReadCtx ctx(ReadCtx::Options{.strictUnknownFields = true});
    auto meta = readMeta(doc.root(), importers, ctx);
    if (!meta) return prefixed(sourceName, meta.error());
    return meta;
}

Result<std::string> writeMeta(const AssetMeta& meta, const ImporterRegistry& importers) {
    HELIOS_TRY(validateMeta(meta, importers));
    return writeMetaText(meta, *importers.find(meta.importer));
}

std::string metaPathFor(std::string_view sourcePath) {
    return std::string(sourcePath) + std::string(kMetaExtension);
}

Result<AssetMeta> loadMeta(const fs::Path& root, std::string_view path, const ImporterRegistry& importers) {
    HELIOS_TRY(checkProjectPath(path));
    const std::string metaRel = metaPathFor(path);
    const fs::Path metaAbs = absolute(root, metaRel);
    if (!fs::isFile(metaAbs))
        return makeError(ErrorCode::NotFound, "{} has no sidecar ({})", shown(path), shown(metaRel));
    HELIOS_TRY_ASSIGN(const std::string text, readSidecar(metaAbs));
    return parseMeta(text, importers, metaRel);
}

Result<void> saveMeta(const fs::Path& root, std::string_view path, const AssetMeta& meta,
                      const ImporterRegistry& importers) {
    HELIOS_TRY(checkProjectPath(path));
    HELIOS_TRY_ASSIGN(const std::string text, writeMeta(meta, importers));
    const std::string metaRel = metaPathFor(path);
    const fs::Path metaAbs = absolute(root, metaRel);
    if (fs::exists(metaAbs)) {
        HELIOS_TRY_ASSIGN(const std::string existing, readSidecar(metaAbs));
        if (existing == text) return {};
        const std::optional<Guid> old = peekGuid(existing);
        if (!old) {
            return makeError(ErrorCode::InvalidState,
                             "{}: the existing sidecar has no readable GUID, so saving could "
                             "change it; repair or remove it first",
                             shown(metaRel));
        }
        if (*old != meta.guid) {
            return makeError(ErrorCode::InvalidState, "{}: its GUID is {}, not {}; a GUID never changes",
                             shown(metaRel), *old, meta.guid);
        }
    }
    return fs::writeTextFile(metaAbs, text);
}

Result<EnsuredMeta> ensureMeta(const fs::Path& root, std::string_view path, const NewMeta& init,
                               const ImporterRegistry& importers) {
    HELIOS_TRY(checkProjectPath(path));
    if (!fs::isFile(absolute(root, path)))
        return makeError(ErrorCode::NotFound, "{}: no such source file", shown(path));
    const std::string metaRel = metaPathFor(path);
    if (fs::exists(absolute(root, metaRel))) {
        HELIOS_TRY_ASSIGN(AssetMeta meta, loadMeta(root, path, importers));
        return EnsuredMeta{std::move(meta), false};
    }
    for (const std::string& other : namesIgnoringCase(root, parentOf(path), fileName(metaRel))) {
        if (other != fileName(metaRel)) {
            return makeError(
                ErrorCode::InvalidState,
                "{}: its sidecar is spelled {}; Windows sees one file, so rename it instead of minting a "
                "second GUID",
                shown(path), shown(joinRel(parentOf(path), other)));
        }
    }
    const ImporterInfo* importer =
        init.importer.empty() ? importers.forFile(path) : importers.find(init.importer);
    if (!importer) {
        return makeError(ErrorCode::NotFound, "{}: {}", shown(path),
                         init.importer.empty()
                             ? std::string("no importer claims its extension")
                             : std::format("{} is not a registered importer", shown(init.importer)));
    }
    if (!importsFile(*importer, path)) {
        return makeError(ErrorCode::InvalidArgument, "{}: importer '{}' does not import this extension",
                         shown(path), importer->id);
    }
    AssetMeta meta;
    meta.guid = Guid::generate();
    meta.importer = importer->id;
    meta.importerVersion = importer->version;
    HELIOS_TRY_ASSIGN(meta.settings, resolveSettings(*importer, init.settings));
    meta.labels = init.labels;
    std::sort(meta.labels.begin(), meta.labels.end());
    meta.labels.erase(std::unique(meta.labels.begin(), meta.labels.end()), meta.labels.end());
    meta.source = init.source;
    meta.provenance = init.provenance;
    auto text = writeMeta(meta, importers);
    if (!text) return prefixed(metaRel, text.error());
    HELIOS_TRY(fs::writeTextFile(absolute(root, metaRel), *text));
    return EnsuredMeta{std::move(meta), true};
}

Result<void> moveAsset(const fs::Path& root, std::string_view from, std::string_view to,
                       const ImporterRegistry& importers) {
    HELIOS_TRY(checkProjectPath(from));
    HELIOS_TRY(checkProjectPath(to));
    HELIOS_TRY_ASSIGN(const AssetMeta meta, loadMeta(root, from, importers));
    if (!fs::isFile(absolute(root, from)))
        return makeError(ErrorCode::NotFound, "{}: no such source file", shown(from));
    if (from == to) return {};
    const ImporterInfo* importer = importers.find(meta.importer);
    if (!importsFile(*importer, to)) {
        return makeError(ErrorCode::InvalidArgument, "{}: importer '{}' of {} does not import this extension",
                         shown(to), importer->id, shown(from));
    }
    const std::string fromMeta = metaPathFor(from);
    const std::string toMeta = metaPathFor(to);
    // Only a rename within one directory (spelled the same) counts as case-only; a change in a directory's
    // case is a move, so on Windows it finds the file itself in the target and is refused.
    const bool caseOnly = parentOf(from) == parentOf(to) && equalsIgnoringCase(fileName(from), fileName(to));
    // A target is taken when its directory holds the name in any ASCII case (Windows sees one file), except
    // for the file being renamed itself in a case-only rename.
    for (const std::string_view target : {std::string_view(to), std::string_view(toMeta)}) {
        const std::string_view self = target == to ? fileName(from) : fileName(fromMeta);
        for (const std::string& name : namesIgnoringCase(root, parentOf(target), fileName(target))) {
            if (caseOnly && name == self) continue;
            return makeError(ErrorCode::AlreadyExists, "{}: {} is already there; Windows sees one file",
                             shown(target), shown(joinRel(parentOf(target), name)));
        }
    }
    if (!parentOf(to).empty()) HELIOS_TRY(fs::createDirectories(absolute(root, parentOf(to))));
    HELIOS_TRY(renameRel(root, from, to, caseOnly));
    if (auto moved = renameRel(root, fromMeta, toMeta, caseOnly); !moved) {
        if (auto back = renameRel(root, to, from, caseOnly); !back) {
            HELIOS_LOG_ERROR(
                "moveAsset: {} moved but its sidecar did not ({}), and moving it back failed ({})", shown(to),
                moved.error().toString(), back.error().toString());
        }
        return moved;
    }
    return {};
}

Result<MetaScan> scanMetas(const fs::Path& root, const ImporterRegistry& importers,
                           asset::AssetIdSet::FoldFn fold) {
    fs::ListOptions options;
    options.recursive = true;
    options.includeDirectories = false;
    HELIOS_TRY_ASSIGN(const std::vector<fs::DirEntry> listing, fs::listDirectory(root, options));

    MetaScan scan;
    const auto problem = [&](std::string_view path, ErrorCode code, std::string message) {
        scan.problems.push_back(MetaProblem{std::string(path), code, std::move(message)});
    };
    std::vector<std::string> sources;
    std::vector<std::string> sidecars;
    std::unordered_map<std::string, std::string> files;  // exact path -> itself (membership)
    std::unordered_map<std::string, std::string> folded; // case-folded path -> first path seen
    for (const fs::DirEntry& e : listing) {
        const std::string& rel = e.relativePath;
        // Hidden directories and files (.git, .helios, .gitattributes) are not content.
        if (rel.front() == '.' || rel.find("/.") != std::string::npos) continue;
        const bool sidecar =
            rel.size() > kMetaExtension.size() &&
            equalsIgnoringCase(std::string_view(rel).substr(rel.size() - kMetaExtension.size()),
                               kMetaExtension);
        if (!sidecar && !importers.forFile(rel)) continue; // not an asset (records, scripts, ...)
        files.emplace(rel, rel);
        if (auto ok = checkProjectPath(rel); !ok) problem(rel, ok.error().code, ok.error().message);
        if (auto [it, fresh] = folded.emplace(foldCase(rel), rel); !fresh) {
            problem(rel, ErrorCode::AlreadyExists,
                    std::format("{} and {} differ only in case; Windows sees one file", shown(it->second),
                                shown(rel)));
        }
        if (!sidecar) {
            sources.push_back(rel);
        } else if (!rel.ends_with(kMetaExtension)) {
            problem(rel, ErrorCode::InvalidArgument, "a sidecar's extension is '.meta' in lower case");
        } else {
            sidecars.push_back(rel);
        }
    }
    const auto otherCase = [&](const std::string& path) -> const std::string* {
        const auto it = folded.find(foldCase(path));
        return it == folded.end() || it->second == path ? nullptr : &it->second;
    };
    for (const std::string& s : sources) {
        const std::string m = metaPathFor(s);
        if (files.contains(m)) continue;
        if (const std::string* actual = otherCase(m)) {
            problem(s, ErrorCode::InvalidArgument,
                    std::format("its sidecar is spelled {}; rename it to {}", shown(*actual), shown(m)));
        } else {
            problem(s, ErrorCode::NotFound, "no sidecar, so the asset has no GUID (import it to create one)");
        }
    }
    asset::AssetIdSet ids(fold);
    std::map<Guid, std::string> byGuid;
    for (const std::string& m : sidecars) {
        const std::string s = m.substr(0, m.size() - kMetaExtension.size());
        if (!files.contains(s) && !fs::isFile(absolute(root, s))) {
            if (!otherCase(s))
                problem(m, ErrorCode::NotFound, std::format("orphan sidecar: no source {}", shown(s)));
            continue;
        }
        auto text = readSidecar(absolute(root, m));
        if (!text) {
            problem(m, text.error().code, text.error().message);
            continue;
        }
        auto meta = parseMeta(*text, importers, m);
        if (!meta) {
            problem(m, meta.error().code, meta.error().message);
            continue;
        }
        if (!importsFile(*importers.find(meta->importer), s)) {
            problem(m, ErrorCode::InvalidArgument,
                    std::format("importer '{}' does not import {}", meta->importer, shown(s)));
            continue;
        }
        if (auto [it, fresh] = byGuid.emplace(meta->guid, m); !fresh) {
            problem(
                m, ErrorCode::AlreadyExists,
                std::format(
                    "GUID {} is also the GUID of {} (a copied file keeps its sidecar's GUID: give the copy "
                    "a new sidecar)",
                    meta->guid, shown(it->second)));
            continue;
        }
        if (auto id = ids.insert(meta->guid); !id) {
            problem(m, id.error().code,
                    std::format("AssetId fold collision (02 §6.1): {}", id.error().message));
            continue;
        }
        scan.assets.push_back(ScannedAsset{s, std::move(*meta)});
    }
    std::stable_sort(scan.problems.begin(), scan.problems.end(),
                     [](const MetaProblem& a, const MetaProblem& b) { return a.path < b.path; });
    std::sort(scan.assets.begin(), scan.assets.end(),
              [](const ScannedAsset& a, const ScannedAsset& b) { return a.path < b.path; });
    return scan;
}

} // namespace helios::assetpipe
