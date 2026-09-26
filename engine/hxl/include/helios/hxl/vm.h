#pragma once
// HXL virtual machine: evaluates a verified Program against a host environment.
//
// Semantics (bit-identical with services/pkg/hxl; 06 §1.2 float rules): every op rounds to f64
// once; + - * / sqrt floor ceil abs are IEEE-754; pow/exp/ln/asinh are helios::det (never the CRT);
// min/max propagate NaN and order -0 < +0; clamp(x, lo, hi) = min(max(x, lo), hi);
// lerp(a, b, t) = a + (b - a) * t; comparisons are IEEE (NaN compares false, != true). A NaN result
// is returned as the canonical quiet NaN (bits 0x7ff8000000000000) so results compare bitwise on
// every CPU. No evaluation allocates.
//
// Threading: evaluate() is reentrant; an Env is used by one evaluation at a time.
//
// FP environment: eval() requires the default floating-point environment (round to nearest, no
// flush-to-zero or denormals-are-zero). Other modes change results, and the corpus fails under them,
// so a thread that changes the mode must restore it before evaluating. Go has no such modes.

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "helios/hxl/program.h"

namespace helios::hxl {

/// Piecewise-linear curve (`curve(Name, x)`; level curves, falloffs). Keys strictly increasing and
/// finite, values finite, at least one point. x at or outside the ends clamps to the end values;
/// inside, y = v[i] + (v[i+1] - v[i]) * ((x - k[i]) / (k[i+1] - k[i])); NaN gives NaN.
struct Curve {
    std::vector<f64> keys;
    std::vector<f64> values;

    /// Checks the invariants above (at most limits::kMaxCurvePoints points).
    Result<void> validate() const;
    /// Samples the curve (requires a valid curve).
    f64 sample(f64 x) const noexcept;
};

namespace limits {
inline constexpr u32 kMaxCurvePoints = 4096;
} // namespace limits

/// Result of an evaluation. Bool results are 0.0 / 1.0 in `number`.
struct Value {
    Type type = Type::Number;
    f64 number = 0.0;

    bool asBool() const noexcept { return number != 0.0; }
    static Value num(f64 v) noexcept { return {Type::Number, v}; }
    static Value boolean(bool b) noexcept { return {Type::Bool, b ? 1.0 : 0.0}; }
};

/// Host inputs of one evaluation. Symbol arguments are indexes into the program's symbol tables
/// (hosts map them to their own ids once per program). Returning false reports E_MISSING_INPUT.
class Env {
public:
    virtual ~Env() = default;
    virtual bool attr(u32 param, u32 symbol, f64& out) noexcept;
    virtual bool field(u32 param, u32 symbol, f64& out) noexcept;
    virtual bool tag(u32 param, u32 symbol, bool& out) noexcept;
    virtual const Curve* curve(u32 symbol) noexcept;
    virtual bool stacks(f64& out) noexcept;
    virtual bool level(f64& out) noexcept;
};

/// Canonical quiet NaN returned for every NaN result.
inline constexpr u64 kCanonicalNaNBits = 0x7ff8000000000000ull;

/// Evaluates `program`. The fast path: returns Status::Ok and writes `out`, or the failure status
/// without touching `out`: E_MISSING_INPUT, or E_BYTECODE for a default-constructed Program (every
/// compiled or decoded program has code). An Env's curve() must return valid curves
/// (Curve::validate()).
Status eval(const Program& program, Env& env, Value& out) noexcept;

/// Convenience wrapper returning a Result (NotFound on a missing input).
Result<Value> evaluate(const Program& program, Env& env);

/// HXL's min/max (NaN-propagating, -0 < +0). Exposed for hosts that must match HXL exactly.
f64 hxlMin(f64 a, f64 b) noexcept;
f64 hxlMax(f64 a, f64 b) noexcept;

/// A string-keyed environment for tools, tests and the corpus: values are looked up by parameter
/// name and symbol name. bind() must be called with the program before evaluating it.
class MapEnv final : public Env {
public:
    /// "param" -> "Attr.Name" -> value, and so on.
    std::unordered_map<std::string, std::unordered_map<std::string, f64>> attrs;
    std::unordered_map<std::string, std::unordered_map<std::string, f64>> fields;
    /// "param" -> tags the parameter has. tag(p, T) is true if one of them equals T or starts
    /// with "T." (hierarchical match, 06 §1.1).
    std::unordered_map<std::string, std::vector<std::string>> tags;
    std::unordered_map<std::string, Curve> curves;
    bool hasStacks = false;
    f64 stacksValue = 0.0;
    bool hasLevel = false;
    f64 levelValue = 0.0;

    /// Resolves the program's symbol tables against the maps (call again after editing them).
    /// Curves that fail Curve::validate() are bound as missing (E_MISSING_INPUT when used).
    void bind(const Program& program);

    bool attr(u32 param, u32 symbol, f64& out) noexcept override;
    bool field(u32 param, u32 symbol, f64& out) noexcept override;
    bool tag(u32 param, u32 symbol, bool& out) noexcept override;
    const Curve* curve(u32 symbol) noexcept override;
    bool stacks(f64& out) noexcept override;
    bool level(f64& out) noexcept override;

private:
    struct Slot {
        bool present = false;
        f64 value = 0.0;
    };
    // [param][symbol]
    std::vector<std::vector<Slot>> m_attrSlots;
    std::vector<std::vector<Slot>> m_fieldSlots;
    std::vector<std::vector<u8>> m_tagSlots; // 0 = false, 1 = true
    std::vector<const Curve*> m_curveSlots;
};

} // namespace helios::hxl
