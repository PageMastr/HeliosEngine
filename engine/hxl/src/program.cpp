// HXL programs: op table, verifier, canonical encoding, status names. Mirrors
// services/pkg/hxl/program.go.
#include "helios/hxl/program.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

#include "helios/core/hash.h"
#include "syntax.h"

namespace helios::hxl {
namespace {

constexpr u8 kFormatVersion = 1;
constexpr char kMagic[4] = {'H', 'X', 'L', '1'};

struct OpInfo {
    const char* name = nullptr; // nullptr = invalid opcode
    u8 operandBytes = 0;
    u8 pops = 0;
    Type in = Type::Number; // type of every popped value
    bool pushes = false;
    Type out = Type::Number;
    u8 cost = 1;
};

constexpr OpInfo makeInfo(const char* name, u8 operandBytes, u8 pops, Type in, bool pushes, Type out, u8 cost) {
    return OpInfo{name, operandBytes, pops, in, pushes, out, cost};
}

constexpr auto N = Type::Number;
constexpr auto B = Type::Bool;

const OpInfo& opInfo(u8 opcode) noexcept {
    static const OpInfo kInvalid{};
    static const auto table = [] {
        std::array<OpInfo, 256> t{};
        auto set = [&](Op op, OpInfo info) { t[static_cast<u8>(op)] = info; };
        set(Op::Const, makeInfo("Const", 2, 0, N, true, N, 1));
        set(Op::True, makeInfo("True", 0, 0, N, true, B, 1));
        set(Op::False, makeInfo("False", 0, 0, N, true, B, 1));
        set(Op::Attr, makeInfo("Attr", 3, 0, N, true, N, 2));
        set(Op::Field, makeInfo("Field", 3, 0, N, true, N, 2));
        set(Op::Tag, makeInfo("Tag", 3, 0, N, true, B, 2));
        set(Op::Stacks, makeInfo("Stacks", 0, 0, N, true, N, 1));
        set(Op::Level, makeInfo("Level", 0, 0, N, true, N, 1));
        set(Op::Curve, makeInfo("Curve", 2, 1, N, true, N, 4));
        set(Op::Neg, makeInfo("Neg", 0, 1, N, true, N, 1));
        set(Op::Add, makeInfo("Add", 0, 2, N, true, N, 1));
        set(Op::Sub, makeInfo("Sub", 0, 2, N, true, N, 1));
        set(Op::Mul, makeInfo("Mul", 0, 2, N, true, N, 1));
        set(Op::Div, makeInfo("Div", 0, 2, N, true, N, 1));
        set(Op::Pow, makeInfo("Pow", 0, 2, N, true, N, 8));
        set(Op::Min, makeInfo("Min", 0, 2, N, true, N, 1));
        set(Op::Max, makeInfo("Max", 0, 2, N, true, N, 1));
        set(Op::Clamp, makeInfo("Clamp", 0, 3, N, true, N, 1));
        set(Op::Lerp, makeInfo("Lerp", 0, 3, N, true, N, 1));
        set(Op::Sqrt, makeInfo("Sqrt", 0, 1, N, true, N, 2));
        set(Op::Exp, makeInfo("Exp", 0, 1, N, true, N, 8));
        set(Op::Ln, makeInfo("Ln", 0, 1, N, true, N, 8));
        set(Op::Asinh, makeInfo("Asinh", 0, 1, N, true, N, 8));
        set(Op::Abs, makeInfo("Abs", 0, 1, N, true, N, 1));
        set(Op::Floor, makeInfo("Floor", 0, 1, N, true, N, 1));
        set(Op::Ceil, makeInfo("Ceil", 0, 1, N, true, N, 1));
        set(Op::Lt, makeInfo("Lt", 0, 2, N, true, B, 1));
        set(Op::Le, makeInfo("Le", 0, 2, N, true, B, 1));
        set(Op::Gt, makeInfo("Gt", 0, 2, N, true, B, 1));
        set(Op::Ge, makeInfo("Ge", 0, 2, N, true, B, 1));
        set(Op::EqN, makeInfo("EqN", 0, 2, N, true, B, 1));
        set(Op::NeN, makeInfo("NeN", 0, 2, N, true, B, 1));
        set(Op::EqB, makeInfo("EqB", 0, 2, B, true, B, 1));
        set(Op::NeB, makeInfo("NeB", 0, 2, B, true, B, 1));
        set(Op::Not, makeInfo("Not", 0, 1, B, true, B, 1));
        set(Op::JumpIfFalse, makeInfo("JumpIfFalse", 2, 1, B, false, N, 1));
        set(Op::Jump, makeInfo("Jump", 2, 0, N, false, N, 1));
        return t;
    }();
    return table[opcode].name ? table[opcode] : kInvalid;
}

u16 readU16(const u8* p) noexcept { return static_cast<u16>(p[0] | (p[1] << 8)); }

void bad(Diagnostic* diag, std::string message) {
    diag->status = Status::Bytecode;
    diag->line = 0;
    diag->column = 0;
    diag->message = std::move(message);
}

// Size limits (code bytes, stack depth, op count, cost) are E_LIMIT at 1:1, so the compiler can
// report them directly; decode() turns them into E_BYTECODE.
void limitErr(Diagnostic* diag, std::string message) {
    diag->status = Status::Limit;
    diag->line = 1;
    diag->column = 1;
    diag->message = std::move(message);
}

// Checks one symbol table: distinct valid names.
bool checkNames(const std::vector<std::string>& names, bool dotted, const char* what, Diagnostic* diag) {
    if (names.size() > limits::kMaxSymbols) {
        bad(diag, std::format("too many {} symbols", what));
        return false;
    }
    for (usize i = 0; i < names.size(); ++i) {
        const bool ok = dotted ? detail::isDottedName(names[i]) : detail::isIdentifier(names[i]);
        if (!ok) {
            bad(diag, std::format("invalid {} name '{}'", what, names[i]));
            return false;
        }
        for (usize j = 0; j < i; ++j) {
            if (names[j] == names[i]) {
                bad(diag, std::format("duplicate {} name '{}'", what, names[i]));
                return false;
            }
        }
    }
    return true;
}

// First-use order tracking: a canonical program references table entries in increasing order of
// first use and uses every entry.
struct UseOrder {
    u32 next = 0;
    bool use(u32 index) noexcept {
        if (index == next) {
            ++next;
            return true;
        }
        return index < next;
    }
};

} // namespace

std::string_view typeName(Type type) noexcept { return type == Type::Bool ? "bool" : "num"; }

namespace {
constexpr std::string_view kStatusNames[] = {
    "OK",           "E_LEX",   "E_NUMBER",     "E_SYNTAX",      "E_UNKNOWN_NAME",  "E_UNKNOWN_FUNCTION",
    "E_ARITY",      "E_TYPE",  "E_ENTITY_ARG", "E_SYMBOL_ARG",  "E_DUPLICATE_PARAM", "E_LIMIT",
    "E_RESULT_TYPE", "E_BYTECODE", "E_MISSING_INPUT",
};
} // namespace

std::string_view statusName(Status status) noexcept {
    const auto i = static_cast<usize>(status);
    return i < std::size(kStatusNames) ? kStatusNames[i] : std::string_view("E_UNKNOWN");
}

Status statusFromName(std::string_view name) noexcept {
    for (usize i = 0; i < std::size(kStatusNames); ++i) {
        if (kStatusNames[i] == name) return static_cast<Status>(i);
    }
    return Status::Ok;
}

std::string Diagnostic::toString() const {
    return std::format("{} {}:{}: {}", statusName(status), line, column, message);
}

Error toError(const Diagnostic& diag) {
    ErrorCode code = ErrorCode::InvalidArgument;
    switch (diag.status) {
    case Status::Ok: code = ErrorCode::Ok; break;
    case Status::Lex:
    case Status::Number:
    case Status::Syntax: code = ErrorCode::ParseError; break;
    case Status::Limit: code = ErrorCode::LimitExceeded; break;
    case Status::Bytecode: code = ErrorCode::Corrupt; break;
    case Status::MissingInput: code = ErrorCode::NotFound; break;
    default: code = ErrorCode::InvalidArgument; break;
    }
    return Error{code, diag.toString()};
}

u32 opCost(Op op) noexcept { return opInfo(static_cast<u8>(op)).cost; }

std::vector<Program::Ref> Program::references() const {
    std::vector<Ref> refs;
    usize pc = 0;
    while (pc < m_code.size()) {
        const u8 opcode = m_code[pc];
        const OpInfo& info = opInfo(opcode);
        const auto op = static_cast<Op>(opcode);
        if (op == Op::Attr || op == Op::Field || op == Op::Tag) {
            refs.push_back(Ref{op, m_code[pc + 1], readU16(&m_code[pc + 2])});
        }
        pc += 1u + info.operandBytes;
    }
    return refs;
}

Result<Program> ProgramBuilder::finish(u32 maxCost, Diagnostic* diag) {
    Diagnostic local;
    if (!diag) diag = &local;
    Program& p = program;
    if (!p.m_name.empty() && !detail::isIdentifier(p.m_name)) {
        bad(diag, "invalid formula name");
        return toError(*diag);
    }
    if (p.m_params.size() > limits::kMaxParams) {
        bad(diag, "too many parameters");
        return toError(*diag);
    }
    if (!checkNames(p.m_params, false, "parameter", diag) || !checkNames(p.m_attrs, true, "attribute", diag) ||
        !checkNames(p.m_tags, true, "tag", diag) || !checkNames(p.m_fields, false, "field", diag) ||
        !checkNames(p.m_curves, true, "curve", diag)) {
        return toError(*diag);
    }
    if (p.m_constants.size() > limits::kMaxConstants) {
        bad(diag, "too many constants");
        return toError(*diag);
    }
    for (usize i = 0; i < p.m_constants.size(); ++i) {
        const f64 c = p.m_constants[i];
        if (!std::isfinite(c) || std::signbit(c)) {
            bad(diag, "constants must be finite and non-negative");
            return toError(*diag);
        }
        for (usize j = 0; j < i; ++j) {
            if (std::bit_cast<u64>(p.m_constants[j]) == std::bit_cast<u64>(c)) {
                bad(diag, "duplicate constant");
                return toError(*diag);
            }
        }
    }
    if (p.m_code.size() > limits::kMaxCodeBytes) {
        limitErr(diag, std::format("code larger than {} bytes", limits::kMaxCodeBytes));
        return toError(*diag);
    }

    std::vector<Type> stack;
    stack.reserve(16);
    std::map<usize, std::vector<Type>> pending; // jump target -> stack at the jump
    bool reachable = true;
    u32 maxStack = 0;
    u32 cost = 0;
    u32 ops = 0;
    bool usesStacks = false;
    bool usesLevel = false;
    UseOrder constOrder, attrOrder, tagOrder, fieldOrder, curveOrder;
    const auto& code = p.m_code;

    auto join = [&](usize at) -> bool {
        auto it = pending.find(at);
        if (it == pending.end()) return true;
        if (reachable) {
            if (it->second != stack) {
                bad(diag, std::format("stack mismatch at jump target {}", at));
                return false;
            }
        } else {
            stack = it->second;
            reachable = true;
        }
        pending.erase(it);
        return true;
    };
    auto addPending = [&](usize target) -> bool {
        auto [it, inserted] = pending.emplace(target, stack);
        if (!inserted && it->second != stack) {
            bad(diag, std::format("stack mismatch at jump target {}", target));
            return false;
        }
        return true;
    };

    usize pc = 0;
    while (pc < code.size()) {
        if (!join(pc)) return toError(*diag);
        if (!reachable) {
            bad(diag, std::format("unreachable code at {}", pc));
            return toError(*diag);
        }
        const u8 opcode = code[pc];
        const OpInfo& info = opInfo(opcode);
        if (!info.name) {
            bad(diag, std::format("invalid opcode 0x{:02x} at {}", opcode, pc));
            return toError(*diag);
        }
        if (pc + 1 + info.operandBytes > code.size()) {
            bad(diag, std::format("truncated instruction at {}", pc));
            return toError(*diag);
        }
        const u8* operands = &code[pc + 1];
        const auto op = static_cast<Op>(opcode);
        const usize nextPc = pc + 1 + info.operandBytes;
        // Operands.
        switch (op) {
        case Op::Const:
            if (readU16(operands) >= p.m_constants.size() || !constOrder.use(readU16(operands))) {
                bad(diag, std::format("bad constant operand at {}", pc));
                return toError(*diag);
            }
            break;
        case Op::Attr:
        case Op::Field:
        case Op::Tag: {
            const u32 param = operands[0];
            const u32 sym = readU16(operands + 1);
            const auto& table = op == Op::Attr ? p.m_attrs : op == Op::Field ? p.m_fields : p.m_tags;
            UseOrder& order = op == Op::Attr ? attrOrder : op == Op::Field ? fieldOrder : tagOrder;
            if (param >= p.m_params.size() || sym >= table.size() || !order.use(sym)) {
                bad(diag, std::format("bad operand at {}", pc));
                return toError(*diag);
            }
            break;
        }
        case Op::Curve:
            if (readU16(operands) >= p.m_curves.size() || !curveOrder.use(readU16(operands))) {
                bad(diag, std::format("bad curve operand at {}", pc));
                return toError(*diag);
            }
            break;
        case Op::Stacks: usesStacks = true; break;
        case Op::Level: usesLevel = true; break;
        default: break;
        }
        // Stack effect.
        if (stack.size() < info.pops) {
            bad(diag, std::format("stack underflow at {}", pc));
            return toError(*diag);
        }
        for (u32 k = 0; k < info.pops; ++k) {
            if (stack[stack.size() - 1 - k] != info.in) {
                bad(diag, std::format("operand type mismatch at {}", pc));
                return toError(*diag);
            }
        }
        stack.resize(stack.size() - info.pops);
        if (info.pushes) {
            stack.push_back(info.out);
            maxStack = std::max(maxStack, static_cast<u32>(stack.size()));
            if (maxStack > limits::kMaxStack) {
                limitErr(diag, std::format("stack deeper than {}", limits::kMaxStack));
                return toError(*diag);
            }
        }
        cost += info.cost;
        ++ops;
        if (op == Op::JumpIfFalse || op == Op::Jump) {
            const usize target = nextPc + readU16(operands);
            if (target > code.size()) {
                bad(diag, std::format("jump out of range at {}", pc));
                return toError(*diag);
            }
            if (target == nextPc) {
                bad(diag, std::format("empty jump at {}", pc)); // never emitted; keeps encodings canonical
                return toError(*diag);
            }
            if (!addPending(target)) return toError(*diag);
            if (op == Op::Jump) reachable = false;
        }
        pc = nextPc;
    }
    if (!join(code.size())) return toError(*diag);
    if (!pending.empty()) {
        bad(diag, "jump into the middle of an instruction");
        return toError(*diag);
    }
    if (!reachable || stack.size() != 1) {
        bad(diag, "program must leave exactly one value");
        return toError(*diag);
    }
    if (ops > limits::kMaxOps) {
        limitErr(diag, std::format("more than {} ops", limits::kMaxOps));
        return toError(*diag);
    }
    if (constOrder.next != p.m_constants.size() || attrOrder.next != p.m_attrs.size() ||
        tagOrder.next != p.m_tags.size() || fieldOrder.next != p.m_fields.size() ||
        curveOrder.next != p.m_curves.size()) {
        bad(diag, "unused table entries");
        return toError(*diag);
    }
    if (cost > maxCost) {
        limitErr(diag, std::format("evaluation cost {} exceeds the budget {}", cost, maxCost));
        return toError(*diag);
    }
    p.m_resultType = stack[0];
    p.m_maxStack = maxStack;
    p.m_cost = cost;
    p.m_usesStacks = usesStacks;
    p.m_usesLevel = usesLevel;
    return std::move(p);
}

// ---- encoding ------------------------------------------------------------------------------------

namespace {

class Writer {
public:
    std::vector<u8> bytes;
    void u8v(u8 v) { bytes.push_back(v); }
    void u16v(u16 v) {
        bytes.push_back(static_cast<u8>(v));
        bytes.push_back(static_cast<u8>(v >> 8));
    }
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64v(u64 v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void str(const std::string& s) {
        u16v(static_cast<u16>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    void strs(const std::vector<std::string>& v) {
        u16v(static_cast<u16>(v.size()));
        for (const auto& s : v) str(s);
    }
};

class Reader {
public:
    explicit Reader(std::span<const u8> b) : m_b(b) {}
    bool ok() const noexcept { return m_ok; }
    bool atEnd() const noexcept { return m_pos == m_b.size(); }
    u8 u8v() {
        if (!need(1)) return 0;
        return m_b[m_pos++];
    }
    u16 u16v() {
        if (!need(2)) return 0;
        const u16 v = readU16(&m_b[m_pos]);
        m_pos += 2;
        return v;
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(m_b[m_pos + i]) << (8 * i);
        m_pos += 4;
        return v;
    }
    u64 u64v() {
        if (!need(8)) return 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<u64>(m_b[m_pos + i]) << (8 * i);
        m_pos += 8;
        return v;
    }
    std::string str() {
        const u16 n = u16v();
        if (n > limits::kMaxNameBytes || !need(n)) {
            m_ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(&m_b[m_pos]), n);
        m_pos += n;
        return s;
    }
    bool strs(std::vector<std::string>& out, u32 maxCount) {
        const u16 n = u16v();
        if (n > maxCount) m_ok = false;
        if (!m_ok) return false;
        out.clear();
        for (u16 i = 0; i < n && m_ok; ++i) out.push_back(str());
        return m_ok;
    }
    bool bytes(std::vector<u8>& out, u32 n) {
        if (!need(n)) return false;
        out.assign(m_b.begin() + static_cast<isize>(m_pos), m_b.begin() + static_cast<isize>(m_pos + n));
        m_pos += n;
        return true;
    }

private:
    bool need(usize n) {
        if (!m_ok || m_b.size() - m_pos < n) {
            m_ok = false;
            return false;
        }
        return true;
    }
    std::span<const u8> m_b;
    usize m_pos = 0;
    bool m_ok = true;
};

} // namespace

std::vector<u8> Program::encode() const {
    Writer w;
    w.bytes.reserve(64 + m_code.size() + m_constants.size() * 8);
    for (char c : kMagic) w.u8v(static_cast<u8>(c));
    w.u8v(kFormatVersion);
    w.u8v(static_cast<u8>(m_resultType));
    w.u16v(static_cast<u16>(m_maxStack));
    w.u32v(m_cost);
    w.str(m_name);
    w.u8v(static_cast<u8>(m_params.size()));
    for (const auto& s : m_params) w.str(s);
    w.u16v(static_cast<u16>(m_constants.size()));
    for (f64 c : m_constants) w.u64v(std::bit_cast<u64>(c));
    w.strs(m_attrs);
    w.strs(m_tags);
    w.strs(m_fields);
    w.strs(m_curves);
    w.u32v(static_cast<u32>(m_code.size()));
    w.bytes.insert(w.bytes.end(), m_code.begin(), m_code.end());
    return std::move(w.bytes);
}

Result<Program> Program::decode(std::span<const u8> bytes, Diagnostic* diag) {
    Diagnostic local;
    if (!diag) diag = &local;
    Reader r(bytes);
    for (char c : kMagic) {
        if (r.u8v() != static_cast<u8>(c)) {
            bad(diag, "not HXL bytecode (bad magic)");
            return toError(*diag);
        }
    }
    const u8 version = r.u8v();
    if (r.ok() && version != kFormatVersion) {
        bad(diag, std::format("unsupported bytecode version {}", version));
        return toError(*diag);
    }
    ProgramBuilder b;
    Program& p = b.program;
    const u8 resultType = r.u8v();
    const u16 maxStack = r.u16v();
    const u32 cost = r.u32v();
    p.m_name = r.str();
    const u8 paramCount = r.u8v();
    if (paramCount > limits::kMaxParams) {
        bad(diag, "too many parameters");
        return toError(*diag);
    }
    for (u8 i = 0; i < paramCount && r.ok(); ++i) p.m_params.push_back(r.str());
    const u16 constCount = r.u16v();
    if (constCount > limits::kMaxConstants) {
        bad(diag, "too many constants");
        return toError(*diag);
    }
    for (u16 i = 0; i < constCount && r.ok(); ++i) p.m_constants.push_back(std::bit_cast<f64>(r.u64v()));
    r.strs(p.m_attrs, limits::kMaxSymbols);
    r.strs(p.m_tags, limits::kMaxSymbols);
    r.strs(p.m_fields, limits::kMaxSymbols);
    r.strs(p.m_curves, limits::kMaxSymbols);
    const u32 codeSize = r.u32v();
    if (r.ok() && codeSize > limits::kMaxCodeBytes) {
        bad(diag, "code too large");
        return toError(*diag);
    }
    if (r.ok()) r.bytes(p.m_code, codeSize);
    if (!r.ok()) {
        bad(diag, "truncated bytecode");
        return toError(*diag);
    }
    if (!r.atEnd()) {
        bad(diag, "trailing bytes after the bytecode");
        return toError(*diag);
    }
    if (resultType > 1) {
        bad(diag, "invalid result type");
        return toError(*diag);
    }
    auto verified = b.finish(limits::kMaxCost, diag);
    if (!verified) {
        if (diag->status == Status::Limit) {
            // Not source related: E_BYTECODE at 0:0, like every other decode failure (and Go).
            diag->status = Status::Bytecode;
            diag->line = 0;
            diag->column = 0;
        }
        return toError(*diag);
    }
    Program out = std::move(*verified);
    if (static_cast<u8>(out.m_resultType) != resultType || out.m_maxStack != maxStack || out.m_cost != cost) {
        bad(diag, "header does not match the code (result type, stack or cost)");
        return toError(*diag);
    }
    return out;
}

u64 Program::hash() const {
    const std::vector<u8> bytes = encode();
    u64 h = kFnv1a64Offset;
    for (u8 b : bytes) {
        h ^= b;
        h *= kFnv1a64Prime;
    }
    return h;
}

std::string Program::disassemble() const {
    std::string out = std::format("; {}({}) -> {}  stack={} cost={}\n", m_name.empty() ? "<expr>" : m_name,
                                  [&] {
                                      std::string s;
                                      for (usize i = 0; i < m_params.size(); ++i) {
                                          if (i) s += ", ";
                                          s += m_params[i];
                                      }
                                      return s;
                                  }(),
                                  typeName(m_resultType), m_maxStack, m_cost);
    usize pc = 0;
    while (pc < m_code.size()) {
        const auto op = static_cast<Op>(m_code[pc]);
        const OpInfo& info = opInfo(m_code[pc]);
        std::string line = std::format("{:04}  {}", pc, info.name ? info.name : "?");
        const u8* o = &m_code[pc + 1];
        switch (op) {
        case Op::Const: line += std::format(" {}  ; {}", readU16(o), m_constants[readU16(o)]); break;
        case Op::Attr: line += std::format(" {}.{}", m_params[o[0]], m_attrs[readU16(o + 1)]); break;
        case Op::Field: line += std::format(" {}.{}", m_params[o[0]], m_fields[readU16(o + 1)]); break;
        case Op::Tag: line += std::format(" {}.{}", m_params[o[0]], m_tags[readU16(o + 1)]); break;
        case Op::Curve: line += std::format(" {}", m_curves[readU16(o)]); break;
        case Op::JumpIfFalse:
        case Op::Jump: line += std::format(" -> {:04}", pc + 3 + readU16(o)); break;
        default: break;
        }
        out += line;
        out += '\n';
        pc += 1u + info.operandBytes;
    }
    return out;
}

} // namespace helios::hxl
