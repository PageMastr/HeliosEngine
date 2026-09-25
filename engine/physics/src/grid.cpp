// PhysicsGrid: one Jolt PhysicsSystem with Helios handles, stable keys and deterministic queries
// (helios/physics/grid.h).
#include "helios/physics/grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <numeric>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/handle.h"
#include "helios/core/jobs.h"
#include "helios/physics/runtime.h"
#include "job_system.h"
#include "jolt.h"
#include "log_channel.h"
#include "temp_allocator.h"

namespace helios::physics {

namespace {

using jolt::fromJolt;
using jolt::fromJoltR;
using jolt::toJolt;

struct BodySlot {
    JPH::BodyID id;
    ObjectLayer layer = ObjectLayer::Static;
    BodyKey key = 0;
};

struct CharacterSlot {
    JPH::Ref<JPH::CharacterVirtual> character;
    CharacterDesc desc;
};

using LayerKey = std::pair<u8, BodyKey>;

bool isStaticLayer(ObjectLayer l) {
    return l == ObjectLayer::Static || l == ObjectLayer::Terrain || l == ObjectLayer::Interior;
}

JPH::EMotionType toJolt(MotionType m) {
    switch (m) {
        case MotionType::Kinematic: return JPH::EMotionType::Kinematic;
        case MotionType::Dynamic: return JPH::EMotionType::Dynamic;
        default: return JPH::EMotionType::Static;
    }
}

MotionType fromJolt(JPH::EMotionType m) {
    switch (m) {
        case JPH::EMotionType::Kinematic: return MotionType::Kinematic;
        case JPH::EMotionType::Dynamic: return MotionType::Dynamic;
        default: return MotionType::Static;
    }
}

bool finite(f64 v) { return v == v && v - v == 0.0; }
bool finite(const DVec3& v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool finite(const Vec3& v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool finite(const Quat& q) { return finite(q.x) && finite(q.y) && finite(q.z) && finite(q.w); }
/// A rotation the grid can normalize: finite and not (near) zero. A zero quaternion passes finite()
/// but Normalized() turns it into NaN, which then spreads through the broadphase and the solver.
bool validRotation(const Quat& q) {
    return finite(q) && static_cast<f64>(q.x) * q.x + static_cast<f64>(q.y) * q.y + static_cast<f64>(q.z) * q.z +
                                static_cast<f64>(q.w) * q.w > 1e-12;
}
bool finiteNonNegative(f32 v) { return finite(v) && v >= 0.0f; }
/// Mesh, height-field and plane shapes (and compounds holding one) have no mass properties and no
/// swept-cast support in Jolt: they may only be static bodies and never the shape of a castShape().
bool mustBeStatic(const ShapeRef& shape) { return static_cast<const JPH::Shape*>(shape.native())->MustBeStatic(); }

// -- Query filters ---------------------------------------------------------------------------------

class MaskBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
    explicit MaskBroadPhaseFilter(ObjectLayerMask mask) {
        for (u32 l = 0; l < kObjectLayerCount; ++l) {
            if (mask.has(static_cast<ObjectLayer>(l))) m_bits |= 1u << static_cast<u32>(broadPhaseLayerOf(static_cast<ObjectLayer>(l)));
        }
    }
    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override { return (m_bits >> layer.GetValue()) & 1u; }

private:
    u32 m_bits = 0;
};

class MaskObjectLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit MaskObjectLayerFilter(ObjectLayerMask mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer < kObjectLayerCount && m_mask.has(static_cast<ObjectLayer>(layer));
    }

private:
    ObjectLayerMask m_mask;
};

/// A hit candidate ordered by (fraction, layer, key, subShape): no BodyID involved (02 §5.4/§7.1).
struct Candidate {
    f32 fraction = 0.0f;
    u8 layer = 0;
    BodyKey key = 0;
    u32 subShape = 0;
    JPH::BodyID body;
    bool operator<(const Candidate& o) const {
        if (fraction != o.fraction) return fraction < o.fraction;
        if (layer != o.layer) return layer < o.layer;
        if (key != o.key) return key < o.key;
        return subShape < o.subShape;
    }
};

JPH::BodyID bodyOf(const JPH::RayCastResult& r) { return r.mBodyID; }
JPH::BodyID bodyOf(const JPH::ShapeCastResult& r) { return r.mBodyID2; }

/// Keeps the smallest candidate. The early-out fraction stays one ulp above the best fraction so hits
/// at exactly the best fraction still arrive and are tie-broken by key, not by traversal order.
template <class Base, class Result>
class ClosestCollector final : public Base {
public:
    void OnBody(const JPH::Body& body) override {
        m_layer = static_cast<u8>(body.GetObjectLayer());
        m_key = body.GetUserData();
    }
    void AddHit(const Result& hit) override {
        Candidate c{hit.mFraction, m_layer, m_key, hit.mSubShapeID2.GetValue(), bodyOf(hit)};
        if (!m_has || c < m_best) {
            m_best = c;
            m_bestResult = hit;
            m_has = true;
        }
        const f32 limit = std::nextafter(m_best.fraction, 2.0f);
        if (limit < this->GetEarlyOutFraction()) this->UpdateEarlyOutFraction(limit);
    }
    bool has() const { return m_has; }
    const Candidate& best() const { return m_best; }
    const Result& bestResult() const { return m_bestResult; }

private:
    u8 m_layer = 0;
    BodyKey m_key = 0;
    bool m_has = false;
    Candidate m_best;
    Result m_bestResult;
};

class AllRayCollector final : public JPH::CastRayCollector {
public:
    void OnBody(const JPH::Body& body) override {
        m_layer = static_cast<u8>(body.GetObjectLayer());
        m_key = body.GetUserData();
    }
    void AddHit(const JPH::RayCastResult& hit) override {
        hits.push_back({hit.mFraction, m_layer, m_key, hit.mSubShapeID2.GetValue(), hit.mBodyID});
    }
    std::vector<Candidate> hits;

private:
    u8 m_layer = 0;
    BodyKey m_key = 0;
};

GroundState fromJolt(JPH::CharacterBase::EGroundState s) {
    switch (s) {
        case JPH::CharacterBase::EGroundState::OnGround: return GroundState::OnGround;
        case JPH::CharacterBase::EGroundState::OnSteepGround: return GroundState::OnSteepGround;
        case JPH::CharacterBase::EGroundState::NotSupported: return GroundState::NotSupported;
        default: return GroundState::InAir;
    }
}

/// `rotation` turned by the shortest arc that takes its local +Y onto the unit vector `up`.
JPH::Quat alignLocalY(JPH::QuatArg rotation, JPH::Vec3Arg up) {
    const JPH::Vec3 localY = rotation.RotateAxisY();
    return (JPH::Quat::sFromTo(localY, up) * rotation).Normalized();
}

template <class T>
void appendBytes(std::vector<u8>& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

} // namespace

// -----------------------------------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------------------------------

struct PhysicsGrid::Impl {
    GridDesc desc;
    jolt::BroadPhaseLayers bpLayers;
    jolt::ObjectVsBroadPhase objectVsBroadPhase;
    jolt::ObjectPairs objectPairs;
    JPH::PhysicsSystem system;
    std::unique_ptr<jolt::TempAllocator> temp;
    std::unique_ptr<JPH::JobSystem> jobSystem;
    jolt::HeliosJobSystem* heliosJobs = nullptr; // jobSystem when stepping on a Helios job system
    HandlePool<BodySlot, BodyTag> bodies;
    std::map<LayerKey, BodyHandle> byKey; // ordered: iteration is (layer, key) order
    HandlePool<CharacterSlot, CharacterTag> characters;
    f64 accumulator = 0.0;
    u64 steps = 0;

    explicit Impl(const GridDesc& d) : desc(d), objectVsBroadPhase(d.collision), objectPairs(d.collision) {}

    ~Impl() {
        // Characters first (they reference the system), then every body in (layer, key) order.
        std::vector<CharacterHandle> chars;
        for (auto item : characters) chars.push_back(item.handle);
        for (CharacterHandle c : chars) characters.destroy(c);
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (const auto& [lk, h] : byKey) {
            const BodySlot* slot = bodies.get(h);
            if (!slot) continue;
            bi.RemoveBody(slot->id);
            bi.DestroyBody(slot->id);
        }
    }

    const BodySlot* slot(BodyHandle h) const { return bodies.get(h); }

    Result<void> validate(const BodyDesc& d) const {
        if (d.key == 0) return Error{ErrorCode::InvalidArgument, "body key 0 is reserved (02 §7.1)"};
        if (d.layer >= ObjectLayer::Count) return Error{ErrorCode::InvalidArgument, "invalid object layer"};
        if (!d.shape) return Error{ErrorCode::InvalidArgument, "body has no shape"};
        if (isStaticLayer(d.layer) && d.motion != MotionType::Static) {
            return makeError(ErrorCode::InvalidArgument, "{} bodies must be static", objectLayerName(d.layer));
        }
        if (d.layer == ObjectLayer::Kinematic && d.motion != MotionType::Kinematic) {
            return Error{ErrorCode::InvalidArgument, "Kinematic-layer bodies must be kinematic"};
        }
        if (!finite(d.position) || !validRotation(d.rotation) || !finite(d.linearVelocity) || !finite(d.angularVelocity) ||
            !finiteNonNegative(d.mass)) {
            return Error{ErrorCode::InvalidArgument,
                         "body pose, velocities and mass must be finite (mass >= 0, rotation not zero)"};
        }
        if (!finiteNonNegative(d.friction) || !finiteNonNegative(d.restitution) || !finiteNonNegative(d.linearDamping) ||
            !finiteNonNegative(d.angularDamping) || !finite(d.gravityFactor) || !(d.maxLinearVelocity > 0.0f) ||
            !finite(d.maxLinearVelocity)) {
            return Error{ErrorCode::InvalidArgument,
                         "body friction, restitution and damping must be finite and >= 0, gravityFactor finite and "
                         "maxLinearVelocity finite and > 0"};
        }
        if (d.motion != MotionType::Static && mustBeStatic(d.shape)) {
            return makeError(ErrorCode::InvalidArgument,
                             "a {} body cannot use a mesh shape (or a compound holding one): Jolt has no mass "
                             "properties for it; make it static",
                             d.motion == MotionType::Dynamic ? "dynamic" : "kinematic");
        }
        if (byKey.contains({static_cast<u8>(d.layer), d.key})) {
            return makeError(ErrorCode::AlreadyExists, "a {} body with key {:#x} already exists in grid '{}'",
                             objectLayerName(d.layer), d.key, desc.name);
        }
        return {};
    }

    Result<BodyHandle> add(const BodyDesc& d) {
        JPH::BodyCreationSettings bcs(static_cast<const JPH::Shape*>(d.shape.native()), toJolt(d.position),
                                      toJolt(d.rotation).Normalized(), toJolt(d.motion), jolt::toJolt(d.layer));
        bcs.mUserData = d.key; // the stable key (02 §7.1): what the stable-order patch sorts by
        bcs.mLinearVelocity = toJolt(d.linearVelocity);
        bcs.mAngularVelocity = toJolt(d.angularVelocity);
        bcs.mFriction = d.friction;
        bcs.mRestitution = d.restitution;
        bcs.mLinearDamping = d.linearDamping;
        bcs.mAngularDamping = d.angularDamping;
        bcs.mGravityFactor = d.gravityFactor;
        bcs.mMaxLinearVelocity = d.maxLinearVelocity;
        bcs.mAllowSleeping = d.allowSleeping;
        bcs.mIsSensor = d.sensor;
        if (d.motion == MotionType::Kinematic) bcs.mAllowDynamicOrKinematic = true;
        if (d.mass > 0.0f && d.motion == MotionType::Dynamic) {
            bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = d.mass;
        }
        JPH::BodyInterface& bi = system.GetBodyInterface();
        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) {
            return makeError(ErrorCode::LimitExceeded, "grid '{}' is full ({} bodies)", desc.name, desc.maxBodies);
        }
        bi.AddBody(body->GetID(), d.motion == MotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        const BodyHandle h = bodies.create(BodySlot{body->GetID(), d.layer, d.key});
        byKey.emplace(LayerKey{static_cast<u8>(d.layer), d.key}, h);
        return h;
    }

    JPH::Vec3 characterGravity(const JPH::CharacterVirtual& c) const {
        const f32 g = std::sqrt(desc.gravity.x * desc.gravity.x + desc.gravity.y * desc.gravity.y +
                                desc.gravity.z * desc.gravity.z);
        return -g * c.GetUp();
    }
};

// -----------------------------------------------------------------------------------------------
// PhysicsGrid
// -----------------------------------------------------------------------------------------------

namespace {
constexpr u32 kMaxGridPairs = 1u << 24;
constexpr u32 kMaxCollisionSteps = 64;
} // namespace

Result<std::unique_ptr<PhysicsGrid>> PhysicsGrid::create(const GridDesc& desc) {
    if (!PhysicsRuntime::initialized()) return Error{ErrorCode::InvalidState, "PhysicsGrid: no PhysicsRuntime is alive"};
    // Jolt's BodyID keeps a 23-bit index (BodyManager::Init only asserts it in debug builds), and
    // Update() takes the collision step count as an int.
    if (desc.maxBodies == 0 || desc.maxBodies > JPH::BodyID::cMaxBodyIndex || desc.maxBodyPairs == 0 ||
        desc.maxBodyPairs > kMaxGridPairs || desc.maxContactConstraints == 0 || desc.maxContactConstraints > kMaxGridPairs ||
        !(desc.fixedDt > 0.0f) || !finite(desc.fixedDt) || desc.collisionSteps == 0 ||
        desc.collisionSteps > kMaxCollisionSteps || desc.maxStepsPerAdvance == 0 || desc.tempAllocatorBytes < (64u << 10) ||
        desc.tempAllocatorBytes > (1u << 30) || !finite(desc.gravity)) {
        return Error{ErrorCode::InvalidArgument, "PhysicsGrid: invalid GridDesc"};
    }
    std::unique_ptr<PhysicsGrid> grid(new PhysicsGrid());
    grid->m_impl = std::make_unique<Impl>(desc);
    Impl& m = *grid->m_impl;
    m.system.Init(desc.maxBodies, 0, desc.maxBodyPairs, desc.maxContactConstraints, m.bpLayers, m.objectVsBroadPhase,
                  m.objectPairs);
    JPH::PhysicsSettings settings;
    settings.mDeterministicSimulation = true; // (the default; stated because RT-03 depends on it)
    m.system.SetPhysicsSettings(settings);
    m.system.SetGravity(toJolt(desc.gravity));
    m.temp = std::make_unique<jolt::TempAllocator>(desc.tempAllocatorBytes, desc.name);
    if (desc.jobs) {
        auto js = std::make_unique<jolt::HeliosJobSystem>(*desc.jobs);
        m.heliosJobs = js.get();
        m.jobSystem = std::move(js);
    } else {
        m.jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    }
    return grid;
}

PhysicsGrid::~PhysicsGrid() = default;

const GridDesc& PhysicsGrid::desc() const noexcept { return m_impl->desc; }

Result<BodyHandle> PhysicsGrid::createBody(const BodyDesc& desc) {
    HELIOS_TRY(m_impl->validate(desc));
    return m_impl->add(desc);
}

Result<std::vector<BodyHandle>> PhysicsGrid::createBodies(std::span<const BodyDesc> descs) {
    Impl& m = *m_impl;
    std::vector<u32> order(descs.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const LayerKey ka{static_cast<u8>(descs[a].layer), descs[a].key}, kb{static_cast<u8>(descs[b].layer), descs[b].key};
        return ka < kb;
    });
    for (usize i = 0; i < order.size(); ++i) {
        HELIOS_TRY(m.validate(descs[order[i]]));
        if (i > 0 && descs[order[i]].layer == descs[order[i - 1]].layer && descs[order[i]].key == descs[order[i - 1]].key) {
            return makeError(ErrorCode::AlreadyExists, "createBodies: duplicate key {:#x} in the batch", descs[order[i]].key);
        }
    }
    if (m.bodies.size() + descs.size() > m.desc.maxBodies) {
        return makeError(ErrorCode::LimitExceeded, "grid '{}' cannot take {} more bodies", m.desc.name, descs.size());
    }
    std::vector<BodyHandle> out(descs.size());
    for (u32 idx : order) {
        auto h = m.add(descs[idx]);
        if (!h.ok()) {
            for (BodyHandle created : out) destroyBody(created); // all-or-nothing
            return h.error();
        }
        out[idx] = *h;
    }
    return out;
}

void PhysicsGrid::destroyBody(BodyHandle body) {
    Impl& m = *m_impl;
    const BodySlot* s = m.slot(body);
    if (!s) return;
    JPH::BodyInterface& bi = m.system.GetBodyInterface();
    bi.RemoveBody(s->id);
    bi.DestroyBody(s->id);
    m.byKey.erase(LayerKey{static_cast<u8>(s->layer), s->key});
    m.bodies.destroy(body);
}

bool PhysicsGrid::contains(BodyHandle body) const noexcept { return m_impl->slot(body) != nullptr; }

BodyHandle PhysicsGrid::findBody(ObjectLayer layer, BodyKey key) const noexcept {
    const auto it = m_impl->byKey.find(LayerKey{static_cast<u8>(layer), key});
    return it == m_impl->byKey.end() ? BodyHandle{} : it->second;
}

u32 PhysicsGrid::bodyCount() const noexcept { return m_impl->bodies.size(); }

std::vector<BodyHandle> PhysicsGrid::bodies() const {
    std::vector<BodyHandle> out;
    out.reserve(m_impl->byKey.size());
    for (const auto& [lk, h] : m_impl->byKey) out.push_back(h);
    return out;
}

namespace {
Error staleBody() { return Error{ErrorCode::NotFound, "stale or invalid body handle"}; }
Error staleCharacter() { return Error{ErrorCode::NotFound, "stale or invalid character handle"}; }
} // namespace

Result<BodyState> PhysicsGrid::bodyState(BodyHandle body) const {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), s->id);
    if (!lock.Succeeded()) return staleBody();
    const JPH::Body& b = lock.GetBody();
    BodyState st;
    st.position = fromJoltR(b.GetPosition());
    st.rotation = fromJolt(b.GetRotation());
    st.linearVelocity = fromJolt(b.GetLinearVelocity());
    st.angularVelocity = fromJolt(b.GetAngularVelocity());
    st.motion = fromJolt(b.GetMotionType());
    st.active = b.IsActive();
    return st;
}

Result<ObjectLayer> PhysicsGrid::bodyLayer(BodyHandle body) const {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    return s->layer;
}

Result<BodyKey> PhysicsGrid::bodyKey(BodyHandle body) const {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    return s->key;
}

Result<void> PhysicsGrid::setPose(BodyHandle body, const DVec3& position, const Quat& rotation, bool activate) {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    if (!finite(position) || !validRotation(rotation)) return Error{ErrorCode::InvalidArgument, "non-finite pose or zero rotation"};
    m_impl->system.GetBodyInterface().SetPositionAndRotation(
        s->id, toJolt(position), toJolt(rotation).Normalized(), activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    return {};
}

Result<void> PhysicsGrid::setVelocity(BodyHandle body, const Vec3& linear, const Vec3& angular) {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    if (!finite(linear) || !finite(angular)) return Error{ErrorCode::InvalidArgument, "non-finite velocity"};
    m_impl->system.GetBodyInterface().SetLinearAndAngularVelocity(s->id, toJolt(linear), toJolt(angular));
    return {};
}

Result<void> PhysicsGrid::addImpulse(BodyHandle body, const Vec3& impulse) {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    if (!finite(impulse)) return Error{ErrorCode::InvalidArgument, "non-finite impulse"};
    m_impl->system.GetBodyInterface().AddImpulse(s->id, toJolt(impulse));
    return {};
}

Result<void> PhysicsGrid::addForce(BodyHandle body, const Vec3& force) {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    if (!finite(force)) return Error{ErrorCode::InvalidArgument, "non-finite force"};
    m_impl->system.GetBodyInterface().AddForce(s->id, toJolt(force));
    return {};
}

Result<void> PhysicsGrid::moveKinematic(BodyHandle body, const DVec3& targetPosition, const Quat& targetRotation, f32 dt) {
    const BodySlot* s = m_impl->slot(body);
    if (!s) return staleBody();
    if (!(dt > 0.0f) || !finite(dt) || !finite(targetPosition) || !validRotation(targetRotation)) {
        return Error{ErrorCode::InvalidArgument, "moveKinematic: dt must be finite and > 0, the target finite"};
    }
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (bi.GetMotionType(s->id) != JPH::EMotionType::Kinematic) {
        return Error{ErrorCode::InvalidState, "moveKinematic: body is not kinematic"};
    }
    bi.MoveKinematic(s->id, toJolt(targetPosition), toJolt(targetRotation).Normalized(), dt);
    return {};
}

// -- Characters -------------------------------------------------------------------------------------

Result<CharacterHandle> PhysicsGrid::createCharacter(const CharacterDesc& desc) {
    Impl& m = *m_impl;
    if (desc.key == 0) return Error{ErrorCode::InvalidArgument, "character key 0 is reserved"};
    if (!(desc.radius > 0.0f) || !finite(desc.radius) || !(desc.halfHeight > 0.0f) || !finite(desc.halfHeight) ||
        !finite(desc.position) || !validRotation(desc.rotation) || !finite(desc.up) ||
        !(toJolt(desc.up).LengthSq() > 1e-12f)) {
        return Error{ErrorCode::InvalidArgument, "createCharacter: invalid capsule, pose or up vector"};
    }
    if (!(desc.maxSlopeRadians >= 0.0f && desc.maxSlopeRadians <= 1.5707964f) || !(desc.mass > 0.0f) ||
        !finite(desc.mass) || !finiteNonNegative(desc.maxStrength) || !finiteNonNegative(desc.stepHeight) ||
        !finiteNonNegative(desc.stickToFloorDistance)) {
        return Error{ErrorCode::InvalidArgument, "createCharacter: slope must be in [0, pi/2], mass > 0, strength, "
                                                 "step height and floor stick distance finite and >= 0"};
    }
    for (auto item : m.characters) {
        if (item.value.desc.key == desc.key) return makeError(ErrorCode::AlreadyExists, "character key {:#x} exists", desc.key);
    }
    // Capsule standing on `position`: its bottom sphere touches the position.
    JPH::RotatedTranslatedShapeSettings capsule(JPH::Vec3(0.0f, desc.halfHeight + desc.radius, 0.0f), JPH::Quat::sIdentity(),
                                                new JPH::CapsuleShapeSettings(desc.halfHeight, desc.radius));
    const auto shape = capsule.Create();
    if (shape.HasError()) return makeError(ErrorCode::InvalidArgument, "createCharacter: {}", shape.GetError().c_str());
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = shape.Get();
    settings->mUp = toJolt(desc.up).Normalized();
    settings->mMaxSlopeAngle = desc.maxSlopeRadians;
    settings->mMass = desc.mass;
    settings->mMaxStrength = desc.maxStrength;
    // Contacts count as support only on the lower hemisphere.
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc.radius);
    // The capsule (and its supporting volume) is built along local +Y: turn the requested rotation
    // so that local +Y lies on `up` (keeping the heading as far as possible).
    const JPH::Vec3 up = toJolt(desc.up).Normalized();
    const JPH::Quat rotation = alignLocalY(toJolt(desc.rotation).Normalized(), up);
    JPH::Ref<JPH::CharacterVirtual> c =
        new JPH::CharacterVirtual(settings, toJolt(desc.position), rotation, desc.key, &m.system);
    c->SetUp(up);
    return m.characters.create(CharacterSlot{c, desc});
}

void PhysicsGrid::destroyCharacter(CharacterHandle character) { m_impl->characters.destroy(character); }

Result<void> PhysicsGrid::moveCharacter(CharacterHandle character, const Vec3& desiredVelocity, f32 jumpSpeed, f32 dt) {
    Impl& m = *m_impl;
    CharacterSlot* slot = m.characters.get(character);
    if (!slot) return staleCharacter();
    if (!(dt > 0.0f) || !finite(dt) || !finite(desiredVelocity) || !(jumpSpeed >= 0.0f) || !finite(jumpSpeed)) {
        return Error{ErrorCode::InvalidArgument, "moveCharacter: invalid velocity or dt"};
    }
    JPH::CharacterVirtual& c = *slot->character;
    const JPH::Vec3 up = c.GetUp();
    const JPH::Vec3 gravity = m.characterGravity(c);
    // Jolt's recommended ExtendedUpdate input (CharacterVirtual.h): keep the vertical speed in the
    // air, follow the ground when supported and not moving away from it.
    c.UpdateGroundVelocity();
    const JPH::Vec3 current = c.GetLinearVelocity();
    const JPH::Vec3 currentVertical = up.Dot(current) * up;
    const JPH::Vec3 groundVelocity = c.GetGroundVelocity();
    JPH::Vec3 velocity;
    if (c.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
        (currentVertical - groundVelocity).Dot(up) < 0.1f) {
        velocity = groundVelocity;
        if (jumpSpeed > 0.0f) velocity += jumpSpeed * up;
    } else {
        velocity = currentVertical;
    }
    velocity += gravity * dt;
    const JPH::Vec3 desired = toJolt(desiredVelocity);
    velocity += desired - up.Dot(desired) * up; // horizontal part of the input
    c.SetLinearVelocity(velocity);

    JPH::CharacterVirtual::ExtendedUpdateSettings eus;
    eus.mStickToFloorStepDown = -up * slot->desc.stickToFloorDistance;
    eus.mWalkStairsStepUp = up * slot->desc.stepHeight;
    const JPH::DefaultBroadPhaseLayerFilter bpFilter(m.objectVsBroadPhase, jolt::toJolt(ObjectLayer::Character));
    const JPH::DefaultObjectLayerFilter layerFilter(m.objectPairs, jolt::toJolt(ObjectLayer::Character));
    const JPH::BodyFilter bodyFilter;
    const JPH::ShapeFilter shapeFilter;
    c.ExtendedUpdate(dt, gravity, eus, bpFilter, layerFilter, bodyFilter, shapeFilter, *m.temp);
    return {};
}

Result<CharacterState> PhysicsGrid::characterState(CharacterHandle character) const {
    const CharacterSlot* slot = m_impl->characters.get(character);
    if (!slot) return staleCharacter();
    const JPH::CharacterVirtual& c = *slot->character;
    CharacterState st;
    st.position = fromJoltR(c.GetPosition());
    st.rotation = fromJolt(c.GetRotation());
    st.linearVelocity = fromJolt(c.GetLinearVelocity());
    st.ground = fromJolt(c.GetGroundState());
    st.groundKey = c.IsSupported() ? c.GetGroundUserData() : 0; // the ground body's key (mUserData)
    return st;
}

Result<void> PhysicsGrid::setCharacterUp(CharacterHandle character, const Vec3& up) {
    CharacterSlot* slot = m_impl->characters.get(character);
    if (!slot) return staleCharacter();
    const JPH::Vec3 u = toJolt(up);
    if (!finite(up) || !(u.LengthSq() > 1e-12f)) return Error{ErrorCode::InvalidArgument, "setCharacterUp: zero or non-finite up"};
    JPH::CharacterVirtual& c = *slot->character;
    const JPH::Vec3 n = u.Normalized();
    // Stand the capsule along the new up about its feet (the character's origin), so radial gravity
    // on a planet keeps the capsule upright and the supporting volume under it.
    c.SetRotation(alignLocalY(c.GetRotation(), n));
    c.SetUp(n);
    return {};
}

// -- Simulation --------------------------------------------------------------------------------------

Result<void> PhysicsGrid::step() {
    Impl& m = *m_impl;
    const JPH::EPhysicsUpdateError err = m.system.Update(m.desc.fixedDt, static_cast<int>(m.desc.collisionSteps),
                                                         m.temp.get(), m.jobSystem.get());
    // Release the job-pool slots that already-executed wrappers still hold (job_system.h).
    if (m.heliosJobs) m.heliosJobs->drain();
    ++m.steps;
    if (err != JPH::EPhysicsUpdateError::None) {
        std::string what;
        if ((err & JPH::EPhysicsUpdateError::ManifoldCacheFull) != JPH::EPhysicsUpdateError::None) what += " manifold cache full;";
        if ((err & JPH::EPhysicsUpdateError::BodyPairCacheFull) != JPH::EPhysicsUpdateError::None) what += " body pair cache full;";
        if ((err & JPH::EPhysicsUpdateError::ContactConstraintsFull) != JPH::EPhysicsUpdateError::None) {
            what += " contact constraints full;";
        }
        HELIOS_LOG_WARN(LogPhysics, "grid '{}' step {}:{}", m.desc.name, m.steps, what);
        return makeError(ErrorCode::LimitExceeded, "grid '{}' step {}:{}", m.desc.name, m.steps, what);
    }
    return {};
}

Result<u32> PhysicsGrid::advance(f64 seconds) {
    Impl& m = *m_impl;
    if (!(seconds >= 0.0) || !finite(seconds)) return Error{ErrorCode::InvalidArgument, "advance: seconds must be >= 0"};
    m.accumulator += seconds;
    const f64 dt = m.desc.fixedDt;
    u32 n = 0;
    while (m.accumulator >= dt && n < m.desc.maxStepsPerAdvance) {
        m.accumulator -= dt;
        HELIOS_TRY(step());
        ++n;
    }
    if (m.accumulator >= dt) {
        // A hitch longer than maxStepsPerAdvance steps: drop the remaining whole steps (keeping the
        // fraction) instead of catching up. The loop above also never spins on an interval so large
        // that subtracting dt no longer changes the accumulator.
        const f64 dropped = std::floor(m.accumulator / dt);
        m.accumulator = std::fmod(m.accumulator, dt);
        HELIOS_LOG_WARN(LogPhysics, "grid '{}': advance() dropped {} fixed steps after running {} (maxStepsPerAdvance)",
                        m.desc.name, dropped, n);
    }
    return n;
}

u64 PhysicsGrid::stepCount() const noexcept { return m_impl->steps; }

void PhysicsGrid::optimizeBroadPhase() { m_impl->system.OptimizeBroadPhase(); }

PhysicsGrid::Stats PhysicsGrid::stats() const noexcept {
    const Impl& m = *m_impl;
    Stats st;
    if (m.heliosJobs) {
        st.jobsInFlight = m.heliosJobs->liveJobs();
        st.jobPoolPeak = m.heliosJobs->peakJobs();
    }
    st.tempAllocatorFallbacks = m.temp->fallbackCount();
    return st;
}

// -- Queries -----------------------------------------------------------------------------------------

std::optional<RayHit> PhysicsGrid::castRay(const RayCast& ray) const {
    const Impl& m = *m_impl;
    if (!finite(ray.origin) || !finite(ray.direction)) return std::nullopt;
    const JPH::RRayCast r(toJolt(ray.origin), toJolt(ray.direction));
    JPH::RayCastSettings settings;
    ClosestCollector<JPH::CastRayCollector, JPH::RayCastResult> collector;
    const MaskBroadPhaseFilter bp(ray.layers);
    const MaskObjectLayerFilter ol(ray.layers);
    const BodySlot* ignored = m.slot(ray.ignore);
    const JPH::IgnoreSingleBodyFilter ignoreFilter(ignored ? ignored->id : JPH::BodyID());
    const JPH::BodyFilter noFilter;
    m.system.GetNarrowPhaseQuery().CastRay(r, settings, collector, bp, ol, ignored ? static_cast<const JPH::BodyFilter&>(ignoreFilter) : noFilter);
    if (!collector.has()) return std::nullopt;
    const Candidate& c = collector.best();
    RayHit hit;
    hit.layer = static_cast<ObjectLayer>(c.layer);
    hit.key = c.key;
    hit.body = findBody(hit.layer, c.key);
    hit.fraction = c.fraction;
    hit.subShape = c.subShape;
    const JPH::RVec3 point = r.GetPointOnRay(c.fraction);
    hit.point = fromJoltR(point);
    JPH::BodyLockRead lock(m.system.GetBodyLockInterface(), c.body);
    if (lock.Succeeded()) {
        JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(collector.bestResult().mSubShapeID2, point);
        if (n.Dot(r.mDirection) > 0.0f) n = -n;
        hit.normal = fromJolt(n);
    }
    return hit;
}

std::vector<RayHit> PhysicsGrid::castRayAll(const RayCast& ray) const {
    const Impl& m = *m_impl;
    std::vector<RayHit> out;
    if (!finite(ray.origin) || !finite(ray.direction)) return out;
    const JPH::RRayCast r(toJolt(ray.origin), toJolt(ray.direction));
    JPH::RayCastSettings settings;
    AllRayCollector collector;
    const MaskBroadPhaseFilter bp(ray.layers);
    const MaskObjectLayerFilter ol(ray.layers);
    const BodySlot* ignored = m.slot(ray.ignore);
    const JPH::IgnoreSingleBodyFilter ignoreFilter(ignored ? ignored->id : JPH::BodyID());
    const JPH::BodyFilter noFilter;
    m.system.GetNarrowPhaseQuery().CastRay(r, settings, collector, bp, ol, ignored ? static_cast<const JPH::BodyFilter&>(ignoreFilter) : noFilter);
    std::sort(collector.hits.begin(), collector.hits.end());
    out.reserve(collector.hits.size());
    for (const Candidate& c : collector.hits) {
        RayHit hit;
        hit.layer = static_cast<ObjectLayer>(c.layer);
        hit.key = c.key;
        hit.body = findBody(hit.layer, c.key);
        hit.fraction = c.fraction;
        hit.subShape = c.subShape;
        const JPH::RVec3 point = r.GetPointOnRay(c.fraction);
        hit.point = fromJoltR(point);
        JPH::BodyLockRead lock(m.system.GetBodyLockInterface(), c.body);
        if (lock.Succeeded()) {
            JPH::SubShapeID sub;
            sub.SetValue(c.subShape);
            JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(sub, point);
            if (n.Dot(r.mDirection) > 0.0f) n = -n;
            hit.normal = fromJolt(n);
        }
        out.push_back(hit);
    }
    return out;
}

std::optional<ShapeHit> PhysicsGrid::castShape(const ShapeCast& cast) const {
    const Impl& m = *m_impl;
    if (!cast.shape || !finite(cast.position) || !validRotation(cast.rotation) || !finite(cast.direction) ||
        mustBeStatic(cast.shape)) {
        return std::nullopt;
    }
    const auto* shape = static_cast<const JPH::Shape*>(cast.shape.native());
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(toJolt(cast.rotation).Normalized(), toJolt(cast.position));
    const JPH::RShapeCast sc = JPH::RShapeCast::sFromWorldTransform(shape, JPH::Vec3::sOne(), start, toJolt(cast.direction));
    JPH::ShapeCastSettings settings;
    ClosestCollector<JPH::CastShapeCollector, JPH::ShapeCastResult> collector;
    const MaskBroadPhaseFilter bp(cast.layers);
    const MaskObjectLayerFilter ol(cast.layers);
    const BodySlot* ignored = m.slot(cast.ignore);
    const JPH::IgnoreSingleBodyFilter ignoreFilter(ignored ? ignored->id : JPH::BodyID());
    const JPH::BodyFilter noFilter;
    const JPH::RVec3 base = toJolt(cast.position); // results relative to the start for precision far from the origin
    m.system.GetNarrowPhaseQuery().CastShape(sc, settings, base, collector, bp, ol,
                                            ignored ? static_cast<const JPH::BodyFilter&>(ignoreFilter) : noFilter);
    if (!collector.has()) return std::nullopt;
    const Candidate& c = collector.best();
    const JPH::ShapeCastResult& r = collector.bestResult();
    ShapeHit hit;
    hit.layer = static_cast<ObjectLayer>(c.layer);
    hit.key = c.key;
    hit.body = findBody(hit.layer, c.key);
    hit.fraction = c.fraction;
    hit.subShape = c.subShape;
    hit.contactPoint = fromJoltR(base + r.mContactPointOn2);
    const JPH::Vec3 axis = r.mPenetrationAxis;
    hit.normal = axis.LengthSq() > 0.0f ? fromJolt(-axis.Normalized()) : Vec3{};
    hit.penetration = r.mPenetrationDepth;
    return hit;
}

// -- Determinism -------------------------------------------------------------------------------------

u64 PhysicsGrid::stateHash() const {
    const Impl& m = *m_impl;
    std::vector<u8> bytes;
    bytes.reserve(m.byKey.size() * 72 + m.characters.size() * 48 + 16);
    for (const auto& [lk, h] : m.byKey) {
        const BodySlot* s = m.slot(h);
        JPH::BodyLockRead lock(m.system.GetBodyLockInterface(), s->id);
        const JPH::Body& b = lock.GetBody();
        appendBytes(bytes, lk.first);
        appendBytes(bytes, lk.second);
        appendBytes(bytes, static_cast<u8>(b.GetMotionType()));
        appendBytes(bytes, static_cast<u8>(b.IsActive() ? 1 : 0));
        const JPH::RVec3 p = b.GetPosition();
        const JPH::Quat q = b.GetRotation();
        const JPH::Vec3 v = b.GetLinearVelocity(), w = b.GetAngularVelocity();
        const f64 pos[3] = {p.GetX(), p.GetY(), p.GetZ()};
        const f32 rest[10] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW(), v.GetX(), v.GetY(), v.GetZ(), w.GetX(), w.GetY(), w.GetZ()};
        appendBytes(bytes, pos);
        appendBytes(bytes, rest);
    }
    std::vector<const CharacterSlot*> chars;
    for (auto item : m.characters) chars.push_back(&item.value);
    std::sort(chars.begin(), chars.end(), [](const CharacterSlot* a, const CharacterSlot* b) { return a->desc.key < b->desc.key; });
    for (const CharacterSlot* cs : chars) {
        const JPH::CharacterVirtual& c = *cs->character;
        appendBytes(bytes, cs->desc.key);
        const JPH::RVec3 p = c.GetPosition();
        const JPH::Quat q = c.GetRotation();
        const JPH::Vec3 v = c.GetLinearVelocity();
        const f64 pos[3] = {p.GetX(), p.GetY(), p.GetZ()};
        const f32 rest[7] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW(), v.GetX(), v.GetY(), v.GetZ()};
        appendBytes(bytes, pos);
        appendBytes(bytes, rest);
        appendBytes(bytes, static_cast<u8>(c.GetGroundState()));
    }
    return hash64(bytes.data(), bytes.size(), 0x70687973ull);
}

} // namespace helios::physics
