#pragma once
// Private state of helios::ecs::World shared by world.cpp, scheduler.cpp and query.cpp.

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "flecs_internal.h"
#include "helios/core/hash.h"
#include "helios/core/jobs.h"
#include "helios/ecs/world.h"

namespace helios::ecs {

/// A flecs query built from Helios terms. Optional terms on DontFragment components are left out of
/// the flecs query (flecs 4.1.6 iterates them incorrectly) and resolved per entity by collectChunks.
struct QueryPlan {
    ecs_query_t* query = nullptr;
    int8_t field[ChunkData::kMaxFields] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int8_t repDirtyField = -1; ///< flecs field of the implicit optional RepDirty term, or -1.
    u32 lookupMask = 0;        ///< Terms resolved per entity (not in the flecs query).
};
static_assert(ChunkData::kMaxFields == 16);

/// Builds the plan for `terms` (plus an implicit optional RepDirty term when `withRepDirty`).
/// plan.query is null on error (logged).
QueryPlan createQueryPlan(World& world, const std::vector<Term>& terms, bool withRepDirty);
void destroyQueryPlan(QueryPlan& plan) noexcept;

/// Collects chunk data of `plan` (sizes per term from the world's registry). Each flecs table result
/// becomes one ChunkData; results with sparse fields or per-entity lookups become one ChunkData per
/// entity. Main thread only.
void collectChunks(World& world, const QueryPlan& plan, const std::vector<Term>& terms, std::vector<ChunkData>& out);

struct SystemRuntime {
    SystemDesc desc;
    u32 index = 0;
    QueryPlan plan;
    u32 termSizes[ChunkData::kMaxFields] = {};
    std::vector<ComponentId> reads;  // sorted, unique
    std::vector<ComponentId> writes; // sorted, unique

    // Schedule (rebuilt by buildSchedule).
    std::vector<u32> successors;
    u32 predecessorCount = 0;
    std::atomic<u32> pending{0};

    // Per-run data.
    struct JobRange {
        u32 firstSlice = 0;
        u32 endSlice = 0;
    };
    bool runsThisTick = false;
    std::vector<ChunkData> tables; // one per flecs result
    std::vector<ChunkData> slices; // tables split to <= grain rows
    std::vector<JobRange> jobs;
    std::vector<CommandBuffer> buffers;
    u32 rows = 0;

    SystemStats stats;
};

struct StageSchedule {
    std::vector<u32> order; // SystemRuntime indices in deterministic linear order
    std::vector<u32> roots;
    u64 lastSyncNs = 0;
};

struct World::Impl {
    explicit Impl(World& w) : self(w) {}

    World& self;

    // Components.
    std::deque<ComponentInfo> components; // stable addresses (hooks point at ComponentHooks below)
    std::deque<ComponentHooks> hooks;
    std::deque<std::vector<std::byte>> defaults;  // default value bytes of plain components (+ align slack)
    U64Map indexById{MemoryTag::Unknown, 64}; // ComponentId -> index + 1
    std::map<std::string, u32, std::less<>> indexByName;
    std::vector<u32> slotOfComponent;         // component index -> bound type slot (~0 = none)
    std::array<u32, kMaxReplicatedComponents> replicated{}; // replIndex -> component index
    u32 replicatedCount = 0;

    // Relationships.
    ReparentHook reparentHook;
    /// DockStorage::Field reverse index: host -> docked entities. Entries are validated (alive and
    /// DockRef still pointing at the host) and pruned lazily in dockedAt().
    std::unordered_map<u64, std::vector<u64>> dockIndex;
    /// Bloom filter over dockIndex's hosts: destroys skip the lookup for entities that never hosted
    /// a dock (bits are only set; rebuilt from dockIndex every kDockFilterRebuild host removals).
    std::array<u64, 64> dockHostFilter{};
    u32 dockHostsErased = 0;
    static constexpr u32 kDockFilterRebuild = 1024;
    static u64 dockFilterBit(u64 host) noexcept { return mix64(host) & 4095; }
    bool mayHostDocks(u64 host) const noexcept {
        const u64 b = dockFilterBit(host);
        return (dockHostFilter[b >> 6] >> (b & 63)) & 1;
    }
    void addDockHost(u64 host) noexcept {
        const u64 b = dockFilterBit(host);
        dockHostFilter[b >> 6] |= u64(1) << (b & 63);
    }

    // Command application scratch (reused; main thread only).
    std::vector<u32> fusedHead, fusedTail, fusedNext;
    std::vector<u8> fusedClosed, fused;
    std::vector<u32> lateOps, opIndex;
    std::vector<Entity> destroyScratch;
    std::vector<u64> typeScratch;
    std::vector<u32> spawnOpBegin;           // per spawn: first index into flatOps
    std::vector<u32> spawnOpEnd;             // per spawn: one past its last op
    std::vector<u32> runEnd;                 // per spawn: one past its fused run (contiguous fusion)
    std::vector<World::SpawnOp> flatOps;      // fused ops of every spawn, grouped per spawn
    std::vector<EntityId> spawnIds;           // allocated in command order
    std::vector<u32> mintTemps;               // temps whose EntityId is minted, in command order
    std::vector<EntityId> mintedIds;
    std::vector<u32> spawnGroup;              // per spawn: group index or ~0
    struct SpawnGroupData {
        std::vector<u32> members; // spawn indices in command order
        u32 opCount = 0;
        u64 nextSameKey = ~0ull;  // hash-collision chain
        bool created = false;
    };
    std::vector<SpawnGroupData> groups;
    U64Map groupByKey{MemoryTag::Unknown, 16};
    std::vector<ecs_entity_t> bulkEntities;
    std::vector<ecs_entity_t> bulkDropped;
    std::vector<std::byte*> groupColumns;
    std::vector<u32> groupSizes;
    std::vector<const ComponentHooks*> groupHooks;
    std::vector<u32> groupDirtyOffsets; // ~0u = not replicated
    std::vector<const World::SpawnOp*> groupMemberOps; // per member: its first flat op

    // Systems.
    std::vector<std::unique_ptr<SystemRuntime>> systems;
    std::map<std::string, u32, std::less<>> systemByName;
    std::array<StageSchedule, kStageCount> stages;
    bool scheduleDirty = true;
    jobs::Counter* stageCounter = nullptr;
    f32 stageDt = 0;

    // Change tracking.
    QueryPlan changeQuery;
    std::vector<std::vector<ComponentChange>> gatherScratch;

    // flecs native path.
    u32 flecsTaskThreads = 0;

    // Stats.
    u64 structuralOps = 0;
    u64 discarded = 0;
    bool finalizing = false;
};

} // namespace helios::ecs
