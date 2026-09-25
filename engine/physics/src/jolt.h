// Internal: the Jolt headers engine/physics uses, and conversions between Helios and Jolt types.
// Only engine/physics/src includes this file (no Jolt type crosses the module boundary, 02 §1.1).
// Jolt's headers pick AVX paths inline from the JPH_USE_* defines tp_jolt exports, so every TU that
// includes this one is built with tp_jolt's ISA flags (02 §1.1 "ISA levels").
#pragma once

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "helios/math/quat.h"
#include "helios/math/vec.h"
#include "helios/physics/types.h"

#if !defined(JPH_DOUBLE_PRECISION) || !defined(JPH_CROSS_PLATFORM_DETERMINISTIC)
#error "engine/physics requires Jolt built with JPH_DOUBLE_PRECISION and JPH_CROSS_PLATFORM_DETERMINISTIC (ADR-013)"
#endif

namespace helios::physics::jolt {

inline JPH::Vec3 toJolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::RVec3 toJolt(const DVec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
inline JPH::Quat toJolt(const Quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline Vec3 fromJolt(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
inline DVec3 fromJoltR(JPH::RVec3Arg v) { return DVec3(v.GetX(), v.GetY(), v.GetZ()); }
inline Quat fromJolt(JPH::QuatArg q) { return Quat(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

inline JPH::ObjectLayer toJolt(ObjectLayer layer) { return static_cast<JPH::ObjectLayer>(layer); }
inline JPH::BroadPhaseLayer toJolt(BroadPhaseLayer layer) {
    return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
}

/// Broadphase layer mapping (02 §7.1: five broadphase layers).
class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return kBroadPhaseLayerCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override;
#endif
};

/// Object layer vs broadphase layer: true if the object layer collides with any object layer that
/// maps to the broadphase layer.
class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    explicit ObjectVsBroadPhase(const CollisionMatrix& matrix);
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override;

private:
    std::array<u32, kObjectLayerCount> m_bpMask{}; // per object layer: bit per broadphase layer
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectPairs(const CollisionMatrix& matrix) : m_matrix(matrix) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;

private:
    CollisionMatrix m_matrix;
};

} // namespace helios::physics::jolt
