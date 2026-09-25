#include "parser.h"

#include <charconv>
#include <format>

#include "lexer.h"

namespace helios::schemac {

namespace {

struct KindEntry {
    std::string_view word;
    DeclKindAst kind;
};
constexpr KindEntry kKinds[] = {
    {"enum", DeclKindAst::Enum},       {"flags", DeclKindAst::Flags},         {"struct", DeclKindAst::Struct},
    {"component", DeclKindAst::Component}, {"relation", DeclKindAst::Relation}, {"record", DeclKindAst::Record},
    {"event", DeclKindAst::Event},     {"rpc", DeclKindAst::Rpc},             {"message", DeclKindAst::Message},
    {"service", DeclKindAst::Service}, {"viewmodel", DeclKindAst::ViewModel}, {"formula", DeclKindAst::Formula},
    {"const", DeclKindAst::Const},     {"alias", DeclKindAst::Alias},         {"scriptlib", DeclKindAst::ScriptLib},
};

/// Nesting limit for types, inline bodies and list literals: hostile or generated input must
/// produce a diagnostic, not a stack overflow (every later pass recurses over the same shapes).
constexpr int kMaxNesting = 64;

std::string describe(const Token& t) {
    switch (t.kind) {
    case Tok::End: return "end of file";
    case Tok::Doc: return "doc comment";
    case Tok::String: return std::format("string {}", t.text);
    default: return std::format("'{}'", t.text);
    }
}

class Parser {
public:
    Parser(std::string_view text, u32 file, DiagnosticEngine& diags)
        : m_text(text), m_diags(diags), m_tokens(tokenize(text, file, diags)) {}

    FileAst run() {
        FileAst out;
        skipDocs();
        if (peek().isIdent("package")) {
            next();
            out.packageLoc = peek().loc;
            if (!parseQualifiedName(out.package, "package name") || !expectPunct(";", "after the package name")) recover();
        } else {
            error(peek().loc, std::format("expected 'package <name>;' at the start of the file, got {}", describe(peek())));
        }
        skipDocs();
        while (peek().isIdent("import")) {
            next();
            ImportAst imp;
            imp.loc = peek().loc;
            if (peek().kind != Tok::String) {
                error(peek().loc, std::format("expected an import path string, got {}", describe(peek())));
                recover();
                continue;
            }
            if (!decodeString(next().text, imp.path)) error(imp.loc, "invalid escape in import path");
            if (!expectPunct(";", "after the import path")) {
                recover();
                continue;
            }
            out.imports.push_back(std::move(imp));
            skipDocs();
        }
        while (true) {
            std::string doc = collectDocs();
            if (peek().kind == Tok::End) break;
            if (peek().isIdent("import") || peek().isIdent("package")) {
                error(peek().loc, std::format("'{}' must come before all declarations", peek().text));
                recover();
                continue;
            }
            DeclAst decl;
            decl.doc = std::move(doc);
            m_failed = false;
            if (parseDecl(decl) && !m_failed) {
                out.decls.push_back(std::move(decl));
            } else {
                recover();
            }
        }
        return out;
    }

private:
    /// RAII nesting counter; enter() fails (with a diagnostic) beyond kMaxNesting.
    struct NestGuard {
        Parser& p;
        bool ok;
        NestGuard(Parser& parser, SourceLoc loc) : p(parser), ok(++parser.m_nesting <= kMaxNesting) {
            if (!ok) p.error(loc, std::format("nesting too deep (more than {} levels of types, bodies or list literals)", kMaxNesting));
        }
        ~NestGuard() { --p.m_nesting; }
        NestGuard(const NestGuard&) = delete;
        NestGuard& operator=(const NestGuard&) = delete;
    };

    // --- token access --------------------------------------------------------------------------
    const Token& peek(usize k = 0) const {
        const usize i = std::min(m_pos + k, m_tokens.size() - 1);
        return m_tokens[i];
    }
    const Token& next() {
        const Token& t = m_tokens[m_pos];
        if (m_pos + 1 < m_tokens.size()) ++m_pos;
        return t;
    }
    bool acceptPunct(std::string_view p) {
        if (!peek().isPunct(p)) return false;
        next();
        return true;
    }
    void error(SourceLoc loc, std::string message) {
        if (m_failed) return; // one error per declaration; the rest is usually a cascade
        m_failed = true;
        m_diags.error(loc, std::move(message));
    }
    bool expectPunct(std::string_view p, std::string_view context) {
        if (acceptPunct(p)) return true;
        error(peek().loc, std::format("expected '{}' {}, got {}", p, context, describe(peek())));
        return false;
    }
    bool expectIdent(std::string& out, std::string_view what) {
        if (peek().kind == Tok::Ident) {
            out = std::string(next().text);
            return true;
        }
        error(peek().loc, std::format("expected {}, got {}", what, describe(peek())));
        return false;
    }
    bool parseQualifiedName(std::string& out, std::string_view what) {
        if (!expectIdent(out, what)) return false;
        while (peek().isPunct(".") && peek(1).kind == Tok::Ident && !peek().spaceBefore) {
            next();
            out += '.';
            out += next().text;
        }
        return true;
    }
    void skipDocs() {
        while (peek().kind == Tok::Doc) next();
    }
    std::string collectDocs() {
        std::string doc;
        while (peek().kind == Tok::Doc) {
            if (!doc.empty()) doc += '\n';
            doc += next().text;
        }
        return doc;
    }
    /// Skips to the next token that starts a declaration on a new line (outside braces).
    void recover() {
        int depth = 0;
        bool first = true;
        while (peek().kind != Tok::End) {
            const Token& t = peek();
            if (!first && depth <= 0 && t.newlineBefore && t.kind == Tok::Ident && isDeclKeyword(t.text)) break;
            if (!first && depth <= 0 && t.kind == Tok::Doc && t.newlineBefore) break;
            if (t.isPunct("{")) ++depth;
            if (t.isPunct("}")) --depth;
            first = false;
            next();
        }
        m_failed = false;
    }

    // --- attributes ----------------------------------------------------------------------------
    bool parseAttrArgs(AttrAst& attr) {
        attr.hasParens = true;
        next(); // '('
        if (acceptPunct(")")) return true;
        while (true) {
            AttrArgAst arg;
            arg.loc = peek().loc;
            if (peek().kind == Tok::Ident && peek(1).isPunct("=") && !peek(2).isPunct("=")) {
                arg.key = std::string(next().text);
                next();
                arg.loc = peek().loc;
            }
            const usize first = m_pos;
            int depth = 0;
            while (true) {
                const Token& t = peek();
                if (t.kind == Tok::End) {
                    error(attr.loc, std::format("unterminated arguments of @{}", attr.name));
                    return false;
                }
                if (depth == 0 && (t.isPunct(",") || t.isPunct(")"))) break;
                if (depth == 0 && (t.isPunct("}") || t.isPunct("]"))) {
                    error(t.loc, std::format("expected ')' to close the arguments of @{}, got '{}'", attr.name, t.text));
                    return false;
                }
                if (t.isPunct("(") || t.isPunct("[") || t.isPunct("{")) ++depth;
                if (t.isPunct(")") || t.isPunct("]") || t.isPunct("}")) --depth;
                next();
            }
            if (m_pos == first) {
                error(peek().loc, std::format("empty argument in @{}(...)", attr.name));
                return false;
            }
            const Token& a = m_tokens[first];
            const Token& b = m_tokens[m_pos - 1];
            if (m_pos - first == 1 && a.kind == Tok::String) {
                arg.isString = true;
                if (!decodeString(a.text, arg.value)) {
                    error(a.loc, "invalid escape sequence in string");
                    return false;
                }
            } else {
                arg.value = std::string(m_text.substr(a.offset, b.end - a.offset));
            }
            attr.args.push_back(std::move(arg));
            if (acceptPunct(",")) continue;
            next(); // ')'
            return true;
        }
    }

    /// '@' ident [(args)]
    bool parseAtAttr(std::vector<AttrAst>& out) {
        next(); // '@'
        AttrAst attr;
        attr.loc = peek().loc;
        if (!expectIdent(attr.name, "an attribute name after '@'")) return false;
        if (peek().isPunct("(") && !parseAttrArgs(attr)) return false;
        out.push_back(std::move(attr));
        return true;
    }

    /// Header attributes: '@' attrs anywhere, bare ones (`replicate(all)`, `reliable`) on the same
    /// line, and message directions (`client->server`).
    bool parseHeaderAttrs(std::vector<AttrAst>& out, std::string* direction, SourceLoc* directionLoc) {
        while (true) {
            const Token& t = peek();
            if (t.isPunct("@")) {
                if (!parseAtAttr(out)) return false;
                continue;
            }
            if (t.kind != Tok::Ident || t.newlineBefore || isDeclKeyword(t.text)) return true;
            if ((t.text == "client" || t.text == "server") && peek(1).isPunct("->")) {
                const SourceLoc loc = t.loc;
                std::string from(next().text);
                next();
                std::string to;
                if (!expectIdent(to, "'client' or 'server' after '->'")) return false;
                if (to != "client" && to != "server") {
                    error(loc, std::format("invalid direction '{}->{}' (use client->server, server->client or server->server)", from, to));
                    return false;
                }
                const std::string dir = from + "->" + to;
                if (dir == "client->client") {
                    error(loc, "invalid direction 'client->client'");
                    return false;
                }
                if (!direction) {
                    error(loc, "a message direction is only valid on an rpc");
                    return false;
                }
                if (!direction->empty()) {
                    error(loc, "rpc has more than one direction");
                    return false;
                }
                *direction = dir;
                if (directionLoc) *directionLoc = loc;
                continue;
            }
            AttrAst attr;
            attr.loc = t.loc;
            attr.name = std::string(next().text);
            if (peek().isPunct("(") && !parseAttrArgs(attr)) return false;
            out.push_back(std::move(attr));
        }
    }

    // --- literals ------------------------------------------------------------------------------
    bool parseLiteral(LiteralAst& lit) {
        lit.loc = peek().loc;
        const NestGuard guard(*this, lit.loc);
        if (!guard.ok) return false;
        if (peek().isPunct("[")) {
            next();
            lit.kind = LiteralAst::Kind::List;
            if (acceptPunct("]")) return true;
            while (true) {
                LiteralAst item;
                if (!parseLiteral(item)) return false;
                lit.items.push_back(std::move(item));
                if (acceptPunct(",")) {
                    if (acceptPunct("]")) return true;
                    continue;
                }
                return expectPunct("]", "to close the list literal");
            }
        }
        if (peek().isPunct("-")) {
            next();
            lit.negative = true;
            if (peek().kind != Tok::Int && peek().kind != Tok::Float) {
                error(peek().loc, std::format("expected a number after '-', got {}", describe(peek())));
                return false;
            }
        }
        const Token& t = peek();
        switch (t.kind) {
        case Tok::Int:
        case Tok::Float: {
            next();
            lit.kind = t.kind == Tok::Int ? LiteralAst::Kind::Int : LiteralAst::Kind::Float;
            lit.text = std::string(t.text);
            std::erase(lit.text, '_');
            if (peek().kind == Tok::Ident && !peek().spaceBefore) {
                lit.kind = LiteralAst::Kind::Unit;
                lit.text += next().text;
            }
            return true;
        }
        case Tok::String:
            next();
            lit.kind = LiteralAst::Kind::String;
            if (!decodeString(t.text, lit.text)) {
                error(t.loc, "invalid escape sequence in string");
                return false;
            }
            return true;
        case Tok::Ident:
            if (lit.negative) break;
            if (t.text == "true" || t.text == "false") {
                next();
                lit.kind = LiteralAst::Kind::Bool;
                lit.boolValue = t.text == "true";
                return true;
            }
            if (t.text == "null" || t.text == "none") {
                next();
                lit.kind = LiteralAst::Kind::Null;
                return true;
            }
            lit.kind = LiteralAst::Kind::Ident;
            return parseQualifiedName(lit.text, "a value");
        default: break;
        }
        error(t.loc, std::format("expected a literal value, got {}", describe(t)));
        return false;
    }

    // --- types ---------------------------------------------------------------------------------
    bool parseType(std::unique_ptr<TypeExpr>& out) {
        auto type = std::make_unique<TypeExpr>();
        type->loc = peek().loc;
        const NestGuard guard(*this, type->loc);
        if (!guard.ok) return false;
        if (peek().isPunct("{")) {
            next();
            type->kind = TypeExpr::Kind::InlineStruct;
            type->members = std::make_unique<MembersAst>();
            if (!parseMembers(*type->members, Block::None, false, true)) return false;
            if (!expectPunct("}", "to close the inline struct")) return false;
        } else if (peek().isIdent("enum") && peek(1).isPunct("{")) {
            next();
            type->kind = TypeExpr::Kind::InlineEnum;
            if (!parseEnumBody(type->enumValues)) return false;
        } else if (peek().isIdent("variant") && peek(1).isPunct("{")) {
            next();
            type->kind = TypeExpr::Kind::InlineVariant;
            if (!parseVariantBody(type->alternatives)) return false;
        } else {
            if (peek().kind != Tok::Ident) {
                error(peek().loc, std::format("expected a type, got {}", describe(peek())));
                return false;
            }
            if (!parseQualifiedName(type->name, "a type name")) return false;
            if (acceptPunct("<")) {
                type->kind = TypeExpr::Kind::Generic;
                while (true) {
                    std::unique_ptr<TypeExpr> arg;
                    if (!parseType(arg)) return false;
                    type->args.push_back(std::move(arg));
                    if (acceptPunct(",")) continue;
                    if (!expectPunct(">", "to close the type argument list")) return false;
                    break;
                }
            }
        }
        // Postfix: T? and T[N], in any order. Each wrapper is a nesting level too (the TypeExpr chain
        // is destroyed and analyzed recursively).
        int wrappers = 0;
        while (true) {
            if ((peek().isPunct("?") || peek().isPunct("[")) && !peek().newlineBefore && m_nesting + ++wrappers > kMaxNesting) {
                error(peek().loc, std::format("nesting too deep (more than {} levels of types, bodies or list literals)", kMaxNesting));
                return false;
            }
            if (peek().isPunct("?") && !peek().newlineBefore) {
                const SourceLoc loc = next().loc;
                auto opt = std::make_unique<TypeExpr>();
                opt->kind = TypeExpr::Kind::Optional;
                opt->loc = loc;
                opt->args.push_back(std::move(type));
                type = std::move(opt);
                continue;
            }
            if (peek().isPunct("[") && !peek().newlineBefore) {
                const SourceLoc loc = next().loc;
                if (peek().kind != Tok::Int) {
                    error(peek().loc, std::format("expected an array size, got {}", describe(peek())));
                    return false;
                }
                std::string digits(next().text);
                std::erase(digits, '_');
                u64 n = 0;
                const bool hex = digits.size() > 2 && (digits[1] == 'x' || digits[1] == 'X');
                const auto r = std::from_chars(digits.data() + (hex ? 2 : 0), digits.data() + digits.size(), n, hex ? 16 : 10);
                if (r.ec != std::errc() || n == 0 || n > 65536) {
                    error(loc, "array size must be between 1 and 65536");
                    return false;
                }
                if (!expectPunct("]", "after the array size")) return false;
                auto arr = std::make_unique<TypeExpr>();
                arr->kind = TypeExpr::Kind::Array;
                arr->loc = loc;
                arr->arraySize = n;
                arr->args.push_back(std::move(type));
                type = std::move(arr);
                continue;
            }
            break;
        }
        out = std::move(type);
        return true;
    }

    bool parseEnumBody(std::vector<EnumValueAst>& values) {
        if (!expectPunct("{", "to open the enum body")) return false;
        while (true) {
            std::string doc = collectDocs();
            if (acceptPunct("}")) return true;
            if (peek().kind == Tok::End) {
                error(peek().loc, "unexpected end of file in enum body (missing '}')");
                return false;
            }
            EnumValueAst v;
            v.doc = std::move(doc);
            v.loc = peek().loc;
            if (!expectIdent(v.name, "an enum value name")) return false;
            if (acceptPunct("=")) {
                LiteralAst lit;
                if (!parseLiteral(lit)) return false;
                v.value = std::move(lit);
            }
            while (peek().isPunct("@")) {
                if (!parseAtAttr(v.attrs)) return false;
            }
            values.push_back(std::move(v));
            if (acceptPunct(";") || acceptPunct(",")) continue;
            if (peek().isPunct("}") || peek().newlineBefore || peek().kind == Tok::Doc) continue;
            error(peek().loc, std::format("expected ';', ',' or a line break after enum value '{}', got {}", values.back().name,
                                          describe(peek())));
            return false;
        }
    }

    bool parseVariantBody(std::vector<AltAst>& alts) {
        if (!expectPunct("{", "to open the variant body")) return false;
        while (true) {
            std::string doc = collectDocs();
            if (acceptPunct("}")) return true;
            if (peek().kind == Tok::End) {
                error(peek().loc, "unexpected end of file in variant body (missing '}')");
                return false;
            }
            AltAst alt;
            alt.doc = std::move(doc);
            alt.loc = peek().loc;
            if (!expectIdent(alt.name, "an alternative name")) return false;
            if (acceptPunct("{")) {
                alt.members = std::make_unique<MembersAst>();
                if (!parseMembers(*alt.members, Block::None, false, false)) return false;
                if (!expectPunct("}", "to close the alternative")) return false;
            }
            alts.push_back(std::move(alt));
            if (acceptPunct(";") || acceptPunct(",")) continue;
            if (peek().isPunct("}") || peek().newlineBefore || peek().kind == Tok::Doc) continue;
            error(peek().loc, std::format("expected ';' or a line break after alternative '{}', got {}", alts.back().name,
                                          describe(peek())));
            return false;
        }
    }

    // --- members -------------------------------------------------------------------------------
    bool parseField(FieldAst& f, bool allowDefault = true) {
        f.loc = peek().loc;
        if (!expectIdent(f.name, "a field name")) return false;
        if (!acceptPunct(":")) {
            error(peek().loc, std::format("expected ':' after field name '{}', got {}", f.name, describe(peek())));
            return false;
        }
        if (!parseType(f.type)) return false;
        if (peek().isPunct("=")) {
            if (!allowDefault) {
                error(peek().loc, "parameters cannot have default values");
                return false;
            }
            next();
            LiteralAst lit;
            if (!parseLiteral(lit)) return false;
            f.defaultValue = std::move(lit);
        }
        while (true) {
            if (peek().isPunct("@")) {
                if (!parseAtAttr(f.attrs)) return false;
                continue;
            }
            // Bare attributes on the same line (`lod(near)`), as written in 04's snippets.
            if (peek().kind == Tok::Ident && !peek().newlineBefore && peek(1).isPunct("(")) {
                AttrAst attr;
                attr.loc = peek().loc;
                attr.name = std::string(next().text);
                if (!parseAttrArgs(attr)) return false;
                f.attrs.push_back(std::move(attr));
                continue;
            }
            break;
        }
        return true;
    }

    bool parseParams(std::vector<FieldAst>& params) {
        if (!expectPunct("(", "to open the parameter list")) return false;
        if (acceptPunct(")")) return true;
        while (true) {
            FieldAst p;
            p.doc = collectDocs();
            if (!parseField(p, false)) return false;
            params.push_back(std::move(p));
            if (acceptPunct(",")) continue;
            return expectPunct(")", "to close the parameter list");
        }
    }

    bool parseRpcMember(RpcAst& rpc) {
        next(); // 'rpc'
        rpc.loc = peek().loc;
        if (!expectIdent(rpc.name, "an rpc name")) return false;
        if (!parseHeaderAttrs(rpc.attrs, &rpc.direction, &rpc.directionLoc)) return false;
        if (!parseParams(rpc.params)) return false;
        if (acceptPunct("->")) {
            if (!parseType(rpc.result)) return false;
        }
        if (!parseHeaderAttrs(rpc.attrs, &rpc.direction, &rpc.directionLoc)) return false;
        if (acceptPunct(";")) return true;
        if (peek().isPunct("}") || peek().newlineBefore) return true;
        error(peek().loc, std::format("expected ';' after rpc '{}', got {}", rpc.name, describe(peek())));
        return false;
    }

    /// `fn name(params) [-> type] {@attr} ;` (scriptlib members; attributes may follow on the next lines).
    bool parseFnMember(FnAst& fn) {
        next(); // 'fn'
        fn.loc = peek().loc;
        if (!expectIdent(fn.name, "a function name")) return false;
        if (!parseParams(fn.params)) return false;
        if (acceptPunct("->") && !parseType(fn.result)) return false;
        while (peek().isPunct("@")) {
            if (!parseAtAttr(fn.attrs)) return false;
        }
        return expectPunct(";", std::format("after fn '{}'", fn.name));
    }

    /// Members until '}' (not consumed).
    bool parseMembers(MembersAst& out, Block block, bool allowRpc, bool allowBlocks, bool allowFn = false) {
        while (true) {
            std::string doc = collectDocs();
            const Token& t = peek();
            if (t.isPunct("}")) return true;
            if (t.kind == Tok::End) {
                error(t.loc, "unexpected end of file (missing '}')");
                return false;
            }
            if (t.kind == Tok::Ident && (t.text == "client" || t.text == "server" || t.text == "editor") &&
                peek(1).isPunct("{")) {
                if (block != Block::None) {
                    error(t.loc, "client/server/editor blocks cannot be nested");
                    return false;
                }
                if (!allowBlocks) {
                    error(t.loc, std::format("'{} {{ }}' blocks are not allowed here", t.text));
                    return false;
                }
                const Block inner = t.text == "client" ? Block::Client : t.text == "server" ? Block::Server : Block::Editor;
                next();
                next();
                if (!parseMembers(out, inner, false, false)) return false;
                if (!expectPunct("}", std::format("to close the '{}' block", inner == Block::Client ? "client" : inner == Block::Server ? "server" : "editor"))) return false;
                acceptPunct(";");
                continue;
            }
            if (t.isIdent("fn") && (allowFn || peek(1).kind == Tok::Ident)) {
                if (!allowFn) {
                    error(t.loc, "fn declarations are only allowed inside a scriptlib");
                    return false;
                }
                FnAst fn;
                fn.doc = std::move(doc);
                if (!parseFnMember(fn)) return false;
                out.fns.push_back(std::move(fn));
                continue;
            }
            if (allowFn) {
                error(t.loc, std::format("a scriptlib contains only fn declarations, got {}", describe(t)));
                return false;
            }
            if (t.isIdent("rpc") && peek(1).kind == Tok::Ident) {
                if (!allowRpc) {
                    error(t.loc, "rpc declarations are only allowed inside a service");
                    return false;
                }
                RpcAst rpc;
                rpc.doc = std::move(doc);
                if (!parseRpcMember(rpc)) return false;
                out.rpcs.push_back(std::move(rpc));
                continue;
            }
            FieldAst f;
            f.doc = std::move(doc);
            f.block = block;
            if (!parseField(f)) return false;
            out.fields.push_back(std::move(f));
            if (acceptPunct(";")) continue;
            if (peek().isPunct("}") || peek().newlineBefore || peek().kind == Tok::Doc) continue;
            error(peek().loc, std::format("expected ';' or a line break after field '{}', got {}", out.fields.back().name,
                                          describe(peek())));
            return false;
        }
    }

    // --- declarations --------------------------------------------------------------------------
    bool parseDecl(DeclAst& decl) {
        const Token& kw = peek();
        decl.kindLoc = kw.loc;
        bool found = false;
        if (kw.kind == Tok::Ident) {
            for (const KindEntry& k : kKinds) {
                if (kw.text == k.word) {
                    decl.kind = k.kind;
                    found = true;
                }
            }
        }
        if (!found) {
            error(kw.loc, std::format("expected a declaration (enum, flags, struct, component, relation, record, event, "
                                      "rpc, message, service, viewmodel, formula, const, alias, scriptlib), got {}",
                                      describe(kw)));
            return false;
        }
        next();
        decl.loc = peek().loc;
        if (!expectIdent(decl.name, std::format("a name after '{}'", kw.text))) return false;

        switch (decl.kind) {
        case DeclKindAst::Rpc:
            if (!parseHeaderAttrs(decl.attrs, &decl.direction, &decl.directionLoc)) return false;
            if (!parseParams(decl.params)) return false;
            if (acceptPunct("->") && !parseType(decl.result)) return false;
            if (!parseHeaderAttrs(decl.attrs, &decl.direction, &decl.directionLoc)) return false;
            return expectPunct(";", std::format("after rpc '{}'", decl.name));
        case DeclKindAst::Formula: {
            if (!expectPunct("(", "to open the formula parameters")) return false;
            if (!acceptPunct(")")) {
                while (true) {
                    std::string p;
                    if (!expectIdent(p, "a formula parameter")) return false;
                    if (acceptPunct(":")) {
                        std::unique_ptr<TypeExpr> ignored;
                        if (!parseType(ignored)) return false;
                    }
                    decl.formulaParams.push_back(std::move(p));
                    if (acceptPunct(",")) continue;
                    if (!expectPunct(")", "to close the formula parameters")) return false;
                    break;
                }
            }
            if (!expectPunct("=", std::format("before the body of formula '{}'", decl.name))) return false;
            return parseRawUntilSemicolon(decl.exprText, decl.exprLoc, "formula body");
        }
        case DeclKindAst::Const:
            if (!expectPunct(":", std::format("and a type after const '{}'", decl.name))) return false;
            if (!parseType(decl.base)) return false;
            if (!expectPunct("=", std::format("and a value for const '{}'", decl.name))) return false;
            {
                LiteralAst lit;
                if (!parseLiteral(lit)) return false;
                decl.constValue = std::move(lit);
            }
            return expectPunct(";", std::format("after const '{}'", decl.name));
        case DeclKindAst::Alias:
            if (!acceptPunct("=") && !acceptPunct(":")) {
                error(peek().loc, std::format("expected '=' and a type after alias '{}', got {}", decl.name, describe(peek())));
                return false;
            }
            if (!parseType(decl.aliasTarget)) return false;
            return expectPunct(";", std::format("after alias '{}'", decl.name));
        default: break;
        }

        if (acceptPunct(":")) {
            if (!parseType(decl.base)) return false;
        }
        if (!parseHeaderAttrs(decl.attrs, nullptr, nullptr)) return false;

        if (decl.kind == DeclKindAst::Enum || decl.kind == DeclKindAst::Flags) {
            decl.hasBody = true;
            if (!parseEnumBody(decl.enumValues)) return false;
            acceptPunct(";");
            return true;
        }
        if (peek().isPunct("{")) {
            next();
            decl.hasBody = true;
            const bool isService = decl.kind == DeclKindAst::Service;
            const bool isScriptLib = decl.kind == DeclKindAst::ScriptLib;
            if (!parseMembers(decl.members, Block::None, isService, !isService && !isScriptLib, isScriptLib)) return false;
            if (!expectPunct("}", std::format("to close '{}'", decl.name))) return false;
            acceptPunct(";");
            return true;
        }
        if (acceptPunct(";")) return true;
        // Body-less declarations (relations) may simply end at the next declaration.
        const Token& t = peek();
        if (decl.kind == DeclKindAst::Relation &&
            (t.kind == Tok::End || t.kind == Tok::Doc || (t.newlineBefore && t.kind == Tok::Ident && isDeclKeyword(t.text))))
            return true;
        error(t.loc, std::format("expected '{{' or ';' after '{}', got {}", decl.name, describe(t)));
        return false;
    }

    bool parseRawUntilSemicolon(std::string& out, SourceLoc& loc, std::string_view what) {
        loc = peek().loc;
        const usize first = m_pos;
        int depth = 0;
        while (true) {
            const Token& t = peek();
            if (t.kind == Tok::End) {
                error(loc, std::format("unterminated {} (missing ';')", what));
                return false;
            }
            if (depth == 0 && t.isPunct(";")) break;
            if (t.isPunct("(") || t.isPunct("[") || t.isPunct("{")) ++depth;
            if (t.isPunct(")") || t.isPunct("]") || t.isPunct("}")) --depth;
            next();
        }
        if (m_pos == first) {
            error(loc, std::format("empty {}", what));
            return false;
        }
        out = std::string(m_text.substr(m_tokens[first].offset, m_tokens[m_pos - 1].end - m_tokens[first].offset));
        next(); // ';'
        return true;
    }

    std::string_view m_text;
    DiagnosticEngine& m_diags;
    std::vector<Token> m_tokens;
    usize m_pos = 0;
    bool m_failed = false;
    int m_nesting = 0;
};

} // namespace

bool isDeclKeyword(std::string_view word) noexcept {
    for (const KindEntry& k : kKinds) {
        if (k.word == word) return true;
    }
    return false;
}

std::string_view declKindName(DeclKindAst kind) noexcept {
    for (const KindEntry& k : kKinds) {
        if (k.kind == kind) return k.word;
    }
    return "?";
}

FileAst parseFile(std::string_view text, u32 fileIndex, DiagnosticEngine& diags) {
    Parser p(text, fileIndex, diags);
    return p.run();
}

} // namespace helios::schemac
