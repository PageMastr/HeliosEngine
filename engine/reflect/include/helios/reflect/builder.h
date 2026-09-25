#pragma once
// Reflection for engine-internal C++ types that are not declared in a schema (02 §3.6
// "HELIOS_REFLECT covers types internal to the engine"). Builder types get lifetime ops from C++
// and use the reflection walker for equality, JSONC and tagged binary, so every generic facility
// (inspector, property paths, diff/patch) works on them.
//
//   // header, at global namespace scope:
//   HELIOS_REFLECT_TYPE(engine::DebugSettings);
//   // .cpp:
//   const helios::refl::TypeInfo& helios::refl::TypeOf<engine::DebugSettings>::get() noexcept {
//       static const TypeInfo* info = StructBuilder<engine::DebugSettings>("engine.DebugSettings")
//                                         .field("drawGrid", &engine::DebugSettings::drawGrid)
//                                         .field("gridSize", &engine::DebugSettings::gridSize, {.id = 5})
//                                         .build();
//       return *info;
//   }
//
// Field ids default to 1, 2, 3... in declaration order; pass explicit ids for data that is
// persisted across versions. T must be default-constructible and copy-assignable.
//
// Threading: build() may be called concurrently for different types (the arena is locked); the
// resulting TypeInfo lives for the rest of the process.

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/reflect/codec.h"
#include "helios/reflect/serialize.h"
#include "helios/reflect/type_info.h"

/// Declares reflection for a builder-reflected struct: TypeOf<T> (define get() with StructBuilder)
/// and Codec<T> (the reflection walker), so T also works inside lists, maps and optionals.
/// Use at global namespace scope.
#define HELIOS_REFLECT_TYPE(...)                                                                    \
    template <>                                                                                     \
    struct helios::refl::TypeOf<__VA_ARGS__> {                                                   \
        static const ::helios::refl::TypeInfo& get() noexcept;                                  \
    };                                                                                              \
    template <>                                                                                     \
    struct helios::refl::Codec<__VA_ARGS__> : ::helios::refl::WalkCodec<__VA_ARGS__> {}

/// Declares TypeOf<E> for an enum reflected with EnumBuilder, or a std::variant reflected with
/// VariantBuilder (their codecs are generic). Use at global namespace scope.
#define HELIOS_REFLECT_ENUM(...)                                                                    \
    template <>                                                                                     \
    struct helios::refl::TypeOf<__VA_ARGS__> {                                                   \
        static const ::helios::refl::TypeInfo& get() noexcept;                                  \
    }
#define HELIOS_REFLECT_VARIANT(...) HELIOS_REFLECT_ENUM(__VA_ARGS__)

namespace helios::refl {

struct BuilderAttr {
    std::string name;
    std::vector<std::pair<std::string, std::string>> args; ///< {key ("" = positional), value}
};

struct FieldOptions {
    u32 id = 0; ///< 0 = previous id + 1.
    FieldFlags flags = FieldFlags::None;
    std::string_view doc;
    std::vector<std::string> was;
    /// For a std::vector of structs: key elements by this field (`@keyed(field)`).
    std::string keyedBy;
    std::vector<BuilderAttr> attrs;
};

namespace detail {
/// Untyped part of the builders; storage lives in a process-lifetime arena.
class TypeBuilderCore {
public:
    TypeBuilderCore(std::string_view qualifiedName, Kind kind, DeclKind decl);
    void addField(std::string_view name, u32 offset, TypeFn type, const FieldOptions& options, const void* defaultValue);
    void addEnumValue(std::string_view name, i64 value);
    void addAlternative(std::string_view name, u32 id, TypeFn type);
    void setDoc(std::string_view doc);
    void addAttr(BuilderAttr attr);
    const TypeInfo* build(u32 size, u32 align, const TypeOps& ops, TypeFn underlying);

private:
    std::string m_name;
    Kind m_kind;
    DeclKind m_decl;
    std::string m_doc;
    struct PendingField {
        std::string name;
        u32 offset;
        TypeFn type;
        FieldOptions options;
        const void* defaultValue;
    };
    std::vector<PendingField> m_fields;
    std::vector<std::pair<std::string, i64>> m_enumValues;
    struct PendingAlt {
        std::string name;
        u32 id;
        TypeFn type;
    };
    std::vector<PendingAlt> m_alternatives;
    std::vector<BuilderAttr> m_attrs;
    u32 m_nextId = 1;
};

template <class T>
const TypeOps& builderOps() noexcept {
    static const TypeOps ops = [] {
        TypeOps o;
        o.construct = &opConstruct<T>;
        o.destruct = &opDestruct<T>;
        o.copy = &opCopy<T>;
        o.equals = [](const void* a, const void* b) { return walk::equals(typeOf<T>(), a, b); };
        o.isDefault = [](const void* v) { return walk::isDefault(typeOf<T>(), v); };
        return o;
    }();
    return ops;
}
} // namespace detail

/// Codec of builder-reflected structs: every operation runs the reflection walker.
template <class S>
struct WalkCodec {
    static constexpr WireType kWire = WireType::Len;
    static constexpr bool kInlineJson = false;
    static bool isDefault(const S& v) noexcept { return walk::isDefault(typeOf<S>(), &v); }
    static bool equals(const S& a, const S& b) noexcept { return walk::equals(typeOf<S>(), &a, &b); }
    static void writeFields(TaggedWriter& w, const S& v) { walk::writeFields(typeOf<S>(), &v, w); }
    static Result<void> readFields(TaggedReader& r, S& v) { return walk::readFields(typeOf<S>(), &v, r); }
    static void writeValue(TaggedWriter& w, const S& v) {
        const usize m = w.beginLen();
        writeFields(w, v);
        w.endLen(m);
    }
    static Result<void> readValue(TaggedReader& r, WireType wire, S& v) {
        if (wire != WireType::Len) return wireTypeMismatch("struct", wire, WireType::Len);
        HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
        return readFields(sub, v);
    }
    static void writeJson(JsonWriter& w, const S& v) { walk::writeJson(typeOf<S>(), &v, w); }
    static void writeJsonFields(JsonWriter& w, const S& v) { walk::writeJsonFields(typeOf<S>(), &v, w); }
    static Result<void> readJson(JsonValue in, S& v, ReadCtx& ctx) { return walk::readJson(typeOf<S>(), &v, in, ctx); }
};

template <class T>
class StructBuilder {
public:
    explicit StructBuilder(std::string_view qualifiedName, DeclKind decl = DeclKind::Struct)
        : m_core(qualifiedName, Kind::Struct, decl) {}

    template <class M>
    StructBuilder& field(std::string_view name, M T::*member, const FieldOptions& options = {}) {
        // The probe holds T's default member initializers, which become the fields' defaults.
        static const T probe{};
        const auto offset = static_cast<u32>(reinterpret_cast<const char*>(&(probe.*member)) -
                                             reinterpret_cast<const char*>(&probe));
        m_core.addField(name, offset, &TypeOf<M>::get, options, &(probe.*member));
        return *this;
    }
    StructBuilder& doc(std::string_view text) {
        m_core.setDoc(text);
        return *this;
    }
    StructBuilder& attr(std::string_view name, std::initializer_list<std::pair<std::string, std::string>> args = {}) {
        m_core.addAttr(BuilderAttr{std::string(name), std::vector<std::pair<std::string, std::string>>(args)});
        return *this;
    }
    /// Returns a process-lifetime TypeInfo (a pointer, so binding it from a temporary builder is
    /// obviously safe).
    const TypeInfo* build() { return m_core.build(sizeof(T), alignof(T), detail::builderOps<T>(), nullptr); }

private:
    detail::TypeBuilderCore m_core;
};

template <class E>
class EnumBuilder {
    static_assert(std::is_enum_v<E>);

public:
    explicit EnumBuilder(std::string_view qualifiedName, bool flags = false)
        : m_core(qualifiedName, flags ? Kind::Flags : Kind::Enum, DeclKind::None) {}
    EnumBuilder& value(std::string_view name, E v) {
        m_core.addEnumValue(name, static_cast<i64>(static_cast<std::underlying_type_t<E>>(v)));
        return *this;
    }
    const TypeInfo* build() {
        return m_core.build(sizeof(E), alignof(E), makeOps<E>(), &TypeOf<std::underlying_type_t<E>>::get);
    }

private:
    detail::TypeBuilderCore m_core;
};

/// Reflects a std::variant of reflected structs; alternatives are given in index order.
template <class V>
class VariantBuilder {
public:
    explicit VariantBuilder(std::string_view qualifiedName) : m_core(qualifiedName, Kind::Variant, DeclKind::None) {}
    /// `id` is the stable tagged field number of the alternative (0 = index + 1).
    template <class Alt>
    VariantBuilder& alternative(std::string_view name, u32 id = 0) {
        m_core.addAlternative(name, id, &TypeOf<Alt>::get);
        return *this;
    }
    const TypeInfo* build() { return m_core.build(sizeof(V), alignof(V), makeOps<V>(), nullptr); }

private:
    detail::TypeBuilderCore m_core;
};

} // namespace helios::refl
