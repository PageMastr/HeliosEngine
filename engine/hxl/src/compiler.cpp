// HXL type checker and code generator. Mirrors services/pkg/hxl/compiler.go: the same checks in
// the same order (children left to right, then the node), the same symbol/constant registration
// order and the same op sequence, so both produce byte-identical bytecode.
#include "helios/hxl/compiler.h"

#include <bit>
#include <format>
#include <string>
#include <utility>

#include "helios/core/platform.h"
#include "syntax.h"

namespace helios::hxl {
namespace {

using detail::Ast;
using detail::Node;
using detail::NodeKind;
using detail::Tok;

enum class Builtin : u8 { Attr, Tag, Curve, Stacks, Level, Select, Min, Max, Clamp, Lerp, Pow, Sqrt, Exp, Ln, Asinh, Abs, Floor, Ceil };

constexpr u32 kVariadic = 0xFFFFFFFFu;

struct BuiltinInfo {
    std::string_view name;
    Builtin id;
    u32 minArgs;
    u32 maxArgs; // kVariadic = unbounded
};

constexpr BuiltinInfo kBuiltins[] = {
    {"attr", Builtin::Attr, 2, 2},     {"tag", Builtin::Tag, 2, 2},       {"curve", Builtin::Curve, 2, 2},
    {"stacks", Builtin::Stacks, 0, 0}, {"level", Builtin::Level, 0, 0},   {"select", Builtin::Select, 3, 3},
    {"min", Builtin::Min, 2, kVariadic}, {"max", Builtin::Max, 2, kVariadic}, {"clamp", Builtin::Clamp, 3, 3},
    {"lerp", Builtin::Lerp, 3, 3},     {"pow", Builtin::Pow, 2, 2},       {"sqrt", Builtin::Sqrt, 1, 1},
    {"exp", Builtin::Exp, 1, 1},       {"ln", Builtin::Ln, 1, 1},         {"asinh", Builtin::Asinh, 1, 1},
    {"abs", Builtin::Abs, 1, 1},       {"floor", Builtin::Floor, 1, 1},   {"ceil", Builtin::Ceil, 1, 1},
};

const BuiltinInfo* findBuiltin(std::string_view name) noexcept {
    for (const auto& b : kBuiltins) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

std::string joinPath(const std::vector<std::string>& segments) {
    std::string s;
    for (usize i = 0; i < segments.size(); ++i) {
        if (i) s += '.';
        s += segments[i];
    }
    return s;
}

class Gen {
public:
    Gen(const Ast& ast, std::vector<std::string> params, Diagnostic& diag)
        : m_ast(ast), m_diag(diag), m_params(std::move(params)) {}

    bool run(Type& resultType) { return emit(m_ast.root, resultType); }

    std::vector<u8>& code() { return m_code; }
    std::vector<f64>& constants() { return m_constants; }
    std::vector<std::string>& attrs() { return m_attrs; }
    std::vector<std::string>& tags() { return m_tags; }
    std::vector<std::string>& fields() { return m_fields; }
    std::vector<std::string>& curves() { return m_curves; }

private:
    // Diagnostics are formatted out of line: emit() recurses once per tree level (up to
    // limits::kMaxAstDepth), and formatting in those frames would multiply their size (the README
    // records the measured worst-case stack).
    bool fail(Status status, u32 line, u32 column, std::string message) {
        m_diag.status = status;
        m_diag.line = line;
        m_diag.column = column;
        m_diag.message = std::move(message);
        return false;
    }
    template <class... Args>
    HELIOS_NOINLINE bool failAt(const Node& n, Status status, std::format_string<Args...> fmt, Args&&... args) {
        return fail(status, n.line, n.column, std::format(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    HELIOS_NOINLINE bool failAtStart(const Node& n, Status status, std::format_string<Args...> fmt, Args&&... args) {
        return fail(status, n.startLine, n.startColumn, std::format(fmt, std::forward<Args>(args)...));
    }

    // Which operand emitNumber() checks (for its message).
    enum class Operand : u8 { CurveInput, MinMax, Argument, SoleArgument };

    void op(Op o) { m_code.push_back(static_cast<u8>(o)); }
    void u16v(u32 v) {
        m_code.push_back(static_cast<u8>(v));
        m_code.push_back(static_cast<u8>(v >> 8));
    }
    // Emits a jump with a placeholder offset; returns the operand position for patch().
    usize jump(Op o) {
        op(o);
        const usize at = m_code.size();
        u16v(0);
        return at;
    }
    // Offsets beyond u16 only occur in code larger than limits::kMaxCodeBytes, which finish()
    // rejects before looking at jumps.
    void patch(usize at) {
        const usize offset = m_code.size() - (at + 2);
        m_code[at] = static_cast<u8>(offset);
        m_code[at + 1] = static_cast<u8>(offset >> 8);
    }

    int paramIndex(const std::string& name) const {
        for (usize i = 0; i < m_params.size(); ++i) {
            if (m_params[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    bool symbol(std::vector<std::string>& table, const std::string& name, const char* what, const Node& at, u32& out) {
        if (name.size() > limits::kMaxNameBytes) {
            return failAtStart(at, Status::Limit, "{} name longer than {} bytes", what, limits::kMaxNameBytes);
        }
        for (usize i = 0; i < table.size(); ++i) {
            if (table[i] == name) {
                out = static_cast<u32>(i);
                return true;
            }
        }
        if (table.size() >= limits::kMaxSymbols) {
            return failAtStart(at, Status::Limit, "more than {} {} names", limits::kMaxSymbols, what);
        }
        table.push_back(name);
        out = static_cast<u32>(table.size() - 1);
        return true;
    }

    bool constant(f64 v, const Node& at) {
        const u64 bits = std::bit_cast<u64>(v);
        for (usize i = 0; i < m_constants.size(); ++i) {
            if (std::bit_cast<u64>(m_constants[i]) == bits) {
                op(Op::Const);
                u16v(static_cast<u32>(i));
                return true;
            }
        }
        if (m_constants.size() >= limits::kMaxConstants) {
            return failAt(at, Status::Limit, "more than {} distinct constants", limits::kMaxConstants);
        }
        m_constants.push_back(v);
        op(Op::Const);
        u16v(static_cast<u32>(m_constants.size() - 1));
        return true;
    }

    // Emits an operand of `call` that must be a number.
    bool emitNumber(u32 index, Operand what, const Node& call) {
        Type t{};
        if (!emit(index, t)) return false;
        if (t != Type::Number) return notANumber(m_ast.nodes[index], what, call);
        return true;
    }
    HELIOS_NOINLINE bool notANumber(const Node& operand, Operand what, const Node& call) {
        switch (what) {
        case Operand::CurveInput:
            return failAtStart(operand, Status::TypeMismatch, "the curve input must be a number, found bool");
        case Operand::MinMax:
            return failAtStart(operand, Status::TypeMismatch, "an argument of min()/max() must be a number, found bool");
        case Operand::Argument:
            return failAtStart(operand, Status::TypeMismatch, "an argument of {}() must be a number, found bool", call.name);
        case Operand::SoleArgument: break;
        }
        return failAtStart(operand, Status::TypeMismatch, "the argument of {}() must be a number, found bool", call.name);
    }
    HELIOS_NOINLINE bool arityError(const Node& n, const BuiltinInfo& fn, u32 argc) {
        const std::string expected = fn.maxArgs == kVariadic      ? std::format("at least {}", fn.minArgs)
                                     : fn.minArgs == fn.maxArgs ? std::format("{}", fn.minArgs)
                                                                : std::format("{} to {}", fn.minArgs, fn.maxArgs);
        return failAt(n, Status::Arity, "{}() takes {} argument(s), got {}", n.name, expected, argc);
    }

    bool emitPath(const Node& n, Type& out) {
        const auto& segs = n.segments;
        const int p = paramIndex(segs[0]);
        if (segs.size() == 1) {
            if (p >= 0) {
                return failAt(n, Status::TypeMismatch, "parameter '{}' is an entity: read a field ({}.name), attr() or tag()",
                              segs[0], segs[0]);
            }
            return failAt(n, Status::UnknownName, "unknown name '{}'", segs[0]);
        }
        if (p < 0) return failAt(n, Status::UnknownName, "unknown parameter '{}'", segs[0]);
        if (segs.size() > 2) {
            return failAt(n, Status::Syntax, "context fields have one level ('{}.{}')", segs[0], segs[1]);
        }
        u32 sym = 0;
        if (!symbol(m_fields, segs[1], "field", n, sym)) return false;
        op(Op::Field);
        m_code.push_back(static_cast<u8>(p));
        u16v(sym);
        out = Type::Number;
        return true;
    }

    bool emitEntityRef(const Node& call, Op o, const char* fn) {
        const Node& e = m_ast.nodes[call.children[0]];
        if (e.kind != NodeKind::Path || e.segments.size() != 1) {
            return failAtStart(e, Status::EntityArg, "the first argument of {}() must be a parameter name", fn);
        }
        const int p = paramIndex(e.segments[0]);
        if (p < 0) return failAtStart(e, Status::UnknownName, "unknown parameter '{}'", e.segments[0]);
        const Node& s = m_ast.nodes[call.children[1]];
        if (s.kind != NodeKind::Path) {
            return failAtStart(s, Status::SymbolArg, "the second argument of {}() must be a {} name", fn,
                               o == Op::Tag ? "tag" : "attribute");
        }
        u32 sym = 0;
        if (!symbol(o == Op::Tag ? m_tags : m_attrs, joinPath(s.segments), o == Op::Tag ? "tag" : "attribute", s, sym)) {
            return false;
        }
        op(o);
        m_code.push_back(static_cast<u8>(p));
        u16v(sym);
        return true;
    }

    bool emitCall(const Node& n, Type& out) {
        const BuiltinInfo* fn = findBuiltin(n.name);
        if (!fn) return failAt(n, Status::UnknownFunction, "unknown function '{}'", n.name);
        const auto argc = static_cast<u32>(n.children.size());
        if (argc < fn->minArgs || argc > fn->maxArgs) return arityError(n, *fn, argc);
        const auto& args = n.children;
        switch (fn->id) {
        case Builtin::Attr:
            if (!emitEntityRef(n, Op::Attr, "attr")) return false;
            out = Type::Number;
            return true;
        case Builtin::Tag:
            if (!emitEntityRef(n, Op::Tag, "tag")) return false;
            out = Type::Bool;
            return true;
        case Builtin::Curve: {
            const Node& s = m_ast.nodes[args[0]];
            if (s.kind != NodeKind::Path) {
                return failAtStart(s, Status::SymbolArg, "the first argument of curve() must be a curve name");
            }
            if (!emitNumber(args[1], Operand::CurveInput, n)) return false;
            u32 sym = 0;
            if (!symbol(m_curves, joinPath(s.segments), "curve", s, sym)) return false;
            op(Op::Curve);
            u16v(sym);
            out = Type::Number;
            return true;
        }
        case Builtin::Stacks:
            op(Op::Stacks);
            out = Type::Number;
            return true;
        case Builtin::Level:
            op(Op::Level);
            out = Type::Number;
            return true;
        case Builtin::Select: {
            Type c{};
            if (!emit(args[0], c)) return false;
            if (c != Type::Bool) {
                return failAtStart(m_ast.nodes[args[0]], Status::TypeMismatch, "the condition of select() must be bool");
            }
            const usize elseJump = jump(Op::JumpIfFalse);
            Type a{};
            if (!emit(args[1], a)) return false;
            const usize endJump = jump(Op::Jump);
            patch(elseJump);
            Type b{};
            if (!emit(args[2], b)) return false;
            if (b != a) {
                return failAtStart(m_ast.nodes[args[2]], Status::TypeMismatch, "select() branches differ in type ({} and {})",
                                   typeName(a), typeName(b));
            }
            patch(endJump);
            out = a;
            return true;
        }
        case Builtin::Min:
        case Builtin::Max: {
            const Op o = fn->id == Builtin::Min ? Op::Min : Op::Max;
            if (!emitNumber(args[0], Operand::MinMax, n)) return false;
            for (usize i = 1; i < args.size(); ++i) {
                if (!emitNumber(args[i], Operand::MinMax, n)) return false;
                op(o);
            }
            out = Type::Number;
            return true;
        }
        case Builtin::Clamp:
        case Builtin::Lerp:
        case Builtin::Pow: {
            for (u32 a : args) {
                if (!emitNumber(a, Operand::Argument, n)) return false;
            }
            op(fn->id == Builtin::Clamp ? Op::Clamp : fn->id == Builtin::Lerp ? Op::Lerp : Op::Pow);
            out = Type::Number;
            return true;
        }
        default: {
            if (!emitNumber(args[0], Operand::SoleArgument, n)) return false;
            static constexpr Op kUnary[] = {Op::Sqrt, Op::Exp, Op::Ln, Op::Asinh, Op::Abs, Op::Floor, Op::Ceil};
            op(kUnary[static_cast<u8>(fn->id) - static_cast<u8>(Builtin::Sqrt)]);
            out = Type::Number;
            return true;
        }
        }
    }

    bool emitBinary(const Node& n, Type& out) {
        const u32 l = n.children[0];
        const u32 r = n.children[1];
        if (n.op == Tok::AndAnd || n.op == Tok::OrOr) {
            const char* opText = n.op == Tok::AndAnd ? "&&" : "||";
            Type tl{};
            if (!emit(l, tl)) return false;
            if (tl != Type::Bool) return failAt(n, Status::TypeMismatch, "operands of '{}' must be bool", opText);
            const usize skip = jump(Op::JumpIfFalse);
            if (n.op == Tok::AndAnd) {
                Type tr{};
                if (!emit(r, tr)) return false;
                if (tr != Type::Bool) return failAt(n, Status::TypeMismatch, "operands of '&&' must be bool");
                const usize end = jump(Op::Jump);
                patch(skip);
                op(Op::False);
                patch(end);
            } else {
                op(Op::True);
                const usize end = jump(Op::Jump);
                patch(skip);
                Type tr{};
                if (!emit(r, tr)) return false;
                if (tr != Type::Bool) return failAt(n, Status::TypeMismatch, "operands of '||' must be bool");
                patch(end);
            }
            out = Type::Bool;
            return true;
        }
        Type tl{};
        Type tr{};
        if (!emit(l, tl)) return false;
        if (!emit(r, tr)) return false;
        switch (n.op) {
        case Tok::EqEq:
        case Tok::NotEq:
            if (tl != tr) {
                return failAt(n, Status::TypeMismatch, "cannot compare {} with {}", typeName(tl), typeName(tr));
            }
            if (tl == Type::Number) {
                op(n.op == Tok::EqEq ? Op::EqN : Op::NeN);
            } else {
                op(n.op == Tok::EqEq ? Op::EqB : Op::NeB);
            }
            out = Type::Bool;
            return true;
        default: break;
        }
        if (tl != Type::Number || tr != Type::Number) {
            return failAt(n, Status::TypeMismatch, "operands of {} must be numbers", detail::tokName(n.op));
        }
        switch (n.op) {
        case Tok::Plus: op(Op::Add); out = Type::Number; return true;
        case Tok::Minus: op(Op::Sub); out = Type::Number; return true;
        case Tok::Star: op(Op::Mul); out = Type::Number; return true;
        case Tok::Slash: op(Op::Div); out = Type::Number; return true;
        case Tok::Caret: op(Op::Pow); out = Type::Number; return true;
        case Tok::Lt: op(Op::Lt); out = Type::Bool; return true;
        case Tok::Le: op(Op::Le); out = Type::Bool; return true;
        case Tok::Gt: op(Op::Gt); out = Type::Bool; return true;
        case Tok::Ge: op(Op::Ge); out = Type::Bool; return true;
        default: return failAt(n, Status::Syntax, "unsupported operator");
        }
    }

    bool emit(u32 index, Type& out) {
        const Node& n = m_ast.nodes[index];
        switch (n.kind) {
        case NodeKind::Number:
            out = Type::Number;
            return constant(n.number, n);
        case NodeKind::Bool:
            op(n.boolean ? Op::True : Op::False);
            out = Type::Bool;
            return true;
        case NodeKind::Path: return emitPath(n, out);
        case NodeKind::Call: return emitCall(n, out);
        case NodeKind::Unary: {
            Type t{};
            if (!emit(n.children[0], t)) return false;
            if (n.op == Tok::Minus) {
                if (t != Type::Number) return failAt(n, Status::TypeMismatch, "operand of unary '-' must be a number");
                op(Op::Neg);
                out = Type::Number;
            } else {
                if (t != Type::Bool) return failAt(n, Status::TypeMismatch, "operand of '!' must be bool");
                op(Op::Not);
                out = Type::Bool;
            }
            return true;
        }
        case NodeKind::Binary: return emitBinary(n, out);
        }
        return failAt(n, Status::Syntax, "unknown node");
    }

    const Ast& m_ast;
    Diagnostic& m_diag;
    std::vector<std::string> m_params;
    std::vector<u8> m_code;
    std::vector<f64> m_constants;
    std::vector<std::string> m_attrs;
    std::vector<std::string> m_tags;
    std::vector<std::string> m_fields;
    std::vector<std::string> m_curves;
};

} // namespace

// Fills a Program's private tables (Program befriends this class); finish() then verifies it.
class CompilerAccess {
public:
    static Program make(std::string name, std::vector<std::string> params, std::vector<f64> consts,
                        std::vector<std::string> attrs, std::vector<std::string> tags, std::vector<std::string> fields,
                        std::vector<std::string> curves, std::vector<u8> code) {
        Program p;
        p.m_name = std::move(name);
        p.m_params = std::move(params);
        p.m_constants = std::move(consts);
        p.m_attrs = std::move(attrs);
        p.m_tags = std::move(tags);
        p.m_fields = std::move(fields);
        p.m_curves = std::move(curves);
        p.m_code = std::move(code);
        return p;
    }
};

Result<Program> compile(std::string_view source, const CompileOptions& options, Diagnostic* diagOut) {
    Diagnostic local;
    Diagnostic& diag = diagOut ? *diagOut : local;
    diag = Diagnostic{};
    auto failWith = [&](Status s, u32 line, u32 col, std::string msg) -> Result<Program> {
        diag.status = s;
        diag.line = line;
        diag.column = col;
        diag.message = std::move(msg);
        return toError(diag);
    };
    if (source.size() > limits::kMaxSourceBytes) {
        return failWith(Status::Limit, 1, 1, std::format("source longer than {} bytes", limits::kMaxSourceBytes));
    }
    std::vector<detail::Token> tokens;
    if (!detail::lex(source, tokens, diag)) return toError(diag);
    Ast ast;
    if (!detail::parse(tokens, ast, diag)) return toError(diag);
    std::vector<std::string> params;
    if (ast.isFormula) {
        params = ast.params;
    } else {
        for (usize i = 0; i < options.params.size(); ++i) {
            // Host parameter names end up in the bytecode, whose verifier accepts only identifiers.
            if (!detail::isIdentifier(options.params[i])) {
                return failWith(Status::Syntax, 1, 1, std::format("invalid parameter name '{}'", options.params[i]));
            }
            for (usize j = 0; j < i; ++j) {
                if (options.params[i] == options.params[j]) {
                    return failWith(Status::DuplicateParam, 1, 1, std::format("duplicate parameter '{}'", options.params[i]));
                }
            }
        }
        if (options.params.size() > limits::kMaxParams) {
            return failWith(Status::Limit, 1, 1, std::format("more than {} parameters", limits::kMaxParams));
        }
        params = options.params;
    }
    Gen gen(ast, params, diag);
    Type type{};
    if (!gen.run(type)) return toError(diag);
    if (options.expectedType && *options.expectedType != type) {
        const Node& root = ast.nodes[ast.root];
        return failWith(Status::ResultType, root.startLine, root.startColumn,
                        std::format("expected a {} expression, found {}", typeName(*options.expectedType), typeName(type)));
    }
    Program p = CompilerAccess::make(ast.isFormula ? ast.formulaName : std::string(), std::move(params),
                                     std::move(gen.constants()), std::move(gen.attrs()), std::move(gen.tags()),
                                     std::move(gen.fields()), std::move(gen.curves()), std::move(gen.code()));
    ProgramBuilder builder;
    builder.program = std::move(p);
    const u32 maxCost = options.maxCost == 0 || options.maxCost > limits::kMaxCost ? limits::kMaxCost : options.maxCost;
    auto result = builder.finish(maxCost, &diag);
    if (!result && diag.status != Status::Limit) {
        // A verifier rejection of compiler output is an internal error; report it as such.
        diag.message = "internal compiler error: " + diag.message;
    }
    return result;
}

} // namespace helios::hxl
