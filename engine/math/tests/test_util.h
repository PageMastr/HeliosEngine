// Shared helpers for the math tests: deterministic RNG, ulp distance, doctest printers.
#pragma once

#include <doctest/doctest.h>

#include "helios/math/color.h"
#include "helios/math/frame.h"
#include "helios/math/geometry.h"
#include "helios/math/mat.h"
#include "helios/math/pack.h"
#include "helios/math/quat.h"
#include "helios/math/scalar.h"
#include "helios/math/transform.h"
#include "helios/math/vec.h"

#include <bit>
#include <cstdio>
#include <string>

namespace helios::test {

/// SplitMix64 sequence: deterministic across platforms (unlike std distributions).
struct Rng {
    u64 state;
    explicit Rng(u64 seed) : state(seed) {}
    u64 next() {
        state += 0x9e3779b97f4a7c15ULL;
        return hashU64(state);
    }
    u32 nextU32() { return static_cast<u32>(next() >> 32); }
    /// [0, 1)
    f64 uniform() { return hashToUnitF64(next()); }
    f64 range(f64 lo, f64 hi) { return lo + (hi - lo) * uniform(); }
    f32 rangef(f32 lo, f32 hi) { return static_cast<f32>(range(lo, hi)); }
    Vec2 vec2(f32 lo, f32 hi) { return {rangef(lo, hi), rangef(lo, hi)}; }
    Vec3 vec3(f32 lo, f32 hi) { return {rangef(lo, hi), rangef(lo, hi), rangef(lo, hi)}; }
    DVec3 dvec3(f64 lo, f64 hi) { return {range(lo, hi), range(lo, hi), range(lo, hi)}; }
    /// Uniform direction (rejection sampling).
    DVec3 unitD() {
        for (;;) {
            const DVec3 v = dvec3(-1.0, 1.0);
            const f64 l2 = lengthSq(v);
            if (l2 > 1e-6 && l2 <= 1.0) return v / std::sqrt(l2);
        }
    }
    Vec3 unit() { return toF32(unitD()); }
    /// Uniform random rotation (Shoemake).
    DQuat quatD() {
        const f64 u1 = uniform(), u2 = uniform() * kTwoPiD, u3 = uniform() * kTwoPiD;
        const f64 a = std::sqrt(1.0 - u1), b = std::sqrt(u1);
        return {a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)};
    }
    Quat quat() { return normalize(toF32(quatD())); }
};

/// Distance in units in the last place between two finite doubles of the same sign.
inline u64 ulpDistance(f64 a, f64 b) {
    if (a == b) return 0;
    const i64 ia = std::bit_cast<i64>(a), ib = std::bit_cast<i64>(b);
    if ((ia < 0) != (ib < 0)) return ~0ull;
    return ia > ib ? static_cast<u64>(ia - ib) : static_cast<u64>(ib - ia);
}
inline u64 ulpDistance(f32 a, f32 b) {
    if (a == b) return 0;
    const i32 ia = std::bit_cast<i32>(a), ib = std::bit_cast<i32>(b);
    if ((ia < 0) != (ib < 0)) return ~0ull;
    return ia > ib ? static_cast<u64>(ia - ib) : static_cast<u64>(ib - ia);
}
inline u64 bits(f64 v) { return std::bit_cast<u64>(v); }
inline u32 bits(f32 v) { return std::bit_cast<u32>(v); }

inline std::string fmt(f64 v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

}  // namespace helios::test

namespace doctest {
template <class T>
struct StringMaker<helios::TVec2<T>> {
    static String convert(const helios::TVec2<T>& v) {
        using helios::test::fmt;
        return ("(" + fmt(static_cast<double>(v.x)) + ", " + fmt(static_cast<double>(v.y)) + ")").c_str();
    }
};
template <class T>
struct StringMaker<helios::TVec3<T>> {
    static String convert(const helios::TVec3<T>& v) {
        using helios::test::fmt;
        return ("(" + fmt(static_cast<double>(v.x)) + ", " + fmt(static_cast<double>(v.y)) + ", " +
                fmt(static_cast<double>(v.z)) + ")")
            .c_str();
    }
};
template <class T>
struct StringMaker<helios::TVec4<T>> {
    static String convert(const helios::TVec4<T>& v) {
        using helios::test::fmt;
        return ("(" + fmt(static_cast<double>(v.x)) + ", " + fmt(static_cast<double>(v.y)) + ", " +
                fmt(static_cast<double>(v.z)) + ", " + fmt(static_cast<double>(v.w)) + ")")
            .c_str();
    }
};
template <class T>
struct StringMaker<helios::TQuat<T>> {
    static String convert(const helios::TQuat<T>& q) {
        using helios::test::fmt;
        return ("quat(" + fmt(q.x) + ", " + fmt(q.y) + ", " + fmt(q.z) + ", " + fmt(q.w) + ")").c_str();
    }
};
template <>
struct StringMaker<helios::Color> {
    static String convert(const helios::Color& c) {
        using helios::test::fmt;
        return ("color(" + fmt(c.r) + ", " + fmt(c.g) + ", " + fmt(c.b) + ", " + fmt(c.a) + ")").c_str();
    }
};
}  // namespace doctest
