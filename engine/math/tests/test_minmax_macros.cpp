// The math headers must survive <windows.h>-style min/max macros (a Win32 TU that forgot
// NOMINMAX) and must leave those macros exactly as they found them.
//
// The standard headers the math headers use come first, as they do in a real TU that includes
// <windows.h>: libstdc++ itself cannot be included after such macros (MSVC's STL can), and its
// c++config.h #undefs min/max on first inclusion.
#include <doctest/doctest.h>

#include <cassert>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <type_traits>

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#include "helios/math/all.h"

#if !defined(min) || !defined(max)
#error "helios/math headers must restore the caller's min/max macros"
#endif

TEST_CASE("headers: tolerate and restore min/max macros") {
    // Parenthesized names suppress function-like macro expansion.
    CHECK((helios::min)(3, 5) == 3);
    CHECK((helios::max)(helios::Vec2(1.0f, 4.0f), helios::Vec2(2.0f, 3.0f)) == helios::Vec2(2.0f, 4.0f));
    CHECK(min(2, 7) == 2);  // the caller's macro is still active
    const helios::AABB box{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    CHECK(box.contains(helios::Vec3(0.5f)));
}

#undef min
#undef max
