#pragma once
// Internal helpers shared by path.cpp and patch.cpp.

#include <string_view>

#include "helios/core/result.h"
#include "helios/reflect/path.h"

namespace helios::refl::detail {

/// Parses `json` onto a default value of the type at `path`, then assigns it (creating optionals,
/// map keys and keyed-list elements as needed). A malformed value leaves `object` unchanged.
Result<void> setAtPath(const TypeInfo& type, void* object, const PropertyPath& path, std::string_view json);
/// Removes a map key, a keyed-list element, a list index, or resets an optional.
Result<void> removeAtPath(const TypeInfo& type, void* object, const PropertyPath& path);

} // namespace helios::refl::detail
