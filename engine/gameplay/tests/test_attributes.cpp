// Attributes and modifiers (06 §1.2): Dogma operator order, stacking penalties, GAS additive mode,
// clamps, derived attributes, dirty propagation, live magnitudes, cycles, determinism.
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <random>

#include "helios/core/assert.h"
#include "helios/core/jobs.h"
#include "helios/gameplay/attributes.h"

using namespace helios;
using namespace helios::gameplay;

namespace {

u64 bits(f64 v) { return std::bit_cast<u64>(v); }

std::shared_ptr<const AttributeLayout> layoutOf(std::vector<AttributeLayout::Input> inputs,
                                                std::shared_ptr<const TagRegistry> tags = nullptr) {
    auto l = AttributeLayout::build(inputs, std::move(tags));
    REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
    return *l;
}

AttributeLayout::Input attr(std::string id, f64 def = 0.0) {
    AttributeLayout::Input in;
    in.id = std::move(id);
    in.defaultValue = def;
    return in;
}

ModifierHandle add(AttributeSet& s, Modifier m) {
    auto h = s.addModifier(std::move(m));
    REQUIRE_MESSAGE(h.ok(), (h.ok() ? std::string() : h.error().message));
    return *h;
}

} // namespace

TEST_CASE("attributes: stacking penalty series S(i) = exp(-(i/2.67)^2)") {
    // Bit-identical with the HXL corpus (formula.stacking_penalty_series).
    const u64 expected[] = {0x3ff0000000000000ull, 0x3febcfd4b4bacfb6ull, 0x3fe2423794a0dafaull, 0x3fd21befef30ee5full,
                            0x3fbb22559442e46eull, 0x3f9eb6011db7a219ull, 0x3f7a41906d9e6c8bull, 0x3f50f4c56821237bull,
                            0x3f208afeb01063e5ull};
    for (u32 i = 0; i < 9; ++i) CHECK(bits(stackingPenalty(i)) == expected[i]);
    // EVE's published series: 100 %, 86.9 %, 57.1 %, 28.3 %, 10.6 %, 3.0 %.
    CHECK(stackingPenalty(1) == doctest::Approx(0.869119980800).epsilon(1e-11));
    CHECK(stackingPenalty(3) == doctest::Approx(0.282955154023).epsilon(1e-11));
    CHECK(stackingPenalty(100) == 0.0); // underflows
    CHECK(bits(stackingPenalty(70)) == bits(stackingPenalty(70)));
}

TEST_CASE("attributes: Dogma operator order") {
    auto layout = layoutOf({attr("Speed", 10.0)});
    AttributeSet s(layout);
    s.recompute();
    CHECK(s.value(0) == 10.0);
    add(s, Modifier::constant(0, ModOp::PreMul, 2.0));
    add(s, Modifier::constant(0, ModOp::PreDiv, 4.0));
    add(s, Modifier::constant(0, ModOp::ModAdd, 3.0));
    add(s, Modifier::constant(0, ModOp::ModSub, 1.0));
    add(s, Modifier::constant(0, ModOp::PostMul, 1.5));
    add(s, Modifier::constant(0, ModOp::PostPercent, 20.0));
    add(s, Modifier::constant(0, ModOp::PostDiv, 2.0));
    s.recompute();
    // v = 10 * 2 / 4 + 3 - 1 = 7; F = {1.5, 1.2, 0.5}, not penalised: exempt product in sorted order.
    const f64 f = (0.5 * (1.0 + 20.0 / 100.0)) * 1.5;
    CHECK(bits(s.value(0)) == bits(7.0 * f));
    // PreAssign replaces the base before everything else; PostAssign replaces the result.
    const auto pre = add(s, Modifier::constant(0, ModOp::PreAssign, 100.0));
    s.recompute();
    CHECK(bits(s.value(0)) == bits((100.0 * 2.0 / 4.0 + 3.0 - 1.0) * f));
    auto post = Modifier::constant(0, ModOp::PostAssign, 5.0);
    post.priority = 1;
    const auto postH = add(s, post);
    add(s, Modifier::constant(0, ModOp::PostAssign, 7.0)); // lower priority
    s.recompute();
    CHECK(s.value(0) == 5.0);
    CHECK(s.removeModifier(postH));
    s.recompute();
    CHECK(s.value(0) == 7.0);
    add(s, Modifier::constant(0, ModOp::PostAssign, 9.0)); // same priority: the larger value wins
    s.recompute();
    CHECK(s.value(0) == 9.0);
    CHECK(s.removeModifier(pre));
    CHECK_FALSE(s.removeModifier(pre)); // stale handle
}

TEST_CASE("attributes: sums and products are exact folds that skip empty stages") {
    auto layout = layoutOf({attr("A", -0.0), attr("B", 1.0)});
    AttributeSet s(layout);
    s.recompute();
    CHECK(bits(s.value(0)) == bits(-0.0)); // no stage touched -0
    add(s, Modifier::constant(1, ModOp::ModAdd, 0.1));
    add(s, Modifier::constant(1, ModOp::ModAdd, 0.2));
    add(s, Modifier::constant(1, ModOp::ModAdd, 0.3));
    s.recompute();
    CHECK(bits(s.value(1)) == bits(1.0 + ((0.1 + 0.2) + 0.3))); // Σ first, then applied once
}

TEST_CASE("attributes: AdditiveBonus sums bonuses (GAS): two 1.5x give 2.0x") {
    auto in = attr("Damage", 100.0);
    in.multiplierMode = MultiplierMode::AdditiveBonus;
    auto layout = layoutOf({in});
    AttributeSet s(layout);
    add(s, Modifier::constant(0, ModOp::PostMul, 1.5));
    add(s, Modifier::constant(0, ModOp::PostMul, 1.5));
    s.recompute();
    CHECK(s.value(0) == 200.0);
    add(s, Modifier::constant(0, ModOp::PostPercent, -25.0));
    s.recompute();
    CHECK(s.value(0) == 175.0);
}

TEST_CASE("attributes: EVE stacking penalties per group, bonuses and maluses separately") {
    auto in = attr("Damage", 100.0);
    in.stackingPenalised = true;
    auto layout = layoutOf({in});
    AttributeSet s(layout);
    for (int i = 0; i < 3; ++i) add(s, Modifier::constant(0, ModOp::PostMul, 1.1));
    s.recompute();
    const f64 t0 = 1.0 + (1.1 - 1.0) * stackingPenalty(0);
    const f64 t1 = 1.0 + (1.1 - 1.0) * stackingPenalty(1);
    const f64 t2 = 1.0 + (1.1 - 1.0) * stackingPenalty(2);
    CHECK(bits(s.value(0)) == bits(100.0 * ((t0 * t1) * t2)));
    CHECK(s.value(0) == doctest::Approx(100.0 * 1.1 * 1.0869119980800 * 1.0570583143511).epsilon(1e-12));

    // Strongest first: 1.25 takes S(0), then 1.1 S(1). A malus chain is independent.
    AttributeSet m(layout);
    add(m, Modifier::constant(0, ModOp::PostMul, 1.1));
    add(m, Modifier::constant(0, ModOp::PostPercent, 25.0));
    add(m, Modifier::constant(0, ModOp::PostMul, 0.8));
    add(m, Modifier::constant(0, ModOp::PostMul, 0.9));
    m.recompute();
    const f64 bonus = (1.0 + (1.25 - 1.0) * stackingPenalty(0)) * (1.0 + (1.1 - 1.0) * stackingPenalty(1));
    const f64 malus = (1.0 + (0.8 - 1.0) * stackingPenalty(0)) * (1.0 + (0.9 - 1.0) * stackingPenalty(1));
    CHECK(bits(m.value(0)) == bits((100.0 * bonus) * malus));

    // Different penalty groups do not penalise each other; exempt factors are outside the chain.
    AttributeSet g(layout);
    auto a = Modifier::constant(0, ModOp::PostMul, 1.1);
    auto b = Modifier::constant(0, ModOp::PostMul, 1.1);
    b.penaltyGroup = 1;
    auto e = Modifier::constant(0, ModOp::PostMul, 1.5);
    e.exempt = true;
    add(g, a);
    add(g, b);
    add(g, e);
    g.recompute();
    const f64 one = 1.0 + (1.1 - 1.0) * stackingPenalty(0);
    CHECK(bits(g.value(0)) == bits(((100.0 * 1.5) * one) * one));

    // Not stacking-penalised: everything multiplies fully.
    auto plainLayout = layoutOf({attr("Damage", 100.0)});
    AttributeSet p(plainLayout);
    for (int i = 0; i < 3; ++i) add(p, Modifier::constant(0, ModOp::PostMul, 1.1));
    p.recompute();
    CHECK(bits(p.value(0)) == bits(100.0 * ((1.1 * 1.1) * 1.1)));
}

TEST_CASE("attributes: results do not depend on the order modifiers were added") {
    auto in = attr("X", 3.0);
    in.stackingPenalised = true;
    auto layout = layoutOf({in});
    std::mt19937 rng(1234);
    std::vector<Modifier> mods;
    const ModOp ops[] = {ModOp::PreMul, ModOp::PreDiv, ModOp::ModAdd, ModOp::ModSub, ModOp::PostMul, ModOp::PostDiv, ModOp::PostPercent};
    for (int i = 0; i < 40; ++i) {
        std::uniform_real_distribution<f64> d(0.5, 1.7);
        const ModOp op = ops[rng() % std::size(ops)];
        const f64 value = d(rng);
        auto m = Modifier::constant(0, op, value);
        m.penaltyGroup = static_cast<u16>(rng() % 3);
        m.exempt = (rng() % 5) == 0;
        mods.push_back(m);
    }
    u64 first = 0;
    for (int perm = 0; perm < 20; ++perm) {
        std::shuffle(mods.begin(), mods.end(), rng);
        AttributeSet s(layout);
        for (const auto& m : mods) add(s, m);
        s.recompute();
        if (perm == 0) first = bits(s.value(0));
        CHECK(bits(s.value(0)) == first);
    }
}

TEST_CASE("attributes: clamps by constant and by another attribute") {
    auto maxIn = attr("Shield.Max", 100.0);
    auto curIn = attr("Shield.Current", 80.0);
    curIn.minClamp = 0.0;
    curIn.maxClamp = std::string("Shield.Max");
    auto layout = layoutOf({curIn, maxIn});
    AttributeSet s(layout);
    s.recompute();
    CHECK(s.value(0) == 80.0);
    s.setBase(0, 150.0);
    s.recompute();
    CHECK(s.value(0) == 100.0);
    add(s, Modifier::constant(1, ModOp::PostPercent, 20.0)); // Max -> 120: Current follows
    s.recompute();
    CHECK(s.value(1) == 120.0);
    CHECK(s.value(0) == 120.0);
    s.setBase(0, -5.0);
    s.recompute();
    CHECK(s.value(0) == 0.0);
    // Unknown clamp attribute.
    auto bad = attr("A");
    bad.maxClamp = std::string("Nope");
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{bad}).errorCode() == ErrorCode::NotFound);
}

TEST_CASE("attributes: derived attributes and minimal dirty propagation") {
    auto ehp = attr("EffectiveHp");
    ehp.derived = "attr(self, Hull.Hp) / (1 - attr(self, Hull.Resist))";
    auto layout = layoutOf({ehp, attr("Hull.Hp", 4200.0), attr("Hull.Resist", 0.25), attr("Speed", 5.0)});
    AttributeSet s(layout);
    s.recompute();
    CHECK(s.value(0) == 4200.0 / (1.0 - 0.25));
    s.clearChanged();
    s.setBase(3, 6.0); // unrelated
    s.recompute();
    CHECK(s.changed(3));
    CHECK_FALSE(s.changed(0));
    s.clearChanged();
    add(s, Modifier::constant(2, ModOp::ModAdd, 0.25));
    s.recompute();
    CHECK(s.changed(2));
    CHECK(s.changed(0));
    CHECK(s.value(0) == 4200.0 / (1.0 - 0.5));
    s.clearChanged();
    s.setBase(1, 4200.0); // same value: nothing changes
    s.recompute();
    for (AttrSlot i = 0; i < 4; ++i) CHECK_FALSE(s.changed(i));
    CHECK_FALSE(s.dirty());
    // Derived formulas: bad formulas and cycles are layout errors.
    auto self = attr("Loop");
    self.derived = "attr(self, Loop) + 1";
    CHECK_FALSE(AttributeLayout::build(std::vector<AttributeLayout::Input>{self}).ok());
    auto a = attr("A"), b = attr("B");
    a.derived = "attr(self, B)";
    b.derived = "attr(self, A)";
    auto cycle = AttributeLayout::build(std::vector<AttributeLayout::Input>{a, b});
    REQUIRE_FALSE(cycle.ok());
    CHECK(cycle.error().message.find("cycle") != std::string::npos);
    auto ctx = attr("C");
    ctx.derived = "stacks() * 2";
    CHECK_FALSE(AttributeLayout::build(std::vector<AttributeLayout::Input>{ctx}).ok());
    auto syntax = attr("D");
    syntax.derived = "1 +";
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{syntax}).errorCode() == ErrorCode::ParseError);
    auto unknown = attr("E");
    unknown.derived = "attr(self, Nope)";
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{unknown}).errorCode() == ErrorCode::NotFound);
}

TEST_CASE("attributes: live attribute and formula magnitudes, cycles rejected") {
    auto layout = layoutOf({attr("Cpu.Max", 100.0), attr("Skill.Electronics", 5.0), attr("Cpu.Bonus", 0.0)});
    AttributeSet s(layout);
    // Cpu.Max *= 1 + 5 % per skill level, via a live formula.
    auto f = layout->compileFormula("1 + 0.05 * attr(self, Skill.Electronics)");
    REQUIRE(f.ok());
    add(s, Modifier::fromFormula(0, ModOp::PostMul, *f));
    // Cpu.Bonus = 0.5 x Skill.Electronics, via a live attribute read.
    add(s, Modifier::fromAttribute(2, ModOp::ModAdd, 1, 0.5));
    s.recompute();
    CHECK(s.value(0) == 100.0 * (1.0 + 0.05 * 5.0));
    CHECK(s.value(2) == 2.5);
    s.setBase(1, 4.0);
    s.recompute();
    CHECK(s.value(0) == 100.0 * (1.0 + 0.05 * 4.0));
    CHECK(s.value(2) == 2.0);
    // Skill.Electronics reading Cpu.Max would close a cycle.
    CHECK_FALSE(s.addModifier(Modifier::fromAttribute(1, ModOp::ModAdd, 0)).ok());
    CHECK_FALSE(s.addModifier(Modifier::fromAttribute(2, ModOp::ModAdd, 2)).ok()); // self read
    s.recompute();
    CHECK(s.value(0) == 100.0 * (1.0 + 0.05 * 4.0)); // the failed adds changed nothing
    CHECK(s.modifierCount() == 2);
    // A formula bound to another layout is rejected.
    auto other = layoutOf({attr("Skill.Electronics", 1.0)});
    auto foreign = other->compileFormula("attr(self, Skill.Electronics)");
    REQUIRE(foreign.ok());
    CHECK_FALSE(s.addModifier(Modifier::fromFormula(0, ModOp::ModAdd, *foreign)).ok());
    CHECK_FALSE(s.addModifier(Modifier::constant(7, ModOp::ModAdd, 1.0)).ok());
}

TEST_CASE("attributes: tag requirements and tag-reading formulas follow the tags") {
    TagRegistry::Builder tb;
    REQUIRE(tb.add("State.Combat").ok());
    REQUIRE(tb.add("Ship.Class.Frigate").ok());
    tb.markAllHot();
    auto tags = *tb.build();
    auto layout = layoutOf({attr("Shield.Regen", 10.0), attr("Agility", 1.0)}, tags);
    AttributeSet s(layout);
    TagContainer held(*tags);
    auto noRegen = Modifier::constant(0, ModOp::PostMul, 0.25);
    noRegen.requirement = std::make_shared<const TagQuery>(*tags->compileQuery("all(State.Combat)"));
    add(s, noRegen);
    auto f = layout->compileFormula("select(tag(self, Ship.Class), 1.5, 1)");
    REQUIRE(f.ok());
    add(s, Modifier::fromFormula(1, ModOp::PostMul, *f));
    s.recompute(&held);
    CHECK(s.value(0) == 10.0);
    CHECK(s.value(1) == 1.0);
    held.add(tags->find("State.Combat"));
    held.add(tags->find("Ship.Class.Frigate"));
    s.onTagsChanged();
    s.recompute(&held);
    CHECK(s.value(0) == 2.5);
    CHECK(s.value(1) == 1.5);
    usize active = 0;
    s.forEachModifier(0, [&](const ModifierView& v) { active += v.active ? 1 : 0; });
    CHECK(active == 1);
    // Without a container requirements see no tags.
    s.markAllDirty();
    s.recompute(nullptr);
    CHECK(s.value(0) == 10.0);
}

TEST_CASE("attributes: sources, views and NaN handling") {
    auto layout = layoutOf({attr("A", 1.0), attr("B", 2.0)});
    AttributeSet s(layout);
    const auto h1 = add(s, Modifier::constant(0, ModOp::ModAdd, 1.0, 77));
    add(s, Modifier::constant(1, ModOp::ModAdd, 1.0, 77));
    add(s, Modifier::constant(1, ModOp::ModAdd, 5.0, 78));
    s.recompute();
    const ModifierView v = s.view(h1);
    CHECK(v.active);
    CHECK(v.magnitude == 1.0);
    CHECK(v.sourceId == 77);
    CHECK(s.removeModifiersFromSource(77) == 2);
    CHECK_FALSE(s.view(h1).handle.valid());
    s.recompute();
    CHECK(s.value(0) == 1.0);
    CHECK(s.value(1) == 7.0);
    // Handles are generational: a reused slot does not answer to the old handle.
    const auto h2 = add(s, Modifier::constant(0, ModOp::ModAdd, 2.0));
    const auto h3 = add(s, Modifier::constant(0, ModOp::ModAdd, 0.0));
    const ModifierHandle reused = h2.index == h1.index ? h2 : h3;
    REQUIRE(reused.index == h1.index);
    CHECK(reused.generation != h1.generation);
    CHECK_FALSE(s.removeModifier(h1));
    CHECK(s.view(reused).handle.valid());
    // A NaN magnitude disables its modifier; a NaN base gives the canonical NaN.
    add(s, Modifier::constant(0, ModOp::ModAdd, std::numeric_limits<f64>::quiet_NaN()));
    s.recompute();
    CHECK(s.value(0) == 3.0);
    CHECK(s.nanMagnitudes() == 1);
    s.setBase(1, -std::numeric_limits<f64>::quiet_NaN());
    s.recompute();
    CHECK(bits(s.value(1)) == 0x7ff8000000000000ull);
}

TEST_CASE("attributes: ModifierDef records instantiate against a layout") {
    std::vector<AttributeDef> defs(3);
    defs[0].id = Name("Hull.Hp");
    defs[0].default_ = 1000.0;
    defs[1].id = Name("Hull.Resist");
    defs[1].default_ = 0.2;
    defs[2].id = Name("Hull.Max");
    defs[2].default_ = 2000.0;
    defs[0].maxClamp = AttrOrConst{AttrOrConstAttr{AttributeRef(103)}};
    std::vector<AttributeLayout::Record> recs = {{101, &defs[0]}, {102, &defs[1]}, {103, &defs[2]}};
    auto layout = AttributeLayout::fromRecords(recs);
    REQUIRE_MESSAGE(layout.ok(), (layout.ok() ? std::string() : layout.error().message));
    AttributeSet target(*layout);
    AttributeSet source(*layout);
    source.setBase(1, 0.5);
    source.recompute();
    target.recompute();
    ModifierContext ctx;
    ctx.layout = layout->get();
    ctx.target = &target;
    ctx.source = &source;
    ctx.level = 3.0;
    std::unordered_map<std::string, hxl::Curve> curves{{"Hp.PerLevel", hxl::Curve{{1, 5}, {100, 500}}}};
    ctx.curves = &curves;

    ModifierDef d;
    d.attr = AttributeRef(101);
    d.op = ModOp::ModAdd;
    d.magnitude = MagnitudeCurve{Name("Hp.PerLevel"), MagnitudeInput::Level, 2.0};
    auto m = instantiateModifier(d, ctx);
    REQUIRE(m.ok());
    CHECK(m->kind == MagnitudeKind::Constant);
    CHECK(m->value == 600.0); // curve(3) = 300, x2
    d.magnitude = MagnitudeAttr{AttributeRef(102), MagnitudeSource::Source, CaptureMode::Snapshot, 100.0};
    m = instantiateModifier(d, ctx);
    REQUIRE(m.ok());
    CHECK(m->value == 50.0);
    d.magnitude = MagnitudeAttr{AttributeRef(102), MagnitudeSource::Target, CaptureMode::Live, 10.0};
    m = instantiateModifier(d, ctx);
    REQUIRE(m.ok());
    CHECK(m->kind == MagnitudeKind::Attribute);
    CHECK(m->source == 1);
    d.magnitude = MagnitudeHxl{refl::HxlExpr{"attr(self, Hull.Resist) * 1000"}};
    m = instantiateModifier(d, ctx);
    REQUIRE(m.ok());
    CHECK(m->kind == MagnitudeKind::Formula);
    add(target, *m);
    target.recompute();
    CHECK(target.value(0) == 1200.0);
    // Clamped by Hull.Max through the record reference.
    target.setBase(0, 5000.0);
    target.recompute();
    CHECK(target.value(0) == 2000.0);
    // Errors: unknown attribute, cross-entity domains, missing curve, live source reads.
    d.attr = AttributeRef(999);
    CHECK(instantiateModifier(d, ctx).errorCode() == ErrorCode::NotFound);
    d.attr = AttributeRef(101);
    d.target.domain = ModDomain::ShipModules;
    CHECK(instantiateModifier(d, ctx).errorCode() == ErrorCode::Unsupported);
    d.target.domain = ModDomain::Self;
    d.magnitude = MagnitudeCurve{Name("Nope"), MagnitudeInput::Level, 1.0};
    CHECK(instantiateModifier(d, ctx).errorCode() == ErrorCode::NotFound);
    d.magnitude = MagnitudeAttr{AttributeRef(102), MagnitudeSource::Source, CaptureMode::Live, 1.0};
    CHECK(instantiateModifier(d, ctx).errorCode() == ErrorCode::Unsupported);
    d.magnitude = MagnitudeConst{1.0};
    d.requirement = refl::TagQuery{"all(State)"};
    CHECK_FALSE(instantiateModifier(d, ctx).ok()); // no tag registry on the layout
}

namespace {

// Deterministic random "ships": a 40-attribute layout (stacking-penalised, AdditiveBonus, derived and
// max-clamped attributes) and constant Pre/Mod/Post arithmetic modifiers in two penalty groups, some
// exempt, with every 50th a live attribute read. No assignments, formulas, requirements or min clamps
// (the tests above check those by value).
struct Build {
    std::shared_ptr<const AttributeLayout> layout;
    std::vector<Modifier> mods;
    std::vector<f64> bases;
};

Build makeBuild(u32 seed, u32 modifierCount) {
    std::mt19937_64 rng(seed);
    std::vector<AttributeLayout::Input> inputs;
    for (int i = 0; i < 40; ++i) {
        auto in = attr("A" + std::to_string(i), 1.0 + static_cast<f64>(rng() % 1000) / 7.0);
        in.stackingPenalised = (i % 3) == 0;
        if (i % 7 == 6) in.multiplierMode = MultiplierMode::AdditiveBonus;
        if (i % 10 == 9) in.derived = "attr(self, A" + std::to_string(i - 1) + ") * 0.5 + attr(self, A" + std::to_string(i - 2) + ")";
        if (i % 11 == 10) in.maxClamp = std::string("A" + std::to_string(i - 3));
        inputs.push_back(in);
    }
    Build b;
    b.layout = layoutOf(inputs);
    // Not std::uniform_real_distribution: its output is implementation-defined (MSVC's STL, libc++
    // and libstdc++ differ), which would change the pinned hash on the primary platform. 53 random
    // bits scaled by 2^-53 are exact; the one addition rounds identically everywhere.
    auto mag = [](std::mt19937_64& g) { return 0.6 + static_cast<f64>(g() >> 11) * 0x1p-53; };
    const ModOp ops[] = {ModOp::PreMul, ModOp::PreDiv, ModOp::ModAdd, ModOp::ModSub, ModOp::PostMul,
                         ModOp::PostDiv, ModOp::PostPercent, ModOp::PostMul, ModOp::PostMul};
    for (u32 k = 0; k < modifierCount; ++k) {
        // One RNG draw per statement: argument evaluation order is unspecified (GCC and Clang differ).
        const auto slot = static_cast<AttrSlot>(rng() % 40);
        const ModOp op = ops[rng() % std::size(ops)];
        const f64 value = mag(rng);
        Modifier m = Modifier::constant(slot, op, value);
        if (m.op == ModOp::PostPercent) m.value = (m.value - 1.0) * 30.0;
        m.penaltyGroup = static_cast<u16>(rng() % 2);
        m.exempt = (rng() % 6) == 0;
        if (k % 50 == 49 && slot > 0) {
            m = Modifier::fromAttribute(slot, ModOp::ModAdd, static_cast<AttrSlot>(slot - 1), 0.01); // live read
        }
        b.mods.push_back(m);
    }
    for (int i = 0; i < 40; ++i) b.bases.push_back(static_cast<f64>(rng() % 5000) / 3.0);
    return b;
}

// Straightforward reference of 06 §1.2 without sorting or folding tricks, in long double.
long double referenceValue(const AttributeSet& s, AttrSlot a, const std::vector<Modifier>& mods) {
    const AttributeSpec& spec = s.layout().spec(a);
    // makeBuild's derived attributes are attr(a-1) * 0.5 + attr(a-2).
    long double v = spec.derived ? static_cast<long double>(s.value(static_cast<AttrSlot>(a - 1))) * 0.5L +
                                       static_cast<long double>(s.value(static_cast<AttrSlot>(a - 2)))
                                 : static_cast<long double>(s.base(a));
    long double preMul = 1, preDiv = 1, add = 0, sub = 0, sumBonus = 0;
    std::vector<std::pair<long double, const Modifier*>> f;
    for (const Modifier& m : mods) {
        if (m.attr != a) continue;
        const long double mag = m.kind == MagnitudeKind::Attribute ? m.value * static_cast<long double>(s.value(m.source)) : m.value;
        switch (m.op) {
        case ModOp::PreMul: preMul *= mag; break;
        case ModOp::PreDiv: preDiv *= mag; break;
        case ModOp::ModAdd: add += mag; break;
        case ModOp::ModSub: sub += mag; break;
        case ModOp::PostMul: f.push_back({mag, &m}); break;
        case ModOp::PostPercent: f.push_back({1 + mag / 100, &m}); break;
        case ModOp::PostDiv: f.push_back({1 / mag, &m}); break;
        default: break;
        }
    }
    v = v * preMul / preDiv + add - sub;
    if (spec.multiplierMode == MultiplierMode::AdditiveBonus) {
        for (auto& [x, m] : f) sumBonus += x - 1;
        if (!f.empty()) v *= 1 + sumBonus;
    } else {
        for (int group = 0; group < 2; ++group) {
            for (int malus = 0; malus < 2; ++malus) {
                std::vector<long double> chain;
                for (auto& [x, m] : f) {
                    const bool penalised = spec.stackingPenalised && !m->exempt;
                    if (!penalised || m->penaltyGroup != group || (x < 1) != (malus == 1) || x == 1) continue;
                    chain.push_back(x);
                }
                std::sort(chain.begin(), chain.end(), [](long double p, long double q) { return std::fabs(p - 1) > std::fabs(q - 1); });
                for (usize i = 0; i < chain.size(); ++i) {
                    const long double q = static_cast<long double>(i) / 2.67L;
                    v *= 1 + (chain[i] - 1) * std::exp(-q * q);
                }
            }
        }
        for (auto& [x, m] : f) {
            if (!(spec.stackingPenalised && !m->exempt)) v *= x;
        }
    }
    if (spec.maxClamp.kind == AttrBound::Kind::Attr && v > s.value(spec.maxClamp.attr)) v = s.value(spec.maxClamp.attr);
    return v;
}

} // namespace

TEST_CASE("attributes: 200 golden builds match a reference within 1e-9 and pin a golden hash") {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 seed = 1; seed <= 200; ++seed) {
        Build b = makeBuild(seed, 150);
        AttributeSet s(b.layout);
        for (usize i = 0; i < b.bases.size(); ++i) s.setBase(static_cast<AttrSlot>(i), b.bases[i]);
        for (const Modifier& m : b.mods) {
            auto r = s.addModifier(m);
            REQUIRE(r.ok());
        }
        s.recompute();
        for (AttrSlot a = 0; a < 40; ++a) {
            const long double ref = referenceValue(s, a, b.mods);
            const f64 got = s.value(a);
            CHECK_MESSAGE(std::fabs(static_cast<long double>(got) - ref) <= 1e-9L * std::max(1.0L, std::fabs(ref)),
                          "seed " << seed << " attr " << a);
            const u64 v = bits(got);
            for (int k = 0; k < 8; ++k) {
                h ^= (v >> (8 * k)) & 0xff;
                h *= 0x100000001b3ull;
            }
        }
    }
    MESSAGE("golden build hash " << std::format("{:#018x}", h));
    // Pinned on GCC 13 and Clang 18; must be identical on MSVC, clang-cl and MinGW (06 §12.2 GP-1).
    // Never update the constant to make one platform pass: fix the build flags instead. (Re-pinned
    // once in the WP-0.19 review, when the input generator stopped using the implementation-defined
    // std::uniform_real_distribution; the kernel's results did not change.)
    CHECK(h == 0x06ab742cc6907032ull);
}

TEST_CASE("attributes: parallel resolve equals sequential resolve") {
    std::vector<std::unique_ptr<AttributeSet>> a, b;
    for (u32 seed = 1; seed <= 300; ++seed) {
        Build bl = makeBuild(seed % 7 + 1, 60);
        for (auto* v : {&a, &b}) {
            v->push_back(std::make_unique<AttributeSet>(bl.layout));
            for (const Modifier& m : bl.mods) REQUIRE(v->back()->addModifier(m).ok());
        }
    }
    std::vector<AttributeSet*> pa, pb;
    for (auto& s : a) pa.push_back(s.get());
    for (auto& s : b) pb.push_back(s.get());
    jobs::JobSystem js(jobs::JobSystemDesc{});
    resolveAttributes(pa, {}, &js);
    resolveAttributes(pb, {}, nullptr);
    for (usize i = 0; i < pa.size(); ++i) {
        for (AttrSlot k = 0; k < 40; ++k) CHECK(bits(pa[i]->value(k)) == bits(pb[i]->value(k)));
    }
}

TEST_CASE("attributes: formulas bound with another tag registry are rejected (review regression)") {
    // The layout hash used to ignore the tag registry, so a formula bound to a layout with the same
    // attributes but a different registry was accepted and its TagIndex values were used against the
    // wrong registry (out of bounds when that registry is smaller).
    TagRegistry::Builder big, small;
    for (int i = 0; i < 50; ++i) REQUIRE(big.add("T.N" + std::to_string(i)).ok());
    REQUIRE(small.add("T.N49").ok());
    auto bigReg = *big.build();
    auto smallReg = *small.build();
    auto layoutBig = layoutOf({attr("Speed", 1.0)}, bigReg);
    auto layoutSmall = layoutOf({attr("Speed", 1.0)}, smallReg);
    CHECK(layoutBig->hash() != layoutSmall->hash());
    auto f = layoutBig->compileFormula("select(tag(self, T.N49), 2, 1)");
    REQUIRE(f.ok());
    REQUIRE((*f)->tagIndices()[0] >= smallReg->size());
    AttributeSet s(layoutSmall);
    CHECK(s.addModifier(Modifier::fromFormula(0, ModOp::PostMul, *f)).errorCode() == ErrorCode::InvalidArgument);
    // Same registry content: layouts agree and the formula is accepted.
    TagRegistry::Builder big2;
    for (int i = 0; i < 50; ++i) REQUIRE(big2.add("T.N" + std::to_string(i)).ok());
    auto layoutBig2 = layoutOf({attr("Speed", 1.0)}, *big2.build());
    CHECK(layoutBig2->hash() == layoutBig->hash());
}

namespace {
int g_assertFailures = 0;
AssertAction countAndContinue(const AssertInfo&) {
    ++g_assertFailures;
    return AssertAction::Continue;
}
} // namespace

TEST_CASE("attributes: setBase ignores slots outside the layout (review regression)") {
    auto layout = layoutOf({attr("A", 1.0)});
    AttributeSet s(layout);
    s.recompute();
    // A bad slot is a caller bug (asserted in development builds); release builds must not write out
    // of bounds either, so the assert handler continues here to exercise that path.
    g_assertFailures = 0;
    const AssertHandler previous = setAssertHandler(&countAndContinue);
    CHECK_FALSE(s.setBase(layout->find("Typo"), 5.0)); // kInvalidAttr used to be written out of bounds
    CHECK_FALSE(s.setBase(1, 5.0));
    setAssertHandler(previous);
#if HELIOS_ENABLE_ASSERTS
    CHECK(g_assertFailures == 2);
#endif
    CHECK_FALSE(s.dirty());
    CHECK(s.setBase(0, 2.0));
    CHECK(s.dirty());
    s.recompute();
    CHECK(s.value(0) == 2.0);
}

TEST_CASE("attributes: formula rules shared with Go content validation (review regression)") {
    // services/pkg/hxl CompileDerived/CompileMagnitude must reject exactly these (review_test.go
    // TestRecordFormulasFollowCppRules); Go used to accept several of them.
    TagRegistry::Builder tb;
    REQUIRE(tb.add("T").ok());
    auto layout = layoutOf({attr("X", 1.0), attr("Skill.X", 2.0), attr("Hull.Hp", 3.0)}, *tb.build());
    for (const char* src : {"self.speed * 2", "curve(Falloff, 0.5)", "formula F(a, b) = attr(a, X) + attr(b, X)",
                            "formula F() = 1", "tag(self, T)"}) {
        CAPTURE(src);
        CHECK_FALSE(layout->compileFormula(src, /*allowContext=*/true).ok());
        CHECK_FALSE(layout->compileFormula(src, /*allowContext=*/false).ok());
    }
    CHECK(layout->compileFormula("stacks() * 5 + level()", true).ok());
    CHECK(layout->compileFormula("formula Bonus(ship) = attr(ship, Skill.X) * 0.05", true).ok());
    CHECK_FALSE(layout->compileFormula("stacks()", false).ok());
    CHECK(layout->compileFormula("formula Ehp(ship) = attr(ship, Hull.Hp) * select(tag(ship, T), 2, 1)", false).ok());
}

TEST_CASE("attributes: two attributes with one record id are rejected (review regression)") {
    auto a = attr("A"), b = attr("B");
    a.rid = 5;
    b.rid = 5;
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{a, b}).errorCode() == ErrorCode::AlreadyExists);
    b.rid = 6;
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{a, b}).ok());
    a.rid = b.rid = 0; // no record ids (layouts built in code)
    CHECK(AttributeLayout::build(std::vector<AttributeLayout::Input>{a, b}).ok());
}

TEST_CASE("attributes: Target/Snapshot magnitudes need a target of the context's layout (review regression)") {
    // The magnitude's slot is resolved in ctx.layout and was read from ctx.target unchecked: a target
    // of a smaller layout was read out of bounds, one of another layout gave another attribute.
    std::vector<AttributeDef> defs(2);
    defs[0].id = Name("Hull.Hp");
    defs[1].id = Name("Hull.Resist");
    defs[1].default_ = 0.25;
    std::vector<AttributeLayout::Record> recs = {{101, &defs[0]}, {102, &defs[1]}};
    auto layout = AttributeLayout::fromRecords(recs);
    REQUIRE(layout.ok());
    auto other = layoutOf({attr("Other", 7.0)});
    AttributeSet small(other);
    small.recompute();
    ModifierContext ctx;
    ctx.layout = layout->get();
    ctx.target = &small;
    ModifierDef d;
    d.attr = AttributeRef(101);
    d.op = ModOp::ModAdd;
    d.magnitude = MagnitudeAttr{AttributeRef(102), MagnitudeSource::Target, CaptureMode::Snapshot, 2.0};
    CHECK(instantiateModifier(d, ctx).errorCode() == ErrorCode::InvalidArgument);
    // A set of the same layout, or of an identical one (same hash), is fine.
    AttributeSet target(*layout);
    target.recompute();
    ctx.target = &target;
    auto m = instantiateModifier(d, ctx);
    REQUIRE(m.ok());
    CHECK(m->value == 0.5);
    auto twin = AttributeLayout::fromRecords(recs);
    REQUIRE(twin.ok());
    AttributeSet twinTarget(*twin);
    twinTarget.recompute();
    ctx.target = &twinTarget;
    CHECK(instantiateModifier(d, ctx).ok());
}
