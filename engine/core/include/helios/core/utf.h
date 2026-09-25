#pragma once
// UTF-8 <-> UTF-16 / UTF-32 / wide conversions and validation.
//
// Engine strings are UTF-8 everywhere; convert at the OS boundary (Win32 W APIs take UTF-16).
// Invalid input never fails: malformed UTF-8 (overlong forms, surrogates, > U+10FFFF, truncated
// sequences) and unpaired UTF-16 surrogates decode to U+FFFD, one replacement per maximal invalid
// subpart (WHATWG / Unicode recommended practice).
//
// Threading: pure functions.

#include <string>
#include <string_view>

#include "helios/core/types.h"

namespace helios {

inline constexpr char32_t kReplacementChar = 0xFFFD;

/// True if `text` is well-formed UTF-8.
bool isValidUtf8(std::string_view text) noexcept;
/// Number of code points (invalid sequences count as one U+FFFD each).
usize utf8Length(std::string_view text) noexcept;

/// Decodes one code point starting at `pos` and advances `pos` (by >= 1). Returns U+FFFD on error.
char32_t decodeUtf8(std::string_view text, usize& pos) noexcept;
/// Appends the UTF-8 encoding of `cp` (invalid code points become U+FFFD).
void appendUtf8(std::string& out, char32_t cp);

std::u16string utf8ToUtf16(std::string_view text);
std::string utf16ToUtf8(std::u16string_view text);
std::u32string utf8ToUtf32(std::string_view text);
std::string utf32ToUtf8(std::u32string_view text);

/// wchar_t is UTF-16 on Windows and UTF-32 elsewhere; these pick the right encoding.
std::wstring utf8ToWide(std::string_view text);
std::string wideToUtf8(std::wstring_view text);

/// Returns a copy with invalid sequences replaced by U+FFFD.
std::string sanitizeUtf8(std::string_view text);

} // namespace helios
