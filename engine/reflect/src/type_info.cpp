// TypeInfo helpers, attribute lookup, vocabulary type helpers.

#include "helios/reflect/type_info.h"

#include <algorithm>
#include <cstring>

#include "helios/core/guid.h"
#include "helios/reflect/types.h"

namespace helios::refl {

std::string_view kindName(Kind kind) noexcept {
    switch (kind) {
    case Kind::Bool: return "bool";
    case Kind::I8: return "i8";
    case Kind::I16: return "i16";
    case Kind::I32: return "i32";
    case Kind::I64: return "i64";
    case Kind::U8: return "u8";
    case Kind::U16: return "u16";
    case Kind::U32: return "u32";
    case Kind::U64: return "u64";
    case Kind::F32: return "f32";
    case Kind::F64: return "f64";
    case Kind::String: return "string";
    case Kind::Name: return "Name";
    case Kind::Enum: return "enum";
    case Kind::Flags: return "flags";
    case Kind::Struct: return "struct";
    case Kind::Builtin: return "builtin";
    case Kind::List: return "list";
    case Kind::KeyedList: return "keyed list";
    case Kind::Set: return "set";
    case Kind::Map: return "map";
    case Kind::Optional: return "optional";
    case Kind::Array: return "array";
    case Kind::Variant: return "variant";
    }
    return "?";
}

std::string_view Attr::arg(std::string_view key) const noexcept {
    for (const AttrArg& a : args) {
        if (a.key == key) return a.value;
    }
    return {};
}

const Attr* findAttr(AttrSpan attrs, std::string_view name) noexcept {
    for (const Attr& a : attrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

const FieldInfo* TypeInfo::field(std::string_view fieldName) const noexcept {
    for (const FieldInfo& f : fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

const FieldInfo* TypeInfo::fieldOrAlias(std::string_view fieldName) const noexcept {
    if (const FieldInfo* f = field(fieldName)) return f;
    for (const FieldInfo& f : fields) {
        for (const std::string_view w : f.was) {
            if (w == fieldName) return &f;
        }
    }
    return nullptr;
}

const FieldInfo* TypeInfo::fieldById(u32 fieldId) const noexcept {
    for (const FieldInfo& f : fields) {
        if (f.id == fieldId) return &f;
    }
    return nullptr;
}

const EnumValue* TypeInfo::enumByName(std::string_view valueName) const noexcept {
    for (const EnumValue& v : enumValues) {
        if (v.name == valueName) return &v;
    }
    return nullptr;
}

const EnumValue* TypeInfo::enumByValue(i64 value) const noexcept {
    for (const EnumValue& v : enumValues) {
        if (v.value == value) return &v;
    }
    return nullptr;
}

const VariantAlt* TypeInfo::alternative(std::string_view altName) const noexcept {
    for (const VariantAlt& a : alternatives) {
        if (a.name == altName) return &a;
    }
    return nullptr;
}

const VariantAlt* TypeInfo::alternativeById(u32 altId) const noexcept {
    for (const VariantAlt& a : alternatives) {
        if (a.id == altId) return &a;
    }
    return nullptr;
}

namespace {
Kind integerKindOf(const TypeInfo& type) noexcept {
    if (type.kind == Kind::Enum || type.kind == Kind::Flags) {
        if (type.elementFn) return type.element().kind;
        switch (type.size) {
        case 1: return Kind::U8;
        case 2: return Kind::U16;
        case 4: return Kind::U32;
        default: return Kind::U64;
        }
    }
    return type.kind;
}
} // namespace

i64 readIntegerBits(const TypeInfo& type, const void* v) noexcept {
    switch (integerKindOf(type)) {
    case Kind::Bool: return *static_cast<const bool*>(v) ? 1 : 0;
    case Kind::I8: return *static_cast<const i8*>(v);
    case Kind::I16: return *static_cast<const i16*>(v);
    case Kind::I32: return *static_cast<const i32*>(v);
    case Kind::I64: return *static_cast<const i64*>(v);
    case Kind::U8: return *static_cast<const u8*>(v);
    case Kind::U16: return *static_cast<const u16*>(v);
    case Kind::U32: return *static_cast<const u32*>(v);
    case Kind::U64: return static_cast<i64>(*static_cast<const u64*>(v));
    default: return 0;
    }
}

void writeIntegerBits(const TypeInfo& type, void* v, i64 bits) noexcept {
    switch (integerKindOf(type)) {
    case Kind::Bool: *static_cast<bool*>(v) = bits != 0; break;
    case Kind::I8: *static_cast<i8*>(v) = static_cast<i8>(bits); break;
    case Kind::I16: *static_cast<i16*>(v) = static_cast<i16>(bits); break;
    case Kind::I32: *static_cast<i32*>(v) = static_cast<i32>(bits); break;
    case Kind::I64: *static_cast<i64*>(v) = bits; break;
    case Kind::U8: *static_cast<u8*>(v) = static_cast<u8>(bits); break;
    case Kind::U16: *static_cast<u16*>(v) = static_cast<u16>(bits); break;
    case Kind::U32: *static_cast<u32*>(v) = static_cast<u32>(bits); break;
    case Kind::U64: *static_cast<u64*>(v) = static_cast<u64>(bits); break;
    default: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Vocabulary types
// ---------------------------------------------------------------------------------------------

bool TagSet::add(Name tag) {
    if (tag.isNone()) return false;
    const auto it = std::lower_bound(m_tags.begin(), m_tags.end(), tag, &Name::lexicalLess);
    if (it != m_tags.end() && *it == tag) return false;
    m_tags.insert(it, tag);
    return true;
}

bool TagSet::remove(Name tag) {
    const auto it = std::lower_bound(m_tags.begin(), m_tags.end(), tag, &Name::lexicalLess);
    if (it == m_tags.end() || *it != tag) return false;
    m_tags.erase(it);
    return true;
}

bool TagSet::contains(Name tag) const noexcept {
    const auto it = std::lower_bound(m_tags.begin(), m_tags.end(), tag, &Name::lexicalLess);
    return it != m_tags.end() && *it == tag;
}

RecordId mintRecordId() {
    for (;;) {
        u64 bits = 0;
        HELIOS_VERIFY(secureRandomBytes(&bits, sizeof(bits)), "secure random source unavailable");
        const RecordId id = bits & kRecordIdMask;
        if (id != 0) return id;
    }
}

} // namespace helios::refl
