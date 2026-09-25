// helios/physics/types.h — physics value types: object/broadphase layers, the collision matrix, body
// keys and handles, body states (02 §5.4, §7.1).
//
// No third-party type appears in engine/physics' public headers (02 §1.1): Jolt stays behind the
// module boundary, so the ECS/physics seam can change without touching callers.
//
// Threading: plain values; thread-safe.
#pragma once

#include <array>
#include <string_view>

#include "helios/core/handle.h"
#include "helios/core/types.h"
#include "helios/math/quat.h"
#include "helios/math/vec.h"

namespace helios::physics {

/// Object layers (02 §7.1). The collision matrix between them is a CollisionMatrix (later filled
/// from the PhysicsLayersDef record).
enum class ObjectLayer : u8 {
    Static = 0,
    Terrain,
    Dynamic,
    Kinematic,
    Character,
    Vehicle,
    ShipHull,
    Debris,
    Projectile,
    Sensor,
    Interior,
    Count
};
inline constexpr u32 kObjectLayerCount = static_cast<u32>(ObjectLayer::Count);

/// The five broadphase layers (02 §7.1): one tree each, so queries and pair finding skip whole
/// layers (never-moving geometry is not re-fitted with moving bodies).
enum class BroadPhaseLayer : u8 { Static = 0, Moving, Debris, Projectile, Sensor, Count };
inline constexpr u32 kBroadPhaseLayerCount = static_cast<u32>(BroadPhaseLayer::Count);

std::string_view objectLayerName(ObjectLayer layer) noexcept;
std::string_view broadPhaseLayerName(BroadPhaseLayer layer) noexcept;
/// Static, Terrain, Interior -> Static; Dynamic, Kinematic, Character, Vehicle, ShipHull -> Moving;
/// Debris, Projectile and Sensor have their own trees.
BroadPhaseLayer broadPhaseLayerOf(ObjectLayer layer) noexcept;

/// A set of object layers (queries).
struct ObjectLayerMask {
    u32 bits = (1u << kObjectLayerCount) - 1u;
    static constexpr ObjectLayerMask all() noexcept { return {}; }
    static constexpr ObjectLayerMask none() noexcept { return {0}; }
    constexpr ObjectLayerMask& add(ObjectLayer l) noexcept {
        bits |= 1u << static_cast<u32>(l);
        return *this;
    }
    constexpr ObjectLayerMask& remove(ObjectLayer l) noexcept {
        bits &= ~(1u << static_cast<u32>(l));
        return *this;
    }
    constexpr bool has(ObjectLayer l) const noexcept { return (bits >> static_cast<u32>(l)) & 1u; }
};

/// Which object layers collide with each other (symmetric).
class CollisionMatrix {
public:
    /// The Phase 0 default (02 §7.1): moving layers collide with statics and with each other, except
    /// kinematic-kinematic, debris-debris, debris-character, projectile-projectile and sensors-statics.
    static CollisionMatrix defaults() noexcept;
    /// Nothing collides.
    static constexpr CollisionMatrix none() noexcept { return CollisionMatrix(); }

    constexpr bool collides(ObjectLayer a, ObjectLayer b) const noexcept {
        return (m_rows[static_cast<u32>(a)] >> static_cast<u32>(b)) & 1u;
    }
    constexpr void set(ObjectLayer a, ObjectLayer b, bool collide) noexcept {
        const u32 ia = static_cast<u32>(a), ib = static_cast<u32>(b);
        if (collide) {
            m_rows[ia] |= 1u << ib;
            m_rows[ib] |= 1u << ia;
        } else {
            m_rows[ia] &= ~(1u << ib);
            m_rows[ib] &= ~(1u << ia);
        }
    }
    friend constexpr bool operator==(const CollisionMatrix&, const CollisionMatrix&) = default;

private:
    std::array<u32, kObjectLayerCount> m_rows{};
};

enum class MotionType : u8 { Static = 0, Kinematic, Dynamic };

/// A body's stable key (02 §7.1 "Stable body keys"): the EntityId for entity bodies, the packed
/// TileKey for Terrain tiles, the PCG instance key for generated statics. Never 0; (layer, key) is
/// unique within a grid. Every order-dependent result (the state hash, query tie-breaks) sorts by
/// (layer, key), never by Jolt's BodyID, which depends on each system's add/remove history.
using BodyKey = u64;

struct BodyTag;
struct CharacterTag;
/// A body in one PhysicsGrid (stale after destroyBody()).
using BodyHandle = Handle<BodyTag>;
/// A character mover in one PhysicsGrid.
using CharacterHandle = Handle<CharacterTag>;

/// Pose and velocities of a body. Positions are f64 grid-local metres (JPH_DOUBLE_PRECISION).
struct BodyState {
    DVec3 position{};
    Quat rotation = Quat::identity();
    Vec3 linearVelocity{};
    Vec3 angularVelocity{};
    MotionType motion = MotionType::Static;
    bool active = false; ///< awake (sleeping and static bodies are inactive)
};

} // namespace helios::physics
