#pragma once
// Deterministic system scheduling on the Helios JobSystem (02 §4.3, 04 §3.3).
//
// A system declares its stage, an optional order key and explicit `after` dependencies, the
// components it reads and writes (query terms plus random-access extras), and a function run per
// chunk (a contiguous row range of one flecs table). World::buildSchedule() then derives, per
// stage, one linear order — Kahn's algorithm over `after` edges with ties broken by (order, name),
// so it is independent of registration order — and a DAG with an edge i -> j (i before j in the
// linear order) whenever the two access sets conflict (write/read or write/write on a component)
// or either system is exclusive. Non-conflicting systems run in parallel; conflicting ones in the
// linear order; each system's chunks are split into jobs of ~chunkGrain rows. Structural changes go
// to per-job CommandBuffers applied at the stage's sync point in (system, job) order, so the result
// of a tick does not depend on the worker count (tests/test_scheduler.cpp checks bit-identical
// state for 0/1/2/4 workers).
//
// flecs itself is read-only while a stage runs: chunk lists are gathered on the calling thread
// before the stage starts; jobs only touch column memory and read components via const lookups.
//
// Threading: SystemDesc/SystemBuilder are built on the main thread before buildSchedule(); run
// functions are called from job workers (one chunk at a time per job).

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/command_buffer.h"
#include "helios/ecs/dirty.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

/// Cell tick stages (04 §3.3). Each stage ends with a sync point.
enum class Stage : u8 {
    Input = 0,
    PrePhysics,
    Physics,
    PostPhysics,
    AuthorityFlush,
    ReplicationGather,
    ConnectionWrite,
    Send,
    Count,
};
inline constexpr u32 kStageCount = static_cast<u32>(Stage::Count);
std::string_view stageName(Stage stage) noexcept;

enum class TermAccess : u8 {
    Read,          ///< Field; may resolve to a prefab's shared value (see ChunkView::isShared).
    Write,         ///< Field; always the entity's own storage.
    With,          ///< Filter: must have, no data access.
    Without,       ///< Filter: must not have.
    OptionalRead,  ///< Field that may be absent (ChunkView::has).
    OptionalWrite, ///< Owned field that may be absent.
};

struct Term {
    ComponentId id = 0;
    TermAccess access = TermAccess::Read;
};

struct UpdatePolicy {
    enum class Kind : u8 { EveryTick, EveryNTicks };
    Kind kind = Kind::EveryTick;
    u32 n = 1;
    u32 phase = 0;
    /// EveryNTicks only: run every tick but process only the jobs with (job % n) == (tick + phase) % n,
    /// spreading the cost over n ticks.
    bool staggered = false;

    static UpdatePolicy everyTick() noexcept { return {}; }
    static UpdatePolicy everyNTicks(u32 n, u32 phase = 0, bool staggered = false) noexcept {
        return {Kind::EveryNTicks, n == 0 ? 1 : n, phase, staggered};
    }
};

class World;
class SystemContext;
class ChunkView;

using ChunkFn = std::function<void(SystemContext&, ChunkView&)>;
using OnceFn = std::function<void(SystemContext&)>;

struct SystemDesc {
    std::string name;                     ///< Unique.
    Stage stage = Stage::PrePhysics;
    i32 order = 0;                        ///< Lower runs first among independent systems (tie: name).
    std::vector<std::string> after;       ///< Systems of the same stage that must finish first.
    std::vector<Term> terms;              ///< Query; empty = no query (use `once`).
    std::vector<ComponentId> extraReads;  ///< Random-access reads (SystemContext::get).
    std::vector<ComponentId> extraWrites; ///< Random-access writes (single-job systems only).
    u32 chunkGrain = 256;                 ///< Target rows per job.
    bool singleJob = false;               ///< All chunks in one job (required for random writes).
    bool exclusive = false;               ///< Conflicts with every other system of the stage.
    UpdatePolicy policy;
    f32 budgetUs = 0;                     ///< Reported in SystemStats (0 = none).
    ChunkFn each;                         ///< Called per chunk (query systems).
    OnceFn once;                          ///< Called once per run (before chunks, if both are set).
};

struct SystemStats {
    std::string name;
    Stage stage = Stage::PrePhysics;
    u64 runs = 0;
    u64 lastNs = 0;
    u64 maxNs = 0;
    f64 avgNs = 0;          ///< Exponential moving average (alpha 1/16).
    u32 lastRows = 0;
    u32 lastJobs = 0;
    u64 overBudgetRuns = 0;
};

/// Fluent SystemDesc builder: world.system("Move").stage(Stage::Physics).write<Pos>().read<Vel>()
///     .each([](SystemContext& ctx, ChunkView& chunk) { ... });
/// Registration happens in each()/once()/commit(); errors (duplicate names, unregistered types) are
/// reported through the returned Result and logged.
class SystemBuilder {
public:
    SystemBuilder(World& world, std::string name);

    SystemBuilder& stage(Stage s) { m_desc.stage = s; return *this; }
    SystemBuilder& order(i32 o) { m_desc.order = o; return *this; }
    SystemBuilder& after(std::string name) { m_desc.after.push_back(std::move(name)); return *this; }
    SystemBuilder& grain(u32 rows) { m_desc.chunkGrain = rows == 0 ? 1 : rows; return *this; }
    SystemBuilder& singleJob(bool v = true) { m_desc.singleJob = v; return *this; }
    SystemBuilder& exclusive(bool v = true) { m_desc.exclusive = v; return *this; }
    SystemBuilder& policy(UpdatePolicy p) { m_desc.policy = p; return *this; }
    SystemBuilder& budgetUs(f32 us) { m_desc.budgetUs = us; return *this; }
    SystemBuilder& term(ComponentId id, TermAccess access) { m_desc.terms.push_back({id, access}); return *this; }
    SystemBuilder& alsoReadsId(ComponentId id) { m_desc.extraReads.push_back(id); return *this; }
    SystemBuilder& alsoWritesId(ComponentId id) { m_desc.extraWrites.push_back(id); return *this; }

    template <class T> SystemBuilder& read() { return term(idOf<T>(), TermAccess::Read); }
    template <class T> SystemBuilder& write() { return term(idOf<T>(), TermAccess::Write); }
    template <class T> SystemBuilder& with() { return term(idOf<T>(), TermAccess::With); }
    template <class T> SystemBuilder& without() { return term(idOf<T>(), TermAccess::Without); }
    template <class T> SystemBuilder& optionalRead() { return term(idOf<T>(), TermAccess::OptionalRead); }
    template <class T> SystemBuilder& optionalWrite() { return term(idOf<T>(), TermAccess::OptionalWrite); }
    template <class T> SystemBuilder& alsoReads() { return alsoReadsId(idOf<T>()); }
    template <class T> SystemBuilder& alsoWrites() { return alsoWritesId(idOf<T>()); }

    Result<void> each(ChunkFn fn);
    Result<void> once(OnceFn fn);
    Result<void> commit();
    SystemDesc& desc() noexcept { return m_desc; }

private:
    template <class T>
    ComponentId idOf() const {
        return componentIdForSlot(*m_world, typeSlot<T>());
    }
    World* m_world;
    SystemDesc m_desc;
};

/// One contiguous row range of a single flecs table matched by a system's query. Queries with
/// Sparse or DontFragment terms yield one single-row chunk per entity (those components have no
/// table column).
struct ChunkData {
    static constexpr u32 kMaxFields = 16;
    /// flecs entity ids of the rows (table storage), or nullptr for a single-entity chunk whose id
    /// is `inlineEntity`. Use entityAt().
    const u64* entities = nullptr;
    u64 inlineEntity = 0;
    u32 count = 0;
    u32 sharedMask = 0;  ///< Field i points to a single (prefab) value.
    u32 presentMask = 0; ///< Field i is present (optional terms).
    void* fields[kMaxFields] = {};
    RepDirty* repDirty = nullptr; ///< Present when the system writes replicated components.

    u64 entityAt(u32 row) const noexcept { return entities ? entities[row] : inlineEntity; }
};

/// Typed view of a chunk handed to SystemDesc::each. Field indices are term indices.
class ChunkView {
public:
    ChunkView(const ChunkData& data, const SystemDesc* desc, const World* world, Tick tick) noexcept
        : m_data(&data), m_desc(desc), m_world(world), m_tick(tick) {}

    u32 count() const noexcept { return m_data->count; }
    Entity entity(u32 row) const noexcept { return Entity(m_data->entityAt(row)); }
    bool has(u32 term) const noexcept { return (m_data->presentMask >> term) & 1u; }
    bool isShared(u32 term) const noexcept { return (m_data->sharedMask >> term) & 1u; }

    /// Column pointer for a Read term; if isShared(term) it points to ONE value for all rows.
    template <class T>
    const T* read(u32 term) const noexcept {
        HELIOS_ASSERT(term < ChunkData::kMaxFields);
        return static_cast<const T*>(m_data->fields[term]);
    }
    /// Row value of a Read term, handling shared (prefab) values.
    template <class T>
    const T& at(u32 term, u32 row) const noexcept {
        const T* p = read<T>(term);
        return isShared(term) ? *p : p[row];
    }
    /// Column pointer for a Write term (owned storage, never shared).
    template <class T>
    T* write(u32 term) const noexcept {
        HELIOS_ASSERT(term < ChunkData::kMaxFields && !isShared(term));
        HELIOS_ASSERT(m_desc == nullptr || m_desc->terms[term].access == TermAccess::Write ||
                          m_desc->terms[term].access == TermAccess::OptionalWrite,
                      "write<T>() on a term not declared Write");
        return static_cast<T*>(m_data->fields[term]);
    }
    /// Dirty-tracking writers for a Write term on a replicated component. Fetch the column once per
    /// chunk (mutColumn) in hot loops; mut(term, row) is the convenience form.
    template <class T>
    MutColumn<T> mutColumn(u32 term) const noexcept;
    template <class T>
    Mut<T> mut(u32 term, u32 row) const noexcept;

private:
    const ChunkData* m_data;
    const SystemDesc* m_desc;
    const World* m_world;
    Tick m_tick;
};

/// Per-job context handed to system functions.
class SystemContext {
public:
    SystemContext(World& world, const SystemDesc& desc, CommandBuffer& commands, f32 dt, Tick tick,
                  u32 jobIndex) noexcept
        : m_world(&world), m_desc(&desc), m_commands(&commands), m_dt(dt), m_tick(tick), m_job(jobIndex) {}

    World& world() const noexcept { return *m_world; }
    const SystemDesc& desc() const noexcept { return *m_desc; }
    /// This job's command buffer (applied at the stage's sync point).
    CommandBuffer& commands() const noexcept { return *m_commands; }
    f32 dt() const noexcept { return m_dt; }
    Tick tick() const noexcept { return m_tick; }
    u32 jobIndex() const noexcept { return m_job; }

    /// Random-access read; T must be declared (term, extraReads or extraWrites) — checked in
    /// development builds. Returns nullptr if the entity lacks T.
    template <class T>
    const T* get(Entity e) const;
    /// Random-access dirty-tracking write; only for singleJob systems that declared extraWrites<T>.
    template <class T>
    Mut<T> mut(Entity e) const;

    /// True if `id` is in the system's declared read or write set.
    bool declares(ComponentId id, bool forWrite) const noexcept;

private:
    World* m_world;
    const SystemDesc* m_desc;
    CommandBuffer* m_commands;
    f32 m_dt;
    Tick m_tick;
    u32 m_job;
};

} // namespace helios::ecs
