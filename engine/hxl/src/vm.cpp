// HXL VM, curves and MapEnv. Mirrors services/pkg/hxl/vm.go op by op (06 §1.2 float rules): no
// folding or fusion, every op rounds to f64 once, transcendental functions come from helios::det.
#include "fp_control.h"

#include "helios/hxl/vm.h"

#include <bit>
#include <cmath>
#include <format>
#include <string_view>

#include "helios/math/scalar.h"

namespace helios::hxl {

bool Env::attr(u32, u32, f64&) noexcept { return false; }
bool Env::field(u32, u32, f64&) noexcept { return false; }
bool Env::tag(u32, u32, bool&) noexcept { return false; }
const Curve* Env::curve(u32) noexcept { return nullptr; }
bool Env::stacks(f64&) noexcept { return false; }
bool Env::level(f64&) noexcept { return false; }

namespace {

inline f64 canonicalNaN() noexcept { return std::bit_cast<f64>(kCanonicalNaNBits); }
inline u16 rd16(const u8* p) noexcept { return static_cast<u16>(p[0] | (p[1] << 8)); }

} // namespace

f64 hxlMin(f64 a, f64 b) noexcept {
    if (std::isnan(a) || std::isnan(b)) return canonicalNaN();
    if (a < b) return a;
    if (b < a) return b;
    return std::signbit(a) ? a : b; // equal: prefer -0
}

f64 hxlMax(f64 a, f64 b) noexcept {
    if (std::isnan(a) || std::isnan(b)) return canonicalNaN();
    if (a > b) return a;
    if (b > a) return b;
    return std::signbit(a) ? b : a; // equal: prefer +0
}

Result<void> Curve::validate() const {
    if (keys.empty()) return Error{ErrorCode::InvalidArgument, "curve has no points"};
    if (keys.size() != values.size()) return Error{ErrorCode::InvalidArgument, "curve keys and values differ in length"};
    if (keys.size() > limits::kMaxCurvePoints) {
        return makeError(ErrorCode::LimitExceeded, "curve has more than {} points", limits::kMaxCurvePoints);
    }
    for (usize i = 0; i < keys.size(); ++i) {
        if (!std::isfinite(keys[i]) || !std::isfinite(values[i])) {
            return makeError(ErrorCode::InvalidArgument, "curve point {} is not finite", i);
        }
        if (i > 0 && !(keys[i - 1] < keys[i])) {
            return makeError(ErrorCode::InvalidArgument, "curve keys must increase strictly (point {})", i);
        }
    }
    return {};
}

f64 Curve::sample(f64 x) const noexcept {
    const usize n = keys.size();
    if (std::isnan(x)) return canonicalNaN();
    if (x <= keys[0]) return values[0];
    if (x >= keys[n - 1]) return values[n - 1];
    usize lo = 0;
    usize hi = n - 1; // keys[lo] <= x < keys[hi]
    while (hi - lo > 1) {
        const usize mid = lo + (hi - lo) / 2;
        if (keys[mid] <= x) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const f64 t = (x - keys[lo]) / (keys[hi] - keys[lo]);
    const f64 d = values[hi] - values[lo];
    const f64 p = d * t;
    return values[lo] + p;
}

Status eval(const Program& program, Env& env, Value& out) noexcept {
    const std::vector<u8>& codeVec = program.code();
    const u8* code = codeVec.data();
    const usize size = codeVec.size();
    const f64* consts = program.constants().data();
    // Every verified program has code; a default-constructed Program has none and must not read
    // the (uninitialized) result slot.
    if (size == 0) return Status::Bytecode;
    f64 stack[limits::kMaxStack];
    usize sp = 0;
    usize pc = 0;
    while (pc < size) {
        const auto op = static_cast<Op>(code[pc]);
        switch (op) {
        case Op::Const:
            stack[sp++] = consts[rd16(code + pc + 1)];
            pc += 3;
            break;
        case Op::True:
            stack[sp++] = 1.0;
            pc += 1;
            break;
        case Op::False:
            stack[sp++] = 0.0;
            pc += 1;
            break;
        case Op::Attr: {
            f64 v = 0.0;
            if (!env.attr(code[pc + 1], rd16(code + pc + 2), v)) return Status::MissingInput;
            stack[sp++] = v;
            pc += 4;
            break;
        }
        case Op::Field: {
            f64 v = 0.0;
            if (!env.field(code[pc + 1], rd16(code + pc + 2), v)) return Status::MissingInput;
            stack[sp++] = v;
            pc += 4;
            break;
        }
        case Op::Tag: {
            bool b = false;
            if (!env.tag(code[pc + 1], rd16(code + pc + 2), b)) return Status::MissingInput;
            stack[sp++] = b ? 1.0 : 0.0;
            pc += 4;
            break;
        }
        case Op::Stacks: {
            f64 v = 0.0;
            if (!env.stacks(v)) return Status::MissingInput;
            stack[sp++] = v;
            pc += 1;
            break;
        }
        case Op::Level: {
            f64 v = 0.0;
            if (!env.level(v)) return Status::MissingInput;
            stack[sp++] = v;
            pc += 1;
            break;
        }
        case Op::Curve: {
            const Curve* c = env.curve(rd16(code + pc + 1));
            if (!c) return Status::MissingInput;
            stack[sp - 1] = c->sample(stack[sp - 1]);
            pc += 3;
            break;
        }
        case Op::Neg:
            stack[sp - 1] = -stack[sp - 1];
            pc += 1;
            break;
        case Op::Add:
            --sp;
            stack[sp - 1] = stack[sp - 1] + stack[sp];
            pc += 1;
            break;
        case Op::Sub:
            --sp;
            stack[sp - 1] = stack[sp - 1] - stack[sp];
            pc += 1;
            break;
        case Op::Mul:
            --sp;
            stack[sp - 1] = stack[sp - 1] * stack[sp];
            pc += 1;
            break;
        case Op::Div:
            --sp;
            stack[sp - 1] = stack[sp - 1] / stack[sp];
            pc += 1;
            break;
        case Op::Pow:
            --sp;
            stack[sp - 1] = det::pow(stack[sp - 1], stack[sp]);
            pc += 1;
            break;
        case Op::Min:
            --sp;
            stack[sp - 1] = hxlMin(stack[sp - 1], stack[sp]);
            pc += 1;
            break;
        case Op::Max:
            --sp;
            stack[sp - 1] = hxlMax(stack[sp - 1], stack[sp]);
            pc += 1;
            break;
        case Op::Clamp: {
            sp -= 2;
            const f64 x = stack[sp - 1];
            const f64 lo = stack[sp];
            const f64 hi = stack[sp + 1];
            stack[sp - 1] = hxlMin(hxlMax(x, lo), hi);
            pc += 1;
            break;
        }
        case Op::Lerp: {
            sp -= 2;
            const f64 a = stack[sp - 1];
            const f64 b = stack[sp];
            const f64 t = stack[sp + 1];
            const f64 d = b - a;
            const f64 p = d * t;
            stack[sp - 1] = a + p;
            pc += 1;
            break;
        }
        case Op::Sqrt:
            stack[sp - 1] = std::sqrt(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Exp:
            stack[sp - 1] = det::exp(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Ln:
            stack[sp - 1] = det::ln(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Asinh:
            stack[sp - 1] = det::asinh(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Abs:
            stack[sp - 1] = std::fabs(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Floor:
            stack[sp - 1] = std::floor(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Ceil:
            stack[sp - 1] = std::ceil(stack[sp - 1]);
            pc += 1;
            break;
        case Op::Lt:
            --sp;
            stack[sp - 1] = stack[sp - 1] < stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::Le:
            --sp;
            stack[sp - 1] = stack[sp - 1] <= stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::Gt:
            --sp;
            stack[sp - 1] = stack[sp - 1] > stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::Ge:
            --sp;
            stack[sp - 1] = stack[sp - 1] >= stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::EqN:
        case Op::EqB:
            --sp;
            stack[sp - 1] = stack[sp - 1] == stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::NeN:
        case Op::NeB:
            --sp;
            stack[sp - 1] = stack[sp - 1] != stack[sp] ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::Not:
            stack[sp - 1] = stack[sp - 1] == 0.0 ? 1.0 : 0.0;
            pc += 1;
            break;
        case Op::JumpIfFalse:
            --sp;
            pc += 3;
            if (stack[sp] == 0.0) pc += rd16(code + pc - 2);
            break;
        case Op::Jump:
            pc += 3 + rd16(code + pc + 1);
            break;
        default:
            // Unreachable for verified programs.
            return Status::Bytecode;
        }
    }
    f64 result = stack[0];
    if (std::isnan(result)) result = canonicalNaN();
    out = Value{program.resultType(), result};
    return Status::Ok;
}

Result<Value> evaluate(const Program& program, Env& env) {
    Value v;
    const Status s = eval(program, env, v);
    if (s != Status::Ok) {
        Diagnostic d;
        d.status = s;
        d.message = s == Status::MissingInput ? "the environment has no value for an input"
                                              : "invalid program (default-constructed or unverified)";
        return toError(d);
    }
    return v;
}

// ---- MapEnv --------------------------------------------------------------------------------------

void MapEnv::bind(const Program& program) {
    const auto& params = program.params();
    m_attrSlots.assign(params.size(), {});
    m_fieldSlots.assign(params.size(), {});
    m_tagSlots.assign(params.size(), {});
    for (usize p = 0; p < params.size(); ++p) {
        auto fillNumbers = [&](const std::unordered_map<std::string, std::unordered_map<std::string, f64>>& src,
                               const std::vector<std::string>& symbols, std::vector<Slot>& dst) {
            dst.assign(symbols.size(), Slot{});
            const auto it = src.find(params[p]);
            if (it == src.end()) return;
            for (usize s = 0; s < symbols.size(); ++s) {
                const auto v = it->second.find(symbols[s]);
                if (v != it->second.end()) dst[s] = Slot{true, v->second};
            }
        };
        fillNumbers(attrs, program.attrSymbols(), m_attrSlots[p]);
        fillNumbers(fields, program.fieldSymbols(), m_fieldSlots[p]);
        const auto& tagSyms = program.tagSymbols();
        m_tagSlots[p].assign(tagSyms.size(), 0);
        const auto it = tags.find(params[p]);
        if (it != tags.end()) {
            for (usize s = 0; s < tagSyms.size(); ++s) {
                const std::string_view want = tagSyms[s];
                for (const std::string& have : it->second) {
                    if (have == want || (have.size() > want.size() && have.compare(0, want.size(), want) == 0 &&
                                         have[want.size()] == '.')) {
                        m_tagSlots[p][s] = 1;
                        break;
                    }
                }
            }
        }
    }
    m_curveSlots.assign(program.curveSymbols().size(), nullptr);
    for (usize s = 0; s < m_curveSlots.size(); ++s) {
        const auto it = curves.find(program.curveSymbols()[s]);
        // Curve::sample() requires a valid curve (an empty or ragged one would be read out of
        // bounds): an invalid curve is reported like a missing one, E_MISSING_INPUT.
        if (it != curves.end() && it->second.validate()) m_curveSlots[s] = &it->second;
    }
}

bool MapEnv::attr(u32 param, u32 symbol, f64& out) noexcept {
    if (param >= m_attrSlots.size() || symbol >= m_attrSlots[param].size()) return false;
    const Slot& s = m_attrSlots[param][symbol];
    if (!s.present) return false;
    out = s.value;
    return true;
}

bool MapEnv::field(u32 param, u32 symbol, f64& out) noexcept {
    if (param >= m_fieldSlots.size() || symbol >= m_fieldSlots[param].size()) return false;
    const Slot& s = m_fieldSlots[param][symbol];
    if (!s.present) return false;
    out = s.value;
    return true;
}

bool MapEnv::tag(u32 param, u32 symbol, bool& out) noexcept {
    if (param >= m_tagSlots.size() || symbol >= m_tagSlots[param].size()) return false;
    out = m_tagSlots[param][symbol] != 0;
    return true;
}

const Curve* MapEnv::curve(u32 symbol) noexcept { return symbol < m_curveSlots.size() ? m_curveSlots[symbol] : nullptr; }

bool MapEnv::stacks(f64& out) noexcept {
    if (!hasStacks) return false;
    out = stacksValue;
    return true;
}

bool MapEnv::level(f64& out) noexcept {
    if (!hasLevel) return false;
    out = levelValue;
    return true;
}

} // namespace helios::hxl
