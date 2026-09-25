// Object/broadphase layers and the collision matrix (02 §7.1 "Layers").
#include "jolt.h"

namespace helios::physics {

std::string_view objectLayerName(ObjectLayer layer) noexcept {
    switch (layer) {
        case ObjectLayer::Static: return "Static";
        case ObjectLayer::Terrain: return "Terrain";
        case ObjectLayer::Dynamic: return "Dynamic";
        case ObjectLayer::Kinematic: return "Kinematic";
        case ObjectLayer::Character: return "Character";
        case ObjectLayer::Vehicle: return "Vehicle";
        case ObjectLayer::ShipHull: return "ShipHull";
        case ObjectLayer::Debris: return "Debris";
        case ObjectLayer::Projectile: return "Projectile";
        case ObjectLayer::Sensor: return "Sensor";
        case ObjectLayer::Interior: return "Interior";
        default: return "?";
    }
}

std::string_view broadPhaseLayerName(BroadPhaseLayer layer) noexcept {
    switch (layer) {
        case BroadPhaseLayer::Static: return "Static";
        case BroadPhaseLayer::Moving: return "Moving";
        case BroadPhaseLayer::Debris: return "Debris";
        case BroadPhaseLayer::Projectile: return "Projectile";
        case BroadPhaseLayer::Sensor: return "Sensor";
        default: return "?";
    }
}

BroadPhaseLayer broadPhaseLayerOf(ObjectLayer layer) noexcept {
    switch (layer) {
        case ObjectLayer::Static:
        case ObjectLayer::Terrain:
        case ObjectLayer::Interior: return BroadPhaseLayer::Static;
        case ObjectLayer::Debris: return BroadPhaseLayer::Debris;
        case ObjectLayer::Projectile: return BroadPhaseLayer::Projectile;
        case ObjectLayer::Sensor: return BroadPhaseLayer::Sensor;
        default: return BroadPhaseLayer::Moving;
    }
}

CollisionMatrix CollisionMatrix::defaults() noexcept {
    using L = ObjectLayer;
    CollisionMatrix m;
    const L statics[] = {L::Static, L::Terrain, L::Interior};
    const L movers[] = {L::Dynamic, L::Character, L::Vehicle, L::ShipHull, L::Debris, L::Projectile};
    for (L s : statics) {
        for (L mv : movers) m.set(s, mv, true);
    }
    const L solid[] = {L::Dynamic, L::Kinematic, L::Character, L::Vehicle, L::ShipHull};
    for (L a : solid) {
        for (L b : solid) m.set(a, b, true);
        m.set(a, L::Debris, true);
        m.set(a, L::Projectile, true);
        m.set(a, L::Sensor, true);
    }
    m.set(L::Kinematic, L::Kinematic, false); // kinematic bodies never respond to each other
    m.set(L::Debris, L::Character, false);    // debris never blocks a character
    m.set(L::Debris, L::Debris, false);
    m.set(L::Projectile, L::Projectile, false);
    m.set(L::Debris, L::Sensor, true);
    return m;
}

namespace jolt {

JPH::BroadPhaseLayer BroadPhaseLayers::GetBroadPhaseLayer(JPH::ObjectLayer layer) const {
    JPH_ASSERT(layer < kObjectLayerCount);
    return toJolt(broadPhaseLayerOf(static_cast<ObjectLayer>(layer)));
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* BroadPhaseLayers::GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const {
    return broadPhaseLayerName(static_cast<BroadPhaseLayer>(layer.GetValue())).data();
}
#endif

ObjectVsBroadPhase::ObjectVsBroadPhase(const CollisionMatrix& matrix) {
    for (u32 a = 0; a < kObjectLayerCount; ++a) {
        for (u32 b = 0; b < kObjectLayerCount; ++b) {
            if (matrix.collides(static_cast<ObjectLayer>(a), static_cast<ObjectLayer>(b))) {
                m_bpMask[a] |= 1u << static_cast<u32>(broadPhaseLayerOf(static_cast<ObjectLayer>(b)));
            }
        }
    }
}

bool ObjectVsBroadPhase::ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const {
    if (layer >= kObjectLayerCount) return false;
    return (m_bpMask[layer] >> bp.GetValue()) & 1u;
}

bool ObjectPairs::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const {
    if (a >= kObjectLayerCount || b >= kObjectLayerCount) return false;
    return m_matrix.collides(static_cast<ObjectLayer>(a), static_cast<ObjectLayer>(b));
}

} // namespace jolt
} // namespace helios::physics
