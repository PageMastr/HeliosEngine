// Collision shapes (helios/physics/shape.h).
#include "helios/physics/shape.h"

#include <format>

#include "helios/physics/runtime.h"
#include "jolt.h"

namespace helios::physics {

namespace {

JPH::Shape* asShape(void* p) { return static_cast<JPH::Shape*>(p); }

Result<ShapeRef> finish(const JPH::ShapeSettings::ShapeResult& result, std::string_view what) {
    if (result.HasError()) {
        return makeError(ErrorCode::InvalidArgument, "{}: {}", what, std::string_view(result.GetError().c_str()));
    }
    return ShapeRef::fromNative(result.Get().GetPtr());
}

Result<void> requireRuntime(std::string_view what) {
    if (!PhysicsRuntime::initialized()) {
        return makeError(ErrorCode::InvalidState, "{}: no PhysicsRuntime is alive", what);
    }
    return {};
}

bool finite(f32 v) { return v == v && v - v == 0.0f; }

} // namespace

ShapeRef::ShapeRef(const ShapeRef& other) noexcept : m_shape(other.m_shape) {
    if (m_shape) asShape(m_shape)->AddRef();
}

ShapeRef::ShapeRef(ShapeRef&& other) noexcept : m_shape(other.m_shape) { other.m_shape = nullptr; }

ShapeRef& ShapeRef::operator=(const ShapeRef& other) noexcept {
    if (this != &other) {
        if (other.m_shape) asShape(other.m_shape)->AddRef();
        if (m_shape) asShape(m_shape)->Release();
        m_shape = other.m_shape;
    }
    return *this;
}

ShapeRef& ShapeRef::operator=(ShapeRef&& other) noexcept {
    if (this != &other) {
        if (m_shape) asShape(m_shape)->Release();
        m_shape = other.m_shape;
        other.m_shape = nullptr;
    }
    return *this;
}

ShapeRef::~ShapeRef() {
    if (m_shape) asShape(m_shape)->Release();
}

u32 ShapeRef::useCount() const noexcept { return m_shape ? asShape(m_shape)->GetRefCount() : 0; }

Vec3 ShapeRef::localHalfExtents() const noexcept {
    if (!m_shape) return {};
    return jolt::fromJolt(asShape(m_shape)->GetLocalBounds().GetExtent());
}

ShapeRef ShapeRef::fromNative(void* shape) noexcept {
    ShapeRef r;
    r.m_shape = shape;
    if (shape) asShape(shape)->AddRef();
    return r;
}

Result<ShapeRef> createBox(Vec3 halfExtents, f32 convexRadius) {
    HELIOS_TRY(requireRuntime("createBox"));
    if (!(convexRadius >= 0.0f) || !(halfExtents.x > convexRadius) || !(halfExtents.y > convexRadius) ||
        !(halfExtents.z > convexRadius) || !finite(halfExtents.x) || !finite(halfExtents.y) || !finite(halfExtents.z)) {
        return Error{ErrorCode::InvalidArgument, "createBox: half extents must be finite and larger than the convex radius"};
    }
    JPH::BoxShapeSettings settings(jolt::toJolt(halfExtents), convexRadius);
    return finish(settings.Create(), "createBox");
}

Result<ShapeRef> createSphere(f32 radius) {
    HELIOS_TRY(requireRuntime("createSphere"));
    if (!(radius > 0.0f) || !finite(radius)) return Error{ErrorCode::InvalidArgument, "createSphere: radius must be > 0"};
    JPH::SphereShapeSettings settings(radius);
    return finish(settings.Create(), "createSphere");
}

Result<ShapeRef> createCapsule(f32 halfHeight, f32 radius) {
    HELIOS_TRY(requireRuntime("createCapsule"));
    if (!(radius > 0.0f) || !(halfHeight > 0.0f) || !finite(radius) || !finite(halfHeight)) {
        return Error{ErrorCode::InvalidArgument, "createCapsule: radius and half height must be > 0"};
    }
    JPH::CapsuleShapeSettings settings(halfHeight, radius);
    return finish(settings.Create(), "createCapsule");
}

Result<ShapeRef> createCylinder(f32 halfHeight, f32 radius, f32 convexRadius) {
    HELIOS_TRY(requireRuntime("createCylinder"));
    if (!(convexRadius >= 0.0f) || !(radius > convexRadius) || !(halfHeight > convexRadius) || !finite(radius) ||
        !finite(halfHeight)) {
        return Error{ErrorCode::InvalidArgument, "createCylinder: radius and half height must exceed the convex radius"};
    }
    JPH::CylinderShapeSettings settings(halfHeight, radius, convexRadius);
    return finish(settings.Create(), "createCylinder");
}

Result<ShapeRef> createConvexHull(std::span<const Vec3> points) {
    HELIOS_TRY(requireRuntime("createConvexHull"));
    if (points.size() < 4) return Error{ErrorCode::InvalidArgument, "createConvexHull: needs at least 4 points"};
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const Vec3& p : points) {
        if (!finite(p.x) || !finite(p.y) || !finite(p.z)) {
            return Error{ErrorCode::InvalidArgument, "createConvexHull: non-finite point"};
        }
        pts.push_back(jolt::toJolt(p));
    }
    JPH::ConvexHullShapeSettings settings(pts);
    return finish(settings.Create(), "createConvexHull");
}

Result<ShapeRef> createMesh(std::span<const Vec3> vertices, std::span<const u32> indices) {
    HELIOS_TRY(requireRuntime("createMesh"));
    if (indices.empty() || indices.size() % 3 != 0) {
        return Error{ErrorCode::InvalidArgument, "createMesh: indices must hold a positive multiple of 3 entries"};
    }
    JPH::VertexList verts;
    verts.reserve(vertices.size());
    for (const Vec3& v : vertices) {
        if (!finite(v.x) || !finite(v.y) || !finite(v.z)) return Error{ErrorCode::InvalidArgument, "createMesh: non-finite vertex"};
        verts.push_back(JPH::Float3(v.x, v.y, v.z));
    }
    JPH::IndexedTriangleList tris;
    tris.reserve(indices.size() / 3);
    for (usize i = 0; i < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() || indices[i + 2] >= vertices.size()) {
            return makeError(ErrorCode::OutOfRange, "createMesh: triangle {} refers to a missing vertex", i / 3);
        }
        tris.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    }
    JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
    return finish(settings.Create(), "createMesh");
}

Result<ShapeRef> createStaticCompound(std::span<const CompoundChild> children) {
    HELIOS_TRY(requireRuntime("createStaticCompound"));
    if (children.empty()) return Error{ErrorCode::InvalidArgument, "createStaticCompound: no children"};
    JPH::StaticCompoundShapeSettings settings;
    for (const CompoundChild& c : children) {
        if (!c.shape) return Error{ErrorCode::InvalidArgument, "createStaticCompound: child without a shape"};
        const Quat& q = c.rotation;
        const f64 len2 = static_cast<f64>(q.x) * q.x + static_cast<f64>(q.y) * q.y + static_cast<f64>(q.z) * q.z +
                         static_cast<f64>(q.w) * q.w;
        if (!finite(c.position.x) || !finite(c.position.y) || !finite(c.position.z) || !finite(q.x) || !finite(q.y) ||
            !finite(q.z) || !finite(q.w) || !(len2 > 1e-12)) {
            return Error{ErrorCode::InvalidArgument, "createStaticCompound: child pose must be finite with a non-zero rotation"};
        }
        settings.AddShape(jolt::toJolt(c.position), jolt::toJolt(c.rotation).Normalized(),
                          static_cast<const JPH::Shape*>(c.shape.native()));
    }
    return finish(settings.Create(), "createStaticCompound");
}

} // namespace helios::physics
