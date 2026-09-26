#pragma once
// HXL front end (private): tokens, lexer, AST and parser. Mirrors services/pkg/hxl/syntax.go
// function by function; any change here must be made there too (the corpus checks error codes and
// positions, not only results).

#include <string>
#include <string_view>
#include <vector>

#include "helios/hxl/program.h"

namespace helios::hxl::detail {

enum class Tok : u8 {
    End,
    Ident,
    Number,
    KwTrue,
    KwFalse,
    KwFormula,
    LParen,
    RParen,
    Comma,
    Dot,
    Plus,
    Minus,
    Star,
    Slash,
    Caret,
    Lt,
    Le,
    Gt,
    Ge,
    EqEq,
    NotEq,
    AndAnd,
    OrOr,
    Bang,
    Assign,
    Semicolon,
};

std::string_view tokName(Tok tok) noexcept;

struct Token {
    Tok kind = Tok::End;
    std::string_view text;
    u32 line = 1;
    u32 column = 1;
    f64 number = 0.0; // Tok::Number
};

/// Tokenizes the whole source; the last token is Tok::End. E_LEX / E_NUMBER on failure.
bool lex(std::string_view source, std::vector<Token>& out, Diagnostic& diag);

enum class NodeKind : u8 { Number, Bool, Path, Unary, Binary, Call };

struct Node {
    NodeKind kind = NodeKind::Number;
    u32 line = 0;      // main token: literal, first path segment, operator, function name
    u32 column = 0;
    u32 startLine = 0; // first token of the expression (an enclosing '(' if parenthesized)
    u32 startColumn = 0;
    u32 depth = 1;
    f64 number = 0.0;
    bool boolean = false;
    Tok op = Tok::End;                 // Unary / Binary
    std::string name;                  // Call
    std::vector<std::string> segments; // Path
    std::vector<u32> children;         // Unary: 1, Binary: 2, Call: args
};

struct Ast {
    std::vector<Node> nodes;
    u32 root = 0;
    bool isFormula = false;
    std::string formulaName;
    std::vector<std::string> params; // formula parameters
};

/// Parses the tokens. E_SYNTAX, E_DUPLICATE_PARAM or E_LIMIT on failure.
bool parse(const std::vector<Token>& tokens, Ast& ast, Diagnostic& diag);

/// True for [A-Za-z_][A-Za-z0-9_]* (at most limits::kMaxNameBytes bytes).
bool isIdentifier(std::string_view text) noexcept;
/// True for identifiers joined by single dots ("Ship.MaxSpeed").
bool isDottedName(std::string_view text) noexcept;

} // namespace helios::hxl::detail
