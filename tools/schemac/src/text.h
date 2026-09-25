#pragma once
// Text helpers shared by the schemac passes and generators. The number, duration and GUID
// formats here must match engine/reflect's canonical JSONC writer exactly (schemac cannot link
// reflect); schemac_tests cross-check them against the runtime.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios::schemac {

/// Shortest round-trip text in JavaScript notation ("0.1", "1e+21", "-0.0"); matches
/// helios::refl::formatJsonF64/F32. Non-finite values yield "nan" / "inf" / "-inf".
std::string formatF64(f64 v);
std::string formatF32(f32 v);

/// Numeric attribute argument text as written in a schema ("1e8", "-0.5", "1_000", "0x1F"):
/// finite decimal or hex-integer numbers only (no "inf"/"nan"). False on anything else.
bool parseSchemaNumber(std::string_view text, f64& out);
/// Non-negative integer text ("64", "1_000", "0x40").
bool parseSchemaUnsigned(std::string_view text, u64& out);

/// "30d", "500ms", ... (largest exact unit), matching helios::refl::formatDuration.
std::string formatDuration(i64 nanos);
/// "<number><unit>" with units d/h/m/s/ms/us/ns and optional decimals; nullopt on error.
std::optional<i64> parseDuration(std::string_view text);

/// Canonical lowercase GUID text for (high, low) halves.
std::string formatGuid(u64 high, u64 low);
/// Parses 8-4-4-4-12 or 32-hex-digit GUID text (optionally "guid:" prefixed).
bool parseGuid(std::string_view text, u64& high, u64& low);

/// JSON string literal (quotes included) with the escapes JsonWriter uses.
std::string jsonQuote(std::string_view s);
/// C++ narrow string literal (quotes included), safe for any bytes.
std::string cppQuote(std::string_view s);
/// Go interpreted string literal (quotes included).
std::string goQuote(std::string_view s);

/// "maxForce" -> "MaxForce", "hull_size" -> "HullSize".
std::string pascalCase(std::string_view s);
/// "MaxForce" -> "maxForce".
std::string camelCase(std::string_view s);
bool isPascalCase(std::string_view s) noexcept;
bool isCamelCase(std::string_view s) noexcept;

usize editDistance(std::string_view a, std::string_view b);
/// Closest candidate within a small edit distance, or "".
std::string suggest(std::string_view word, const std::vector<std::string>& candidates);

std::vector<std::string> splitDots(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

/// 64-bit FNV-1a (layout hashes; same function as helios::fnv1a64).
u64 fnv1a64(std::string_view s, u64 seed = 0xcbf29ce484222325ull) noexcept;
u32 fnv1a32(std::string_view s) noexcept;

/// C++ and Go keywords that cannot be used as identifiers in generated code.
bool isCppKeyword(std::string_view s) noexcept;

} // namespace helios::schemac
