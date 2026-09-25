#pragma once
// Compiled codecs: Codec<T> implements the tagged binary format (tagged.h) and canonical JSONC
// (json.h) for every schema-expressible C++ type — scalars, text, vocabulary types, enums,
// containers and variants here; structs are specialized by helios-schemac generated code (on top
// of StructCodec). typeOf<T>() / makeOps<T>() expose the same codecs to generic code.
//
// Codec<T> interface (all static):
//   kWire, kInlineJson                      wire type of one value; "arrays of T fit on a line"
//   isDefault(v), equals(a, b)              implicit default; bitwise-float value equality
//   writeValue(w, v) / readValue(r, wire, v)  one value (LEN values include their length prefix)
//   writeJson(w, v) / readJson(in, v, ctx)
// Repeated types (vector, KeyedList, array, set, map) instead provide writeRepeated/readRepeated
// (one field's occurrences); use writeField/readField, which dispatch correctly for any T.
//
// Threading: stateless; safe to use concurrently on different objects.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "helios/core/guid.h"
#include "helios/core/hash.h"
#include "helios/core/name.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/math/color.h"
#include "helios/math/quat.h"
#include "helios/math/vec.h"
#include "helios/reflect/json.h"
#include "helios/reflect/tagged.h"
#include "helios/reflect/type_info.h"
#include "helios/reflect/types.h"

namespace helios::refl {

template <class T, class Enable = void>
struct Codec;

// ---------------------------------------------------------------------------------------------
// Traits
// ---------------------------------------------------------------------------------------------
namespace detail {
template <class T>
inline constexpr bool kDependentFalse = false;

template <class T>
struct IsVector : std::false_type {};
template <class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {};
template <class T>
struct IsKeyedList : std::false_type {};
template <class T>
struct IsKeyedList<KeyedList<T>> : std::true_type {};
template <class T>
struct IsArray : std::false_type {};
template <class T, usize N>
struct IsArray<std::array<T, N>> : std::true_type {};
template <class T>
struct IsSet : std::false_type {};
template <class T, class C, class A>
struct IsSet<std::set<T, C, A>> : std::true_type {};
template <class T>
struct IsMap : std::false_type {};
template <class K, class V, class C, class A>
struct IsMap<std::map<K, V, C, A>> : std::true_type {};
template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};
template <class T>
struct IsVariant : std::false_type {};
template <class... A>
struct IsVariant<std::variant<A...>> : std::true_type {};
} // namespace detail

/// Encoded as several field occurrences (or one packed run) instead of a single value.
template <class T>
inline constexpr bool kIsRepeated = detail::IsVector<T>::value || detail::IsKeyedList<T>::value ||
                                    detail::IsArray<T>::value || detail::IsSet<T>::value ||
                                    detail::IsMap<T>::value;
template <class T>
inline constexpr bool kIsOptional = detail::IsOptional<T>::value;

// ---------------------------------------------------------------------------------------------
// Field-level helpers
// ---------------------------------------------------------------------------------------------

/// Writes field `id` holding `v` (repeated types: all occurrences; optionals: only when engaged).
template <class T>
void writeField(TaggedWriter& w, u32 id, const T& v);
/// Reads one occurrence of a field into `v` (appends for repeated types, engages optionals).
template <class T>
Result<void> readField(TaggedReader& r, WireType wire, T& v);

/// How a value is encoded as an element of a repeated field. Lists, maps and optionals cannot be
/// elements directly, so they are wrapped in a message {1: value}.
template <class E>
struct ElementCodec {
    static constexpr bool kWrapped = kIsRepeated<E> || kIsOptional<E>;
    static constexpr WireType kWire = [] {
        if constexpr (kWrapped) {
            return WireType::Len;
        } else {
            return Codec<E>::kWire;
        }
    }();
    static constexpr bool kPackable = kWire != WireType::Len;

    static void writeValue(TaggedWriter& w, const E& e) {
        if constexpr (kWrapped) {
            const usize m = w.beginLen();
            writeField(w, 1, e);
            w.endLen(m);
        } else {
            Codec<E>::writeValue(w, e);
        }
    }
    static Result<void> readValue(TaggedReader& r, WireType wire, E& e) {
        if constexpr (kWrapped) {
            if (wire != WireType::Len) return wireTypeMismatch("wrapped element", wire, WireType::Len);
            HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
            while (!sub.atEnd()) {
                HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
                if (tag.id == 1) {
                    HELIOS_TRY(readField(sub, tag.wire, e));
                } else {
                    HELIOS_TRY(sub.skip(tag.wire));
                }
            }
            return {};
        } else {
            return Codec<E>::readValue(r, wire, e);
        }
    }
};

template <class T>
void writeField(TaggedWriter& w, u32 id, const T& v) {
    if constexpr (kIsRepeated<T>) {
        Codec<T>::writeRepeated(w, id, v);
    } else if constexpr (kIsOptional<T>) {
        if (v.has_value()) writeField(w, id, *v);
    } else {
        w.writeTag(id, Codec<T>::kWire);
        Codec<T>::writeValue(w, v);
    }
}

template <class T>
Result<void> readField(TaggedReader& r, WireType wire, T& v) {
    if constexpr (kIsRepeated<T>) {
        return Codec<T>::readRepeated(r, wire, v);
    } else if constexpr (kIsOptional<T>) {
        if (!v.has_value()) v.emplace();
        return readField(r, wire, *v);
    } else {
        return Codec<T>::readValue(r, wire, v);
    }
}

/// Default test / equality for any codec-supported type (including optionals).
template <class T>
bool isDefaultValue(const T& v) noexcept {
    if constexpr (kIsOptional<T>) {
        return !v.has_value();
    } else {
        return Codec<T>::isDefault(v);
    }
}
template <class T>
bool valuesEqual(const T& a, const T& b) noexcept {
    if constexpr (kIsOptional<T>) {
        if (a.has_value() != b.has_value()) return false;
        return !a.has_value() || Codec<typename T::value_type>::equals(*a, *b);
    } else {
        return Codec<T>::equals(a, b);
    }
}
template <class T>
void writeJsonValue(JsonWriter& w, const T& v) {
    if constexpr (kIsOptional<T>) {
        if (v.has_value()) {
            Codec<typename T::value_type>::writeJson(w, *v);
        } else {
            w.null();
        }
    } else {
        Codec<T>::writeJson(w, v);
    }
}
template <class T>
Result<void> readJsonValue(JsonValue in, T& v, ReadCtx& ctx) {
    if constexpr (kIsOptional<T>) {
        if (in.isNull()) {
            v.reset();
            return {};
        }
        if (!v.has_value()) v.emplace();
        return Codec<typename T::value_type>::readJson(in, *v, ctx);
    } else {
        return Codec<T>::readJson(in, v, ctx);
    }
}
template <class T>
inline constexpr bool kInlineJson = [] {
    if constexpr (kIsOptional<T>) {
        return Codec<typename T::value_type>::kInlineJson;
    } else {
        return Codec<T>::kInlineJson;
    }
}();

// ---------------------------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------------------------

template <>
struct Codec<bool> {
    static constexpr WireType kWire = WireType::Varint;
    static constexpr bool kInlineJson = true;
    static bool isDefault(bool v) noexcept { return !v; }
    static bool equals(bool a, bool b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, bool v) { w.writeVarint(v ? 1 : 0); }
    static Result<void> readValue(TaggedReader& r, WireType wire, bool& v);
    static void writeJson(JsonWriter& w, bool v) { w.boolean(v); }
    static Result<void> readJson(JsonValue in, bool& v, ReadCtx& ctx);
};

namespace detail {
/// Some field of `type` has @was aliases.
bool hasFieldAliases(const TypeInfo& type) noexcept;
/// `key` is an @was alias (not the current name) of a field of `type`.
bool isFieldAlias(const TypeInfo& type, std::string_view key) noexcept;
Result<u64> readVarintField(TaggedReader& r, WireType wire, std::string_view what);
/// LEN payload that must be well-formed UTF-8 (strings, Names, tags); Corrupt otherwise.
Result<std::string_view> readUtf8(TaggedReader& r, std::string_view what);
Error integerOutOfRange(std::string_view what, i64 v);
Error integerOutOfRangeU(std::string_view what, u64 v);
} // namespace detail

template <class T>
struct Codec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static_assert(sizeof(T) <= 8);
    static constexpr WireType kWire = WireType::Varint;
    static constexpr bool kInlineJson = true;
    static bool isDefault(T v) noexcept { return v == 0; }
    static bool equals(T a, T b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, T v) {
        if constexpr (std::is_signed_v<T>) {
            w.writeZigZag(static_cast<i64>(v));
        } else {
            w.writeVarint(static_cast<u64>(v));
        }
    }
    static Result<void> readValue(TaggedReader& r, WireType wire, T& v) {
        HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "integer"));
        if constexpr (std::is_signed_v<T>) {
            const i64 s = zigzagDecode(raw);
            if (s < static_cast<i64>(std::numeric_limits<T>::min()) || s > static_cast<i64>(std::numeric_limits<T>::max()))
                return detail::integerOutOfRange("integer", s);
            v = static_cast<T>(s);
        } else {
            if (raw > static_cast<u64>(std::numeric_limits<T>::max())) return detail::integerOutOfRangeU("integer", raw);
            v = static_cast<T>(raw);
        }
        return {};
    }
    static void writeJson(JsonWriter& w, T v) {
        if constexpr (std::is_signed_v<T>) {
            w.integer(static_cast<i64>(v));
        } else {
            w.unsignedInteger(static_cast<u64>(v));
        }
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if constexpr (std::is_signed_v<T>) {
            i64 s = 0;
            if (!in.getI64(s)) return ctx.typeError("integer", in);
            if (s < static_cast<i64>(std::numeric_limits<T>::min()) || s > static_cast<i64>(std::numeric_limits<T>::max()))
                return ctx.error("integer out of range");
            v = static_cast<T>(s);
        } else {
            u64 u = 0;
            if (!in.getU64(u)) return ctx.typeError("unsigned integer", in);
            if (u > static_cast<u64>(std::numeric_limits<T>::max())) return ctx.error("integer out of range");
            v = static_cast<T>(u);
        }
        return {};
    }
};

template <>
struct Codec<f32> {
    static constexpr WireType kWire = WireType::I32;
    static constexpr bool kInlineJson = true;
    static bool isDefault(f32 v) noexcept { return std::bit_cast<u32>(v) == 0; }
    static bool equals(f32 a, f32 b) noexcept { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
    static void writeValue(TaggedWriter& w, f32 v) { w.writeF32(v); }
    static Result<void> readValue(TaggedReader& r, WireType wire, f32& v);
    static void writeJson(JsonWriter& w, f32 v) { w.numberF32(v); }
    static Result<void> readJson(JsonValue in, f32& v, ReadCtx& ctx);
};

template <>
struct Codec<f64> {
    static constexpr WireType kWire = WireType::I64;
    static constexpr bool kInlineJson = true;
    static bool isDefault(f64 v) noexcept { return std::bit_cast<u64>(v) == 0; }
    static bool equals(f64 a, f64 b) noexcept { return std::bit_cast<u64>(a) == std::bit_cast<u64>(b); }
    static void writeValue(TaggedWriter& w, f64 v) { w.writeF64(v); }
    /// Accepts I32 (f32 -> f64 widening, 02 §3.4) and I64.
    static Result<void> readValue(TaggedReader& r, WireType wire, f64& v);
    static void writeJson(JsonWriter& w, f64 v) { w.number(v); }
    static Result<void> readJson(JsonValue in, f64& v, ReadCtx& ctx);
};

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

template <>
struct Codec<std::string> {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = true;
    static bool isDefault(const std::string& v) noexcept { return v.empty(); }
    static bool equals(const std::string& a, const std::string& b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, const std::string& v) { w.writeString(v); }
    static Result<void> readValue(TaggedReader& r, WireType wire, std::string& v);
    static void writeJson(JsonWriter& w, const std::string& v) { w.string(v); }
    static Result<void> readJson(JsonValue in, std::string& v, ReadCtx& ctx);
};

template <>
struct Codec<Name> {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = true;
    static bool isDefault(Name v) noexcept { return v.isNone(); }
    static bool equals(Name a, Name b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, Name v) { w.writeString(v.view()); }
    static Result<void> readValue(TaggedReader& r, WireType wire, Name& v);
    static void writeJson(JsonWriter& w, Name v) { w.string(v.view()); }
    static Result<void> readJson(JsonValue in, Name& v, ReadCtx& ctx);
};

// ---------------------------------------------------------------------------------------------
// Vocabulary types
// ---------------------------------------------------------------------------------------------

template <>
struct Codec<Guid> {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = true;
    static bool isDefault(const Guid& v) noexcept { return v.isNil(); }
    static bool equals(const Guid& a, const Guid& b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, const Guid& v);
    static Result<void> readValue(TaggedReader& r, WireType wire, Guid& v);
    static void writeJson(JsonWriter& w, const Guid& v);
    /// Canonical text, optionally prefixed with "guid:".
    static Result<void> readJson(JsonValue in, Guid& v, ReadCtx& ctx);
};

namespace detail {
/// Packed float tuples (vectors, quaternions, colours, WorldPos).
template <class F, usize N>
struct FloatTuple {
    static void write(TaggedWriter& w, const F* c) {
        const usize m = w.beginLen();
        for (usize i = 0; i < N; ++i) {
            if constexpr (std::is_same_v<F, f32>) {
                w.writeF32(c[i]);
            } else {
                w.writeF64(c[i]);
            }
        }
        w.endLen(m);
    }
    static Result<void> read(TaggedReader& r, WireType wire, F* c) {
        if (wire != WireType::Len) return wireTypeMismatch("float tuple", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(const auto payload, r.readLen());
        if (payload.size() != N * sizeof(F))
            return makeError(ErrorCode::Corrupt, "float tuple: {} bytes, expected {}", payload.size(), N * sizeof(F));
        for (usize i = 0; i < N; ++i) {
            if constexpr (std::is_same_v<F, f32>) {
                c[i] = std::bit_cast<f32>(loadLE<u32>(payload.data() + i * 4));
            } else {
                c[i] = std::bit_cast<f64>(loadLE<u64>(payload.data() + i * 8));
            }
        }
        return {};
    }
    static void writeJson(JsonWriter& w, const F* c) {
        w.beginArray(true);
        for (usize i = 0; i < N; ++i) {
            if constexpr (std::is_same_v<F, f32>) {
                w.numberF32(c[i]);
            } else {
                w.number(c[i]);
            }
        }
        w.endArray();
    }
    static Result<void> readJson(JsonValue in, F* c, ReadCtx& ctx) {
        if (!in.isArray() || in.size() != N) return ctx.typeError(N == 2 ? "array of 2 numbers" : N == 3 ? "array of 3 numbers" : "array of 4 numbers", in);
        usize i = 0;
        for (JsonValue e : in.elements()) {
            bool ok = false;
            if constexpr (std::is_same_v<F, f32>) {
                ok = e.getF32(c[i]);
            } else {
                ok = e.getF64(c[i]);
            }
            if (!ok) {
                ReadCtx::Scope s(ctx, i);
                return ctx.typeError("number", e);
            }
            ++i;
        }
        return {};
    }
    static bool isZero(const F* c) noexcept {
        for (usize i = 0; i < N; ++i) {
            if (!Codec<F>::isDefault(c[i])) return false;
        }
        return true;
    }
    static bool equals(const F* a, const F* b) noexcept {
        for (usize i = 0; i < N; ++i) {
            if (!Codec<F>::equals(a[i], b[i])) return false;
        }
        return true;
    }
};
} // namespace detail

/// Codec for math tuples laid out as N consecutive components (Vec2/3/4, DVec3, Quat, Color, WorldPos).
template <class T, class F, usize N>
struct TupleCodec {
    static_assert(sizeof(T) == sizeof(F) * N, "tuple layout must be N packed components");
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static const F* comps(const T& v) noexcept { return reinterpret_cast<const F*>(&v); }
    static F* comps(T& v) noexcept { return reinterpret_cast<F*>(&v); }
    static bool isDefault(const T& v) noexcept { return equals(v, T{}); }
    static bool equals(const T& a, const T& b) noexcept { return detail::FloatTuple<F, N>::equals(comps(a), comps(b)); }
    static void writeValue(TaggedWriter& w, const T& v) { detail::FloatTuple<F, N>::write(w, comps(v)); }
    static Result<void> readValue(TaggedReader& r, WireType wire, T& v) {
        return detail::FloatTuple<F, N>::read(r, wire, comps(v));
    }
    static void writeJson(JsonWriter& w, const T& v) { detail::FloatTuple<F, N>::writeJson(w, comps(v)); }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        return detail::FloatTuple<F, N>::readJson(in, comps(v), ctx);
    }
};

template <class F>
struct Codec<TVec2<F>> : TupleCodec<TVec2<F>, F, 2> {};
template <class F>
struct Codec<TVec3<F>> : TupleCodec<TVec3<F>, F, 3> {};
template <class F>
struct Codec<TVec4<F>> : TupleCodec<TVec4<F>, F, 4> {};
template <class F>
struct Codec<TQuat<F>> : TupleCodec<TQuat<F>, F, 4> {};
template <>
struct Codec<Color> : TupleCodec<Color, f32, 4> {};
template <>
struct Codec<WorldPos> : TupleCodec<WorldPos, f64, 3> {};

template <>
struct Codec<EntityId> {
    static constexpr WireType kWire = WireType::I64;
    static constexpr bool kInlineJson = true;
    static bool isDefault(EntityId v) noexcept { return v.value == 0; }
    static bool equals(EntityId a, EntityId b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, EntityId v) { w.writeFixed64(v.value); }
    static Result<void> readValue(TaggedReader& r, WireType wire, EntityId& v);
    /// Canonical JSONC reference form "ent:<decimal id>" (02 §3.7); readers also accept a number.
    static void writeJson(JsonWriter& w, EntityId v);
    static Result<void> readJson(JsonValue in, EntityId& v, ReadCtx& ctx);
};

template <>
struct Codec<NetHandle> {
    static constexpr WireType kWire = WireType::Varint;
    static constexpr bool kInlineJson = true;
    static bool isDefault(NetHandle v) noexcept { return v.value == 0; }
    static bool equals(NetHandle a, NetHandle b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, NetHandle v) { w.writeVarint(v.value); }
    static Result<void> readValue(TaggedReader& r, WireType wire, NetHandle& v);
    static void writeJson(JsonWriter& w, NetHandle v) { w.unsignedInteger(v.value); }
    static Result<void> readJson(JsonValue in, NetHandle& v, ReadCtx& ctx);
};

template <>
struct Codec<Duration> {
    static constexpr WireType kWire = WireType::Varint;
    static constexpr bool kInlineJson = true;
    static bool isDefault(Duration v) noexcept { return v.nanos == 0; }
    static bool equals(Duration a, Duration b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, Duration v) { w.writeZigZag(v.nanos); }
    static Result<void> readValue(TaggedReader& r, WireType wire, Duration& v);
    static void writeJson(JsonWriter& w, Duration v);
    static Result<void> readJson(JsonValue in, Duration& v, ReadCtx& ctx);
};
/// "30d", "500ms", "-1500us", "0s" (largest exact unit among d, h, m, s, ms, us, ns).
std::string formatDuration(Duration d);
/// Parses "<number><unit>" (decimals allowed, e.g. "1.5s") with units d/h/m/s/ms/us/ns.
Result<Duration> parseDuration(std::string_view text);

namespace detail {
Result<u64> readFixed64Field(TaggedReader& r, WireType wire, std::string_view what);
Result<void> readRecordIdJson(JsonValue in, RecordId& v, ReadCtx& ctx);
} // namespace detail

template <class T>
struct Codec<RecordRef<T>> {
    static constexpr WireType kWire = WireType::I64;
    static constexpr bool kInlineJson = true;
    static bool isDefault(RecordRef<T> v) noexcept { return v.id == 0; }
    static bool equals(RecordRef<T> a, RecordRef<T> b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, RecordRef<T> v) { w.writeFixed64(v.id); }
    static Result<void> readValue(TaggedReader& r, WireType wire, RecordRef<T>& v) {
        HELIOS_TRY_ASSIGN(v.id, detail::readFixed64Field(r, wire, "record reference"));
        return {};
    }
    static void writeJson(JsonWriter& w, RecordRef<T> v) { w.unsignedInteger(v.id); }
    static Result<void> readJson(JsonValue in, RecordRef<T>& v, ReadCtx& ctx) {
        return detail::readRecordIdJson(in, v.id, ctx);
    }
};

template <>
struct Codec<AssetRef> {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = true;
    static bool isDefault(const AssetRef& v) noexcept { return v.guid.isNil(); }
    static bool equals(const AssetRef& a, const AssetRef& b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, const AssetRef& v) { Codec<Guid>::writeValue(w, v.guid); }
    static Result<void> readValue(TaggedReader& r, WireType wire, AssetRef& v) {
        return Codec<Guid>::readValue(r, wire, v.guid);
    }
    /// Canonical JSONC reference form "guid:<canonical guid>" (02 §3.7); readers also accept a bare GUID.
    static void writeJson(JsonWriter& w, const AssetRef& v);
    static Result<void> readJson(JsonValue in, AssetRef& v, ReadCtx& ctx) {
        return Codec<Guid>::readJson(in, v.guid, ctx);
    }
};

/// Codec for vocabulary types that are a single std::string (LocString, TagQuery, HxlExpr).
/// `JsonPrefix` (a type with `static constexpr std::string_view kPrefix`) adds the canonical JSONC
/// reference prefix ("loc:" for LocString, 02 §3.7): written in front of the text, optional when
/// reading.
template <class T, std::string T::*Member, class JsonPrefix = void>
struct StringWrapperCodec {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = true;
    static bool isDefault(const T& v) noexcept { return (v.*Member).empty(); }
    static bool equals(const T& a, const T& b) noexcept { return a.*Member == b.*Member; }
    static void writeValue(TaggedWriter& w, const T& v) { w.writeString(v.*Member); }
    static Result<void> readValue(TaggedReader& r, WireType wire, T& v) {
        return Codec<std::string>::readValue(r, wire, v.*Member);
    }
    static void writeJson(JsonWriter& w, const T& v) {
        if constexpr (!std::is_void_v<JsonPrefix>) {
            w.string(std::string(JsonPrefix::kPrefix) + v.*Member);
        } else {
            w.string(v.*Member);
        }
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        HELIOS_TRY(Codec<std::string>::readJson(in, v.*Member, ctx));
        if constexpr (!std::is_void_v<JsonPrefix>) {
            if (std::string_view(v.*Member).starts_with(JsonPrefix::kPrefix)) (v.*Member).erase(0, JsonPrefix::kPrefix.size());
        }
        return {};
    }
};
struct LocStringJsonPrefix {
    static constexpr std::string_view kPrefix = "loc:";
};
template <>
struct Codec<LocString> : StringWrapperCodec<LocString, &LocString::key, LocStringJsonPrefix> {};
template <>
struct Codec<TagQuery> : StringWrapperCodec<TagQuery, &TagQuery::text> {};
template <>
struct Codec<HxlExpr> : StringWrapperCodec<HxlExpr, &HxlExpr::text> {};

template <>
struct Codec<TagSet> {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const TagSet& v) noexcept { return v.empty(); }
    static bool equals(const TagSet& a, const TagSet& b) noexcept { return a == b; }
    /// Message {1: repeated tag name}.
    static void writeValue(TaggedWriter& w, const TagSet& v);
    static Result<void> readValue(TaggedReader& r, WireType wire, TagSet& v);
    static void writeJson(JsonWriter& w, const TagSet& v);
    static Result<void> readJson(JsonValue in, TagSet& v, ReadCtx& ctx);
};

// ---------------------------------------------------------------------------------------------
// Enums and flags (names come from typeOf<E>(), which generated code specializes)
// ---------------------------------------------------------------------------------------------

namespace detail {
void writeEnumJson(JsonWriter& w, const TypeInfo& type, i64 value);
Result<i64> readEnumJson(JsonValue in, const TypeInfo& type, ReadCtx& ctx);
i64 enumDefaultValue(const TypeInfo& type) noexcept;
} // namespace detail

template <class E>
struct Codec<E, std::enable_if_t<std::is_enum_v<E>>> {
    using U = std::underlying_type_t<E>;
    static constexpr WireType kWire = WireType::Varint;
    static constexpr bool kInlineJson = true;
    static i64 bits(E v) noexcept { return static_cast<i64>(static_cast<U>(v)); }
    static bool isDefault(E v) noexcept { return bits(v) == detail::enumDefaultValue(typeOf<E>()); }
    static bool equals(E a, E b) noexcept { return a == b; }
    static void writeValue(TaggedWriter& w, E v) { Codec<U>::writeValue(w, static_cast<U>(v)); }
    static Result<void> readValue(TaggedReader& r, WireType wire, E& v) {
        U u{};
        HELIOS_TRY(Codec<U>::readValue(r, wire, u));
        v = static_cast<E>(u);
        return {};
    }
    static void writeJson(JsonWriter& w, E v) { detail::writeEnumJson(w, typeOf<E>(), bits(v)); }
    static Result<void> readJson(JsonValue in, E& v, ReadCtx& ctx) {
        HELIOS_TRY_ASSIGN(const i64 b, detail::readEnumJson(in, typeOf<E>(), ctx));
        if constexpr (std::is_signed_v<U>) {
            if (b < static_cast<i64>(std::numeric_limits<U>::min()) || b > static_cast<i64>(std::numeric_limits<U>::max()))
                return ctx.error("enum value out of range");
        } else {
            if (static_cast<u64>(b) > static_cast<u64>(std::numeric_limits<U>::max()))
                return ctx.error("enum value out of range");
        }
        v = static_cast<E>(static_cast<U>(b));
        return {};
    }
};

// ---------------------------------------------------------------------------------------------
// Map keys and set elements: canonical order and text form
// ---------------------------------------------------------------------------------------------

/// Canonical order and text form of map keys / set elements; kSupported marks the key types.
template <class K, class Enable = void>
struct KeyTraits {
    static constexpr bool kSupported = false;
};

template <class K>
struct KeyTraits<K, std::enable_if_t<std::is_integral_v<K> && !std::is_same_v<K, bool>>> {
    static constexpr bool kSupported = true;
    static bool less(const K& a, const K& b) noexcept { return a < b; }
    static std::string toText(const K& k) {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof(buf), k);
        return std::string(buf, r.ptr);
    }
    static bool fromText(std::string_view t, K& k) noexcept {
        const auto r = std::from_chars(t.data(), t.data() + t.size(), k);
        return r.ec == std::errc() && r.ptr == t.data() + t.size();
    }
};
template <class E>
struct KeyTraits<E, std::enable_if_t<std::is_enum_v<E>>> {
    static constexpr bool kSupported = true;
    static bool less(const E& a, const E& b) noexcept { return a < b; }
    static std::string toText(const E& k);
    static bool fromText(std::string_view t, E& k) noexcept;
};
template <>
struct KeyTraits<std::string> {
    static constexpr bool kSupported = true;
    static bool less(const std::string& a, const std::string& b) noexcept { return a < b; }
    static std::string toText(const std::string& k) { return k; }
    static bool fromText(std::string_view t, std::string& k) {
        k.assign(t);
        return true;
    }
};
template <>
struct KeyTraits<Name> {
    static constexpr bool kSupported = true;
    static bool less(const Name& a, const Name& b) noexcept { return a.view() < b.view(); }
    static std::string toText(const Name& k) { return std::string(k.view()); }
    static bool fromText(std::string_view t, Name& k) {
        k = Name(t);
        return true;
    }
};
template <>
struct KeyTraits<Guid> {
    static constexpr bool kSupported = true;
    static bool less(const Guid& a, const Guid& b) noexcept { return a < b; }
    static std::string toText(const Guid& k) { return k.toString(); }
    static bool fromText(std::string_view t, Guid& k);
};
template <>
struct KeyTraits<EntityId> {
    static constexpr bool kSupported = true;
    static bool less(const EntityId& a, const EntityId& b) noexcept { return a < b; }
    static std::string toText(const EntityId& k) { return KeyTraits<u64>::toText(k.value); }
    static bool fromText(std::string_view t, EntityId& k) noexcept { return KeyTraits<u64>::fromText(t, k.value); }
};
template <class T>
struct KeyTraits<RecordRef<T>> {
    static constexpr bool kSupported = true;
    static bool less(const RecordRef<T>& a, const RecordRef<T>& b) noexcept { return a < b; }
    static std::string toText(const RecordRef<T>& k) { return KeyTraits<u64>::toText(k.id); }
    static bool fromText(std::string_view t, RecordRef<T>& k) noexcept { return KeyTraits<u64>::fromText(t, k.id); }
};

template <class E>
std::string KeyTraits<E, std::enable_if_t<std::is_enum_v<E>>>::toText(const E& k) {
    const EnumValue* ev = typeOf<E>().enumByValue(static_cast<i64>(static_cast<std::underlying_type_t<E>>(k)));
    if (ev) return std::string(ev->name);
    return KeyTraits<std::underlying_type_t<E>>::toText(static_cast<std::underlying_type_t<E>>(k));
}
template <class E>
bool KeyTraits<E, std::enable_if_t<std::is_enum_v<E>>>::fromText(std::string_view t, E& k) noexcept {
    if (const EnumValue* ev = typeOf<E>().enumByName(t)) {
        k = static_cast<E>(static_cast<std::underlying_type_t<E>>(ev->value));
        return true;
    }
    std::underlying_type_t<E> u{};
    if (!KeyTraits<std::underlying_type_t<E>>::fromText(t, u)) return false;
    k = static_cast<E>(u);
    return true;
}

/// Pointers to the elements of an ordered container, sorted canonically.
template <class K, class It, class Proj>
std::vector<It> sortedByKey(It first, It last, Proj proj) {
    std::vector<It> order;
    for (It it = first; it != last; ++it) order.push_back(it);
    std::sort(order.begin(), order.end(), [&](const It& a, const It& b) { return KeyTraits<K>::less(proj(*a), proj(*b)); });
    return order;
}

// ---------------------------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------------------------

namespace detail {
template <class E, class Seq>
void writeSequence(TaggedWriter& w, u32 id, const Seq& seq) {
    if (seq.empty()) return;
    if constexpr (ElementCodec<E>::kPackable) {
        w.writeTag(id, WireType::Len);
        const usize m = w.beginLen();
        for (const E& e : seq) Codec<E>::writeValue(w, e);
        w.endLen(m);
    } else {
        for (const E& e : seq) {
            w.writeTag(id, ElementCodec<E>::kWire);
            ElementCodec<E>::writeValue(w, e);
        }
    }
}
/// Reads one occurrence (a packed run or a single element) and passes each element to `sink`.
template <class E, class Sink>
Result<void> readSequenceOccurrence(TaggedReader& r, WireType wire, Sink&& sink) {
    if constexpr (ElementCodec<E>::kPackable) {
        if (wire == WireType::Len) {
            HELIOS_TRY_ASSIGN(const auto payload, r.readLen());
            TaggedReader sub(payload, r.depth());
            while (!sub.atEnd()) {
                E e{};
                HELIOS_TRY(Codec<E>::readValue(sub, Codec<E>::kWire, e));
                HELIOS_TRY(sink(std::move(e)));
            }
            return {};
        }
    }
    E e{};
    HELIOS_TRY(ElementCodec<E>::readValue(r, wire, e));
    return sink(std::move(e));
}
template <class E, class Seq>
bool sequenceEquals(const Seq& a, const Seq& b) noexcept {
    if (a.size() != b.size()) return false;
    auto ia = a.begin();
    for (auto ib = b.begin(); ib != b.end(); ++ia, ++ib) {
        if (!valuesEqual<E>(*ia, *ib)) return false;
    }
    return true;
}
template <class E, class Seq>
void writeJsonSequence(JsonWriter& w, const Seq& seq) {
    w.beginArray(kInlineJson<E>);
    for (const E& e : seq) writeJsonValue(w, e);
    w.endArray();
}
} // namespace detail

template <class E, class A>
struct Codec<std::vector<E, A>> {
    static_assert(!std::is_same_v<E, bool>, "list<bool> is not supported (std::vector<bool> has no addressable elements)");
    using T = std::vector<E, A>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const T& v) noexcept { return v.empty(); }
    static bool equals(const T& a, const T& b) noexcept { return detail::sequenceEquals<E>(a, b); }
    static void writeRepeated(TaggedWriter& w, u32 id, const T& v) { detail::writeSequence<E>(w, id, v); }
    static Result<void> readRepeated(TaggedReader& r, WireType wire, T& v) {
        return detail::readSequenceOccurrence<E>(r, wire, [&](E&& e) -> Result<void> {
            v.push_back(std::move(e));
            return {};
        });
    }
    static void writeJson(JsonWriter& w, const T& v) { detail::writeJsonSequence<E>(w, v); }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if (in.isNull()) { // Go encodes empty (nil) slices and maps as null
            v.clear();
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        v.clear();
        v.reserve(in.size());
        usize i = 0;
        for (JsonValue e : in.elements()) {
            ReadCtx::Scope s(ctx, i++);
            E item{};
            HELIOS_TRY(readJsonValue(e, item, ctx));
            v.push_back(std::move(item));
        }
        return {};
    }
};

template <class E, usize N>
struct Codec<std::array<E, N>> {
    using T = std::array<E, N>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const T& v) noexcept {
        for (const E& e : v) {
            if (!isDefaultValue(e)) return false;
        }
        return true;
    }
    static bool equals(const T& a, const T& b) noexcept { return detail::sequenceEquals<E>(a, b); }
    /// Always a single occurrence: packed, or a message {1: element...} (positions are kept).
    static void writeRepeated(TaggedWriter& w, u32 id, const T& v) {
        w.writeTag(id, WireType::Len);
        const usize m = w.beginLen();
        if constexpr (ElementCodec<E>::kPackable) {
            for (const E& e : v) Codec<E>::writeValue(w, e);
        } else {
            for (const E& e : v) {
                w.writeTag(1, ElementCodec<E>::kWire);
                ElementCodec<E>::writeValue(w, e);
            }
        }
        w.endLen(m);
    }
    static Result<void> readRepeated(TaggedReader& r, WireType wire, T& v) {
        if (wire != WireType::Len) return wireTypeMismatch("array", wire, WireType::Len);
        usize count = 0;
        auto sink = [&](E&& e) -> Result<void> {
            if (count >= N) return makeError(ErrorCode::Corrupt, "array: more than {} elements", N);
            v[count++] = std::move(e);
            return {};
        };
        if constexpr (ElementCodec<E>::kPackable) {
            return detail::readSequenceOccurrence<E>(r, wire, sink);
        } else {
            HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
            while (!sub.atEnd()) {
                HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
                if (tag.id != 1) {
                    HELIOS_TRY(sub.skip(tag.wire));
                    continue;
                }
                E e{};
                HELIOS_TRY(ElementCodec<E>::readValue(sub, tag.wire, e));
                HELIOS_TRY(sink(std::move(e)));
            }
            return {};
        }
    }
    static void writeJson(JsonWriter& w, const T& v) { detail::writeJsonSequence<E>(w, v); }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if (!in.isArray()) return ctx.typeError("array", in);
        if (in.size() > N) return ctx.error("array has " + std::to_string(in.size()) + " elements, at most " + std::to_string(N) + " allowed");
        v = T{};
        usize i = 0;
        for (JsonValue e : in.elements()) {
            ReadCtx::Scope s(ctx, i);
            HELIOS_TRY(readJsonValue(e, v[i], ctx));
            ++i;
        }
        return {};
    }
};

template <class E, class C, class A>
struct Codec<std::set<E, C, A>> {
    using T = std::set<E, C, A>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const T& v) noexcept { return v.empty(); }
    static bool equals(const T& a, const T& b) noexcept { return detail::sequenceEquals<E>(a, b); }
    static std::vector<typename T::const_iterator> ordered(const T& v) {
        return sortedByKey<E>(v.begin(), v.end(), [](const E& e) -> const E& { return e; });
    }
    static void writeRepeated(TaggedWriter& w, u32 id, const T& v) {
        if (v.empty()) return;
        const auto order = ordered(v);
        if constexpr (ElementCodec<E>::kPackable) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            for (const auto& it : order) Codec<E>::writeValue(w, *it);
            w.endLen(m);
        } else {
            for (const auto& it : order) {
                w.writeTag(id, ElementCodec<E>::kWire);
                ElementCodec<E>::writeValue(w, *it);
            }
        }
    }
    static Result<void> readRepeated(TaggedReader& r, WireType wire, T& v) {
        return detail::readSequenceOccurrence<E>(r, wire, [&](E&& e) -> Result<void> {
            v.insert(std::move(e));
            return {};
        });
    }
    static void writeJson(JsonWriter& w, const T& v) {
        w.beginArray(Codec<E>::kInlineJson);
        for (const auto& it : ordered(v)) Codec<E>::writeJson(w, *it);
        w.endArray();
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if (in.isNull()) { // Go encodes empty (nil) slices and maps as null
            v.clear();
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        v.clear();
        usize i = 0;
        for (JsonValue e : in.elements()) {
            ReadCtx::Scope s(ctx, i++);
            E item{};
            HELIOS_TRY(Codec<E>::readJson(e, item, ctx));
            v.insert(std::move(item));
        }
        return {};
    }
};

template <class K, class V, class C, class A>
struct Codec<std::map<K, V, C, A>> {
    using T = std::map<K, V, C, A>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const T& v) noexcept { return v.empty(); }
    static bool equals(const T& a, const T& b) noexcept {
        if (a.size() != b.size()) return false;
        for (auto ia = a.begin(), ib = b.begin(); ia != a.end(); ++ia, ++ib) {
            if (!valuesEqual<K>(ia->first, ib->first) || !valuesEqual<V>(ia->second, ib->second)) return false;
        }
        return true;
    }
    static std::vector<typename T::const_iterator> ordered(const T& v) {
        return sortedByKey<K>(v.begin(), v.end(), [](const auto& kv) -> const K& { return kv.first; });
    }
    static void writeRepeated(TaggedWriter& w, u32 id, const T& v) {
        for (const auto& it : ordered(v)) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            writeField(w, 1, it->first);
            writeField(w, 2, it->second);
            w.endLen(m);
        }
    }
    static Result<void> readRepeated(TaggedReader& r, WireType wire, T& v) {
        if (wire != WireType::Len) return wireTypeMismatch("map entry", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        K key{};
        V value{};
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            if (tag.id == 1) {
                HELIOS_TRY(readField(sub, tag.wire, key));
            } else if (tag.id == 2) {
                HELIOS_TRY(readField(sub, tag.wire, value));
            } else {
                HELIOS_TRY(sub.skip(tag.wire));
            }
        }
        v.insert_or_assign(std::move(key), std::move(value));
        return {};
    }
    static void writeJson(JsonWriter& w, const T& v) {
        w.beginObject();
        for (const auto& it : ordered(v)) {
            w.key(KeyTraits<K>::toText(it->first));
            writeJsonValue(w, it->second);
        }
        w.endObject();
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if (in.isNull()) { // Go encodes empty (nil) slices and maps as null
            v.clear();
            return {};
        }
        if (!in.isObject()) return ctx.typeError("object", in);
        v.clear();
        for (const JsonValue::Member m : in.members()) {
            ReadCtx::Scope s(ctx, m.key);
            K key{};
            if (!KeyTraits<K>::fromText(m.key, key)) return ctx.error("invalid map key");
            V value{};
            HELIOS_TRY(readJsonValue(m.value, value, ctx));
            v.insert_or_assign(std::move(key), std::move(value));
        }
        return {};
    }
};

namespace detail {
Result<Guid> readKeyedListKey(JsonValue element, ReadCtx& ctx);
} // namespace detail

/// Keyed lists hold generated structs (schemac enforces it): Codec<T> has writeJsonFields().
template <class E>
struct Codec<KeyedList<E>> {
    using T = KeyedList<E>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const T& v) noexcept { return v.empty(); }
    static bool equals(const T& a, const T& b) noexcept {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            if (a.keyAt(i) != b.keyAt(i) || !Codec<E>::equals(a[i], b[i])) return false;
        }
        return true;
    }
    /// One entry {1: key, 2: value} per element.
    static void writeRepeated(TaggedWriter& w, u32 id, const T& v) {
        for (usize i = 0; i < v.size(); ++i) {
            w.writeTag(id, WireType::Len);
            const usize m = w.beginLen();
            writeField(w, 1, v.keyAt(i));
            writeField(w, 2, v[i]);
            w.endLen(m);
        }
    }
    static Result<void> readRepeated(TaggedReader& r, WireType wire, T& v) {
        if (wire != WireType::Len) return wireTypeMismatch("keyed list entry", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        Guid key;
        E value{};
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            if (tag.id == 1) {
                HELIOS_TRY(readField(sub, tag.wire, key));
            } else if (tag.id == 2) {
                HELIOS_TRY(readField(sub, tag.wire, value));
            } else {
                HELIOS_TRY(sub.skip(tag.wire));
            }
        }
        if (key.isNil()) return Error{ErrorCode::Corrupt, "keyed list entry without key"};
        if (v.indexOf(key)) return Error{ErrorCode::Corrupt, "duplicate keyed list key " + key.toString()};
        v.add(key, std::move(value));
        return {};
    }
    static void writeJson(JsonWriter& w, const T& v) {
        w.beginArray(false);
        for (usize i = 0; i < v.size(); ++i) {
            w.beginObject();
            w.key("$key");
            Codec<Guid>::writeJson(w, v.keyAt(i));
            Codec<E>::writeJsonFields(w, v[i]);
            w.endObject();
        }
        w.endArray();
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        if (in.isNull()) { // Go encodes empty (nil) slices and maps as null
            v.clear();
            return {};
        }
        if (!in.isArray()) return ctx.typeError("array", in);
        v.clear();
        usize i = 0;
        for (JsonValue e : in.elements()) {
            ReadCtx::Scope s(ctx, i++);
            HELIOS_TRY_ASSIGN(const Guid key, detail::readKeyedListKey(e, ctx));
            if (v.indexOf(key)) return ctx.error("duplicate $key " + key.toString());
            E item{};
            HELIOS_TRY(Codec<E>::readJson(e, item, ctx));
            v.add(key, std::move(item));
        }
        return {};
    }
};

// ---------------------------------------------------------------------------------------------
// Variants (alternative names/ids come from typeOf<std::variant<...>>(), generated per schema)
// ---------------------------------------------------------------------------------------------

namespace detail {
template <class V, usize... I>
void emplaceVariantIndex(V& v, usize index, std::index_sequence<I...>) {
    ((index == I ? (void)v.template emplace<I>() : (void)0), ...);
}
Error unknownAlternative(std::string_view variant, std::string_view name);
} // namespace detail

template <class... A>
struct Codec<std::variant<A...>> {
    using T = std::variant<A...>;
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;

    static void emplaceIndex(T& v, usize index) {
        if (v.index() != index) detail::emplaceVariantIndex(v, index, std::index_sequence_for<A...>{});
    }
    static bool isDefault(const T& v) noexcept {
        return v.index() == 0 && Codec<std::variant_alternative_t<0, T>>::isDefault(std::get<0>(v));
    }
    static bool equals(const T& a, const T& b) noexcept {
        if (a.index() != b.index()) return false;
        return std::visit(
            [&](const auto& x) {
                using X = std::remove_cvref_t<decltype(x)>;
                return Codec<X>::equals(x, std::get<X>(b));
            },
            a);
    }
    /// Message with one field: the active alternative's id.
    static void writeValue(TaggedWriter& w, const T& v) {
        const TypeInfo& info = typeOf<T>();
        const usize m = w.beginLen();
        std::visit([&](const auto& x) { writeField(w, info.alternatives[v.index()].id, x); }, v);
        w.endLen(m);
    }
    static Result<void> readValue(TaggedReader& r, WireType wire, T& v) {
        if (wire != WireType::Len) return wireTypeMismatch("variant", wire, WireType::Len);
        const TypeInfo& info = typeOf<T>();
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        while (!sub.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
            const VariantAlt* alt = info.alternativeById(tag.id);
            if (!alt) {
                HELIOS_TRY(sub.skip(tag.wire)); // alternative unknown to this reader version
                continue;
            }
            emplaceIndex(v, static_cast<usize>(alt - info.alternatives.data()));
            Result<void> res;
            std::visit([&](auto& x) { res = readField(sub, tag.wire, x); }, v);
            HELIOS_TRY(res);
        }
        return {};
    }
    /// Unit alternatives: "Name"; others: {"Name": {...}}.
    static void writeJson(JsonWriter& w, const T& v) {
        const TypeInfo& info = typeOf<T>();
        const VariantAlt& alt = info.alternatives[v.index()];
        if (alt.type().fields.empty()) {
            w.string(alt.name);
            return;
        }
        w.beginObject();
        w.key(alt.name);
        std::visit([&](const auto& x) { Codec<std::remove_cvref_t<decltype(x)>>::writeJson(w, x); }, v);
        w.endObject();
    }
    static Result<void> readJson(JsonValue in, T& v, ReadCtx& ctx) {
        const TypeInfo& info = typeOf<T>();
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
        const VariantAlt* alt = info.alternative(name);
        if (!alt) return ctx.error("unknown alternative '" + std::string(name) + "' of " + std::string(info.qualifiedName));
        emplaceIndex(v, static_cast<usize>(alt - info.alternatives.data()));
        if (!body.isValid()) return {};
        ReadCtx::Scope s(ctx, name);
        Result<void> res;
        std::visit([&](auto& x) { res = Codec<std::remove_cvref_t<decltype(x)>>::readJson(body, x, ctx); }, v);
        return res;
    }
};

// ---------------------------------------------------------------------------------------------
// Structs: generated Codec<S> specializations derive from StructCodec<S> and provide
//   isDefault, equals, writeFields, readField(r, tag, v) -> Result<bool> (false = unknown id),
//   writeJsonFields, readJsonMember(key, value, v, ctx) -> Result<bool> (false = unknown key).
// ---------------------------------------------------------------------------------------------

template <class S>
struct StructCodec {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;

    static void writeValue(TaggedWriter& w, const S& v) {
        const usize m = w.beginLen();
        Codec<S>::writeFields(w, v);
        w.endLen(m);
    }
    /// Reads fields from a message body (skips unknown field ids).
    static Result<void> readFields(TaggedReader& r, S& v) {
        while (!r.atEnd()) {
            HELIOS_TRY_ASSIGN(const FieldTag tag, r.readTag());
            HELIOS_TRY_ASSIGN(const bool known, Codec<S>::readField(r, tag, v));
            if (!known) HELIOS_TRY(r.skip(tag.wire));
        }
        return {};
    }
    static Result<void> readValue(TaggedReader& r, WireType wire, S& v) {
        if (wire != WireType::Len) return wireTypeMismatch("struct", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        return readFields(sub, v);
    }
    static void writeJson(JsonWriter& w, const S& v) {
        w.beginObject();
        Codec<S>::writeJsonFields(w, v);
        w.endObject();
    }
    /// Unknown keys warn (or fail in strict mode); "$"-prefixed keys are metadata and ignored.
    /// `@was` aliases are read before current names, so the current name wins when an object has
    /// both (in any order), as in the Go reader.
    static Result<void> readJson(JsonValue in, S& v, ReadCtx& ctx) {
        if (!in.isObject()) return ctx.typeError("object", in);
        const TypeInfo& info = typeOf<S>();
        const bool aliases = detail::hasFieldAliases(info);
        for (int pass = aliases ? 0 : 1; pass < 2; ++pass) {
            for (const JsonValue::Member m : in.members()) {
                if (!m.key.empty() && m.key[0] == '$') continue;
                if (aliases && detail::isFieldAlias(info, m.key) != (pass == 0)) continue;
                ReadCtx::Scope s(ctx, m.key);
                HELIOS_TRY_ASSIGN(const bool known, Codec<S>::readJsonMember(m.key, m.value, v, ctx));
                if (!known) HELIOS_TRY(ctx.unknownField(m.key));
            }
        }
        return {};
    }
};

/// Helper for generated readField: reads field `v` after checking nothing (returns true).
template <class T>
Result<bool> readKnownField(TaggedReader& r, WireType wire, T& v) {
    HELIOS_TRY(readField(r, wire, v));
    return true;
}
/// Helper for generated readJsonMember.
template <class T>
Result<bool> readKnownMember(JsonValue in, T& v, ReadCtx& ctx) {
    HELIOS_TRY(readJsonValue(in, v, ctx));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Typed conveniences
// ---------------------------------------------------------------------------------------------

/// Tagged message bytes of a struct (no envelope).
template <class S>
std::vector<u8> encodeTagged(const S& v) {
    std::vector<u8> out;
    TaggedWriter w(out);
    Codec<S>::writeFields(w, v);
    return out;
}
template <class S>
Result<void> decodeTagged(std::span<const u8> bytes, S& v) {
    TaggedReader r(bytes);
    return Codec<S>::readFields(r, v);
}
template <class S>
Result<S> decodeTagged(std::span<const u8> bytes) {
    S v{};
    HELIOS_TRY(decodeTagged(bytes, v));
    return v;
}
/// Canonical JSONC text of any codec-supported value.
template <class T>
std::string toJson(const T& v, JsonStyle style = JsonStyle::Pretty) {
    JsonWriter w(style);
    writeJsonValue(w, v);
    return w.take();
}
/// Parses JSONC into `v` (which should start at its default value). Warnings go to `ctx`.
template <class T>
Result<void> fromJson(std::string_view text, T& v, ReadCtx& ctx) {
    HELIOS_TRY_ASSIGN(const JsonDocument doc, JsonDocument::parse(text));
    return readJsonValue(doc.root(), v, ctx);
}
template <class T>
Result<T> fromJson(std::string_view text) {
    T v{};
    ReadCtx ctx;
    HELIOS_TRY(fromJson(text, v, ctx));
    return v;
}

// ---------------------------------------------------------------------------------------------
// TypeOps from codecs
// ---------------------------------------------------------------------------------------------

namespace detail {
template <class T>
void opConstruct(void* p) {
    ::new (p) T{};
}
template <class T>
void opDestruct(void* p) {
    static_cast<T*>(p)->~T();
}
template <class T>
void opCopy(void* d, const void* s) {
    *static_cast<T*>(d) = *static_cast<const T*>(s);
}
template <class T>
bool opEquals(const void* a, const void* b) {
    return valuesEqual<T>(*static_cast<const T*>(a), *static_cast<const T*>(b));
}
template <class T>
bool opIsDefault(const void* v) {
    return isDefaultValue<T>(*static_cast<const T*>(v));
}
template <class T>
void opWriteJson(JsonWriter& w, const void* v) {
    writeJsonValue(w, *static_cast<const T*>(v));
}
template <class T>
Result<void> opReadJson(JsonValue in, void* v, ReadCtx& ctx) {
    return readJsonValue(in, *static_cast<T*>(v), ctx);
}
template <class T>
void opWriteTagged(TaggedWriter& w, const void* v) {
    if constexpr (!kIsRepeated<T> && !kIsOptional<T>) Codec<T>::writeValue(w, *static_cast<const T*>(v));
}
template <class T>
Result<void> opReadTagged(TaggedReader& r, WireType wire, void* v) {
    if constexpr (!kIsRepeated<T> && !kIsOptional<T>) {
        return Codec<T>::readValue(r, wire, *static_cast<T*>(v));
    } else {
        return Error{ErrorCode::Unsupported, "value-level tagged read of a repeated/optional type"};
    }
}
} // namespace detail

/// TypeOps for T built from Codec<T> and T's container interface.
template <class T>
const TypeOps& makeOps() noexcept {
    static const TypeOps ops = [] {
        TypeOps o;
        o.construct = &detail::opConstruct<T>;
        o.destruct = &detail::opDestruct<T>;
        o.copy = &detail::opCopy<T>;
        o.equals = &detail::opEquals<T>;
        o.isDefault = &detail::opIsDefault<T>;
        o.writeJson = &detail::opWriteJson<T>;
        o.readJson = &detail::opReadJson<T>;
        if constexpr (!kIsRepeated<T> && !kIsOptional<T>) {
            o.writeTagged = &detail::opWriteTagged<T>;
            o.readTagged = &detail::opReadTagged<T>;
        }
        if constexpr (requires(TaggedWriter& w, TaggedReader& r, T& v) {
                          Codec<T>::writeFields(w, v);
                          Codec<T>::readFields(r, v);
                      }) {
            o.writeMessage = [](TaggedWriter& w, const void* v) { Codec<T>::writeFields(w, *static_cast<const T*>(v)); };
            o.readMessage = [](TaggedReader& r, void* v) -> Result<void> { return Codec<T>::readFields(r, *static_cast<T*>(v)); };
        }
        if constexpr (KeyTraits<T>::kSupported) {
            o.keyLess = [](const void* a, const void* b) -> bool {
                return KeyTraits<T>::less(*static_cast<const T*>(a), *static_cast<const T*>(b));
            };
            o.keyToText = [](const void* k) -> std::string { return KeyTraits<T>::toText(*static_cast<const T*>(k)); };
            o.keyFromText = [](std::string_view t, void* k) -> bool { return KeyTraits<T>::fromText(t, *static_cast<T*>(k)); };
        }
        if constexpr (detail::IsVector<T>::value || detail::IsKeyedList<T>::value) {
            o.size = [](const void* s) -> usize { return static_cast<const T*>(s)->size(); };
            o.element = [](void* s, usize i) -> void* { return &(*static_cast<T*>(s))[i]; };
            o.resize = [](void* s, usize n) { static_cast<T*>(s)->resize(n); };
            o.eraseAt = [](void* s, usize i) {
                T& t = *static_cast<T*>(s);
                if constexpr (detail::IsKeyedList<T>::value) {
                    t.erase(i);
                } else {
                    t.erase(t.begin() + static_cast<isize>(i));
                }
            };
            o.insertAt = [](void* s, usize i) -> void* {
                T& t = *static_cast<T*>(s);
                if constexpr (detail::IsKeyedList<T>::value) {
                    return &t.insert(i, Guid(), typename T::value_type{});
                } else {
                    return &*t.insert(t.begin() + static_cast<isize>(i), typename T::value_type{});
                }
            };
            o.clear = [](void* s) { static_cast<T*>(s)->clear(); };
            if constexpr (detail::IsKeyedList<T>::value) {
                o.keyAt = [](const void* s, usize i) -> Guid { return static_cast<const T*>(s)->keyAt(i); };
                o.setKeyAt = [](void* s, usize i, const Guid& k) { static_cast<T*>(s)->setKey(i, k); };
            }
        } else if constexpr (detail::IsArray<T>::value) {
            o.size = [](const void*) -> usize { return std::tuple_size_v<T>; };
            o.element = [](void* s, usize i) -> void* { return &(*static_cast<T*>(s))[i]; };
        } else if constexpr (detail::IsSet<T>::value) {
            using E = typename T::value_type;
            o.clear = [](void* s) { static_cast<T*>(s)->clear(); };
            o.forEach = [](const void* s, void* user, void (*fn)(void*, const void*, const void*)) {
                for (const auto& it : Codec<T>::ordered(*static_cast<const T*>(s))) fn(user, &*it, nullptr);
            };
            o.findOrInsert = [](void* s, const void* k) -> void* {
                auto res = static_cast<T*>(s)->insert(*static_cast<const E*>(k));
                return const_cast<E*>(&*res.first);
            };
            o.find = [](void* s, const void* k) -> void* {
                auto it = static_cast<T*>(s)->find(*static_cast<const E*>(k));
                return it == static_cast<T*>(s)->end() ? nullptr : const_cast<E*>(&*it);
            };
            o.eraseKey = [](void* s, const void* k) -> bool {
                return static_cast<T*>(s)->erase(*static_cast<const E*>(k)) != 0;
            };
            o.size = [](const void* s) -> usize { return static_cast<const T*>(s)->size(); };
        } else if constexpr (detail::IsMap<T>::value) {
            using K = typename T::key_type;
            o.clear = [](void* s) { static_cast<T*>(s)->clear(); };
            o.forEach = [](const void* s, void* user, void (*fn)(void*, const void*, const void*)) {
                for (const auto& it : Codec<T>::ordered(*static_cast<const T*>(s))) fn(user, &it->first, &it->second);
            };
            o.findOrInsert = [](void* s, const void* k) -> void* { return &(*static_cast<T*>(s))[*static_cast<const K*>(k)]; };
            o.find = [](void* s, const void* k) -> void* {
                auto it = static_cast<T*>(s)->find(*static_cast<const K*>(k));
                return it == static_cast<T*>(s)->end() ? nullptr : &it->second;
            };
            o.eraseKey = [](void* s, const void* k) -> bool {
                return static_cast<T*>(s)->erase(*static_cast<const K*>(k)) != 0;
            };
            o.size = [](const void* s) -> usize { return static_cast<const T*>(s)->size(); };
        } else if constexpr (detail::IsOptional<T>::value) {
            o.has = [](const void* s) -> bool { return static_cast<const T*>(s)->has_value(); };
            o.emplace = [](void* s) -> void* {
                T& t = *static_cast<T*>(s);
                if (!t.has_value()) t.emplace();
                return &*t;
            };
            o.reset = [](void* s) { static_cast<T*>(s)->reset(); };
            o.get = [](void* s) -> void* {
                T& t = *static_cast<T*>(s);
                return t.has_value() ? &*t : nullptr;
            };
        } else if constexpr (detail::IsVariant<T>::value) {
            o.index = [](const void* s) -> u32 { return static_cast<u32>(static_cast<const T*>(s)->index()); };
            o.emplaceAlt = [](void* s, u32 i) -> void* {
                T& t = *static_cast<T*>(s);
                Codec<T>::emplaceIndex(t, i);
                return std::visit([](auto& x) -> void* { return &x; }, t);
            };
            o.alt = [](void* s) -> void* { return std::visit([](auto& x) -> void* { return &x; }, *static_cast<T*>(s)); };
        }
        return o;
    }();
    return ops;
}

// ---------------------------------------------------------------------------------------------
// TypeOf for builtins (defined in codec.cpp) and containers (lazily built here)
// ---------------------------------------------------------------------------------------------

#define HELIOS_REFLECT_DECLARE_BUILTIN_(T)                                                          \
    template <>                                                                                     \
    struct TypeOf<T> {                                                                              \
        static const TypeInfo& get() noexcept;                                                     \
    }
HELIOS_REFLECT_DECLARE_BUILTIN_(bool);
HELIOS_REFLECT_DECLARE_BUILTIN_(i8);
HELIOS_REFLECT_DECLARE_BUILTIN_(i16);
HELIOS_REFLECT_DECLARE_BUILTIN_(i32);
HELIOS_REFLECT_DECLARE_BUILTIN_(i64);
HELIOS_REFLECT_DECLARE_BUILTIN_(u8);
HELIOS_REFLECT_DECLARE_BUILTIN_(u16);
HELIOS_REFLECT_DECLARE_BUILTIN_(u32);
HELIOS_REFLECT_DECLARE_BUILTIN_(u64);
HELIOS_REFLECT_DECLARE_BUILTIN_(f32);
HELIOS_REFLECT_DECLARE_BUILTIN_(f64);
HELIOS_REFLECT_DECLARE_BUILTIN_(std::string);
HELIOS_REFLECT_DECLARE_BUILTIN_(Name);
HELIOS_REFLECT_DECLARE_BUILTIN_(Guid);
HELIOS_REFLECT_DECLARE_BUILTIN_(Vec2);
HELIOS_REFLECT_DECLARE_BUILTIN_(Vec3);
HELIOS_REFLECT_DECLARE_BUILTIN_(Vec4);
HELIOS_REFLECT_DECLARE_BUILTIN_(DVec3);
HELIOS_REFLECT_DECLARE_BUILTIN_(Quat);
HELIOS_REFLECT_DECLARE_BUILTIN_(DQuat);
HELIOS_REFLECT_DECLARE_BUILTIN_(Color);
HELIOS_REFLECT_DECLARE_BUILTIN_(WorldPos);
HELIOS_REFLECT_DECLARE_BUILTIN_(EntityId);
HELIOS_REFLECT_DECLARE_BUILTIN_(NetHandle);
HELIOS_REFLECT_DECLARE_BUILTIN_(Duration);
HELIOS_REFLECT_DECLARE_BUILTIN_(AssetRef);
HELIOS_REFLECT_DECLARE_BUILTIN_(LocString);
HELIOS_REFLECT_DECLARE_BUILTIN_(TagSet);
HELIOS_REFLECT_DECLARE_BUILTIN_(TagQuery);
HELIOS_REFLECT_DECLARE_BUILTIN_(HxlExpr);
#undef HELIOS_REFLECT_DECLARE_BUILTIN_

namespace detail {
/// fnv1a32 of a canonical type name (ids of builtins and containers).
constexpr TypeId typeIdFromName(std::string_view name) noexcept { return fnv1a32(name); }

/// Builds and caches a container TypeInfo. `makeName` runs once, lazily.
template <class T>
TypeInfo makeContainerInfo(Kind kind, std::string_view qualifiedName, TypeFn element, TypeFn key, u32 arraySize) {
    TypeInfo t;
    t.qualifiedName = qualifiedName;
    t.name = qualifiedName;
    t.id = typeIdFromName(qualifiedName);
    t.size = static_cast<u32>(sizeof(T));
    t.align = static_cast<u32>(alignof(T));
    t.kind = kind;
    t.wire = WireType::Len;
    t.elementFn = element;
    t.keyFn = key;
    t.arraySize = arraySize;
    t.ops = &makeOps<T>();
    t.layoutHash = hashCombine(t.id, element ? element().layoutHash : 0);
    return t;
}
} // namespace detail

template <class E, class A>
struct TypeOf<std::vector<E, A>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = "list<" + std::string(typeOf<E>().qualifiedName) + ">";
        static const TypeInfo info =
            detail::makeContainerInfo<std::vector<E, A>>(Kind::List, name, &TypeOf<E>::get, nullptr, 0);
        return info;
    }
};
template <class E>
struct TypeOf<KeyedList<E>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = "list<" + std::string(typeOf<E>().qualifiedName) + "> @keyed";
        static const TypeInfo info = detail::makeContainerInfo<KeyedList<E>>(Kind::KeyedList, name, &TypeOf<E>::get, nullptr, 0);
        return info;
    }
};
template <class E, usize N>
struct TypeOf<std::array<E, N>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = std::string(typeOf<E>().qualifiedName) + "[" + std::to_string(N) + "]";
        static const TypeInfo info =
            detail::makeContainerInfo<std::array<E, N>>(Kind::Array, name, &TypeOf<E>::get, nullptr, static_cast<u32>(N));
        return info;
    }
};
template <class E, class C, class A>
struct TypeOf<std::set<E, C, A>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = "set<" + std::string(typeOf<E>().qualifiedName) + ">";
        static const TypeInfo info =
            detail::makeContainerInfo<std::set<E, C, A>>(Kind::Set, name, &TypeOf<E>::get, nullptr, 0);
        return info;
    }
};
template <class K, class V, class C, class A>
struct TypeOf<std::map<K, V, C, A>> {
    static const TypeInfo& get() noexcept {
        static const std::string name =
            "map<" + std::string(typeOf<K>().qualifiedName) + "," + std::string(typeOf<V>().qualifiedName) + ">";
        static const TypeInfo info =
            detail::makeContainerInfo<std::map<K, V, C, A>>(Kind::Map, name, &TypeOf<V>::get, &TypeOf<K>::get, 0);
        return info;
    }
};
template <class E>
struct TypeOf<std::optional<E>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = std::string(typeOf<E>().qualifiedName) + "?";
        static const TypeInfo info =
            detail::makeContainerInfo<std::optional<E>>(Kind::Optional, name, &TypeOf<E>::get, nullptr, 0);
        return info;
    }
};
template <class T>
struct TypeOf<RecordRef<T>> {
    static const TypeInfo& get() noexcept {
        static const std::string name = "Ref<" + std::string(typeOf<T>().qualifiedName) + ">";
        static const TypeInfo info = [] {
            TypeInfo t;
            t.qualifiedName = name;
            t.name = name;
            t.id = detail::typeIdFromName(name);
            t.size = sizeof(RecordRef<T>);
            t.align = alignof(RecordRef<T>);
            t.kind = Kind::Builtin;
            t.wire = WireType::I64;
            t.flags = TypeFlags::InlineJson;
            t.elementFn = &TypeOf<T>::get;
            t.ops = &makeOps<RecordRef<T>>();
            t.layoutHash = t.id;
            return t;
        }();
        return info;
    }
};

} // namespace helios::refl
