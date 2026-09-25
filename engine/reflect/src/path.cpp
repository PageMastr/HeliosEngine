// Property path parsing and resolution.

#include "helios/reflect/path.h"

#include <charconv>
#include <format>

#include "helios/reflect/serialize.h"
#include "path_internal.h"

namespace helios::refl {

namespace {
bool isDigits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

void appendEscaped(std::string& out, std::string_view text) {
    for (const char c : text) {
        if (c == ']' || c == '\\') out += '\\';
        out += c;
    }
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// Text form of a keyed-list key prefix without dashes, lowercased.
std::string normalizeHex(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '-') out += lower(c);
    }
    return out;
}

Error pathError(ErrorCode code, const PropertyPath& path, usize upTo, std::string_view message) {
    PropertyPath prefix;
    for (usize i = 0; i < upTo && i < path.size(); ++i) {
        const PathSegment& s = path.segments()[i];
        switch (s.kind) {
        case PathSegment::Kind::Field: prefix.field(s.text); break;
        case PathSegment::Kind::Index: prefix.index(s.index); break;
        case PathSegment::Kind::Key: prefix.key(s.text); break;
        case PathSegment::Kind::Keyed: prefix.keyed(s.text); break;
        }
    }
    const std::string where = prefix.empty() ? std::string("<root>") : prefix.toString();
    return Error{code, std::format("{}: {} (in '{}')", where, message, path.toString())};
}

/// Text of the key field of an element in a @keyed(field) list; empty when not keyable.
bool keyFieldText(const TypeInfo& elemType, const void* elem, std::string_view keyField, std::string& out) {
    const FieldInfo* f = elemType.field(keyField);
    if (!f || !f->type().ops || !f->type().ops->keyToText) return false;
    out = f->type().ops->keyToText(f->ptr(elem));
    return true;
}

/// Canonical text of a key written in a path for a @keyed(field) list ("01" -> "1"); the text
/// itself when it does not parse as the key type.
std::string canonicalKeyText(const TypeInfo& elemType, std::string_view keyField, std::string_view text) {
    const FieldInfo* f = elemType.field(keyField);
    if (!f || !f->type().ops || !f->type().ops->keyFromText || !f->type().ops->keyToText) return std::string(text);
    Value key(f->type());
    if (!f->type().ops->keyFromText(text, key.data())) return std::string(text);
    return f->type().ops->keyToText(key.data());
}

std::string_view keyedByField(const FieldInfo* field) noexcept {
    if (!field) return {};
    const auto* k = field->attr<attrs::Keyed>();
    return k ? k->field : std::string_view();
}

struct Cursor {
    const TypeInfo* type;
    void* ptr;
    const FieldInfo* field; // field that produced the current value (for @keyed(field) lists)
};

Result<Cursor> resolveImpl(const TypeInfo& root, void* object, const PropertyPath& path, ResolveMode mode) {
    Cursor c{&root, object, nullptr};
    const bool write = mode == ResolveMode::Write;
    const auto segs = path.segments();
    for (usize si = 0; si < segs.size(); ++si) {
        const PathSegment& seg = segs[si];
        while (c.type->kind == Kind::Optional) {
            if (!c.type->ops->has(c.ptr)) {
                if (!write) return pathError(ErrorCode::NotFound, path, si, "optional value is empty");
                c.ptr = c.type->ops->emplace(c.ptr);
            } else {
                c.ptr = c.type->ops->get(c.ptr);
            }
            c.type = &c.type->element();
        }
        const TypeInfo& t = *c.type;
        switch (seg.kind) {
        case PathSegment::Kind::Field: {
            if (t.kind == Kind::Struct || (t.kind == Kind::Builtin && !t.fields.empty())) {
                const FieldInfo* f = t.fieldOrAlias(seg.text);
                if (!f) return pathError(ErrorCode::NotFound, path, si, std::format("{} has no field '{}'", t.qualifiedName, seg.text));
                c = Cursor{&f->type(), f->ptr(c.ptr), f};
            } else if (t.kind == Kind::Variant) {
                const VariantAlt* alt = t.alternative(seg.text);
                if (!alt) return pathError(ErrorCode::NotFound, path, si, std::format("{} has no alternative '{}'", t.qualifiedName, seg.text));
                const u32 index = static_cast<u32>(alt - t.alternatives.data());
                void* p = nullptr;
                if (t.ops->index(c.ptr) == index) {
                    p = t.ops->alt(c.ptr);
                } else {
                    if (!write) return pathError(ErrorCode::NotFound, path, si, std::format("alternative '{}' is not active", seg.text));
                    p = t.ops->emplaceAlt(c.ptr, index);
                }
                c = Cursor{&alt->type(), p, nullptr};
            } else {
                return pathError(ErrorCode::InvalidArgument, path, si, std::format("cannot select field '{}' of a {}", seg.text, kindName(t.kind)));
            }
            break;
        }
        case PathSegment::Kind::Index:
            if (isSequenceKind(t.kind)) {
                const usize n = t.ops->size(c.ptr);
                if (seg.index >= n) return pathError(ErrorCode::OutOfRange, path, si, std::format("index {} out of range (size {})", seg.index, n));
                c = Cursor{&t.element(), t.ops->element(c.ptr, static_cast<usize>(seg.index)), nullptr};
                break;
            }
            if (t.kind != Kind::Map) return pathError(ErrorCode::InvalidArgument, path, si, std::format("cannot index a {}", kindName(t.kind)));
            [[fallthrough]];
        case PathSegment::Kind::Key: {
            if (t.kind != Kind::Map) return pathError(ErrorCode::InvalidArgument, path, si, std::format("[key] needs a map, not a {}", kindName(t.kind)));
            const TypeInfo& keyType = t.key();
            Value key(keyType);
            if (!keyType.ops->keyFromText(seg.text, key.data()))
                return pathError(ErrorCode::InvalidArgument, path, si, std::format("invalid {} key '{}'", keyType.qualifiedName, seg.text));
            void* v = t.ops->find(c.ptr, key.data());
            if (!v) {
                if (!write) return pathError(ErrorCode::NotFound, path, si, std::format("no key '{}'", seg.text));
                v = t.ops->findOrInsert(c.ptr, key.data());
            }
            c = Cursor{&t.element(), v, nullptr};
            break;
        }
        case PathSegment::Kind::Keyed: {
            const std::string_view byField = keyedByField(c.field);
            if (t.kind == Kind::KeyedList) {
                const std::string want = normalizeHex(seg.text);
                const usize n = t.ops->size(c.ptr);
                usize found = n;
                for (usize i = 0; i < n; ++i) {
                    if (keyedKeyText(t.ops->keyAt(c.ptr, i)).starts_with(want)) {
                        if (found != n) return pathError(ErrorCode::InvalidArgument, path, si, std::format("key prefix '{}' is ambiguous", seg.text));
                        found = i;
                    }
                }
                if (found == n) {
                    auto key = Guid::parse(seg.text);
                    if (!write || !key || key->isNil()) return pathError(ErrorCode::NotFound, path, si, std::format("no element with key '{}'", seg.text));
                    void* p = t.ops->insertAt(c.ptr, n);
                    t.ops->setKeyAt(c.ptr, n, *key);
                    c = Cursor{&t.element(), p, nullptr};
                } else {
                    c = Cursor{&t.element(), t.ops->element(c.ptr, found), nullptr};
                }
            } else if (t.kind == Kind::List && !byField.empty()) {
                const TypeInfo& e = t.element();
                const usize n = t.ops->size(c.ptr);
                usize found = n;
                // Compare canonical key text ("01" and "1" name the same integer key), so a write
                // never appends a second element with an existing key.
                const std::string want = canonicalKeyText(e, byField, seg.text);
                std::string text;
                for (usize i = 0; i < n && found == n; ++i) {
                    if (keyFieldText(e, t.ops->element(c.ptr, i), byField, text) && text == want) found = i;
                }
                if (found == n) {
                    const FieldInfo* kf = e.field(byField);
                    if (!write || !kf) return pathError(ErrorCode::NotFound, path, si, std::format("no element with {} '{}'", byField, seg.text));
                    void* p = t.ops->insertAt(c.ptr, n);
                    if (!kf->type().ops->keyFromText(seg.text, kf->ptr(p))) {
                        t.ops->eraseAt(c.ptr, n);
                        return pathError(ErrorCode::InvalidArgument, path, si, std::format("invalid key '{}'", seg.text));
                    }
                    c = Cursor{&e, p, nullptr};
                } else {
                    c = Cursor{&e, t.ops->element(c.ptr, found), nullptr};
                }
            } else {
                return pathError(ErrorCode::InvalidArgument, path, si, std::format("[#key] needs a keyed list, not a {}", kindName(t.kind)));
            }
            break;
        }
        }
    }
    return c;
}

/// Type at `path` without touching an object.
Result<const TypeInfo*> resolveType(const TypeInfo& root, const PropertyPath& path) {
    const TypeInfo* t = &root;
    const auto segs = path.segments();
    for (usize si = 0; si < segs.size(); ++si) {
        while (t->kind == Kind::Optional) t = &t->element();
        const PathSegment& seg = segs[si];
        if (seg.kind == PathSegment::Kind::Field) {
            if (t->kind == Kind::Variant) {
                const VariantAlt* alt = t->alternative(seg.text);
                if (!alt) return pathError(ErrorCode::NotFound, path, si, std::format("{} has no alternative '{}'", t->qualifiedName, seg.text));
                t = &alt->type();
            } else {
                const FieldInfo* f = t->fieldOrAlias(seg.text);
                if (!f) return pathError(ErrorCode::NotFound, path, si, std::format("{} has no field '{}'", t->qualifiedName, seg.text));
                t = &f->type();
            }
        } else {
            if (!isSequenceKind(t->kind) && t->kind != Kind::Map)
                return pathError(ErrorCode::InvalidArgument, path, si, std::format("cannot index a {}", kindName(t->kind)));
            t = &t->element();
        }
    }
    return t;
}
} // namespace

std::string keyedKeyText(const Guid& key) {
    std::string s = key.toString();
    std::erase(s, '-');
    return s;
}

Result<PropertyPath> PropertyPath::parse(std::string_view text) {
    PropertyPath p;
    usize i = 0;
    bool expectSegment = true; // after a separator (or at the start) a name or '[' must follow
    auto fail = [&](std::string_view why) {
        return Error{ErrorCode::ParseError, std::format("invalid property path '{}' at column {}: {}", text, i + 1, why)};
    };
    while (i < text.size()) {
        const char c = text[i];
        if (c == '/' || c == '.') {
            if (expectSegment) return fail("empty segment");
            expectSegment = true;
            ++i;
            continue;
        }
        if (c == '[') {
            if (expectSegment && !p.m_segments.empty()) return fail("'[' after a separator");
            std::string content;
            ++i;
            bool closed = false;
            bool firstEscaped = false; // `[\#x]` is the map key "#x", not a keyed-list key
            while (i < text.size()) {
                const char d = text[i];
                if (d == '\\' && i + 1 < text.size()) {
                    if (content.empty()) firstEscaped = true;
                    content += text[i + 1];
                    i += 2;
                    continue;
                }
                if (d == ']') {
                    closed = true;
                    ++i;
                    break;
                }
                content += d;
                ++i;
            }
            if (!closed) return fail("missing ']'");
            if (content.empty()) {
                p.key(""); // "[]" is the empty map key
            } else if (content[0] == '#' && !firstEscaped) {
                if (content.size() == 1) return fail("empty [#key]");
                p.keyed(std::string_view(content).substr(1));
            } else if (isDigits(content)) {
                u64 idx = 0;
                const auto r = std::from_chars(content.data(), content.data() + content.size(), idx);
                if (r.ec != std::errc()) return fail("index out of range");
                p.index(idx);
                p.m_segments.back().text = content; // map lookups use the text
            } else {
                p.key(content);
            }
            expectSegment = false;
            continue;
        }
        if (!expectSegment) return fail("expected '/', '.' or '['");
        const usize start = i;
        while (i < text.size() && text[i] != '/' && text[i] != '.' && text[i] != '[' && text[i] != ']') ++i;
        if (i == start) return fail("unexpected ']'");
        p.field(text.substr(start, i - start));
        expectSegment = false;
    }
    if (expectSegment && !p.m_segments.empty()) return fail("trailing separator");
    return p;
}

PropertyPath& PropertyPath::field(std::string_view name) {
    m_segments.push_back({PathSegment::Kind::Field, std::string(name), 0});
    return *this;
}
PropertyPath& PropertyPath::index(u64 i) {
    m_segments.push_back({PathSegment::Kind::Index, std::to_string(i), i});
    return *this;
}
PropertyPath& PropertyPath::key(std::string_view text) {
    m_segments.push_back({PathSegment::Kind::Key, std::string(text), 0});
    return *this;
}
PropertyPath& PropertyPath::keyed(std::string_view keyText) {
    m_segments.push_back({PathSegment::Kind::Keyed, std::string(keyText), 0});
    return *this;
}

PropertyPath PropertyPath::parent() const {
    PropertyPath p = *this;
    if (!p.m_segments.empty()) p.m_segments.pop_back();
    return p;
}

std::string PropertyPath::toString() const {
    std::string out;
    for (const PathSegment& s : m_segments) {
        switch (s.kind) {
        case PathSegment::Kind::Field:
            if (!out.empty()) out += '/';
            out += s.text;
            break;
        case PathSegment::Kind::Index:
            out += '[';
            out += std::to_string(s.index);
            out += ']';
            break;
        case PathSegment::Kind::Key:
            out += '[';
            // A leading '#' would read back as a keyed-list key and digits as an index (which map
            // lookups also accept): escape the '#'.
            if (!s.text.empty() && s.text[0] == '#') out += '\\';
            appendEscaped(out, s.text);
            out += ']';
            break;
        case PathSegment::Kind::Keyed:
            out += "[#";
            appendEscaped(out, s.text);
            out += ']';
            break;
        }
    }
    return out;
}

Result<Ref> resolve(const TypeInfo& type, void* object, const PropertyPath& path, ResolveMode mode) {
    HELIOS_TRY_ASSIGN(const Cursor c, resolveImpl(type, object, path, mode));
    return Ref{c.type, c.ptr, c.field};
}

Result<ConstRef> resolve(const TypeInfo& type, const void* object, const PropertyPath& path) {
    HELIOS_TRY_ASSIGN(const Cursor c, resolveImpl(type, const_cast<void*>(object), path, ResolveMode::Read));
    return ConstRef{c.type, c.ptr, c.field};
}

Result<std::string> getJson(const TypeInfo& type, const void* object, std::string_view path) {
    HELIOS_TRY_ASSIGN(const PropertyPath p, PropertyPath::parse(path));
    HELIOS_TRY_ASSIGN(const ConstRef r, resolve(type, object, p));
    return toJson(*r.type, r.ptr, JsonStyle::Compact);
}

namespace detail {
/// Shared by setJson and patch application: parse first (so a bad value changes nothing), then
/// resolve in Write mode and assign; keyed-by-field elements keep the key named by the path.
Result<void> setAtPath(const TypeInfo& type, void* object, const PropertyPath& path, std::string_view json) {
    HELIOS_TRY_ASSIGN(const JsonDocument doc, JsonDocument::parse(json, path.toString()));
    HELIOS_TRY_ASSIGN(const TypeInfo* target, resolveType(type, path));
    Value tmp(*target);
    ReadCtx ctx;
    HELIOS_TRY(readJson(*target, tmp.data(), doc.root(), ctx));
    HELIOS_TRY_ASSIGN(const Cursor c, resolveImpl(type, object, path, ResolveMode::Write));
    target->ops->copy(c.ptr, tmp.data());
    if (!path.empty() && path.back().kind == PathSegment::Kind::Keyed) {
        // Re-assert the key field for @keyed(field) lists (the value may omit or change it).
        HELIOS_TRY_ASSIGN(const Cursor listCursor, resolveImpl(type, object, path.parent(), ResolveMode::Read));
        const std::string_view byField = keyedByField(listCursor.field);
        if (listCursor.type->kind == Kind::List && !byField.empty()) {
            if (const FieldInfo* kf = target->field(byField)) kf->type().ops->keyFromText(path.back().text, kf->ptr(c.ptr));
        }
    }
    return {};
}

Result<void> removeAtPath(const TypeInfo& type, void* object, const PropertyPath& path) {
    if (path.empty()) return Error{ErrorCode::InvalidArgument, "cannot remove the root"};
    const PathSegment& last = path.back();
    if (last.kind == PathSegment::Kind::Field) {
        HELIOS_TRY_ASSIGN(const Cursor c, resolveImpl(type, object, path, ResolveMode::Read));
        if (c.type->kind != Kind::Optional)
            return pathError(ErrorCode::InvalidArgument, path, path.size(), "only optionals, map keys and keyed-list elements can be removed");
        c.type->ops->reset(c.ptr);
        return {};
    }
    HELIOS_TRY_ASSIGN(Cursor parent, resolveImpl(type, object, path.parent(), ResolveMode::Read));
    while (parent.type->kind == Kind::Optional) {
        if (!parent.type->ops->has(parent.ptr)) return pathError(ErrorCode::NotFound, path, path.size() - 1, "optional value is empty");
        parent.ptr = parent.type->ops->get(parent.ptr);
        parent.type = &parent.type->element();
    }
    const TypeInfo& t = *parent.type;
    if (t.kind == Kind::Map && last.kind != PathSegment::Kind::Keyed) {
        Value key(t.key());
        if (!t.key().ops->keyFromText(last.text, key.data()))
            return pathError(ErrorCode::InvalidArgument, path, path.size(), "invalid map key");
        if (!t.ops->eraseKey(parent.ptr, key.data())) return pathError(ErrorCode::NotFound, path, path.size(), "no such key");
        return {};
    }
    if (last.kind == PathSegment::Kind::Keyed && (t.kind == Kind::KeyedList || t.kind == Kind::List)) {
        HELIOS_TRY_ASSIGN(const Cursor elem, resolveImpl(type, object, path, ResolveMode::Read));
        const usize n = t.ops->size(parent.ptr);
        for (usize i = 0; i < n; ++i) {
            if (t.ops->element(parent.ptr, i) == elem.ptr) {
                t.ops->eraseAt(parent.ptr, i);
                return {};
            }
        }
        return pathError(ErrorCode::NotFound, path, path.size(), "element not found");
    }
    if (last.kind == PathSegment::Kind::Index && t.kind == Kind::List) {
        if (last.index >= t.ops->size(parent.ptr)) return pathError(ErrorCode::OutOfRange, path, path.size(), "index out of range");
        t.ops->eraseAt(parent.ptr, static_cast<usize>(last.index));
        return {};
    }
    return pathError(ErrorCode::InvalidArgument, path, path.size(), std::format("cannot remove an element of a {}", kindName(t.kind)));
}
} // namespace detail

Result<void> setJson(const TypeInfo& type, void* object, std::string_view path, std::string_view json) {
    HELIOS_TRY_ASSIGN(const PropertyPath p, PropertyPath::parse(path));
    return detail::setAtPath(type, object, p, json);
}

} // namespace helios::refl
