#pragma once
// HXL compiler: source text -> verified Program (lexer, parser, type checker, code generator).
//
// Grammar (06 §1.2; identical in services/pkg/hxl):
//   source  := formula | expr [';']
//   formula := 'formula' Ident '(' [Ident {',' Ident}] ')' '=' expr [';']
//   expr    := or
//   or      := and {'||' and}            and := equality {'&&' equality}
//   equality:= compare {('=='|'!=') compare}
//   compare := sum {('<'|'<='|'>'|'>=') sum}
//   sum     := product {('+'|'-') product}   product := unary {('*'|'/') unary}
//   unary   := ('-'|'!') unary | power
//   power   := primary ['^' unary]       (right associative; -2^2 == -(2^2), 2^-1 is fine)
//   primary := Number | 'true' | 'false' | '(' expr ')' | Ident '(' [expr {',' expr}] ')'
//            | Ident {'.' Ident}
// Comments: `//` to the end of the line. Numbers: decimal with optional fraction and exponent.
//
// Values: `p.field` reads a context field of parameter p; attr(p, Name.Path) reads an attribute;
// tag(p, Tag.Path) tests a tag (hierarchically); curve(Curve.Name, x) samples a piecewise-linear
// curve; stacks() and level() read the evaluation context. Other built-ins: min, max (2+ args),
// clamp, lerp, select (lazy), pow, exp, ln, sqrt, asinh (06's hmath set, via helios::det), plus
// abs, floor and ceil (exact IEEE operations). `a ^ b` is pow(a, b). && and || short-circuit.
//
// Threading: compile() is pure and reentrant.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/hxl/program.h"

namespace helios::hxl {

struct CompileOptions {
    /// Parameters of a bare expression (ignored for a `formula` declaration).
    std::vector<std::string> params{"self"};
    /// When set, a program of another type fails with E_RESULT_TYPE.
    std::optional<Type> expectedType;
    /// Static cost budget; 0 or anything above limits::kMaxCost means limits::kMaxCost.
    u32 maxCost = limits::kMaxCost;
};

/// Compiles HXL source. On failure returns the error and, if `diag` is given, the structured
/// diagnostic (status, 1-based line/column, message).
Result<Program> compile(std::string_view source, const CompileOptions& options = {}, Diagnostic* diag = nullptr);

} // namespace helios::hxl
