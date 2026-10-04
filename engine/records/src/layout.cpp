// Cooked layouts: audience filtering, AAA-SEC-4 type checks, sizes and structural hashes.
#include "helios/records/layout.h"

#include <algorithm>
#include <format>
#include <unordered_set>

#include "helios/core/assert.h"
#include "helios/core/hash.h"

namespace helios::records {
namespace {

using refl::FieldFlags;
using refl::Kind;
using refl::TypeFlags;
using refl::TypeInfo;

bool isServerSide(const TypeInfo& t) noexcept {
    return t.hasFlag(TypeFlags::ServerOnly) || t.hasFlag(TypeFlags::ServerPart) || t.attr("server_only") != nullptr;
}
bool isClientSide(const TypeInfo& t) noexcept {
    return t.hasFlag(TypeFlags::ClientOnly) || t.hasFlag(TypeFlags::ClientPart) || t.attr("client_only") != nullptr;
}

constexpr u32 alignUp(u32 v, u32 a) noexcept { return (v + a - 1) / a * a; }

u32 intWidth(Kind k) noexcept {
    switch (k) {
    case Kind::Bool:
    case Kind::I8:
    case Kind::U8: return 1;
    case Kind::I16:
    case Kind::U16: return 2;
    case Kind::I32:
    case Kind::U32: return 4;
    default: return 8;
    }
}

Kind unsignedKindOfSize(u32 size) noexcept {
    switch (size) {
    case 1: return Kind::U8;
    case 2: return Kind::U16;
    case 4: return Kind::U32;
    default: return Kind::U64;
    }
}

bool isRecordRef(const TypeInfo& t) noexcept {
    return t.kind == Kind::Builtin && t.elementFn && t.qualifiedName.starts_with("Ref<");
}

/// The first record type of the other side that `t` references through containers (not through
/// struct fields or variant alternatives, whose own layouts check their fields), or null.
const TypeInfo* otherSideRecord(const TypeInfo& t, CookAudience audience) noexcept {
    switch (t.kind) {
    case Kind::Builtin: return isRecordRef(t) && excludesType(t.element(), audience) ? &t.element() : nullptr;
    case Kind::Map:
        if (const TypeInfo* k = otherSideRecord(t.key(), audience)) return k;
        return otherSideRecord(t.element(), audience);
    case Kind::List:
    case Kind::KeyedList:
    case Kind::Set:
    case Kind::Optional:
    case Kind::Array: return otherSideRecord(t.element(), audience);
    default: return nullptr;
    }
}

std::string_view sideName(CookAudience audience) noexcept {
    return audience == CookAudience::Client ? "server-only" : "client-only";
}

} // namespace

const CookedLayout::Field* CookedLayout::field(std::string_view name) const noexcept {
    for (const Field& f : fields) {
        if (f.info->name == name) return &f;
    }
    return nullptr;
}

bool keepsField(const refl::FieldInfo& field, CookAudience audience) noexcept {
    if (refl::hasFlag(field.flags, FieldFlags::EditorOnly)) return false;
    if (audience == CookAudience::Client) {
        return !refl::hasFlag(field.flags, FieldFlags::ServerOnly) && field.attr("server_only") == nullptr;
    }
    return !refl::hasFlag(field.flags, FieldFlags::ClientOnly) && field.attr("client_only") == nullptr;
}

bool excludesType(const refl::TypeInfo& type, CookAudience audience) noexcept {
    return audience == CookAudience::Client ? isServerSide(type) : isClientSide(type);
}

LayoutCache::LayoutCache(CookAudience audience) : m_audience(audience) {}
LayoutCache::~LayoutCache() = default;

Result<const CookedLayout*> LayoutCache::get(const refl::TypeInfo& type) {
    auto built = build(type);
    // The elements of lists, sets and maps are built only once the by-value chain that reached them
    // has closed: a span's size does not depend on its element, and a type may hold a list of
    // something that holds it by value (a dialogue node's choices, each with an optional next node).
    // Building those elements eagerly would meet their container's struct half built, and refuse a
    // legal schema or not depending on which type was asked for first.
    // First in, first out: the first error reported is the one in the earliest field.
    for (usize i = 0; built && i < m_deferred.size(); ++i) {
        HELIOS_ASSERT(m_open.empty(), "a by-value chain is closed before its containers' elements are built");
        m_base = std::move(m_deferred[i].where);
        auto el = buildElements(*m_deferred[i].layout);
        if (!el) built = Error{el.error().code, m_base.empty() ? el.error().message : std::format("{}: {}", m_base, el.error().message)};
    }
    m_deferred.clear();
    m_base.clear();
    if (!built) {
        // Drop everything this call created: some of it is incomplete.
        for (CookedLayout* l : m_pending) m_layouts.erase(l->type);
        m_pending.clear();
        m_open.clear();
        m_deferred.clear();
        m_where.clear();
        return built.error();
    }
    finish();
    return static_cast<const CookedLayout*>(*built);
}

Result<void> LayoutCache::buildElements(CookedLayout& l) {
    const refl::TypeInfo& type = *l.type;
    if (type.kind == Kind::Map) {
        HELIOS_TRY_ASSIGN(l.key, build(type.key()));
    }
    HELIOS_TRY_ASSIGN(l.element, build(type.element()));
    return {};
}

std::string LayoutCache::where() const {
    std::string s = m_base;
    for (const std::string& w : m_where) {
        if (!s.empty()) s += ": ";
        s += w;
    }
    return s;
}

Result<const CookedLayout*> LayoutCache::byValue(const refl::TypeInfo& type, std::string_view what) {
    HELIOS_TRY_ASSIGN(CookedLayout* l, build(type));
    // Everything on the current by-value chain is open; reaching one of them again would make its
    // size depend on itself. Containers end the chain (their elements are built later).
    if (std::find(m_open.begin(), m_open.end(), l) != m_open.end()) {
        return makeError(ErrorCode::InvalidArgument, "{}: type '{}' contains itself by value", what, type.qualifiedName);
    }
    return static_cast<const CookedLayout*>(l);
}

Result<CookedLayout*> LayoutCache::build(const refl::TypeInfo& type) {
    if (auto it = m_layouts.find(&type); it != m_layouts.end()) return it->second.get();
    auto owned = std::make_unique<CookedLayout>();
    CookedLayout& l = *owned;
    l.type = &type;
    m_layouts.emplace(&type, std::move(owned));
    m_pending.push_back(&l);
    auto scalar = [&](Enc enc, u32 size) {
        l.enc = enc;
        l.size = size;
        l.align = size;
    };
    auto span = [&](Enc enc, u32 size) {
        l.enc = enc;
        l.size = size;
        l.align = 4;
    };
    auto embedsOtherSide = [&]() -> Result<CookedLayout*> {
        return makeError(ErrorCode::InvalidArgument, "{} type '{}' cannot be part of a {} cook (AAA-SEC-4)", sideName(m_audience),
                         type.qualifiedName, cookAudienceName(m_audience));
    };
    switch (type.kind) {
    case Kind::Bool: scalar(Enc::Bool, 1); break;
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
        scalar(Enc::Int, intWidth(type.kind));
        l.intKind = type.kind;
        break;
    case Kind::F32: scalar(Enc::F32, 4); break;
    case Kind::F64: scalar(Enc::F64, 8); break;
    case Kind::String:
    case Kind::Name: span(Enc::Text, 8); break;
    case Kind::Enum:
    case Kind::Flags: {
        if (excludesType(type, m_audience)) return embedsOtherSide();
        const Kind k = type.elementFn ? type.element().kind : unsignedKindOfSize(type.size);
        if (!refl::isIntegerKind(k) || intWidth(k) != type.size) {
            return makeError(ErrorCode::Unsupported, "enum '{}' has no integer underlying type of its size", type.qualifiedName);
        }
        scalar(Enc::Int, intWidth(k));
        l.intKind = k;
        break;
    }
    case Kind::Struct:
        if (excludesType(type, m_audience)) return embedsOtherSide();
        HELIOS_TRY(buildStruct(l));
        break;
    case Kind::Builtin: {
        const std::string_view n = type.qualifiedName;
        if (type.hasFlag(TypeFlags::Tuple)) {
            HELIOS_TRY(buildStruct(l));
        } else if (n == "Guid" || n == "AssetRef") {
            l.enc = Enc::Guid;
            l.size = 16;
            l.align = 8;
        } else if (n == "EntityId") {
            scalar(Enc::EntityId, 8);
        } else if (n == "NetHandle") {
            scalar(Enc::NetHandle, 4);
        } else if (n == "Duration") {
            scalar(Enc::Duration, 8);
        } else if (n == "LocString" || n == "TagQuery") {
            span(Enc::Text, 8);
        } else if (n == "TagSet") {
            span(Enc::TagSet, 8);
        } else if (n == "HxlExpr") {
            span(Enc::Hxl, 16);
        } else if (isRecordRef(type)) {
            scalar(Enc::RecordRef, 8);
        } else {
            return makeError(ErrorCode::Unsupported, "type '{}' has no cooked encoding in .hrdb v0", n);
        }
        break;
    }
    case Kind::List:
    case Kind::KeyedList:
    case Kind::Set: {
        if (type.kind == Kind::Set && !type.element().ops->keyLess) {
            return makeError(ErrorCode::Unsupported, "set '{}' has no canonical element order", type.qualifiedName);
        }
        if (type.kind == Kind::KeyedList) {
            span(Enc::KeyedList, 16);
        } else {
            span(type.kind == Kind::List ? Enc::List : Enc::Set, 8);
        }
        m_deferred.push_back(Deferred{&l, where()});
        break;
    }
    case Kind::Map: {
        if (!type.key().ops->keyLess) return makeError(ErrorCode::Unsupported, "map '{}' has no canonical key order", type.qualifiedName);
        span(Enc::Map, 8);
        m_deferred.push_back(Deferred{&l, where()});
        break;
    }
    case Kind::Optional: {
        m_open.push_back(&l);
        HELIOS_TRY_ASSIGN(l.element, byValue(type.element(), type.qualifiedName));
        m_open.pop_back();
        l.enc = Enc::Optional;
        l.align = std::max<u32>(1, l.element->align);
        l.payloadOffset = alignUp(1, l.align);
        l.size = alignUp(l.payloadOffset + l.element->size, l.align);
        break;
    }
    case Kind::Array: {
        // A zero-length array would give its lists a zero stride: the loader could then not bound a
        // list's count by the bytes it occupies (schemac allows sizes 1..65536 only).
        if (type.arraySize == 0) return makeError(ErrorCode::Unsupported, "array type '{}' has no elements", type.qualifiedName);
        m_open.push_back(&l);
        HELIOS_TRY_ASSIGN(l.element, byValue(type.element(), type.qualifiedName));
        m_open.pop_back();
        const u64 bytes = u64{type.arraySize} * l.element->size;
        if (bytes > (1ull << 24)) return makeError(ErrorCode::LimitExceeded, "array type '{}' is larger than 16 MiB", type.qualifiedName);
        l.enc = Enc::Array;
        l.count = type.arraySize;
        l.align = std::max<u32>(1, l.element->align);
        l.size = static_cast<u32>(bytes);
        break;
    }
    case Kind::Variant: {
        if (excludesType(type, m_audience)) return embedsOtherSide();
        if (type.alternatives.empty()) return makeError(ErrorCode::Unsupported, "variant '{}' has no alternatives", type.qualifiedName);
        u32 align = 4;
        u32 largest = 0;
        m_open.push_back(&l);
        for (const refl::VariantAlt& alt : type.alternatives) {
            HELIOS_TRY_ASSIGN(const CookedLayout* a, byValue(alt.type(), type.qualifiedName));
            l.alternatives.push_back(a);
            align = std::max(align, a->align);
            largest = std::max(largest, a->size);
        }
        m_open.pop_back();
        l.enc = Enc::Variant;
        l.align = align;
        l.payloadOffset = alignUp(4, align);
        l.size = alignUp(l.payloadOffset + largest, align);
        break;
    }
    }
    return &l;
}

Result<void> LayoutCache::buildStruct(CookedLayout& l) {
    m_open.push_back(&l);
    u32 offset = 0;
    u32 align = 1;
    for (const refl::FieldInfo& f : l.type->fields) {
        if (!keepsField(f, m_audience)) continue;
        const refl::TypeInfo& ft = f.type();
        if (const TypeInfo* other = otherSideRecord(ft, m_audience);
            other && !refl::hasFlag(f.flags, FieldFlags::Opaque) && f.attr("opaque") == nullptr) {
            return makeError(ErrorCode::InvalidArgument,
                             "field '{}.{}' references {} record type '{}' in a {} cook; mark it @opaque (the reference then "
                             "cooks as a bare RecordId) or move it into a {} {{}} block (AAA-SEC-4)",
                             l.type->qualifiedName, f.name, sideName(m_audience), other->qualifiedName,
                             cookAudienceName(m_audience), m_audience == CookAudience::Client ? "server" : "client");
        }
        m_where.push_back(std::format("field '{}.{}'", l.type->qualifiedName, f.name));
        auto fl = byValue(ft, std::format("{}.{}", l.type->qualifiedName, f.name));
        if (!fl) return Error{fl.error().code, std::format("{}: {}", m_where.back(), fl.error().message)};
        m_where.pop_back();
        offset = alignUp(offset, (*fl)->align);
        l.fields.push_back(CookedLayout::Field{&f, *fl, offset});
        offset += (*fl)->size;
        align = std::max(align, (*fl)->align);
        if (offset > (1u << 24)) return makeError(ErrorCode::LimitExceeded, "struct '{}' is larger than 16 MiB", l.type->qualifiedName);
    }
    l.enc = Enc::Struct;
    l.align = align;
    // At least one byte, so every element of a list occupies the file and validation stays linear.
    l.size = std::max<u32>(1, alignUp(offset, align));
    m_open.pop_back();
    return {};
}

void LayoutCache::finish() {
    for (CookedLayout* l : m_pending) {
        if (l->enc != Enc::Map) continue;
        const u32 align = std::max(l->key->align, l->element->align);
        l->payloadOffset = alignUp(l->key->size, l->element->align);
        l->entrySize = alignUp(l->payloadOffset + l->element->size, align);
    }
    // Shallow hash: the node itself, naming its children by type. The structural hash of a layout is the
    // hash of the sorted set of shallow hashes of everything reachable from it, so it is the same whatever
    // order the layouts were built in (recursive types reach themselves through lists).
    auto shallow = [](const CookedLayout& l) {
        Hasher64 h(0x48524442ull); // "HRDB"
        auto u32v = [&](u32 v) { h.updateValue(v); };
        auto text = [&](std::string_view s) {
            u32v(static_cast<u32>(s.size()));
            h.update(s);
        };
        u32v(static_cast<u32>(l.enc));
        u32v(static_cast<u32>(l.intKind));
        u32v(l.size);
        u32v(l.align);
        u32v(l.count);
        u32v(l.payloadOffset);
        u32v(l.entrySize);
        text(l.type->qualifiedName);
        for (const CookedLayout::Field& f : l.fields) {
            u32v(f.info->id);
            text(f.info->name);
            u32v(f.offset);
            text(f.layout->type->qualifiedName);
        }
        if (l.element) text(l.element->type->qualifiedName);
        if (l.key) text(l.key->type->qualifiedName);
        for (usize i = 0; i < l.alternatives.size(); ++i) {
            text(l.type->alternatives[i].name);
            u32v(l.type->alternatives[i].id);
            text(l.alternatives[i]->type->qualifiedName);
        }
        if (l.type->kind == Kind::Enum || l.type->kind == Kind::Flags) {
            for (const refl::EnumValue& v : l.type->enumValues) {
                text(v.name);
                h.updateValue(v.value);
            }
        }
        return h.digest();
    };
    std::unordered_map<const CookedLayout*, u64> shallowOf;
    for (const CookedLayout* l : m_pending) shallowOf.emplace(l, shallow(*l));
    std::vector<CookedLayout*> pending = std::move(m_pending);
    m_pending.clear();
    for (CookedLayout* root : pending) {
        std::vector<const CookedLayout*> stack{root};
        std::unordered_set<const CookedLayout*> seen{root};
        std::vector<u64> hashes;
        while (!stack.empty()) {
            const CookedLayout* l = stack.back();
            stack.pop_back();
            auto it = shallowOf.find(l);
            hashes.push_back(it != shallowOf.end() ? it->second : shallow(*l));
            auto visit = [&](const CookedLayout* c) {
                if (c && seen.insert(c).second) stack.push_back(c);
            };
            for (const CookedLayout::Field& f : l->fields) visit(f.layout);
            visit(l->element);
            visit(l->key);
            for (const CookedLayout* a : l->alternatives) visit(a);
        }
        std::sort(hashes.begin(), hashes.end());
        hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
        root->hash = hash64(hashes.data(), hashes.size() * sizeof(u64), hrdb::kFormatVersion);
    }
}

} // namespace helios::records
