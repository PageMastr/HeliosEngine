#include "test_types.h"

using namespace helios;
using namespace helios::refl;
using namespace rtest;

const TypeInfo& TypeOf<Tint>::get() noexcept {
    static const TypeInfo* info = EnumBuilder<Tint>("rtest.Tint")
                                      .value("Red", Tint::Red)
                                      .value("Green", Tint::Green)
                                      .value("Blue", Tint::Blue)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<Caps>::get() noexcept {
    static const TypeInfo* info = EnumBuilder<Caps>("rtest.Caps", true)
                                      .value("None", Caps::None)
                                      .value("Fly", Caps::Fly)
                                      .value("Swim", Caps::Swim)
                                      .value("Dig", Caps::Dig)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<Delta>::get() noexcept {
    static const TypeInfo* info = EnumBuilder<Delta>("rtest.Delta")
                                      .value("Level", Delta::Level)
                                      .value("Down", Delta::Down)
                                      .value("Up", Delta::Up)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<Inner>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Inner>("rtest.Inner")
                                      .doc("Nested struct")
                                      .field("a", &Inner::a)
                                      .field("s", &Inner::s, {.id = 5, .was = {"text"}})
                                      .field("f", &Inner::f)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<Mount>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Mount>("rtest.Mount")
                                      .field("bone", &Mount::bone)
                                      .field("force", &Mount::force, {.attrs = {{"unit", {{"", "N"}}}}})
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<AltEmpty>::get() noexcept {
    static const TypeInfo* info = StructBuilder<AltEmpty>("rtest.Choice.AltEmpty", DeclKind::Alternative).build();
    return *info;
}

const TypeInfo& TypeOf<AltValue>::get() noexcept {
    static const TypeInfo* info = StructBuilder<AltValue>("rtest.Choice.AltValue", DeclKind::Alternative)
                                      .field("x", &AltValue::x)
                                      .field("label", &AltValue::label)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<Choice>::get() noexcept {
    static const TypeInfo* info =
        VariantBuilder<Choice>("rtest.Choice").alternative<AltEmpty>("AltEmpty").alternative<AltValue>("AltValue", 7).build();
    return *info;
}

const TypeInfo& TypeOf<Everything>::get() noexcept {
    static const TypeInfo* info = StructBuilder<Everything>("rtest.Everything")
                                      .attr("table", {{"", "everything"}})
                                      .field("b", &Everything::b)
                                      .field("i8v", &Everything::i8v)
                                      .field("i16v", &Everything::i16v)
                                      .field("i32v", &Everything::i32v)
                                      .field("i64v", &Everything::i64v)
                                      .field("u8v", &Everything::u8v)
                                      .field("u16v", &Everything::u16v)
                                      .field("u32v", &Everything::u32v)
                                      .field("u64v", &Everything::u64v)
                                      .field("f32v", &Everything::f32v)
                                      .field("f64v", &Everything::f64v)
                                      .field("withDefault", &Everything::withDefault)
                                      .field("str", &Everything::str)
                                      .field("name", &Everything::name)
                                      .field("guid", &Everything::guid)
                                      .field("v3", &Everything::v3)
                                      .field("dv3", &Everything::dv3)
                                      .field("q", &Everything::q)
                                      .field("col", &Everything::col)
                                      .field("wp", &Everything::wp)
                                      .field("ent", &Everything::ent)
                                      .field("nh", &Everything::nh)
                                      .field("dur", &Everything::dur)
                                      .field("ref", &Everything::ref)
                                      .field("asset", &Everything::asset)
                                      .field("loc", &Everything::loc)
                                      .field("tags", &Everything::tags)
                                      .field("tq", &Everything::tq)
                                      .field("hx", &Everything::hx)
                                      .field("tint", &Everything::tint)
                                      .field("caps", &Everything::caps)
                                      .field("delta", &Everything::delta)
                                      .field("inner", &Everything::inner)
                                      .field("optInner", &Everything::optInner)
                                      .field("optInt", &Everything::optInt)
                                      .field("ints", &Everything::ints)
                                      .field("doubles", &Everything::doubles)
                                      .field("strs", &Everything::strs)
                                      .field("inners", &Everything::inners)
                                      .field("nested", &Everything::nested)
                                      .field("optInts", &Everything::optInts)
                                      .field("points", &Everything::points)
                                      .field("arr", &Everything::arr)
                                      .field("arrInner", &Everything::arrInner)
                                      .field("nameSet", &Everything::nameSet)
                                      .field("intSet", &Everything::intSet)
                                      .field("attrs", &Everything::attrs)
                                      .field("byId", &Everything::byId)
                                      .field("byTint", &Everything::byTint)
                                      .field("mounts", &Everything::mounts)
                                      .field("namedMounts", &Everything::namedMounts, {.keyedBy = "bone"})
                                      .field("choice", &Everything::choice)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<HealthComp>::get() noexcept {
    static const TypeInfo* info =
        StructBuilder<HealthComp>("rtest.Health").field("current", &HealthComp::current).field("max", &HealthComp::max).build();
    return *info;
}

const TypeInfo& TypeOf<TransformComp>::get() noexcept {
    static const TypeInfo* info = StructBuilder<TransformComp>("rtest.Transform")
                                      .field("position", &TransformComp::position)
                                      .field("rotation", &TransformComp::rotation)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<ComponentSet>::get() noexcept {
    static const TypeInfo* info = StructBuilder<ComponentSet>("rtest.ComponentSet")
                                      .field("Health", &ComponentSet::Health)
                                      .field("Transform", &ComponentSet::Transform)
                                      .build();
    return *info;
}

const TypeInfo& TypeOf<EntityDoc>::get() noexcept {
    static const TypeInfo* info = StructBuilder<EntityDoc>("rtest.EntityDoc")
                                      .field("name", &EntityDoc::name)
                                      .field("components", &EntityDoc::components)
                                      .build();
    return *info;
}

namespace rtest {

Everything makeFull() {
    Everything e;
    e.b = true;
    e.i8v = -8;
    e.i16v = -1600;
    e.i32v = -320000;
    e.i64v = -6400000000000ll;
    e.u8v = 200;
    e.u16v = 60000;
    e.u32v = 4000000000u;
    e.u64v = 18000000000000000000ull;
    e.f32v = 0.1f;
    e.f64v = -1.0 / 3.0;
    e.withDefault = 0.0f; // non-default zero
    e.str = "hello \"quoted\"\n\tworld \xE2\x9C\x93";
    e.name = Name("hull_a");
    e.guid = Guid(0x0123456789abcdefull, 0xfedcba9876543210ull);
    e.v3 = Vec3(1.0f, -2.5f, 3.25f);
    e.dv3 = DVec3(1e13, -0.5, 123456.789);
    e.q = Quat(0.0f, 0.70710677f, 0.0f, 0.70710677f);
    e.col = Color(0.25f, 0.5f, 0.75f, 1.0f);
    e.wp.local = DVec3(9e12, 1.0, -2.0);
    e.ent = EntityId(0x8000000000000123ull);
    e.nh = NetHandle::make(77, 3);
    e.dur = Duration::fromMillis(1500);
    e.ref = RecordRef<Inner>(0x123456789abcdefull);
    e.asset.guid = Guid(1, 2);
    e.loc.key = "ship.kestrel.name";
    e.tags.add("Item.Weapon");
    e.tags.add("Item.Armor");
    e.tq.text = "Item.Weapon & !Item.Rare";
    e.hx.text = "attr(ship, Mass) * 9.81";
    e.tint = Tint::Blue;
    e.caps = static_cast<Caps>(static_cast<u32>(Caps::Fly) | static_cast<u32>(Caps::Dig));
    e.delta = Delta::Down;
    e.inner = Inner{7, "inner", 2.0f};
    e.optInner = Inner{0, "", 1.5f}; // engaged with default contents
    e.optInt = 0;                    // engaged zero
    e.ints = {1, -2, 300, 0};
    e.doubles = {0.5, -0.0};
    e.strs = {"a", "", "c"};
    e.inners = {Inner{1, "x", 1.5f}, Inner{}};
    e.nested = {{1, 2}, {}, {3}};
    e.optInts = {5, std::nullopt, 0};
    e.points = {Vec3(1, 2, 3), Vec3(0, 0, -1)};
    e.arr = {1.0f, 0.0f, -1.0f};
    e.arrInner = {Inner{}, Inner{3, "z", 1.5f}};
    e.nameSet = {Name("zeta"), Name("alpha"), Name("mid")};
    e.intSet = {3, -1, 7};
    e.attrs = {{Name("Ship.Speed"), 12.5f}, {Name("Ship.Armor"), 0.0f}};
    e.byId = {{10, Inner{1, "ten", 1.5f}}, {2, Inner{}}};
    e.byTint = {{Tint::Green, {1, 2}}, {Tint::Red, {}}};
    e.mounts.add(Guid(0x1111, 0x2222), Mount{Name("bone_a"), 5.0f});
    e.mounts.add(Guid(0x3333, 0x4444), Mount{Name("bone_b"), 100.0f});
    e.namedMounts = {Mount{Name("left"), 1.0f}, Mount{Name("right"), 2.0f}};
    e.choice = AltValue{9, "nine"};
    return e;
}

} // namespace rtest
