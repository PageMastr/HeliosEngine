#pragma once
// Type-erased serialization of objects known only by TypeInfo (editor, tools, patches, hot
// reload). Each entry point uses the type's compiled codec (TypeOps) when it has one and otherwise
// the reflection walker, which produces identical bytes/text from FieldInfo + container ops.
// `walk::` always uses the walker (engine types registered with StructBuilder have no compiled
// codec; tests cross-check both paths on generated types).
//
// Threading: stateless; concurrent use on different objects is safe.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/reflect/tagged.h"
#include "helios/reflect/type_info.h"

namespace helios::refl {

/// Writes `obj` (of `type`) as JSON.
void writeJson(const TypeInfo& type, const void* obj, JsonWriter& out);
/// Reads JSON into `obj`; object members not present keep their current values, so start from a
/// default-constructed object to get "missing = default" semantics.
Result<void> readJson(const TypeInfo& type, void* obj, JsonValue in, ReadCtx& ctx);
std::string toJson(const TypeInfo& type, const void* obj, JsonStyle style = JsonStyle::Pretty);
Result<void> fromJson(const TypeInfo& type, void* obj, std::string_view text, ReadCtx& ctx);

/// Tagged message body of a struct-kind type (no envelope).
void encodeTagged(const TypeInfo& type, const void* obj, std::vector<u8>& out);
Result<void> decodeTagged(const TypeInfo& type, void* obj, std::span<const u8> bytes);

/// Versioned, self-describing blob: magic "HTB1", varint type id, then the tagged message.
/// decodeEnvelope() rejects other magics and other type ids (VersionMismatch / Corrupt).
std::vector<u8> encodeEnvelope(const TypeInfo& type, const void* obj);
Result<void> decodeEnvelope(const TypeInfo& type, void* obj, std::span<const u8> bytes);
/// Type id stored in an envelope (to pick the TypeInfo from a registry before decoding).
Result<TypeId> peekEnvelopeType(std::span<const u8> bytes);

/// Value equality (bitwise floats) and implicit-default test for any type.
bool equals(const TypeInfo& type, const void* a, const void* b);
bool isDefault(const TypeInfo& type, const void* v);
/// A struct field holds its schema default (explicit default value, else the type's default).
bool fieldIsDefault(const FieldInfo& field, const void* fieldPtr);

namespace walk {
void writeJson(const TypeInfo& type, const void* obj, JsonWriter& out);
/// Object members only (no braces), used for keyed-list elements.
void writeJsonFields(const TypeInfo& type, const void* obj, JsonWriter& out);
Result<void> readJson(const TypeInfo& type, void* obj, JsonValue in, ReadCtx& ctx);
void encodeTagged(const TypeInfo& type, const void* obj, std::vector<u8>& out);
Result<void> decodeTagged(const TypeInfo& type, void* obj, std::span<const u8> bytes);
/// Message body of a struct (fields only) to/from an existing writer/reader (keeps nesting depth).
void writeFields(const TypeInfo& type, const void* obj, TaggedWriter& out);
Result<void> readFields(const TypeInfo& type, void* obj, TaggedReader& in);
bool equals(const TypeInfo& type, const void* a, const void* b);
bool isDefault(const TypeInfo& type, const void* v);
} // namespace walk

/// Owning, type-erased value: storage constructed/destroyed with the type's ops.
class Value {
public:
    Value() noexcept = default;
    explicit Value(const TypeInfo& type);
    ~Value();
    Value(Value&& o) noexcept;
    Value& operator=(Value&& o) noexcept;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    const TypeInfo* type() const noexcept { return m_type; }
    void* data() noexcept { return m_data; }
    const void* data() const noexcept { return m_data; }
    explicit operator bool() const noexcept { return m_type != nullptr; }

private:
    void release() noexcept;
    const TypeInfo* m_type = nullptr;
    void* m_data = nullptr;
};

} // namespace helios::refl
