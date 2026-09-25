#pragma once
// Structural diff and patch over reflection (02 §3.6): the basis of editor undo/redo
// transactions, prefab `$overrides` and collaborative editing (07 §1.1).
//
// diff(before, after) emits the smallest set of property-path operations that turns `before`
// into `after`:
//   * structs recurse into fields; variants with the same alternative recurse into it;
//   * optionals recurse when both are engaged, otherwise the whole optional is set (null = reset);
//   * lists/arrays of equal length recurse per index, otherwise the whole list is set;
//   * keyed lists (GUID `$key` or @keyed(field)) remove missing keys, recurse into common keys and
//     append new keys, so edits never depend on indices; a reordering sets the whole list;
//   * maps remove missing keys, set new keys and recurse into common keys;
//   * sets and scalar-like values are set whole.
// apply(before, diff(before, after)) == after for every pair of values (tested).
//
// Threading: stateless; objects must not be mutated concurrently.

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/reflect/path.h"
#include "helios/reflect/type_info.h"

namespace helios::refl {

struct PatchOp {
    enum class Kind : u8 { Set, Remove };
    Kind kind = Kind::Set;
    std::string path;  ///< Canonical property path.
    std::string value; ///< Compact canonical JSON (Set only).
    friend bool operator==(const PatchOp&, const PatchOp&) = default;
};

struct Patch {
    std::vector<PatchOp> ops;

    bool empty() const noexcept { return ops.empty(); }
    /// `[{"op": "set", "path": "...", "value": ...}, {"op": "remove", "path": "..."}]`
    std::string toJson(JsonStyle style = JsonStyle::Pretty) const;
    static Result<Patch> fromJson(std::string_view text);
    friend bool operator==(const Patch&, const Patch&) = default;
};

/// Receives diff operations with typed values (editor transactions can keep them unserialized).
class PatchWriter {
public:
    virtual ~PatchWriter() = default;
    virtual void set(const PropertyPath& path, const TypeInfo& type, const void* value) = 0;
    virtual void remove(const PropertyPath& path) = 0;
};

/// PatchWriter producing a Patch with compact canonical JSON values.
class PatchBuilder final : public PatchWriter {
public:
    void set(const PropertyPath& path, const TypeInfo& type, const void* value) override;
    void remove(const PropertyPath& path) override;
    Patch take() { return std::move(m_patch); }
    const Patch& patch() const noexcept { return m_patch; }

private:
    Patch m_patch;
};

void diff(const TypeInfo& type, const void* before, const void* after, PatchWriter& out);
Patch diff(const TypeInfo& type, const void* before, const void* after);
Result<void> apply(const TypeInfo& type, void* object, const PatchOp& op);
/// Applies ops in order; stops at the first failing op (earlier ops stay applied).
Result<void> apply(const TypeInfo& type, void* object, const Patch& patch);

template <class T>
Patch diff(const T& before, const T& after) {
    return diff(typeOf<T>(), &before, &after);
}
template <class T>
Result<void> apply(T& object, const Patch& patch) {
    return apply(typeOf<T>(), &object, patch);
}

} // namespace helios::refl
