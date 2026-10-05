// Importer registry (see importer.h).

#include "helios/assetpipe/importer.h"

#include <format>
#include <optional>
#include <unordered_map>

#include "helios/reflect/json.h"
#include "helios/reflect/serialize.h"
#include "typed_object.h"

namespace helios::assetpipe {

namespace {

char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool idChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool validId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() == '.' || id.front() == '_' || id.front() == '-')
        return false;
    for (const char c : id) {
        if (!idChar(c)) return false;
    }
    return true;
}

/// ".png": a dot, then 1-16 of [a-z0-9]; not ".meta", the sidecars' own extension.
bool validExtension(std::string_view ext) noexcept {
    if (ext.size() < 2 || ext.size() > 17 || ext.front() != '.' || ext == ".meta") return false;
    for (const char c : ext.substr(1)) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) return false;
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        if (lowerAscii(tail[i]) != suffix[i]) return false;
    }
    return true;
}

/// The walk behind hashSettingsType(): a tagged, length-prefixed little-endian stream, so two different
/// walks never write the same bytes.
class SettingsTypeWalk {
public:
    Result<void> type(const refl::TypeInfo& t, u32 depth) {
        if (depth > kMaxSettingsTypeDepth) {
            return makeError(ErrorCode::LimitExceeded, "settings type '{}' is nested deeper than {} levels",
                             t.qualifiedName, kMaxSettingsTypeDepth);
        }
        if (const auto it = m_seen.find(&t); it != m_seen.end()) {
            tag('R'); // seen before (shared or recursive): its position in first-visit order
            u32le(it->second);
            return {};
        }
        m_seen.emplace(&t, static_cast<u32>(m_seen.size()));
        tag('T');
        tag(static_cast<u8>(t.kind));
        text(t.qualifiedName);
        u32le(t.id);
        u32le(t.version);
        u32le(static_cast<u32>(t.flags));
        switch (t.kind) {
        case refl::Kind::Struct: return structFields(t, depth);
        case refl::Kind::Enum:
        case refl::Kind::Flags:
            u64le(t.enumValues.size());
            for (const refl::EnumValue& v : t.enumValues) {
                text(v.name);
                u64le(static_cast<u64>(v.value));
            }
            return t.elementFn ? type(t.element(), depth + 1) : Result<void>{};
        case refl::Kind::List:
        case refl::Kind::KeyedList:
        case refl::Kind::Set:
        case refl::Kind::Optional: return type(t.element(), depth + 1);
        case refl::Kind::Array:
            u32le(t.arraySize);
            return type(t.element(), depth + 1);
        case refl::Kind::Map:
            HELIOS_TRY(type(t.key(), depth + 1));
            return type(t.element(), depth + 1);
        case refl::Kind::Variant:
            u64le(t.alternatives.size());
            for (const refl::VariantAlt& a : t.alternatives) {
                text(a.name);
                u32le(a.id);
                HELIOS_TRY(type(a.type(), depth + 1));
            }
            return {};
        default:
            // Scalars, strings and names are their kind; a builtin vocabulary type (Guid, vec3f) is its name,
            // and its encoding belongs to the engine's code (kCookerVersion).
            return {};
        }
    }

    Hash128 digest() { return m_hasher.digest(); }

private:
    Result<void> structFields(const refl::TypeInfo& t, u32 depth) {
        // Defaults as a default-constructed object holds them: what a build step reading the settings into
        // its type starts from. Without lifetime ops, the fields' recorded defaults.
        std::optional<detail::TypedObject> object;
        if (t.ops && t.ops->construct && t.ops->destruct) object.emplace(t);
        u64le(t.fields.size());
        for (const refl::FieldInfo& f : t.fields) {
            text(f.name);
            u32le(f.id);
            u32le(static_cast<u32>(f.flags));
            const void* value = object ? f.ptr(static_cast<const void*>(object->get())) : f.defaultValue;
            if (value) {
                tag(1);
                refl::JsonWriter w(refl::JsonStyle::Compact);
                refl::writeJson(f.type(), value, w);
                text(w.take());
            } else {
                tag(0);
            }
            HELIOS_TRY(type(f.type(), depth + 1));
        }
        return {};
    }

    void tag(u8 v) { m_hasher.update(&v, 1); }
    void u32le(u32 v) {
        u8 b[4];
        storeLE<u32>(b, v);
        m_hasher.update(b, sizeof(b));
    }
    void u64le(u64 v) {
        u8 b[8];
        storeLE<u64>(b, v);
        m_hasher.update(b, sizeof(b));
    }
    void text(std::string_view s) {
        u64le(s.size());
        m_hasher.update(s);
    }

    Hasher128 m_hasher;
    std::unordered_map<const refl::TypeInfo*, u32> m_seen;
};

} // namespace

Result<Hash128> hashSettingsType(const refl::TypeInfo* type) {
    if (!type) return Hash128{};
    SettingsTypeWalk walk;
    HELIOS_TRY(walk.type(*type, 0));
    return walk.digest();
}

bool importsFile(const ImporterInfo& importer, std::string_view fileName) noexcept {
    // The extension is the text after the last '.', so "a.tar.png" is a ".png" and ".png" alone (a
    // dotfile with no stem) is nothing.
    const usize slash = fileName.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? fileName : fileName.substr(slash + 1);
    const usize dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string_view ext = name.substr(dot);
    for (const std::string& e : importer.extensions) {
        if (ext.size() == e.size() && endsWithIgnoringCase(ext, e)) return true;
    }
    return false;
}

Result<void> ImporterRegistry::add(ImporterInfo info) {
    if (!validId(info.id)) {
        return makeError(
            ErrorCode::InvalidArgument,
            "importer id '{}' must be 1-64 characters of [a-z0-9._-] starting with a letter or digit",
            info.id);
    }
    if (info.version == 0)
        return makeError(ErrorCode::InvalidArgument, "importer '{}': version 0 (versions start at 1)",
                         info.id);
    if (info.settings && (info.settings->kind != refl::Kind::Struct || !info.settings->ops ||
                          !info.settings->ops->construct || !info.settings->ops->destruct)) {
        return makeError(ErrorCode::InvalidArgument, "importer '{}': settings type '{}' is not a struct",
                         info.id, info.settings->qualifiedName);
    }
    auto settingsType = hashSettingsType(info.settings);
    if (!settingsType) {
        return makeError(ErrorCode::InvalidArgument, "importer '{}': {}", info.id,
                         settingsType.error().message);
    }
    if (find(info.id))
        return makeError(ErrorCode::AlreadyExists, "importer '{}' is already registered", info.id);
    for (const std::string& ext : info.extensions) {
        if (!validExtension(ext)) {
            return makeError(
                ErrorCode::InvalidArgument,
                "importer '{}': extension '{}' must be a dot and 1-16 lower-case letters or digits", info.id,
                ext);
        }
        for (const auto& other : m_importers) {
            for (const std::string& taken : other->info.extensions) {
                if (taken == ext) {
                    return makeError(ErrorCode::AlreadyExists,
                                     "importer '{}': extension '{}' is already imported by '{}'", info.id,
                                     ext, other->info.id);
                }
            }
        }
    }
    m_importers.push_back(std::make_unique<Registered>(Registered{std::move(info), *settingsType}));
    return {};
}

const ImporterInfo* ImporterRegistry::find(std::string_view id) const noexcept {
    for (const auto& r : m_importers) {
        if (r->info.id == id) return &r->info;
    }
    return nullptr;
}

const ImporterInfo* ImporterRegistry::forFile(std::string_view fileName) const noexcept {
    for (const auto& r : m_importers) {
        if (importsFile(r->info, fileName)) return &r->info;
    }
    return nullptr;
}

Hash128 ImporterRegistry::settingsTypeHash(std::string_view id) const noexcept {
    for (const auto& r : m_importers) {
        if (r->info.id == id) return r->settingsType;
    }
    return Hash128{};
}

} // namespace helios::assetpipe
