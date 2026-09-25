#pragma once
// Property paths (02 §3.6, 07 §1.1): address a value inside an object by field names, list
// indices, map keys and keyed-list keys, e.g.
//
//   Transform/position                         field / field
//   thrusters[#9a1e0c27d8f14b6e8a55f0c3b1d2e4f6]/maxForce   keyed-list element by $key (hex prefix ok)
//   baseAttrs[Ship.MaxLinearSpeed]             map key (text form of the key)
//   points[3]                                  list / array index
//   components.Health.max                      '.' is accepted as a separator too
//   duration/Periodic/period                   variant alternative by name
//
// Optionals are transparent (a path continues into the contained value). Canonical text uses '/'
// and full 32-digit keys; `\` escapes ']' and '\' inside brackets, a map key starting with '#' is
// written `[\#…]` (so it is not read as a keyed-list key) and the empty map key is `[]`.
//
// Threading: PropertyPath is a value type; resolve() does not synchronize access to the object.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/reflect/type_info.h"

namespace helios::refl {

struct PathSegment {
    enum class Kind : u8 {
        Field, ///< struct field, or variant alternative name
        Index, ///< [3]
        Key,   ///< [text] map key
        Keyed, ///< [#key] keyed-list element (GUID hex, or key-field value for @keyed(field))
    };
    Kind kind = Kind::Field;
    std::string text; ///< Field name / key text (Keyed: without '#').
    u64 index = 0;    ///< Index segments.
    friend bool operator==(const PathSegment&, const PathSegment&) = default;
};

class PropertyPath {
public:
    PropertyPath() = default;
    static Result<PropertyPath> parse(std::string_view text);

    PropertyPath& field(std::string_view name);
    PropertyPath& index(u64 i);
    PropertyPath& key(std::string_view text);
    PropertyPath& keyed(std::string_view keyText);
    /// Copy with one more segment.
    PropertyPath withField(std::string_view name) const { return PropertyPath(*this).field(name); }
    PropertyPath withIndex(u64 i) const { return PropertyPath(*this).index(i); }
    PropertyPath withKey(std::string_view text) const { return PropertyPath(*this).key(text); }
    PropertyPath withKeyed(std::string_view keyText) const { return PropertyPath(*this).keyed(keyText); }

    std::span<const PathSegment> segments() const noexcept { return m_segments; }
    bool empty() const noexcept { return m_segments.empty(); }
    usize size() const noexcept { return m_segments.size(); }
    const PathSegment& back() const noexcept { return m_segments.back(); }
    PropertyPath parent() const;

    /// Canonical text ("a/b[3]/c[#…]").
    std::string toString() const;
    friend bool operator==(const PropertyPath&, const PropertyPath&) = default;

private:
    std::vector<PathSegment> m_segments;
};

/// Canonical key text of a keyed-list GUID key (32 lowercase hex digits).
std::string keyedKeyText(const Guid& key);

struct Ref {
    const TypeInfo* type = nullptr;
    void* ptr = nullptr;
    /// Field whose value this is (for the last Field segment), else null.
    const FieldInfo* field = nullptr;
};
struct ConstRef {
    const TypeInfo* type = nullptr;
    const void* ptr = nullptr;
    const FieldInfo* field = nullptr;
};

enum class ResolveMode : u8 {
    Read,  ///< Every segment must exist.
    Write, ///< Engage optionals, insert missing map keys and keyed-list keys (full keys only),
           ///< switch variant alternatives.
};

Result<Ref> resolve(const TypeInfo& type, void* object, const PropertyPath& path, ResolveMode mode = ResolveMode::Read);
Result<ConstRef> resolve(const TypeInfo& type, const void* object, const PropertyPath& path);

/// Compact JSON of the value at `path`.
Result<std::string> getJson(const TypeInfo& type, const void* object, std::string_view path);
/// Replaces the value at `path` with `json` (parsed onto a default value of the target type).
Result<void> setJson(const TypeInfo& type, void* object, std::string_view path, std::string_view json);

/// Typed access: fails with InvalidArgument if the value at `path` is not a T.
template <class T>
Result<T*> getAs(const TypeInfo& type, void* object, std::string_view path) {
    HELIOS_TRY_ASSIGN(const PropertyPath p, PropertyPath::parse(path));
    HELIOS_TRY_ASSIGN(const Ref r, resolve(type, object, p));
    if (r.type != &typeOf<T>()) {
        return Error{ErrorCode::InvalidArgument,
                     std::string(path) + ": value is " + std::string(r.type->qualifiedName) + ", not " +
                         std::string(typeOf<T>().qualifiedName)};
    }
    return static_cast<T*>(r.ptr);
}

} // namespace helios::refl
