// Attributes and modifiers (06 §1.2). Deterministic aggregation: see attributes.h for the normative
// formula; every fold below is explicit and left to right, and FP contraction is off.
#include "fp_control.h"

#include "helios/gameplay/attributes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <queue>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/jobs.h"
#include "helios/math/scalar.h"

namespace helios::gameplay {
namespace {

constexpr u64 kCanonicalNaN = 0x7ff8000000000000ull;

inline f64 canonical(f64 v) noexcept { return std::isnan(v) ? std::bit_cast<f64>(kCanonicalNaN) : v; }

// Maps IEEE doubles to integers with the same total order (-NaN < -inf < ... < -0 < +0 < ... < NaN).
inline u64 orderKey(f64 v) noexcept {
    const u64 b = std::bit_cast<u64>(v);
    return (b >> 63) ? ~b : (b | 0x8000000000000000ull);
}

class FNV {
public:
    void bytes(const void* p, usize n) {
        const auto* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) {
            m_h ^= b[i];
            m_h *= kFnv1a64Prime;
        }
    }
    void str(std::string_view s) {
        bytes(s.data(), s.size());
        u8 zero = 0;
        bytes(&zero, 1);
    }
    template <class T>
    void pod(const T& v) {
        bytes(&v, sizeof(T));
    }
    u64 value() const noexcept { return m_h; }

private:
    u64 m_h = kFnv1a64Offset;
};

// hxl::Env over one entity: attr(self, X) = final value, tag(self, T) = hierarchical tag test.
class SelfEnv final : public hxl::Env {
public:
    SelfEnv(const BoundFormula& f, std::span<const f64> finals, const TagContainer* tags, f64 stacks, f64 level) noexcept
        : m_f(f), m_finals(finals), m_tags(tags), m_stacks(stacks), m_level(level) {}
    bool attr(u32, u32 symbol, f64& out) noexcept override {
        out = m_finals[m_f.attrSlots()[symbol]];
        return true;
    }
    bool tag(u32, u32 symbol, bool& out) noexcept override {
        out = m_tags != nullptr && m_tags->has(m_f.tagIndices()[symbol]);
        return true;
    }
    bool stacks(f64& out) noexcept override {
        out = m_stacks;
        return true;
    }
    bool level(f64& out) noexcept override {
        out = m_level;
        return true;
    }

private:
    const BoundFormula& m_f;
    std::span<const f64> m_finals;
    const TagContainer* m_tags;
    f64 m_stacks;
    f64 m_level;
};

// Kahn's algorithm, smallest slot first: a deterministic topological order. False on a cycle.
template <class ForEachDependent>
bool topoOrder(usize n, ForEachDependent&& forEachDependent, std::vector<AttrSlot>& out) {
    std::vector<u32> indegree(n, 0);
    for (usize a = 0; a < n; ++a) {
        forEachDependent(static_cast<AttrSlot>(a), [&](AttrSlot d) { ++indegree[d]; });
    }
    std::priority_queue<AttrSlot, std::vector<AttrSlot>, std::greater<AttrSlot>> ready;
    for (usize a = 0; a < n; ++a) {
        if (indegree[a] == 0) ready.push(static_cast<AttrSlot>(a));
    }
    out.clear();
    while (!ready.empty()) {
        const AttrSlot a = ready.top();
        ready.pop();
        out.push_back(a);
        forEachDependent(a, [&](AttrSlot d) {
            if (--indegree[d] == 0) ready.push(d);
        });
    }
    return out.size() == n;
}

bool matchesEmpty(const TagQuery& q) noexcept { return !q.hasAny && q.allMask.none() && q.allCold.empty(); }

} // namespace

f64 stackingPenalty(u32 index) noexcept {
    static const std::array<f64, 64> kTable = [] {
        std::array<f64, 64> t{};
        for (u32 k = 0; k < t.size(); ++k) {
            const f64 q = static_cast<f64>(k) / 2.67;
            t[k] = det::exp(-(q * q));
        }
        return t;
    }();
    if (index < kTable.size()) return kTable[index];
    const f64 q = static_cast<f64>(index) / 2.67;
    return det::exp(-(q * q));
}

// ---- BoundFormula ----------------------------------------------------------------------------------

f64 BoundFormula::evaluate(std::span<const f64> finals, const TagContainer* tags, f64 stacks, f64 level) const noexcept {
    SelfEnv env(*this, finals, tags, stacks, level);
    hxl::Value v;
    if (hxl::eval(m_program, env, v) != hxl::Status::Ok) return std::bit_cast<f64>(kCanonicalNaN);
    return v.number;
}

// ---- AttributeLayout --------------------------------------------------------------------------------

AttrSlot AttributeLayout::find(std::string_view id) const noexcept {
    auto it = m_byId.find(id); // heterogeneous lookup: no std::string
    return it == m_byId.end() ? kInvalidAttr : it->second;
}

AttrSlot AttributeLayout::findByRecord(refl::RecordId rid) const noexcept {
    if (rid == 0) return kInvalidAttr;
    for (usize i = 0; i < m_specs.size(); ++i) {
        if (m_specs[i].rid == rid) return static_cast<AttrSlot>(i);
    }
    return kInvalidAttr;
}

Result<std::shared_ptr<const BoundFormula>> AttributeLayout::bindFormula(const hxl::Program& program,
                                                                         bool allowContext) const {
    if (program.params().size() != 1) {
        return makeError(ErrorCode::InvalidArgument, "an attribute formula takes exactly one parameter (self), not {}",
                         program.params().size());
    }
    if (program.resultType() != hxl::Type::Number) return Error{ErrorCode::InvalidArgument, "an attribute formula must be a number"};
    if (!program.fieldSymbols().empty()) {
        return makeError(ErrorCode::InvalidArgument, "context field '{}' is not available to attribute formulas",
                         program.fieldSymbols()[0]);
    }
    if (!program.curveSymbols().empty()) {
        return makeError(ErrorCode::Unsupported, "curve '{}' is not available to live attribute formulas",
                         program.curveSymbols()[0]);
    }
    if (!allowContext && (program.usesStacks() || program.usesLevel())) {
        return Error{ErrorCode::InvalidArgument, "stacks() and level() are not available to derived attributes"};
    }
    auto f = std::make_shared<BoundFormula>();
    f->m_program = program;
    for (const std::string& a : program.attrSymbols()) {
        const AttrSlot s = find(a);
        if (s == kInvalidAttr) return makeError(ErrorCode::NotFound, "unknown attribute '{}' in formula", a);
        f->m_attrSlots.push_back(s);
    }
    for (const std::string& t : program.tagSymbols()) {
        if (!m_tagRegistry) return makeError(ErrorCode::InvalidArgument, "formula tests tag '{}' but the layout has no tag registry", t);
        const TagIndex idx = m_tagRegistry->find(t);
        if (idx == kInvalidTag) return makeError(ErrorCode::NotFound, "unknown tag '{}' in formula", t);
        f->m_tags.push_back(idx);
    }
    f->m_layoutHash = m_hash;
    return std::shared_ptr<const BoundFormula>(std::move(f));
}

Result<std::shared_ptr<const BoundFormula>> AttributeLayout::compileFormula(std::string_view source, bool allowContext) const {
    hxl::CompileOptions opts;
    opts.params = {"self"};
    opts.expectedType = hxl::Type::Number;
    hxl::Diagnostic diag;
    auto program = hxl::compile(source, opts, &diag);
    if (!program) return makeError(program.errorCode(), "formula '{}': {}", source, diag.toString());
    return bindFormula(*program, allowContext);
}

Result<std::shared_ptr<const AttributeLayout>> AttributeLayout::build(std::span<const Input> inputs,
                                                                      std::shared_ptr<const TagRegistry> tags) {
    if (inputs.size() > kMaxAttributes) return makeError(ErrorCode::LimitExceeded, "more than {} attributes", kMaxAttributes);
    auto layout = std::make_shared<AttributeLayout>();
    layout->m_tagRegistry = std::move(tags);
    const usize n = inputs.size();
    layout->m_specs.resize(n);
    for (usize i = 0; i < n; ++i) {
        const Input& in = inputs[i];
        if (!isValidTagName(in.id)) return makeError(ErrorCode::InvalidArgument, "invalid attribute id '{}'", in.id);
        if (!layout->m_byId.emplace(in.id, static_cast<AttrSlot>(i)).second) {
            return makeError(ErrorCode::AlreadyExists, "duplicate attribute '{}'", in.id);
        }
        // Modifiers and clamps name attributes by RecordId: two slots with one id would be ambiguous.
        for (usize j = 0; in.rid != 0 && j < i; ++j) {
            if (inputs[j].rid == in.rid) {
                return makeError(ErrorCode::AlreadyExists, "attributes '{}' and '{}' share record id {}", inputs[j].id, in.id, in.rid);
            }
        }
        AttributeSpec& s = layout->m_specs[i];
        s.id = in.id;
        s.rid = in.rid;
        s.defaultValue = in.defaultValue;
        s.multiplierMode = in.multiplierMode;
        s.stackingPenalised = in.stackingPenalised;
        s.replicate = in.replicate;
        s.quant = in.quant;
        s.persistBase = in.persistBase;
    }
    auto bound = [&](const std::optional<std::variant<f64, std::string>>& in, const std::string& owner,
                     AttrBound& out) -> Result<void> {
        if (!in) return {};
        if (const f64* c = std::get_if<f64>(&*in)) {
            if (std::isnan(*c)) return makeError(ErrorCode::InvalidArgument, "attribute '{}': clamp bound is NaN", owner);
            out.kind = AttrBound::Kind::Const;
            out.value = *c;
            return {};
        }
        const std::string& ref = std::get<std::string>(*in);
        const AttrSlot s = layout->find(ref);
        if (s == kInvalidAttr) return makeError(ErrorCode::NotFound, "attribute '{}': clamp names unknown attribute '{}'", owner, ref);
        out.kind = AttrBound::Kind::Attr;
        out.attr = s;
        return {};
    };
    // Hash the declarative content first; derived formulas are bound with it.
    FNV h;
    for (usize i = 0; i < n; ++i) {
        const Input& in = inputs[i];
        AttributeSpec& s = layout->m_specs[i];
        HELIOS_TRY(bound(in.minClamp, in.id, s.minClamp));
        HELIOS_TRY(bound(in.maxClamp, in.id, s.maxClamp));
        if (s.minClamp.kind == AttrBound::Kind::Const && s.maxClamp.kind == AttrBound::Kind::Const &&
            s.minClamp.value > s.maxClamp.value) {
            return makeError(ErrorCode::InvalidArgument, "attribute '{}': minClamp {} > maxClamp {}", in.id, s.minClamp.value,
                             s.maxClamp.value);
        }
        if (s.quant && (s.quant->bits < 1 || s.quant->bits > 32 || !std::isfinite(s.quant->min) ||
                        !std::isfinite(s.quant->max) || !(s.quant->min < s.quant->max))) {
            return makeError(ErrorCode::InvalidArgument, "attribute '{}': quantization needs 1..32 bits and min < max",
                             in.id);
        }
        h.str(s.id);
        h.pod(std::bit_cast<u64>(s.defaultValue));
        for (const AttrBound* b : {&s.minClamp, &s.maxClamp}) {
            h.pod(static_cast<u8>(b->kind));
            h.pod(std::bit_cast<u64>(b->value));
            h.pod(b->attr);
        }
        h.str(in.derived);
        h.pod(static_cast<u8>(s.multiplierMode));
        h.pod(static_cast<u8>(s.stackingPenalised));
        h.pod(static_cast<u8>(s.replicate));
        h.pod(static_cast<u8>(s.persistBase));
    }
    // Bound formulas carry TagIndex values of this registry: a layout with the same attributes but
    // another registry must not accept them (addModifier compares layout hashes).
    h.pod(layout->m_tagRegistry ? layout->m_tagRegistry->hash() : u64{0});
    h.pod(static_cast<u64>(layout->m_tagRegistry ? layout->m_tagRegistry->size() : 0));
    layout->m_hash = h.value();
    for (usize i = 0; i < n; ++i) {
        if (inputs[i].derived.empty()) continue;
        auto f = layout->compileFormula(inputs[i].derived, /*allowContext=*/false);
        if (!f) return makeError(f.errorCode(), "attribute '{}': derived {}", inputs[i].id, f.error().message);
        layout->m_specs[i].derived = std::move(*f);
    }
    // Static dependency graph: clamp and derived reads.
    layout->m_dependents.assign(n, {});
    auto edge = [&](AttrSlot from, AttrSlot to) {
        auto& d = layout->m_dependents[from];
        if (std::find(d.begin(), d.end(), to) == d.end()) d.push_back(to);
    };
    for (usize i = 0; i < n; ++i) {
        const AttributeSpec& s = layout->m_specs[i];
        const auto to = static_cast<AttrSlot>(i);
        if (s.minClamp.kind == AttrBound::Kind::Attr) edge(s.minClamp.attr, to);
        if (s.maxClamp.kind == AttrBound::Kind::Attr) edge(s.maxClamp.attr, to);
        if (s.derived) {
            for (AttrSlot from : s.derived->attrSlots()) edge(from, to);
        }
    }
    for (auto& d : layout->m_dependents) std::sort(d.begin(), d.end());
    const bool acyclic = topoOrder(
        n,
        [&](AttrSlot a, auto&& fn) {
            for (AttrSlot d : layout->m_dependents[a]) fn(d);
        },
        layout->m_order);
    if (!acyclic) {
        std::string members;
        std::vector<bool> ordered(n, false);
        for (AttrSlot a : layout->m_order) ordered[a] = true;
        for (usize i = 0; i < n; ++i) {
            if (!ordered[i]) members += (members.empty() ? "" : ", ") + layout->m_specs[i].id;
        }
        return makeError(ErrorCode::InvalidArgument, "attribute dependency cycle among: {}", members);
    }
    return std::shared_ptr<const AttributeLayout>(std::move(layout));
}

Result<std::shared_ptr<const AttributeLayout>> AttributeLayout::fromRecords(std::span<const Record> records,
                                                                            std::shared_ptr<const TagRegistry> tags) {
    if (records.size() > kMaxAttributes) return makeError(ErrorCode::LimitExceeded, "more than {} attributes", kMaxAttributes);
    std::vector<Input> inputs;
    inputs.reserve(records.size());
    std::unordered_map<refl::RecordId, const AttributeDef*> byRid; // clamp references (was a linear scan)
    byRid.reserve(records.size());
    for (const Record& r : records) {
        if (r.rid != 0) byRid.emplace(r.rid, r.def); // duplicates are rejected by build()
    }
    auto idOf = [&](refl::RecordId rid) -> const AttributeDef* {
        const auto it = byRid.find(rid);
        return it == byRid.end() ? nullptr : it->second;
    };
    for (const Record& r : records) {
        if (!r.def) return Error{ErrorCode::InvalidArgument, "attribute record without a definition"};
        const AttributeDef& d = *r.def;
        Input in;
        in.id = std::string(d.id.view());
        in.rid = r.rid;
        in.defaultValue = d.default_;
        for (int k = 0; k < 2; ++k) {
            const auto& clamp = k == 0 ? d.minClamp : d.maxClamp;
            auto& out = k == 0 ? in.minClamp : in.maxClamp;
            if (!clamp) continue;
            if (const auto* c = std::get_if<AttrOrConstConst>(&*clamp)) {
                out = c->value;
            } else {
                const auto& a = std::get<AttrOrConstAttr>(*clamp);
                const AttributeDef* target = idOf(a.attr.id);
                if (!target) {
                    return makeError(ErrorCode::NotFound, "attribute '{}': clamp references record {} outside the set", in.id,
                                     a.attr.id);
                }
                out = std::string(target->id.view());
            }
        }
        if (d.derived) in.derived = d.derived->text;
        in.multiplierMode = d.multiplierMode;
        in.stackingPenalised = d.stackingPenalised;
        in.replicate = d.replicate;
        in.quant = d.quant;
        in.persistBase = d.persistBase;
        inputs.push_back(std::move(in));
    }
    return build(inputs, std::move(tags));
}

// ---- Modifier ---------------------------------------------------------------------------------------

Modifier Modifier::constant(AttrSlot attr, ModOp op, f64 value, u64 sourceId) {
    Modifier m;
    m.attr = attr;
    m.op = op;
    m.kind = MagnitudeKind::Constant;
    m.value = value;
    m.sourceId = sourceId;
    return m;
}

Modifier Modifier::fromAttribute(AttrSlot attr, ModOp op, AttrSlot source, f64 coefficient, u64 sourceId) {
    Modifier m = constant(attr, op, coefficient, sourceId);
    m.kind = MagnitudeKind::Attribute;
    m.source = source;
    return m;
}

Modifier Modifier::fromFormula(AttrSlot attr, ModOp op, std::shared_ptr<const BoundFormula> formula, u64 sourceId) {
    Modifier m = constant(attr, op, 0.0, sourceId);
    m.kind = MagnitudeKind::Formula;
    m.formula = std::move(formula);
    return m;
}

Result<Modifier> instantiateModifier(const ModifierDef& def, const ModifierContext& ctx) {
    if (!ctx.layout) return Error{ErrorCode::InvalidArgument, "instantiateModifier: no layout"};
    if (def.target.domain != ModDomain::Self || def.target.filter) {
        return Error{ErrorCode::Unsupported, "cross-entity modifier domains and filters arrive in Phase 2 (06 §1.2)"};
    }
    const AttributeLayout& layout = *ctx.layout;
    const AttrSlot slot = layout.findByRecord(def.attr.id);
    if (slot == kInvalidAttr) return makeError(ErrorCode::NotFound, "modifier targets attribute record {} not in the layout", def.attr.id);
    Modifier m = Modifier::constant(slot, def.op, 0.0, ctx.sourceId);
    m.layoutHash = layout.hash();
    m.priority = def.priority;
    m.penaltyGroup = def.penaltyGroup;
    m.exempt = def.exempt;
    m.stacks = ctx.stacks;
    m.level = ctx.level;
    if (def.requirement) {
        if (!layout.tags()) return Error{ErrorCode::InvalidArgument, "modifier requirement needs a tag registry"};
        HELIOS_TRY_ASSIGN(TagQuery query, layout.tags()->compileQuery(def.requirement->text));
        m.requirement = std::make_shared<const TagQuery>(std::move(query));
    }
    if (const auto* c = std::get_if<MagnitudeConst>(&def.magnitude)) {
        m.value = c->value;
    } else if (const auto* cv = std::get_if<MagnitudeCurve>(&def.magnitude)) {
        const std::string name(cv->curve.view());
        const hxl::Curve* curve = nullptr;
        if (ctx.curves) {
            auto it = ctx.curves->find(name);
            if (it != ctx.curves->end()) curve = &it->second;
        }
        if (!curve) return makeError(ErrorCode::NotFound, "modifier magnitude curve '{}' not found", name);
        HELIOS_TRY(curve->validate());
        const f64 x = cv->input == MagnitudeInput::Level ? ctx.level : ctx.stacks;
        m.value = curve->sample(x) * cv->scale;
    } else if (const auto* a = std::get_if<MagnitudeAttr>(&def.magnitude)) {
        if (a->from == MagnitudeSource::Target) {
            const AttrSlot src = layout.findByRecord(a->attr.id);
            if (src == kInvalidAttr) return makeError(ErrorCode::NotFound, "magnitude attribute record {} not in the layout", a->attr.id);
            if (a->capture == CaptureMode::Live) {
                m.kind = MagnitudeKind::Attribute;
                m.source = src;
                m.value = a->coefficient;
            } else {
                if (!ctx.target) return Error{ErrorCode::InvalidArgument, "a Target/Snapshot magnitude needs the target set"};
                // `src` is a slot of ctx.layout: reading it from a set of another layout would read
                // another attribute, or past the end of a smaller set.
                if (&ctx.target->layout() != &layout && ctx.target->layout().hash() != layout.hash()) {
                    return Error{ErrorCode::InvalidArgument, "the target set does not use the modifier context's layout"};
                }
                m.value = a->coefficient * ctx.target->value(src);
            }
        } else {
            if (a->capture == CaptureMode::Live) {
                return Error{ErrorCode::Unsupported, "live source-entity magnitudes are cross-entity (Phase 2)"};
            }
            if (!ctx.source) return Error{ErrorCode::InvalidArgument, "a Source magnitude needs the source set"};
            const AttrSlot src = ctx.source->layout().findByRecord(a->attr.id);
            if (src == kInvalidAttr) {
                return makeError(ErrorCode::NotFound, "magnitude attribute record {} not in the source layout", a->attr.id);
            }
            m.value = a->coefficient * ctx.source->value(src);
        }
    } else {
        const auto& hx = std::get<MagnitudeHxl>(def.magnitude);
        HELIOS_TRY_ASSIGN(m.formula, layout.compileFormula(hx.expr.text));
        m.kind = MagnitudeKind::Formula;
    }
    return m;
}

// ---- AttributeSet -----------------------------------------------------------------------------------

AttributeSet::AttributeSet(std::shared_ptr<const AttributeLayout> layout) : m_layout(std::move(layout)) {
    HELIOS_ASSERT(m_layout, "AttributeSet needs a layout");
    m_size = m_layout->size();
    m_words = (m_size + 63) / 64;
    m_values.resize(2 * m_size);
    for (usize i = 0; i < m_size; ++i) {
        m_values[i] = m_layout->spec(static_cast<AttrSlot>(i)).defaultValue;
        m_values[m_size + i] = m_values[i];
    }
    m_bits.assign(2 * m_words, 0);
    m_attrInfo.assign(2 * m_size, 0);
    std::fill(m_attrInfo.begin(), m_attrInfo.begin() + static_cast<isize>(m_size), kNone);
    markAllDirty();
}

void AttributeSet::setDirty(AttrSlot slot) noexcept {
    m_bits[slot >> 6] |= u64{1} << (slot & 63);
    m_anyDirty = true;
}

void AttributeSet::markAllDirty() {
    for (usize i = 0; i < m_size; ++i) setDirty(static_cast<AttrSlot>(i));
}

void AttributeSet::markDependentsDirty(AttrSlot slot) noexcept {
    for (AttrSlot d : m_layout->staticDependents(slot)) setDirty(d);
    if (!m_dynDependents.empty()) {
        for (const auto& [d, count] : m_dynDependents[slot]) setDirty(d);
    }
}

bool AttributeSet::setBase(AttrSlot slot, f64 value) {
    HELIOS_ASSERT(slot < m_size, "attribute slot out of range");
    if (slot >= m_size) return false; // e.g. kInvalidAttr from a failed find()
    if (std::bit_cast<u64>(m_values[slot]) == std::bit_cast<u64>(value)) return true;
    m_values[slot] = value;
    setDirty(slot);
    return true;
}

void AttributeSet::dependencies(const Modifier& m, std::vector<AttrSlot>& out) const {
    out.clear();
    if (m.kind == MagnitudeKind::Attribute) out.push_back(m.source);
    if (m.kind == MagnitudeKind::Formula && m.formula) {
        for (AttrSlot s : m.formula->attrSlots()) out.push_back(s);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool AttributeSet::addEdge(AttrSlot from, AttrSlot to) {
    if (m_dynDependents.empty()) m_dynDependents.assign(m_size, {});
    for (auto& [d, count] : m_dynDependents[from]) {
        if (d == to) {
            ++count;
            return false;
        }
    }
    m_dynDependents[from].emplace_back(to, 1u);
    return true;
}

void AttributeSet::removeEdge(AttrSlot from, AttrSlot to) {
    if (m_dynDependents.empty()) return;
    auto& deps = m_dynDependents[from];
    for (usize i = 0; i < deps.size(); ++i) {
        if (deps[i].first != to) continue;
        if (--deps[i].second == 0) {
            deps.erase(deps.begin() + static_cast<isize>(i));
            m_orderDirty = true;
        }
        return;
    }
}

bool AttributeSet::rebuildOrder() {
    const bool ok = topoOrder(
        m_size,
        [&](AttrSlot a, auto&& fn) {
            for (AttrSlot d : m_layout->staticDependents(a)) fn(d);
            if (!m_dynDependents.empty()) {
                for (const auto& [d, count] : m_dynDependents[a]) fn(d);
            }
        },
        m_ownOrder);
    if (ok) {
        m_orderDirty = false;
        m_useOwnOrder = true;
    }
    return ok;
}

Result<ModifierHandle> AttributeSet::addModifier(Modifier m) {
    if (m.attr >= m_size) return Error{ErrorCode::OutOfRange, "modifier attribute slot out of range"};
    if (static_cast<u8>(m.op) > static_cast<u8>(ModOp::PostAssign)) return Error{ErrorCode::InvalidArgument, "invalid ModOp"};
    if (m.kind == MagnitudeKind::Attribute && m.source >= m_size) {
        return Error{ErrorCode::OutOfRange, "modifier source attribute out of range"};
    }
    if (m.layoutHash != 0 && m.layoutHash != m_layout->hash()) {
        return Error{ErrorCode::InvalidArgument, "modifier was instantiated for a different attribute layout"};
    }
    if (m.kind == MagnitudeKind::Formula) {
        if (!m.formula) return Error{ErrorCode::InvalidArgument, "formula modifier without a formula"};
        if (m.formula->layoutHash() != m_layout->hash()) {
            return Error{ErrorCode::InvalidArgument, "formula was bound to a different attribute layout"};
        }
    }
    if (m.requirement && m.requirement->registry != 0 &&
        (!m_layout->tags() || m_layout->tags()->queryHash() != m.requirement->registry)) {
        return Error{ErrorCode::InvalidArgument, "requirement was compiled against a different tag registry"};
    }
    std::vector<AttrSlot> deps;
    dependencies(m, deps);
    if (std::find(deps.begin(), deps.end(), m.attr) != deps.end()) {
        return makeError(ErrorCode::InvalidArgument, "modifier on '{}' reads '{}' itself (dependency cycle)",
                         m_layout->spec(m.attr).id, m_layout->spec(m.attr).id);
    }
    bool newEdge = false;
    for (AttrSlot d : deps) newEdge |= addEdge(d, m.attr);
    if (newEdge && !rebuildOrder()) {
        for (AttrSlot d : deps) removeEdge(d, m.attr);
        const bool restored = rebuildOrder();
        HELIOS_ASSERT(restored, "attribute order must be restorable");
        (void)restored;
        return makeError(ErrorCode::InvalidArgument, "modifier on '{}' would create a dependency cycle",
                         m_layout->spec(m.attr).id);
    }
    u32 index = 0;
    if (!m_freeMods.empty()) {
        index = m_freeMods.back();
        m_freeMods.pop_back();
    } else {
        index = static_cast<u32>(m_mods.size());
        m_mods.emplace_back();
    }
    ModSlot& slot = m_mods[index];
    const AttrSlot attr = m.attr;
    if (m.requirement || (m.formula && m.formula->usesTags())) ++m_attrInfo[m_size + attr];
    slot.mod = std::move(m);
    slot.alive = true;
    slot.lastActive = false;
    slot.lastMagnitude = 0.0;
    slot.next = m_attrInfo[attr];
    m_attrInfo[attr] = index;
    ++m_liveMods;
    setDirty(attr);
    return ModifierHandle{index, slot.generation};
}

bool AttributeSet::removeModifier(ModifierHandle h) {
    if (h.index >= m_mods.size()) return false;
    ModSlot& slot = m_mods[h.index];
    if (!slot.alive || slot.generation != h.generation) return false;
    const AttrSlot attr = slot.mod.attr;
    std::vector<AttrSlot> deps;
    dependencies(slot.mod, deps);
    for (AttrSlot d : deps) removeEdge(d, attr);
    // Unlink from the attribute's list.
    u32* link = &m_attrInfo[attr];
    while (*link != h.index) link = &m_mods[*link].next;
    *link = slot.next;
    if (slot.mod.requirement || (slot.mod.formula && slot.mod.formula->usesTags())) --m_attrInfo[m_size + attr];
    slot.mod = Modifier{};
    slot.alive = false;
    slot.next = kNone;
    ++slot.generation;
    m_freeMods.push_back(h.index);
    --m_liveMods;
    setDirty(attr);
    return true;
}

usize AttributeSet::removeModifiersFromSource(u64 sourceId) {
    usize removed = 0;
    for (u32 i = 0; i < m_mods.size(); ++i) {
        if (m_mods[i].alive && m_mods[i].mod.sourceId == sourceId) {
            removed += removeModifier(ModifierHandle{i, m_mods[i].generation}) ? 1 : 0;
        }
    }
    return removed;
}

void AttributeSet::onTagsChanged() {
    for (usize i = 0; i < m_size; ++i) {
        const auto slot = static_cast<AttrSlot>(i);
        const auto& derived = m_layout->spec(slot).derived;
        if (m_attrInfo[m_size + i] != 0 || (derived && derived->usesTags())) setDirty(slot);
    }
}

void AttributeSet::clearChanged() noexcept { std::fill(m_bits.begin() + static_cast<isize>(m_words), m_bits.end(), 0); }

ModifierView AttributeSet::view(u32 index) const noexcept {
    const ModSlot& s = m_mods[index];
    ModifierView v;
    v.handle = ModifierHandle{index, s.generation};
    v.attr = s.mod.attr;
    v.op = s.mod.op;
    v.magnitude = s.lastMagnitude;
    v.priority = s.mod.priority;
    v.penaltyGroup = s.mod.penaltyGroup;
    v.exempt = s.mod.exempt;
    v.active = s.lastActive;
    v.sourceId = s.mod.sourceId;
    return v;
}

ModifierView AttributeSet::view(ModifierHandle h) const noexcept {
    if (h.index >= m_mods.size() || !m_mods[h.index].alive || m_mods[h.index].generation != h.generation) return {};
    return view(h.index);
}

f64 AttributeSet::resolveMagnitude(const ModSlot& s, const TagContainer* tags) const noexcept {
    const Modifier& m = s.mod;
    switch (m.kind) {
    case MagnitudeKind::Constant: return m.value;
    case MagnitudeKind::Attribute: return m.value * m_values[m_size + m.source];
    case MagnitudeKind::Formula: return m.formula->evaluate(finalValues(), tags, m.stacks, m.level);
    }
    return std::bit_cast<f64>(kCanonicalNaN);
}

f64 AttributeSet::computeOne(AttrSlot a, const TagContainer* tags, AttributeScratch& s) {
    using Entry = AttributeScratch::Entry;
    const AttributeSpec& spec = m_layout->spec(a);
    f64 v = spec.derived ? spec.derived->evaluate(finalValues(), tags, 1.0, 1.0) : m_values[a];

    s.entries.clear();
    for (u32 mi = m_attrInfo[a]; mi != kNone; mi = m_mods[mi].next) {
        ModSlot& m = m_mods[mi];
        bool active = true;
        if (m.mod.requirement) active = tags ? m.mod.requirement->matches(*tags) : matchesEmpty(*m.mod.requirement);
        f64 mag = active ? resolveMagnitude(m, tags) : 0.0;
        if (active && std::isnan(mag)) {
            active = false;
            ++m_nanMagnitudes;
        }
        m.lastActive = active;
        m.lastMagnitude = mag;
        if (active) s.entries.push_back(Entry{m.mod.op, mag, m.mod.priority, m.mod.penaltyGroup, m.mod.exempt});
    }
    if (!s.entries.empty()) {
        std::sort(s.entries.begin(), s.entries.end(), [](const Entry& x, const Entry& y) {
            if (x.op != y.op) return x.op < y.op;
            return orderKey(x.value) < orderKey(y.value);
        });
        // Consume the op-sorted entries stage by stage, in ModOp order.
        usize cursor = 0;
        auto take = [&](ModOp op) {
            const usize begin = cursor;
            while (cursor < s.entries.size() && s.entries[cursor].op == op) ++cursor;
            return std::span<const Entry>(s.entries.data() + begin, cursor - begin);
        };
        auto best = [](std::span<const Entry> r) {
            const Entry* b = &r[0];
            for (const Entry& e : r) {
                if (e.priority > b->priority || (e.priority == b->priority && orderKey(e.value) > orderKey(b->value))) b = &e;
            }
            return b->value;
        };
        auto product = [](std::span<const Entry> r) {
            f64 p = r[0].value;
            for (usize i = 1; i < r.size(); ++i) p = p * r[i].value;
            return p;
        };
        auto sum = [](std::span<const Entry> r) {
            f64 t = r[0].value;
            for (usize i = 1; i < r.size(); ++i) t = t + r[i].value;
            return t;
        };
        if (auto r = take(ModOp::PreAssign); !r.empty()) v = best(r);
        if (auto r = take(ModOp::PreMul); !r.empty()) v = v * product(r);
        if (auto r = take(ModOp::PreDiv); !r.empty()) v = v / product(r);
        if (auto r = take(ModOp::ModAdd); !r.empty()) v = v + sum(r);
        if (auto r = take(ModOp::ModSub); !r.empty()) v = v - sum(r);

        // Multiplicative factors F (their order does not matter: they are sorted below).
        s.factors.clear();
        for (const Entry& e : take(ModOp::PostMul)) s.factors.push_back(e);
        for (const Entry& e : take(ModOp::PostDiv)) {
            Entry f = e;
            f.value = 1.0 / e.value;
            s.factors.push_back(f);
        }
        for (const Entry& e : take(ModOp::PostPercent)) {
            Entry f = e;
            f.value = 1.0 + e.value / 100.0;
            s.factors.push_back(f);
        }
        if (!s.factors.empty()) {
            if (spec.multiplierMode == MultiplierMode::AdditiveBonus) {
                for (Entry& f : s.factors) f.value = f.value - 1.0;
                std::sort(s.factors.begin(), s.factors.end(),
                          [](const Entry& x, const Entry& y) { return orderKey(x.value) < orderKey(y.value); });
                v = v * (1.0 + sum(s.factors));
            } else {
                const bool penalisedAttr = spec.stackingPenalised;
                auto penalised = [&](const Entry& e) { return penalisedAttr && !e.exempt; };
                std::sort(s.factors.begin(), s.factors.end(), [&](const Entry& x, const Entry& y) {
                    const bool px = penalised(x), py = penalised(y);
                    if (px != py) return !px; // exempt factors first
                    if (!px) return orderKey(x.value) < orderKey(y.value);
                    if (x.group != y.group) return x.group < y.group;
                    const bool mx = x.value < 1.0, my = y.value < 1.0;
                    if (mx != my) return !mx; // bonuses before maluses
                    const u64 dx = orderKey(std::fabs(x.value - 1.0)), dy = orderKey(std::fabs(y.value - 1.0));
                    if (dx != dy) return dx > dy; // strongest first
                    return orderKey(x.value) < orderKey(y.value);
                });
                const usize n = s.factors.size();
                usize i = 0;
                while (i < n && !penalised(s.factors[i])) ++i;
                if (i > 0) v = v * product(std::span<const Entry>(s.factors.data(), i));
                while (i < n) {
                    const u16 group = s.factors[i].group;
                    const bool malus = s.factors[i].value < 1.0;
                    f64 chain = 1.0;
                    bool any = false;
                    u32 k = 0;
                    for (; i < n && s.factors[i].group == group && (s.factors[i].value < 1.0) == malus; ++i) {
                        const f64 f = s.factors[i].value;
                        if (f == 1.0) continue;
                        const f64 term = 1.0 + (f - 1.0) * stackingPenalty(k++);
                        chain = any ? chain * term : term;
                        any = true;
                    }
                    if (any) v = v * chain;
                }
            }
        }
        if (auto r = take(ModOp::PostAssign); !r.empty()) v = best(r);
    }
    auto boundValue = [&](const AttrBound& b) { return b.kind == AttrBound::Kind::Const ? b.value : m_values[m_size + b.attr]; };
    if (spec.minClamp.kind != AttrBound::Kind::None) {
        const f64 lo = boundValue(spec.minClamp);
        if (v < lo) v = lo;
    }
    if (spec.maxClamp.kind != AttrBound::Kind::None) {
        const f64 hi = boundValue(spec.maxClamp);
        if (v > hi) v = hi;
    }
    return v;
}

void AttributeSet::recompute(const TagContainer* tags, AttributeScratch* scratch) {
    if (!m_anyDirty) return;
    HELIOS_ASSERT(!tags || !m_layout->tags() || &tags->registry() == m_layout->tags(),
                  "the tag container must use the layout's tag registry");
    AttributeScratch local;
    AttributeScratch& s = scratch ? *scratch : local;
    if (m_orderDirty) {
        const bool ok = rebuildOrder();
        HELIOS_ASSERT(ok, "removing edges cannot create a cycle");
        (void)ok;
    }
    const std::span<const AttrSlot> order = m_useOwnOrder ? std::span<const AttrSlot>(m_ownOrder) : m_layout->order();
    for (AttrSlot a : order) {
        const u64 bit = u64{1} << (a & 63);
        if ((m_bits[a >> 6] & bit) == 0) continue;
        m_bits[a >> 6] &= ~bit;
        const f64 v = canonical(computeOne(a, tags, s));
        f64& final = m_values[m_size + a];
        if (std::bit_cast<u64>(v) != std::bit_cast<u64>(final)) {
            final = v;
            m_bits[m_words + (a >> 6)] |= bit;
            markDependentsDirty(a);
        }
    }
    m_anyDirty = false;
}

void resolveAttributes(std::span<AttributeSet* const> sets, std::span<const TagContainer* const> tags, jobs::JobSystem* jobs) {
    HELIOS_ASSERT(tags.empty() || tags.size() == sets.size(), "tags must be empty or match sets");
    auto run = [&](u64 begin, u64 end) {
        AttributeScratch scratch;
        for (u64 i = begin; i < end; ++i) {
            // A short tags span (a caller bug, asserted above) reads as "no tags", never out of bounds.
            if (sets[i]) sets[i]->recompute(i < tags.size() ? tags[i] : nullptr, &scratch);
        }
    };
    if (!jobs || sets.size() < 128) {
        run(0, sets.size());
        return;
    }
    jobs->parallelFor(0, sets.size(), 64, run);
}

} // namespace helios::gameplay
