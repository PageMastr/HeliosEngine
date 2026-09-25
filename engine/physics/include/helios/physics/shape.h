// helios/physics/shape.h — collision shapes (02 §7.1 "Shapes are shared through an AssetId cache").
//
// A ShapeRef is a reference-counted, immutable shape. One shape may be used by any number of bodies in
// any number of grids (statics instanced into every bubble they overlap share one shape, 02 §5.4).
// Shapes are created in their local frame; capsules and cylinders are aligned with +Y.
//
// Threading: creating shapes and copying ShapeRefs is thread-safe (atomic reference counts). A
// PhysicsRuntime must be alive while any ShapeRef exists.
#pragma once

#include <span>

#include "helios/core/result.h"
#include "helios/physics/types.h"

namespace helios::physics {

class ShapeRef {
public:
    ShapeRef() noexcept = default;
    ShapeRef(const ShapeRef& other) noexcept;
    ShapeRef(ShapeRef&& other) noexcept;
    ShapeRef& operator=(const ShapeRef& other) noexcept;
    ShapeRef& operator=(ShapeRef&& other) noexcept;
    ~ShapeRef();

    bool valid() const noexcept { return m_shape != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
    /// Number of references (bodies and ShapeRefs) to the shape; for tests and leak checks.
    u32 useCount() const noexcept;
    /// Local-space bounding box half extents around the centre of mass.
    Vec3 localHalfExtents() const noexcept;

    /// The underlying JPH::Shape* (engine/physics internals only).
    void* native() const noexcept { return m_shape; }
    /// Takes a new reference to a JPH::Shape* (engine/physics internals only).
    static ShapeRef fromNative(void* shape) noexcept;

    friend bool operator==(const ShapeRef& a, const ShapeRef& b) noexcept { return a.m_shape == b.m_shape; }

private:
    void* m_shape = nullptr;
};

/// Box with half extents (all > convexRadius); convexRadius rounds the edges (Jolt's convex radius).
Result<ShapeRef> createBox(Vec3 halfExtents, f32 convexRadius = 0.05f);
Result<ShapeRef> createSphere(f32 radius);
/// Capsule along Y: two hemispheres of `radius` joined by a cylinder of half height `halfHeight`.
Result<ShapeRef> createCapsule(f32 halfHeight, f32 radius);
/// Cylinder along Y.
Result<ShapeRef> createCylinder(f32 halfHeight, f32 radius, f32 convexRadius = 0.05f);
/// Convex hull of at least four non-coplanar points.
Result<ShapeRef> createConvexHull(std::span<const Vec3> points);
/// Static triangle mesh (terrain tiles, placed statics); `indices` holds 3 per triangle.
Result<ShapeRef> createMesh(std::span<const Vec3> vertices, std::span<const u32> indices);

struct CompoundChild {
    ShapeRef shape;
    Vec3 position{};
    Quat rotation = Quat::identity();
};
/// Compound of convex or mesh children (ship hulls, placed prefabs).
Result<ShapeRef> createStaticCompound(std::span<const CompoundChild> children);

} // namespace helios::physics
