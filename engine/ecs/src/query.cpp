// Query construction, chunk collection and the replication change gather.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <memory>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/ecs/os_api.h"
#include "world_impl.h"

namespace helios::ecs {

QueryPlan createQueryPlan(World& world, const std::vector<Term>& terms, bool withRepDirty) {
    QueryPlan plan;
    const usize total = terms.size() + (withRepDirty ? 1 : 0);
    if (total == 0) return plan;
    if (total > ChunkData::kMaxFields) {
        HELIOS_LOG_ERROR(LogEcs, "query has {} terms; at most {} are supported", total, ChunkData::kMaxFields);
        return plan;
    }
    ecs_query_desc_t qd = {};
    int8_t field = 0;
    for (usize i = 0; i < terms.size(); ++i) {
        const bool optional = terms[i].access == TermAccess::OptionalRead || terms[i].access == TermAccess::OptionalWrite;
        const ComponentInfo* info = world.componentInfo(terms[i].id);
        if (optional && info && hasFlag(info->flags, ComponentFlags::DontFragment)) {
            // flecs 4.1.6 iterates optional non-fragmenting terms incorrectly (entities lacking the
            // component are skipped in some tables and repeated in others): resolve per entity.
            plan.lookupMask |= 1u << i;
            continue;
        }
        ecs_term_t& t = qd.terms[field];
        plan.field[i] = field++;
        t.id = terms[i].id;
        switch (terms[i].access) {
        case TermAccess::Read: t.inout = EcsIn; break;
        case TermAccess::Write:
            t.inout = EcsInOut;
            t.src.id = EcsSelf; // writes never target a prefab's shared value
            break;
        case TermAccess::With: t.inout = EcsInOutNone; break;
        case TermAccess::Without:
            t.inout = EcsInOutNone;
            t.oper = EcsNot;
            break;
        case TermAccess::OptionalRead:
            t.inout = EcsIn;
            t.oper = EcsOptional;
            break;
        case TermAccess::OptionalWrite:
            t.inout = EcsInOut;
            t.oper = EcsOptional;
            t.src.id = EcsSelf;
            break;
        }
    }
    if (withRepDirty) {
        ecs_term_t& t = qd.terms[field];
        t.id = world.repDirtyId();
        t.inout = EcsInOut;
        t.oper = EcsOptional;
        t.src.id = EcsSelf;
        plan.repDirtyField = field++;
    }
    if (field == 0) {
        HELIOS_LOG_ERROR(LogEcs, "query has no term flecs can iterate (only optional non-fragmenting terms)");
        return plan;
    }
    qd.cache_kind = EcsQueryCacheAuto;
    plan.query = ecs_query_init(world.flecsWorld(), &qd);
    if (!plan.query) HELIOS_LOG_ERROR(LogEcs, "ecs_query_init failed");
    return plan;
}

void destroyQueryPlan(QueryPlan& plan) noexcept {
    if (plan.query) ecs_query_fini(plan.query);
    plan.query = nullptr;
}

void collectChunks(World& world, const QueryPlan& plan, const std::vector<Term>& terms, std::vector<ChunkData>& out) {
    if (!plan.query) return;
    ecs_world_t* const fw = world.flecsWorld();
    u32 sizes[ChunkData::kMaxFields] = {};
    for (usize i = 0; i < terms.size(); ++i) {
        const bool data = terms[i].access != TermAccess::With && terms[i].access != TermAccess::Without;
        const ComponentInfo* info = data ? world.componentInfo(terms[i].id) : nullptr;
        sizes[i] = info ? info->size : 0;
    }
    const u32 termCount = static_cast<u32>(terms.size());
    ecs_iter_t it = ecs_query_iter(fw, plan.query);
    while (ecs_query_next(&it)) {
        if (it.count == 0) continue;
        RepDirty* rep = nullptr;
        if (plan.repDirtyField >= 0 && ecs_field_is_set(&it, plan.repDirtyField)) {
            rep = static_cast<RepDirty*>(ecs_field_w_size(&it, sizeof(RepDirty), plan.repDirtyField));
        }
        // Sparse and DontFragment components have no table column: flecs marks them in row_fields
        // and they must be fetched per entity with ecs_field_at (ecs_field on them reads a bogus
        // "shared" source and crashes). Such results become one single-row chunk per entity, as do
        // results with per-entity lookups (plan.lookupMask). flecs also yields table-less
        // single-entity results (non-fragmenting $this) whose it.entities points into the iterator
        // itself, which the next ecs_query_next overwrites: those ids are copied into the chunk
        // (ChunkData::inlineEntity).
        const u64 rowFields = static_cast<u64>(it.row_fields);
        if (rowFields == 0 && plan.lookupMask == 0 && it.table) {
            ChunkData& c = out.emplace_back();
            c.entities = reinterpret_cast<const u64*>(it.entities);
            c.count = static_cast<u32>(it.count);
            for (u32 f = 0; f < termCount; ++f) {
                const int8_t fi = plan.field[f];
                if (!ecs_field_is_set(&it, fi)) continue;
                c.presentMask |= 1u << f;
                if (sizes[f] == 0) continue;
                c.fields[f] = ecs_field_w_size(&it, sizes[f], fi);
                if (!ecs_field_is_self(&it, fi)) c.sharedMask |= 1u << f;
            }
            c.repDirty = rep;
            continue;
        }
        for (int32_t row = 0; row < it.count; ++row) {
            ChunkData& c = out.emplace_back();
            if (it.table) {
                c.entities = reinterpret_cast<const u64*>(it.entities) + row; // table storage: stable
            } else {
                c.inlineEntity = static_cast<u64>(it.entities[row]);
            }
            c.count = 1;
            const ecs_entity_t e = it.entities[row];
            for (u32 f = 0; f < termCount; ++f) {
                if ((plan.lookupMask >> f) & 1u) {
                    const ComponentId id = terms[f].id;
                    const bool write = terms[f].access == TermAccess::OptionalWrite;
                    if (!(write ? ecs_owns_id(fw, e, id) : ecs_has_id(fw, e, id))) continue;
                    c.presentMask |= 1u << f;
                    if (sizes[f] == 0) continue;
                    c.fields[f] = write ? ecs_get_mut_id(fw, e, id) : const_cast<void*>(ecs_get_id(fw, e, id));
                    if (!write && !ecs_owns_id(fw, e, id)) c.sharedMask |= 1u << f;
                    continue;
                }
                const int8_t fi = plan.field[f];
                if (!ecs_field_is_set(&it, fi)) continue;
                if ((rowFields >> fi) & 1u) {
                    if (sizes[f] == 0) {
                        c.presentMask |= 1u << f;
                        continue;
                    }
                    // Optional sparse fields are absent per entity: presence follows the pointer.
                    void* ptr = ecs_field_at_w_size(&it, sizes[f], fi, row);
                    if (!ptr) continue;
                    c.presentMask |= 1u << f;
                    c.fields[f] = ptr;
                    if (!ecs_field_is_self(&it, fi)) c.sharedMask |= 1u << f;
                    continue;
                }
                c.presentMask |= 1u << f;
                if (sizes[f] == 0) continue;
                auto* base = static_cast<std::byte*>(ecs_field_w_size(&it, sizes[f], fi));
                if (ecs_field_is_self(&it, fi)) {
                    c.fields[f] = base + static_cast<usize>(row) * sizes[f];
                } else {
                    c.fields[f] = base;
                    c.sharedMask |= 1u << f;
                }
            }
            c.repDirty = rep ? rep + row : nullptr;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------------------------

Query::Query(World& world, std::vector<Term> terms) : m_world(&world), m_terms(std::move(terms)) {
    auto plan = std::make_unique<QueryPlan>(createQueryPlan(world, m_terms, false));
    if (plan->query) m_query = plan.release();
}

Query::~Query() {
    if (auto* plan = static_cast<QueryPlan*>(m_query)) {
        destroyQueryPlan(*plan);
        delete plan;
    }
}

Query::Query(Query&& other) noexcept
    : m_world(std::exchange(other.m_world, nullptr)), m_query(std::exchange(other.m_query, nullptr)),
      m_terms(std::move(other.m_terms)) {}

Query& Query::operator=(Query&& other) noexcept {
    if (this != &other) {
        if (auto* plan = static_cast<QueryPlan*>(m_query)) {
            destroyQueryPlan(*plan);
            delete plan;
        }
        m_world = std::exchange(other.m_world, nullptr);
        m_query = std::exchange(other.m_query, nullptr);
        m_terms = std::move(other.m_terms);
    }
    return *this;
}

void Query::collect(std::vector<ChunkData>& out) const {
    if (m_query) collectChunks(*m_world, *static_cast<const QueryPlan*>(m_query), m_terms, out);
}

void Query::forEachChunk(const std::function<void(ChunkView&)>& fn) const {
    std::vector<ChunkData> chunks;
    collect(chunks);
    for (const ChunkData& c : chunks) {
        ChunkView view(c, nullptr, m_world, m_world->currentTick());
        fn(view);
    }
}

u32 Query::count() const {
    if (!m_query) return 0;
    const auto* plan = static_cast<const QueryPlan*>(m_query);
    if (plan->lookupMask != 0) {
        // Optional per-entity terms never filter, but flecs' count would include the dropped term's
        // quirks: count what collect() yields instead.
        std::vector<ChunkData> chunks;
        collect(chunks);
        u32 n = 0;
        for (const ChunkData& c : chunks) n += c.count;
        return n;
    }
    return static_cast<u32>(ecs_query_count(plan->query).entities);
}

// ---------------------------------------------------------------------------------------------
// Change gather (02 §4.4 / 04 §4.2 step 1)
// ---------------------------------------------------------------------------------------------

namespace {

struct GatherTable {
    RepDirty* rep = nullptr;
    const NetIdentity* ids = nullptr;
    const u64* entities = nullptr;
    u32 count = 0;
    std::byte* columns[kMaxReplicatedComponents] = {};
};

} // namespace

void World::gatherChanges(ChangeList& out) {
    assertNotInStage();
    out.clear();
    out.tick = m_tick;
    Impl& impl = *m_impl;
    if (!impl.changeQuery.query || impl.replicatedCount == 0) return;

    // 1. Table list + column pointers of every replicated component (main thread).
    std::vector<GatherTable> tables;
    u32 totalRows = 0;
    ecs_iter_t it = ecs_query_iter(m_flecs, impl.changeQuery.query);
    while (ecs_query_next(&it)) {
        if (it.count == 0) continue;
        GatherTable& t = tables.emplace_back();
        t.rep = static_cast<RepDirty*>(ecs_field_w_size(&it, sizeof(RepDirty), 0));
        t.ids = static_cast<const NetIdentity*>(ecs_field_w_size(&it, sizeof(NetIdentity), 1));
        t.entities = reinterpret_cast<const u64*>(it.entities);
        t.count = static_cast<u32>(it.count);
        totalRows += t.count;
        if (it.table) {
            for (u32 r = 0; r < impl.replicatedCount; ++r) {
                const ComponentInfo& info = impl.components[impl.replicated[r]];
                const int32_t col = ecs_table_get_column_index(m_flecs, it.table, info.id);
                if (col >= 0) t.columns[r] = static_cast<std::byte*>(ecs_table_get_column(it.table, col, it.offset));
            }
        }
    }

    // 2. Scan rows; per-table outputs keep the final order independent of the job partition.
    if (impl.gatherScratch.size() < tables.size()) impl.gatherScratch.resize(tables.size());
    std::atomic<u32> entityCount{0};
    auto scanTable = [&](u64 ti) {
        GatherTable& t = tables[ti];
        std::vector<ComponentChange>& dst = impl.gatherScratch[ti];
        dst.clear();
        u32 dirtyEntities = 0;
        for (u32 row = 0; row < t.count; ++row) {
            u64 mask = t.rep[row].componentMask;
            if (mask == 0) continue;
            const usize before = dst.size();
            while (mask) {
                const u32 r = static_cast<u32>(std::countr_zero(mask));
                mask &= mask - 1;
                if (r >= impl.replicatedCount) continue;
                const ComponentInfo& info = impl.components[impl.replicated[r]];
                std::byte* base = t.columns[r] ? t.columns[r] + static_cast<usize>(row) * info.size : nullptr;
                if (!base) {
                    // Sparse / DontFragment storage (not a table column): fall back to a lookup.
                    base = static_cast<std::byte*>(ecs_get_mut_id(m_flecs, t.entities[row], info.id));
                    if (!base) continue; // removed after it was dirtied: the structural log covers it
                }
                auto* dirty = reinterpret_cast<FieldMask*>(base + info.dirtyOffset);
                const FieldMask fields = *dirty;
                *dirty = 0;
                if (fields != 0) dst.push_back(ComponentChange{t.ids[row].id, t.ids[row].handle, static_cast<u8>(r), fields});
            }
            t.rep[row] = RepDirty{};
            if (dst.size() != before) ++dirtyEntities; // "changed" = at least one field reported
        }
        entityCount.fetch_add(dirtyEntities, std::memory_order_relaxed);
    };
    const bool parallel = m_jobs != nullptr && totalRows >= 8192 && tables.size() > 1;
    if (parallel) {
        m_jobs->parallelFor(0, tables.size(), 1, [&](u64 ti) { scanTable(ti); });
    } else {
        for (u64 ti = 0; ti < tables.size(); ++ti) scanTable(ti);
    }

    usize total = 0;
    for (usize ti = 0; ti < tables.size(); ++ti) total += impl.gatherScratch[ti].size();
    out.changes.reserve(total);
    for (usize ti = 0; ti < tables.size(); ++ti) {
        out.changes.insert(out.changes.end(), impl.gatherScratch[ti].begin(), impl.gatherScratch[ti].end());
    }
    out.entityCount = entityCount.load(std::memory_order_relaxed);
}

} // namespace helios::ecs
