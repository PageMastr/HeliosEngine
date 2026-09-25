#pragma once
// Components shared by the helios::ecs test suites.

#include <string>
#include <tuple>

#include "helios/ecs/world.h"
#include "helios/math/vec.h"

namespace ecs_test {

using helios::f32;
using helios::f64;
using helios::u32;
using helios::u64;

struct Position {
    helios::DVec3 value{};
    helios::ecs::FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Position::value);
};

struct Velocity {
    helios::Vec3 value{};
};

struct Health {
    f32 hp = 100.0f;
    f32 maxHp = 100.0f;
    u32 armor = 0;
    helios::ecs::FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Health::hp, &Health::maxHp, &Health::armor);
};

struct Shield {
    f32 value = 50.0f;
    helios::ecs::FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Shield::value);
};

/// Shared through prefabs (ComponentFlags::Shared).
struct HullSpec {
    f32 mass = 1000.0f;
    f32 maxHp = 500.0f;
};

struct Counter {
    u64 value = 0;
};

struct Tagged {};
struct Frozen {};

/// Registers the common set with the usual flags.
inline void registerCommon(helios::ecs::World& w) {
    using helios::ecs::ComponentFlags;
    w.registerComponent<Position>();
    w.registerComponent<Velocity>();
    w.registerComponent<Health>();
    w.registerComponent<Shield>();
    w.registerComponent<HullSpec>(ComponentFlags::Shared);
    w.registerComponent<Counter>();
    w.registerComponent<Tagged>();
    w.registerComponent<Frozen>();
}

} // namespace ecs_test
