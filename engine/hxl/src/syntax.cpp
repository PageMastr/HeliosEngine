// HXL lexer and parser. Mirrors services/pkg/hxl/syntax.go (same checks in the same order, so both
// report the same status at the same position).
#include "syntax.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <system_error>

#include "helios/core/platform.h"

namespace helios::hxl::detail {
namespace {

bool isIdentStart(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isIdentChar(char c) noexcept { return isIdentStart(c) || isDigit(c); }

void fail(Diagnostic& diag, Status status, u32 line, u32 column, std::string message) {
    diag.status = status;
    diag.line = line;
    diag.column = column;
    diag.message = std::move(message);
}

// Parses a lexically valid decimal literal of at most limits::kMaxNumberBytes bytes. Literals must
// be zero or normal finite doubles. std::from_chars is correctly rounded for every input, but Go's
// strconv.ParseFloat only up to 800 significant digits (it truncates longer mantissas when its fast
// paths fail), so the lexer bounds the length first: within it both parsers agree on every literal
// (the corpus pins the rounding edges, tests/corpus/hxl).
bool parseNumber(std::string_view text, f64& out) {
    f64 value = 0.0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (ec != std::errc() || ptr != text.data() + text.size()) return false;
    if (!std::isfinite(value)) return false;
    if (value == 0.0) {
        // Mantissa digits must all be zero (otherwise the literal underflowed).
        for (char c : text) {
            if (c == 'e' || c == 'E') break;
            if (c >= '1' && c <= '9') return false;
        }
    } else if (value < std::numeric_limits<f64>::min()) {
        return false; // subnormal
    }
    out = value;
    return true;
}

} // namespace

std::string_view tokName(Tok tok) noexcept {
    switch (tok) {
    case Tok::End: return "end of input";
    case Tok::Ident: return "a name";
    case Tok::Number: return "a number";
    case Tok::KwTrue: return "'true'";
    case Tok::KwFalse: return "'false'";
    case Tok::KwFormula: return "'formula'";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::Comma: return "','";
    case Tok::Dot: return "'.'";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Caret: return "'^'";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::EqEq: return "'=='";
    case Tok::NotEq: return "'!='";
    case Tok::AndAnd: return "'&&'";
    case Tok::OrOr: return "'||'";
    case Tok::Bang: return "'!'";
    case Tok::Assign: return "'='";
    case Tok::Semicolon: return "';'";
    }
    return "?";
}

bool isIdentifier(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxNameBytes || !isIdentStart(text[0])) return false;
    for (char c : text) {
        if (!isIdentChar(c)) return false;
    }
    return true;
}

bool isDottedName(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxNameBytes) return false;
    usize start = 0;
    while (true) {
        const usize dot = text.find('.', start);
        const std::string_view seg = text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (!isIdentifier(seg)) return false;
        if (dot == std::string_view::npos) return true;
        start = dot + 1;
    }
}

bool lex(std::string_view src, std::vector<Token>& out, Diagnostic& diag) {
    out.clear();
    usize i = 0;
    u32 line = 1;
    u32 col = 1;
    const usize n = src.size();
    auto peek = [&](usize k) -> char { return i + k < n ? src[i + k] : '\0'; };
    while (true) {
        // Whitespace and comments.
        while (i < n) {
            const char c = src[i];
            if (c == ' ' || c == '\t' || c == '\r') {
                ++i;
                ++col;
            } else if (c == '\n') {
                ++i;
                ++line;
                col = 1;
            } else if (c == '/' && peek(1) == '/') {
                while (i < n && src[i] != '\n') {
                    ++i;
                    ++col;
                }
            } else {
                break;
            }
        }
        Token tok;
        tok.line = line;
        tok.column = col;
        if (i >= n) {
            tok.kind = Tok::End;
            out.push_back(tok);
            return true;
        }
        const usize start = i;
        const char c = src[i];
        if (isIdentStart(c)) {
            while (i < n && isIdentChar(src[i])) ++i;
            tok.text = src.substr(start, i - start);
            if (tok.text == "true") {
                tok.kind = Tok::KwTrue;
            } else if (tok.text == "false") {
                tok.kind = Tok::KwFalse;
            } else if (tok.text == "formula") {
                tok.kind = Tok::KwFormula;
            } else {
                tok.kind = Tok::Ident;
                if (tok.text.size() > limits::kMaxNameBytes) {
                    fail(diag, Status::Limit, line, col, std::format("name longer than {} bytes", limits::kMaxNameBytes));
                    return false;
                }
            }
        } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
            bool ok = true;
            while (i < n && isDigit(src[i])) ++i;
            if (i < n && src[i] == '.') {
                ++i;
                while (i < n && isDigit(src[i])) ++i;
            }
            if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                ++i;
                if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
                if (i >= n || !isDigit(src[i])) ok = false;
                while (i < n && isDigit(src[i])) ++i;
            }
            if (i < n && (isIdentChar(src[i]) || src[i] == '.')) ok = false;
            tok.text = src.substr(start, i - start);
            tok.kind = Tok::Number;
            if (!ok) {
                fail(diag, Status::Number, line, col, "malformed number literal");
                return false;
            }
            if (tok.text.size() > limits::kMaxNumberBytes) {
                fail(diag, Status::Number, line, col, std::format("number literal longer than {} bytes", limits::kMaxNumberBytes));
                return false;
            }
            if (!parseNumber(tok.text, tok.number)) {
                fail(diag, Status::Number, line, col,
                     std::format("number literal '{}' is out of range (zero or a normal double)", tok.text));
                return false;
            }
        } else {
            const char d = peek(1);
            usize len = 1;
            switch (c) {
            case '(': tok.kind = Tok::LParen; break;
            case ')': tok.kind = Tok::RParen; break;
            case ',': tok.kind = Tok::Comma; break;
            case '.': tok.kind = Tok::Dot; break;
            case '+': tok.kind = Tok::Plus; break;
            case '-': tok.kind = Tok::Minus; break;
            case '*': tok.kind = Tok::Star; break;
            case '/': tok.kind = Tok::Slash; break;
            case '^': tok.kind = Tok::Caret; break;
            case ';': tok.kind = Tok::Semicolon; break;
            case '<':
                tok.kind = d == '=' ? Tok::Le : Tok::Lt;
                len = d == '=' ? 2 : 1;
                break;
            case '>':
                tok.kind = d == '=' ? Tok::Ge : Tok::Gt;
                len = d == '=' ? 2 : 1;
                break;
            case '=':
                tok.kind = d == '=' ? Tok::EqEq : Tok::Assign;
                len = d == '=' ? 2 : 1;
                break;
            case '!':
                tok.kind = d == '=' ? Tok::NotEq : Tok::Bang;
                len = d == '=' ? 2 : 1;
                break;
            case '&':
                if (d != '&') {
                    fail(diag, Status::Lex, line, col, "unexpected character '&' (did you mean '&&'?)");
                    return false;
                }
                tok.kind = Tok::AndAnd;
                len = 2;
                break;
            case '|':
                if (d != '|') {
                    fail(diag, Status::Lex, line, col, "unexpected character '|' (did you mean '||'?)");
                    return false;
                }
                tok.kind = Tok::OrOr;
                len = 2;
                break;
            default: {
                const auto byte = static_cast<unsigned char>(c);
                if (byte >= 0x21 && byte < 0x7f) {
                    fail(diag, Status::Lex, line, col, std::format("unexpected character '{}'", c));
                } else {
                    fail(diag, Status::Lex, line, col, std::format("unexpected byte 0x{:02x}", byte));
                }
                return false;
            }
            }
            i += len;
            tok.text = src.substr(start, len);
        }
        col += static_cast<u32>(i - start);
        out.push_back(tok);
    }
}

namespace {

// Binary operators by precedence, loosest first; 0 = not a binary operator. Every level is
// left-associative: or := and {'||' and}, and := equality {'&&' equality}, and so on down to
// product := unary {('*'|'/') unary}. `^` binds tighter than the prefix operators and is parsed by
// parsePower().
int binaryPrecedence(Tok tok) noexcept {
    switch (tok) {
    case Tok::OrOr: return 1;
    case Tok::AndAnd: return 2;
    case Tok::EqEq:
    case Tok::NotEq: return 3;
    case Tok::Lt:
    case Tok::Le:
    case Tok::Gt:
    case Tok::Ge: return 4;
    case Tok::Plus:
    case Tok::Minus: return 5;
    case Tok::Star:
    case Tok::Slash: return 6;
    default: return 0;
    }
}

// Stack use: hostile sources nest up to limits::kMaxParseDepth levels, and every level recurses
// through parseExpr -> parseBinary -> parseUnary -> parsePower -> parsePrimary. Precedence climbing
// (one parseBinary frame per precedence level actually used, instead of one function per level)
// and out-of-line node builders and error paths keep those frames small; the README records the
// measured worst case. The trees, the node order and every diagnostic are exactly those of one
// recursive-descent function per level (services/pkg/hxl/syntax.go is the same code; the corpus
// checks error codes and positions).
class Parser {
public:
    Parser(const std::vector<Token>& tokens, Ast& ast, Diagnostic& diag) : m_toks(tokens), m_ast(ast), m_diag(diag) {}

    bool parseSource() {
        if (cur().kind == Tok::KwFormula) {
            advance();
            if (cur().kind != Tok::Ident) return syntaxError("expected the formula name");
            m_ast.isFormula = true;
            m_ast.formulaName = std::string(cur().text);
            advance();
            if (cur().kind != Tok::LParen) return syntaxError("expected '(' after the formula name");
            advance();
            if (cur().kind != Tok::RParen) {
                while (true) {
                    if (cur().kind != Tok::Ident) return syntaxError("expected a parameter name");
                    const std::string param(cur().text);
                    for (const std::string& p : m_ast.params) {
                        if (p == param) {
                            fail(m_diag, Status::DuplicateParam, cur().line, cur().column,
                                 std::format("duplicate parameter '{}'", param));
                            return false;
                        }
                    }
                    if (m_ast.params.size() == limits::kMaxParams) {
                        fail(m_diag, Status::Limit, cur().line, cur().column,
                             std::format("more than {} parameters", limits::kMaxParams));
                        return false;
                    }
                    m_ast.params.push_back(param);
                    advance();
                    if (cur().kind == Tok::Comma) {
                        advance();
                        continue;
                    }
                    break;
                }
            }
            if (cur().kind != Tok::RParen) return syntaxError("expected ',' or ')' in the parameter list");
            advance();
            if (cur().kind != Tok::Assign) return syntaxError("expected '=' after the parameter list");
            advance();
        }
        u32 root = 0;
        if (!parseExpr(root)) return false;
        m_ast.root = root;
        if (cur().kind == Tok::Semicolon) advance();
        if (cur().kind != Tok::End) return syntaxError("expected an operator or the end of the expression");
        return true;
    }

private:
    // Tokens are never modified while parsing, so references into m_toks stay valid.
    const Token& cur() const { return m_toks[m_pos]; }
    const Token& next() const { return m_toks[m_pos + 1 < m_toks.size() ? m_pos + 1 : m_pos]; }
    void advance() {
        if (m_pos + 1 < m_toks.size()) ++m_pos;
    }

    HELIOS_NOINLINE bool syntaxError(std::string_view what) {
        fail(m_diag, Status::Syntax, cur().line, cur().column, std::format("{}, found {}", what, tokName(cur().kind)));
        return false;
    }

    HELIOS_NOINLINE bool depthError(const Token& at) {
        fail(m_diag, Status::Limit, at.line, at.column,
             std::format("expression nested deeper than {} levels", limits::kMaxParseDepth));
        return false;
    }

    u32 addNode(Node node) {
        m_ast.nodes.push_back(std::move(node));
        return static_cast<u32>(m_ast.nodes.size() - 1);
    }

    // Composite nodes: depth = 1 + deepest child; the tree depth is bounded (codegen recursion).
    bool finishComposite(Node& node) {
        u32 depth = 0;
        for (u32 child : node.children) depth = std::max(depth, m_ast.nodes[child].depth);
        node.depth = depth + 1;
        if (node.depth > limits::kMaxAstDepth) {
            fail(m_diag, Status::Limit, node.line, node.column,
                 std::format("expression tree deeper than {} levels", limits::kMaxAstDepth));
            return false;
        }
        return true;
    }

    HELIOS_NOINLINE bool makeBinary(const Token& opTok, u32 lhs, u32 rhs, u32& out) {
        Node node;
        node.kind = NodeKind::Binary;
        node.op = opTok.kind;
        node.line = opTok.line;
        node.column = opTok.column;
        node.startLine = m_ast.nodes[lhs].startLine;
        node.startColumn = m_ast.nodes[lhs].startColumn;
        node.children = {lhs, rhs};
        if (!finishComposite(node)) return false;
        out = addNode(std::move(node));
        return true;
    }

    HELIOS_NOINLINE bool makeUnary(const Token& opTok, u32 child, u32& out) {
        Node node;
        node.kind = NodeKind::Unary;
        node.op = opTok.kind;
        node.line = opTok.line;
        node.column = opTok.column;
        node.startLine = opTok.line;
        node.startColumn = opTok.column;
        node.children = {child};
        if (!finishComposite(node)) return false;
        out = addNode(std::move(node));
        return true;
    }

    HELIOS_NOINLINE bool makeCall(const Token& nameTok, std::vector<u32> args, u32& out) {
        Node node;
        node.kind = NodeKind::Call;
        node.name = std::string(nameTok.text);
        node.line = node.startLine = nameTok.line;
        node.column = node.startColumn = nameTok.column;
        node.children = std::move(args);
        if (!finishComposite(node)) return false;
        out = addNode(std::move(node));
        return true;
    }

    HELIOS_NOINLINE u32 addLiteral(const Token& tok) {
        Node node;
        node.line = node.startLine = tok.line;
        node.column = node.startColumn = tok.column;
        if (tok.kind == Tok::Number) {
            node.kind = NodeKind::Number;
            node.number = tok.number;
        } else {
            node.kind = NodeKind::Bool;
            node.boolean = tok.kind == Tok::KwTrue;
        }
        return addNode(std::move(node));
    }

    bool parseExpr(u32& out) {
        if (++m_depth > limits::kMaxParseDepth) return depthError(cur());
        const bool ok = parseBinary(1, out);
        --m_depth;
        return ok;
    }

    // Parses a chain of binary operators of precedence >= minPrec (precedence climbing).
    bool parseBinary(int minPrec, u32& out) {
        u32 lhs = 0;
        if (!parseUnary(lhs)) return false;
        for (int prec = binaryPrecedence(cur().kind); prec >= minPrec; prec = binaryPrecedence(cur().kind)) {
            const Token& opTok = cur();
            advance();
            u32 rhs = 0;
            if (!parseBinary(prec + 1, rhs)) return false;
            if (!makeBinary(opTok, lhs, rhs, lhs)) return false;
        }
        out = lhs;
        return true;
    }

    bool parseUnary(u32& out) {
        const Token& opTok = cur();
        if (opTok.kind != Tok::Minus && opTok.kind != Tok::Bang) return parsePower(out);
        advance();
        if (++m_depth > limits::kMaxParseDepth) return depthError(opTok);
        u32 child = 0;
        const bool ok = parseUnary(child);
        --m_depth;
        return ok && makeUnary(opTok, child, out);
    }

    bool parsePower(u32& out) {
        u32 base = 0;
        if (!parsePrimary(base)) return false;
        if (cur().kind != Tok::Caret) {
            out = base;
            return true;
        }
        const Token& opTok = cur();
        advance();
        if (++m_depth > limits::kMaxParseDepth) return depthError(opTok);
        u32 exponent = 0;
        const bool ok = parseUnary(exponent);
        --m_depth;
        return ok && makeBinary(opTok, base, exponent, out);
    }

    bool parsePrimary(u32& out) {
        const Token& tok = cur();
        switch (tok.kind) {
        case Tok::Number:
        case Tok::KwTrue:
        case Tok::KwFalse:
            out = addLiteral(tok);
            advance();
            return true;
        case Tok::LParen: {
            advance();
            u32 inner = 0;
            if (!parseExpr(inner)) return false;
            if (cur().kind != Tok::RParen) return syntaxError("expected ')'");
            advance();
            m_ast.nodes[inner].startLine = tok.line;
            m_ast.nodes[inner].startColumn = tok.column;
            out = inner;
            return true;
        }
        case Tok::Ident:
            if (next().kind == Tok::LParen) return parseCall(out);
            return parsePath(out);
        default: return syntaxError("expected an expression");
        }
    }

    bool parseCall(u32& out) {
        const Token& nameTok = cur();
        advance();
        advance();
        std::vector<u32> args;
        if (cur().kind != Tok::RParen) {
            while (true) {
                u32 arg = 0;
                if (!parseExpr(arg)) return false;
                args.push_back(arg);
                if (cur().kind == Tok::Comma) {
                    advance();
                    continue;
                }
                break;
            }
        }
        if (cur().kind != Tok::RParen) return syntaxError("expected ',' or ')' in the argument list");
        advance();
        return makeCall(nameTok, std::move(args), out);
    }

    HELIOS_NOINLINE bool parsePath(u32& out) {
        const Token& tok = cur();
        Node node;
        node.kind = NodeKind::Path;
        node.line = node.startLine = tok.line;
        node.column = node.startColumn = tok.column;
        node.segments.emplace_back(tok.text);
        advance();
        while (cur().kind == Tok::Dot) {
            advance();
            if (cur().kind != Tok::Ident) return syntaxError("expected a name after '.'");
            node.segments.emplace_back(cur().text);
            advance();
        }
        out = addNode(std::move(node));
        return true;
    }

    const std::vector<Token>& m_toks;
    Ast& m_ast;
    Diagnostic& m_diag;
    usize m_pos = 0;
    u32 m_depth = 0;
};

} // namespace

bool parse(const std::vector<Token>& tokens, Ast& ast, Diagnostic& diag) {
    ast = Ast{};
    Parser parser(tokens, ast, diag);
    return parser.parseSource();
}

} // namespace helios::hxl::detail
