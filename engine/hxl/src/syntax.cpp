// HXL lexer and parser. Mirrors services/pkg/hxl/syntax.go (same checks in the same order, so both
// report the same status at the same position).
#include "syntax.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <system_error>

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

// Parses a lexically valid decimal literal. Literals must be zero or normal finite doubles, so
// both languages' correctly rounded parsers agree on every accepted literal.
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
    const Token& cur() const { return m_toks[m_pos]; }
    const Token& next() const { return m_toks[m_pos + 1 < m_toks.size() ? m_pos + 1 : m_pos]; }
    void advance() {
        if (m_pos + 1 < m_toks.size()) ++m_pos;
    }

    bool syntaxError(std::string_view what) {
        fail(m_diag, Status::Syntax, cur().line, cur().column, std::format("{}, found {}", what, tokName(cur().kind)));
        return false;
    }

    bool depthError(const Token& at) {
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

    bool makeBinary(const Token& opTok, u32 lhs, u32 rhs, u32& out) {
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

    bool parseExpr(u32& out) {
        if (++m_depth > limits::kMaxParseDepth) return depthError(cur());
        const bool ok = parseOr(out);
        --m_depth;
        return ok;
    }

    template <class Next>
    bool parseLeftAssoc(u32& out, Next next, std::initializer_list<Tok> ops) {
        u32 lhs = 0;
        if (!(this->*next)(lhs)) return false;
        while (std::find(ops.begin(), ops.end(), cur().kind) != ops.end()) {
            const Token opTok = cur();
            advance();
            u32 rhs = 0;
            if (!(this->*next)(rhs)) return false;
            if (!makeBinary(opTok, lhs, rhs, lhs)) return false;
        }
        out = lhs;
        return true;
    }

    bool parseOr(u32& out) { return parseLeftAssoc(out, &Parser::parseAnd, {Tok::OrOr}); }
    bool parseAnd(u32& out) { return parseLeftAssoc(out, &Parser::parseEquality, {Tok::AndAnd}); }
    bool parseEquality(u32& out) { return parseLeftAssoc(out, &Parser::parseCompare, {Tok::EqEq, Tok::NotEq}); }
    bool parseCompare(u32& out) {
        return parseLeftAssoc(out, &Parser::parseSum, {Tok::Lt, Tok::Le, Tok::Gt, Tok::Ge});
    }
    bool parseSum(u32& out) { return parseLeftAssoc(out, &Parser::parseProduct, {Tok::Plus, Tok::Minus}); }
    bool parseProduct(u32& out) { return parseLeftAssoc(out, &Parser::parseUnary, {Tok::Star, Tok::Slash}); }

    bool parseUnary(u32& out) {
        if (cur().kind == Tok::Minus || cur().kind == Tok::Bang) {
            const Token opTok = cur();
            advance();
            if (++m_depth > limits::kMaxParseDepth) return depthError(opTok);
            u32 child = 0;
            const bool ok = parseUnary(child);
            --m_depth;
            if (!ok) return false;
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
        return parsePower(out);
    }

    bool parsePower(u32& out) {
        u32 base = 0;
        if (!parsePrimary(base)) return false;
        if (cur().kind != Tok::Caret) {
            out = base;
            return true;
        }
        const Token opTok = cur();
        advance();
        if (++m_depth > limits::kMaxParseDepth) return depthError(opTok);
        u32 exponent = 0;
        const bool ok = parseUnary(exponent);
        --m_depth;
        if (!ok) return false;
        return makeBinary(opTok, base, exponent, out);
    }

    bool parsePrimary(u32& out) {
        const Token tok = cur();
        Node node;
        node.line = node.startLine = tok.line;
        node.column = node.startColumn = tok.column;
        switch (tok.kind) {
        case Tok::Number:
            node.kind = NodeKind::Number;
            node.number = tok.number;
            advance();
            out = addNode(std::move(node));
            return true;
        case Tok::KwTrue:
        case Tok::KwFalse:
            node.kind = NodeKind::Bool;
            node.boolean = tok.kind == Tok::KwTrue;
            advance();
            out = addNode(std::move(node));
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
            if (next().kind == Tok::LParen) {
                node.kind = NodeKind::Call;
                node.name = std::string(tok.text);
                advance();
                advance();
                if (cur().kind != Tok::RParen) {
                    while (true) {
                        u32 arg = 0;
                        if (!parseExpr(arg)) return false;
                        node.children.push_back(arg);
                        if (cur().kind == Tok::Comma) {
                            advance();
                            continue;
                        }
                        break;
                    }
                }
                if (cur().kind != Tok::RParen) return syntaxError("expected ',' or ')' in the argument list");
                advance();
                if (!finishComposite(node)) return false;
                out = addNode(std::move(node));
                return true;
            }
            node.kind = NodeKind::Path;
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
        default: return syntaxError("expected an expression");
        }
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
