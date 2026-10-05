// The second translation unit of test_type_keys.cpp: a type in an unnamed namespace with the same
// qualified name as that file's KeyClash, so the two share a type key on compilers that print unnamed
// namespaces alike (GCC, Clang).

#include "helios/ecs/world.h"

namespace {

struct KeyClash {
    helios::u32 other = 0; // same size as test_type_keys.cpp's KeyClash: only the key may differ
};

} // namespace

namespace ecs_test {

helios::Result<void> bindOtherKeyClash(helios::ecs::World& world, helios::ecs::ComponentId id) {
    return world.bindType<KeyClash>(id);
}

helios::ecs::TypeKey otherKeyClashKey() noexcept { return helios::ecs::kTypeKey<KeyClash>; }

} // namespace ecs_test
