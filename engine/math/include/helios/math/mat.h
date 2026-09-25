// helios/math/mat.h — 3x3 and 4x4 matrices (Mat3/Mat4 = f32, DMat3/DMat4 = f64).
//
// Conventions (see README.md):
//  * Column-major storage, column vectors: v' = M * v, and (A * B) * v == A * (B * v).
//    `m[c]` is column c; `m.at(row, col)` is an element. Memory layout matches GLSL/Slang
//    column-major matrices, so a Mat4 can be copied into a uniform/storage buffer unchanged.
//  * Right-handed, Y-up world; cameras look down -Z in view space.
//  * Projections target Vulkan/D3D clip space with depth in [0, 1] and REVERSE-Z (near plane at
//    depth 1, far plane / infinity at depth 0; use a GREATER depth test and clear depth to 0).
//  * Projections produce clip space with +Y up. The Vulkan backend renders with a negative
//    viewport height (core since Vulkan 1.1), which maps +Y to the top of the framebuffer and
//    keeps counter-clockwise front faces; the same matrices then also work unchanged for a future
//    D3D12 backend. Passes that rasterize without the flipped viewport (or reconstruct from raw
//    Vulkan NDC, where +Y points down) use flipClipY(). Frustum extraction is unaffected.
//
// Threading: plain value types; all functions are pure and thread-safe.
#pragma once

#include "helios/math/quat.h"

#if !defined(HELIOS_MATH_NO_SIMD) && \
    (defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define HELIOS_MATH_SSE2 1
#include <emmintrin.h>
#else
#define HELIOS_MATH_SSE2 0
#endif

// Keep <windows.h>-style min/max macros (when NOMINMAX is missing) out of this header.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

namespace helios {

template <FloatingPoint T>
struct TMat3 {
    using ValueType = T;
    using Column = TVec3<T>;

    Column cols[3] = {{T(1), T(0), T(0)}, {T(0), T(1), T(0)}, {T(0), T(0), T(1)}};

    /// Identity.
    constexpr TMat3() noexcept = default;
    constexpr TMat3(const Column& c0, const Column& c1, const Column& c2) noexcept : cols{c0, c1, c2} {}
    template <FloatingPoint U>
    constexpr explicit TMat3(const TMat3<U>& o) noexcept
        : cols{Column(o.cols[0]), Column(o.cols[1]), Column(o.cols[2])} {}

    [[nodiscard]] static constexpr TMat3 identity() noexcept { return {}; }
    [[nodiscard]] static constexpr TMat3 zero() noexcept { return {Column{}, Column{}, Column{}}; }
    /// Builds a matrix from rows (reads like the math on paper).
    [[nodiscard]] static constexpr TMat3 fromRows(const Column& r0, const Column& r1,
                                                  const Column& r2) noexcept {
        return {{r0.x, r1.x, r2.x}, {r0.y, r1.y, r2.y}, {r0.z, r1.z, r2.z}};
    }
    [[nodiscard]] static constexpr TMat3 scaling(const Column& s) noexcept {
        return {{s.x, T(0), T(0)}, {T(0), s.y, T(0)}, {T(0), T(0), s.z}};
    }

    [[nodiscard]] constexpr Column& operator[](int c) noexcept {
        HELIOS_MATH_ASSERT(c >= 0 && c < 3);
        return cols[c];
    }
    [[nodiscard]] constexpr const Column& operator[](int c) const noexcept {
        HELIOS_MATH_ASSERT(c >= 0 && c < 3);
        return cols[c];
    }
    [[nodiscard]] constexpr T& at(int row, int col) noexcept { return cols[col][row]; }
    [[nodiscard]] constexpr T at(int row, int col) const noexcept { return cols[col][row]; }
    [[nodiscard]] constexpr Column row(int r) const noexcept { return {cols[0][r], cols[1][r], cols[2][r]}; }
    [[nodiscard]] T* data() noexcept { return &cols[0].x; }
    [[nodiscard]] const T* data() const noexcept { return &cols[0].x; }

    friend constexpr Column operator*(const TMat3& m, const Column& v) noexcept {
        return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z;
    }
    friend constexpr TMat3 operator*(const TMat3& a, const TMat3& b) noexcept {
        return {a * b.cols[0], a * b.cols[1], a * b.cols[2]};
    }
    friend constexpr TMat3 operator*(const TMat3& a, T s) noexcept {
        return {a.cols[0] * s, a.cols[1] * s, a.cols[2] * s};
    }
    friend constexpr TMat3 operator+(const TMat3& a, const TMat3& b) noexcept {
        return {a.cols[0] + b.cols[0], a.cols[1] + b.cols[1], a.cols[2] + b.cols[2]};
    }
    friend constexpr bool operator==(const TMat3& a, const TMat3& b) noexcept {
        return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2];
    }
};

template <FloatingPoint T>
struct TMat4 {
    using ValueType = T;
    using Column = TVec4<T>;

    Column cols[4] = {{T(1), T(0), T(0), T(0)},
                      {T(0), T(1), T(0), T(0)},
                      {T(0), T(0), T(1), T(0)},
                      {T(0), T(0), T(0), T(1)}};

    /// Identity.
    constexpr TMat4() noexcept = default;
    constexpr TMat4(const Column& c0, const Column& c1, const Column& c2, const Column& c3) noexcept
        : cols{c0, c1, c2, c3} {}
    /// Embeds a 3x3 linear part and a translation.
    constexpr explicit TMat4(const TMat3<T>& m, const TVec3<T>& t = {}) noexcept
        : cols{Column(m.cols[0], T(0)), Column(m.cols[1], T(0)), Column(m.cols[2], T(0)), Column(t, T(1))} {}
    template <FloatingPoint U>
    constexpr explicit TMat4(const TMat4<U>& o) noexcept
        : cols{Column(o.cols[0]), Column(o.cols[1]), Column(o.cols[2]), Column(o.cols[3])} {}

    [[nodiscard]] static constexpr TMat4 identity() noexcept { return {}; }
    [[nodiscard]] static constexpr TMat4 zero() noexcept { return {Column{}, Column{}, Column{}, Column{}}; }
    [[nodiscard]] static constexpr TMat4 fromRows(const Column& r0, const Column& r1, const Column& r2,
                                                  const Column& r3) noexcept {
        return {{r0.x, r1.x, r2.x, r3.x},
                {r0.y, r1.y, r2.y, r3.y},
                {r0.z, r1.z, r2.z, r3.z},
                {r0.w, r1.w, r2.w, r3.w}};
    }

    // --- Affine builders -----------------------------------------------------------------------
    [[nodiscard]] static constexpr TMat4 translation(const TVec3<T>& t) noexcept {
        return TMat4(TMat3<T>{}, t);
    }
    [[nodiscard]] static constexpr TMat4 scaling(const TVec3<T>& s) noexcept {
        return TMat4(TMat3<T>::scaling(s));
    }
    /// Rotation of `angle` radians about the X axis (right-hand rule).
    [[nodiscard]] static TMat4 rotationX(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return TMat4(TMat3<T>{{T(1), T(0), T(0)}, {T(0), c, s}, {T(0), -s, c}});
    }
    [[nodiscard]] static TMat4 rotationY(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return TMat4(TMat3<T>{{c, T(0), -s}, {T(0), T(1), T(0)}, {s, T(0), c}});
    }
    [[nodiscard]] static TMat4 rotationZ(T angle) noexcept {
        const T c = std::cos(angle), s = std::sin(angle);
        return TMat4(TMat3<T>{{c, s, T(0)}, {-s, c, T(0)}, {T(0), T(0), T(1)}});
    }
    /// Rotation of `angle` radians about unit vector `axis`.
    [[nodiscard]] static TMat4 rotationAxis(const TVec3<T>& axis, T angle) noexcept;
    [[nodiscard]] static constexpr TMat4 rotation(const TQuat<T>& q) noexcept;
    /// Translation * Rotation * Scale (scale applied first).
    [[nodiscard]] static constexpr TMat4 trs(const TVec3<T>& t, const TQuat<T>& r,
                                             const TVec3<T>& s) noexcept;

    // --- Camera -------------------------------------------------------------------------------
    /// Right-handed view matrix (world -> view) for a camera at `eye` looking at `target`.
    /// View space: +X right, +Y up, camera looks down -Z. Precondition: target != eye and the
    /// view direction is not parallel to `up`.
    [[nodiscard]] static TMat4 lookAt(const TVec3<T>& eye, const TVec3<T>& target,
                                      const TVec3<T>& up) noexcept;
    /// Reverse-Z perspective with an INFINITE far plane: view z = -zNear maps to depth 1 and
    /// z -> -infinity maps to depth 0. fovY in radians, aspect = width / height.
    /// Depth = zNear / -z_view, so precision is ~uniform in log(z) with a D32_SFLOAT buffer.
    [[nodiscard]] static TMat4 perspectiveReverseZ(T fovY, T aspect, T zNear) noexcept;
    /// Reverse-Z perspective with a finite far plane: -zNear -> depth 1, -zFar -> depth 0.
    [[nodiscard]] static TMat4 perspectiveReverseZ(T fovY, T aspect, T zNear, T zFar) noexcept;
    /// Reverse-Z orthographic projection of the view-space box [l,r] x [b,t] x [-zNear, -zFar]:
    /// z = -zNear -> depth 1, z = -zFar -> depth 0.
    [[nodiscard]] static constexpr TMat4 orthographicReverseZ(T l, T r, T b, T t, T zNear, T zFar) noexcept;

    [[nodiscard]] constexpr Column& operator[](int c) noexcept {
        HELIOS_MATH_ASSERT(c >= 0 && c < 4);
        return cols[c];
    }
    [[nodiscard]] constexpr const Column& operator[](int c) const noexcept {
        HELIOS_MATH_ASSERT(c >= 0 && c < 4);
        return cols[c];
    }
    [[nodiscard]] constexpr T& at(int row, int col) noexcept { return cols[col][row]; }
    [[nodiscard]] constexpr T at(int row, int col) const noexcept { return cols[col][row]; }
    [[nodiscard]] constexpr Column row(int r) const noexcept {
        return {cols[0][r], cols[1][r], cols[2][r], cols[3][r]};
    }
    [[nodiscard]] T* data() noexcept { return &cols[0].x; }
    [[nodiscard]] const T* data() const noexcept { return &cols[0].x; }
    /// Upper-left 3x3 (linear part).
    [[nodiscard]] constexpr TMat3<T> linear() const noexcept {
        return {cols[0].xyz(), cols[1].xyz(), cols[2].xyz()};
    }
    [[nodiscard]] constexpr TVec3<T> translationPart() const noexcept { return cols[3].xyz(); }

    friend constexpr Column operator*(const TMat4& m, const Column& v) noexcept {
        // Linear combination of columns; the evaluation order matches the SSE path exactly.
        return ((m.cols[0] * v.x + m.cols[1] * v.y) + m.cols[2] * v.z) + m.cols[3] * v.w;
    }
    friend constexpr TMat4 operator*(const TMat4& a, T s) noexcept {
        return {a.cols[0] * s, a.cols[1] * s, a.cols[2] * s, a.cols[3] * s};
    }
    friend constexpr TMat4 operator+(const TMat4& a, const TMat4& b) noexcept {
        return {a.cols[0] + b.cols[0], a.cols[1] + b.cols[1], a.cols[2] + b.cols[2], a.cols[3] + b.cols[3]};
    }
    friend constexpr bool operator==(const TMat4& a, const TMat4& b) noexcept {
        return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2] &&
               a.cols[3] == b.cols[3];
    }
};

using Mat3 = TMat3<f32>;
using Mat4 = TMat4<f32>;
using DMat3 = TMat3<f64>;
using DMat4 = TMat4<f64>;
static_assert(sizeof(Mat3) == 36 && sizeof(Mat4) == 64 && sizeof(DMat4) == 128);
static_assert(std::is_trivially_copyable_v<Mat4> && std::is_standard_layout_v<Mat4>);

template <FloatingPoint T>
[[nodiscard]] constexpr TMat3<f32> toF32(const TMat3<T>& m) noexcept {
    return TMat3<f32>(m);
}
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<f32> toF32(const TMat4<T>& m) noexcept {
    return TMat4<f32>(m);
}
template <FloatingPoint T>
[[nodiscard]] constexpr TMat3<f64> toF64(const TMat3<T>& m) noexcept {
    return TMat3<f64>(m);
}
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<f64> toF64(const TMat4<T>& m) noexcept {
    return TMat4<f64>(m);
}

// ---------------------------------------------------------------------------------------------
// Multiplication. The scalar path is the reference; the SSE2 path (f32 Mat4 only) performs the
// same IEEE operations in the same order, so both give bit-identical results (tested).
// ---------------------------------------------------------------------------------------------
namespace detail {
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> mulMat4Scalar(const TMat4<T>& a, const TMat4<T>& b) noexcept {
    return {a * b.cols[0], a * b.cols[1], a * b.cols[2], a * b.cols[3]};
}
#if HELIOS_MATH_SSE2
[[nodiscard]] inline Mat4 mulMat4Sse(const Mat4& a, const Mat4& b) noexcept {
    const __m128 a0 = _mm_loadu_ps(a.cols[0].data());
    const __m128 a1 = _mm_loadu_ps(a.cols[1].data());
    const __m128 a2 = _mm_loadu_ps(a.cols[2].data());
    const __m128 a3 = _mm_loadu_ps(a.cols[3].data());
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        const TVec4<f32>& bc = b.cols[c];
        __m128 v = _mm_add_ps(_mm_mul_ps(a0, _mm_set1_ps(bc.x)), _mm_mul_ps(a1, _mm_set1_ps(bc.y)));
        v = _mm_add_ps(v, _mm_mul_ps(a2, _mm_set1_ps(bc.z)));
        v = _mm_add_ps(v, _mm_mul_ps(a3, _mm_set1_ps(bc.w)));
        _mm_storeu_ps(r.cols[c].data(), v);
    }
    return r;
}
#endif
}  // namespace detail

template <FloatingPoint T>
[[nodiscard]] inline TMat4<T> operator*(const TMat4<T>& a, const TMat4<T>& b) noexcept {
#if HELIOS_MATH_SSE2
    if constexpr (std::is_same_v<T, f32>) {
        return detail::mulMat4Sse(a, b);
    } else {
        return detail::mulMat4Scalar(a, b);
    }
#else
    return detail::mulMat4Scalar(a, b);
#endif
}

// ---------------------------------------------------------------------------------------------
// Transpose, determinant, inverse.
// ---------------------------------------------------------------------------------------------
/// Transpose.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat3<T> transpose(const TMat3<T>& m) noexcept {
    return {m.row(0), m.row(1), m.row(2)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> transpose(const TMat4<T>& m) noexcept {
    return {m.row(0), m.row(1), m.row(2), m.row(3)};
}
/// Determinant (negative for mirroring transforms).
template <FloatingPoint T>
[[nodiscard]] constexpr T determinant(const TMat3<T>& m) noexcept {
    return dot(m.cols[0], cross(m.cols[1], m.cols[2]));
}
template <FloatingPoint T>
[[nodiscard]] constexpr T determinant(const TMat4<T>& m) noexcept {
    const T a00 = m.at(0, 0), a01 = m.at(0, 1), a02 = m.at(0, 2), a03 = m.at(0, 3);
    const T a10 = m.at(1, 0), a11 = m.at(1, 1), a12 = m.at(1, 2), a13 = m.at(1, 3);
    const T a20 = m.at(2, 0), a21 = m.at(2, 1), a22 = m.at(2, 2), a23 = m.at(2, 3);
    const T a30 = m.at(3, 0), a31 = m.at(3, 1), a32 = m.at(3, 2), a33 = m.at(3, 3);
    const T s0 = a00 * a11 - a10 * a01, s1 = a00 * a12 - a10 * a02, s2 = a00 * a13 - a10 * a03;
    const T s3 = a01 * a12 - a11 * a02, s4 = a01 * a13 - a11 * a03, s5 = a02 * a13 - a12 * a03;
    const T c5 = a22 * a33 - a32 * a23, c4 = a21 * a33 - a31 * a23, c3 = a21 * a32 - a31 * a22;
    const T c2 = a20 * a33 - a30 * a23, c1 = a20 * a32 - a30 * a22, c0 = a20 * a31 - a30 * a21;
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}
/// Inverse of a 3x3 matrix. Returns false (and leaves `out` untouched) if |det| <= `eps`.
template <FloatingPoint T>
[[nodiscard]] constexpr bool tryInverse(const TMat3<T>& m, TMat3<T>& out, T eps = T(0)) noexcept {
    const TVec3<T> r0 = cross(m.cols[1], m.cols[2]);
    const TVec3<T> r1 = cross(m.cols[2], m.cols[0]);
    const TVec3<T> r2 = cross(m.cols[0], m.cols[1]);
    const T det = dot(m.cols[0], r0);
    if (!(abs(det) > eps)) return false;
    const T inv = T(1) / det;
    out = TMat3<T>::fromRows(r0 * inv, r1 * inv, r2 * inv);
    return true;
}
/// Inverse of a 3x3 matrix. Precondition: non-singular.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat3<T> inverse(const TMat3<T>& m) noexcept {
    TMat3<T> r = TMat3<T>::zero();
    [[maybe_unused]] const bool ok = tryInverse(m, r);
    HELIOS_MATH_ASSERT(ok);
    return r;
}
/// General 4x4 inverse (cofactor expansion). Returns false if |det| <= `eps`.
template <FloatingPoint T>
[[nodiscard]] constexpr bool tryInverse(const TMat4<T>& m, TMat4<T>& out, T eps = T(0)) noexcept {
    const T a00 = m.at(0, 0), a01 = m.at(0, 1), a02 = m.at(0, 2), a03 = m.at(0, 3);
    const T a10 = m.at(1, 0), a11 = m.at(1, 1), a12 = m.at(1, 2), a13 = m.at(1, 3);
    const T a20 = m.at(2, 0), a21 = m.at(2, 1), a22 = m.at(2, 2), a23 = m.at(2, 3);
    const T a30 = m.at(3, 0), a31 = m.at(3, 1), a32 = m.at(3, 2), a33 = m.at(3, 3);
    const T s0 = a00 * a11 - a10 * a01, s1 = a00 * a12 - a10 * a02, s2 = a00 * a13 - a10 * a03;
    const T s3 = a01 * a12 - a11 * a02, s4 = a01 * a13 - a11 * a03, s5 = a02 * a13 - a12 * a03;
    const T c5 = a22 * a33 - a32 * a23, c4 = a21 * a33 - a31 * a23, c3 = a21 * a32 - a31 * a22;
    const T c2 = a20 * a33 - a30 * a23, c1 = a20 * a32 - a30 * a22, c0 = a20 * a31 - a30 * a21;
    const T det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (!(abs(det) > eps)) return false;
    const T inv = T(1) / det;
    const TVec4<T> r0{(a11 * c5 - a12 * c4 + a13 * c3), (-a01 * c5 + a02 * c4 - a03 * c3),
                      (a31 * s5 - a32 * s4 + a33 * s3), (-a21 * s5 + a22 * s4 - a23 * s3)};
    const TVec4<T> r1{(-a10 * c5 + a12 * c2 - a13 * c1), (a00 * c5 - a02 * c2 + a03 * c1),
                      (-a30 * s5 + a32 * s2 - a33 * s1), (a20 * s5 - a22 * s2 + a23 * s1)};
    const TVec4<T> r2{(a10 * c4 - a11 * c2 + a13 * c0), (-a00 * c4 + a01 * c2 - a03 * c0),
                      (a30 * s4 - a31 * s2 + a33 * s0), (-a20 * s4 + a21 * s2 - a23 * s0)};
    const TVec4<T> r3{(-a10 * c3 + a11 * c1 - a12 * c0), (a00 * c3 - a01 * c1 + a02 * c0),
                      (-a30 * s3 + a31 * s1 - a32 * s0), (a20 * s3 - a21 * s1 + a22 * s0)};
    out = TMat4<T>::fromRows(r0 * inv, r1 * inv, r2 * inv, r3 * inv);
    return true;
}
/// General 4x4 inverse. Precondition: non-singular.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> inverse(const TMat4<T>& m) noexcept {
    TMat4<T> r = TMat4<T>::zero();
    [[maybe_unused]] const bool ok = tryInverse(m, r);
    HELIOS_MATH_ASSERT(ok);
    return r;
}
/// Fast inverse for affine matrices (last row 0,0,0,1): inverts the 3x3 part and the translation.
/// Handles non-uniform scale and shear. Precondition: m is affine and non-singular.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> inverseAffine(const TMat4<T>& m) noexcept {
    const TMat3<T> li = inverse(m.linear());
    return TMat4<T>(li, -(li * m.translationPart()));
}
/// Fastest inverse for rigid transforms (orthonormal rotation + translation, no scale).
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> inverseRigid(const TMat4<T>& m) noexcept {
    const TMat3<T> rt = transpose(m.linear());
    return TMat4<T>(rt, -(rt * m.translationPart()));
}

// ---------------------------------------------------------------------------------------------
// Point/vector transformation.
// ---------------------------------------------------------------------------------------------
/// Transforms a point (w = 1) by an affine matrix (no perspective divide).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> transformPoint(const TMat4<T>& m, const TVec3<T>& p) noexcept {
    return (m * TVec4<T>(p, T(1))).xyz();
}
/// Transforms a direction/vector (w = 0): linear part only.
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> transformVector(const TMat4<T>& m, const TVec3<T>& v) noexcept {
    return (m * TVec4<T>(v, T(0))).xyz();
}
/// Transforms a point and divides by w (for projection matrices).
template <FloatingPoint T>
[[nodiscard]] constexpr TVec3<T> transformPointProjective(const TMat4<T>& m, const TVec3<T>& p) noexcept {
    const TVec4<T> h = m * TVec4<T>(p, T(1));
    return h.xyz() / h.w;
}
/// Transforms a surface normal by the inverse-transpose of the linear part (renormalized).
template <FloatingPoint T>
[[nodiscard]] inline TVec3<T> transformNormal(const TMat4<T>& m, const TVec3<T>& n) noexcept {
    return normalize(transpose(inverse(m.linear())) * n);
}

// ---------------------------------------------------------------------------------------------
// Quaternion <-> matrix.
// ---------------------------------------------------------------------------------------------
/// Rotation matrix of a unit quaternion.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat3<T> toMat3(const TQuat<T>& q) noexcept {
    return {axisX(q), axisY(q), axisZ(q)};
}
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> toMat4(const TQuat<T>& q) noexcept {
    return TMat4<T>(toMat3(q));
}
/// Quaternion of a pure rotation matrix (orthonormal, det = +1). Columns are renormalized
/// first, so small drift is tolerated.
template <FloatingPoint T>
[[nodiscard]] inline TQuat<T> toQuat(const TMat3<T>& m) noexcept {
    return normalize(TQuat<T>::fromBasis(normalize(m.cols[0]), normalize(m.cols[1]), normalize(m.cols[2])));
}

template <FloatingPoint T>
constexpr TMat4<T> TMat4<T>::rotation(const TQuat<T>& q) noexcept {
    return toMat4(q);
}
template <FloatingPoint T>
TMat4<T> TMat4<T>::rotationAxis(const TVec3<T>& axis, T angle) noexcept {
    return toMat4(TQuat<T>::fromAxisAngle(axis, angle));
}
template <FloatingPoint T>
constexpr TMat4<T> TMat4<T>::trs(const TVec3<T>& t, const TQuat<T>& r, const TVec3<T>& s) noexcept {
    const TMat3<T> rm = toMat3(r);
    return TMat4<T>(TMat3<T>{rm.cols[0] * s.x, rm.cols[1] * s.y, rm.cols[2] * s.z}, t);
}

/// Splits an affine matrix into translation, rotation and (signed) scale such that
/// m == trs(t, r, s). Mirroring (negative determinant) is folded into -scale.x. Shear cannot be
/// represented and is discarded. Returns false if any axis has (near) zero scale.
template <FloatingPoint T>
[[nodiscard]] inline bool decomposeTRS(const TMat4<T>& m, TVec3<T>& t, TQuat<T>& r, TVec3<T>& s) noexcept {
    t = m.translationPart();
    TVec3<T> c0 = m.cols[0].xyz(), c1 = m.cols[1].xyz(), c2 = m.cols[2].xyz();
    s = {length(c0), length(c1), length(c2)};
    const T tiny = std::numeric_limits<T>::min() * T(16);
    if (s.x <= tiny || s.y <= tiny || s.z <= tiny) return false;
    if (dot(c0, cross(c1, c2)) < T(0)) s.x = -s.x;
    c0 /= s.x;
    c1 /= s.y;
    c2 /= s.z;
    // Gram-Schmidt removes shear/rounding so the rotation is exactly orthonormal.
    c0 = normalize(c0);
    c1 = normalize(c1 - c0 * dot(c0, c1));
    c2 = cross(c0, c1);
    r = normalize(TQuat<T>::fromBasis(c0, c1, c2));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Camera builders.
// ---------------------------------------------------------------------------------------------
template <FloatingPoint T>
TMat4<T> TMat4<T>::lookAt(const TVec3<T>& eye, const TVec3<T>& target, const TVec3<T>& up) noexcept {
    const TVec3<T> f = normalize(target - eye);
    const TVec3<T> s = normalize(cross(f, up));
    const TVec3<T> u = cross(s, f);
    return {{s.x, u.x, -f.x, T(0)},
            {s.y, u.y, -f.y, T(0)},
            {s.z, u.z, -f.z, T(0)},
            {-dot(s, eye), -dot(u, eye), dot(f, eye), T(1)}};
}
template <FloatingPoint T>
TMat4<T> TMat4<T>::perspectiveReverseZ(T fovY, T aspect, T zNear) noexcept {
    HELIOS_MATH_ASSERT(fovY > T(0) && aspect > T(0) && zNear > T(0));
    const T f = T(1) / std::tan(fovY * T(0.5));
    // clip = (f/aspect * x, f * y, zNear, -z)  =>  depth = zNear / -z.
    return {{f / aspect, T(0), T(0), T(0)},
            {T(0), f, T(0), T(0)},
            {T(0), T(0), T(0), T(-1)},
            {T(0), T(0), zNear, T(0)}};
}
template <FloatingPoint T>
TMat4<T> TMat4<T>::perspectiveReverseZ(T fovY, T aspect, T zNear, T zFar) noexcept {
    HELIOS_MATH_ASSERT(fovY > T(0) && aspect > T(0) && zNear > T(0) && zFar > zNear);
    const T f = T(1) / std::tan(fovY * T(0.5));
    const T range = zFar - zNear;
    // clip.z = zNear/(zFar-zNear) * z + zNear*zFar/(zFar-zNear), clip.w = -z.
    return {{f / aspect, T(0), T(0), T(0)},
            {T(0), f, T(0), T(0)},
            {T(0), T(0), zNear / range, T(-1)},
            {T(0), T(0), zNear * zFar / range, T(0)}};
}
template <FloatingPoint T>
constexpr TMat4<T> TMat4<T>::orthographicReverseZ(T l, T r, T b, T t, T zNear, T zFar) noexcept {
    const T rl = T(1) / (r - l), tb = T(1) / (t - b), fn = T(1) / (zFar - zNear);
    return {{T(2) * rl, T(0), T(0), T(0)},
            {T(0), T(2) * tb, T(0), T(0)},
            {T(0), T(0), fn, T(0)},
            {-(r + l) * rl, -(t + b) * tb, zFar * fn, T(1)}};
}
/// Negates clip-space Y (pre-multiplies by diag(1, -1, 1, 1)) for passes that target raw Vulkan
/// NDC (+Y down) without the negative-viewport convention. Also flips triangle winding.
template <FloatingPoint T>
[[nodiscard]] constexpr TMat4<T> flipClipY(const TMat4<T>& proj) noexcept {
    TMat4<T> r = proj;
    for (int c = 0; c < 4; ++c) r.cols[c].y = -r.cols[c].y;
    return r;
}
/// Linear view distance (positive, metres) from a reverse-Z depth value for the infinite
/// projection: d = zNear / depth. Depth 0 (the far plane at infinity) returns +inf.
template <FloatingPoint T>
[[nodiscard]] constexpr T linearDepthFromReverseZ(T depth, T zNear) noexcept {
    return depth > T(0) ? zNear / depth : std::numeric_limits<T>::infinity();
}

/// Component-wise approximate comparison with an absolute tolerance.
template <FloatingPoint T>
[[nodiscard]] constexpr bool approxEqual(const TMat4<T>& a, const TMat4<T>& b, T tol) noexcept {
    for (int c = 0; c < 4; ++c) {
        if (!approxEqual(a.cols[c], b.cols[c], tol)) return false;
    }
    return true;
}
template <FloatingPoint T>
[[nodiscard]] constexpr bool approxEqual(const TMat3<T>& a, const TMat3<T>& b, T tol) noexcept {
    for (int c = 0; c < 3; ++c) {
        if (!approxEqual(a.cols[c], b.cols[c], tol)) return false;
    }
    return true;
}

}  // namespace helios

#pragma pop_macro("max")
#pragma pop_macro("min")
