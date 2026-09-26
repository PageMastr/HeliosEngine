// ItemDef / ItemInstance validation (06 §2).
#include <doctest/doctest.h>

#include "helios/gameplay/items.h"

using namespace helios;
using namespace helios::gameplay;

TEST_CASE("items: ItemDef validation") {
    TagRegistry::Builder tb;
    REQUIRE(tb.add("Item.Weapon.Laser").ok());
    REQUIRE(tb.add("Item.Plug.Barrel").ok());
    auto tags = *tb.build();
    ItemDef def;
    def.stackMax = 1;
    def.volume = 2.5f;
    def.mass = 10.0f;
    def.tags.add("Item.Weapon.Laser");
    def.arrangements = {{Name("Hand.Right")}, {Name("Hand.Left"), Name("Hand.Right")}};
    def.sockets.push_back(SocketEntry{Name("Barrel"), std::nullopt, Name("Plugs.Barrel"), Name(), 1});
    def.equipReq.tags = refl::TagQuery{"all(Item.Weapon)"};
    CHECK(validateItemDef(def, tags.get()).ok());
    CHECK(validateItemDef(def).ok()); // without a registry only syntax is checked

    ItemDef bad = def;
    bad.stackMax = 0;
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.mass = -1.0f;
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.tags.add("Item.Unknown");
    CHECK(validateItemDef(bad, tags.get()).errorCode() == ErrorCode::NotFound);
    bad = def;
    bad.arrangements.push_back({});
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.arrangements = {{Name("A"), Name("A")}};
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.stackMax = 100; // sockets are per-instance state
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.equipReq.tags = refl::TagQuery{"all(Item.Nope)"};
    CHECK_FALSE(validateItemDef(bad, tags.get()).ok());
    bad = def;
    bad.decay = DecaySpec{0.0f, 0.1f, 0.0f};
    CHECK_FALSE(validateItemDef(bad).ok());
    bad = def;
    bad.port = ItemPortSpec{Name("Hardpoint"), 3, 1, std::nullopt};
    CHECK_FALSE(validateItemDef(bad).ok());

    ItemDef ammo;
    ammo.stackMax = 9999;
    ammo.volume = 0.01f;
    CHECK(validateItemDef(ammo).ok());
}

TEST_CASE("items: ItemInstance validation") {
    ItemDef ammo;
    ammo.stackMax = 500;
    ItemInstance stack;
    stack.quantity = 500;
    CHECK(validateItemInstance(stack, ammo).ok());
    stack.quantity = 501;
    CHECK(validateItemInstance(stack, ammo).errorCode() == ErrorCode::OutOfRange);
    stack.quantity = 0;
    CHECK_FALSE(validateItemInstance(stack, ammo).ok());
    ItemDef gun;
    gun.decay = DecaySpec{100.0f, 0.5f, 0.0f};
    ItemInstance g;
    g.payload.durability = 80.0f;
    CHECK(validateItemInstance(g, gun).ok());
    g.payload.durability = 120.0f;
    CHECK_FALSE(validateItemInstance(g, gun).ok());
    g.payload.durability = 1.0f;
    g.payload.rolled[AttributeRef(5)] = std::numeric_limits<f64>::infinity();
    CHECK_FALSE(validateItemInstance(g, gun).ok());
}
