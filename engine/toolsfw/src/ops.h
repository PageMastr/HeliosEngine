#pragma once
// Applying document ops (transaction.h) to reflected objects, and turning a reflection diff into
// exact, invertible ops.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/reflect/path.h"
#include "helios/reflect/serialize.h"
#include "helios/toolsfw/transaction.h"

namespace helios::tf::detail {

/// A resolved list (plain, keyed or @keyed(field)) inside an object.
struct ListRef {
    const refl::TypeInfo* type = nullptr; ///< List or KeyedList type.
    void* ptr = nullptr;
    std::string_view keyField;            ///< @keyed(field) lists: the key field.
    bool keyed() const noexcept;
};

Result<ListRef> resolveList(const refl::TypeInfo& root, void* object, std::string_view listPath);
/// Key text of element `i` ("" for plain lists).
std::string elementKey(const ListRef& list, usize i);
/// Index of the element with key text `key` (keyed lists), or nullopt.
std::optional<usize> findElement(const ListRef& list, std::string_view key);
/// Path of element `i` (`list[#key]` for keyed lists, `list[i]` otherwise).
std::string elementPath(std::string_view listPath, const ListRef& list, usize i);
/// Compact JSON of element `i` (without its `$key`).
std::string elementJson(const ListRef& list, usize i);

/// Current compact JSON at `path`, or nullopt when the path names no value (missing map key or
/// element, empty optional on the way). Other resolution errors are returned.
Result<std::optional<std::string>> valueAt(const refl::TypeInfo& root, const void* object, std::string_view path);

/// Applies a Set/Insert/Remove/Move op to an object, checking its preconditions first (conflicts
/// fail with InvalidState and change nothing).
Result<void> applyValueOp(const refl::TypeInfo& root, void* object, const Op& op);

/// Ops that turn `before` into `after` (both of `type`), using refl::diff plus exact list-element
/// ops, each recorded with the precondition it will check. `doc` is stamped on every op. Applying
/// them in order to a copy of `before` yields an object equal to `after`.
Result<std::vector<Op>> diffOps(const refl::TypeInfo& type, const void* before, const void* after, const DocId& doc);

} // namespace helios::tf::detail
