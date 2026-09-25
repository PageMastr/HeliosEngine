// Type-erased serialization: dispatch to compiled codecs, and the reflection walker that mirrors
// them byte for byte (see codec.h for the typed implementation this must match).

#include "helios/reflect/serialize.h"

#include <cstring>
#include <format>
#include <limits>
#include <new>

#include "helios/reflect/codec.h"

namespace helios::refl {

namespace {

// ---------------------------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------------------------

bool isWrappedElement(const TypeInfo& e) noexcept { return isRepeatedKind(e.kind) || e.kind == Kind::Optional; }
WireType elementWire(const TypeInfo& e) noexcept { return isWrappedElement(e) ? WireType::Len : e.wire; }
bool isPackable(const TypeInfo& e) noexcept { return elementWire(e) != WireType::Len; }

bool inlineJson(const TypeInfo& t) noexcept {
    if (t.kind == Kind::Optional) return inlineJson(t.element());
    if (isScalarKind(t.kind) || t.kind == Kind::String || t.kind == Kind::Name || t.kind == Kind::Enum ||
        t.kind == Kind::Flags)
        return true;
    if (t.kind == Kind::Builtin) return t.hasFlag(TypeFlags::InlineJson);
    return false;
}

Kind underlyingKind(const TypeInfo& t) noexcept {
    if ((t.kind == Kind::Enum || t.kind == Kind::Flags) && t.elementFn) return t.element().kind;
    if (t.kind == Kind::Enum || t.kind == Kind::Flags) {
        switch (t.size) {
        case 1: return Kind::U8;
        case 2: return Kind::U16;
        case 4: return Kind::U32;
        default: return Kind::U64;
        }
    }
    return t.kind;
}

bool fitsKind(Kind k, i64 s) noexcept {
    switch (k) {
    case Kind::I8: return s >= std::numeric_limits<i8>::min() && s <= std::numeric_limits<i8>::max();
    case Kind::I16: return s >= std::numeric_limits<i16>::min() && s <= std::numeric_limits<i16>::max();
    case Kind::I32: return s >= std::numeric_limits<i32>::min() && s <= std::numeric_limits<i32>::max();
    default: return true;
    }
}
bool fitsKindU(Kind k, u64 u) noexcept {
    switch (k) {
    case Kind::U8: return u <= std::numeric_limits<u8>::max();
    case Kind::U16: return u <= std::numeric_limits<u16>::max();
    case Kind::U32: return u <= std::numeric_limits<u32>::max();
    default: return true;
    }
}

void* mut(const void* p) noexcept { return const_cast<void*>(p); }

struct KV {
    const void* key;
    const void* value;
};
std::vector<KV> collect(const TypeInfo& t, const void* container) {
    std::vector<KV> out;
    t.ops->forEach(container, &out, [](void* user, const void* k, const void* v) {
        static_cast<std::vector<KV>*>(user)->push_back({k, v});
    });
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Walker: equality and defaults
// ---------------------------------------------------------------------------------------------

namespace walk {

namespace {
bool fieldDefault(const FieldInfo& f, const void* p) {
    return f.defaultValue ? walk::equals(f.type(), p, f.defaultValue) : walk::isDefault(f.type(), p);
}
} // namespace

bool isDefault(const TypeInfo& t, const void* v) {
    switch (t.kind) {
    case Kind::Bool:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64: return readIntegerBits(t, v) == 0;
    case Kind::F32: return Codec<f32>::isDefault(*static_cast<const f32*>(v));
    case Kind::F64: return Codec<f64>::isDefault(*static_cast<const f64*>(v));
    case Kind::String: return static_cast<const std::string*>(v)->empty();
    case Kind::Name: return static_cast<const Name*>(v)->isNone();
    case Kind::Enum:
    case Kind::Flags: return readIntegerBits(t, v) == detail::enumDefaultValue(t);
    case Kind::Builtin: return t.ops->isDefault(v);
    case Kind::Struct:
        for (const FieldInfo& f : t.fields) {
            if (!fieldDefault(f, f.ptr(v))) return false;
        }
        return true;
    case Kind::List:
    case Kind::KeyedList:
    case Kind::Set:
    case Kind::Map: return t.ops->size(v) == 0;
    case Kind::Optional: return !t.ops->has(v);
    case Kind::Array:
        for (usize i = 0; i < t.arraySize; ++i) {
            if (!walk::isDefault(t.element(), t.ops->element(mut(v), i))) return false;
        }
        return true;
    case Kind::Variant: {
        if (t.ops->index(v) != 0) return false;
        return walk::isDefault(t.alternatives[0].type(), t.ops->alt(mut(v)));
    }
    }
    return false;
}

bool equals(const TypeInfo& t, const void* a, const void* b) {
    switch (t.kind) {
    case Kind::Bool:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::Enum:
    case Kind::Flags: return readIntegerBits(t, a) == readIntegerBits(t, b);
    case Kind::F32: return std::memcmp(a, b, 4) == 0;
    case Kind::F64: return std::memcmp(a, b, 8) == 0;
    case Kind::String: return *static_cast<const std::string*>(a) == *static_cast<const std::string*>(b);
    case Kind::Name: return *static_cast<const Name*>(a) == *static_cast<const Name*>(b);
    case Kind::Builtin: return t.ops->equals(a, b);
    case Kind::Struct:
        for (const FieldInfo& f : t.fields) {
            if (!walk::equals(f.type(), f.ptr(a), f.ptr(b))) return false;
        }
        return true;
    case Kind::List:
    case Kind::KeyedList:
    case Kind::Array: {
        const usize n = t.ops->size(a);
        if (n != t.ops->size(b)) return false;
        for (usize i = 0; i < n; ++i) {
            if (t.kind == Kind::KeyedList && t.ops->keyAt(a, i) != t.ops->keyAt(b, i)) return false;
            if (!walk::equals(t.element(), t.ops->element(mut(a), i), t.ops->element(mut(b), i))) return false;
        }
        return true;
    }
    case Kind::Set:
    case Kind::Map: {
        const auto ea = collect(t, a);
        const auto eb = collect(t, b);
        if (ea.size() != eb.size()) return false;
        const TypeInfo& keyType = t.kind == Kind::Map ? t.key() : t.element();
        for (usize i = 0; i < ea.size(); ++i) {
            if (!walk::equals(keyType, ea[i].key, eb[i].key)) return false;
            if (t.kind == Kind::Map && !walk::equals(t.element(), ea[i].value, eb[i].value)) return false;
        }
        return true;
    }
    case Kind::Optional: {
        const bool ha = t.ops->has(a);
        if (ha != t.ops->has(b)) return false;
        return !ha || walk::equals(t.element(), t.ops->get(mut(a)), t.ops->get(mut(b)));
    }
    case Kind::Variant: {
        const u32 ia = t.ops->index(a);
        if (ia != t.ops->index(b)) return false;
        return walk::equals(t.alternatives[ia].type(), t.ops->alt(mut(a)), t.ops->alt(mut(b)));
    }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Walker: tagged binary
// ---------------------------------------------------------------------------------------------

namespace {
void writeFieldW(TaggedWriter& w, u32 id, const TypeInfo& t, const void* v);

void writeFieldsW(TaggedWriter& w, const TypeInfo& t, const void* obj) {
    for (const FieldInfo& f : t.fields) {
        const void* p = f.ptr(obj);
        if (!fieldDefault(f, p)) writeFieldW(w, f.id, f.type(), p);
    }
}

void writeValueW(TaggedWriter& w, const TypeInfo& t, const void* v) {
    switch (t.kind) {
    case Kind::Bool:
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64: w.writeVarint(static_cast<u64>(readIntegerBits(t, v))); return;
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: w.writeZigZag(readIntegerBits(t, v)); return;
    case Kind::Enum:
    case Kind::Flags:
        if (isSignedKind(underlyingKind(t))) {
            w.writeZigZag(readIntegerBits(t, v));
        } else {
            w.writeVarint(static_cast<u64>(readIntegerBits(t, v)));
        }
        return;
    case Kind::F32: w.writeF32(*static_cast<const f32*>(v)); return;
    case Kind::F64: w.writeF64(*static_cast<const f64*>(v)); return;
    case Kind::String: w.writeString(*static_cast<const std::string*>(v)); return;
    case Kind::Name: w.writeString(static_cast<const Name*>(v)->view()); return;
    case Kind::Builtin: t.ops->writeTagged(w, v); return;
    case Kind::Struct: {
        const usize m = w.beginLen();
        writeFieldsW(w, t, v);
        w.endLen(m);
        return;
    }
    case Kind::Variant: {
        const usize m = w.beginLen();
        const VariantAlt& alt = t.alternatives[t.ops->index(v)];
        writeFieldW(w, alt.id, alt.type(), t.ops->alt(mut(v)));
        w.endLen(m);
        return;
    }
    default: HELIOS_ASSERT(false, "writeValueW: repeated/optional kinds have no single value"); return;
    }
}

void writeElementW(TaggedWriter& w, const TypeInfo& e, const void* v) {
    if (isWrappedElement(e)) {
        const usize m = w.beginLen();
        writeFieldW(w, 1, e, v);
        w.endLen(m);
    } else {
        writeValueW(w, e, v);
    }
}

void writeRepeatedW(TaggedWriter& w, u32 id, const TypeInfo& t, const void* v) {
    switch (t.kind) {
    case Kind::List: {
        const TypeInfo& e = t.element();
        const usize n = t.ops->size(v);
        if (n == 0) return;
        if (isPackable(e)) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            for (usize i = 0; i < n; ++i) writeValueW(w, e, t.ops->element(mut(v), i));
            w.endLen(m);
        } else {
            for (usize i = 0; i < n; ++i) {
                w.writeTag(id, elementWire(e));
                writeElementW(w, e, t.ops->element(mut(v), i));
            }
        }
        return;
    }
    case Kind::Set: {
        const TypeInfo& e = t.element();
        const auto items = collect(t, v);
        if (items.empty()) return;
        if (isPackable(e)) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            for (const KV& kv : items) writeValueW(w, e, kv.key);
            w.endLen(m);
        } else {
            for (const KV& kv : items) {
                w.writeTag(id, elementWire(e));
                writeElementW(w, e, kv.key);
            }
        }
        return;
    }
    case Kind::Array: {
        const TypeInfo& e = t.element();
        w.writeTag(id, WireType::Len);
        const usize m = w.beginLen();
        for (usize i = 0; i < t.arraySize; ++i) {
            const void* p = t.ops->element(mut(v), i);
            if (isPackable(e)) {
                writeValueW(w, e, p);
            } else {
                w.writeTag(1, elementWire(e));
                writeElementW(w, e, p);
            }
        }
        w.endLen(m);
        return;
    }
    case Kind::KeyedList: {
        const TypeInfo& e = t.element();
        const usize n = t.ops->size(v);
        for (usize i = 0; i < n; ++i) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            const Guid key = t.ops->keyAt(v, i);
            writeField(w, 1, key);
            writeFieldW(w, 2, e, t.ops->element(mut(v), i));
            w.endLen(m);
        }
        return;
    }
    case Kind::Map: {
        for (const KV& kv : collect(t, v)) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            writeFieldW(w, 1, t.key(), kv.key);
            writeFieldW(w, 2, t.element(), kv.value);
            w.endLen(m);
        }
        return;
    }
    default: return;
    }
}

void writeFieldW(TaggedWriter& w, u32 id, const TypeInfo& t, const void* v) {
    if (isRepeatedKind(t.kind)) {
        writeRepeatedW(w, id, t, v);
    } else if (t.kind == Kind::Optional) {
        if (t.ops->has(v)) writeFieldW(w, id, t.element(), t.ops->get(mut(v)));
    } else {
        w.writeTag(id, t.wire);
        writeValueW(w, t, v);
    }
}

Result<void> readFieldW(TaggedReader& r, WireType wire, const TypeInfo& t, void* v);

Result<void> readFieldsW(TaggedReader& r, const TypeInfo& t, void* obj) {
    while (!r.atEnd()) {
        HELIOS_TRY_ASSIGN(const FieldTag tag, r.readTag());
        const FieldInfo* f = t.fieldById(tag.id);
        if (!f) {
            HELIOS_TRY(r.skip(tag.wire));
            continue;
        }
        HELIOS_TRY(readFieldW(r, tag.wire, f->type(), f->ptr(obj)));
    }
    return {};
}

Result<void> readValueW(TaggedReader& r, WireType wire, const TypeInfo& t, void* v) {
    switch (t.kind) {
    case Kind::Bool: {
        HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "bool"));
        *static_cast<bool*>(v) = raw != 0;
        return {};
    }
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64:
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64:
    case Kind::Enum:
    case Kind::Flags: {
        const Kind k = underlyingKind(t);
        HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "integer"));
        if (isSignedKind(k)) {
            const i64 s = zigzagDecode(raw);
            if (!fitsKind(k, s)) return detail::integerOutOfRange("integer", s);
            writeIntegerBits(t, v, s);
        } else {
            if (!fitsKindU(k, raw)) return detail::integerOutOfRangeU("integer", raw);
            writeIntegerBits(t, v, static_cast<i64>(raw));
        }
        return {};
    }
    case Kind::F32: return Codec<f32>::readValue(r, wire, *static_cast<f32*>(v));
    case Kind::F64: return Codec<f64>::readValue(r, wire, *static_cast<f64*>(v));
    case Kind::String: return Codec<std::string>::readValue(r, wire, *static_cast<std::string*>(v));
    case Kind::Name: return Codec<Name>::readValue(r, wire, *static_cast<Name*>(v));
    case Kind::Builtin: return t.ops->readTagged(r, wire, v);
    case Kind::Struct: {
        if (wire != WireType::Len) return wireTypeMismatch("struct", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        return readFieldsW(sub, t, v);
    }
    case Kind::Variant: {
        if (wire != WireType::Len) return wireTypeMismatch("variant", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            const VariantAlt* alt = t.alternativeById(tag.id);
            if (!alt) {
                HELIOS_TRY(sub.skip(tag.wire));
                continue;
            }
            const u32 index = static_cast<u32>(alt - t.alternatives.data());
            void* altPtr = t.ops->index(v) == index ? t.ops->alt(v) : t.ops->emplaceAlt(v, index);
            HELIOS_TRY(readFieldW(sub, tag.wire, alt->type(), altPtr));
        }
        return {};
    }
    default: return Error{ErrorCode::InvalidState, "readValueW: repeated/optional kinds have no single value"};
    }
}

Result<void> readElementW(TaggedReader& r, WireType wire, const TypeInfo& e, void* v) {
    if (!isWrappedElement(e)) return readValueW(r, wire, e, v);
    if (wire != WireType::Len) return wireTypeMismatch("wrapped element", wire, WireType::Len);
    HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
    while (!sub.atEnd()) {
        HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
        if (tag.id == 1) {
            HELIOS_TRY(readFieldW(sub, tag.wire, e, v));
        } else {
            HELIOS_TRY(sub.skip(tag.wire));
        }
    }
    return {};
}

Result<void> readRepeatedW(TaggedReader& r, WireType wire, const TypeInfo& t, void* v) {
    const TypeInfo& e = t.element();
    switch (t.kind) {
    case Kind::List:
    case Kind::Set: {
        auto sink = [&](Value& item) {
            if (t.kind == Kind::List) {
                const usize n = t.ops->size(v);
                t.ops->resize(v, n + 1);
                e.ops->copy(t.ops->element(v, n), item.data());
            } else {
                t.ops->findOrInsert(v, item.data());
            }
        };
        if (isPackable(e) && wire == WireType::Len) {
            HELIOS_TRY_ASSIGN(const auto payload, r.readLen());
            TaggedReader sub(payload, r.depth());
            while (!sub.atEnd()) {
                Value item(e);
                HELIOS_TRY(readValueW(sub, e.wire, e, item.data()));
                sink(item);
            }
            return {};
        }
        Value item(e);
        HELIOS_TRY(readElementW(r, wire, e, item.data()));
        sink(item);
        return {};
    }
    case Kind::Array: {
        if (wire != WireType::Len) return wireTypeMismatch("array", wire, WireType::Len);
        usize count = 0;
        auto store = [&](Value& item) -> Result<void> {
            if (count >= t.arraySize) return makeError(ErrorCode::Corrupt, "array: more than {} elements", t.arraySize);
            e.ops->copy(t.ops->element(v, count++), item.data());
            return {};
        };
        if (isPackable(e)) {
            HELIOS_TRY_ASSIGN(const auto payload, r.readLen());
            TaggedReader sub(payload, r.depth());
            while (!sub.atEnd()) {
                Value item(e);
                HELIOS_TRY(readValueW(sub, e.wire, e, item.data()));
                HELIOS_TRY(store(item));
            }
            return {};
        }
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            if (tag.id != 1) {
                HELIOS_TRY(sub.skip(tag.wire));
                continue;
            }
            Value item(e);
            HELIOS_TRY(readElementW(sub, tag.wire, e, item.data()));
            HELIOS_TRY(store(item));
        }
        return {};
    }
    case Kind::KeyedList: {
        if (wire != WireType::Len) return wireTypeMismatch("keyed list entry", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        Guid key;
        Value item(e);
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            if (tag.id == 1) {
                HELIOS_TRY(readField(sub, tag.wire, key));
            } else if (tag.id == 2) {
                HELIOS_TRY(readFieldW(sub, tag.wire, e, item.data()));
            } else {
                HELIOS_TRY(sub.skip(tag.wire));
            }
        }
        if (key.isNil()) return Error{ErrorCode::Corrupt, "keyed list entry without key"};
        const usize n = t.ops->size(v);
        for (usize i = 0; i < n; ++i) {
            if (t.ops->keyAt(v, i) == key) return Error{ErrorCode::Corrupt, "duplicate keyed list key " + key.toString()};
        }
        e.ops->copy(t.ops->insertAt(v, n), item.data());
        t.ops->setKeyAt(v, n, key);
        return {};
    }
    case Kind::Map: {
        if (wire != WireType::Len) return wireTypeMismatch("map entry", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        Value key(t.key());
        Value value(e);
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            if (tag.id == 1) {
                HELIOS_TRY(readFieldW(sub, tag.wire, t.key(), key.data()));
            } else if (tag.id == 2) {
                HELIOS_TRY(readFieldW(sub, tag.wire, e, value.data()));
            } else {
                HELIOS_TRY(sub.skip(tag.wire));
            }
        }
        e.ops->copy(t.ops->findOrInsert(v, key.data()), value.data());
        return {};
    }
    default: return Error{ErrorCode::InvalidState, "readRepeatedW: not a repeated kind"};
    }
}

Result<void> readFieldW(TaggedReader& r, WireType wire, const TypeInfo& t, void* v) {
    if (isRepeatedKind(t.kind)) return readRepeatedW(r, wire, t, v);
    if (t.kind == Kind::Optional) return readFieldW(r, wire, t.element(), t.ops->emplace(v));
    return readValueW(r, wire, t, v);
}
} // namespace

void encodeTagged(const TypeInfo& type, const void* obj, std::vector<u8>& out) {
    HELIOS_ASSERT(type.kind == Kind::Struct, "encodeTagged needs a struct type");
    TaggedWriter w(out);
    writeFieldsW(w, type, obj);
}

void writeFields(const TypeInfo& type, const void* obj, TaggedWriter& out) { writeFieldsW(out, type, obj); }

Result<void> readFields(const TypeInfo& type, void* obj, TaggedReader& in) { return readFieldsW(in, type, obj); }

Result<void> decodeTagged(const TypeInfo& type, void* obj, std::span<const u8> bytes) {
    if (type.kind != Kind::Struct) return Error{ErrorCode::InvalidArgument, "decodeTagged needs a struct type"};
    TaggedReader r(bytes);
    return readFieldsW(r, type, obj);
}

// ---------------------------------------------------------------------------------------------
// Walker: JSON
// ---------------------------------------------------------------------------------------------

void writeJsonFields(const TypeInfo& t, const void* obj, JsonWriter& out) {
    for (const FieldInfo& f : t.fields) {
        const void* p = f.ptr(obj);
        if (fieldDefault(f, p)) continue;
        out.key(f.name);
        walk::writeJson(f.type(), p, out);
    }
}

void writeJson(const TypeInfo& t, const void* v, JsonWriter& out) {
    switch (t.kind) {
    case Kind::Bool: out.boolean(*static_cast<const bool*>(v)); return;
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: out.integer(readIntegerBits(t, v)); return;
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64: out.unsignedInteger(static_cast<u64>(readIntegerBits(t, v))); return;
    case Kind::F32: out.numberF32(*static_cast<const f32*>(v)); return;
    case Kind::F64: out.number(*static_cast<const f64*>(v)); return;
    case Kind::String: out.string(*static_cast<const std::string*>(v)); return;
    case Kind::Name: out.string(static_cast<const Name*>(v)->view()); return;
    case Kind::Enum:
    case Kind::Flags: detail::writeEnumJson(out, t, readIntegerBits(t, v)); return;
    case Kind::Builtin: t.ops->writeJson(out, v); return;
    case Kind::Struct:
        out.beginObject();
        writeJsonFields(t, v, out);
        out.endObject();
        return;
    case Kind::List:
    case Kind::Array: {
        const TypeInfo& e = t.element();
        out.beginArray(inlineJson(e));
        const usize n = t.ops->size(v);
        for (usize i = 0; i < n; ++i) walk::writeJson(e, t.ops->element(mut(v), i), out);
        out.endArray();
        return;
    }
    case Kind::Set: {
        const TypeInfo& e = t.element();
        out.beginArray(inlineJson(e));
        for (const KV& kv : collect(t, v)) walk::writeJson(e, kv.key, out);
        out.endArray();
        return;
    }
    case Kind::KeyedList: {
        const TypeInfo& e = t.element();
        out.beginArray(false);
        const usize n = t.ops->size(v);
        for (usize i = 0; i < n; ++i) {
            out.beginObject();
            out.key("$key");
            Codec<Guid>::writeJson(out, t.ops->keyAt(v, i));
            writeJsonFields(e, t.ops->element(mut(v), i), out);
            out.endObject();
        }
        out.endArray();
        return;
    }
    case Kind::Map: {
        out.beginObject();
        for (const KV& kv : collect(t, v)) {
            out.key(t.key().ops->keyToText(kv.key));
            walk::writeJson(t.element(), kv.value, out);
        }
        out.endObject();
        return;
    }
    case Kind::Optional:
        if (t.ops->has(v)) {
            walk::writeJson(t.element(), t.ops->get(mut(v)), out);
        } else {
            out.null();
        }
        return;
    case Kind::Variant: {
        const VariantAlt& alt = t.alternatives[t.ops->index(v)];
        if (alt.type().fields.empty()) {
            out.string(alt.name);
            return;
        }
        out.beginObject();
        out.key(alt.name);
        walk::writeJson(alt.type(), t.ops->alt(mut(v)), out);
        out.endObject();
        return;
    }
    }
}

Result<void> readJson(const TypeInfo& t, void* v, JsonValue in, ReadCtx& ctx) {
    switch (t.kind) {
    case Kind::Bool: return Codec<bool>::readJson(in, *static_cast<bool*>(v), ctx);
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: {
        i64 s = 0;
        if (!in.getI64(s)) return ctx.typeError("integer", in);
        if (!fitsKind(t.kind, s)) return ctx.error("integer out of range");
        writeIntegerBits(t, v, s);
        return {};
    }
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64: {
        u64 u = 0;
        if (!in.getU64(u)) return ctx.typeError("unsigned integer", in);
        if (!fitsKindU(t.kind, u)) return ctx.error("integer out of range");
        writeIntegerBits(t, v, static_cast<i64>(u));
        return {};
    }
    case Kind::F32: return Codec<f32>::readJson(in, *static_cast<f32*>(v), ctx);
    case Kind::F64: return Codec<f64>::readJson(in, *static_cast<f64*>(v), ctx);
    case Kind::String: return Codec<std::string>::readJson(in, *static_cast<std::string*>(v), ctx);
    case Kind::Name: return Codec<Name>::readJson(in, *static_cast<Name*>(v), ctx);
    case Kind::Enum:
    case Kind::Flags: {
        HELIOS_TRY_ASSIGN(const i64 bits, detail::readEnumJson(in, t, ctx));
        const Kind k = underlyingKind(t);
        if (isSignedKind(k) ? !fitsKind(k, bits) : !fitsKindU(k, static_cast<u64>(bits)))
            return ctx.error("enum value out of range");
        writeIntegerBits(t, v, bits);
        return {};
    }
    case Kind::Builtin: return t.ops->readJson(in, v, ctx);
    case Kind::Struct: {
        if (!in.isObject()) return ctx.typeError("object", in);
        // @was aliases first: the current name wins when both are present (as in Codec<S>).
        const bool aliases = detail::hasFieldAliases(t);
        for (int pass = aliases ? 0 : 1; pass < 2; ++pass) {
            for (const JsonValue::Member m : in.members()) {
                if (!m.key.empty() && m.key[0] == '$') continue;
                if (aliases && detail::isFieldAlias(t, m.key) != (pass == 0)) continue;
                ReadCtx::Scope s(ctx, m.key);
                const FieldInfo* f = t.fieldOrAlias(m.key);
                if (!f) {
                    HELIOS_TRY(ctx.unknownField(m.key));
                    continue;
                }
                HELIOS_TRY(walk::readJson(f->type(), f->ptr(v), m.value, ctx));
            }
        }
        return {};
    }
    case Kind::List: {
        if (in.isNull()) { // empty container (Go writes nil slices and maps as null)
            t.ops->resize(v, 0);
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        const TypeInfo& e = t.element();
        t.ops->resize(v, 0);
        usize i = 0;
        for (JsonValue el : in.elements()) {
            ReadCtx::Scope s(ctx, i);
            t.ops->resize(v, i + 1);
            HELIOS_TRY(walk::readJson(e, t.ops->element(v, i), el, ctx));
            ++i;
        }
        return {};
    }
    case Kind::Array: {
        if (!in.isArray()) return ctx.typeError("array", in);
        if (in.size() > t.arraySize)
            return ctx.error(std::format("array has {} elements, at most {} allowed", in.size(), t.arraySize));
        const TypeInfo& e = t.element();
        const Value blank(e);
        for (usize i = 0; i < t.arraySize; ++i) e.ops->copy(t.ops->element(v, i), blank.data());
        usize i = 0;
        for (JsonValue el : in.elements()) {
            ReadCtx::Scope s(ctx, i);
            HELIOS_TRY(walk::readJson(e, t.ops->element(v, i), el, ctx));
            ++i;
        }
        return {};
    }
    case Kind::Set: {
        if (in.isNull()) { // empty container (Go writes nil slices and maps as null)
            t.ops->clear(v);
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        const TypeInfo& e = t.element();
        t.ops->clear(v);
        usize i = 0;
        for (JsonValue el : in.elements()) {
            ReadCtx::Scope s(ctx, i++);
            Value item(e);
            HELIOS_TRY(walk::readJson(e, item.data(), el, ctx));
            t.ops->findOrInsert(v, item.data());
        }
        return {};
    }
    case Kind::KeyedList: {
        if (in.isNull()) { // empty container (Go writes nil slices and maps as null)
            t.ops->resize(v, 0);
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        const TypeInfo& e = t.element();
        t.ops->resize(v, 0);
        usize i = 0;
        for (JsonValue el : in.elements()) {
            ReadCtx::Scope s(ctx, i);
            HELIOS_TRY_ASSIGN(const Guid key, detail::readKeyedListKey(el, ctx));
            for (usize k = 0; k < i; ++k) {
                if (t.ops->keyAt(v, k) == key) return ctx.error("duplicate $key " + key.toString());
            }
            void* item = t.ops->insertAt(v, i);
            t.ops->setKeyAt(v, i, key);
            HELIOS_TRY(walk::readJson(e, item, el, ctx));
            ++i;
        }
        return {};
    }
    case Kind::Map: {
        if (in.isNull()) { // empty container (Go writes nil slices and maps as null)
            t.ops->clear(v);
            return {};
        }
        if (!in.isObject()) return ctx.typeError("object", in);
        t.ops->clear(v);
        for (const JsonValue::Member m : in.members()) {
            ReadCtx::Scope s(ctx, m.key);
            Value key(t.key());
            if (!t.key().ops->keyFromText(m.key, key.data())) return ctx.error("invalid map key");
            Value value(t.element());
            HELIOS_TRY(walk::readJson(t.element(), value.data(), m.value, ctx));
            t.element().ops->copy(t.ops->findOrInsert(v, key.data()), value.data());
        }
        return {};
    }
    case Kind::Optional:
        if (in.isNull()) {
            t.ops->reset(v);
            return {};
        }
        return walk::readJson(t.element(), t.ops->emplace(v), in, ctx);
    case Kind::Variant: {
        std::string_view name;
        JsonValue body;
        if (in.isString()) {
            name = in.asString();
        } else if (in.isObject() && in.size() == 1) {
            const JsonValue::Member m = *in.members().begin();
            name = m.key;
            body = m.value;
        } else {
            return ctx.typeError("alternative name or {\"Alternative\": {...}}", in);
        }
        const VariantAlt* alt = t.alternative(name);
        if (!alt) return ctx.error("unknown alternative '" + std::string(name) + "' of " + std::string(t.qualifiedName));
        const u32 index = static_cast<u32>(alt - t.alternatives.data());
        void* altPtr = t.ops->index(v) == index ? t.ops->alt(v) : t.ops->emplaceAlt(v, index);
        if (!body.isValid()) return {};
        ReadCtx::Scope s(ctx, name);
        return walk::readJson(alt->type(), altPtr, body, ctx);
    }
    }
    return Error{ErrorCode::InvalidState, "unknown kind"};
}

} // namespace walk

// ---------------------------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------------------------

void writeJson(const TypeInfo& type, const void* obj, JsonWriter& out) {
    if (type.ops && type.ops->writeJson) {
        type.ops->writeJson(out, obj);
    } else {
        walk::writeJson(type, obj, out);
    }
}

Result<void> readJson(const TypeInfo& type, void* obj, JsonValue in, ReadCtx& ctx) {
    if (type.ops && type.ops->readJson) return type.ops->readJson(in, obj, ctx);
    return walk::readJson(type, obj, in, ctx);
}

std::string toJson(const TypeInfo& type, const void* obj, JsonStyle style) {
    JsonWriter w(style);
    writeJson(type, obj, w);
    return w.take();
}

Result<void> fromJson(const TypeInfo& type, void* obj, std::string_view text, ReadCtx& ctx) {
    HELIOS_TRY_ASSIGN(const JsonDocument doc, JsonDocument::parse(text));
    return readJson(type, obj, doc.root(), ctx);
}

void encodeTagged(const TypeInfo& type, const void* obj, std::vector<u8>& out) {
    if (type.ops && type.ops->writeMessage) {
        TaggedWriter w(out);
        type.ops->writeMessage(w, obj);
    } else {
        walk::encodeTagged(type, obj, out);
    }
}

Result<void> decodeTagged(const TypeInfo& type, void* obj, std::span<const u8> bytes) {
    if (type.ops && type.ops->readMessage) {
        TaggedReader r(bytes);
        return type.ops->readMessage(r, obj);
    }
    return walk::decodeTagged(type, obj, bytes);
}

namespace {
constexpr u8 kEnvelopeMagic[4] = {'H', 'T', 'B', '1'};
} // namespace

std::vector<u8> encodeEnvelope(const TypeInfo& type, const void* obj) {
    std::vector<u8> out(kEnvelopeMagic, kEnvelopeMagic + 4);
    TaggedWriter w(out);
    w.writeVarint(type.id);
    encodeTagged(type, obj, out);
    return out;
}

Result<TypeId> peekEnvelopeType(std::span<const u8> bytes) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kEnvelopeMagic, 4) != 0)
        return Error{ErrorCode::VersionMismatch, "not a Helios tagged blob (bad magic)"};
    TaggedReader r(bytes.subspan(4));
    HELIOS_TRY_ASSIGN(const u64 id, r.readVarint());
    if (id > std::numeric_limits<TypeId>::max()) return Error{ErrorCode::Corrupt, "type id out of range"};
    return static_cast<TypeId>(id);
}

Result<void> decodeEnvelope(const TypeInfo& type, void* obj, std::span<const u8> bytes) {
    HELIOS_TRY_ASSIGN(const TypeId id, peekEnvelopeType(bytes));
    if (id != type.id)
        return makeError(ErrorCode::InvalidArgument, "blob holds type id {:#010x}, expected {} ({:#010x})", id,
                         type.qualifiedName, type.id);
    TaggedReader r(bytes.subspan(4));
    HELIOS_TRY(r.readVarint());
    return decodeTagged(type, obj, bytes.subspan(4 + r.position()));
}

bool equals(const TypeInfo& type, const void* a, const void* b) {
    if (type.ops && type.ops->equals) return type.ops->equals(a, b);
    return walk::equals(type, a, b);
}

bool isDefault(const TypeInfo& type, const void* v) {
    if (type.ops && type.ops->isDefault) return type.ops->isDefault(v);
    return walk::isDefault(type, v);
}

bool fieldIsDefault(const FieldInfo& field, const void* fieldPtr) {
    return field.defaultValue ? equals(field.type(), fieldPtr, field.defaultValue) : isDefault(field.type(), fieldPtr);
}

// ---------------------------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------------------------

Value::Value(const TypeInfo& type) : m_type(&type) {
    m_data = ::operator new(type.size == 0 ? 1 : type.size, std::align_val_t(type.align));
    type.ops->construct(m_data);
}

Value::~Value() { release(); }

Value::Value(Value&& o) noexcept : m_type(std::exchange(o.m_type, nullptr)), m_data(std::exchange(o.m_data, nullptr)) {}

Value& Value::operator=(Value&& o) noexcept {
    if (this != &o) {
        release();
        m_type = std::exchange(o.m_type, nullptr);
        m_data = std::exchange(o.m_data, nullptr);
    }
    return *this;
}

void Value::release() noexcept {
    if (m_data) {
        m_type->ops->destruct(m_data);
        ::operator delete(m_data, std::align_val_t(m_type->align));
    }
    m_type = nullptr;
    m_data = nullptr;
}

} // namespace helios::refl
