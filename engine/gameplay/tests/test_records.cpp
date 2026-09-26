// The kernel records compiled by helios-schemac (schemas/gameplay/*.hschema): reflection metadata,
// JSONC and tagged-binary round trips, and loading into the runtime registries.
#include <doctest/doctest.h>

#include "gameplay/attributes.gen.h"
#include "gameplay/economy.gen.h"
#include "gameplay/effects.gen.h"
#include "gameplay/items.gen.h"
#include "gameplay/tags.gen.h"
#include "helios/gameplay/attributes.h"
#include "helios/gameplay/tags.h"
#include "helios/reflect/reflect.h"

using namespace helios;
using namespace helios::gameplay;

TEST_CASE("records: generated kernel types register and round-trip") {
    REQUIRE(registerTagsTypes().ok());
    REQUIRE(registerAttributesTypes().ok());
    REQUIRE(registerEffectsTypes().ok());
    REQUIRE(registerItemsTypes().ok());
    REQUIRE(registerEconomyTypes().ok());
    const auto& reg = refl::TypeRegistry::global();
    for (const char* name : {"helios.gameplay.TagDef", "helios.gameplay.AttributeDef", "helios.gameplay.ModifierDef",
                             "helios.gameplay.EffectDef", "helios.gameplay.ItemDef", "helios.gameplay.ItemInstance",
                             "helios.gameplay.ReasonCodeDef"}) {
        CHECK_MESSAGE(reg.find(name) != nullptr, name);
    }

    AttributeDef a;
    a.id = Name("Shield.Current");
    a.default_ = 50.0;
    a.maxClamp = AttrOrConst{AttrOrConstAttr{AttributeRef(42)}};
    a.derived = refl::HxlExpr{"attr(self, Shield.Max) * 0.5"};
    a.stackingPenalised = true;
    a.replicate = Audience::All;
    const std::string json = refl::toJson(a);
    auto back = refl::fromJson<AttributeDef>(json);
    REQUIRE(back.ok());
    CHECK(*back == a);
    auto bin = refl::decodeTagged<AttributeDef>(refl::encodeTagged(a));
    REQUIRE(bin.ok());
    CHECK(*bin == a);

    EffectDef e;
    e.duration = EffectDef::DurationPeriodic{MagnitudeConst{1.0}, MagnitudeConst{10.0}, true};
    ModifierDef m;
    m.attr = AttributeRef(42);
    m.op = ModOp::PostPercent;
    m.magnitude = MagnitudeHxl{refl::HxlExpr{"stacks() * 5"}};
    m.requirement = refl::TagQuery{"none(State.Suppressed)"};
    e.modifiers.push_back(m);
    e.grantedTags.add("State.Buff.Overclock");
    e.stacking.policy = StackingPolicy::BySource;
    e.stacking.limit = 3;
    auto e2 = refl::fromJson<EffectDef>(refl::toJson(e));
    REQUIRE(e2.ok());
    CHECK(*e2 == e);

    ItemDef item;
    item.name = refl::LocString{"item.rail.name"};
    item.stackMax = 1;
    item.baseAttrs[AttributeRef(42)] = 12.5;
    item.stateEffects[ItemState::Active] = {EffectRef(7)};
    item.arrangements = {{Name("Turret.Small")}};
    auto i2 = refl::decodeTagged<ItemDef>(refl::encodeTagged(item));
    REQUIRE(i2.ok());
    CHECK(*i2 == item);

    ReasonCodeDef rc;
    rc.code = Name("Faucet.Bounty.NPC");
    rc.kind = ReasonKind::Faucet;
    rc.faucet = FaucetCap{1000};
    auto rc2 = refl::fromJson<ReasonCodeDef>(refl::toJson(rc));
    REQUIRE(rc2.ok());
    CHECK(*rc2 == rc);
}

TEST_CASE("records: TagDef and AttributeDef records feed the runtime") {
    const char* tagJson[] = {R"({"tag": "Ship.Class.Frigate", "replicate": "All"})", R"({"tag": "State.Debuff.Stun"})"};
    std::vector<TagDef> tags;
    for (const char* j : tagJson) {
        auto t = refl::fromJson<TagDef>(j);
        REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
        tags.push_back(*t);
    }
    auto reg = TagRegistry::fromRecords(tags);
    REQUIRE(reg.ok());
    CHECK((*reg)->info((*reg)->find("Ship.Class.Frigate")).replicate == Audience::All);

    auto hp = refl::fromJson<AttributeDef>(R"({"id": "Hull.Hp", "default": 1000, "persistBase": true})");
    auto ehp = refl::fromJson<AttributeDef>(
        R"json({"id": "Hull.Effective", "derived": "attr(self, Hull.Hp) * select(tag(self, Ship.Class.Frigate), 2, 1)"})json");
    REQUIRE(hp.ok());
    REQUIRE(ehp.ok());
    std::vector<AttributeLayout::Record> recs = {{1, &*hp}, {2, &*ehp}};
    auto layout = AttributeLayout::fromRecords(recs, *reg);
    REQUIRE_MESSAGE(layout.ok(), (layout.ok() ? std::string() : layout.error().message));
    CHECK((*layout)->spec(0).persistBase);
    AttributeSet s(*layout);
    TagContainer held(**reg);
    held.add((*reg)->find("Ship.Class.Frigate"));
    s.recompute(&held);
    CHECK(s.value(1) == 2000.0);
}
