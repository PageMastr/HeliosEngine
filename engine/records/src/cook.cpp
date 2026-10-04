// The records cook: load, inherit, check, build the tag table, encode both cooks.
#include "helios/records/cook.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "helios/core/assert.h"
#include "helios/hxl/compiler.h"
#include "helios/records/layout.h"
#include "helios/reflect/codec.h"
#include "helios/reflect/json.h"
#include "helios/reflect/record.h"
#include "helios/reflect/serialize.h"

namespace helios::records {

using hrdb::store;
using refl::FieldInfo;
using refl::JsonValue;
using refl::Kind;
using refl::ReadCtx;
using refl::TypeInfo;

namespace {

bool isSegStart(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool isSegChar(char c) noexcept { return isSegStart(c) || (c >= '0' && c <= '9'); }

/// gameplay's tag-query grammar (engine/gameplay/src/tags.cpp parseQuery; records is L2 and cannot link
/// L4 gameplay, and the tag-numbering test cross-checks the two): `all(A, B) any(C) none(D)`, each
/// clause at most once, in any order, with one or more comma-separated tag names; whitespace between
/// tokens; nothing else. Appends the tag names in order of appearance.
Result<void> parseTagQuery(std::string_view text, std::vector<std::string_view>& names) {
    usize i = 0;
    bool seen[3] = {false, false, false};
    auto skipWs = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
    };
    auto readName = [&](usize& start) -> std::string_view {
        start = i;
        while (i < text.size() && (isSegChar(text[i]) || text[i] == '.')) ++i;
        return text.substr(start, i - start);
    };
    while (true) {
        skipWs();
        if (i >= text.size()) return {};
        usize kwStart = 0;
        const std::string_view kw = readName(kwStart);
        const int clause = kw == "all" ? 0 : kw == "any" ? 1 : kw == "none" ? 2 : -1;
        if (clause < 0) return makeError(ErrorCode::ParseError, "tag query: expected all(, any( or none( at offset {}", kwStart);
        if (seen[clause]) return makeError(ErrorCode::ParseError, "tag query: duplicate {}() clause at offset {}", kw, kwStart);
        seen[clause] = true;
        skipWs();
        if (i >= text.size() || text[i] != '(') return makeError(ErrorCode::ParseError, "tag query: expected '(' at offset {}", i);
        ++i;
        while (true) {
            skipWs();
            usize nameStart = 0;
            const std::string_view name = readName(nameStart);
            if (!isValidTagName(name)) return makeError(ErrorCode::ParseError, "tag query: expected a tag name at offset {}", nameStart);
            names.push_back(name);
            skipWs();
            if (i < text.size() && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == ')') {
                ++i;
                break;
            }
            return makeError(ErrorCode::ParseError, "tag query: expected ',' or ')' at offset {}", i);
        }
    }
}

std::string_view parentTag(std::string_view name) noexcept {
    const usize dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
}

bool isRecordRef(const TypeInfo& t) noexcept {
    return t.kind == Kind::Builtin && t.elementFn && t.qualifiedName.starts_with("Ref<");
}

u64 refValue(const void* obj) noexcept {
    u64 id = 0;
    std::memcpy(&id, obj, sizeof(id)); // every RecordRef<T> is one RecordId
    return id;
}

/// Pointers to a set's elements or a map's entries in canonical key order (02 §3.7: never the
/// container's own order, which for Name keys depends on interning).
struct Entry {
    const void* key;
    const void* value;
};
std::vector<Entry> sortedEntries(const TypeInfo& t, const void* container) {
    std::vector<Entry> out;
    t.ops->forEach(container, &out, [](void* user, const void* k, const void* v) { static_cast<std::vector<Entry>*>(user)->push_back({k, v}); });
    const TypeInfo& keyType = t.kind == Kind::Map ? t.key() : t.element();
    if (!keyType.ops->keyLess) return out; // not cookable; the layout reports it
    std::sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) { return keyType.ops->keyLess(a.key, b.key); });
    return out;
}

// ---------------------------------------------------------------------------------------------
// Inheritance: overlaying a record's JSON onto its parent's resolved value
// ---------------------------------------------------------------------------------------------

class Overlay {
public:
    explicit Overlay(ReadCtx& ctx) : m_ctx(ctx) {}

    /// Overlays the members of `in` onto the struct `obj`. `inherited`: `obj` holds a parent's value, so
    /// keyed lists and @merge(append) lists merge into it; otherwise it holds defaults, and they replace
    /// them as refl::readRecord does (a record without $parent, or an element the child adds).
    Result<void> fields(const TypeInfo& t, void* obj, JsonValue in, bool root, bool inherited) {
        if (!in.isObject()) return m_ctx.typeError("object", in);
        // `@was` aliases first, then current names, so the current name wins (as the readers do).
        for (int pass = 0; pass < 2; ++pass) {
            for (const JsonValue::Member m : in.members()) {
                if (!m.key.empty() && m.key[0] == '$') {
                    if (pass == 1 && m.key != "$comment" && m.key != (root ? "$rid" : "$key") &&
                        !(root && (m.key == "$name" || m.key == "$parent"))) {
                        ReadCtx::Scope s(m_ctx, m.key);
                        return m_ctx.error("unknown metadata key");
                    }
                    continue;
                }
                const FieldInfo* f = t.field(m.key);
                const bool alias = f == nullptr && (f = t.fieldOrAlias(m.key)) != nullptr;
                if (!f) {
                    if (pass == 1) {
                        ReadCtx::Scope s(m_ctx, m.key);
                        HELIOS_TRY(m_ctx.unknownField(m.key));
                    }
                    continue;
                }
                if (alias != (pass == 0)) continue;
                ReadCtx::Scope s(m_ctx, m.key);
                HELIOS_TRY(value(f, f->type(), f->ptr(obj), m.value, inherited));
            }
        }
        return {};
    }

private:
    Result<void> value(const FieldInfo* f, const TypeInfo& t, void* obj, JsonValue in, bool inherited) {
        if (t.kind == Kind::Struct && in.isObject()) return fields(t, obj, in, false, inherited);
        if (t.kind == Kind::KeyedList) return keyedByGuid(t, obj, in, inherited);
        if (t.kind == Kind::List && f) {
            if (const auto* k = f->attr<refl::attrs::Keyed>(); k && !k->field.empty()) return keyedByField(t, k->field, obj, in, inherited);
            if (const refl::Attr* mg = f->attr("merge"); mg && mg->arg(0) == "append") return append(t, obj, in, inherited);
        }
        return replace(f, t, obj, in);
    }

    /// Replaces the value, reading it as a record without a parent would (from the field's default).
    Result<void> replace(const FieldInfo* f, const TypeInfo& t, void* obj, JsonValue in) {
        refl::Value fresh(t);
        if (f && f->defaultValue) t.ops->copy(fresh.data(), f->defaultValue);
        HELIOS_TRY(refl::readJson(t, fresh.data(), in, m_ctx));
        t.ops->copy(obj, fresh.data());
        return {};
    }

    Result<void> keyedByGuid(const TypeInfo& t, void* obj, JsonValue in, bool inherited) {
        const refl::TypeOps& ops = *t.ops;
        if (in.isNull()) {
            ops.resize(obj, 0);
            return {};
        }
        if (!in.isArray()) return m_ctx.typeError("array", in);
        if (!inherited) ops.resize(obj, 0); // a default is replaced, not merged into
        std::unordered_map<Guid, usize> index; // inherited and new elements by key (linear, whatever the list size)
        const usize inheritedCount = ops.size(obj);
        for (usize j = 0; j < inheritedCount; ++j) index.emplace(ops.keyAt(obj, j), j);
        std::unordered_set<Guid> seen;
        usize i = 0;
        for (const JsonValue e : in.elements()) {
            ReadCtx::Scope s(m_ctx, i++);
            // The readers mint a random key for an element without one; a cook must be deterministic.
            if (e.isObject() && !e.get("$key").isValid()) return m_ctx.error("element has no $key (keyed lists need stable keys)");
            HELIOS_TRY_ASSIGN(const Guid key, refl::detail::readKeyedListKey(e, m_ctx));
            if (!seen.insert(key).second) return m_ctx.error("duplicate $key " + key.toString());
            void* elem = nullptr;
            bool merged = false;
            if (const auto it = index.find(key); it != index.end()) {
                elem = ops.element(obj, it->second);
                merged = it->second < inheritedCount;
            } else {
                const usize n = ops.size(obj);
                elem = ops.insertAt(obj, n);
                ops.setKeyAt(obj, n, key);
                index.emplace(key, n);
            }
            HELIOS_TRY(value(nullptr, t.element(), elem, e, merged));
        }
        return {};
    }

    Result<void> keyedByField(const TypeInfo& t, std::string_view keyName, void* obj, JsonValue in, bool inherited) {
        const refl::TypeOps& ops = *t.ops;
        const TypeInfo& et = t.element();
        const FieldInfo* kf = et.field(keyName);
        if (!kf) return m_ctx.error(std::format("@keyed({}) names no field of {}", keyName, et.qualifiedName));
        if (in.isNull()) {
            ops.resize(obj, 0);
            return {};
        }
        if (!in.isArray()) return m_ctx.typeError("array", in);
        if (!inherited) ops.resize(obj, 0); // a default is replaced, not merged into
        // Keys by their canonical JSON text, so any key type indexes in one map.
        auto keyText = [&](const void* key) { return refl::toJson(kf->type(), key, refl::JsonStyle::Compact); };
        std::unordered_map<std::string, usize> index;
        const usize inheritedCount = ops.size(obj);
        for (usize j = 0; j < inheritedCount; ++j) index.emplace(keyText(kf->ptr(ops.element(obj, j))), j);
        std::unordered_set<std::string> seen;
        usize i = 0;
        for (const JsonValue e : in.elements()) {
            ReadCtx::Scope s(m_ctx, i++);
            if (!e.isObject()) return m_ctx.typeError("object", e);
            const JsonValue kj = e.get(keyName);
            if (!kj.isValid()) return m_ctx.error(std::format("element has no '{}' (the list is @keyed({}))", keyName, keyName));
            refl::Value key(kf->type());
            {
                ReadCtx::Scope ks(m_ctx, keyName);
                HELIOS_TRY(refl::readJson(kf->type(), key.data(), kj, m_ctx));
            }
            std::string text = keyText(key.data());
            if (!seen.insert(text).second) return m_ctx.error(std::format("duplicate {} in a @keyed({}) list", keyName, keyName));
            void* elem = nullptr;
            bool merged = false;
            if (const auto it = index.find(text); it != index.end()) {
                elem = ops.element(obj, it->second);
                merged = it->second < inheritedCount;
            } else {
                const usize n = ops.size(obj);
                elem = ops.insertAt(obj, n);
                index.emplace(std::move(text), n);
            }
            HELIOS_TRY(value(nullptr, et, elem, e, merged));
        }
        return {};
    }

    Result<void> append(const TypeInfo& t, void* obj, JsonValue in, bool inherited) {
        const refl::TypeOps& ops = *t.ops;
        if (in.isNull()) {
            ops.resize(obj, 0);
            return {};
        }
        if (!in.isArray()) return m_ctx.typeError("array", in);
        if (!inherited) ops.resize(obj, 0); // a default is replaced, not appended to
        usize i = 0;
        for (const JsonValue e : in.elements()) {
            ReadCtx::Scope s(m_ctx, i++);
            HELIOS_TRY(value(nullptr, t.element(), ops.insertAt(obj, ops.size(obj)), e, false));
        }
        return {};
    }

    ReadCtx& m_ctx;
};

// ---------------------------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------------------------

enum class State : u8 { Unvisited, Visiting, Done, Failed };

struct Rec {
    const SourceRecord* src = nullptr;
    refl::JsonDocument doc;
    refl::RecordId rid = 0;
    std::string name;
    std::string parent;
    State state = State::Unvisited;
    refl::Value value;
};

class Diagnostics {
public:
    void add(std::string path, std::string message) { m_list.push_back({std::move(path), std::move(message)}); }
    bool empty() const noexcept { return m_list.empty(); }
    std::vector<CookDiagnostic> take() {
        std::stable_sort(m_list.begin(), m_list.end(), [](const CookDiagnostic& a, const CookDiagnostic& b) {
            return a.path != b.path ? a.path < b.path : a.message < b.message;
        });
        m_list.erase(std::unique(m_list.begin(), m_list.end()), m_list.end());
        return std::move(m_list);
    }

private:
    std::vector<CookDiagnostic> m_list;
};

// ---------------------------------------------------------------------------------------------
// Checks: references, tags, formulas
// ---------------------------------------------------------------------------------------------

struct TagUse {
    bool client = false; ///< Used by data the client cook keeps.
};

class Scanner {
public:
    Scanner(const std::unordered_map<refl::RecordId, const Rec*>& byId, const CookOptions& options, Diagnostics& diags)
        : m_byId(byId), m_options(options), m_diags(diags) {}

    std::map<std::string, TagUse> tags;
    std::unordered_map<std::string, std::vector<u8>> bytecode;

    void record(const Rec& r) {
        m_rec = &r;
        m_path.clear();
        walk(*r.src->type, r.value.data(), !excludesType(*r.src->type, CookAudience::Client));
    }

private:
    struct Seg {
        std::string_view name;
        std::string owned;
        usize index = 0;
        int kind = 0; // 0 name, 1 index, 2 owned text
    };

    std::string where() const {
        std::string s;
        for (const Seg& g : m_path) {
            if (g.kind == 1) {
                s += std::format("[{}]", g.index);
            } else if (g.kind == 2) {
                s += std::format("[{}]", g.owned);
            } else {
                if (!s.empty()) s += '.';
                s += g.name;
            }
        }
        return s;
    }
    void error(std::string_view message) {
        const std::string at = where();
        m_diags.add(m_rec->src->path, at.empty() ? std::string(message) : std::format("{}: {}", at, message));
    }

    void walk(const TypeInfo& t, const void* obj, bool client) {
        const refl::TypeOps& ops = *t.ops;
        void* mut = const_cast<void*>(obj); // container ops take non-const pointers; nothing is modified
        switch (t.kind) {
        case Kind::Struct:
            for (const FieldInfo& f : t.fields) {
                m_path.push_back(Seg{f.name});
                walk(f.type(), f.ptr(obj), client && keepsField(f, CookAudience::Client));
                m_path.pop_back();
            }
            return;
        case Kind::Builtin:
            if (isRecordRef(t)) {
                checkRef(t, refValue(obj));
            } else if (t.qualifiedName == "TagSet") {
                for (const Name tag : static_cast<const refl::TagSet*>(obj)->tags()) useTag(tag.view(), client, "");
            } else if (t.qualifiedName == "HxlExpr") {
                compile(static_cast<const refl::HxlExpr*>(obj)->text, client);
            } else if (t.qualifiedName == "TagQuery") {
                queryTags(static_cast<const refl::TagQuery*>(obj)->text, client);
            }
            return;
        case Kind::List:
        case Kind::KeyedList:
        case Kind::Array:
            for (usize i = 0, n = ops.size(obj); i < n; ++i) {
                m_path.push_back(Seg{{}, {}, i, 1});
                walk(t.element(), ops.element(mut, i), client);
                m_path.pop_back();
            }
            return;
        case Kind::Set:
        case Kind::Map: {
            const TypeInfo& kt = t.kind == Kind::Map ? t.key() : t.element();
            for (const Entry& e : sortedEntries(t, obj)) {
                m_path.push_back(Seg{{}, kt.ops->keyToText ? kt.ops->keyToText(e.key) : std::string("?"), 0, 2});
                walk(kt, e.key, client);
                if (t.kind == Kind::Map) walk(t.element(), e.value, client);
                m_path.pop_back();
            }
            return;
        }
        case Kind::Enum:
        case Kind::Flags: {
            // JSON readers accept bare integers; a cook only holds declared values (the loader checks).
            const i64 v = refl::readIntegerBits(t, obj);
            i64 all = 0;
            for (const refl::EnumValue& e : t.enumValues) all |= e.value;
            if (t.kind == Kind::Enum ? t.enumByValue(v) == nullptr : (v & ~all) != 0) {
                error(std::format("{} is not a declared value of {}", v, t.qualifiedName));
            }
            return;
        }
        case Kind::Optional:
            if (ops.has(obj)) walk(t.element(), ops.get(mut), client);
            return;
        case Kind::Variant: {
            const refl::VariantAlt& alt = t.alternatives[ops.index(obj)];
            m_path.push_back(Seg{alt.name});
            walk(alt.type(), ops.alt(mut), client);
            m_path.pop_back();
            return;
        }
        default: return;
        }
    }

    void checkRef(const TypeInfo& t, refl::RecordId id) {
        if (id == 0) return;
        const auto it = m_byId.find(id);
        if (it == m_byId.end()) {
            error(std::format("references record {}, which does not exist", id));
        } else if (it->second->src->type != &t.element()) {
            error(std::format("references record {} ('{}'), a {}, where a {} is expected", id, it->second->name,
                              it->second->src->type->qualifiedName, t.element().qualifiedName));
        }
    }

    void useTag(std::string_view tag, bool client, std::string_view where) {
        if (!isValidTagName(tag)) {
            error(std::format("invalid tag name '{}'{}", tag, where));
            return;
        }
        tags[std::string(tag)].client |= client;
    }

    /// Checks a TagQuery and gives each tag it names a TagIndex. The query itself stays text in v0:
    /// gameplay compiles it against the hot set at run time (06 §1.1), so a query it would refuse must
    /// fail here, and only real tag names may take a slot in the table (one more slot renumbers every
    /// tag after it).
    void queryTags(std::string_view text, bool client) {
        std::vector<std::string_view> names;
        if (auto r = parseTagQuery(text, names); !r) {
            error(r.error().message);
            return;
        }
        for (const std::string_view n : names) useTag(n, client, " in a tag query");
    }

    void compile(const std::string& source, bool client) {
        if (source.empty()) return;
        if (auto it = m_failed.find(source); it != m_failed.end()) {
            error(it->second);
            return;
        }
        auto it = m_tagSymbols.find(source);
        if (it == m_tagSymbols.end()) {
            hxl::CompileOptions opts;
            opts.params = m_options.hxlParams;
            hxl::Diagnostic diag;
            auto program = hxl::compile(source, opts, &diag);
            if (!program) {
                std::string msg =
                    std::format("formula does not compile: {}", diag.status != hxl::Status::Ok ? diag.toString() : program.error().message);
                error(msg);
                m_failed.emplace(source, std::move(msg));
                return;
            }
            bytecode.emplace(source, program->encode());
            it = m_tagSymbols.emplace(source, program->tagSymbols()).first;
        }
        // Tags a formula tests (tag(p, A.B)) get a TagIndex like the tags records hold.
        for (const std::string& tag : it->second) useTag(tag, client, " in a formula");
    }

    const std::unordered_map<refl::RecordId, const Rec*>& m_byId;
    const CookOptions& m_options;
    Diagnostics& m_diags;
    const Rec* m_rec = nullptr;
    std::vector<Seg> m_path;
    std::unordered_map<std::string, std::string> m_failed;
    std::unordered_map<std::string, std::vector<std::string>> m_tagSymbols;
};

// ---------------------------------------------------------------------------------------------
// Tag table
// ---------------------------------------------------------------------------------------------

struct TagRow {
    std::string name;
    u8 audience = 0;
    bool declared = false;
    bool declaredByRecord = false;
    bool client = false;
    u16 parent = kNoTag;
    u16 subtreeEnd = 0;
    u8 depth = 0;
};

// ---------------------------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------------------------

class Encoder {
public:
    Encoder(const std::unordered_map<std::string_view, u16>& tagIndex,
            const std::unordered_map<std::string, std::vector<u8>>& bytecode)
        : m_tagIndex(tagIndex), m_bytecode(bytecode) {}

    std::vector<u8> buf;
    std::string error; ///< First error ("" when none).

    usize alloc(usize size) {
        const usize at = (buf.size() + hrdb::kAlignment - 1) / hrdb::kAlignment * hrdb::kAlignment;
        buf.resize(at + size);
        return at;
    }
    void span(usize field, usize target, usize count) {
        if (count == 0) return; // an empty span is {0, 0}
        store<i32>(buf.data() + field, static_cast<i32>(target - field));
        store<u32>(buf.data() + field + 4, static_cast<u32>(count));
    }
    /// Copies `count` elements of `stride` bytes into a new block and points the span at `field` to it.
    void block(usize field, const void* data, usize count, usize stride) {
        if (count == 0) return;
        const usize at = alloc(count * stride);
        std::memcpy(buf.data() + at, data, count * stride);
        span(field, at, count);
    }
    void text(usize field, std::string_view s) { block(field, s.data(), s.size(), 1); }

    void value(const CookedLayout& l, const void* obj, usize at, u32 depth) {
        const TypeInfo& t = *l.type;
        const refl::TypeOps& ops = *t.ops;
        void* mut = const_cast<void*>(obj);
        u8* p = buf.data() + at;
        switch (l.enc) {
        case Enc::Bool: store<u8>(p, *static_cast<const bool*>(obj) ? u8{1} : u8{0}); return;
        case Enc::Int: {
            const i64 v = refl::readIntegerBits(t, obj);
            std::memcpy(p, &v, l.size); // little-endian: the low bytes
            return;
        }
        case Enc::F32: std::memcpy(p, obj, 4); return;
        case Enc::F64: std::memcpy(p, obj, 8); return;
        case Enc::Text: {
            if (t.kind == Kind::String) {
                text(at, *static_cast<const std::string*>(obj));
            } else if (t.kind == Kind::Name) {
                text(at, static_cast<const Name*>(obj)->view());
            } else if (t.qualifiedName == "LocString") {
                text(at, static_cast<const refl::LocString*>(obj)->key);
            } else {
                text(at, static_cast<const refl::TagQuery*>(obj)->text);
            }
            return;
        }
        case Enc::TagSet: {
            std::vector<u16> idx;
            for (const Name tag : static_cast<const refl::TagSet*>(obj)->tags()) {
                const auto it = m_tagIndex.find(tag.view());
                HELIOS_ASSERT(it != m_tagIndex.end(), "the scan put every used tag in the table");
                idx.push_back(it->second);
            }
            std::sort(idx.begin(), idx.end());
            block(at, idx.data(), idx.size(), sizeof(u16));
            return;
        }
        case Enc::Hxl: {
            const std::string& src = static_cast<const refl::HxlExpr*>(obj)->text;
            text(at, src);
            if (!src.empty()) {
                const auto it = m_bytecode.find(src);
                HELIOS_ASSERT(it != m_bytecode.end(), "the scan compiled every formula");
                block(at + 8, it->second.data(), it->second.size(), 1);
            }
            return;
        }
        case Enc::Guid: {
            const Guid& g = t.qualifiedName == "AssetRef" ? static_cast<const refl::AssetRef*>(obj)->guid : *static_cast<const Guid*>(obj);
            store<u64>(p, g.high);
            store<u64>(p + 8, g.low);
            return;
        }
        case Enc::EntityId: store<u64>(p, static_cast<const refl::EntityId*>(obj)->value); return;
        case Enc::NetHandle: store<u32>(p, static_cast<const refl::NetHandle*>(obj)->value); return;
        case Enc::Duration: store<i64>(p, static_cast<const refl::Duration*>(obj)->nanos); return;
        case Enc::RecordRef: store<u64>(p, refValue(obj)); return;
        case Enc::Struct:
            for (const CookedLayout::Field& f : l.fields) value(*f.layout, f.info->ptr(obj), at + f.offset, depth);
            return;
        case Enc::List:
        case Enc::KeyedList:
        case Enc::Set:
        case Enc::Map: {
            const usize count = l.enc == Enc::List || l.enc == Enc::KeyedList ? ops.size(obj) : sortedEntries(t, obj).size();
            if (count != 0 && depth >= hrdb::kMaxNesting) {
                if (error.empty()) error = std::format("non-empty lists, sets and maps nest deeper than {}", hrdb::kMaxNesting);
                return;
            }
            const u32 stride = l.stride();
            if (l.enc == Enc::List || l.enc == Enc::KeyedList) {
                const usize n = ops.size(obj);
                if (n == 0) return;
                const usize block = alloc(n * stride);
                span(at, block, n);
                for (usize i = 0; i < n; ++i) value(*l.element, ops.element(mut, i), block + i * stride, depth + 1);
                if (l.enc == Enc::KeyedList) {
                    const usize keys = alloc(n * 16);
                    span(at + 8, keys, n);
                    for (usize i = 0; i < n; ++i) {
                        const Guid k = ops.keyAt(obj, i);
                        store<u64>(buf.data() + keys + i * 16, k.high);
                        store<u64>(buf.data() + keys + i * 16 + 8, k.low);
                    }
                }
                return;
            }
            const std::vector<Entry> entries = sortedEntries(t, obj);
            if (entries.empty()) return;
            const usize block = alloc(entries.size() * stride);
            span(at, block, entries.size());
            for (usize i = 0; i < entries.size(); ++i) {
                if (l.enc == Enc::Map) {
                    value(*l.key, entries[i].key, block + i * stride, depth + 1);
                    value(*l.element, entries[i].value, block + i * stride + l.payloadOffset, depth + 1);
                } else {
                    value(*l.element, entries[i].key, block + i * stride, depth + 1);
                }
            }
            return;
        }
        case Enc::Optional:
            if (ops.has(obj)) {
                store<u8>(p, 1);
                value(*l.element, ops.get(mut), at + l.payloadOffset, depth);
            }
            return;
        case Enc::Array:
            for (usize i = 0; i < l.count; ++i) value(*l.element, ops.element(mut, i), at + i * l.element->size, depth);
            return;
        case Enc::Variant: {
            const u32 index = ops.index(obj);
            store<u32>(p, index);
            value(*l.alternatives[index], ops.alt(mut), at + l.payloadOffset, depth);
            return;
        }
        }
    }

private:
    const std::unordered_map<std::string_view, u16>& m_tagIndex;
    const std::unordered_map<std::string, std::vector<u8>>& m_bytecode;
};

/// One cook's file. Records must be sorted by RecordId; their types are not excluded by `audience`.
Result<std::vector<u8>> encodeDb(CookAudience audience, std::span<const Rec* const> records, const std::vector<TagRow>& tags,
                                 u64 tagTableHash, const std::unordered_map<std::string_view, u16>& tagIndex,
                                 const std::unordered_map<std::string, std::vector<u8>>& bytecode, Diagnostics& diags) {
    LayoutCache layouts(audience);
    std::map<refl::TypeId, const TypeInfo*> typeMap;
    for (const Rec* r : records) typeMap.emplace(r->src->type->id, r->src->type);
    std::vector<const TypeInfo*> types;
    std::unordered_map<const TypeInfo*, u32> typeIndex;
    std::vector<const CookedLayout*> typeLayouts;
    bool ok = true;
    for (const auto& [id, t] : typeMap) {
        (void)id;
        auto l = layouts.get(*t);
        if (!l) {
            diags.add("", std::format("{} cook: record type '{}': {}", cookAudienceName(audience), t->qualifiedName, l.error().message));
            ok = false;
            continue;
        }
        typeIndex.emplace(t, static_cast<u32>(types.size()));
        types.push_back(t);
        typeLayouts.push_back(*l);
    }
    if (!ok) return Error{ErrorCode::InvalidArgument, "layout errors"};

    Encoder enc(tagIndex, bytecode);
    enc.alloc(hrdb::kTablesOffset); // header + root
    auto rootSpan = [&](usize field, usize target, usize count) { enc.span(hrdb::kRootOffset + field, target, count); };
    const usize typesAt = enc.alloc(types.size() * hrdb::kTypeEntryBytes);
    const usize recordsAt = enc.alloc(records.size() * hrdb::kRecordEntryBytes);
    const usize byNameAt = enc.alloc(records.size() * 4);
    const usize tagsAt = enc.alloc(tags.size() * hrdb::kTagEntryBytes);
    std::vector<u16> visible;
    for (usize i = 0; i < tags.size(); ++i) {
        if (audience == CookAudience::Server || tags[i].client) visible.push_back(static_cast<u16>(i));
    }
    const usize visibleAt = enc.alloc(visible.size() * 2);
    rootSpan(hrdb::kRootTypes, typesAt, types.size());
    rootSpan(hrdb::kRootRecords, recordsAt, records.size());
    rootSpan(hrdb::kRootByName, byNameAt, records.size());
    rootSpan(hrdb::kRootTags, tagsAt, tags.size());
    rootSpan(hrdb::kRootVisibleTags, visibleAt, visible.size());

    Hasher64 dbHash(hrdb::kRootTypeId);
    dbHash.updateValue(hrdb::kFormatVersion);
    dbHash.updateValue(static_cast<u8>(audience));
    for (usize i = 0; i < types.size(); ++i) {
        const usize e = typesAt + i * hrdb::kTypeEntryBytes;
        store<u32>(enc.buf.data() + e, types[i]->id);
        store<u32>(enc.buf.data() + e + 4, typeLayouts[i]->size);
        store<u64>(enc.buf.data() + e + 8, typeLayouts[i]->hash);
        enc.text(e + 16, types[i]->qualifiedName);
        dbHash.updateValue(types[i]->id);
        dbHash.updateValue(typeLayouts[i]->hash);
    }
    for (usize i = 0; i < records.size(); ++i) {
        const Rec& r = *records[i];
        const auto tit = typeIndex.find(r.src->type);
        HELIOS_ASSERT(tit != typeIndex.end(), "cook() refuses record types that share a TypeId");
        const u32 ti = tit->second;
        const CookedLayout& layout = *typeLayouts[ti];
        const usize e = recordsAt + i * hrdb::kRecordEntryBytes;
        store<u64>(enc.buf.data() + e, r.rid);
        store<u32>(enc.buf.data() + e + 8, ti);
        enc.text(e + 16, r.name);
        const usize data = enc.alloc(layout.size);
        store<i32>(enc.buf.data() + e + 12, static_cast<i32>(data - (e + 12)));
        enc.value(layout, r.value.data(), data, 0);
        if (!enc.error.empty()) {
            diags.add(r.src->path, std::format("{} cook: {}", cookAudienceName(audience), enc.error));
            enc.error.clear();
            ok = false;
        }
    }
    std::vector<u32> byName(records.size());
    for (usize i = 0; i < byName.size(); ++i) byName[i] = static_cast<u32>(i);
    std::sort(byName.begin(), byName.end(), [&](u32 a, u32 b) { return records[a]->name < records[b]->name; });
    for (usize i = 0; i < byName.size(); ++i) store<u32>(enc.buf.data() + byNameAt + i * 4, byName[i]);
    for (usize i = 0; i < tags.size(); ++i) {
        const TagRow& t = tags[i];
        const usize e = tagsAt + i * hrdb::kTagEntryBytes;
        const bool withheld = audience == CookAudience::Client && !t.client;
        if (!withheld) enc.text(e, t.name);
        store<u16>(enc.buf.data() + e + 8, t.parent);
        store<u16>(enc.buf.data() + e + 10, t.subtreeEnd);
        store<u8>(enc.buf.data() + e + 12, t.depth);
        store<u8>(enc.buf.data() + e + 13, t.audience);
        const bool clientWithheld = audience == CookAudience::Server && !t.client;
        store<u8>(enc.buf.data() + e + 14, static_cast<u8>((t.declared ? hrdb::kTagDeclared : 0) | (withheld ? hrdb::kTagWithheld : 0) |
                                                           (clientWithheld ? hrdb::kTagClientWithheld : 0)));
    }
    for (usize i = 0; i < visible.size(); ++i) store<u16>(enc.buf.data() + visibleAt + i * 2, visible[i]);
    if (!ok) return Error{ErrorCode::InvalidArgument, "encoding errors"};
    enc.alloc(0); // the file ends 16-byte aligned too
    if (enc.buf.size() > hrdb::kMaxFileSize) {
        diags.add("", std::format("{} cook: {} bytes exceeds the 2 GiB format limit", cookAudienceName(audience), enc.buf.size()));
        return Error{ErrorCode::LimitExceeded, "too large"};
    }
    hrdb::Header h;
    h.audience = static_cast<u8>(audience);
    h.layoutHash = dbHash.digest();
    h.tagTableHash = tagTableHash;
    h.size = enc.buf.size();
    hrdb::encodeHeader(h, std::span<u8, hrdb::kHeaderBytes>(enc.buf.data(), hrdb::kHeaderBytes));
    hrdb::seal(enc.buf);
    return std::move(enc.buf);
}

/// Reads the `$` header of a record file.
bool readHeader(Rec& r, Diagnostics& diags) {
    const std::string& path = r.src->path;
    const JsonValue root = r.doc.root();
    if (!root.isObject()) {
        diags.add(path, "a record file holds one JSON object");
        return false;
    }
    bool ok = true;
    const JsonValue rid = root.get("$rid");
    if (!rid.isValid()) {
        diags.add(path, "no $rid (mint one once with refl::mintRecordId() and keep it)");
        ok = false;
    } else if (!rid.getU64(r.rid) || !refl::isValidRecordId(r.rid)) {
        diags.add(path, "$rid must be a non-zero 63-bit integer");
        ok = false;
    }
    auto text = [&](std::string_view key, std::string& out, bool required) {
        const JsonValue v = root.get(key);
        if (!v.isValid()) {
            if (required) {
                diags.add(path, std::format("no {}", key));
                ok = false;
            }
            return;
        }
        if (!v.isString() || v.asString().empty()) {
            diags.add(path, std::format("{} must be a non-empty string", key));
            ok = false;
            return;
        }
        out.assign(v.asString());
    };
    text("$name", r.name, true);
    text("$parent", r.parent, false);
    if (const JsonValue c = root.get("$comment"); c.isValid() && !c.isString()) {
        diags.add(path, "$comment must be a string");
        ok = false;
    }
    if (r.src->type->decl != refl::DeclKind::Record) {
        diags.add(path, std::format("'{}' is not a record type", r.src->type->qualifiedName));
        ok = false;
    }
    return ok;
}

} // namespace

bool isValidTagName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 256) return false;
    bool segStart = true;
    for (const char c : name) {
        if (segStart) {
            if (!isSegStart(c)) return false;
            segStart = false;
        } else if (c == '.') {
            segStart = true;
        } else if (!isSegChar(c)) {
            return false;
        }
    }
    return !segStart;
}

Result<CookOutput> cook(std::span<const SourceRecord> sources, const CookOptions& options, std::vector<CookDiagnostic>* diagnostics) {
    Diagnostics diags;
    auto fail = [&]() -> Result<CookOutput> {
        std::vector<CookDiagnostic> list = diags.take();
        std::string msg = std::format("records cook failed with {} error(s):", list.size());
        for (const CookDiagnostic& d : list) msg += std::format("\n  {}{}{}", d.path, d.path.empty() ? "" : ": ", d.message);
        if (diagnostics) *diagnostics = std::move(list);
        return Error{ErrorCode::InvalidArgument, std::move(msg)};
    };

    // 1. Parse every source and its header, in path order.
    std::vector<Rec> recs(sources.size());
    std::vector<usize> order(sources.size());
    for (usize i = 0; i < sources.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return sources[a].path < sources[b].path; });
    std::unordered_map<refl::RecordId, const Rec*> byId;
    std::unordered_map<std::string_view, usize> byName;
    std::map<refl::TypeId, const TypeInfo*> typeIds;
    for (usize k = 0; k < order.size(); ++k) {
        const usize i = order[k];
        Rec& r = recs[i];
        r.src = &sources[i];
        // Two sources with one path would make "first" depend on the caller's order (sorting is stable).
        if ((k > 0 && sources[order[k - 1]].path == r.src->path) || (k + 1 < order.size() && sources[order[k + 1]].path == r.src->path)) {
            diags.add(r.src->path, "another source has the same path");
            r.state = State::Failed;
            continue;
        }
        if (!r.src->type) {
            diags.add(r.src->path, "no record type");
            r.state = State::Failed;
            continue;
        }
        // The cooked type table and the loader find types by TypeId.
        if (auto [it, fresh] = typeIds.emplace(r.src->type->id, r.src->type); !fresh && it->second != r.src->type) {
            diags.add(r.src->path, std::format("record type '{}' has TypeId {}, which record type '{}' also has", r.src->type->qualifiedName,
                                               r.src->type->id, it->second->qualifiedName));
            r.state = State::Failed;
            continue;
        }
        auto doc = refl::JsonDocument::parse(r.src->text, r.src->path);
        if (!doc) {
            diags.add(r.src->path, doc.error().message);
            r.state = State::Failed;
            continue;
        }
        r.doc = std::move(*doc);
        if (!readHeader(r, diags)) {
            r.state = State::Failed;
            continue;
        }
        if (auto [it, fresh] = byId.emplace(r.rid, &r); !fresh) {
            diags.add(r.src->path, std::format("$rid {} is already used by {} (record ids are minted once and never reused)", r.rid,
                                               it->second->src->path));
            r.state = State::Failed;
        }
        if (auto [it, fresh] = byName.emplace(r.name, i); !fresh) {
            diags.add(r.src->path, std::format("$name '{}' is already used by {}", r.name, recs[it->second].src->path));
            r.state = State::Failed;
        }
    }

    // 2. Inheritance, parents first; iterative, so a long $parent chain cannot exhaust the stack.
    CookStats stats;
    ReadCtx::Options readOptions;
    readOptions.strictUnknownFields = options.strictUnknownFields;
    for (const usize start : order) {
        if (recs[start].state != State::Unvisited) continue;
        std::vector<usize> chain;
        usize cur = start;
        while (true) {
            Rec& r = recs[cur];
            if (r.state == State::Done || r.state == State::Failed) break;
            if (r.state == State::Visiting) {
                const auto from = std::find(chain.begin(), chain.end(), cur);
                std::string cycle;
                for (auto it = from; it != chain.end(); ++it) cycle += std::format("'{}' -> ", recs[*it].name);
                cycle += std::format("'{}'", r.name);
                for (auto it = from; it != chain.end(); ++it) {
                    diags.add(recs[*it].src->path, std::format("$parent cycle: {}", cycle));
                    recs[*it].state = State::Failed;
                }
                break;
            }
            r.state = State::Visiting;
            chain.push_back(cur);
            if (r.parent.empty()) break;
            const auto p = byName.find(r.parent);
            if (p == byName.end()) {
                diags.add(r.src->path, std::format("$parent '{}' does not exist", r.parent));
                r.state = State::Failed;
                break;
            }
            if (recs[p->second].src->type != r.src->type) {
                diags.add(r.src->path, std::format("$parent '{}' is a {}, not a {}", r.parent, recs[p->second].src->type->qualifiedName,
                                                   r.src->type->qualifiedName));
                r.state = State::Failed;
                break;
            }
            cur = p->second;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            Rec& r = recs[*it];
            if (r.state == State::Failed) continue; // reported where it failed
            const Rec* parent = r.parent.empty() ? nullptr : &recs[byName.at(r.parent)];
            if (parent && parent->state != State::Done) {
                diags.add(r.src->path, std::format("$parent '{}' has errors", r.parent));
                r.state = State::Failed;
                continue;
            }
            r.value = refl::Value(*r.src->type);
            if (parent) {
                r.src->type->ops->copy(r.value.data(), parent->value.data());
                ++stats.inherited;
            }
            ReadCtx ctx(readOptions);
            Overlay overlay(ctx);
            if (auto res = overlay.fields(*r.src->type, r.value.data(), r.doc.root(), true, parent != nullptr); !res) {
                diags.add(r.src->path, res.error().message);
                r.state = State::Failed;
                continue;
            }
            // Reader warnings are errors, except ignored unknown fields when they are allowed: a keyed
            // list read without `$key`s gets random keys, which would make the cook non-deterministic.
            bool warned = false;
            for (const std::string& w : ctx.warnings()) {
                if (!options.strictUnknownFields && w.find("unknown field ignored") != std::string::npos) continue;
                diags.add(r.src->path, w);
                warned = true;
            }
            if (warned) {
                r.state = State::Failed;
                continue;
            }
            r.state = State::Done;
        }
    }

    // 3. References, tags and formulas over the resolved values (every field, both sides).
    std::vector<const Rec*> done;
    for (const usize i : order) {
        if (recs[i].state == State::Done) done.push_back(&recs[i]);
    }
    Scanner scanner(byId, options, diags);
    std::map<std::string, std::pair<u8, bool>> declared; // tag -> (audience, client-visible)
    for (const Rec* r : done) {
        scanner.record(*r);
        for (const TagDeclarationType& d : options.tagDeclarations) {
            if (r->src->type->qualifiedName != d.typeName) continue;
            const FieldInfo* tf = r->src->type->field(d.tagField);
            const FieldInfo* af = r->src->type->field(d.audienceField);
            if (!tf || !af || tf->type().kind != Kind::Name || af->type().kind != Kind::Enum) {
                diags.add(r->src->path, std::format("tag declaration type '{}' needs a Name field '{}' and an enum field '{}'", d.typeName,
                                                    d.tagField, d.audienceField));
                continue;
            }
            const std::string_view tag = static_cast<const Name*>(tf->ptr(r->value.data()))->view();
            const refl::EnumValue* ev = af->type().enumByValue(refl::readIntegerBits(af->type(), af->ptr(r->value.data())));
            const std::string_view an = ev ? ev->name : std::string_view();
            const u8 audience = an == "Server" ? u8{0} : an == "Owner" ? u8{1} : an == "All" ? u8{2} : u8{0xFF};
            if (!isValidTagName(tag)) {
                diags.add(r->src->path, std::format("{}: invalid tag name '{}'", d.tagField, tag));
            } else if (audience == 0xFF) {
                diags.add(r->src->path, std::format("{}: audience '{}' is not Server, Owner or All", d.audienceField, an));
            } else if (auto [it, fresh] = declared.emplace(std::string(tag), std::pair{audience, false}); !fresh && it->second.first != audience) {
                diags.add(r->src->path, std::format("tag '{}' is declared twice with different audiences", tag));
            } else {
                it->second.second |= !excludesType(*r->src->type, CookAudience::Client);
            }
        }
    }

    // 4. The tag table: used and declared tags plus their ancestors, in byte-wise name order.
    std::map<std::string, TagRow> rows;
    for (const auto& [name, use] : scanner.tags) {
        TagRow& t = rows[name];
        t.declared = true;
        t.client |= use.client;
    }
    for (const auto& [name, decl] : declared) {
        TagRow& t = rows[name];
        t.declared = t.declaredByRecord = true;
        t.audience = decl.first;
        t.client |= decl.second;
    }
    std::vector<std::string> names;
    for (const auto& [name, row] : rows) names.push_back(name);
    for (const std::string& n : names) {
        const bool client = rows[n].client;
        for (std::string_view p = parentTag(n); !p.empty(); p = parentTag(p)) rows[std::string(p)].client |= client;
    }
    std::vector<TagRow> tags;
    std::unordered_map<std::string_view, u16> tagIndex;
    if (rows.size() > hrdb::kMaxTags) {
        diags.add("", std::format("{} tags; at most {} fit a TagIndex", rows.size(), hrdb::kMaxTags));
    } else {
        for (auto& [name, row] : rows) {
            row.name = name;
            tags.push_back(row);
        }
        for (usize i = 0; i < tags.size(); ++i) tagIndex.emplace(tags[i].name, static_cast<u16>(i));
        for (usize i = 0; i < tags.size(); ++i) {
            TagRow& t = tags[i];
            const std::string_view p = parentTag(t.name);
            t.parent = p.empty() ? kNoTag : tagIndex.at(p);
            t.depth = static_cast<u8>(std::count(t.name.begin(), t.name.end(), '.'));
            t.subtreeEnd = static_cast<u16>(i + 1);
        }
        for (usize i = tags.size(); i-- > 0;) {
            if (tags[i].parent != kNoTag) tags[tags[i].parent].subtreeEnd = std::max(tags[tags[i].parent].subtreeEnd, tags[i].subtreeEnd);
        }
    }
    if (!diags.empty()) return fail();

    // 5. Both cooks, with one tag-table hash: the client's view of the table (withheld names left out).
    hrdb::TagTableHasher tagHash(static_cast<u32>(tags.size()));
    for (const TagRow& t : tags) tagHash.add(t.client ? std::string_view(t.name) : std::string_view(), t.parent, t.subtreeEnd, t.depth, t.audience, t.declared);
    const u64 tagTableHash = tagHash.digest();
    std::vector<const Rec*> sorted = done;
    std::sort(sorted.begin(), sorted.end(), [](const Rec* a, const Rec* b) { return a->rid < b->rid; });
    CookOutput out;
    for (const CookAudience audience : {CookAudience::Client, CookAudience::Server}) {
        std::vector<const Rec*> in;
        for (const Rec* r : sorted) {
            if (!excludesType(*r->src->type, audience)) in.push_back(r);
        }
        auto bytes = encodeDb(audience, in, tags, tagTableHash, tagIndex, scanner.bytecode, diags);
        if (!bytes) continue;
        (audience == CookAudience::Client ? out.client : out.server) = std::move(*bytes);
        (audience == CookAudience::Client ? stats.clientRecords : stats.serverRecords) = in.size();
    }
    if (!diags.empty()) return fail();
    stats.records = done.size();
    stats.tags = tags.size();
    stats.withheldTags = static_cast<usize>(std::count_if(tags.begin(), tags.end(), [](const TagRow& t) { return !t.client; }));
    stats.formulas = scanner.bytecode.size();
    out.stats = stats;
    if (diagnostics) diagnostics->clear();
    return out;
}

Result<std::vector<SourceRecord>> collectSources(const fs::Path& projectRoot, const refl::TypeRegistry& registry) {
    const fs::Path dir = projectRoot / "records";
    if (!fs::isDirectory(dir)) return Error{ErrorCode::NotFound, std::format("{}: no records/ directory", fs::pathToGenericUtf8(projectRoot))};
    std::map<std::string_view, const TypeInfo*> tables;
    std::map<std::string_view, std::string> ambiguous;
    for (const TypeInfo* t : registry.types()) {
        if (t->decl != refl::DeclKind::Record) continue;
        const auto* tbl = t->attr<refl::attrs::Table>();
        if (!tbl) continue;
        if (auto [it, fresh] = tables.emplace(tbl->name, t); !fresh) {
            ambiguous[tbl->name] = std::format("{} and {}", it->second->qualifiedName, t->qualifiedName);
        }
    }
    fs::ListOptions opts;
    opts.recursive = true;
    opts.includeDirectories = false;
    opts.extension = ".hrec";
    HELIOS_TRY_ASSIGN(const std::vector<fs::DirEntry> entries, fs::listDirectory(dir, opts));
    std::vector<SourceRecord> out;
    std::string errors;
    for (const fs::DirEntry& e : entries) {
        SourceRecord s;
        s.path = "records/" + e.relativePath;
        const usize slash = e.relativePath.find('/');
        const std::string_view table = slash == std::string::npos ? std::string_view() : std::string_view(e.relativePath).substr(0, slash);
        if (auto a = ambiguous.find(table); a != ambiguous.end()) {
            errors += std::format("\n  {}: table '{}' is ambiguous ({})", s.path, table, a->second);
            continue;
        }
        const auto t = tables.find(table);
        if (t == tables.end()) {
            errors += std::format("\n  {}: no record type has @table(\"{}\") (expected records/<table>/...)", s.path, table);
            continue;
        }
        s.type = t->second;
        HELIOS_TRY_ASSIGN(s.text, fs::readTextFile(e.path));
        out.push_back(std::move(s));
    }
    if (!errors.empty()) return Error{ErrorCode::NotFound, "cannot type every record file:" + errors};
    return out;
}

Result<void> writeCookOutput(const CookOutput& output, const fs::Path& outDir) {
    HELIOS_TRY(fs::createDirectories(outDir));
    HELIOS_TRY(fs::writeFile(outDir / fs::pathFromUtf8(kClientDbFile), output.client));
    HELIOS_TRY(fs::writeFile(outDir / fs::pathFromUtf8(kServerDbFile), output.server));
    return {};
}

} // namespace helios::records
