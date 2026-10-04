#pragma once
// Small JSON helpers shared by ToolsFramework, the editor UI, helios-tool and helios-uitest. They
// build on refl::JsonDocument (yyjson, JSONC input) and refl::JsonWriter (canonical output).
//
// Threading: pure functions.

#include <optional>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/reflect/json.h"

namespace helios::tf::json {

/// `"text"` with JSON escapes.
std::string quote(std::string_view text);
/// Compact canonical text of a parsed value (numbers keep their source text).
std::string compact(refl::JsonValue value);
/// Re-renders JSON text compactly (validates it). ParseError on bad input.
Result<std::string> normalize(std::string_view text);

/// Member accessors of an object value (nullopt when absent or of another type).
std::optional<std::string_view> getString(refl::JsonValue object, std::string_view key);
std::optional<f64> getNumber(refl::JsonValue object, std::string_view key);
std::optional<i64> getInteger(refl::JsonValue object, std::string_view key);
std::optional<bool> getBool(refl::JsonValue object, std::string_view key);

/// Parses text that must hold an object ("" = empty object). InvalidArgument otherwise.
Result<refl::JsonDocument> parseObject(std::string_view text, std::string_view sourceName = "<json>");

} // namespace helios::tf::json
