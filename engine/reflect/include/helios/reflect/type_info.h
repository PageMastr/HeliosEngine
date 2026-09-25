#pragma once
// Runtime type descriptions (02 §3.6). Every schema type (and every builtin/container it uses)
// has one immutable TypeInfo: name, stable lock id, size/alignment, kind, fields with stable
// field ids and byte offsets, attributes, and TypeOps (lifetime, equality, compiled codecs and
// container navigation) so generic code — JSONC, tagged binary, property paths, diff/patch, the
// editor inspector — can operate on objects it knows only by TypeInfo.
//
// Type references are function pointers (TypeFn) rather than TypeInfo pointers so static tables
// can reference each other (and themselves, e.g. `list<Node>` inside Node) without static
// initialization order problems. typeOf<T>() returns the TypeInfo of a compiled type.
//
// Threading: TypeInfos are immutable after construction; everything here is safe to read
// concurrently.

#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "helios/core/guid.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/reflect/tagged.h"

namespace helios::refl {

class JsonWriter;
class JsonValue;
class ReadCtx;
struct TypeInfo;

/// Stable 32-bit type id: minted once by helios-schemac and stored in the schema lock (02 §3.4);
/// builtins and containers use fnv1a32 of their canonical name.
using TypeId = u32;
using TypeFn = const TypeInfo& (*)() noexcept;

enum class Kind : u8 {
    Bool,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    String,    ///< std::string
    Name,      ///< helios::Name
    Enum,      ///< enum class; enumValues
    Flags,     ///< bit-flag enum class; enumValues are the single bits
    Struct,    ///< fields
    Builtin,   ///< vocabulary type (Guid, vec3f, EntityId, ...): compiled ops, optional fields
    List,      ///< std::vector<T>
    KeyedList, ///< KeyedList<T>
    Set,       ///< std::set<T>
    Map,       ///< std::map<K, V>
    Optional,  ///< std::optional<T>
    Array,     ///< std::array<T, N>
    Variant,   ///< std::variant<Alt...>; alternatives
};

std::string_view kindName(Kind kind) noexcept;
constexpr bool isIntegerKind(Kind k) noexcept { return k >= Kind::I8 && k <= Kind::U64; }
constexpr bool isSignedKind(Kind k) noexcept { return k >= Kind::I8 && k <= Kind::I64; }
constexpr bool isFloatKind(Kind k) noexcept { return k == Kind::F32 || k == Kind::F64; }
constexpr bool isScalarKind(Kind k) noexcept { return k <= Kind::F64; }
constexpr bool isSequenceKind(Kind k) noexcept {
    return k == Kind::List || k == Kind::KeyedList || k == Kind::Array;
}
/// Kinds encoded as several field occurrences (or packed) rather than one value.
constexpr bool isRepeatedKind(Kind k) noexcept {
    return k == Kind::List || k == Kind::KeyedList || k == Kind::Array || k == Kind::Set || k == Kind::Map;
}

/// Schema declaration a struct-like type came from.
enum class DeclKind : u8 { None, Struct, Component, Record, Event, Message, ViewModel, Relation, Rpc, Alternative };

enum class TypeFlags : u32 {
    None = 0,
    Tuple = 1u << 0,       ///< Builtin math tuple: JSON array of its fields, tagged as packed floats.
    Replicated = 1u << 1,  ///< Component with a `_dirty` field mask (02 §4.4).
    ServerPart = 1u << 2,  ///< Generated `X::Server` part of a component (never replicated).
    ClientPart = 1u << 3,  ///< Generated `X::Client` part of a component.
    ServerOnly = 1u << 4,  ///< @server_only: must never reach a client cook.
    ClientOnly = 1u << 5,  ///< @client_only.
    Authoring = 1u << 6,   ///< @authoring: source/editor worlds only.
    InlineJson = 1u << 7,  ///< Arrays of this type are written on one line.
    Unit = 1u << 8,        ///< Variant alternative / struct without fields.
};
HELIOS_ENUM_FLAGS(TypeFlags)

enum class FieldFlags : u32 {
    None = 0,
    ServerOnly = 1u << 0, ///< Declared in a `server {}` block or @server_only.
    ClientOnly = 1u << 1, ///< Declared in a `client {}` block or @client_only.
    EditorOnly = 1u << 2, ///< Declared in an `editor {}` block.
    Replicated = 1u << 3, ///< Has a dirty bit (repIndex).
    Predicted = 1u << 4,  ///< @predicted: joins the rollback snapshot.
    Keyed = 1u << 5,      ///< @keyed list.
    Hidden = 1u << 6,     ///< @hidden in the inspector.
    ReadOnly = 1u << 7,   ///< @readonly in the inspector.
    Opaque = 1u << 8,     ///< @opaque reference to server-only data.
    Deprecated = 1u << 9, ///< @deprecated.
};
HELIOS_ENUM_FLAGS(FieldFlags)

/// Replication audience (04 §4.1).
enum class Audience : u8 { None, All, Owner, Server };

/// One attribute argument as written in the schema: `category="Thrust"` -> {key "category",
/// value "Thrust"}; `0` -> {"", "0"}; `1/256m` -> {"", "1/256m"} (units are kept verbatim).
struct AttrArg {
    std::string_view key;
    std::string_view value;
};

/// A schema attribute (`@range(0, 1e8)`), with an optional typed payload for well-known ones.
struct Attr {
    std::string_view name;
    std::span<const AttrArg> args;
    const void* typed = nullptr;
    u32 typedKind = 0;

    /// Positional argument i ("" if absent).
    std::string_view arg(usize i) const noexcept { return i < args.size() ? args[i].value : std::string_view(); }
    /// Named argument ("" if absent).
    std::string_view arg(std::string_view key) const noexcept;
};
using AttrSpan = std::span<const Attr>;

const Attr* findAttr(AttrSpan attrs, std::string_view name) noexcept;

/// Typed payloads of well-known attributes (`TypeInfo::attr<attrs::Range>()`).
namespace attrs {
struct Doc {
    static constexpr u32 kKind = 1;
    std::string_view text;
};
struct Range {
    static constexpr u32 kKind = 2;
    f64 min = 0;
    f64 max = 0;
};
struct Unit {
    static constexpr u32 kKind = 3;
    std::string_view unit;
};
struct Editor {
    static constexpr u32 kKind = 4;
    std::string_view category;
    std::string_view widget;
    i32 order = 0;
};
struct Keyed {
    static constexpr u32 kKind = 5;
    std::string_view field; ///< Empty: elements carry "$key" GUIDs.
};
struct Max {
    static constexpr u32 kKind = 6;
    u64 count = 0;
};
struct Step {
    static constexpr u32 kKind = 7;
    f64 step = 0;
};
struct Table {
    static constexpr u32 kKind = 8;
    std::string_view name;
};
struct Replicate {
    static constexpr u32 kKind = 9;
    Audience audience = Audience::None;
};
struct Asset {
    static constexpr u32 kKind = 10;
    std::string_view kind; ///< AssetRef<Kind> / @asset(Kind)
};
} // namespace attrs

template <class A>
const A* findTypedAttr(AttrSpan attrs) noexcept {
    for (const Attr& a : attrs) {
        if (a.typedKind == A::kKind && a.typed) return static_cast<const A*>(a.typed);
    }
    return nullptr;
}

struct EnumValue {
    std::string_view name;
    i64 value = 0;
    std::string_view doc;
};

struct VariantAlt {
    std::string_view name; ///< Short name ("Periodic").
    u32 id = 0;            ///< Stable lock id (tagged field number of the alternative).
    TypeFn type = nullptr; ///< Alternative struct type.
};

struct FieldInfo {
    std::string_view name;
    u32 id = 0;     ///< Stable lock id = tagged field number.
    u32 offset = 0; ///< Byte offset in the owning struct.
    TypeFn typeFn = nullptr;
    FieldFlags flags = FieldFlags::None;
    Audience audience = Audience::None;
    u8 repIndex = 0xFF;                   ///< Dirty bit (Replicated fields), else 0xFF.
    std::span<const std::string_view> was; ///< @was aliases accepted by readers.
    const void* defaultValue = nullptr;   ///< Explicit schema default (object of the field type), or null.
    std::string_view defaultJson;         ///< Canonical JSON of the explicit default ("" if implicit).
    AttrSpan attrs;
    std::string_view doc;

    const TypeInfo& type() const noexcept { return typeFn(); }
    void* ptr(void* object) const noexcept { return static_cast<u8*>(object) + offset; }
    const void* ptr(const void* object) const noexcept { return static_cast<const u8*>(object) + offset; }
    const Attr* attr(std::string_view attrName) const noexcept { return findAttr(attrs, attrName); }
    template <class A>
    const A* attr() const noexcept {
        return findTypedAttr<A>(attrs);
    }
};

/// Operations on objects of one type. Lifetime ops and equals/isDefault exist for every type;
/// codec hooks exist for compiled types (null for builder types, which use the generic walker);
/// container hooks exist for the matching Kind only.
struct TypeOps {
    void (*construct)(void* dst) = nullptr; ///< Placement-default-construct.
    void (*destruct)(void* obj) = nullptr;
    void (*copy)(void* dst, const void* src) = nullptr; ///< Copy-assign into a constructed dst.
    /// Value equality with bitwise float comparison (NaN == NaN, -0 != +0), i.e. "serializes the same".
    bool (*equals)(const void* a, const void* b) = nullptr;
    /// Equals the type's implicit default (T{} for structs: every field at its schema default).
    bool (*isDefault)(const void* v) = nullptr;

    // Compiled codecs (value level: LEN values include their length prefix).
    void (*writeJson)(JsonWriter& out, const void* v) = nullptr;
    Result<void> (*readJson)(JsonValue in, void* v, ReadCtx& ctx) = nullptr;
    void (*writeTagged)(TaggedWriter& out, const void* v) = nullptr;
    Result<void> (*readTagged)(TaggedReader& in, WireType wire, void* v) = nullptr;
    /// Struct message body (no length prefix) — compiled struct types only.
    void (*writeMessage)(TaggedWriter& out, const void* v) = nullptr;
    Result<void> (*readMessage)(TaggedReader& in, void* v) = nullptr;

    // Sequences (List, KeyedList, Array).
    usize (*size)(const void* seq) = nullptr;
    void* (*element)(void* seq, usize index) = nullptr;
    void (*resize)(void* seq, usize n) = nullptr;                 ///< List/KeyedList
    void* (*insertAt)(void* seq, usize index) = nullptr;          ///< default element; List/KeyedList
    void (*eraseAt)(void* seq, usize index) = nullptr;            ///< List/KeyedList
    Guid (*keyAt)(const void* seq, usize index) = nullptr;         ///< KeyedList
    void (*setKeyAt)(void* seq, usize index, const Guid& key) = nullptr; ///< KeyedList

    // Sets and maps (Set: key = element, value = null).
    void (*clear)(void* container) = nullptr;
    void (*forEach)(const void* container, void* user, void (*fn)(void* user, const void* key, const void* value)) =
        nullptr;
    /// Map: value for key, inserting a default value if absent. Set: inserts key, returns non-null.
    void* (*findOrInsert)(void* container, const void* key) = nullptr;
    /// Map: value pointer or null. Set: non-null iff present.
    void* (*find)(void* container, const void* key) = nullptr;
    bool (*eraseKey)(void* container, const void* key) = nullptr;

    // Optional.
    bool (*has)(const void* opt) = nullptr;
    void* (*emplace)(void* opt) = nullptr; ///< Engages with a default value (or keeps the current one).
    void (*reset)(void* opt) = nullptr;
    void* (*get)(void* opt) = nullptr;     ///< Null if disengaged.

    // Map keys and set elements: canonical order (keyLess) and text form (JSON object keys,
    // property-path segments). Present for integer, enum, string, Name, Guid, EntityId, RecordRef.
    bool (*keyLess)(const void* a, const void* b) = nullptr;
    std::string (*keyToText)(const void* key) = nullptr;
    bool (*keyFromText)(std::string_view text, void* key) = nullptr;

    // Variant.
    u32 (*index)(const void* var) = nullptr;
    void* (*emplaceAlt)(void* var, u32 index) = nullptr; ///< Switches alternative (default value).
    void* (*alt)(void* var) = nullptr;                    ///< Active alternative.
};

struct TypeInfo {
    std::string_view qualifiedName; ///< "sample.ship.ThrusterMount", "list<f32>", "vec3f"
    std::string_view name;          ///< Last component ("ThrusterMount").
    TypeId id = 0;
    u32 size = 0;
    u32 align = 1;
    Kind kind = Kind::Struct;
    WireType wire = WireType::Len;
    DeclKind decl = DeclKind::None;
    TypeFlags flags = TypeFlags::None;
    u32 version = 0;    ///< @version(n)
    /// Hash of field ids, names and types, including every type reachable through the fields
    /// (cooked-data DDC key: a change in a nested type changes it).
    u64 layoutHash = 0;
    std::span<const FieldInfo> fields;
    std::span<const EnumValue> enumValues;
    std::span<const VariantAlt> alternatives;
    TypeFn elementFn = nullptr; ///< List/KeyedList/Set/Optional/Array element, Map value.
    TypeFn keyFn = nullptr;     ///< Map key.
    u32 arraySize = 0;          ///< Array N.
    const TypeOps* ops = nullptr;
    AttrSpan attrs;
    std::string_view doc;

    const TypeInfo& element() const noexcept { return elementFn(); }
    const TypeInfo& key() const noexcept { return keyFn(); }
    bool hasFlag(TypeFlags f) const noexcept { return ::helios::refl::hasFlag(flags, f); }

    const FieldInfo* field(std::string_view fieldName) const noexcept;
    /// Field by name or @was alias.
    const FieldInfo* fieldOrAlias(std::string_view fieldName) const noexcept;
    const FieldInfo* fieldById(u32 fieldId) const noexcept;
    const EnumValue* enumByName(std::string_view valueName) const noexcept;
    const EnumValue* enumByValue(i64 value) const noexcept;
    const VariantAlt* alternative(std::string_view altName) const noexcept;
    const VariantAlt* alternativeById(u32 altId) const noexcept;

    const Attr* attr(std::string_view attrName) const noexcept { return findAttr(attrs, attrName); }
    template <class A>
    const A* attr() const noexcept {
        return findTypedAttr<A>(attrs);
    }
};

/// Specialized for every reflectable C++ type: `static const TypeInfo& get() noexcept;`.
template <class T>
struct TypeOf;

template <class T>
const TypeInfo& typeOf() noexcept {
    return TypeOf<std::remove_cvref_t<T>>::get();
}

/// Reads an integer-kind value (Bool, I8..U64, Enum, Flags) of `type` as i64 / u64 bits.
i64 readIntegerBits(const TypeInfo& type, const void* v) noexcept;
void writeIntegerBits(const TypeInfo& type, void* v, i64 bits) noexcept;

/// Canonical ordering of map keys / set elements (integers numerically, enums by value, text
/// byte-wise, GUIDs by bytes). Both the C++ and the Go codecs emit maps and sets in this order.
inline bool keyLess(const TypeInfo& keyType, const void* a, const void* b) noexcept {
    return keyType.ops->keyLess(a, b);
}

} // namespace helios::refl
