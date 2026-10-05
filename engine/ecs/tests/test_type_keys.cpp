// Type keys (helios/ecs/type_key.h; 02 §1.4, WP-0.6c): the World's C++ type -> component table is keyed by
// typeKey<T>(). A type in a named namespace has a name key that every image derives alike (its canonical
// name, size and alignment), and every supported compiler too unless its name has template arguments or an
// inline namespace; every other type has a per-image key, so two types that print the same name never reach
// each other's component (except the Clang residuals type_key.h lists).

#include <doctest/doctest.h>

#include <atomic>
#include <string_view>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/ecs/type_key.h"
#include "helios/ecs/world.h"
#include "test_types.h"
#include "type_key_clash.h"

// helios-lint: outside-anon-namespace begin (name keys: only types in a named namespace have one)
namespace ecs_test {

struct KeyOuter {
    struct Inner {
        helios::u32 value = 0;
    };
};
enum class KeyKind : helios::u8 { A, B };
union KeyUnion {
    helios::u32 bits;
    float value;
};
template <class A, class B>
struct KeyPair {
    A a{};
    B b{};
};
template <helios::u32 N>
struct KeyNumbered {
    helios::u32 value = N;
};
template <class T>
struct KeyHolder {
    struct Inner {
        T value{};
    };
};
inline namespace v1 {
struct KeyVersioned {
    helios::u32 value = 0;
};
} // namespace v1

} // namespace ecs_test
// helios-lint: outside-anon-namespace end

namespace {

using namespace helios;
using namespace helios::ecs;
namespace tk = helios::ecs::detail;
using tk::canonicalNameEquals;
using tk::isPerImageTypeName;
using tk::typeNameFromSignature;

// ---------------------------------------------------------------------------------------------
// Each compiler's spelling, as literal strings, so every compiler checks every format.
// ---------------------------------------------------------------------------------------------

static_assert(typeNameFromSignature("constexpr const char* helios::ecs::detail::signatureNaming() "
                                    "[with T = helios::ecs::NetIdentity]") == "helios::ecs::NetIdentity"); // GCC
static_assert(typeNameFromSignature("constexpr const char* f() [with T = ns::A; X = int]") == "ns::A");
static_assert(typeNameFromSignature("const char *helios::ecs::detail::signatureNaming() "
                                    "[T = helios::ecs::NetIdentity]") == "helios::ecs::NetIdentity"); // Clang
static_assert(typeNameFromSignature("const char *__cdecl helios::ecs::detail::signatureNaming<struct "
                                    "helios::ecs::NetIdentity>(void) noexcept") ==
              "struct helios::ecs::NetIdentity"); // MSVC

static_assert(canonicalNameEquals("struct helios::ecs::NetIdentity", "helios::ecs::NetIdentity"));
static_assert(canonicalNameEquals("enum ns::Kind", "ns::Kind"));
static_assert(canonicalNameEquals("union ns::U", "ns::U"));
static_assert(canonicalNameEquals("struct ns::Pair<struct ns::A,enum ns::Kind>", "ns::Pair<ns::A,ns::Kind>"));
static_assert(canonicalNameEquals("ns::Pair<ns::A, ns::Kind>", "ns::Pair<ns::A,ns::Kind>"));
static_assert(canonicalNameEquals("class std::vector<int,class std::allocator<int> >",
                                  "std::vector<int,std::allocator<int>>"));
static_assert(canonicalNameEquals("ns::Pair<const char *, unsigned int>", "ns::Pair<const char*,unsigned int>"));
static_assert(canonicalNameEquals("const struct ns::A", "const ns::A"));
// A word that only starts like a class-key stays.
static_assert(canonicalNameEquals("ns::classic::structure", "ns::classic::structure"));
static_assert(!canonicalNameEquals("ns::A", "ns::AB"));
static_assert(tk::canonicalNameHash("struct helios::ecs::NetIdentity") == fnv1a64("helios::ecs::NetIdentity"));

// Keyed per image, as each compiler prints the type.
static_assert(isPerImageTypeName("(anonymous namespace)::Cooldown"));                    // Clang
static_assert(isPerImageTypeName("{anonymous}::Cooldown"));                              // GCC
static_assert(isPerImageTypeName("struct `anonymous namespace'::Cooldown"));             // MSVC
static_assert(isPerImageTypeName("game::f()::Local"));                                   // GCC local class
static_assert(isPerImageTypeName("struct `void __cdecl game::f(void)'::`2'::Local"));    // MSVC local class
static_assert(isPerImageTypeName("Local"));                // Clang local class, and the global namespace
static_assert(isPerImageTypeName("struct Global"));
static_assert(isPerImageTypeName("Box<helios::ecs::NetIdentity>")); // a global-namespace template
static_assert(isPerImageTypeName("ns::Box<(anonymous namespace)::Cooldown>"));
static_assert(isPerImageTypeName("(lambda at a.cpp:1:2)"));
static_assert(isPerImageTypeName("<lambda()>"));
static_assert(isPerImageTypeName("class <lambda_1>"));
// A template argument that names a class without a scope: Clang and clang-cl print every function's local
// class Local, and a global-namespace Local, as "ns::Box<Local>".
static_assert(isPerImageTypeName("ns::Box<Local>"));
static_assert(isPerImageTypeName("ns::Pair<ns::A, Local>"));
static_assert(isPerImageTypeName("ns::Box<const Local *>"));
static_assert(isPerImageTypeName("ns::Box<ns::Inner<Local> >"));
static_assert(isPerImageTypeName("struct ns::Box<struct Global>"));      // MSVC
static_assert(isPerImageTypeName("ns::Box<game::f()::Local>"));          // GCC
// Keyed by name.
static_assert(!isPerImageTypeName("helios::ecs::NetIdentity"));
static_assert(!isPerImageTypeName("struct helios::ecs::NetIdentity"));
static_assert(!isPerImageTypeName("ns::Box<int>"));
static_assert(!isPerImageTypeName("ns::Outer::Inner"));
static_assert(!isPerImageTypeName("ns::Box<long unsigned int, 3>"));                       // GCC
static_assert(!isPerImageTypeName("ns::Box<unsigned long, 3UL>"));                         // Clang
static_assert(!isPerImageTypeName("struct ns::Box<class ns::A,unsigned __int64>"));        // MSVC
static_assert(!isPerImageTypeName("ns::Fn<void (__cdecl *)(const ns::A &) noexcept>"));    // MSVC
static_assert(!isPerImageTypeName("ns::Fn<void (*)(const volatile ns::A&, signed char)>")); // GCC
static_assert(!isPerImageTypeName("ns::Flag<true, false, nullptr>"));
// What a name cannot tell apart (the header's documented residual): Clang prints a class nested in a local
// class as "Local::Inner" and a pointer to a member of a local class as "int Local::*", as the type itself
// and as an argument.
static_assert(!isPerImageTypeName("Local::Inner"));
static_assert(!isPerImageTypeName("ns::Box<Local::Inner>"));
static_assert(!isPerImageTypeName("int Local::*"));
static_assert(!isPerImageTypeName("ns::Box<int Local::*>"));

// ---------------------------------------------------------------------------------------------
// This compiler's keys. The name key of a class, union or enum whose name has neither template arguments nor
// an inline namespace is the same on every compiler: its canonical name is pinned here.
// ---------------------------------------------------------------------------------------------

static_assert(tk::kKeyedByName<NetIdentity>);
static_assert(tk::kCanonicalNameHash<NetIdentity> == fnv1a64("helios::ecs::NetIdentity"));
static_assert(tk::kNameTypeKey<NetIdentity> ==
              tk::nameTypeKey(fnv1a64("helios::ecs::NetIdentity"), sizeof(NetIdentity), alignof(NetIdentity)));
static_assert(tk::kCanonicalNameHash<RepDirty> == fnv1a64("helios::ecs::RepDirty"));
static_assert(tk::kCanonicalNameHash<ecs_test::Position> == fnv1a64("ecs_test::Position"));
static_assert(tk::kCanonicalNameHash<ecs_test::KeyOuter::Inner> == fnv1a64("ecs_test::KeyOuter::Inner"));
static_assert(tk::kCanonicalNameHash<ecs_test::KeyKind> == fnv1a64("ecs_test::KeyKind"));
static_assert(tk::kCanonicalNameHash<ecs_test::KeyUnion> == fnv1a64("ecs_test::KeyUnion"));
// A template of class and enum types also matches on GCC, Clang and MSVC (guaranteed only within one compiler).
static_assert(tk::kCanonicalNameHash<ecs_test::KeyPair<ecs_test::Position, ecs_test::KeyKind>> ==
              fnv1a64("ecs_test::KeyPair<ecs_test::Position,ecs_test::KeyKind>"));
static_assert(tk::kKeyedByName<ecs_test::KeyPair<ecs_test::Position, ecs_test::KeyKind>>);
static_assert(tk::kKeyedByName<ecs_test::KeyPair<ecs_test::Position, u32>>); // a fundamental argument
static_assert(tk::kNameTypeKey<NetIdentity> % 2 == 1); // name keys are odd, per-image keys even
static_assert(tk::kNameTypeKey<ecs_test::Position> != tk::kNameTypeKey<ecs_test::Velocity>);
static_assert(tk::kNameTypeKey<NetIdentity> != tk::kNameTypeKey<RepDirty>);
static_assert(tk::kNameTypeKey<ecs_test::KeyNumbered<1>> != tk::kNameTypeKey<ecs_test::KeyNumbered<2>>);
// The layout is part of a name key: one name, another size, another key.
static_assert(tk::nameTypeKey(fnv1a64("ns::A"), 4, 4) != tk::nameTypeKey(fnv1a64("ns::A"), 16, 8));

// Names that hold only within one compiler (type_key.h), pinned per compiler so that CI records each
// compiler's spelling. An inline namespace: GCC and MSVC print it, Clang and clang-cl leave it out.
static_assert(tk::kKeyedByName<ecs_test::KeyVersioned>);
#if defined(HELIOS_COMPILER_CLANG)
static_assert(canonicalNameEquals(tk::kPrintedTypeName<ecs_test::KeyVersioned>, "ecs_test::KeyVersioned"));
#else
static_assert(canonicalNameEquals(tk::kPrintedTypeName<ecs_test::KeyVersioned>,
                                  "ecs_test::v1::KeyVersioned"));
#endif
// A type nested in a specialization is not a template, but its name has the template's arguments, which GCC
// and Clang spell differently.
using NestedInSpecialization = ecs_test::KeyHolder<unsigned long>::Inner;
static_assert(tk::kKeyedByName<NestedInSpecialization>);
#if defined(HELIOS_COMPILER_GCC)
static_assert(canonicalNameEquals(tk::kPrintedTypeName<NestedInSpecialization>,
                                  "ecs_test::KeyHolder<long unsigned int>::Inner"));
#elif defined(HELIOS_COMPILER_CLANG)
static_assert(canonicalNameEquals(tk::kPrintedTypeName<NestedInSpecialization>,
                                  "ecs_test::KeyHolder<unsigned long>::Inner"));
#endif

// Clang and clang-cl print a local class without its function, so a type named through one is keyed by name
// there (the header's residual) and per image on GCC and MSVC, which print the function.
#if defined(HELIOS_COMPILER_CLANG)
constexpr bool kLocalMembersKeyedByName = true;
#else
constexpr bool kLocalMembersKeyedByName = false;
#endif

struct Cooldown {
    u32 ticks = 0;
};

struct KeyClash {
    u32 value = 0;
};

static_assert(!tk::kKeyedByName<Cooldown>);
static_assert(!tk::kKeyedByName<KeyClash>);
static_assert(!tk::kKeyedByName<GlobalKeyed>);

std::atomic<u32> g_verifyFailures{0};

AssertAction countVerifyFailures(const AssertInfo& info) {
    if (std::string_view(info.kind) == "VERIFY") g_verifyFailures.fetch_add(1);
    return AssertAction::Continue;
}

TEST_CASE("ecs type keys: the built-ins are bound by key") {
    World world;
    CHECK(world.id<NetIdentity>() == world.netIdentityId());
    CHECK(world.idForKey(typeKey<NetIdentity>()) == world.netIdentityId());
    CHECK(world.idForKey(typeKey<RepDirty>()) == world.repDirtyId());
    CHECK(world.id<FrameRef>() != 0);
    CHECK(world.id<DockRef>() != 0);
    CHECK(world.id<KeyClash>() == 0);
    CHECK(world.replicationBit<KeyClash>() == 0);
    CHECK(typeKey<const ecs_test::Position>() == typeKey<ecs_test::Position>());
    CHECK(typeKey<const KeyClash>() == typeKey<KeyClash>());
}

template <u32... N>
void registerAndCheck(World& world, std::integer_sequence<u32, N...>) {
    const ComponentId ids[] = {world.registerComponent<ecs_test::KeyNumbered<N>>()...};
    const ComponentId found[] = {world.id<ecs_test::KeyNumbered<N>>()...};
    for (usize i = 0; i < sizeof...(N); ++i) {
        CAPTURE(i);
        CHECK(ids[i] != 0);
        CHECK(found[i] == ids[i]);
    }
    // Every id is distinct: the table kept each key apart through its growth.
    for (usize i = 0; i < sizeof...(N); ++i) {
        for (usize j = i + 1; j < sizeof...(N); ++j) CHECK(ids[i] != ids[j]);
    }
}

TEST_CASE("ecs type keys: many types in one world keep their ids as the table grows") {
    World world;
    registerAndCheck(world, std::make_integer_sequence<u32, 70>{});
    CHECK(world.id<NetIdentity>() == world.netIdentityId());
    // Registering a type again is idempotent and keeps its binding.
    const ComponentId again = world.registerComponent<ecs_test::KeyNumbered<3>>();
    CHECK(again == world.id<ecs_test::KeyNumbered<3>>());
}

TEST_CASE("ecs type keys: an unregistered type never resolves to another type's component") {
    // test_type_keys_other.cpp's Cooldown prints this Cooldown's name, but it is another, larger type.
    World world;
    REQUIRE(world.registerComponent<Cooldown>() != 0);
    const Entity e = world.spawn();
    world.set(e, Cooldown{5});
    CHECK(ecs_test::otherCooldownId(world) == 0);
    CHECK(ecs_test::otherCooldownGet(world, e) == nullptr);
    CHECK_FALSE(ecs_test::otherCooldownHas(world, e));
    CHECK(ecs_test::otherCooldownStarted(world, e) == ~u64(0));

    // Registered too, each type keeps its own component.
    const ComponentId other = ecs_test::otherCooldownRegisterAndSet(world, e, 7, 9);
    REQUIRE(other != 0);
    CHECK(other != world.id<Cooldown>());
    CHECK(ecs_test::otherCooldownId(world) == other);
    CHECK(ecs_test::otherCooldownStarted(world, e) == 7);
    REQUIRE(world.get<Cooldown>(e) != nullptr);
    CHECK(world.get<Cooldown>(e)->ticks == 5);
}

TEST_CASE("ecs type keys: two types with one printed name bind to their own components") {
    World world;
    const Result<ComponentId> a = world.registerComponent(componentDescOf<KeyClash>("ecs_test.KeyClashA"));
    const Result<ComponentId> b = world.registerComponent(componentDescOf<KeyClash>("ecs_test.KeyClashB"));
    REQUIRE(a.hasValue());
    REQUIRE(b.hasValue());
    CHECK(ecs_test::otherKeyClashKey() != typeKey<KeyClash>());
    REQUIRE(world.bindType<KeyClash>(*a).hasValue());
    CHECK(world.bindType<KeyClash>(*a).hasValue()); // the same binding again is fine
    CHECK(ecs_test::bindOtherKeyClash(world, *b).hasValue());
    CHECK(world.id<KeyClash>() == *a);
    CHECK(ecs_test::otherKeyClashId(world) == *b);
    // A component bound to one type cannot be bound to another.
    const Result<void> twice = world.bindType<ecs_test::KeyNumbered<7>>(*a);
    REQUIRE_FALSE(twice.hasValue());
    CHECK(twice.errorCode() == ErrorCode::AlreadyExists);
    CHECK(world.id<ecs_test::KeyNumbered<7>>() == 0);
}

// Two local classes with one name. Clang prints both as "Local", like a global-namespace type.
u32 useLocalA(World& world, Entity e, u32 value) {
    struct Local {
        struct Inner {
            u32 value = 0;
        };
        enum class Kind : u8 { A };
        u32 value = 0;
    };
    static_assert(!tk::kKeyedByName<Local>);
    static_assert(tk::kKeyedByName<Local::Inner> == kLocalMembersKeyedByName);
    static_assert(tk::kKeyedByName<Local::Kind> == kLocalMembersKeyedByName);
    static_assert(tk::kKeyedByName<ecs_test::KeyPair<u32 Local::*, u32>> == kLocalMembersKeyedByName);
    if (world.id<Local>() == 0) world.registerComponent<Local>(ComponentFlags::None, "ecs_test.LocalA");
    if (value != 0) world.set(e, Local{value});
    const Local* local = world.get<Local>(e);
    return local ? local->value : 0;
}

u32 useLocalB(World& world, Entity e, u32 value) {
    struct Local {
        u32 value = 0;
    };
    static_assert(!tk::kKeyedByName<Local>);
    if (world.id<Local>() == 0) world.registerComponent<Local>(ComponentFlags::None, "ecs_test.LocalB");
    if (value != 0) world.set(e, Local{value});
    const Local* local = world.get<Local>(e);
    return local ? local->value : 0;
}

TEST_CASE("ecs type keys: local classes with one name keep their own components") {
    World world;
    const Entity e = world.spawn();
    CHECK(useLocalA(world, e, 1) == 1);
    CHECK(useLocalB(world, e, 2) == 2);
    CHECK(useLocalA(world, e, 0) == 1);
    CHECK(useLocalB(world, e, 0) == 2);
    CHECK(world.findComponent("ecs_test.LocalA") != nullptr);
    CHECK(world.findComponent("ecs_test.LocalB") != nullptr);
}

// A class template specialized on local classes with one name. Clang and clang-cl print both specializations
// as "ecs_test::KeyPair<Local, unsigned int>" (review round 3, finding 2).
u32 useBoxedLocalA(World& world, Entity e, u32 value) {
    struct Local {
        u32 hp = 0;
    };
    using Boxed = ecs_test::KeyPair<Local, u32>;
    static_assert(!tk::kKeyedByName<Boxed>);
    if (world.id<Boxed>() == 0) world.registerComponent<Boxed>(ComponentFlags::None, "ecs_test.BoxedLocalA");
    if (value != 0) world.set(e, Boxed{Local{value}, 0});
    const Boxed* boxed = world.get<Boxed>(e);
    return boxed ? boxed->a.hp : 0;
}

u32 useBoxedLocalB(World& world, Entity e, u32 value) {
    struct Local {
        u32 ammo = 0;
    };
    using Boxed = ecs_test::KeyPair<Local, u32>;
    static_assert(!tk::kKeyedByName<Boxed>);
    if (world.id<Boxed>() == 0) world.registerComponent<Boxed>(ComponentFlags::None, "ecs_test.BoxedLocalB");
    if (value != 0) world.set(e, Boxed{Local{value}, 0});
    const Boxed* boxed = world.get<Boxed>(e);
    return boxed ? boxed->a.ammo : 0;
}

TEST_CASE("ecs type keys: templates of local classes with one name keep their own components") {
    World world;
    const Entity e = world.spawn();
    CHECK(useBoxedLocalA(world, e, 1) == 1);
    CHECK(useBoxedLocalB(world, e, 2) == 2);
    CHECK(useBoxedLocalA(world, e, 0) == 1);
    CHECK(useBoxedLocalB(world, e, 0) == 2);
    CHECK(world.findComponent("ecs_test.BoxedLocalA") != nullptr);
    CHECK(world.findComponent("ecs_test.BoxedLocalB") != nullptr);
}

TEST_CASE("ecs type keys: a global-namespace type has one per-image key in every translation unit") {
    const TypeKey key = typeKey<GlobalKeyed>();
    CHECK(key != 0);
    CHECK(key % 2 == 0);
    CHECK(ecs_test::otherGlobalKeyedKey() == key);
    CHECK(typeKey<GlobalKeyed>() == key); // drawn once
    World world;
    const ComponentId id = world.registerComponent<GlobalKeyed>();
    REQUIRE(id != 0);
    CHECK(world.idForKey(ecs_test::otherGlobalKeyedKey()) == id);
}

TEST_CASE("ecs type keys: registerComponent<T> returns 0 when T cannot be bound") {
    World world;
    const ComponentId taken =
        world.registerComponent<ecs_test::KeyNumbered<100>>(ComponentFlags::None, "ecs_test.Taken");
    REQUIRE(taken != 0);
    const AssertHandler previous = setAssertHandler(countVerifyFailures);
    g_verifyFailures = 0;
    // The name resolves to a component of the same layout that another C++ type is bound to.
    const ComponentId sameName =
        world.registerComponent<ecs_test::KeyNumbered<101>>(ComponentFlags::None, "ecs_test.Taken");
    const u32 failuresAfterName = g_verifyFailures.load();
    // T is already bound to another component.
    const ComponentId sameType =
        world.registerComponent<ecs_test::KeyNumbered<100>>(ComponentFlags::None, "ecs_test.TakenAgain");
    const u32 failuresAfterType = g_verifyFailures.load();
    setAssertHandler(previous);
    CHECK(sameName == 0);
    CHECK(failuresAfterName == 1);
    CHECK(world.id<ecs_test::KeyNumbered<101>>() == 0);
    CHECK(sameType == 0);
    CHECK(failuresAfterType == 2);
    CHECK(world.id<ecs_test::KeyNumbered<100>>() == taken); // the first binding stands
}

} // namespace
