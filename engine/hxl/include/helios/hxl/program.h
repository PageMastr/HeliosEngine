#pragma once
// HXL (Helios eXpression Language, 06 §1.2): compiled programs, status codes and the bytecode format.
//
// A Program is immutable, verified stack bytecode plus the symbol tables it references (attribute,
// tag, context-field and curve names) and its parameter list. The same bytes are produced by the Go
// compiler (services/pkg/hxl), and the shared corpus (tests/corpus/hxl) pins the FNV-1a hash of the
// encoding, so both compilers must stay op-for-op identical. There is no constant folding,
// reassociation or fusion anywhere: every op rounds to f64 exactly once (06 §1.2 float rules).
//
// Bytecode is forward-only (no loops): evaluation executes each op at most once, so the static
// `cost()` bounds the run time of every evaluation. decode() verifies untrusted bytes completely
// (operand ranges, stack depth and types at every op and jump target, canonical encoding), so a
// decoded Program is as safe to run as a compiled one.
//
// Threading: Program is a value type; const methods are safe to call concurrently.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::hxl {

/// Static type of an HXL value. Booleans are carried on the VM stack as 0.0 / 1.0.
enum class Type : u8 { Number = 0, Bool = 1 };

/// "num" / "bool".
std::string_view typeName(Type type) noexcept;

/// Stable status codes. The names ("E_SYNTAX", ...) are shared with Go and the corpus; never
/// renumber or rename an existing code.
enum class Status : u16 {
    Ok = 0,
    Lex,             ///< E_LEX: character that starts no token
    Number,          ///< E_NUMBER: malformed or out-of-range number literal
    Syntax,          ///< E_SYNTAX: unexpected token
    UnknownName,     ///< E_UNKNOWN_NAME: identifier that is not a parameter
    UnknownFunction, ///< E_UNKNOWN_FUNCTION
    Arity,           ///< E_ARITY: wrong argument count
    TypeMismatch,    ///< E_TYPE
    EntityArg,       ///< E_ENTITY_ARG: attr()/tag() need a parameter name as first argument
    SymbolArg,       ///< E_SYMBOL_ARG: attr()/tag()/curve() need a (dotted) name argument
    DuplicateParam,  ///< E_DUPLICATE_PARAM
    Limit,           ///< E_LIMIT: a size, depth or cost limit was exceeded
    ResultType,      ///< E_RESULT_TYPE: the program's type differs from the expected one
    Bytecode,        ///< E_BYTECODE: malformed or unverifiable bytecode
    MissingInput,    ///< E_MISSING_INPUT: the environment has no value for an input
};

/// "E_SYNTAX", ... ("OK" for Status::Ok).
std::string_view statusName(Status status) noexcept;
/// Inverse of statusName(); Status::Ok if unknown.
Status statusFromName(std::string_view name) noexcept;

/// A compile/load/eval failure with a 1-based source position (0:0 when not source related).
struct Diagnostic {
    Status status = Status::Ok;
    u32 line = 0;
    u32 column = 0; ///< byte column
    std::string message;

    /// "E_TYPE 1:7: operands of '+' must be numbers".
    std::string toString() const;
};

/// Opcodes. Operands are little-endian and follow the opcode byte.
enum class Op : u8 {
    Const = 0x01,  ///< u16 constant index
    True = 0x02,
    False = 0x03,
    Attr = 0x04,   ///< u8 param, u16 attribute symbol
    Field = 0x05,  ///< u8 param, u16 field symbol
    Tag = 0x06,    ///< u8 param, u16 tag symbol
    Stacks = 0x07,
    Level = 0x08,
    Curve = 0x09,  ///< u16 curve symbol; pops x
    Neg = 0x10,
    Add = 0x11,
    Sub = 0x12,
    Mul = 0x13,
    Div = 0x14,
    Pow = 0x15,
    Min = 0x16,
    Max = 0x17,
    Clamp = 0x18,  ///< x lo hi
    Lerp = 0x19,   ///< a b t
    Sqrt = 0x1A,
    Exp = 0x1B,
    Ln = 0x1C,
    Asinh = 0x1D,
    Abs = 0x1E,
    Floor = 0x1F,
    Ceil = 0x20,
    Lt = 0x30,
    Le = 0x31,
    Gt = 0x32,
    Ge = 0x33,
    EqN = 0x34,
    NeN = 0x35,
    EqB = 0x36,
    NeB = 0x37,
    Not = 0x38,
    JumpIfFalse = 0x40, ///< u16 forward offset from the next op; pops the condition
    Jump = 0x41,        ///< u16 forward offset from the next op
};

/// Limits shared with Go (exceeding one is E_LIMIT, or E_BYTECODE when decoding).
namespace limits {
inline constexpr u32 kMaxSourceBytes = 65536;
inline constexpr u32 kMaxParseDepth = 128;  ///< nested parentheses/calls/prefix operators
inline constexpr u32 kMaxAstDepth = 256;    ///< expression tree depth (long operator chains)
inline constexpr u32 kMaxParams = 16;
inline constexpr u32 kMaxOps = 4096;
inline constexpr u32 kMaxCodeBytes = 16384;
inline constexpr u32 kMaxConstants = 1024;
inline constexpr u32 kMaxSymbols = 256;     ///< per symbol table
inline constexpr u32 kMaxNameBytes = 256;
/// Longest number literal. Go's strconv.ParseFloat is not correctly rounded beyond 800 significant
/// digits, so an unbounded literal could compile to different constants in C++ and Go.
inline constexpr u32 kMaxNumberBytes = 100;
inline constexpr u32 kMaxStack = 256;
inline constexpr u32 kMaxCost = 4096;       ///< default and maximum evaluation cost
} // namespace limits

/// Static evaluation cost of one op (the budget unit of limits::kMaxCost).
u32 opCost(Op op) noexcept;

/// A verified HXL program. Built by compile() or decode(); immutable afterwards.
class Program {
public:
    Program() = default;

    /// Formula name ("TurretHitChance"); empty for a bare expression.
    const std::string& name() const noexcept { return m_name; }
    /// Parameter names, in declaration order (the `param` operand indexes this list).
    const std::vector<std::string>& params() const noexcept { return m_params; }
    Type resultType() const noexcept { return m_resultType; }
    /// Maximum VM stack depth (verified).
    u32 maxStack() const noexcept { return m_maxStack; }
    /// Static cost: an upper bound of the work of any evaluation (verified).
    u32 cost() const noexcept { return m_cost; }
    const std::vector<f64>& constants() const noexcept { return m_constants; }
    /// Symbol tables, deduplicated in first-use order. Hosts bind them once per program.
    const std::vector<std::string>& attrSymbols() const noexcept { return m_attrs; }
    const std::vector<std::string>& tagSymbols() const noexcept { return m_tags; }
    const std::vector<std::string>& fieldSymbols() const noexcept { return m_fields; }
    const std::vector<std::string>& curveSymbols() const noexcept { return m_curves; }
    const std::vector<u8>& code() const noexcept { return m_code; }
    /// True if the program calls stacks() / level().
    bool usesStacks() const noexcept { return m_usesStacks; }
    bool usesLevel() const noexcept { return m_usesLevel; }

    /// A (param, symbol) reference of an Attr, Field or Tag op.
    struct Ref {
        Op op;
        u8 param;
        u16 symbol;
    };
    /// Every Attr/Field/Tag reference in code order (duplicates included).
    std::vector<Ref> references() const;

    /// Canonical bytecode ("HXL1" container). decode(encode(p)) == p.
    std::vector<u8> encode() const;
    /// Decodes and fully verifies untrusted bytes (E_BYTECODE on any defect).
    static Result<Program> decode(std::span<const u8> bytes, Diagnostic* diag = nullptr);
    /// FNV-1a 64 of encode(): the corpus' "bytecode" value.
    u64 hash() const;
    /// Human-readable listing (debugging; not a stable format).
    std::string disassemble() const;

    /// Programs are equal when their canonical encodings are.
    friend bool operator==(const Program& a, const Program& b) { return a.encode() == b.encode(); }

private:
    friend class ProgramBuilder;
    friend class CompilerAccess;

    std::string m_name;
    std::vector<std::string> m_params;
    Type m_resultType = Type::Number;
    u32 m_maxStack = 0;
    u32 m_cost = 0;
    std::vector<f64> m_constants;
    std::vector<std::string> m_attrs;
    std::vector<std::string> m_tags;
    std::vector<std::string> m_fields;
    std::vector<std::string> m_curves;
    std::vector<u8> m_code;
    bool m_usesStacks = false;
    bool m_usesLevel = false;
};

/// Internal construction path used by the compiler and the decoder; verifies before publishing.
class ProgramBuilder {
public:
    Program program;

    /// Verifies code against the tables (types, stack, jumps, costs) and fills maxStack, cost and
    /// the uses* flags. Fails with E_BYTECODE (or E_LIMIT for a cost above `maxCost`).
    Result<Program> finish(u32 maxCost, Diagnostic* diag);
};

/// Constructs the helios::Error for a diagnostic (ParseError for lexical/syntax errors,
/// InvalidArgument for semantic ones, LimitExceeded, Corrupt for bytecode, NotFound for inputs).
Error toError(const Diagnostic& diag);

} // namespace helios::hxl
