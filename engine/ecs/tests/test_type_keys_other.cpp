// The second translation unit of test_type_keys.cpp: types in an unnamed namespace that print the same names
// as that file's Cooldown and KeyClash. Every compiler gives them per-image keys, so neither type reaches the
// other's component (helios/ecs/type_key.h).

#include "type_key_clash.h"

#include "helios/ecs/world.h"

namespace {

// Larger than test_type_keys.cpp's Cooldown {u32}: reaching that component through this type would read
// past its end.
struct Cooldown {
    helios::u64 started = 0;
    helios::u64 length = 0;
};

struct KeyClash {
    helios::u32 other = 0; // same size as test_type_keys.cpp's KeyClash: only the key may tell them apart
};

} // namespace

namespace ecs_test {

helios::ecs::ComponentId otherCooldownId(const helios::ecs::World& world) { return world.id<Cooldown>(); }

const void* otherCooldownGet(const helios::ecs::World& world, helios::ecs::Entity e) {
    return world.get<Cooldown>(e);
}

bool otherCooldownHas(const helios::ecs::World& world, helios::ecs::Entity e) { return world.has<Cooldown>(e); }

helios::ecs::ComponentId otherCooldownRegisterAndSet(helios::ecs::World& world, helios::ecs::Entity e,
                                                     helios::u64 started, helios::u64 length) {
    const helios::ecs::ComponentId id =
        world.registerComponent<Cooldown>(helios::ecs::ComponentFlags::None, "ecs_test.OtherCooldown");
    if (id != 0) world.set(e, Cooldown{started, length});
    return id;
}

helios::u64 otherCooldownStarted(const helios::ecs::World& world, helios::ecs::Entity e) {
    const Cooldown* c = world.get<Cooldown>(e);
    return c ? c->started : ~helios::u64(0);
}

helios::Result<void> bindOtherKeyClash(helios::ecs::World& world, helios::ecs::ComponentId id) {
    return world.bindType<KeyClash>(id);
}

helios::ecs::ComponentId otherKeyClashId(const helios::ecs::World& world) { return world.id<KeyClash>(); }

helios::ecs::TypeKey otherKeyClashKey() noexcept { return helios::ecs::typeKey<KeyClash>(); }

helios::ecs::TypeKey otherGlobalKeyedKey() noexcept { return helios::ecs::typeKey<GlobalKeyed>(); }

} // namespace ecs_test
