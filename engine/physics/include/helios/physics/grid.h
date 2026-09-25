// helios/physics/grid.h — a physics grid: one Jolt PhysicsSystem (02 §5.4, §7.1).
//
// A grid is a frame that owns one PhysicsSystem: a host grid (ship or station interior, EVA shell)
// or a bubble grid (open space, planet surface; ≈20 km half extent). Phase 0 runs one grid; the
// multi-grid PhysicsWorld, grid transfer and bubble merge/split come with `world` (Phase 1).
//
// Determinism (02 §7.1, RT-03):
//   * fixed step: step() advances exactly GridDesc::fixedDt with GridDesc::collisionSteps; advance()
//     turns real time into whole steps with an accumulator;
//   * bodies are added in (layer, key) order within every createBodies() batch;
//   * every order-dependent result (stateHash(), query tie-breaks, bodies()) is ordered by
//     (layer, key), never by Jolt's BodyID. Each body's user data holds its key, which is what the
//     `stable-order` Jolt patch (02 §7.1; not vendored yet, see engine/physics/README.md) needs to
//     take the BodyID out of the solver's own ordering as well;
//   * the result does not depend on the number of job-system workers.
//
// Threading: a grid is not thread-safe; one thread drives it (step() fans out onto the job system
// internally). Different grids may step in parallel on different threads.
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "helios/core/result.h"
#include "helios/physics/shape.h"
#include "helios/physics/types.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::physics {

enum class GridKind : u8 {
    Host,   ///< ship/station interior, EVA shell: pose set kinematically by the parent grid
    Bubble, ///< open space or planet surface: origin fixed in the parent frame
};

struct GridDesc {
    std::string name = "grid";
    GridKind kind = GridKind::Bubble;
    u32 maxBodies = 16384; ///< <= 2^23 - 1 (Jolt's BodyID index)
    u32 maxBodyPairs = 65536; ///< <= 2^24 (like maxContactConstraints)
    /// Jolt reserves ~480 bytes of temp memory per contact constraint every step, so this and
    /// tempAllocatorBytes are sized together (8192 constraints ~ 3.9 MB of the 8 MB default).
    u32 maxContactConstraints = 8192;
    /// Uniform gravity (m/s^2) for the grid; radial surface gravity arrives with `world`.
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    f32 fixedDt = 1.0f / 60.0f;
    u32 collisionSteps = 1; ///< 1..64
    /// advance() runs at most this many fixed steps per call and drops any further whole steps
    /// (logged), so a long hitch cannot snowball into a catch-up spiral.
    u32 maxStepsPerAdvance = 16;
    /// Temp allocator per grid (02 §7.1: 8 MB client, 32 MB cell; a small bubble uses its worker's 4 MB).
    /// A step that needs more falls back to the heap (logged once per grid) instead of aborting.
    usize tempAllocatorBytes = 8u << 20;
    /// Job system for the solver (High priority); null steps single-threaded on the caller. It must
    /// outlive the grid (the grid's destructor waits for the jobs it queued). step() returns only
    /// after every Helios job it queued has finished; it helps while waiting, so it may also run
    /// other queued Helios jobs.
    jobs::JobSystem* jobs = nullptr;
    CollisionMatrix collision = CollisionMatrix::defaults();
};

struct BodyDesc {
    BodyKey key = 0;
    ObjectLayer layer = ObjectLayer::Dynamic;
    MotionType motion = MotionType::Dynamic;
    ShapeRef shape;
    DVec3 position{};
    Quat rotation = Quat::identity();
    Vec3 linearVelocity{};
    Vec3 angularVelocity{};
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.0f;
    /// Mass in kg; 0 = from the shape's volume at 1000 kg/m^3.
    f32 mass = 0.0f;
    /// Speed cap in m/s. Jolt's own default (500 m/s) is below 06's NAV mode (1 km/s) and BENCH-2
    /// (1.5 km/s), so Helios defaults to 10 km/s.
    f32 maxLinearVelocity = 10'000.0f;
    bool allowSleeping = true;
    /// Sensor: reports overlaps, never collides.
    bool sensor = false;
};

/// A ray from `origin` to `origin + direction` (the length of `direction` is the range).
struct RayCast {
    DVec3 origin{};
    Vec3 direction{};
    ObjectLayerMask layers = ObjectLayerMask::all();
    BodyHandle ignore{}; ///< body excluded from the query (e.g. the caster)
};

struct RayHit {
    BodyHandle body{};
    ObjectLayer layer = ObjectLayer::Static;
    BodyKey key = 0;
    f32 fraction = 1.0f; ///< hit point = origin + fraction * direction
    u32 subShape = 0;    ///< Jolt SubShapeID value (a stable path through compounds and meshes)
    DVec3 point{};
    Vec3 normal{};       ///< surface normal at the hit (unit, facing the ray)
};

/// A shape swept from `position` along `direction`.
struct ShapeCast {
    ShapeRef shape;
    DVec3 position{};
    Quat rotation = Quat::identity();
    Vec3 direction{};
    ObjectLayerMask layers = ObjectLayerMask::all();
    BodyHandle ignore{};
};

struct ShapeHit {
    BodyHandle body{};
    ObjectLayer layer = ObjectLayer::Static;
    BodyKey key = 0;
    f32 fraction = 1.0f;
    u32 subShape = 0;
    DVec3 contactPoint{}; ///< on the hit body's surface
    Vec3 normal{};        ///< from the hit body towards the cast shape (unit)
    f32 penetration = 0.0f;
};

/// Character mover (02 §7.1 "Characters": CharacterVirtual; shared by client and cell).
struct CharacterDesc {
    BodyKey key = 0; ///< the character entity's EntityId
    f32 radius = 0.3f;
    f32 halfHeight = 0.6f; ///< of the capsule's cylinder part (total height 2 * (halfHeight + radius))
    DVec3 position{};      ///< capsule bottom
    Quat rotation = Quat::identity();
    Vec3 up{0.0f, 1.0f, 0.0f};
    f32 maxSlopeRadians = 0.87266f; ///< 50 degrees
    f32 mass = 80.0f;
    f32 maxStrength = 100.0f; ///< N pushed into dynamic bodies
    f32 stepHeight = 0.4f;    ///< ExtendedUpdate stair walking
    f32 stickToFloorDistance = 0.5f;
};

enum class GroundState : u8 { OnGround, OnSteepGround, NotSupported, InAir };

struct CharacterState {
    DVec3 position{};
    Quat rotation = Quat::identity();
    Vec3 linearVelocity{};
    GroundState ground = GroundState::InAir;
    BodyKey groundKey = 0; ///< key of the supporting body (0 if none)
};

class PhysicsGrid {
public:
    /// Creates a grid. Requires a live PhysicsRuntime.
    static Result<std::unique_ptr<PhysicsGrid>> create(const GridDesc& desc);
    ~PhysicsGrid();
    PhysicsGrid(const PhysicsGrid&) = delete;
    PhysicsGrid& operator=(const PhysicsGrid&) = delete;

    const GridDesc& desc() const noexcept;

    // -- Bodies ---------------------------------------------------------------------------------
    /// Adds one body. Fails for key 0, a duplicate (layer, key), a missing shape, a full grid, a
    /// motion type the layer does not allow (Static/Terrain/Interior bodies must be static), a mesh
    /// shape (or a compound holding one) on a moving body, a zero or non-finite rotation, and
    /// non-finite or negative mass, friction, restitution, damping or speed cap.
    Result<BodyHandle> createBody(const BodyDesc& desc);
    /// Adds a batch in (layer, key) order, whatever the order of `descs`; handles are returned in
    /// the order of `descs`. All-or-nothing: on error no body of the batch exists.
    Result<std::vector<BodyHandle>> createBodies(std::span<const BodyDesc> descs);
    /// Removes a body; stale or invalid handles are ignored.
    void destroyBody(BodyHandle body);
    bool contains(BodyHandle body) const noexcept;
    /// The body with (layer, key), or an invalid handle.
    BodyHandle findBody(ObjectLayer layer, BodyKey key) const noexcept;
    u32 bodyCount() const noexcept;
    /// Every body, ordered by (layer, key).
    std::vector<BodyHandle> bodies() const;

    Result<BodyState> bodyState(BodyHandle body) const;
    Result<ObjectLayer> bodyLayer(BodyHandle body) const;
    Result<BodyKey> bodyKey(BodyHandle body) const;
    Result<void> setPose(BodyHandle body, const DVec3& position, const Quat& rotation, bool activate = true);
    Result<void> setVelocity(BodyHandle body, const Vec3& linear, const Vec3& angular);
    Result<void> addImpulse(BodyHandle body, const Vec3& impulse);
    Result<void> addForce(BodyHandle body, const Vec3& force);
    /// Moves a kinematic body so that it reaches the pose after `dt` seconds (sets its velocities).
    Result<void> moveKinematic(BodyHandle body, const DVec3& targetPosition, const Quat& targetRotation, f32 dt);

    // -- Characters -----------------------------------------------------------------------------
    /// Adds a character. Its rotation is turned so that the capsule's local +Y lies on desc.up.
    /// Fails for key 0, a duplicate key and non-finite, zero or out-of-range parameters.
    Result<CharacterHandle> createCharacter(const CharacterDesc& desc);
    void destroyCharacter(CharacterHandle character);
    /// Moves the character by one ExtendedUpdate step of `dt` (stairs, floor sticking) with the grid's
    /// gravity along -up: horizontal velocity from `desiredVelocity`, vertical velocity kept (plus
    /// `jumpSpeed` along up when grounded and jumpSpeed > 0).
    Result<void> moveCharacter(CharacterHandle character, const Vec3& desiredVelocity, f32 jumpSpeed, f32 dt);
    Result<CharacterState> characterState(CharacterHandle character) const;
    /// Sets the character's up (radial on planets, grid gravity indoors) and turns the capsule about
    /// its feet by the shortest arc so that it stands along the new up.
    Result<void> setCharacterUp(CharacterHandle character, const Vec3& up);

    // -- Simulation -----------------------------------------------------------------------------
    /// One fixed step (desc().fixedDt, desc().collisionSteps). Fails if Jolt reports an overflow
    /// (body pair cache, manifold cache or contact constraints full).
    Result<void> step();
    /// Adds `seconds` of real time and runs the whole fixed steps it completes, at most
    /// desc().maxStepsPerAdvance (further whole steps are dropped and logged); returns the count.
    Result<u32> advance(f64 seconds);
    /// Fixed steps run so far.
    u64 stepCount() const noexcept;
    /// Rebuilds the broadphase trees (after adding many bodies at once).
    void optimizeBroadPhase();

    /// Diagnostics of the grid's step machinery.
    struct Stats {
        u32 jobsInFlight = 0;        ///< Jolt jobs holding a job-pool slot now (0 between steps)
        u32 jobPoolPeak = 0;         ///< most Jolt jobs ever alive at once (pool size 4096)
        u64 tempAllocatorFallbacks = 0; ///< temp allocations that did not fit and went to the heap
    };
    /// Current diagnostics (0 job counts without a job system). Same threading rule as step().
    Stats stats() const noexcept;

    // -- Queries (deterministic: ties on the fraction break by (layer, key, subShape)) ------------
    /// Closest hit, or none.
    std::optional<RayHit> castRay(const RayCast& ray) const;
    /// Every hit, sorted by (fraction, layer, key, subShape).
    std::vector<RayHit> castRayAll(const RayCast& ray) const;
    /// First hit of a swept shape, or none. The swept shape must be convex (or a compound of convex
    /// shapes): a mesh cannot be swept, and such a cast returns none.
    std::optional<ShapeHit> castShape(const ShapeCast& cast) const;

    // -- Determinism ------------------------------------------------------------------------------
    /// Hash of every body (layer, key, motion, pose, velocities, activity) in (layer, key) order and
    /// every character in key order. Bit-exact, so it is equal across hosts only when the simulation
    /// is (RT-03).
    u64 stateHash() const;

    struct Impl;

private:
    PhysicsGrid() = default;
    std::unique_ptr<Impl> m_impl;
};

} // namespace helios::physics
