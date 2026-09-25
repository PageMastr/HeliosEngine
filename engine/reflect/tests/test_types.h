#pragma once
// Builder-reflected test types covering every Kind (engine/reflect tests do not depend on
// helios-schemac; generated types are exercised by tools/schemac tests).

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "helios/reflect/reflect.h"

namespace rtest {
using namespace helios;
using namespace helios::refl;

enum class Tint : u8 { Red = 1, Green = 2, Blue = 3 };
enum class Caps : u32 { None = 0, Fly = 1, Swim = 2, Dig = 4 };
enum class Delta : i16 { Down = -1, Level = 0, Up = 1 };

struct Inner {
    i32 a = 0;
    std::string s;
    f32 f = 1.5f;
    friend bool operator==(const Inner&, const Inner&) = default;
};

struct Mount {
    Name bone;
    f32 force = 100.0f;
    friend bool operator==(const Mount&, const Mount&) = default;
};

struct AltEmpty {
    friend bool operator==(const AltEmpty&, const AltEmpty&) = default;
};
struct AltValue {
    i32 x = 0;
    std::string label;
    friend bool operator==(const AltValue&, const AltValue&) = default;
};
using Choice = std::variant<AltEmpty, AltValue>;

/// One field of every supported kind.
struct Everything {
    bool b = false;
    i8 i8v = 0;
    i16 i16v = 0;
    i32 i32v = 0;
    i64 i64v = 0;
    u8 u8v = 0;
    u16 u16v = 0;
    u32 u32v = 0;
    u64 u64v = 0;
    f32 f32v = 0;
    f64 f64v = 0;
    f32 withDefault = 42.5f;
    std::string str;
    Name name;
    Guid guid;
    Vec3 v3;
    DVec3 dv3;
    Quat q;
    Color col;
    WorldPos wp;
    EntityId ent;
    NetHandle nh;
    Duration dur;
    RecordRef<Inner> ref;
    AssetRef asset;
    LocString loc;
    TagSet tags;
    TagQuery tq;
    HxlExpr hx;
    Tint tint = Tint::Red;
    Caps caps = Caps::None;
    Delta delta = Delta::Level;
    Inner inner;
    std::optional<Inner> optInner;
    std::optional<i32> optInt;
    std::vector<i32> ints;
    std::vector<f64> doubles;
    std::vector<std::string> strs;
    std::vector<Inner> inners;
    std::vector<std::vector<u8>> nested;
    std::vector<std::optional<i32>> optInts;
    std::vector<Vec3> points;
    std::array<f32, 3> arr{};
    std::array<Inner, 2> arrInner{};
    std::set<Name> nameSet;
    std::set<i32> intSet;
    std::map<Name, f32> attrs;
    std::map<u32, Inner> byId;
    std::map<Tint, std::vector<i32>> byTint;
    KeyedList<Mount> mounts;
    std::vector<Mount> namedMounts; // @keyed(bone)
    Choice choice;
};

/// Editor-style entity document: `components.Health.max`.
struct HealthComp {
    f32 current = 100.0f;
    f32 max = 100.0f;
};
struct TransformComp {
    WorldPos position;
    Quat rotation;
};
struct ComponentSet {
    std::optional<HealthComp> Health;
    TransformComp Transform;
};
struct EntityDoc {
    std::string name;
    ComponentSet components;
};

/// Deterministic, fully populated sample (every field non-default).
Everything makeFull();

} // namespace rtest

HELIOS_REFLECT_ENUM(rtest::Tint);
HELIOS_REFLECT_ENUM(rtest::Caps);
HELIOS_REFLECT_ENUM(rtest::Delta);
HELIOS_REFLECT_TYPE(rtest::Inner);
HELIOS_REFLECT_TYPE(rtest::Mount);
HELIOS_REFLECT_TYPE(rtest::AltEmpty);
HELIOS_REFLECT_TYPE(rtest::AltValue);
HELIOS_REFLECT_VARIANT(rtest::Choice);
HELIOS_REFLECT_TYPE(rtest::Everything);
HELIOS_REFLECT_TYPE(rtest::HealthComp);
HELIOS_REFLECT_TYPE(rtest::TransformComp);
HELIOS_REFLECT_TYPE(rtest::ComponentSet);
HELIOS_REFLECT_TYPE(rtest::EntityDoc);
