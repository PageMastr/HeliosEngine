#pragma once
// Attributes and modifiers (06 §1.2: EVE Dogma + Unreal GAS).
//
// AttributeLayout is the immutable layout of a record template's attribute set (from AttributeDef
// records): defaults, clamps, derived HXL formulas, replication audiences and the static dependency
// graph. AttributeSet is one entity's AttrBlock: SoA base[] / final[] arrays, a dirty bitset and the
// modifier stacks. recompute() resolves only dirty attributes, in dependency order, and marks an
// attribute's dependents dirty only if its final value actually changed (bitwise).
//
// Aggregation (06 §1.2, normative; `sorted` = ascending IEEE total order of the values, so the
// result never depends on the order in which modifiers were added):
//   v = PreAssign ? PreAssign.highestPriority : base          (ties: the larger value)
//   v = v * Π sorted(PreMul) / Π sorted(PreDiv) + Σ sorted(ModAdd) − Σ sorted(ModSub)
//   F = PostMul ∪ {1 + p/100 : PostPercent} ∪ {1/d : PostDiv}
//   AdditiveBonus: v *= 1 + Σ sorted(f − 1)
//   Product:       v *= Π sorted(exempt f); then per penaltyGroup (ascending), bonuses (f > 1) and
//                  maluses (f < 1) each sorted by |f − 1| descending (ties: f ascending):
//                  v *= Π_i (1 + (f_i − 1)·S(i)),  S(i) = e^{−(i/2.67)²}
//                  (exempt modifiers, or all of them when the attribute is not stackingPenalised,
//                  use S ≡ 1 and do not take a place in the chain; f == 1 is skipped)
//   v = PostAssign ? PostAssign.highestPriority : v;   final = clamp(v, minClamp, maxClamp)
// The clamp is `if (v < min) v = min; if (v > max) v = max` (IEEE compares, unlike HXL's clamp()):
// a NaN v passes through (and is made canonical), -0 stays -0 against a min of +0, and a bound that
// reads a NaN attribute clamps nothing. Constant bounds must be ordered and not NaN (build() fails).
// Empty stages are skipped entirely (so -0 and exact values pass through untouched). Every product
// and sum is an explicit left-to-right fold (products Π and sums Σ are computed first, then applied
// once), all transcendental math is helios::det, and FP contraction is off, so final values are
// bit-identical on MSVC, GCC, Clang and MinGW. A NaN magnitude disables its modifier (counted in
// nanMagnitudes()); a NaN final value is canonical (0x7ff8000000000000).
//
// Magnitudes: constants, live reads of another attribute of the same entity (x coefficient), and
// live HXL formulas over `self` (06 §1.2; formula stacks()/level() come from the modifier).
// Snapshot, curve and source-entity magnitudes are captured to constants by instantiateModifier().
// Live magnitudes and derived formulas add dependency edges; a cycle is rejected when the modifier
// is added (layouts reject static cycles when built).
//
// Replication (06 §1.6): finalValues() plus the changed() bitset (per-attribute audience in the
// spec) and forEachModifier() views (source id, op, resolved magnitude, active flag) for owners.
//
// Budget (06 §12.2 GP-1): a 300-modifier ship recompute <= 50 us, one incremental change <= 5 us,
// 10k entities x 40 attributes at 5 % dirty <= 1 ms on 8 workers (resolveAttributes); the
// benchmarks are in engine/gameplay/tests/test_attributes_perf.cpp.
//
// Threading: AttributeLayout and BoundFormula are immutable and shareable. An AttributeSet has one
// writer; different sets may be recomputed concurrently (resolveAttributes does exactly that).
//
// FP environment: recompute() and the HXL VM require the default floating-point environment (round
// to nearest, no flush-to-zero or denormals-are-zero); other modes change results, and the HXL
// corpus fails under them. Threads that change the mode must restore it before resolving.

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "gameplay/attributes.gen.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/gameplay/tags.h"
#include "helios/hxl/hxl.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::gameplay {

using AttrSlot = u16;
inline constexpr AttrSlot kInvalidAttr = 0xFFFF;
inline constexpr u32 kMaxAttributes = 4096;

/// S(i) = e^{-(i/2.67)^2} (06 §1.2), computed with helios::det: bit-identical everywhere and equal
/// to HXL's `exp(-((i / 2.67) ^ 2))` (the corpus pins S(0..8)).
f64 stackingPenalty(u32 index) noexcept;

/// A clamp bound: a constant or another attribute's final value.
struct AttrBound {
    enum class Kind : u8 { None, Const, Attr };
    Kind kind = Kind::None;
    f64 value = 0.0;
    AttrSlot attr = kInvalidAttr;
};

class AttributeLayout;
class AttributeSet;

/// An HXL formula bound to one layout: its only parameter (`self`) is the entity, attr(self, X)
/// reads X's final value, tag(self, T) tests the entity's tags. Immutable; shareable.
class BoundFormula {
public:
    const hxl::Program& program() const noexcept { return m_program; }
    /// Attribute slot of each attr symbol of the program.
    std::span<const AttrSlot> attrSlots() const noexcept { return m_attrSlots; }
    /// Tag of each tag symbol of the program.
    std::span<const TagIndex> tagIndices() const noexcept { return m_tags; }
    bool usesTags() const noexcept { return !m_tags.empty(); }
    /// Hash of the layout it was bound to (formulas only work with that layout).
    u64 layoutHash() const noexcept { return m_layoutHash; }
    /// Evaluates against final values (and optional tags: without them every tag() is false).
    /// Never fails for a bound formula; returns the canonical NaN on a VM error.
    f64 evaluate(std::span<const f64> finals, const TagContainer* tags, f64 stacks, f64 level) const noexcept;

private:
    friend class AttributeLayout;
    hxl::Program m_program;
    std::vector<AttrSlot> m_attrSlots;
    std::vector<TagIndex> m_tags;
    u64 m_layoutHash = 0;
};

/// Runtime form of an AttributeDef.
struct AttributeSpec {
    std::string id;
    refl::RecordId rid = 0;
    f64 defaultValue = 0.0;
    AttrBound minClamp;
    AttrBound maxClamp;
    std::shared_ptr<const BoundFormula> derived; ///< base computed from other attributes
    MultiplierMode multiplierMode = MultiplierMode::Product;
    bool stackingPenalised = false;
    Audience replicate = Audience::Server;
    std::optional<Quantization> quant;
    bool persistBase = false;
};

/// The immutable attribute layout of a record template (06 §1.2 AttributeSetDef).
class AttributeLayout {
public:
    /// One attribute for build(): clamps name other attributes by id.
    struct Input {
        std::string id;
        refl::RecordId rid = 0;
        f64 defaultValue = 0.0;
        std::optional<std::variant<f64, std::string>> minClamp;
        std::optional<std::variant<f64, std::string>> maxClamp;
        std::string derived; ///< HXL source over `self`; empty = none
        MultiplierMode multiplierMode = MultiplierMode::Product;
        bool stackingPenalised = false;
        Audience replicate = Audience::Server;
        std::optional<Quantization> quant;
        bool persistBase = false;
    };
    /// An AttributeDef with its RecordId (clamp AttributeRefs resolve against the set's records).
    struct Record {
        refl::RecordId rid = 0;
        const AttributeDef* def = nullptr;
    };

    /// Validates ids, clamps and derived formulas (compiled with HXL, `self` only; tags need
    /// `tags`), and orders the static dependency graph. Slots follow the input order.
    static Result<std::shared_ptr<const AttributeLayout>> build(std::span<const Input> inputs,
                                                                std::shared_ptr<const TagRegistry> tags = nullptr);
    static Result<std::shared_ptr<const AttributeLayout>> fromRecords(std::span<const Record> records,
                                                                      std::shared_ptr<const TagRegistry> tags = nullptr);

    /// Number of attributes (slots [0, size())).
    usize size() const noexcept { return m_specs.size(); }
    /// Slot of an attribute id ("Hull.Hp"), or kInvalidAttr. No allocation.
    AttrSlot find(std::string_view id) const noexcept;
    /// Slot of the attribute with this RecordId, or kInvalidAttr (also for 0). O(n).
    AttrSlot findByRecord(refl::RecordId rid) const noexcept;
    /// Runtime spec of a valid slot (unchecked, like the other slot accessors).
    const AttributeSpec& spec(AttrSlot slot) const noexcept { return m_specs[slot]; }
    /// Deterministic topological order of the static graph (derived and clamp edges).
    std::span<const AttrSlot> order() const noexcept { return m_order; }
    /// Attributes whose derived formula or clamp reads `slot`.
    std::span<const AttrSlot> staticDependents(AttrSlot slot) const noexcept { return m_dependents[slot]; }
    const TagRegistry* tags() const noexcept { return m_tagRegistry.get(); }
    /// FNV-1a 64 over ids, defaults, clamps, formulas, flags and the tag registry's hash.
    u64 hash() const noexcept { return m_hash; }

    /// Binds a compiled program with exactly one parameter to this layout. Attribute and tag names
    /// must exist; context fields and curves are not available; stacks()/level() only if allowed.
    Result<std::shared_ptr<const BoundFormula>> bindFormula(const hxl::Program& program, bool allowContext = true) const;
    /// Compiles (params {"self"}, number result) and binds.
    Result<std::shared_ptr<const BoundFormula>> compileFormula(std::string_view source, bool allowContext = true) const;

private:
    std::vector<AttributeSpec> m_specs;
    struct IdHash {
        using is_transparent = void;
        usize operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, AttrSlot, IdHash, std::equal_to<>> m_byId;
    std::vector<AttrSlot> m_order;
    std::vector<std::vector<AttrSlot>> m_dependents;
    std::shared_ptr<const TagRegistry> m_tagRegistry;
    u64 m_hash = 0;
};

/// Generational handle of a modifier inside one AttributeSet.
struct ModifierHandle {
    u32 index = 0xFFFFFFFFu;
    u32 generation = 0;
    bool valid() const noexcept { return index != 0xFFFFFFFFu; }
    friend bool operator==(const ModifierHandle&, const ModifierHandle&) = default;
};

enum class MagnitudeKind : u8 {
    Constant,  ///< `value`
    Attribute, ///< `value` x final value of `source` (same entity, read live)
    Formula,   ///< live HXL over self
};

/// A runtime modifier (06 §1.2).
struct Modifier {
    AttrSlot attr = kInvalidAttr;
    ModOp op = ModOp::ModAdd;
    MagnitudeKind kind = MagnitudeKind::Constant;
    f64 value = 0.0;                             ///< Constant value, or Attribute coefficient
    AttrSlot source = kInvalidAttr;              ///< Attribute magnitude
    std::shared_ptr<const BoundFormula> formula; ///< Formula magnitude
    f64 stacks = 1.0;                            ///< stacks() / level() of a formula magnitude
    f64 level = 1.0;
    i32 priority = 0;                            ///< PreAssign / PostAssign: highest wins
    u16 penaltyGroup = 0;
    bool exempt = false;
    /// Active only while the entity's tags match. Held by shared_ptr so copies of one modifier share
    /// the compiled query (instantiateModifier compiles a new one on every call); it must be
    /// compiled against the layout's tag registry (addModifier checks TagQuery::registry).
    std::shared_ptr<const TagQuery> requirement;
    u64 sourceId = 0;                            ///< owning effect / item / skill (removal, UI)
    /// AttributeLayout::hash() of the layout `attr` and `source` are slots of. instantiateModifier
    /// sets it and addModifier rejects a mismatch; 0 (modifiers built in code) is not checked.
    u64 layoutHash = 0;

    /// A constant magnitude.
    static Modifier constant(AttrSlot attr, ModOp op, f64 value, u64 sourceId = 0);
    /// coefficient x the live final value of `source` (same entity).
    static Modifier fromAttribute(AttrSlot attr, ModOp op, AttrSlot source, f64 coefficient = 1.0, u64 sourceId = 0);
    /// A live HXL magnitude bound to the set's layout (AttributeLayout::compileFormula).
    static Modifier fromFormula(AttrSlot attr, ModOp op, std::shared_ptr<const BoundFormula> formula, u64 sourceId = 0);
};

/// Inputs for turning a ModifierDef record into a Modifier.
struct ModifierContext {
    const AttributeLayout* layout = nullptr; ///< the target's layout (required)
    const AttributeSet* target = nullptr;    ///< for Target/Snapshot magnitudes; must use `layout`
    const AttributeSet* source = nullptr;    ///< for Source magnitudes (captured at apply)
    const std::unordered_map<std::string, hxl::Curve>* curves = nullptr; ///< for Curve magnitudes
    f64 stacks = 1.0;
    f64 level = 1.0;
    u64 sourceId = 0;
};

/// Instantiates a ModifierDef: resolves the attribute, compiles the requirement, and captures
/// Snapshot/Source/Curve magnitudes (Target+Live attribute and HXL magnitudes stay live). Only the
/// Self domain is supported in Phase 0 (Unsupported otherwise). The result carries the layout's
/// hash. It compiles the requirement and HXL formula on every call (~3 us); cache the Modifier to
/// apply one definition many times.
/// Threading: reads ctx.target's and ctx.source's final values, so neither may be recomputed
/// concurrently; otherwise reentrant.
Result<Modifier> instantiateModifier(const ModifierDef& def, const ModifierContext& ctx);

/// Replication and UI view of a modifier (06 §1.6: the owner sees the source modifiers).
struct ModifierView {
    ModifierHandle handle;
    AttrSlot attr = kInvalidAttr;
    ModOp op = ModOp::ModAdd;
    f64 magnitude = 0.0; ///< as resolved by the last recompute
    i32 priority = 0;
    u16 penaltyGroup = 0;
    bool exempt = false;
    bool active = false; ///< requirement met and magnitude not NaN at the last recompute
    u64 sourceId = 0;
};

/// Reusable scratch memory for recompute() (one per worker thread).
struct AttributeScratch {
    struct Entry {
        ModOp op;
        f64 value;
        i32 priority;
        u16 group;
        bool exempt;
    };
    std::vector<Entry> entries;
    std::vector<Entry> factors;
};

/// One entity's attributes (06 §1.2 AttrBlock).
class AttributeSet {
public:
    explicit AttributeSet(std::shared_ptr<const AttributeLayout> layout);

    /// The layout the slots refer to.
    const AttributeLayout& layout() const noexcept { return *m_layout; }
    const std::shared_ptr<const AttributeLayout>& layoutPtr() const noexcept { return m_layout; }
    /// Number of attributes (layout().size()).
    usize size() const noexcept { return m_size; }

    /// Base value; `slot` must be a valid slot of the layout.
    f64 base(AttrSlot slot) const noexcept { return m_values[slot]; }
    /// Final value as of the last recompute(); `slot` must be a valid slot of the layout.
    f64 value(AttrSlot slot) const noexcept { return m_values[m_size + slot]; }
    std::span<const f64> finalValues() const noexcept { return std::span<const f64>(m_values).subspan(m_size); }
    /// Sets a base value (derived attributes ignore theirs) and marks the attribute dirty if it
    /// changed. Returns false (and changes nothing) for a slot outside the layout, such as
    /// kInvalidAttr from a failed find().
    bool setBase(AttrSlot slot, f64 value);

    /// Adds a modifier. Fails on a bad slot, a modifier instantiated for another layout, a formula
    /// bound to another layout, a requirement compiled against another tag registry, or a
    /// dependency cycle (e.g. an attribute whose magnitude reads itself).
    Result<ModifierHandle> addModifier(Modifier modifier);
    /// Removes a modifier; false for a stale handle.
    bool removeModifier(ModifierHandle handle);
    /// Removes every modifier with this sourceId; returns how many. Note that 0 is the sourceId of
    /// every modifier added without one.
    usize removeModifiersFromSource(u64 sourceId);
    /// Number of live modifiers.
    usize modifierCount() const noexcept { return m_liveMods; }

    /// Call when the entity's tags changed: marks attributes with tag requirements or tag-reading
    /// formulas dirty.
    void onTagsChanged();
    /// Marks everything dirty (e.g. after hot reload of records).
    void markAllDirty();

    /// True if recompute() has work to do.
    bool dirty() const noexcept { return m_anyDirty; }
    /// True if the attribute will be recomputed by the next recompute(); `slot` must be valid.
    bool isDirty(AttrSlot slot) const noexcept { return (m_bits[slot >> 6] >> (slot & 63)) & 1; }
    /// Resolves dirty attributes in dependency order. `tags` answers requirements and tag() in
    /// formulas (nullptr: no tags held); it must use the layout's tag registry.
    void recompute(const TagContainer* tags = nullptr, AttributeScratch* scratch = nullptr);

    /// Attributes whose final value changed since the last clearChanged() (replication dirty bits).
    bool changed(AttrSlot slot) const noexcept { return (m_bits[m_words + (slot >> 6)] >> (slot & 63)) & 1; }
    std::span<const u64> changedBits() const noexcept { return std::span<const u64>(m_bits).subspan(m_words); }
    /// Clears the changed bits (after replication has consumed them).
    void clearChanged() noexcept;

    /// Visits the modifiers of one attribute (or all when slot == kInvalidAttr) in handle order.
    template <class F>
    void forEachModifier(AttrSlot slot, F&& fn) const {
        for (u32 i = 0; i < m_mods.size(); ++i) {
            const ModSlot& m = m_mods[i];
            if (!m.alive || (slot != kInvalidAttr && m.mod.attr != slot)) continue;
            fn(view(i));
        }
    }
    ModifierView view(ModifierHandle handle) const noexcept;

    /// Modifiers skipped because their magnitude was NaN (diagnostics).
    u64 nanMagnitudes() const noexcept { return m_nanMagnitudes; }

private:
    static constexpr u32 kNone = 0xFFFFFFFFu;
    struct ModSlot {
        Modifier mod;
        f64 lastMagnitude = 0.0;
        u32 generation = 0;
        u32 next = kNone; ///< next modifier of the same attribute (intrusive list)
        bool alive = false;
        bool lastActive = false;
    };

    void setDirty(AttrSlot slot) noexcept;
    void markDependentsDirty(AttrSlot slot) noexcept;
    ModifierView view(u32 index) const noexcept;
    void dependencies(const Modifier& m, std::vector<AttrSlot>& out) const;
    bool addEdge(AttrSlot from, AttrSlot to);   // true if the edge is new
    void removeEdge(AttrSlot from, AttrSlot to); // decrements; drops at zero
    bool rebuildOrder();                         // false on a cycle
    f64 resolveMagnitude(const ModSlot& m, const TagContainer* tags) const noexcept;
    f64 computeOne(AttrSlot slot, const TagContainer* tags, AttributeScratch& scratch);

    // Few, contiguous allocations per entity (10k-entity resolves are cache-miss bound).
    std::shared_ptr<const AttributeLayout> m_layout;
    usize m_size = 0;
    usize m_words = 0;
    std::vector<f64> m_values;       // [0, n) base, [n, 2n) final
    std::vector<u64> m_bits;         // [0, w) dirty, [w, 2w) changed
    std::vector<u32> m_attrInfo;     // [0, n) first modifier (kNone), [n, 2n) tag-sensitive modifier count
    std::vector<ModSlot> m_mods;
    std::vector<u32> m_freeMods;
    std::vector<std::vector<std::pair<AttrSlot, u32>>> m_dynDependents; // from -> (to, refcount); empty until used
    std::vector<AttrSlot> m_ownOrder; // used once live magnitudes add edges; else the layout's order
    bool m_useOwnOrder = false;
    usize m_liveMods = 0;
    u64 m_nanMagnitudes = 0;
    bool m_anyDirty = true;
    bool m_orderDirty = false;
};

/// Recomputes many sets, in parallel on `jobs` when given (chunks of 64 sets, one scratch per
/// chunk; fewer than 128 sets always run on the calling thread). tags[i] belongs to sets[i] (the
/// span may be empty: no tags). The sets must be distinct objects (a set listed twice would be
/// recomputed by two threads at once). Deterministic: each set's result is independent of
/// scheduling. Null entries are skipped.
void resolveAttributes(std::span<AttributeSet* const> sets, std::span<const TagContainer* const> tags,
                       jobs::JobSystem* jobs = nullptr);

} // namespace helios::gameplay
