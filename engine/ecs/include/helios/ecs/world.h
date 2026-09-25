#pragma once
// helios::ecs::World — the Helios wrapper around one flecs::world (ADR-004, 02 §4).
//
// Owns: the flecs world (its OS API routed to Helios logging/asserts/time/mimalloc, os_api.h), the
// component registry (template + runtime descriptor paths), the EntityId allocator and the
// EntityId <-> Entity <-> NetHandle maps, relationship helpers (ChildOf-style hierarchy, InFrame,
// DockedTo, IsA prefabs with overrides), the deterministic system scheduler (system.h), command
// buffer application at sync points, per-field dirty tracking and the per-tick structural log.
//
// Relationship storage (SPIKES.md §2): DockedTo defaults to a DockRef field plus a reverse index
// (no table per host, re-docking is a value write; flecs 4.1.6 DontFragment pairs were measured
// ~2x slower to churn than even fragmenting pairs), the hierarchy uses flecs 4.1's non-fragmenting
// Parent component (ChildOf-per-parent tables would break RT-01's 5,000-table cap with thousands of
// ships), and InFrame stays fragmenting (few frames; systems iterate per-frame tables).
//
// Query objects and any pointer obtained from the World must not outlive it.
//
// Threading: a World is driven by one thread (the zone's tick thread). During tick() stages, system
// functions run on job workers under the rules in system.h. All mutating World functions must be
// called outside a running stage (asserted in development builds); const lookups may be called
// from system functions for components the system declared.

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/memory.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/command_buffer.h"
#include "helios/ecs/component.h"
#include "helios/ecs/dirty.h"
#include "helios/ecs/entity_id.h"
#include "helios/ecs/registry.h"
#include "helios/ecs/system.h"
#include "helios/ecs/types.h"
#include "helios/math/frame.h"

namespace helios::jobs {
class JobSystem;
}

namespace helios::ecs {

/// Marks a frame entity (target of InFrame) and names the world module's frame.
struct FrameRef {
    FrameId frame;
};

/// How DockedTo is stored (SPIKES.md §2 measured all three).
enum class DockStorage : u8 {
    Field,            ///< DockRef{host} component + reverse index: re-dock = value write, no table move.
    Pair,             ///< (DockedTo, host) exclusive pair: one table per host.
    PairDontFragment, ///< (DockedTo, host) with flecs DontFragment: no tables, but costly churn in 4.1.6.
};

struct RelationConfig {
    DockStorage docking = DockStorage::Field;
    bool inFrameDontFragment = false;   ///< (InFrame, frame) — frames are few; keep per-frame tables.
    bool nonFragmentingHierarchy = true; ///< Parent component instead of (ChildOf, parent) pairs.
};

/// DockStorage::Field: the host an entity is docked at (invalid = undocked). Never inherited.
struct DockRef {
    Entity host;
};

struct WorldDesc {
    std::string name = "World";
    jobs::JobSystem* jobs = nullptr;  ///< nullptr = run every stage on the calling thread.
    u32 shard = 0;                    ///< EntityId shard (< 32).
    /// ID block source (the cell's AllocateIdBlocks client; not owned). nullptr = a private
    /// LocalIdBlockSource on `idClock` (tests, tools, offline worlds, deterministic replays).
    IdBlockSource* idBlocks = nullptr;
    /// ms since the ID epoch, for the private source and block retirement. Empty = wall clock.
    /// Deterministic replays pass simulated time.
    EntityIdMinter::Clock idClock;
    i64 idLastPrefix = -1;            ///< Private source only: persisted LocalIdBlockSource::lastPrefix().
    u32 idHoldBlocks = 2;             ///< Blocks held by the minter (4 for battle-profile cells).
    NetHandleTable::Desc handles;
    RelationConfig relations;
};

enum class StructuralOp : u8 {
    Create,
    Destroy,
    Add,
    Remove,
    SetParent,
    ClearParent,
    SetFrame,
    Dock,
    Undock,
};
std::string_view structuralOpName(StructuralOp op) noexcept;

/// One entry of the per-tick structural log (02 §4.4), consumed by replication (creates/destroys,
/// reparents) and presentation. `arg`: Add/Remove = ComponentId; SetParent/Dock = the other
/// entity's EntityId; SetFrame = the frame's FrameId value (or its flecs id without a FrameRef; 0 =
/// no frame). Destroying a frame or docking host logs SetFrame(0) / Undock for the entities that
/// were in it or docked at it.
struct StructuralEvent {
    StructuralOp op = StructuralOp::Create;
    EntityId entity;
    NetHandle handle;
    u64 arg = 0;
    friend bool operator==(const StructuralEvent&, const StructuralEvent&) = default;
};

/// Called while applying an InFrame change (before the pair changes) so the world module can
/// convert the entity's frame-local state (02 §5.3). `from` is invalid when the entity had no frame.
using ReparentHook = std::function<void(World& world, Entity e, Entity from, Entity to)>;

struct WorldStats {
    u32 tableCount = 0;
    u32 entityCount = 0;        ///< Entities with NetIdentity (spawned through World).
    u32 componentCount = 0;     ///< Registered through World.
    u32 systemCount = 0;
    u64 structuralOpsApplied = 0;
    u64 commandsDiscarded = 0;  ///< Refused commands/spawns: dead targets or relation targets, cycles, taken ids.
    i64 ecsHeapLiveBytes = 0;   ///< All worlds + containers (tag "ECS").
};

/// Main-thread query over chunks (tools, tests, bespoke passes). Uses a flecs cached query; destroy
/// it before its World.
class Query {
public:
    Query() noexcept = default;
    Query(World& world, std::vector<Term> terms);
    ~Query();
    Query(Query&& other) noexcept;
    Query& operator=(Query&& other) noexcept;
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    bool isValid() const noexcept { return m_query != nullptr; }
    /// Calls fn(ChunkView&) for every matched table range, in deterministic order. The world must
    /// not change structurally during the call.
    void forEachChunk(const std::function<void(ChunkView&)>& fn) const;
    /// Appends every matched chunk (valid until the next structural change).
    void collect(std::vector<ChunkData>& out) const;
    /// Number of matched entities.
    u32 count() const;

private:
    World* m_world = nullptr;
    void* m_query = nullptr; // QueryPlan* (world_impl.h)
    std::vector<Term> m_terms;
};

class World {
public:
    explicit World(const WorldDesc& desc = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /// The underlying flecs world (escape hatch; stays owned by this World). Its user ctx
    /// (ecs_get_ctx) is reserved for the wrapper.
    ecs_world_t* flecsWorld() const noexcept { return m_flecs; }
    jobs::JobSystem* jobs() const noexcept { return m_jobs; }
    const std::string& name() const noexcept;

    // ---------------------------------------------------------------------------- components
    /// Runtime registration (reflection/schema path). Idempotent per name: re-registering an
    /// existing name with the same size/alignment returns the existing id; a mismatch is an error.
    Result<ComponentId> registerComponent(const ComponentDesc& desc);
    /// Template registration: builds the descriptor with componentDescOf<T>() and binds T's slot.
    template <class T>
    ComponentId registerComponent(ComponentFlags flags = ComponentFlags::None, std::string_view name = {});
    /// Binds C++ type T to an already registered component (e.g. registered at runtime by name).
    template <class T>
    Result<void> bindType(ComponentId id);

    /// Id of registered type T (0 if unregistered). Lock-free; callable from systems.
    template <class T>
    ComponentId id() const noexcept {
        const u32 slot = typeSlot<T>();
        return slot < m_typeIds.size() ? m_typeIds[slot] : 0;
    }
    ComponentId idForSlot(u32 slot) const noexcept { return slot < m_typeIds.size() ? m_typeIds[slot] : 0; }
    /// RepDirty::componentMask bit of replicated type T (0 if T is not replicated). O(1).
    template <class T>
    u64 replicationBit() const noexcept {
        const u32 slot = typeSlot<T>();
        return slot < m_typeReplBits.size() ? m_typeReplBits[slot] : 0;
    }
    const ComponentInfo* componentInfo(ComponentId id) const noexcept;
    const ComponentInfo* findComponent(std::string_view name) const noexcept;
    /// Replicated component with RepDirty bit `replIndex` (nullptr if none).
    const ComponentInfo* replicatedComponent(u32 replIndex) const noexcept;
    /// Relationship pair id (rel, target).
    static ComponentId pair(Entity relation, Entity target) noexcept;

    // ------------------------------------------------------------------------------ entities
    /// Creates an entity with NetIdentity (EntityId minted unless desc.id is set; NetHandle unless
    /// desc.netHandle is false), optional prefab/parent/frame. Logged as Create. Returns an invalid
    /// Entity (and counts WorldStats::commandsDiscarded) if the id is taken or no id is available,
    /// or if the prefab, parent or frame is not alive.
    Entity spawn(const SpawnDesc& desc = {});
    /// Deletes the entity (children in the hierarchy are deleted with it). Logged as Destroy. Always
    /// delete World entities through this call (not ecs_delete) so the identity maps stay in sync.
    void destroy(Entity e);
    bool isAlive(Entity e) const noexcept;
    EntityId entityId(Entity e) const noexcept;
    NetHandle netHandle(Entity e) const noexcept;
    AgId authorityGroup(Entity e) const noexcept;
    Entity find(EntityId id) const noexcept { return m_registry.find(id); }
    Entity find(NetHandle handle) const noexcept { return m_registry.find(handle); }
    const EntityRegistry& registry() const noexcept { return m_registry; }
    EntityIdMinter& idMinter() noexcept { return m_ids; }
    /// The private block source (nullptr when WorldDesc::idBlocks was given).
    LocalIdBlockSource* localIdBlocks() noexcept { return m_localIdBlocks.get(); }
    /// Sets the entity's flecs name (debugging/explorer; not replicated).
    void setName(Entity e, std::string_view name);
    std::string nameOf(Entity e) const;

    // ------------------------------------------------------------------------ raw components
    void addId(Entity e, ComponentId id);
    void removeId(Entity e, ComponentId id);
    bool hasId(Entity e, ComponentId id) const noexcept;  ///< Includes inherited components.
    bool ownsId(Entity e, ComponentId id) const noexcept; ///< Own storage only.
    const void* getRaw(Entity e, ComponentId id) const noexcept; ///< Includes inherited (shared) values.
    void* getMutRaw(Entity e, ComponentId id) noexcept;          ///< Own storage only (no dirty marks).
    /// Copies `size` bytes worth of an object into the entity (copy hook), adding it if missing.
    /// Replicated components: overwriting an owned value marks every field dirty (keeping bits already
    /// pending); adding one logs Add and starts with a clean _dirty mask. Ignores id 0 and dead entities.
    void setRaw(Entity e, ComponentId id, const void* value, usize size);

    // ---------------------------------------------------------------------- typed components
    template <class T> void add(Entity e) { addId(e, checkedId<T>()); }
    template <class T> void remove(Entity e) { removeId(e, checkedId<T>()); }
    template <class T> bool has(Entity e) const noexcept { return hasId(e, id<T>()); }
    template <class T> bool owns(Entity e) const noexcept { return ownsId(e, id<T>()); }
    template <class T> const T* get(Entity e) const noexcept { return static_cast<const T*>(getRaw(e, id<T>())); }
    /// Owned storage without dirty marks (server-only data, initialization).
    template <class T> T* getMut(Entity e) noexcept { return static_cast<T*>(getMutRaw(e, id<T>())); }
    template <class T> void set(Entity e, const T& value) { setRaw(e, checkedId<T>(), &value, sizeof(T)); }
    /// Dirty-tracking writer. Overrides an inherited (prefab) value first. Invalid Mut if absent.
    template <class T> Mut<T> mut(Entity e);
    /// Singletons live on their component entity.
    template <class T> void setSingleton(const T& value) { set<T>(Entity(checkedId<T>()), value); }
    template <class T> const T* singleton() const noexcept { return get<T>(Entity(id<T>())); }

    // ------------------------------------------------------------------------- relationships
    Entity inFrameRelation() const noexcept { return m_inFrame; }
    Entity dockedToRelation() const noexcept { return m_dockedTo; }
    /// Makes `parent` the transform parent of `child` (replacing any previous parent). Fails with
    /// InvalidArgument if it would create a cycle or either entity is dead.
    Result<void> setParent(Entity child, Entity parent);
    void clearParent(Entity child);
    Entity parentOf(Entity e) const noexcept;
    /// Direct children of `parent` (deterministic order).
    void childrenOf(Entity parent, std::vector<Entity>& out) const;
    /// Creates a frame entity (InFrame target) carrying FrameRef{frameId}.
    Entity createFrame(FrameId frameId, std::string_view name = {});
    /// Moves `e` into `frame` (runs the reparent hook). Invalid frame = remove from any frame. Fails
    /// with InvalidArgument if `e` or a valid `frame` is dead, or `frame == e`.
    Result<void> setFrame(Entity e, Entity frame);
    Entity frameOf(Entity e) const noexcept;
    void setReparentHook(ReparentHook hook);
    /// Docks `e` at `host` (exclusive: replaces a previous dock). Fails if host == e or dead.
    Result<void> dock(Entity e, Entity host);
    void undock(Entity e);
    Entity dockedTo(Entity e) const noexcept;
    /// Entities docked at `host` (ascending flecs id order). With DockStorage::Field this reads a
    /// reverse index maintained by dock(); like every lookup it must not race structural changes.
    void dockedAt(Entity host, std::vector<Entity>& out) const;
    DockStorage dockStorage() const noexcept { return m_desc.relations.docking; }

    // ------------------------------------------------------------------------------- prefabs
    /// Creates a prefab entity (ignored by queries). Add components with set<T>/add<T>; components
    /// flagged Shared are inherited by instances until overridden, others are copied.
    Entity createPrefab(std::string_view name = {});
    /// Prefab `child` becomes part of `prefab`'s hierarchy (instantiated with it, with its own
    /// identity). Uses the non-fragmenting Parent storage when RelationConfig::nonFragmentingHierarchy
    /// is set, so instances do not create a table each. Fails for dead or non-prefab entities and cycles.
    Result<void> addPrefabChild(Entity prefab, Entity child);
    /// spawn() with desc.prefab = prefab.
    Entity instantiate(Entity prefab, SpawnDesc desc = {});
    bool isA(Entity e, Entity prefab) const noexcept;
    /// Gives the instance its own copy of an inherited component (no-op if already owned).
    void overrideId(Entity e, ComponentId id);
    template <class T> void override(Entity e) { overrideId(e, checkedId<T>()); }

    // --------------------------------------------------------------- command buffers and log
    /// Applies one buffer now (main thread, outside stages).
    void apply(CommandBuffer& buffer);
    /// Applies buffers in order. Commands take effect immediately and in sequence; a spawn is fused
    /// with the set/add commands recorded for its TempEntity (up to the first other command on it)
    /// and inserted directly into its final table. Initial values of fused spawns are written in
    /// place, so flecs OnSet observers do not fire for them.
    void apply(std::span<CommandBuffer* const> buffers);
    const std::vector<StructuralEvent>& structuralLog() const noexcept { return m_log; }
    void clearStructuralLog() noexcept { m_log.clear(); }

    // ------------------------------------------------------------------------------ systems
    SystemBuilder system(std::string name) { return SystemBuilder(*this, std::move(name)); }
    Result<void> addSystem(SystemDesc desc);
    /// Validates and orders all systems (called by tick() when needed).
    Result<void> buildSchedule();
    /// Deterministic linear order of the stage's systems (builds the schedule if needed).
    std::vector<std::string> executionOrder(Stage stage);
    /// Edges of the stage DAG as (before, after) names.
    std::vector<std::pair<std::string, std::string>> scheduleEdges(Stage stage);
    /// Advances the tick counter and runs every stage (gather chunks -> run jobs -> sync point).
    Result<void> tick(f32 dt);
    /// Runs one stage of the current tick (for callers that interleave their own work).
    Result<void> runStage(Stage stage, f32 dt);
    /// Increments the tick counter and refills/retires ID blocks (tick() does this itself).
    void beginTick();
    Tick currentTick() const noexcept { return m_tick; }
    const SystemStats* systemStats(std::string_view name) const noexcept;
    std::vector<SystemStats> allSystemStats() const;
    /// Wall time of the last sync point (command application) per stage, in ns.
    u64 lastSyncNs(Stage stage) const noexcept;
    bool inParallelStage() const noexcept { return m_inStage; }

    // ----------------------------------------------------------------------- change tracking
    /// Collects and clears every dirty replicated component (deterministic order: table order,
    /// then row). Uses the job system for large worlds. Main thread, between stages.
    void gatherChanges(ChangeList& out);

    // ----------------------------------------------------------------------- flecs native path
    /// flecs-native multithreading for flecs systems/pipelines (compatibility path): flecs task
    /// threads are created through ecs_os_api.task_new_, which Helios maps to JobSystem jobs
    /// (os_api.h). `count` is clamped to the job system's worker count; 0/1 disables.
    void setFlecsTaskThreads(u32 count);
    /// ecs_progress (runs flecs pipelines/systems, if any were registered with the flecs API).
    void progressFlecs(f32 dt);

    WorldStats stats() const;
    const WorldDesc& desc() const noexcept { return m_desc; }

    // Internal (used by the scheduler/command buffers in src/; not part of the public contract).
    struct Impl;
    Impl& impl() noexcept { return *m_impl; }
    ComponentId repDirtyId() const noexcept { return m_repDirtyId; }
    ComponentId netIdentityId() const noexcept { return m_netIdentityId; }

private:
    template <class T>
    ComponentId checkedId() const noexcept {
        const ComponentId cid = id<T>();
        HELIOS_ASSERT(cid != 0, "component type is not registered in this world");
        return cid;
    }
    void bindSlot(u32 slot, ComponentId id);
    RepDirty* repDirtyOf(Entity e) noexcept;
    NetIdentity identityOf(Entity e) const noexcept;
    void assignChildIdentities(Entity root, bool netHandles, AgId ag);
    static constexpr u32 kMaxHierarchyDepth = 4096;
    bool isAncestor(Entity ancestor, Entity e) const noexcept;
    /// A component set/added together with a spawn (value == nullptr: add only).
    struct SpawnOp {
        ComponentId id = 0;
        const void* value = nullptr;
        u32 size = 0;
    };
    Entity spawnImpl(const SpawnDesc& desc, std::span<const SpawnOp> ops, EntityId preallocated);
    void spawnGroup(CommandBuffer& buffer, u32 groupIndex);
    void pruneDockList(Entity host, std::vector<u64>& list);
    void unregisterSubtree(Entity e);
    void releaseRelationTargets(Entity e);
    ::ecs_table_t* findTable(std::vector<u64>& ids);
    void ensureRepDirty(Entity e);
    /// Freshly instantiated entity: owned replicated components start with a clean _dirty mask and
    /// the entity gets its RepDirty summary.
    void initReplicatedState(Entity e);
    static void clearDirtyMask(void* component, const ComponentInfo& info) noexcept;
    void assertNotInStage() const noexcept { HELIOS_ASSERT(!m_inStage, "structural World call during a stage"); }
    void logEvent(StructuralOp op, Entity e, u64 arg);
    void applyOne(CommandBuffer& buffer);

    friend class Query;
    friend struct WorldAccess;

    WorldDesc m_desc;
    ecs_world_t* m_flecs = nullptr;
    jobs::JobSystem* m_jobs = nullptr;
    std::vector<ComponentId> m_typeIds; // typeSlot -> ComponentId
    std::vector<u64> m_typeReplBits;    // typeSlot -> RepDirty bit (0 = not replicated)
    std::unique_ptr<LocalIdBlockSource> m_localIdBlocks;
    EntityIdMinter m_ids;
    EntityRegistry m_registry;
    std::vector<StructuralEvent> m_log;
    ComponentId m_netIdentityId = 0;
    ComponentId m_repDirtyId = 0;
    ComponentId m_frameRefId = 0;
    ComponentId m_dockRefId = 0;
    Entity m_inFrame;
    Entity m_dockedTo;
    Tick m_tick = 0;
    bool m_inStage = false;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------------------------
// Template definitions
// ---------------------------------------------------------------------------------------------

template <class T>
ComponentId World::registerComponent(ComponentFlags flags, std::string_view name) {
    Result<ComponentId> r = registerComponent(componentDescOf<T>(name, flags));
    HELIOS_VERIFY(r.hasValue(), "registerComponent<T> failed");
    if (!r.hasValue()) return 0;
    bindSlot(typeSlot<T>(), *r);
    return *r;
}

template <class T>
Result<void> World::bindType(ComponentId cid) {
    const ComponentInfo* info = componentInfo(cid);
    if (!info) return Error{ErrorCode::NotFound, "bindType: unknown component"};
    const u32 size = std::is_empty_v<T> ? 0u : static_cast<u32>(sizeof(T));
    if (info->size != size) return Error{ErrorCode::InvalidArgument, "bindType: size mismatch"};
    if (size != 0 && info->alignment < alignof(T)) return Error{ErrorCode::InvalidArgument, "bindType: alignment mismatch"};
    bindSlot(typeSlot<T>(), cid);
    return {};
}

template <class T>
Mut<T> World::mut(Entity e) {
    const ComponentId cid = checkedId<T>();
    if (!ownsId(e, cid)) {
        if (!hasId(e, cid)) return {};
        overrideId(e, cid);
    }
    T* ptr = static_cast<T*>(getMutRaw(e, cid));
    const u64 bit = replicationBit<T>();
    return Mut<T>(ptr, bit ? repDirtyOf(e) : nullptr, bit, m_tick);
}

template <class T>
MutColumn<T> ChunkView::mutColumn(u32 term) const noexcept {
    const u64 bit = m_world ? m_world->replicationBit<T>() : 0;
    return MutColumn<T>(write<T>(term), bit ? m_data->repDirty : nullptr, bit, m_tick);
}

template <class T>
Mut<T> ChunkView::mut(u32 term, u32 row) const noexcept {
    return mutColumn<T>(term)[row];
}

template <class T>
const T* SystemContext::get(Entity e) const {
    const ComponentId cid = m_world->id<T>();
    HELIOS_ASSERT(declares(cid, false), "SystemContext::get<T>: T not declared by this system");
    return m_world->get<T>(e);
}

template <class T>
Mut<T> SystemContext::mut(Entity e) const {
    const ComponentId cid = m_world->id<T>();
    HELIOS_ASSERT(m_desc->singleJob || m_desc->exclusive, "random-access writes need a singleJob system");
    HELIOS_ASSERT(declares(cid, true), "SystemContext::mut<T>: T not declared as a write");
    if (!m_world->ownsId(e, cid)) return {};
    T* ptr = static_cast<T*>(m_world->getMutRaw(e, cid));
    const u64 bit = m_world->replicationBit<T>();
    RepDirty* rep = bit ? static_cast<RepDirty*>(m_world->getMutRaw(e, m_world->repDirtyId())) : nullptr;
    return Mut<T>(ptr, rep, bit, m_tick);
}

} // namespace helios::ecs
