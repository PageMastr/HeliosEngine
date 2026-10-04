#pragma once
// Cooked layouts (02 §3.7): how a value of a reflected type is laid out in a `.hrdb` for one cook
// audience. The cooker and the loader both derive the layout from the TypeInfo, so the bytes never
// describe themselves; the layout hash in the database header proves both sides agree.
//
// Fixed parts (natural alignment, fields in schema order, sizes rounded up to the alignment):
//   bool u8 (0/1) · integers and floats as themselves · enum and flags as their underlying integer ·
//   string, Name, LocString, TagQuery: RelSpan<char> (UTF-8) · TagSet: RelSpan<u16> of TagIndex,
//   ascending · HxlExpr: RelSpan<char> source + RelSpan<u8> HXL1 bytecode · Guid, AssetRef: high u64,
//   low u64 · EntityId u64 · NetHandle u32 · Duration i64 ns · Ref<T> u64 RecordId · vec/quat/color/
//   WorldPos as their components · struct: its fields · list, set: RelSpan<element> (sets in canonical
//   key order) · keyed list: RelSpan<element> + RelSpan<Guid> keys · map: RelSpan<{key, value}> in
//   canonical key order · T?: u8 present + T · T[N]: N elements inline · variant: u32 alternative
//   index + the largest alternative inline (unused bytes zero).
//
// Audience filter and AAA-SEC-4 (02 §3.3, §6.5). The client layout drops fields declared in
// `server {}` blocks or @server_only, the server layout drops `client {}` / @client_only fields, and
// both drop `editor {}` fields. A field a layout keeps may not embed a type of the other side (a
// @server_only struct, enum or variant, or a component's ::Server part) by value at all, and may
// reference a record type of the other side only when it is @opaque: the reference then cooks as the
// bare RecordId, and the record itself stays out of the cook. Violations make get() fail with an error
// that names the field and the type.
//
// Threading: a LayoutCache is not thread-safe while it builds layouts; the layouts it returned are
// immutable and safe to read from any thread until the cache is destroyed.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "helios/core/result.h"
#include "helios/records/hrdb_format.h"
#include "helios/reflect/type_info.h"

namespace helios::records {

/// How one cooked value is encoded.
enum class Enc : u8 {
    Bool,
    Int,       ///< Integer, enum or flags: `intKind` gives width and signedness.
    F32,
    F64,
    Text,      ///< RelSpan<char>: string, Name, LocString, TagQuery.
    TagSet,    ///< RelSpan<u16>.
    Hxl,       ///< RelSpan<char> source, RelSpan<u8> bytecode.
    Guid,      ///< Guid or AssetRef (16 bytes).
    EntityId,  ///< u64.
    NetHandle, ///< u32.
    Duration,  ///< i64 nanoseconds.
    RecordRef, ///< u64 RecordId.
    Struct,    ///< Fields (also the math tuples).
    List,      ///< RelSpan<element>.
    KeyedList, ///< RelSpan<element>, RelSpan<Guid>.
    Set,       ///< RelSpan<element>, canonical order.
    Map,       ///< RelSpan<entry>, canonical key order.
    Optional,  ///< u8 present, payload at payloadOffset.
    Array,     ///< `count` elements inline.
    Variant,   ///< u32 index, payload at payloadOffset.
};

/// The cooked layout of one type for one audience. Built by LayoutCache; immutable afterwards.
struct CookedLayout {
    struct Field {
        const refl::FieldInfo* info = nullptr;
        const CookedLayout* layout = nullptr;
        u32 offset = 0; ///< In the cooked fixed part.
    };

    const refl::TypeInfo* type = nullptr;
    Enc enc = Enc::Struct;
    refl::Kind intKind = refl::Kind::U8; ///< Enc::Int: the integer kind (an enum's underlying one).
    u32 size = 0;  ///< Fixed-part bytes (a multiple of align).
    u32 align = 1;
    std::vector<Field> fields;                    ///< Struct: the fields this audience keeps.
    const CookedLayout* element = nullptr;        ///< List, KeyedList, Set, Array, Optional; Map: the value.
    const CookedLayout* key = nullptr;            ///< Map key.
    std::vector<const CookedLayout*> alternatives; ///< Variant, in index order.
    u32 count = 0;          ///< Array length.
    u32 payloadOffset = 0;  ///< Optional and Variant payload; Map: the value's offset in an entry.
    u32 entrySize = 0;      ///< Map: bytes per entry (set when the cache finishes the layout).
    u64 hash = 0;           ///< Structural hash of everything reachable from here (see LayoutCache).

    /// Bytes per element of a List, KeyedList, Set or Array, per entry of a Map.
    u32 stride() const noexcept { return enc == Enc::Map ? entrySize : element->size; }
    /// Field by name, or null when the type has no such field or this audience drops it.
    const Field* field(std::string_view name) const noexcept;
};

/// Is a field kept by this audience's layout (02 §3.3)?
bool keepsField(const refl::FieldInfo& field, CookAudience audience) noexcept;
/// Does this audience exclude the type itself (a server-only type in a client cook, or the reverse)?
bool excludesType(const refl::TypeInfo& type, CookAudience audience) noexcept;

/// Builds and caches cooked layouts for one audience.
class LayoutCache {
public:
    explicit LayoutCache(CookAudience audience);
    ~LayoutCache();
    LayoutCache(const LayoutCache&) = delete;
    LayoutCache& operator=(const LayoutCache&) = delete;

    CookAudience audience() const noexcept { return m_audience; }

    /// The layout of `type` (InvalidArgument for an AAA-SEC-4 violation or a type that contains itself
    /// by value, Unsupported for a type v0 cannot cook). Recursion through a list, set or map is fine
    /// (a span's size does not depend on its element), whichever type of the cycle is asked for first.
    /// The hash of a returned layout covers kind, sizes, offsets, field ids and names, enum values and
    /// alternatives of every layout reachable from it, in an order that does not depend on which type
    /// was asked for first, so a cooker and a loader that build their caches in different orders agree.
    Result<const CookedLayout*> get(const refl::TypeInfo& type);

private:
    /// A list, set or map whose element (and key) layouts are built once the by-value chain closes.
    struct Deferred {
        CookedLayout* layout = nullptr;
        std::string where; ///< The field path that reached it, for errors.
    };

    Result<CookedLayout*> build(const refl::TypeInfo& type);
    Result<void> buildStruct(CookedLayout& l);
    Result<void> buildElements(CookedLayout& l);
    Result<const CookedLayout*> byValue(const refl::TypeInfo& type, std::string_view what);
    std::string where() const;
    void finish();

    CookAudience m_audience;
    std::unordered_map<const refl::TypeInfo*, std::unique_ptr<CookedLayout>> m_layouts;
    std::vector<CookedLayout*> m_pending;    ///< Built, but entry sizes and hashes not computed yet.
    std::vector<const CookedLayout*> m_open; ///< The by-value chain being built (structs, optionals, arrays, variants).
    std::vector<Deferred> m_deferred;        ///< Containers whose elements are not built yet.
    std::vector<std::string> m_where;        ///< Field frames of the current chain ("field 'T.f'").
    std::string m_base;                      ///< The path of the deferred container being built.
};

} // namespace helios::records
