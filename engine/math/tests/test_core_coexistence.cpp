// helios/math and helios/core share namespace helios, and engine code includes both. math_tests
// cannot link helios::core (math is a leaf module), so this TU re-declares the core names that
// overlap with math exactly as helios/core declares them, then includes every math header.
//
// Regression: math used to declare hashCombine(u32, u32) next to core's hashCombine(u64, u64).
// A mixed-width call was then ambiguous (compile error), and a u32 call site silently switched
// from core's 64-bit combine to math's 32-bit one depending on which headers were included.
#include <cstdint>
#include <type_traits>

namespace helios {
// Same aliases as helios/core/types.h (redeclaring an alias to the same type is legal).
using u32 = std::uint32_t;
using u64 = std::uint64_t;
// Same signature as helios/core/hash.h.
constexpr u64 hashCombine(u64 seed, u64 value) noexcept {
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}
}  // namespace helios

#include "helios/math/all.h"

#include <doctest/doctest.h>

TEST_CASE("math headers coexist with helios/core declarations") {
    // Both calls would be ambiguous or re-routed if math declared another hashCombine overload.
    constexpr helios::u64 mixed = helios::hashCombine(helios::u64{1}, 2u);
    static_assert(std::is_same_v<decltype(helios::hashCombine(1u, 2u)), helios::u64>);
    CHECK(mixed == helios::hashCombine(1ull, 2ull));
    // math's own 32-bit combine has a distinct name.
    static_assert(std::is_same_v<decltype(helios::hashCombineU32(1u, 2u)), helios::u32>);
    CHECK(helios::hashCombineU32(1u, 2u) != helios::hashCombineU32(2u, 1u));
}
