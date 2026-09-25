#include <doctest/doctest.h>

#include <format>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "helios/core/name.h"

using namespace helios;
using namespace helios::literals;

TEST_CASE("name: interning yields stable ids and text") {
    const Name a("ship.hull");
    const Name b(std::string("ship.hull"));
    const Name c("ship.shield");
    CHECK(a == b);
    CHECK(a.id() == b.id());
    CHECK(a != c);
    CHECK(a.view() == "ship.hull");
    CHECK(std::string_view(a.c_str()) == "ship.hull");
    CHECK(a.toString() == "ship.hull");
    CHECK(a.view().data() == b.view().data()); // same interned storage

    const Name none;
    CHECK(none.isNone());
    CHECK(!none);
    CHECK(none.view().empty());
    CHECK(Name("").isNone());
    CHECK(Name("case") != Name("CASE"));
}

TEST_CASE("name: find, fromId and ordering") {
    CHECK(Name::find("name-test-never-interned-xyz").isNone());
    const Name n("name-test-find");
    CHECK(Name::find("name-test-find") == n);
    CHECK(Name::fromId(n.id()) == n);
    CHECK(Name::fromId(0xFFFFFF00u).isNone());
    CHECK(Name::internedCount() >= 2);
    CHECK(Name::lexicalLess(Name("alpha"), Name("beta")));
    std::unordered_set<Name> set{Name("x"), Name("y"), Name("x")};
    CHECK(set.size() == 2);
    CHECK(std::format("[{}]", n) == "[name-test-find]");
}

TEST_CASE("name: concurrent interning agrees on ids") {
    constexpr int kThreads = 8;
    constexpr int kStrings = 2000;
    std::vector<std::vector<u32>> ids(kThreads, std::vector<u32>(kStrings));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &ids] {
            for (int i = 0; i < kStrings; ++i) {
                // Different threads walk the strings in different orders to maximize races.
                const int k = (t % 2 == 0) ? i : kStrings - 1 - i;
                ids[t][k] = Name(std::format("concurrent.name.{}", k)).id();
            }
        });
    }
    for (auto& th : threads) th.join();
    std::unordered_set<u32> distinct;
    for (int i = 0; i < kStrings; ++i) {
        for (int t = 1; t < kThreads; ++t) CHECK(ids[t][i] == ids[0][i]);
        distinct.insert(ids[0][i]);
        CHECK(Name::fromId(ids[0][i]).view() == std::format("concurrent.name.{}", i));
    }
    CHECK(distinct.size() == static_cast<usize>(kStrings));
}

TEST_CASE("name: long strings are interned intact") {
    const std::string big(100'000, 'q');
    const Name n(big);
    CHECK(n.view().size() == big.size());
    CHECK(n.view() == big);
}

TEST_CASE("string id: constexpr hashing and debug registry") {
    constexpr StringId a("weapon.laser");
    static_assert(a.value() == fnv1a64("weapon.laser"));
    static_assert("weapon.laser"_sid == a);
    static_assert(StringId().isNone());
    switch (StringId("weapon.laser").value()) {
    case "weapon.laser"_sid.value(): CHECK(true); break;
    default: CHECK(false); break;
    }

    const StringId made = StringId::make("weapon.railgun");
    CHECK(made == StringId("weapon.railgun"));
    CHECK(made.debugString() == "weapon.railgun");
    CHECK(registerStringId("weapon.railgun")); // re-registering the same text is fine
    const StringId unknown = StringId::fromValue(0x1234);
    CHECK(unknown.debugString() == "#0000000000001234");
    CHECK(std::format("{}", made) == "weapon.railgun");
    std::unordered_set<StringId> set{a, made, a};
    CHECK(set.size() == 2);
}
