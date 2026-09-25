// Deterministic system scheduler (02 §4.3). See system.h for the contract.

#include <algorithm>
#include <queue>
#include <tuple>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/ecs/os_api.h"
#include "world_impl.h"

namespace helios::ecs {

std::string_view stageName(Stage stage) noexcept {
    switch (stage) {
    case Stage::Input: return "Input";
    case Stage::PrePhysics: return "PrePhysics";
    case Stage::Physics: return "Physics";
    case Stage::PostPhysics: return "PostPhysics";
    case Stage::AuthorityFlush: return "AuthorityFlush";
    case Stage::ReplicationGather: return "ReplicationGather";
    case Stage::ConnectionWrite: return "ConnectionWrite";
    case Stage::Send: return "Send";
    case Stage::Count: break;
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// SystemBuilder / SystemContext
// ---------------------------------------------------------------------------------------------

SystemBuilder::SystemBuilder(World& world, std::string name) : m_world(&world) { m_desc.name = std::move(name); }

Result<void> SystemBuilder::each(ChunkFn fn) {
    m_desc.each = std::move(fn);
    return commit();
}

Result<void> SystemBuilder::once(OnceFn fn) {
    m_desc.once = std::move(fn);
    return commit();
}

Result<void> SystemBuilder::commit() {
    Result<void> r = m_world->addSystem(m_desc);
    if (!r) HELIOS_LOG_ERROR(LogEcs, "system '{}': {}", m_desc.name, r.error());
    return r;
}

bool SystemContext::declares(ComponentId id, bool forWrite) const noexcept {
    for (const Term& t : m_desc->terms) {
        if (t.id != id) continue;
        const bool write = t.access == TermAccess::Write || t.access == TermAccess::OptionalWrite;
        const bool read = t.access == TermAccess::Read || t.access == TermAccess::OptionalRead;
        if (write || (!forWrite && read)) return true;
    }
    for (ComponentId w : m_desc->extraWrites) {
        if (w == id) return true;
    }
    if (!forWrite) {
        for (ComponentId r : m_desc->extraReads) {
            if (r == id) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Registration and schedule
// ---------------------------------------------------------------------------------------------

namespace {

bool intersects(const std::vector<ComponentId>& a, const std::vector<ComponentId>& b) {
    usize i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) return true;
        if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return false;
}

bool conflicts(const SystemRuntime& a, const SystemRuntime& b) {
    if (a.desc.exclusive || b.desc.exclusive) return true;
    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads) || intersects(b.writes, a.reads);
}

void sortUnique(std::vector<ComponentId>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

} // namespace

Result<void> World::addSystem(SystemDesc desc) {
    assertNotInStage();
    Impl& impl = *m_impl;
    if (desc.name.empty()) return Error{ErrorCode::InvalidArgument, "system name is empty"};
    if (impl.systemByName.contains(desc.name)) {
        return makeError(ErrorCode::AlreadyExists, "system '{}' already exists", desc.name);
    }
    if (desc.stage >= Stage::Count) return Error{ErrorCode::InvalidArgument, "invalid stage"};
    if (!desc.each && !desc.once) return makeError(ErrorCode::InvalidArgument, "system '{}' has no function", desc.name);
    if (desc.each && desc.terms.empty()) {
        return makeError(ErrorCode::InvalidArgument, "system '{}' has an each() function but no query terms", desc.name);
    }
    auto rt = std::make_unique<SystemRuntime>();
    bool writesReplicated = false;
    for (const Term& t : desc.terms) {
        if (t.id == 0) return makeError(ErrorCode::InvalidArgument, "system '{}' uses an unregistered component", desc.name);
        switch (t.access) {
        case TermAccess::Read:
        case TermAccess::OptionalRead: rt->reads.push_back(t.id); break;
        case TermAccess::Write:
        case TermAccess::OptionalWrite: {
            rt->writes.push_back(t.id);
            const ComponentInfo* info = componentInfo(t.id);
            writesReplicated = writesReplicated || (info && info->isReplicated());
            break;
        }
        case TermAccess::With:
        case TermAccess::Without: break;
        }
    }
    for (ComponentId id : desc.extraReads) {
        if (id == 0) return makeError(ErrorCode::InvalidArgument, "system '{}' reads an unregistered component", desc.name);
        rt->reads.push_back(id);
    }
    for (ComponentId id : desc.extraWrites) {
        if (id == 0) return makeError(ErrorCode::InvalidArgument, "system '{}' writes an unregistered component", desc.name);
        if (!desc.singleJob && !desc.exclusive) {
            return makeError(ErrorCode::InvalidArgument, "system '{}': random-access writes need singleJob", desc.name);
        }
        rt->writes.push_back(id);
    }
    sortUnique(rt->reads);
    sortUnique(rt->writes);
    if (!desc.terms.empty()) {
        rt->plan = createQueryPlan(*this, desc.terms, writesReplicated);
        if (!rt->plan.query) return makeError(ErrorCode::InvalidArgument, "system '{}': invalid query", desc.name);
    }
    for (usize i = 0; i < desc.terms.size(); ++i) {
        const bool data = desc.terms[i].access != TermAccess::With && desc.terms[i].access != TermAccess::Without;
        const ComponentInfo* info = data ? componentInfo(desc.terms[i].id) : nullptr;
        rt->termSizes[i] = info ? info->size : 0;
    }
    rt->index = static_cast<u32>(impl.systems.size());
    rt->stats.name = desc.name;
    rt->stats.stage = desc.stage;
    rt->desc = std::move(desc);
    impl.systemByName.emplace(rt->desc.name, rt->index);
    impl.systems.push_back(std::move(rt));
    impl.scheduleDirty = true;
    return {};
}

Result<void> World::buildSchedule() {
    Impl& impl = *m_impl;
    if (!impl.scheduleDirty) return {};
    for (StageSchedule& st : impl.stages) {
        st.order.clear();
        st.roots.clear();
    }
    for (auto& s : impl.systems) {
        s->successors.clear();
        s->predecessorCount = 0;
    }
    for (u32 stage = 0; stage < kStageCount; ++stage) {
        std::vector<u32> members;
        for (auto& s : impl.systems) {
            if (static_cast<u32>(s->desc.stage) == stage) members.push_back(s->index);
        }
        if (members.empty()) continue;
        // Explicit `after` edges (same stage only; earlier stages are implicitly before).
        std::vector<std::vector<u32>> next(impl.systems.size());
        std::vector<u32> indeg(impl.systems.size(), 0);
        for (u32 m : members) {
            for (const std::string& dep : impl.systems[m]->desc.after) {
                auto it = impl.systemByName.find(dep);
                if (it == impl.systemByName.end()) {
                    return makeError(ErrorCode::NotFound, "system '{}' runs after unknown system '{}'",
                                     impl.systems[m]->desc.name, dep);
                }
                const SystemRuntime& d = *impl.systems[it->second];
                if (d.desc.stage > impl.systems[m]->desc.stage) {
                    return makeError(ErrorCode::InvalidArgument, "system '{}' runs after '{}' of a later stage",
                                     impl.systems[m]->desc.name, dep);
                }
                if (d.desc.stage < impl.systems[m]->desc.stage) continue;
                next[d.index].push_back(m);
                ++indeg[m];
            }
        }
        // Kahn's algorithm with (order, name) tie-breaking: independent of registration order.
        auto key = [&](u32 i) { return std::tie(impl.systems[i]->desc.order, impl.systems[i]->desc.name); };
        auto greater = [&](u32 a, u32 b) { return key(a) > key(b); };
        std::priority_queue<u32, std::vector<u32>, decltype(greater)> ready(greater);
        for (u32 m : members) {
            if (indeg[m] == 0) ready.push(m);
        }
        std::vector<u32>& order = impl.stages[stage].order;
        while (!ready.empty()) {
            const u32 m = ready.top();
            ready.pop();
            order.push_back(m);
            for (u32 n : next[m]) {
                if (--indeg[n] == 0) ready.push(n);
            }
        }
        if (order.size() != members.size()) {
            order.clear();
            return makeError(ErrorCode::InvalidState, "cyclic `after` dependencies in stage {}",
                             stageName(static_cast<Stage>(stage)));
        }
        // DAG edges: i before j in the linear order and (conflict or explicit dependency).
        for (usize j = 0; j < order.size(); ++j) {
            SystemRuntime& b = *impl.systems[order[j]];
            for (usize i = 0; i < j; ++i) {
                SystemRuntime& a = *impl.systems[order[i]];
                bool edge = conflicts(a, b);
                if (!edge) {
                    for (const std::string& dep : b.desc.after) edge = edge || dep == a.desc.name;
                }
                if (edge) {
                    a.successors.push_back(b.index);
                    ++b.predecessorCount;
                }
            }
        }
        for (u32 m : order) {
            if (impl.systems[m]->predecessorCount == 0) impl.stages[stage].roots.push_back(m);
        }
    }
    impl.scheduleDirty = false;
    return {};
}

std::vector<std::string> World::executionOrder(Stage stage) {
    std::vector<std::string> names;
    if (!buildSchedule() || stage >= Stage::Count) return names;
    for (u32 i : m_impl->stages[static_cast<u32>(stage)].order) names.push_back(m_impl->systems[i]->desc.name);
    return names;
}

std::vector<std::pair<std::string, std::string>> World::scheduleEdges(Stage stage) {
    std::vector<std::pair<std::string, std::string>> edges;
    if (!buildSchedule() || stage >= Stage::Count) return edges;
    for (u32 i : m_impl->stages[static_cast<u32>(stage)].order) {
        const SystemRuntime& s = *m_impl->systems[i];
        for (u32 succ : s.successors) edges.emplace_back(s.desc.name, m_impl->systems[succ]->desc.name);
    }
    return edges;
}

// ---------------------------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------------------------

namespace {

bool shouldRun(const SystemDesc& d, Tick tick) {
    if (d.policy.kind == UpdatePolicy::Kind::EveryTick || d.policy.staggered) return true;
    return (tick + d.policy.phase) % d.policy.n == 0;
}

/// Splits matched tables into slices of <= grain rows and groups them into jobs of ~grain rows.
/// The partition depends only on the chunk list and the grain (never on the worker count).
void partition(SystemRuntime& s, Tick tick) {
    s.slices.clear();
    s.jobs.clear();
    s.rows = 0;
    const u32 grain = s.desc.singleJob ? ~0u : std::max<u32>(1, s.desc.chunkGrain);
    SystemRuntime::JobRange job{0, 0};
    u32 jobRows = 0;
    for (const ChunkData& table : s.tables) {
        u32 offset = 0;
        while (offset < table.count) {
            const u32 take = std::min(table.count - offset, grain - jobRows);
            ChunkData slice = table;
            slice.count = take;
            if (table.entities) slice.entities = table.entities + offset; // else: single inline entity
            for (u32 f = 0; f < ChunkData::kMaxFields; ++f) {
                if (!table.fields[f] || ((table.sharedMask >> f) & 1u)) continue; // shared: one value
                slice.fields[f] = static_cast<std::byte*>(table.fields[f]) + static_cast<usize>(offset) * s.termSizes[f];
            }
            if (table.repDirty) slice.repDirty = table.repDirty + offset;
            s.slices.push_back(slice);
            offset += take;
            jobRows += take;
            s.rows += take;
            if (jobRows >= grain) {
                job.endSlice = static_cast<u32>(s.slices.size());
                s.jobs.push_back(job);
                job.firstSlice = job.endSlice;
                jobRows = 0;
            }
        }
    }
    if (jobRows > 0 || (s.jobs.empty() && s.desc.once)) {
        job.endSlice = static_cast<u32>(s.slices.size());
        s.jobs.push_back(job);
    }
    if (s.desc.policy.kind == UpdatePolicy::Kind::EveryNTicks && s.desc.policy.staggered && s.desc.policy.n > 1) {
        const u32 n = s.desc.policy.n;
        const u32 phase = static_cast<u32>((tick + s.desc.policy.phase) % n);
        std::vector<SystemRuntime::JobRange> kept;
        // `once` runs every tick in job 0, but job 0's chunks follow the stagger like every other
        // job's: out of phase, `once` gets a job of its own with no chunks.
        if (s.desc.once && phase != 0) kept.push_back(SystemRuntime::JobRange{0, 0});
        for (u32 j = 0; j < s.jobs.size(); ++j) {
            if (j % n == phase) kept.push_back(s.jobs[j]);
        }
        s.jobs = std::move(kept);
    }
}

} // namespace

/// Runs system work; lives in a struct so it can reach World internals via Impl.
struct SchedulerRun {
    static void runJob(World& world, SystemRuntime& s, u32 job, f32 dt, Tick tick) {
        CommandBuffer& buffer = s.buffers[job];
        SystemContext ctx(world, s.desc, buffer, dt, tick, job);
        const SystemRuntime::JobRange& range = s.jobs[job];
        if (job == 0 && s.desc.once) s.desc.once(ctx);
        if (!s.desc.each) return;
        for (u32 i = range.firstSlice; i < range.endSlice; ++i) {
            ChunkView view(s.slices[i], &s.desc, &world, tick);
            s.desc.each(ctx, view);
        }
    }

    static void runSystem(World& world, SystemRuntime& s, f32 dt) {
        const Tick tick = world.currentTick();
        const Stopwatch sw;
        jobs::JobSystem* js = world.jobs();
        const u32 jobCount = static_cast<u32>(s.jobs.size());
        if (!js || jobCount <= 1) {
            for (u32 j = 0; j < jobCount; ++j) runJob(world, s, j, dt, tick);
        } else {
            jobs::Counter counter;
            World* w = &world;
            SystemRuntime* sp = &s;
            for (u32 j = 1; j < jobCount; ++j) {
                js->run([w, sp, j, dt, tick] { runJob(*w, *sp, j, dt, tick); }, &counter);
            }
            runJob(world, s, 0, dt, tick);
            js->wait(counter);
        }
        const u64 ns = sw.elapsedNanos();
        SystemStats& st = s.stats;
        ++st.runs;
        st.lastNs = ns;
        st.maxNs = std::max(st.maxNs, ns);
        st.avgNs = st.runs == 1 ? static_cast<f64>(ns) : st.avgNs + (static_cast<f64>(ns) - st.avgNs) / 16.0;
        st.lastRows = s.rows;
        st.lastJobs = jobCount;
        if (s.desc.budgetUs > 0 && static_cast<f64>(ns) > static_cast<f64>(s.desc.budgetUs) * 1000.0) ++st.overBudgetRuns;
    }

    static void runNode(World& world, World::Impl& impl, u32 index) {
        SystemRuntime& s = *impl.systems[index];
        if (s.runsThisTick) runSystem(world, s, impl.stageDt);
        for (u32 succ : s.successors) {
            SystemRuntime& n = *impl.systems[succ];
            if (n.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                World* w = &world;
                World::Impl* ip = &impl;
                world.jobs()->run([w, ip, succ] { runNode(*w, *ip, succ); }, impl.stageCounter, jobs::Priority::High);
            }
        }
    }
};

Result<void> World::runStage(Stage stage, f32 dt) {
    assertNotInStage();
    HELIOS_TRY(buildSchedule());
    if (stage >= Stage::Count) return Error{ErrorCode::InvalidArgument, "invalid stage"};
    Impl& impl = *m_impl;
    StageSchedule& st = impl.stages[static_cast<u32>(stage)];
    if (st.order.empty()) return {};

    // 1. Gather chunk lists and partition into jobs (main thread; flecs is not touched afterwards
    //    until the sync point).
    for (u32 i : st.order) {
        SystemRuntime& s = *impl.systems[i];
        s.runsThisTick = shouldRun(s.desc, m_tick);
        s.tables.clear();
        if (!s.runsThisTick) {
            s.jobs.clear();
            continue;
        }
        if (s.plan.query) collectChunks(*this, s.plan, s.desc.terms, s.tables);
        partition(s, m_tick);
        if (s.buffers.size() < s.jobs.size()) {
            const usize old = s.buffers.size();
            s.buffers.resize(s.jobs.size());
            for (usize b = old; b < s.buffers.size(); ++b) s.buffers[b].setWorld(this);
        }
    }

    // 2. Execute.
    impl.stageDt = dt;
    m_inStage = true;
    if (!m_jobs || m_jobs->workerCount() == 0) {
        for (u32 i : st.order) {
            SystemRuntime& s = *impl.systems[i];
            if (s.runsThisTick) SchedulerRun::runSystem(*this, s, dt);
        }
    } else {
        for (u32 i : st.order) {
            SystemRuntime& s = *impl.systems[i];
            s.pending.store(s.predecessorCount, std::memory_order_relaxed);
        }
        jobs::Counter stageCounter;
        impl.stageCounter = &stageCounter;
        World* w = this;
        Impl* ip = &impl;
        for (u32 root : st.roots) {
            m_jobs->run([w, ip, root] { SchedulerRun::runNode(*w, *ip, root); }, &stageCounter, jobs::Priority::High);
        }
        m_jobs->wait(stageCounter);
        impl.stageCounter = nullptr;
    }
    m_inStage = false;

    // 3. Sync point: apply every job's buffer in (system order, job index) order.
    const Stopwatch sync;
    std::vector<CommandBuffer*> buffers;
    for (u32 i : st.order) {
        SystemRuntime& s = *impl.systems[i];
        if (!s.runsThisTick) continue;
        for (usize j = 0; j < s.jobs.size(); ++j) {
            if (!s.buffers[j].empty()) buffers.push_back(&s.buffers[j]);
        }
    }
    apply(std::span<CommandBuffer* const>(buffers.data(), buffers.size()));
    st.lastSyncNs = sync.elapsedNanos();
    return {};
}

Result<void> World::tick(f32 dt) {
    HELIOS_TRY(buildSchedule());
    beginTick();
    for (u32 s = 0; s < kStageCount; ++s) HELIOS_TRY(runStage(static_cast<Stage>(s), dt));
    return {};
}

const SystemStats* World::systemStats(std::string_view name) const noexcept {
    auto it = m_impl->systemByName.find(name);
    return it != m_impl->systemByName.end() ? &m_impl->systems[it->second]->stats : nullptr;
}

std::vector<SystemStats> World::allSystemStats() const {
    std::vector<SystemStats> out;
    for (const auto& s : m_impl->systems) out.push_back(s->stats);
    return out;
}

u64 World::lastSyncNs(Stage stage) const noexcept {
    return stage < Stage::Count ? m_impl->stages[static_cast<u32>(stage)].lastSyncNs : 0;
}

} // namespace helios::ecs
