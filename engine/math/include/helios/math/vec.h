// helios/math/vec.h — 2/3/4-component vectors.
//
//   Vec2/Vec3/Vec4     f32  (rendering, local offsets, GPU data)
//   DVec2/DVec3/DVec4  f64  (frame-local world positions, CPU culling; see ADR-005)
//   IVec2/IVec3/IVec4  i32  (grid cells, texel coordinates)
//   UVec2/UVec3/UVec4  u32
//
// Layout is exactly N tightly packed components (static_asserted), so arrays of vectors can be
// memcpy'd to GPU buffers. Components default to zero. Conversions between element types are
// always explicit (Vec3(dvec) or toF32(dvec)) because f64 -> f32 loses precision silently.
// Arithmetic operators are component-wise; `*` between two vectors is the Hadamard product.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/scalar.h"

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

template <class T>
struct TVec2;
template <class T>
struct TVec3;
template <class T>
struct TVec4;

template <class T>
struct TVec2 {
    static_assert(std::is_arithmetic_v<T> && sizeof(T) >= 4, "TVec2 supports 32/64-bit arithmetic types");
    using ValueType = T;
    static constexpr int kSize = 2;

    T x{};
    T y{};

    constexpr TVec2() noexcept = default;
    constexpr TVec2(T ax, T ay) noexcept : x(ax), y(ay) {}
    /// Splat: all components = s.
    constexpr explicit TVec2(T s) noexcept : x(s), y(s) {}
    /// Explicit element-type conversion (e.g. DVec2 -> Vec2).
    template <class U>
    constexpr explicit TVec2(const TVec2<U>& o) noexcept : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)) {}

    [[nodiscard]] static constexpr TVec2 zero() noexcept { return {}; }
    [[nodiscard]] static constexpr TVec2 one() noexcept { return TVec2(T(1)); }
    [[nodiscard]] static constexpr TVec2 unitX() noexcept { return {T(1), T(0)}; }
    [[nodiscard]] static constexpr TVec2 unitY() noexcept { return {T(0), T(1)}; }

    /// Component access by index (0..1).
    [[nodiscard]] constexpr T& operator[](int i) noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 2);
        return i == 0 ? x : y;
    }
    [[nodiscard]] constexpr const T& operator[](int i) const noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 2);
        return i == 0 ? x : y;
    }
    [[nodiscard]] T* data() noexcept { return &x; }
    [[nodiscard]] const T* data() const noexcept { return &x; }

    [[nodiscard]] constexpr TVec2 yx() const noexcept { return {y, x}; }

    constexpr TVec2& operator+=(const TVec2& o) noexcept {
        x += o.x;
        y += o.y;
        return *this;
    }
    constexpr TVec2& operator-=(const TVec2& o) noexcept {
        x -= o.x;
        y -= o.y;
        return *this;
    }
    constexpr TVec2& operator*=(const TVec2& o) noexcept {
        x *= o.x;
        y *= o.y;
        return *this;
    }
    constexpr TVec2& operator/=(const TVec2& o) noexcept {
        x /= o.x;
        y /= o.y;
        return *this;
    }
    constexpr TVec2& operator*=(T s) noexcept {
        x *= s;
        y *= s;
        return *this;
    }
    constexpr TVec2& operator/=(T s) noexcept {
        x /= s;
        y /= s;
        return *this;
    }

    friend constexpr TVec2 operator+(const TVec2& a, const TVec2& b) noexcept {
        return {a.x + b.x, a.y + b.y};
    }
    friend constexpr TVec2 operator-(const TVec2& a, const TVec2& b) noexcept {
        return {a.x - b.x, a.y - b.y};
    }
    friend constexpr TVec2 operator*(const TVec2& a, const TVec2& b) noexcept {
        return {a.x * b.x, a.y * b.y};
    }
    friend constexpr TVec2 operator/(const TVec2& a, const TVec2& b) noexcept {
        return {a.x / b.x, a.y / b.y};
    }
    friend constexpr TVec2 operator*(const TVec2& a, T s) noexcept { return {a.x * s, a.y * s}; }
    friend constexpr TVec2 operator*(T s, const TVec2& a) noexcept { return {s * a.x, s * a.y}; }
    friend constexpr TVec2 operator/(const TVec2& a, T s) noexcept { return {a.x / s, a.y / s}; }
    friend constexpr TVec2 operator-(const TVec2& a) noexcept { return {-a.x, -a.y}; }
    friend constexpr bool operator==(const TVec2& a, const TVec2& b) noexcept = default;
};

template <class T>
struct TVec3 {
    static_assert(std::is_arithmetic_v<T> && sizeof(T) >= 4, "TVec3 supports 32/64-bit arithmetic types");
    using ValueType = T;
    static constexpr int kSize = 3;

    T x{};
    T y{};
    T z{};

    constexpr TVec3() noexcept = default;
    constexpr TVec3(T ax, T ay, T az) noexcept : x(ax), y(ay), z(az) {}
    constexpr explicit TVec3(T s) noexcept : x(s), y(s), z(s) {}
    constexpr TVec3(const TVec2<T>& axy, T az) noexcept : x(axy.x), y(axy.y), z(az) {}
    template <class U>
    constexpr explicit TVec3(const TVec3<U>& o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)) {}

    [[nodiscard]] static constexpr TVec3 zero() noexcept { return {}; }
    [[nodiscard]] static constexpr TVec3 one() noexcept { return TVec3(T(1)); }
    [[nodiscard]] static constexpr TVec3 unitX() noexcept { return {T(1), T(0), T(0)}; }
    [[nodiscard]] static constexpr TVec3 unitY() noexcept { return {T(0), T(1), T(0)}; }
    [[nodiscard]] static constexpr TVec3 unitZ() noexcept { return {T(0), T(0), T(1)}; }

    [[nodiscard]] constexpr T& operator[](int i) noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 3);
        return i == 0 ? x : (i == 1 ? y : z);
    }
    [[nodiscard]] constexpr const T& operator[](int i) const noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 3);
        return i == 0 ? x : (i == 1 ? y : z);
    }
    [[nodiscard]] T* data() noexcept { return &x; }
    [[nodiscard]] const T* data() const noexcept { return &x; }

    // Common swizzles; use swizzle<...>() for anything else.
    [[nodiscard]] constexpr TVec2<T> xy() const noexcept { return {x, y}; }
    [[nodiscard]] constexpr TVec2<T> xz() const noexcept { return {x, z}; }
    [[nodiscard]] constexpr TVec2<T> yz() const noexcept { return {y, z}; }
    [[nodiscard]] constexpr TVec3 zyx() const noexcept { return {z, y, x}; }

    constexpr TVec3& operator+=(const TVec3& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr TVec3& operator-=(const TVec3& o) noexcept {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    constexpr TVec3& operator*=(const TVec3& o) noexcept {
        x *= o.x;
        y *= o.y;
        z *= o.z;
        return *this;
    }
    constexpr TVec3& operator/=(const TVec3& o) noexcept {
        x /= o.x;
        y /= o.y;
        z /= o.z;
        return *this;
    }
    constexpr TVec3& operator*=(T s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    constexpr TVec3& operator/=(T s) noexcept {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }

    friend constexpr TVec3 operator+(const TVec3& a, const TVec3& b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    friend constexpr TVec3 operator-(const TVec3& a, const TVec3& b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    friend constexpr TVec3 operator*(const TVec3& a, const TVec3& b) noexcept {
        return {a.x * b.x, a.y * b.y, a.z * b.z};
    }
    friend constexpr TVec3 operator/(const TVec3& a, const TVec3& b) noexcept {
        return {a.x / b.x, a.y / b.y, a.z / b.z};
    }
    friend constexpr TVec3 operator*(const TVec3& a, T s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr TVec3 operator*(T s, const TVec3& a) noexcept { return {s * a.x, s * a.y, s * a.z}; }
    friend constexpr TVec3 operator/(const TVec3& a, T s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
    friend constexpr TVec3 operator-(const TVec3& a) noexcept { return {-a.x, -a.y, -a.z}; }
    friend constexpr bool operator==(const TVec3& a, const TVec3& b) noexcept = default;
};

template <class T>
struct TVec4 {
    static_assert(std::is_arithmetic_v<T> && sizeof(T) >= 4, "TVec4 supports 32/64-bit arithmetic types");
    using ValueType = T;
    static constexpr int kSize = 4;

    T x{};
    T y{};
    T z{};
    T w{};

    constexpr TVec4() noexcept = default;
    constexpr TVec4(T ax, T ay, T az, T aw) noexcept : x(ax), y(ay), z(az), w(aw) {}
    constexpr explicit TVec4(T s) noexcept : x(s), y(s), z(s), w(s) {}
    constexpr TVec4(const TVec3<T>& axyz, T aw) noexcept : x(axyz.x), y(axyz.y), z(axyz.z), w(aw) {}
    constexpr TVec4(const TVec2<T>& axy, const TVec2<T>& azw) noexcept
        : x(axy.x), y(axy.y), z(azw.x), w(azw.y) {}
    template <class U>
    constexpr explicit TVec4(const TVec4<U>& o) noexcept
        : x(static_cast<T>(o.x)), y(static_cast<T>(o.y)), z(static_cast<T>(o.z)), w(static_cast<T>(o.w)) {}

    [[nodiscard]] static constexpr TVec4 zero() noexcept { return {}; }
    [[nodiscard]] static constexpr TVec4 one() noexcept { return TVec4(T(1)); }
    [[nodiscard]] static constexpr TVec4 unitX() noexcept { return {T(1), T(0), T(0), T(0)}; }
    [[nodiscard]] static constexpr TVec4 unitY() noexcept { return {T(0), T(1), T(0), T(0)}; }
    [[nodiscard]] static constexpr TVec4 unitZ() noexcept { return {T(0), T(0), T(1), T(0)}; }
    [[nodiscard]] static constexpr TVec4 unitW() noexcept { return {T(0), T(0), T(0), T(1)}; }

    [[nodiscard]] constexpr T& operator[](int i) noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 4);
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    [[nodiscard]] constexpr const T& operator[](int i) const noexcept {
        HELIOS_MATH_ASSERT(i >= 0 && i < 4);
        return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w));
    }
    [[nodiscard]] T* data() noexcept { return &x; }
    [[nodiscard]] const T* data() const noexcept { return &x; }

    [[nodiscard]] constexpr TVec2<T> xy() const noexcept { return {x, y}; }
    [[nodiscard]] constexpr TVec2<T> zw() const noexcept { return {z, w}; }
    [[nodiscard]] constexpr TVec3<T> xyz() const noexcept { return {x, y, z}; }

    constexpr TVec4& operator+=(const TVec4& o) noexcept {
        x += o.x;
        y += o.y;
        z += o.z;
        w += o.w;
        return *this;
    }
    constexpr TVec4& operator-=(const TVec4& o) noexcept {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        w -= o.w;
        return *this;
    }
    constexpr TVec4& operator*=(const TVec4& o) noexcept {
        x *= o.x;
        y *= o.y;
        z *= o.z;
        w *= o.w;
        return *this;
    }
    constexpr TVec4& operator/=(const TVec4& o) noexcept {
        x /= o.x;
        y /= o.y;
        z /= o.z;
        w /= o.w;
        return *this;
    }
    constexpr TVec4& operator*=(T s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        w *= s;
        return *this;
    }
    constexpr TVec4& operator/=(T s) noexcept {
        x /= s;
        y /= s;
        z /= s;
        w /= s;
        return *this;
    }

    friend constexpr TVec4 operator+(const TVec4& a, const TVec4& b) noexcept {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    friend constexpr TVec4 operator-(const TVec4& a, const TVec4& b) noexcept {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    friend constexpr TVec4 operator*(const TVec4& a, const TVec4& b) noexcept {
        return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
    }
    friend constexpr TVec4 operator/(const TVec4& a, const TVec4& b) noexcept {
        return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
    }
    friend constexpr TVec4 operator*(const TVec4& a, T s) noexcept {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }
    friend constexpr TVec4 operator*(T s, const TVec4& a) noexcept {
        return {s * a.x, s * a.y, s * a.z, s * a.w};
    }
    friend constexpr TVec4 operator/(const TVec4& a, T s) noexcept {
        return {a.x / s, a.y / s, a.z / s, a.w / s};
    }
    friend constexpr TVec4 operator-(const TVec4& a) noexcept { return {-a.x, -a.y, -a.z, -a.w}; }
    friend constexpr bool operator==(const TVec4& a, const TVec4& b) noexcept = default;
};

using Vec2 = TVec2<f32>;
using Vec3 = TVec3<f32>;
using Vec4 = TVec4<f32>;
using DVec2 = TVec2<f64>;
using DVec3 = TVec3<f64>;
using DVec4 = TVec4<f64>;
using IVec2 = TVec2<i32>;
using IVec3 = TVec3<i32>;
using IVec4 = TVec4<i32>;
using UVec2 = TVec2<u32>;
using UVec3 = TVec3<u32>;
using UVec4 = TVec4<u32>;

static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12 && sizeof(Vec4) == 16);
static_assert(sizeof(DVec3) == 24 && sizeof(IVec3) == 12 && sizeof(UVec4) == 16);
static_assert(std::is_trivially_copyable_v<Vec3> && std::is_standard_layout_v<Vec3>);
static_assert(std::is_trivially_copyable_v<DVec4> && std::is_standard_layout_v<DVec4>);

/// World axis conventions: right-handed, +Y up, forward is -Z (cameras and objects look down -Z),
/// right is +X. See README.md.
inline constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kWorldForward{0.0f, 0.0f, -1.0f};
inline constexpr Vec3 kWorldRight{1.0f, 0.0f, 0.0f};

// ---------------------------------------------------------------------------------------------
// Explicit precision conversions (toF32 rounds to nearest; toF64 is exact).
// ---------------------------------------------------------------------------------------------
template <class T>
[[nodiscard]] constexpr TVec2<f32> toF32(const TVec2<T>& v) noexcept {
    return TVec2<f32>(v);
}
template <class T>
[[nodiscard]] constexpr TVec3<f32> toF32(const TVec3<T>& v) noexcept {
    return TVec3<f32>(v);
}
template <class T>
[[nodiscard]] constexpr TVec4<f32> toF32(const TVec4<T>& v) noexcept {
    return TVec4<f32>(v);
}
template <class T>
[[nodiscard]] constexpr TVec2<f64> toF64(const TVec2<T>& v) noexcept {
    return TVec2<f64>(v);
}
template <class T>
[[nodiscard]] constexpr TVec3<f64> toF64(const TVec3<T>& v) noexcept {
    return TVec3<f64>(v);
}
template <class T>
[[nodiscard]] constexpr TVec4<f64> toF64(const TVec4<T>& v) noexcept {
    return TVec4<f64>(v);
}

/// Generic swizzle: swizzle<2, 0>(v) == {v.z, v.x}; swizzle<0, 2, 1>(v) == {v.x, v.z, v.y}.
template <int A, int B, class V>
[[nodiscard]] constexpr TVec2<typename V::ValueType> swizzle(const V& v) noexcept {
    static_assert(A >= 0 && B >= 0 && A < V::kSize && B < V::kSize);
    return {v[A], v[B]};
}
template <int A, int B, int C, class V>
[[nodiscard]] constexpr TVec3<typename V::ValueType> swizzle(const V& v) noexcept {
    static_assert(A >= 0 && B >= 0 && C >= 0 && A < V::kSize && B < V::kSize && C < V::kSize);
    return {v[A], v[B], v[C]};
}
template <int A, int B, int C, int D, class V>
[[nodiscard]] constexpr TVec4<typename V::ValueType> swizzle(const V& v) noexcept {
    static_assert(A >= 0 && B >= 0 && C >= 0 && D >= 0);
    static_assert(A < V::kSize && B < V::kSize && C < V::kSize && D < V::kSize);
    return {v[A], v[B], v[C], v[D]};
}

// ---------------------------------------------------------------------------------------------
// Component-wise helpers (all sizes). Implemented once through a small applicator.
// ---------------------------------------------------------------------------------------------
namespace detail {
template <class T, class F>
constexpr TVec2<T> mapVec(const TVec2<T>& a, F f) {
    return {f(a.x), f(a.y)};
}
template <class T, class F>
constexpr TVec3<T> mapVec(const TVec3<T>& a, F f) {
    return {f(a.x), f(a.y), f(a.z)};
}
template <class T, class F>
constexpr TVec4<T> mapVec(const TVec4<T>& a, F f) {
    return {f(a.x), f(a.y), f(a.z), f(a.w)};
}
template <class T, class F>
constexpr TVec2<T> zipVec(const TVec2<T>& a, const TVec2<T>& b, F f) {
    return {f(a.x, b.x), f(a.y, b.y)};
}
template <class T, class F>
constexpr TVec3<T> zipVec(const TVec3<T>& a, const TVec3<T>& b, F f) {
    return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z)};
}
template <class T, class F>
constexpr TVec4<T> zipVec(const TVec4<T>& a, const TVec4<T>& b, F f) {
    return {f(a.x, b.x), f(a.y, b.y), f(a.z, b.z), f(a.w, b.w)};
}
template <class V>
struct IsVec : std::false_type {};
template <class T>
struct IsVec<TVec2<T>> : std::true_type {};
template <class T>
struct IsVec<TVec3<T>> : std::true_type {};
template <class T>
struct IsVec<TVec4<T>> : std::true_type {};
}  // namespace detail

/// Any of the TVec2/3/4 types.
template <class V>
concept VecType = detail::IsVec<V>::value;
/// A TVec with floating-point components.
template <class V>
concept FloatVecType = VecType<V> && std::floating_point<typename V::ValueType>;

/// Component-wise minimum.
template <VecType V>
[[nodiscard]] constexpr V min(const V& a, const V& b) noexcept {
    return detail::zipVec(a, b, [](auto p, auto q) { return helios::min(p, q); });
}
/// Component-wise maximum.
template <VecType V>
[[nodiscard]] constexpr V max(const V& a, const V& b) noexcept {
    return detail::zipVec(a, b, [](auto p, auto q) { return helios::max(p, q); });
}
/// Component-wise absolute value.
template <VecType V>
[[nodiscard]] constexpr V abs(const V& a) noexcept {
    return detail::mapVec(a, [](auto p) { return helios::abs(p); });
}
/// Component-wise sign (-1, 0, +1).
template <VecType V>
[[nodiscard]] constexpr V sign(const V& a) noexcept {
    return detail::mapVec(a, [](auto p) { return helios::sign(p); });
}
/// Component-wise clamp with vector bounds.
template <VecType V>
[[nodiscard]] constexpr V clamp(const V& v, const V& lo, const V& hi) noexcept {
    return min(max(v, lo), hi);
}
/// Component-wise clamp with scalar bounds.
template <VecType V>
[[nodiscard]] constexpr V clamp(const V& v, typename V::ValueType lo, typename V::ValueType hi) noexcept {
    return detail::mapVec(v, [lo, hi](auto p) { return helios::clamp(p, lo, hi); });
}
/// Component-wise clamp to [0, 1].
template <FloatVecType V>
[[nodiscard]] constexpr V saturate(const V& v) noexcept {
    using T = typename V::ValueType;
    return clamp(v, T(0), T(1));
}
/// Component-wise floor / ceil / round / trunc / fract.
template <FloatVecType V>
[[nodiscard]] inline V floor(const V& v) noexcept {
    return detail::mapVec(v, [](auto p) { return std::floor(p); });
}
template <FloatVecType V>
[[nodiscard]] inline V ceil(const V& v) noexcept {
    return detail::mapVec(v, [](auto p) { return std::ceil(p); });
}
/// Round half away from zero.
template <FloatVecType V>
[[nodiscard]] inline V round(const V& v) noexcept {
    return detail::mapVec(v, [](auto p) { return std::round(p); });
}
template <FloatVecType V>
[[nodiscard]] inline V trunc(const V& v) noexcept {
    return detail::mapVec(v, [](auto p) { return std::trunc(p); });
}
template <FloatVecType V>
[[nodiscard]] inline V fract(const V& v) noexcept {
    return detail::mapVec(v, [](auto p) { return helios::fract(p); });
}
/// Linear interpolation with a scalar parameter.
template <FloatVecType V>
[[nodiscard]] constexpr V lerp(const V& a, const V& b, typename V::ValueType t) noexcept {
    return a + (b - a) * t;
}
/// Linear interpolation with a per-component parameter.
template <FloatVecType V>
[[nodiscard]] constexpr V lerp(const V& a, const V& b, const V& t) noexcept {
    return a + (b - a) * t;
}
/// Component-wise smoothstep with scalar edges.
template <FloatVecType V>
[[nodiscard]] constexpr V smoothstep(typename V::ValueType e0, typename V::ValueType e1,
                                     const V& v) noexcept {
    return detail::mapVec(v, [e0, e1](auto p) { return helios::smoothstep(e0, e1, p); });
}
/// Component-wise step: 0 where v < edge, else 1.
template <FloatVecType V>
[[nodiscard]] constexpr V step(const V& edge, const V& v) noexcept {
    return detail::zipVec(edge, v, [](auto e, auto p) { return helios::step(e, p); });
}

/// Smallest / largest component and horizontal sum.
template <class T>
[[nodiscard]] constexpr T minComponent(const TVec2<T>& v) noexcept {
    return min(v.x, v.y);
}
template <class T>
[[nodiscard]] constexpr T minComponent(const TVec3<T>& v) noexcept {
    return min(min(v.x, v.y), v.z);
}
template <class T>
[[nodiscard]] constexpr T minComponent(const TVec4<T>& v) noexcept {
    return min(min(v.x, v.y), min(v.z, v.w));
}
template <class T>
[[nodiscard]] constexpr T maxComponent(const TVec2<T>& v) noexcept {
    return max(v.x, v.y);
}
template <class T>
[[nodiscard]] constexpr T maxComponent(const TVec3<T>& v) noexcept {
    return max(max(v.x, v.y), v.z);
}
template <class T>
[[nodiscard]] constexpr T maxComponent(const TVec4<T>& v) noexcept {
    return max(max(v.x, v.y), max(v.z, v.w));
}
/// Index (0..2) of the component with the largest magnitude; ties resolve to the lowest index.
template <class T>
[[nodiscard]] constexpr int maxAbsAxis(const TVec3<T>& v) noexcept {
    const T ax = abs(v.x), ay = abs(v.y), az = abs(v.z);
    if (ax >= ay && ax >= az) return 0;
    return ay >= az ? 1 : 2;
}
template <class T>
[[nodiscard]] constexpr T sum(const TVec2<T>& v) noexcept {
    return v.x + v.y;
}
template <class T>
[[nodiscard]] constexpr T sum(const TVec3<T>& v) noexcept {
    return v.x + v.y + v.z;
}
template <class T>
[[nodiscard]] constexpr T sum(const TVec4<T>& v) noexcept {
    return (v.x + v.y) + (v.z + v.w);
}

// ---------------------------------------------------------------------------------------------
// Geometric functions.
// ---------------------------------------------------------------------------------------------
/// Dot product.
template <class T>
[[nodiscard]] constexpr T dot(const TVec2<T>& a, const TVec2<T>& b) noexcept {
    return a.x * b.x + a.y * b.y;
}
template <class T>
[[nodiscard]] constexpr T dot(const TVec3<T>& a, const TVec3<T>& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
template <class T>
[[nodiscard]] constexpr T dot(const TVec4<T>& a, const TVec4<T>& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
/// Right-handed cross product.
template <class T>
[[nodiscard]] constexpr TVec3<T> cross(const TVec3<T>& a, const TVec3<T>& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
/// 2D cross product (z of the 3D cross product; positive when b is counter-clockwise from a).
template <class T>
[[nodiscard]] constexpr T cross(const TVec2<T>& a, const TVec2<T>& b) noexcept {
    return a.x * b.y - a.y * b.x;
}
/// Counter-clockwise perpendicular (-y, x).
template <class T>
[[nodiscard]] constexpr TVec2<T> perp(const TVec2<T>& v) noexcept {
    return {-v.y, v.x};
}

/// Squared length / length / (squared) distance.
template <VecType V>
[[nodiscard]] constexpr typename V::ValueType lengthSq(const V& v) noexcept {
    return dot(v, v);
}
template <FloatVecType V>
[[nodiscard]] inline typename V::ValueType length(const V& v) noexcept {
    return std::sqrt(dot(v, v));
}
template <VecType V>
[[nodiscard]] constexpr typename V::ValueType distanceSq(const V& a, const V& b) noexcept {
    return lengthSq(b - a);
}
template <FloatVecType V>
[[nodiscard]] inline typename V::ValueType distance(const V& a, const V& b) noexcept {
    return length(b - a);
}
/// Unit vector in the direction of v. Precondition: v is non-zero and finite.
template <FloatVecType V>
[[nodiscard]] inline V normalize(const V& v) noexcept {
    using T = typename V::ValueType;
    return v * (T(1) / length(v));
}
/// normalize(v), or `fallback` if v is (near) zero or not finite.
template <FloatVecType V>
[[nodiscard]] inline V normalizeOr(const V& v, const V& fallback) noexcept {
    const auto inv = safeRsqrt(lengthSq(v));
    return inv > typename V::ValueType(0) ? v * inv : fallback;
}
/// normalize(v), or zero if v is (near) zero or not finite.
template <FloatVecType V>
[[nodiscard]] inline V normalizeOrZero(const V& v) noexcept {
    return normalizeOr(v, V{});
}
/// True if |length(v) - 1| <= tol.
template <FloatVecType V>
[[nodiscard]] inline bool isNormalized(const V& v,
                                       typename V::ValueType tol = typename V::ValueType(1e-4)) noexcept {
    return abs(lengthSq(v) - typename V::ValueType(1)) <= typename V::ValueType(2) * tol;
}
/// Component-wise approxEqual with an absolute tolerance.
template <FloatVecType V>
[[nodiscard]] constexpr bool approxEqual(
    const V& a, const V& b, typename V::ValueType tol = kEpsilonT<typename V::ValueType>) noexcept {
    for (int i = 0; i < V::kSize; ++i) {
        if (!(abs(a[i] - b[i]) <= tol)) return false;
    }
    return true;
}
/// True if no component is infinite or NaN.
template <FloatVecType V>
[[nodiscard]] inline bool isFinite(const V& v) noexcept {
    for (int i = 0; i < V::kSize; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}
/// Reflects incident vector i about the plane with unit normal n (GLSL `reflect`). Named
/// reflectVector so `helios::reflect` stays free for the reflection module's namespace.
template <FloatVecType V>
[[nodiscard]] constexpr V reflectVector(const V& i, const V& n) noexcept {
    using T = typename V::ValueType;
    return i - n * (T(2) * dot(n, i));
}
/// Refracts unit incident vector i through a surface with unit normal n and index ratio eta
/// (n1/n2). Returns zero on total internal reflection (GLSL semantics).
template <FloatVecType V>
[[nodiscard]] inline V refract(const V& i, const V& n, typename V::ValueType eta) noexcept {
    using T = typename V::ValueType;
    const T ni = dot(n, i);
    const T k = T(1) - eta * eta * (T(1) - ni * ni);
    if (k < T(0)) return V{};
    return i * eta - n * (eta * ni + std::sqrt(k));
}
/// Projection of v onto `onto` (any length, non-zero).
template <FloatVecType V>
[[nodiscard]] constexpr V project(const V& v, const V& onto) noexcept {
    return onto * (dot(v, onto) / dot(onto, onto));
}
/// Component of v perpendicular to `onto`.
template <FloatVecType V>
[[nodiscard]] constexpr V reject(const V& v, const V& onto) noexcept {
    return v - project(v, onto);
}
/// Unsigned angle between two non-zero vectors in [0, pi]. Uses atan2(|a x b|, a.b), which stays
/// accurate for nearly parallel vectors where acos(dot) loses half the digits.
template <FloatingPoint T>
[[nodiscard]] inline T angleBetween(const TVec3<T>& a, const TVec3<T>& b) noexcept {
    return std::atan2(length(cross(a, b)), dot(a, b));
}
template <FloatingPoint T>
[[nodiscard]] inline T angleBetween(const TVec2<T>& a, const TVec2<T>& b) noexcept {
    return std::abs(std::atan2(cross(a, b), dot(a, b)));
}
/// Signed angle from a to b (counter-clockwise positive) in [-pi, pi].
template <FloatingPoint T>
[[nodiscard]] inline T signedAngle(const TVec2<T>& a, const TVec2<T>& b) noexcept {
    return std::atan2(cross(a, b), dot(a, b));
}
/// Rotates a 2D vector counter-clockwise by `angle` radians.
template <FloatingPoint T>
[[nodiscard]] inline TVec2<T> rotate(const TVec2<T>& v, T angle) noexcept {
    const T c = std::cos(angle), s = std::sin(angle);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}
/// Builds tangent t and bitangent b so that (t, b, n) is a right-handed orthonormal basis
/// (cross(t, b) == n). Singularity-free (Duff et al., JCGT 2017), but the basis flips where n.z
/// changes sign, so it is not continuous across the n.z == 0 great circle: do not use it to
/// build frames that must vary smoothly with n. Precondition: n is unit length.
template <FloatingPoint T>
constexpr void orthonormalBasis(const TVec3<T>& n, TVec3<T>& t, TVec3<T>& b) noexcept {
    const T s = n.z >= T(0) ? T(1) : T(-1);
    const T a = T(-1) / (s + n.z);
    const T c = n.x * n.y * a;
    t = {T(1) + s * n.x * n.x * a, s * c, -s * n.x};
    b = {c, s + n.y * n.y * a, -n.y};
}
/// Some unit vector perpendicular to unit vector n.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> anyPerpendicular(const TVec3<T>& n) noexcept {
    TVec3<T> t, b;
    orthonormalBasis(n, t, b);
    return t;
}

/// Component-wise floor to integers (fast, deterministic). Precondition as floorToI32.
template <FloatingPoint T>
[[nodiscard]] constexpr IVec2 floorToI32(const TVec2<T>& v) noexcept {
    return {floorToI32(v.x), floorToI32(v.y)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr IVec3 floorToI32(const TVec3<T>& v) noexcept {
    return {floorToI32(v.x), floorToI32(v.y), floorToI32(v.z)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr IVec4 floorToI32(const TVec4<T>& v) noexcept {
    return {floorToI32(v.x), floorToI32(v.y), floorToI32(v.z), floorToI32(v.w)};
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
