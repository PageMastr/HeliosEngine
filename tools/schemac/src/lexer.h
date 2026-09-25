#pragma once
// Tokenizer for .hschema sources (02 §3.1).
//
// Comments: `//` to end of line, `/* ... */`, and `///` doc comments (returned as Doc tokens and
// attached to the following declaration, field or enum value). Newlines are not tokens; each token
// records whether a newline precedes it (fields may end at a line break instead of ';').
// Any other character (including multi-byte UTF-8 such as '±') becomes a one-character Punct
// token so attribute arguments and HXL bodies can be captured verbatim.

#include <string>
#include <string_view>
#include <vector>

#include "diagnostics.h"

namespace helios::schemac {

enum class Tok : u8 { End, Ident, Int, Float, String, Punct, Doc };

struct Token {
    Tok kind = Tok::End;
    std::string_view text; ///< Source slice (String: including quotes; Doc: text after "///").
    SourceLoc loc;
    u32 offset = 0; ///< Byte offset of the first character.
    u32 end = 0;    ///< Byte offset one past the last character.
    bool newlineBefore = false;
    bool spaceBefore = false; ///< Any whitespace/comment directly before the token.

    bool is(Tok k, std::string_view t) const noexcept { return kind == k && text == t; }
    bool isPunct(std::string_view t) const noexcept { return kind == Tok::Punct && text == t; }
    bool isIdent(std::string_view t) const noexcept { return kind == Tok::Ident && text == t; }
};

/// Tokenizes `text`; lexical errors are reported to `diags` and skipped.
std::vector<Token> tokenize(std::string_view text, u32 fileIndex, DiagnosticEngine& diags);

/// Decodes a String token (escapes \" \\ \n \t \r \0 \uXXXX); false on a bad escape.
bool decodeString(std::string_view quoted, std::string& out);

} // namespace helios::schemac
