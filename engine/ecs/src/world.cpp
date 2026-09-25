#include "helios/ecs/world.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "helios/core/assert.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/os_api.h"
#include "world_impl.h"

namespace helios::ecs {

using detail::fe;
using detail::he;

namespace {

// flecs hook trampolines: the type info's binding_ctx is our ComponentHooks.
const ComponentHooks& hooksOf(const ecs_type_info_t* ti) {
    return *static_cast<const ComponentHooks*>(ti->hooks.binding_ctx);
}
void ctorTrampoline(void* ptr, int32_t count, const ecs_type_info_t* ti) { hooksOf(ti).construct(ptr, count); }
void dtorTrampoline(void* ptr, int32_t count, const ecs_type_info_t* ti) { hooksOf(ti).destruct(ptr, count); }
void copyTrampoline(void* dst, const void* src, int32_t count, const ecs_type_info_t* ti) {
    hooksOf(ti).copy(dst, src, count);
}
void moveTrampoline(void* dst, void* src, int32_t count, const ecs_type_info_t* ti) {
    hooksOf(ti).move(dst, src, count);
}

void collectRelated(ecs_world_t* w, ecs_entity_t rel, ecs_entity_t target, std::vector<Entity>& out) {
    const usize first = out.size();
    ecs_iter_t it = ecs_children_w_rel(w, rel, target);
    while (ecs_children_next(&it)) {
        for (int32_t i = 0; i < it.count; ++i) out.push_back(he(it.entities[i]));
    }
    std::sort(out.begin() + static_cast<isize>(first), out.end());
}

std::string sanitizeName(std::string_view name) {
    std::string out(name);
    for (std::string_view anon : {std::string_view("(anonymous namespace)"), std::string_view("`anonymous namespace'")}) {
        for (usize pos = out.find(anon); pos != std::string::npos; pos = out.find(anon)) out.replace(pos, anon.size(), "anon");
    }
    return out;
}

} // namespace

std::string_view structuralOpName(StructuralOp op) noexcept {
    switch (op) {
    case StructuralOp::Create: return "Create";
    case StructuralOp::Destroy: return "Destroy";
    case StructuralOp::Add: return "Add";
    case StructuralOp::Remove: return "Remove";
    case StructuralOp::SetParent: return "SetParent";
    case StructuralOp::ClearParent: return "ClearParent";
    case StructuralOp::SetFrame: return "SetFrame";
    case StructuralOp::Dock: return "Dock";
    case StructuralOp::Undock: return "Undock";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

namespace {
NetHandleTable::Desc handleDesc(NetHandleTable::Desc d) {
    if (d.tag == MemoryTag::Unknown) d.tag = ecsMemoryTag();
    return d;
}

std::unique_ptr<LocalIdBlockSource> makeLocalIdBlocks(const WorldDesc& desc) {
    if (desc.idBlocks) return nullptr;
    return std::make_unique<LocalIdBlockSource>(LocalIdBlockSource::Desc{desc.idClock, desc.idLastPrefix});
}

EntityIdMinter::Desc minterDesc(const WorldDesc& desc, LocalIdBlockSource* local) {
    EntityIdMinter::Desc d;
    d.shard = desc.shard;
    d.source = desc.idBlocks ? desc.idBlocks : local;
    d.hold = desc.idHoldBlocks;
    d.clock = desc.idClock;
    return d;
}
} // namespace

World::World(const WorldDesc& desc)
    : m_desc(desc), m_jobs(desc.jobs), m_localIdBlocks(makeLocalIdBlocks(desc)),
      m_ids(minterDesc(desc, m_localIdBlocks.get())), m_registry(handleDesc(desc.handles), ecsMemoryTag()),
      m_impl(std::make_unique<Impl>(*this)) {
    installFlecsOsApi();
    m_impl->indexById = U64Map(ecsMemoryTag(), 64);

    m_flecs = ecs_init();
    HELIOS_VERIFY(m_flecs != nullptr, "ecs_init failed");
    ecs_set_ctx(m_flecs, this, nullptr);

    // Built-in components. Identity and dirty state are never copied from prefabs.
    m_netIdentityId = *registerComponent(componentDescOf<NetIdentity>("helios.NetIdentity", ComponentFlags::DontInherit));
    bindSlot(typeSlot<NetIdentity>(), m_netIdentityId);
    m_repDirtyId = *registerComponent(componentDescOf<RepDirty>("helios.RepDirty", ComponentFlags::DontInherit));
    bindSlot(typeSlot<RepDirty>(), m_repDirtyId);
    m_frameRefId = *registerComponent(componentDescOf<FrameRef>("helios.FrameRef", ComponentFlags::DontInherit));
    bindSlot(typeSlot<FrameRef>(), m_frameRefId);
    m_dockRefId = *registerComponent(componentDescOf<DockRef>("helios.DockRef", ComponentFlags::DontInherit));
    bindSlot(typeSlot<DockRef>(), m_dockRefId);

    // Relationships (SPIKES.md §2 explains the storage choices).
    auto makeRelation = [&](const char* name, bool dontFragment) {
        ecs_entity_desc_t ed = {};
        ed.name = name;
        ed.sep = ".";
        const ecs_entity_t rel = ecs_entity_init(m_flecs, &ed);
        ecs_add_id(m_flecs, rel, EcsExclusive);
        if (dontFragment) ecs_add_id(m_flecs, rel, EcsDontFragment);
        return he(rel);
    };
    m_inFrame = makeRelation("helios.InFrame", desc.relations.inFrameDontFragment);
    m_dockedTo = makeRelation("helios.DockedTo", desc.relations.docking == DockStorage::PairDontFragment);

    // Destroys are unregistered by World::destroy (which walks the hierarchy), not by an on_remove
    // hook: any lifecycle hook on NetIdentity would make every entity table a flecs "complex" table.

    m_impl->changeQuery =
        createQueryPlan(*this, {Term{m_repDirtyId, TermAccess::Write}, Term{m_netIdentityId, TermAccess::Read}}, false);
}

World::~World() {
    m_impl->finalizing = true;
    for (auto& s : m_impl->systems) destroyQueryPlan(s->plan);
    destroyQueryPlan(m_impl->changeQuery);
    if (m_impl->flecsTaskThreads > 1) ecs_set_task_threads(m_flecs, 0);
    ecs_fini(m_flecs);
    m_flecs = nullptr;
}

const std::string& World::name() const noexcept { return m_desc.name; }

void World::beginTick() {
    ++m_tick;
    m_ids.maintain(); // refill ID blocks between ticks, never in the middle of a spawn burst
}

// ---------------------------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------------------------

void World::bindSlot(u32 slot, ComponentId cid) {
    if (slot >= m_typeIds.size()) {
        m_typeIds.resize(static_cast<usize>(slot) + 1, 0);
        m_typeReplBits.resize(static_cast<usize>(slot) + 1, 0);
    }
    const u64 index = m_impl->indexById.find(cid);
    HELIOS_ASSERT(index != 0, "bindSlot: unknown component");
    if (index == 0) return;
    u32& bound = m_impl->slotOfComponent[index - 1];
    if (bound != ~0u && bound != slot) {
        HELIOS_LOG_ERROR(LogEcs, "component '{}' is already bound to another C++ type",
                         m_impl->components[index - 1].name);
        HELIOS_ASSERT(false, "two C++ types registered under one component name");
        return;
    }
    bound = slot;
    m_typeIds[slot] = cid;
    const ComponentInfo& info = m_impl->components[index - 1];
    m_typeReplBits[slot] = info.isReplicated() ? (u64(1) << info.replIndex) : 0;
}

Result<ComponentId> World::registerComponent(const ComponentDesc& desc) {
    assertNotInStage();
    if (desc.name.empty()) return Error{ErrorCode::InvalidArgument, "component name is empty"};
    const std::string name = sanitizeName(desc.name);
    if (auto it = m_impl->indexByName.find(name); it != m_impl->indexByName.end()) {
        const ComponentInfo& existing = m_impl->components[it->second];
        if (existing.size != desc.size || (desc.size != 0 && existing.alignment != desc.alignment)) {
            return makeError(ErrorCode::AlreadyExists, "component '{}' re-registered with a different layout", name);
        }
        return existing.id;
    }

    const bool replicated = hasFlag(desc.flags, ComponentFlags::Replicated);
    if (replicated) {
        if (desc.size == 0 || desc.dirtyOffset == ~0u || desc.dirtyOffset + sizeof(FieldMask) > desc.size ||
            desc.replicatedFieldCount == 0 || desc.replicatedFieldCount > kMaxReplicatedFields) {
            return makeError(ErrorCode::InvalidArgument, "replicated component '{}' has an invalid dirty layout", name);
        }
        if (m_impl->replicatedCount >= kMaxReplicatedComponents) {
            return makeError(ErrorCode::LimitExceeded, "more than {} replicated components", kMaxReplicatedComponents);
        }
    }
    if (desc.size != 0 && (desc.alignment == 0 || !isPowerOfTwo(desc.alignment))) {
        return makeError(ErrorCode::InvalidArgument, "component '{}' alignment {} is invalid", name, desc.alignment);
    }

    // ecs_entity_init resolves names: an existing flecs entity (a builtin component, a relation, a
    // named frame or scope) would be turned into this component, and an existing flecs component of
    // another size would be returned as is (flecs only checks sizes in debug builds).
    if (ecs_lookup_path_w_sep(m_flecs, 0, name.c_str(), ".", nullptr, false) != 0) {
        return makeError(ErrorCode::AlreadyExists, "component name '{}' is already used by a flecs entity", name);
    }
    ecs_entity_desc_t ed = {};
    ed.name = name.c_str();
    ed.sep = ".";
    ed.use_low_id = true;
    const ecs_entity_t ent = ecs_entity_init(m_flecs, &ed);
    if (!ent) return makeError(ErrorCode::Unknown, "ecs_entity_init failed for '{}'", name);

    const u32 index = static_cast<u32>(m_impl->components.size());
    ComponentHooks& hooks = m_impl->hooks.emplace_back(desc.hooks);
    std::vector<std::byte>& defaults = m_impl->defaults.emplace_back();
    const std::byte* defaultValue = nullptr;
    // Plain components (memcpy-relocatable, nothing to destroy): no flecs hooks, captured default.
    const bool plain = desc.size != 0 && !hooks.destruct && !hooks.copy && !hooks.move;
    if (plain) {
        defaults.assign(desc.size + desc.alignment, std::byte{0});
        std::byte* aligned = alignPointer(defaults.data(), desc.alignment);
        if (hooks.construct) hooks.construct(aligned, 1);
        defaultValue = aligned;
    }
    if (desc.size != 0) {
        ecs_component_desc_t cd = {};
        cd.entity = ent;
        cd.type.size = static_cast<ecs_size_t>(desc.size);
        cd.type.alignment = static_cast<ecs_size_t>(desc.alignment);
        if (!plain) {
            cd.type.hooks.binding_ctx = &hooks;
            if (hooks.construct) cd.type.hooks.ctor = ctorTrampoline;
            if (hooks.destruct) cd.type.hooks.dtor = dtorTrampoline;
            if (hooks.copy) cd.type.hooks.copy = copyTrampoline;
            if (hooks.move) cd.type.hooks.move = moveTrampoline;
        }
        if (!ecs_component_init(m_flecs, &cd)) {
            // Keep hooks/defaults index-aligned with `components` (spawn paths index them together).
            m_impl->hooks.pop_back();
            m_impl->defaults.pop_back();
            return makeError(ErrorCode::Unknown, "ecs_component_init failed for '{}'", name);
        }
    }

    if (hasFlag(desc.flags, ComponentFlags::Shared)) ecs_add_pair(m_flecs, ent, EcsOnInstantiate, EcsInherit);
    if (hasFlag(desc.flags, ComponentFlags::DontInherit)) ecs_add_pair(m_flecs, ent, EcsOnInstantiate, EcsDontInherit);
    if (hasFlag(desc.flags, ComponentFlags::DontFragment)) {
        ecs_add_id(m_flecs, ent, EcsDontFragment);
    } else if (hasFlag(desc.flags, ComponentFlags::Sparse)) {
        ecs_add_id(m_flecs, ent, EcsSparse);
    }
    if (hasFlag(desc.flags, ComponentFlags::Singleton)) ecs_add_id(m_flecs, ent, EcsSingleton);

    ComponentInfo info;
    info.id = ent;
    info.name = name;
    info.size = desc.size;
    info.alignment = desc.size != 0 ? desc.alignment : 1;
    info.flags = desc.flags;
    if (replicated) {
        info.dirtyOffset = desc.dirtyOffset;
        info.replicatedFieldCount = desc.replicatedFieldCount;
        info.replIndex = static_cast<u8>(m_impl->replicatedCount);
        m_impl->replicated[m_impl->replicatedCount++] = index;
        // RepDirty is added by the World next to the first replicated component of an entity (a
        // (With, RepDirty) trait would leave it uninitialized: RepDirty has no flecs ctor).
    }
    info.defaultValue = defaultValue;
    m_impl->components.push_back(std::move(info));
    m_impl->slotOfComponent.push_back(~0u);
    m_impl->indexById.insert(ent, index + 1);
    m_impl->indexByName.emplace(name, index);
    return ComponentId(ent);
}

const ComponentInfo* World::componentInfo(ComponentId cid) const noexcept {
    const u64 index = m_impl->indexById.find(cid);
    return index ? &m_impl->components[index - 1] : nullptr;
}

const ComponentInfo* World::findComponent(std::string_view name) const noexcept {
    auto it = m_impl->indexByName.find(name);
    return it != m_impl->indexByName.end() ? &m_impl->components[it->second] : nullptr;
}

const ComponentInfo* World::replicatedComponent(u32 replIndex) const noexcept {
    return replIndex < m_impl->replicatedCount ? &m_impl->components[m_impl->replicated[replIndex]] : nullptr;
}

ComponentId World::pair(Entity relation, Entity target) noexcept { return ecs_pair(fe(relation), fe(target)); }

// ---------------------------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------------------------

NetIdentity World::identityOf(Entity e) const noexcept {
    const auto* ni = static_cast<const NetIdentity*>(ecs_get_id(m_flecs, fe(e), m_netIdentityId));
    return ni ? *ni : NetIdentity{};
}

void World::logEvent(StructuralOp op, Entity e, u64 arg) {
    const NetIdentity ni = identityOf(e);
    StructuralEvent ev;
    ev.op = op;
    ev.entity = ni.id;
    ev.handle = ni.handle;
    ev.arg = arg;
    m_log.push_back(ev);
}

Entity World::spawn(const SpawnDesc& desc) { return spawnImpl(desc, {}, EntityId()); }

ecs_table_t* World::findTable(std::vector<u64>& ids) {
    // flecs types are sorted, duplicate-free id arrays.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ecs_table_find(m_flecs, ids.data(), static_cast<int32_t>(ids.size()));
}

Entity World::spawnImpl(const SpawnDesc& desc, std::span<const SpawnOp> ops, EntityId preallocated) {
    assertNotInStage();
    Impl& impl = *m_impl;
    // A dead prefab/parent/frame (e.g. destroyed earlier in the same command batch) would make flecs
    // delete the new entity at once (dead parent) or store a pair with a dangling target (dead
    // frame/prefab), leaving the EntityId and NetHandle registered for an entity that is gone.
    for (const auto& [ref, what] : {std::pair{desc.prefab, "prefab"}, std::pair{desc.parent, "parent"},
                                    std::pair{desc.frame, "frame"}}) {
        if (ref.isValid() && !isAlive(ref)) {
            HELIOS_LOG_WARN(LogEcs, "spawn refused: its {} ({:#x}) is not alive", what, ref.id);
            ++impl.discarded;
            return Entity();
        }
    }
    const EntityId id = preallocated.isValid() ? preallocated : (desc.id.isValid() ? desc.id : m_ids.allocate());
    if (!id.isValid()) {
        HELIOS_LOG_ERROR(LogEcs, "spawn refused: no EntityId available");
        ++impl.discarded;
        return Entity();
    }
    if (m_registry.contains(id)) {
        HELIOS_LOG_ERROR(LogEcs, "spawn: EntityId {:#x} already registered", id.value);
        ++impl.discarded;
        return Entity();
    }

    // Final table: identity, prefab, frame and every table-stored component of `ops`, so the entity
    // is inserted once instead of moving table per component. Without a prefab the type is built
    // directly (no intermediate tables); IsA goes through the graph so flecs adds the prefab's
    // override components to the type.
    const bool frameInTable = desc.frame && !m_desc.relations.inFrameDontFragment;
    const bool childOfInTable = desc.parent && !m_desc.relations.nonFragmentingHierarchy;
    impl.typeScratch.clear();
    impl.typeScratch.push_back(m_netIdentityId);
    if (frameInTable) impl.typeScratch.push_back(ecs_pair(fe(m_inFrame), fe(desc.frame)));
    if (childOfInTable) impl.typeScratch.push_back(ecs_pair(EcsChildOf, fe(desc.parent)));
    impl.lateOps.clear();
    impl.opIndex.clear();
    for (usize i = 0; i < ops.size(); ++i) {
        const u64 index = impl.indexById.find(ops[i].id);
        const ComponentInfo* info = index ? &impl.components[index - 1] : nullptr;
        const bool sparse = info && hasFlag(info->flags, ComponentFlags::Sparse | ComponentFlags::DontFragment);
        // Values of foreign (non-World) components go through ecs_set_id so their hooks run.
        if (sparse || (!info && ops[i].value)) {
            impl.lateOps.push_back(static_cast<u32>(i));
            impl.opIndex.push_back(0);
        } else {
            impl.typeScratch.push_back(ops[i].id);
            impl.opIndex.push_back(static_cast<u32>(index));
        }
    }
    ecs_table_t* table = findTable(impl.typeScratch);
    if (desc.prefab) table = ecs_table_add_id(m_flecs, table, ecs_pair(EcsIsA, fe(desc.prefab)));

    const ecs_entity_t e = ecs_new_w_table(m_flecs, table); // constructs + instantiates the prefab
    (void)m_registry.add(id, he(e));
    NetIdentity ni;
    ni.id = id;
    ni.ag = desc.ag;
    if (desc.netHandle) {
        Result<NetHandle> h = m_registry.assignHandle(id, desc.contentHandleIndex);
        if (h) {
            ni.handle = *h;
        } else {
            HELIOS_LOG_WARN(LogEcs, "spawn: no NetHandle for {:#x}: {}", id.value, h.error());
        }
    }

    // Write the initial values in place (buffer values win over prefab copies). No OnSet events are
    // emitted for them: Helios components have no on_set hooks.
    ecs_record_t* record = ecs_record_find(m_flecs, e);
    ecs_table_t* t = record->table;
    const int32_t row = ECS_RECORD_TO_ROW(record->row);
    auto columnOf = [&](ComponentId cid) -> void* {
        const int32_t col = ecs_table_get_column_index(m_flecs, t, cid);
        return col >= 0 ? ecs_table_get_column(t, col, row) : nullptr;
    };
    std::memcpy(columnOf(m_netIdentityId), &ni, sizeof(NetIdentity));
    for (usize i = 0; i < ops.size(); ++i) {
        const u32 index = impl.opIndex[i];
        if (index == 0 || !ops[i].value) continue; // tags (plain components carry their default)
        void* dst = columnOf(ops[i].id);
        if (!dst) {
            ecs_set_id(m_flecs, e, ops[i].id, ops[i].size, ops[i].value);
            dst = ecs_get_mut_id(m_flecs, e, ops[i].id);
        } else {
            const ComponentHooks& hooks = impl.hooks[index - 1];
            if (hooks.copy) {
                hooks.copy(dst, ops[i].value, 1);
            } else {
                std::memcpy(dst, ops[i].value, ops[i].size);
            }
        }
        clearDirtyMask(dst, impl.components[index - 1]); // the Create event carries the full value
    }
    for (const u32 i : impl.lateOps) {
        if (ops[i].value) {
            ecs_set_id(m_flecs, e, ops[i].id, ops[i].size, ops[i].value);
        } else {
            ecs_add_id(m_flecs, e, ops[i].id);
        }
        if (const ComponentInfo* info = componentInfo(ops[i].id)) clearDirtyMask(ecs_get_mut_id(m_flecs, e, ops[i].id), *info);
    }
    if (desc.parent && !childOfInTable) {
        EcsParent p = {fe(desc.parent)};
        ecs_set_id(m_flecs, e, ecs_id(EcsParent), sizeof(EcsParent), &p);
    }
    if (desc.frame && !frameInTable) ecs_add_pair(m_flecs, e, fe(m_inFrame), fe(desc.frame));

    StructuralEvent ev;
    ev.op = StructuralOp::Create;
    ev.entity = id;
    ev.handle = ni.handle;
    m_log.push_back(ev);
    ++impl.structuralOps;
    if (desc.prefab) {
        // Replicated components copied from the prefab need the RepDirty summary (and must not
        // carry the prefab's _dirty bits); flecs also instantiated the prefab's children, which
        // need identities.
        initReplicatedState(he(e));
        assignChildIdentities(he(e), desc.netHandle, desc.ag);
    }
    return he(e);
}

void World::spawnGroup(CommandBuffer& buffer, u32 groupIndex) {
    Impl& impl = *m_impl;
    Impl::SpawnGroupData& grp = impl.groups[groupIndex];
    grp.created = true;
    const u32 rep = grp.members.front();
    const SpawnDesc& repDesc = buffer.m_spawns[rep];
    const SpawnOp* repOps = impl.flatOps.data() + impl.spawnOpBegin[rep];
    if (repDesc.frame && !isAlive(repDesc.frame)) {
        // The group's frame died earlier in the batch: refuse every member (see spawnImpl).
        HELIOS_LOG_WARN(LogEcs, "{} spawns refused: their frame ({:#x}) is not alive", grp.members.size(), repDesc.frame.id);
        impl.discarded += grp.members.size();
        return;
    }

    impl.typeScratch.clear();
    impl.typeScratch.push_back(m_netIdentityId);
    if (repDesc.frame) impl.typeScratch.push_back(ecs_pair(fe(m_inFrame), fe(repDesc.frame)));
    for (u32 k = 0; k < grp.opCount; ++k) impl.typeScratch.push_back(repOps[k].id);
    ecs_table_t* table = findTable(impl.typeScratch);

    const u32 count = static_cast<u32>(grp.members.size());
    ecs_bulk_desc_t bd = {};
    bd.count = static_cast<int32_t>(count);
    bd.table = table;
    const ecs_entity_t* created = ecs_bulk_init(m_flecs, &bd); // rows appended + constructed at once
    impl.bulkEntities.assign(created, created + count);

    // Column bases and element sizes, resolved once per group.
    auto column = [&](ComponentId cid, u32& size) -> std::byte* {
        const int32_t col = ecs_table_get_column_index(m_flecs, table, cid);
        if (col < 0) return nullptr;
        size = static_cast<u32>(ecs_table_get_column_size(table, col));
        return static_cast<std::byte*>(ecs_table_get_column(table, col, 0));
    };
    u32 netSize = 0;
    std::byte* netColumn = column(m_netIdentityId, netSize);
    impl.groupColumns.assign(grp.opCount, nullptr);
    impl.groupSizes.assign(grp.opCount, 0);
    impl.groupHooks.assign(grp.opCount, nullptr);
    impl.groupDirtyOffsets.assign(grp.opCount, ~0u);
    for (u32 k = 0; k < grp.opCount; ++k) {
        // Resolved for every op: members share the op ids but not whether they carry a value (one
        // may add<T>() a non-trivial component while another sets it).
        impl.groupColumns[k] = column(repOps[k].id, impl.groupSizes[k]);
        const u64 index = impl.indexById.find(repOps[k].id);
        if (index && impl.hooks[index - 1].copy) impl.groupHooks[k] = &impl.hooks[index - 1];
        if (index && impl.components[index - 1].isReplicated()) impl.groupDirtyOffsets[k] = impl.components[index - 1].dirtyOffset;
    }
    const int32_t firstRow = ECS_RECORD_TO_ROW(ecs_record_find(m_flecs, impl.bulkEntities[0])->row);

    for (u32 m = 0; m < count; ++m) {
        const u32 t = grp.members[m];
        const SpawnDesc& desc = buffer.m_spawns[t];
        const ecs_entity_t e = impl.bulkEntities[m];
        const usize row = static_cast<usize>(firstRow) + m;
        const EntityId id = impl.spawnIds[t];
        if (!id.isValid() || !m_registry.add(id, he(e))) {
            // No id could be minted, or two spawns of one buffer carried the same explicit id: the
            // entity is dropped after the loop (deleting now would swap rows under the remaining writes).
            HELIOS_LOG_ERROR(LogEcs, "spawn refused: EntityId {:#x} is {}", id.value, id.isValid() ? "already registered" : "invalid");
            impl.bulkDropped.push_back(e);
            ++impl.discarded;
            continue;
        }
        NetIdentity ni;
        ni.id = id;
        ni.ag = desc.ag;
        if (desc.netHandle) {
            Result<NetHandle> h = m_registry.assignHandle(id, desc.contentHandleIndex);
            if (h) ni.handle = *h;
        }
        std::memcpy(netColumn + row * netSize, &ni, sizeof(NetIdentity));
        const SpawnOp* ops = impl.flatOps.data() + impl.spawnOpBegin[t];
        for (u32 k = 0; k < grp.opCount; ++k) {
            std::byte* base = impl.groupColumns[k];
            if (!base || !ops[k].value) continue;
            std::byte* dst = base + row * impl.groupSizes[k];
            if (impl.groupHooks[k]) {
                impl.groupHooks[k]->copy(dst, ops[k].value, 1);
            } else {
                std::memcpy(dst, ops[k].value, ops[k].size);
            }
            if (impl.groupDirtyOffsets[k] != ~0u) {
                const FieldMask clean = 0; // the Create event carries the full value
                std::memcpy(dst + impl.groupDirtyOffsets[k], &clean, sizeof clean);
            }
        }
        buffer.m_resolved[t] = he(e);
        StructuralEvent ev;
        ev.op = StructuralOp::Create;
        ev.entity = id;
        ev.handle = ni.handle;
        m_log.push_back(ev);
    }
    impl.structuralOps += count - static_cast<u32>(impl.bulkDropped.size());
    for (const ecs_entity_t e : impl.bulkDropped) ecs_delete(m_flecs, e); // default NetIdentity: hook ignores it
    impl.bulkDropped.clear();
}

void World::assignChildIdentities(Entity root, bool netHandles, AgId ag) {
    std::vector<Entity> children;
    childrenOf(root, children); // ascending flecs id: deterministic
    for (const Entity child : children) {
        if (!ecs_has_id(m_flecs, fe(child), m_netIdentityId)) {
            NetIdentity ni;
            ni.id = m_ids.allocate();
            ni.ag = ag;
            if (!m_registry.add(ni.id, child)) continue;
            if (netHandles) {
                if (Result<NetHandle> h = m_registry.assignHandle(ni.id); h) ni.handle = *h;
            }
            ecs_set_id(m_flecs, fe(child), m_netIdentityId, sizeof(NetIdentity), &ni);
            initReplicatedState(child);
            StructuralEvent ev;
            ev.op = StructuralOp::Create;
            ev.entity = ni.id;
            ev.handle = ni.handle;
            m_log.push_back(ev);
        }
        assignChildIdentities(child, netHandles, ag);
    }
}

void World::releaseRelationTargets(Entity e) {
    // Entities in frame `e` or docked at host `e` lose that relation when `e` dies (flecs removes the
    // (InFrame, e) / (DockedTo, e) pairs; DockRef is cleared here). Log it: replication and
    // presentation would otherwise never learn about the implicit change.
    if (ecs_owns_id(m_flecs, fe(e), m_frameRefId)) {
        std::vector<Entity> members;
        collectRelated(m_flecs, fe(m_inFrame), fe(e), members);
        for (const Entity x : members) logEvent(StructuralOp::SetFrame, x, 0);
    }
    if (m_desc.relations.docking == DockStorage::Field) {
        auto it = m_impl->dockIndex.find(e.id);
        if (it == m_impl->dockIndex.end()) return;
        std::vector<u64> docked = std::move(it->second);
        m_impl->dockIndex.erase(it);
        std::sort(docked.begin(), docked.end());
        docked.erase(std::unique(docked.begin(), docked.end()), docked.end());
        for (const u64 id : docked) {
            auto* ref = isAlive(Entity(id)) ? static_cast<DockRef*>(ecs_get_mut_id(m_flecs, id, m_dockRefId)) : nullptr;
            if (!ref || ref->host != e) continue;
            ref->host = Entity();
            logEvent(StructuralOp::Undock, Entity(id), 0);
        }
    } else if (m_desc.relations.docking == DockStorage::PairDontFragment ||
               ecs_id_in_use(m_flecs, ecs_pair(fe(m_dockedTo), fe(e)))) { // (in_use misses non-fragmenting pairs)
        std::vector<Entity> docked;
        collectRelated(m_flecs, fe(m_dockedTo), fe(e), docked);
        for (const Entity x : docked) logEvent(StructuralOp::Undock, x, 0);
    }
}

void World::unregisterSubtree(Entity e) {
    releaseRelationTargets(e);
    if (const auto* ni = static_cast<const NetIdentity*>(ecs_get_id(m_flecs, fe(e), m_netIdentityId));
        ni && ni->id.isValid()) {
        m_registry.remove(ni->id, ni->handle);
        StructuralEvent ev;
        ev.op = StructuralOp::Destroy;
        ev.entity = ni->id;
        ev.handle = ni->handle;
        m_log.push_back(ev);
    }
    // flecs deletes the hierarchy below `e` with it (ChildOf / Parent cascade).
    const usize first = m_impl->destroyScratch.size();
    collectRelated(m_flecs, EcsChildOf, fe(e), m_impl->destroyScratch);
    const usize last = m_impl->destroyScratch.size();
    for (usize i = first; i < last; ++i) unregisterSubtree(m_impl->destroyScratch[i]);
    m_impl->destroyScratch.resize(first);
}

void World::destroy(Entity e) {
    assertNotInStage();
    if (!isAlive(e)) return;
    // Unregister first (depth-first, parents before children), then let flecs cascade. Entities
    // must be deleted through World::destroy for the identity maps to stay consistent.
    unregisterSubtree(e);
    ecs_delete(m_flecs, fe(e));
    ++m_impl->structuralOps;
}

bool World::isAlive(Entity e) const noexcept { return e.isValid() && ecs_is_alive(m_flecs, fe(e)); }

EntityId World::entityId(Entity e) const noexcept { return isAlive(e) ? identityOf(e).id : EntityId(); }

NetHandle World::netHandle(Entity e) const noexcept { return isAlive(e) ? identityOf(e).handle : NetHandle(); }

AgId World::authorityGroup(Entity e) const noexcept { return isAlive(e) ? identityOf(e).ag : 0; }

void World::setName(Entity e, std::string_view name) {
    assertNotInStage();
    const std::string n(name);
    ecs_set_name(m_flecs, fe(e), n.empty() ? nullptr : n.c_str());
}

std::string World::nameOf(Entity e) const {
    const char* n = isAlive(e) ? ecs_get_name(m_flecs, fe(e)) : nullptr;
    return n ? std::string(n) : std::string();
}

// ---------------------------------------------------------------------------------------------
// Raw component access
// ---------------------------------------------------------------------------------------------

void World::ensureRepDirty(Entity e) {
    if (!ecs_owns_id(m_flecs, fe(e), m_repDirtyId)) {
        const RepDirty zero{};
        ecs_set_id(m_flecs, fe(e), m_repDirtyId, sizeof(RepDirty), &zero);
    }
}

void World::addId(Entity e, ComponentId cid) {
    assertNotInStage();
    if (cid == 0 || !isAlive(e)) return;
    const ComponentInfo* info = componentInfo(cid);
    const bool fresh = !ecs_owns_id(m_flecs, fe(e), cid);
    if (info && info->isReplicated() && fresh) ensureRepDirty(e);
    if (info && info->defaultValue && fresh) {
        // Plain components have no flecs ctor: add them with their default value.
        ecs_set_id(m_flecs, fe(e), cid, info->size, info->defaultValue);
    } else {
        ecs_add_id(m_flecs, fe(e), cid);
    }
    ++m_impl->structuralOps;
    if (info && fresh) logEvent(StructuralOp::Add, e, cid);
}

void World::removeId(Entity e, ComponentId cid) {
    assertNotInStage();
    if (cid == 0 || !isAlive(e)) return;
    HELIOS_ASSERT(cid != m_netIdentityId, "NetIdentity cannot be removed; destroy the entity instead");
    const bool log = componentInfo(cid) != nullptr && ecs_owns_id(m_flecs, fe(e), cid);
    ecs_remove_id(m_flecs, fe(e), cid);
    ++m_impl->structuralOps;
    if (log) logEvent(StructuralOp::Remove, e, cid);
}

bool World::hasId(Entity e, ComponentId cid) const noexcept {
    return cid != 0 && isAlive(e) && ecs_has_id(m_flecs, fe(e), cid);
}

bool World::ownsId(Entity e, ComponentId cid) const noexcept {
    return cid != 0 && isAlive(e) && ecs_owns_id(m_flecs, fe(e), cid);
}

const void* World::getRaw(Entity e, ComponentId cid) const noexcept {
    if (cid == 0 || !e) return nullptr;
    return ecs_get_id(m_flecs, fe(e), cid);
}

void* World::getMutRaw(Entity e, ComponentId cid) noexcept {
    if (cid == 0 || !e) return nullptr;
    return ecs_get_mut_id(m_flecs, fe(e), cid);
}

void World::setRaw(Entity e, ComponentId cid, const void* value, usize size) {
    assertNotInStage();
    if (cid == 0 || !value || !isAlive(e)) return;
    const ComponentInfo* info = componentInfo(cid);
    HELIOS_ASSERT(info == nullptr || info->size == size, "setRaw: size mismatch");
    const bool owned = ecs_owns_id(m_flecs, fe(e), cid);
    const bool replicated = info != nullptr && info->isReplicated();
    FieldMask pending = 0;
    if (replicated) {
        if (owned) {
            // A whole-value write of an owned component is a raw write: keep the bits already
            // pending this tick (the copy would overwrite them) and mark every field.
            std::memcpy(&pending, static_cast<const std::byte*>(ecs_get_id(m_flecs, fe(e), cid)) + info->dirtyOffset,
                        sizeof pending);
        } else {
            ensureRepDirty(e);
        }
    }
    ecs_set_id(m_flecs, fe(e), cid, size, value);
    if (replicated) {
        void* stored = ecs_get_mut_id(m_flecs, fe(e), cid);
        if (owned) {
            const FieldMask all = pending | allFieldsMask(info->replicatedFieldCount);
            std::memcpy(static_cast<std::byte*>(stored) + info->dirtyOffset, &all, sizeof all);
            if (RepDirty* rep = repDirtyOf(e)) {
                rep->componentMask |= u64(1) << info->replIndex;
                rep->changed = m_tick;
            }
        } else {
            clearDirtyMask(stored, *info); // the Add event carries the full value
        }
    }
    if (info && !owned) {
        ++m_impl->structuralOps;
        logEvent(StructuralOp::Add, e, cid);
    }
}

RepDirty* World::repDirtyOf(Entity e) noexcept {
    return static_cast<RepDirty*>(ecs_get_mut_id(m_flecs, fe(e), m_repDirtyId));
}

void World::clearDirtyMask(void* component, const ComponentInfo& info) noexcept {
    if (!component || !info.isReplicated()) return;
    const FieldMask clean = 0;
    std::memcpy(static_cast<std::byte*>(component) + info.dirtyOffset, &clean, sizeof clean);
}

// ---------------------------------------------------------------------------------------------
// Relationships
// ---------------------------------------------------------------------------------------------

Entity World::parentOf(Entity e) const noexcept {
    if (!isAlive(e)) return Entity();
    return he(ecs_get_parent(m_flecs, fe(e)));
}

bool World::isAncestor(Entity ancestor, Entity e) const noexcept {
    u32 depth = 0;
    for (Entity p = parentOf(e); p && depth <= kMaxHierarchyDepth; p = parentOf(p), ++depth) {
        if (p == ancestor) return true;
    }
    return false;
}

Result<void> World::setParent(Entity child, Entity parent) {
    assertNotInStage();
    if (!isAlive(child) || !isAlive(parent)) return Error{ErrorCode::InvalidArgument, "setParent: dead entity"};
    if (child == parent) return Error{ErrorCode::InvalidArgument, "setParent: entity cannot parent itself"};
    // Acyclic: `child` must not be an ancestor of `parent`.
    u32 depth = 0;
    for (Entity p = parent; p; ++depth) {
        if (p == child) return Error{ErrorCode::InvalidArgument, "setParent: would create a cycle"};
        if (depth > kMaxHierarchyDepth) return Error{ErrorCode::InvalidState, "setParent: hierarchy too deep"};
        p = parentOf(p);
    }
    if (m_desc.relations.nonFragmentingHierarchy) {
        EcsParent p = {fe(parent)};
        ecs_set_id(m_flecs, fe(child), ecs_id(EcsParent), sizeof(EcsParent), &p);
    } else {
        ecs_add_pair(m_flecs, fe(child), EcsChildOf, fe(parent));
    }
    ++m_impl->structuralOps;
    logEvent(StructuralOp::SetParent, child, entityId(parent).value);
    return {};
}

void World::clearParent(Entity child) {
    assertNotInStage();
    if (!isAlive(child)) return;
    if (m_desc.relations.nonFragmentingHierarchy) {
        ecs_remove_id(m_flecs, fe(child), ecs_id(EcsParent));
    } else {
        ecs_remove_pair(m_flecs, fe(child), EcsChildOf, EcsWildcard);
    }
    ++m_impl->structuralOps;
    logEvent(StructuralOp::ClearParent, child, 0);
}



void World::childrenOf(Entity parent, std::vector<Entity>& out) const {
    if (!isAlive(parent)) return;
    collectRelated(m_flecs, EcsChildOf, fe(parent), out);
}

Entity World::createFrame(FrameId frameId, std::string_view name) {
    assertNotInStage();
    const ecs_entity_t e = ecs_new(m_flecs);
    FrameRef ref{frameId};
    ecs_set_id(m_flecs, e, m_frameRefId, sizeof(FrameRef), &ref);
    if (!name.empty()) setName(he(e), name);
    return he(e);
}

Entity World::frameOf(Entity e) const noexcept {
    if (!isAlive(e)) return Entity();
    return he(ecs_get_target(m_flecs, fe(e), fe(m_inFrame), 0));
}

void World::setReparentHook(ReparentHook hook) { m_impl->reparentHook = std::move(hook); }

Result<void> World::setFrame(Entity e, Entity frame) {
    assertNotInStage();
    if (!isAlive(e)) return Error{ErrorCode::InvalidArgument, "setFrame: dead entity"};
    // A dead target would leave an (InFrame, <stale id>) pair behind (flecs asserts in debug builds).
    if (frame && !isAlive(frame)) return Error{ErrorCode::InvalidArgument, "setFrame: frame is not alive"};
    if (frame == e) return Error{ErrorCode::InvalidArgument, "setFrame: entity cannot be its own frame"};
    const Entity from = frameOf(e);
    if (from == frame) return {};
    if (m_impl->reparentHook) m_impl->reparentHook(*this, e, from, frame);
    u64 arg = 0;
    if (frame) {
        ecs_add_pair(m_flecs, fe(e), fe(m_inFrame), fe(frame));
        const auto* ref = static_cast<const FrameRef*>(ecs_get_id(m_flecs, fe(frame), m_frameRefId));
        arg = ref ? ref->frame.value : frame.id;
    } else {
        ecs_remove_pair(m_flecs, fe(e), fe(m_inFrame), EcsWildcard);
    }
    ++m_impl->structuralOps;
    logEvent(StructuralOp::SetFrame, e, arg);
    return {};
}

Result<void> World::dock(Entity e, Entity host) {
    assertNotInStage();
    if (!isAlive(e) || !isAlive(host)) return Error{ErrorCode::InvalidArgument, "dock: dead entity"};
    if (e == host) return Error{ErrorCode::InvalidArgument, "dock: entity cannot dock at itself"};
    if (m_desc.relations.docking == DockStorage::Field) {
        auto* ref = static_cast<DockRef*>(ecs_get_mut_id(m_flecs, fe(e), m_dockRefId));
        if (ref) {
            if (ref->host == host) return {};
            ref->host = host; // re-dock: a value write, no table move (the old index entry is pruned lazily)
        } else {
            const DockRef value{host};
            ecs_set_id(m_flecs, fe(e), m_dockRefId, sizeof(DockRef), &value); // first dock: one table move
        }
        std::vector<u64>& list = m_impl->dockIndex[host.id];
        list.push_back(e.id);
        // Amortized pruning of stale entries (re-docked elsewhere, undocked or dead).
        if (list.size() >= 16 && isPowerOfTwo(list.size())) pruneDockList(host, list);
    } else {
        ecs_add_pair(m_flecs, fe(e), fe(m_dockedTo), fe(host));
    }
    ++m_impl->structuralOps;
    logEvent(StructuralOp::Dock, e, entityId(host).value);
    return {};
}

void World::undock(Entity e) {
    assertNotInStage();
    if (!isAlive(e) || !dockedTo(e)) return;
    if (m_desc.relations.docking == DockStorage::Field) {
        static_cast<DockRef*>(ecs_get_mut_id(m_flecs, fe(e), m_dockRefId))->host = Entity();
    } else {
        ecs_remove_pair(m_flecs, fe(e), fe(m_dockedTo), EcsWildcard);
    }
    ++m_impl->structuralOps;
    logEvent(StructuralOp::Undock, e, 0);
}

Entity World::dockedTo(Entity e) const noexcept {
    if (!isAlive(e)) return Entity();
    if (m_desc.relations.docking == DockStorage::Field) {
        const auto* ref = static_cast<const DockRef*>(ecs_get_id(m_flecs, fe(e), m_dockRefId));
        // A deleted host reads as "not docked" (flecs never revives an id with the same generation).
        return ref && isAlive(ref->host) ? ref->host : Entity();
    }
    return he(ecs_get_target(m_flecs, fe(e), fe(m_dockedTo), 0));
}

void World::dockedAt(Entity host, std::vector<Entity>& out) const {
    if (!isAlive(host)) return;
    if (m_desc.relations.docking != DockStorage::Field) {
        collectRelated(m_flecs, fe(m_dockedTo), fe(host), out);
        return;
    }
    auto it = m_impl->dockIndex.find(host.id);
    if (it == m_impl->dockIndex.end()) return;
    const usize first = out.size();
    for (const u64 id : it->second) {
        const Entity e(id);
        const auto* ref = isAlive(e) ? static_cast<const DockRef*>(ecs_get_id(m_flecs, id, m_dockRefId)) : nullptr;
        if (ref && ref->host == host) out.push_back(e);
    }
    std::sort(out.begin() + static_cast<isize>(first), out.end());
    out.erase(std::unique(out.begin() + static_cast<isize>(first), out.end()), out.end());
}

void World::pruneDockList(Entity host, std::vector<u64>& list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    usize kept = 0;
    for (const u64 id : list) {
        const auto* ref = isAlive(Entity(id)) ? static_cast<const DockRef*>(ecs_get_id(m_flecs, id, m_dockRefId)) : nullptr;
        if (ref && ref->host == host) list[kept++] = id;
    }
    list.resize(kept);
}

// ---------------------------------------------------------------------------------------------
// Prefabs
// ---------------------------------------------------------------------------------------------

Entity World::createPrefab(std::string_view name) {
    assertNotInStage();
    const ecs_entity_t p = ecs_new_w_id(m_flecs, EcsPrefab);
    if (!name.empty()) setName(he(p), name);
    return he(p);
}

Result<void> World::addPrefabChild(Entity prefab, Entity child) {
    assertNotInStage();
    if (!isAlive(prefab) || !isAlive(child)) return Error{ErrorCode::InvalidArgument, "addPrefabChild: dead entity"};
    if (!ecs_has_id(m_flecs, fe(prefab), EcsPrefab) || !ecs_has_id(m_flecs, fe(child), EcsPrefab)) {
        return Error{ErrorCode::InvalidArgument, "addPrefabChild: both entities must be prefabs"};
    }
    if (child == prefab || isAncestor(child, prefab)) {
        return Error{ErrorCode::InvalidArgument, "addPrefabChild: would create a cycle"};
    }
    if (m_desc.relations.nonFragmentingHierarchy) {
        // flecs 4.1.6 instantiates Parent-based prefab children as well (EcsTreeSpawner), giving the
        // instance's children a Parent component instead of a (ChildOf, instance) pair: no table per
        // instance, so thousands of ship instances stay under RT-01's 5,000-table cap.
        EcsParent p = {fe(prefab)};
        ecs_set_id(m_flecs, fe(child), ecs_id(EcsParent), sizeof(EcsParent), &p);
    } else {
        ecs_add_pair(m_flecs, fe(child), EcsChildOf, fe(prefab));
    }
    return {};
}

Entity World::instantiate(Entity prefab, SpawnDesc desc) {
    desc.prefab = prefab;
    return spawn(desc);
}

bool World::isA(Entity e, Entity prefab) const noexcept {
    return isAlive(e) && isAlive(prefab) && ecs_has_pair(m_flecs, fe(e), EcsIsA, fe(prefab));
}

void World::overrideId(Entity e, ComponentId cid) {
    assertNotInStage();
    if (cid == 0 || !isAlive(e) || ecs_owns_id(m_flecs, fe(e), cid)) return;
    const ComponentInfo* info = componentInfo(cid);
    if (info && info->isReplicated()) ensureRepDirty(e);
    if (!ecs_has_id(m_flecs, fe(e), cid) && info && info->defaultValue) {
        ecs_set_id(m_flecs, fe(e), cid, info->size, info->defaultValue); // nothing to inherit from
    } else {
        ecs_add_id(m_flecs, fe(e), cid); // adding an inherited component copies the prefab's value
    }
    // The own copy starts clean: it does not inherit the prefab's _dirty bits.
    if (info) clearDirtyMask(ecs_get_mut_id(m_flecs, fe(e), cid), *info);
    ++m_impl->structuralOps;
}

void World::initReplicatedState(Entity e) {
    if (m_impl->replicatedCount == 0) return;
    const ecs_type_t* type = ecs_get_type(m_flecs, fe(e));
    if (!type) return;
    bool any = false;
    for (int32_t i = 0; i < type->count; ++i) {
        const ComponentInfo* info = componentInfo(type->array[i]);
        if (!info || !info->isReplicated()) continue;
        any = true;
        clearDirtyMask(ecs_get_mut_id(m_flecs, fe(e), info->id), *info);
    }
    // Sparse / DontFragment replicated components are not part of the table type.
    for (u32 r = 0; r < m_impl->replicatedCount; ++r) {
        const ComponentInfo& info = m_impl->components[m_impl->replicated[r]];
        if (!hasFlag(info.flags, ComponentFlags::Sparse | ComponentFlags::DontFragment)) continue;
        if (!ecs_owns_id(m_flecs, fe(e), info.id)) continue;
        any = true;
        clearDirtyMask(ecs_get_mut_id(m_flecs, fe(e), info.id), info);
    }
    if (any) ensureRepDirty(e);
}

// ---------------------------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------------------------

void World::applyOne(CommandBuffer& buffer) {
    Impl& impl = *m_impl;
    // Typed commands carry this world's component ids; a buffer recorded for another world would
    // apply foreign ids.
    HELIOS_VERIFY(buffer.m_world == nullptr || buffer.m_world == this, "CommandBuffer recorded for another World");
    std::vector<CommandBuffer::Command>& cmds = buffer.m_commands;
    const u32 n = static_cast<u32>(cmds.size());
    const u32 spawns = static_cast<u32>(buffer.m_spawns.size());
    buffer.m_resolved.assign(spawns, Entity());

    // Fusion: Set/Add commands on a temp entity join its spawn (one table insertion) until another
    // kind of command targets that temp; later commands then apply in place, preserving order.
    impl.fusedHead.assign(spawns, ~0u);
    impl.fusedTail.assign(spawns, ~0u);
    impl.fusedNext.assign(n, ~0u);
    impl.fusedClosed.assign(spawns, 0);
    impl.fused.assign(n, 0);
    for (u32 i = 0; i < n; ++i) {
        const CommandBuffer::Command& cmd = cmds[i];
        if (!cmd.target.isTemp() || cmd.kind == CommandKind::Spawn) continue;
        const u32 t = cmd.target.tempIndex();
        if (t >= spawns) continue;
        if ((cmd.kind == CommandKind::Set || cmd.kind == CommandKind::Add) && !impl.fusedClosed[t]) {
            impl.fused[i] = 1;
            if (impl.fusedHead[t] == ~0u) {
                impl.fusedHead[t] = i;
            } else {
                impl.fusedNext[impl.fusedTail[t]] = i;
            }
            impl.fusedTail[t] = i;
        } else {
            impl.fusedClosed[t] = 1;
        }
    }

    // Per-spawn fused op lists (flat), EntityIds in command order, and grouping: spawns without
    // prefab/parent whose ops are all table-stored World components and that share (frame, op ids)
    // are created together with one ecs_bulk_init at the position of the group's first spawn.
    impl.spawnOpBegin.assign(spawns + 1, 0);
    impl.spawnOpEnd.assign(spawns, 0);
    impl.flatOps.clear();
    impl.spawnIds.assign(spawns, EntityId());
    impl.spawnGroup.assign(spawns, ~0u);
    impl.groups.clear();
    impl.groupByKey.clear();
    for (u32 i = 0; i < n; ++i) {
        const CommandBuffer::Command& cmd = cmds[i];
        if (cmd.kind != CommandKind::Spawn) continue;
        const u32 t = static_cast<u32>(cmd.arg);
        const SpawnDesc& desc = buffer.m_spawns[t];
        impl.spawnOpBegin[t] = static_cast<u32>(impl.flatOps.size());
        bool groupable = !desc.prefab && !desc.parent && !(desc.frame && m_desc.relations.inFrameDontFragment) &&
                         !(desc.id.isValid() && m_registry.contains(desc.id));
        u64 key = hashCombine(0x5A11u, desc.frame.id);
        bool replicated = false;
        for (u32 j = impl.fusedHead[t]; j != ~0u; j = impl.fusedNext[j]) {
            const CommandBuffer::Command& op = cmds[j];
            if (op.arg == 0) { // unregistered type (asserted at record time in dev builds)
                ++impl.discarded;
                continue;
            }
            const ComponentInfo* info = componentInfo(op.arg);
            SpawnOp flat{op.arg, op.kind == CommandKind::Set ? op.payload : nullptr, op.payloadSize};
            if (!flat.value && info && info->defaultValue) { // plain component added without a value
                flat.value = info->defaultValue;
                flat.size = info->size;
            }
            impl.flatOps.push_back(flat);
            if (!info || hasFlag(info->flags, ComponentFlags::Sparse | ComponentFlags::DontFragment)) groupable = false;
            replicated = replicated || (info && info->isReplicated());
            key = hashCombine(key, op.arg);
        }
        if (replicated) {
            const ComponentInfo* rep = componentInfo(m_repDirtyId);
            impl.flatOps.push_back(SpawnOp{m_repDirtyId, rep->defaultValue, rep->size});
            key = hashCombine(key, m_repDirtyId);
        }
        const u32 opCount = static_cast<u32>(impl.flatOps.size()) - impl.spawnOpBegin[t];
        impl.spawnOpEnd[t] = static_cast<u32>(impl.flatOps.size());
        if (opCount + 2 >= FLECS_ID_DESC_MAX) groupable = false;
        impl.spawnIds[t] = desc.id.isValid() ? desc.id : m_ids.allocate();
        if (!groupable) continue;
        // Find a group with the same (frame, op id sequence); the hash only narrows the search.
        key = hashCombine(key, opCount) | 1; // U64Map keys must be non-zero
        u32 g = ~0u;
        for (u64 cand = impl.groupByKey.find(key, ~0ull); cand != ~0ull;) {
            Impl::SpawnGroupData& grp = impl.groups[cand];
            const u32 rep = grp.members.front();
            bool same = buffer.m_spawns[rep].frame == desc.frame && grp.opCount == opCount;
            for (u32 k = 0; same && k < opCount; ++k) {
                same = impl.flatOps[impl.spawnOpBegin[rep] + k].id == impl.flatOps[impl.spawnOpBegin[t] + k].id;
            }
            if (same) {
                g = static_cast<u32>(cand);
                break;
            }
            cand = grp.nextSameKey;
        }
        if (g == ~0u) {
            g = static_cast<u32>(impl.groups.size());
            Impl::SpawnGroupData& grp = impl.groups.emplace_back();
            grp.opCount = opCount;
            grp.nextSameKey = impl.groupByKey.find(key, ~0ull);
            impl.groupByKey.insert(key, g);
        }
        impl.groups[g].members.push_back(t);
        impl.spawnGroup[t] = g;
    }
    impl.spawnOpBegin[spawns] = static_cast<u32>(impl.flatOps.size());

    auto resolve = [&](EntityRef ref) -> Entity {
        if (!ref.isTemp()) return ref.entity();
        const u32 i = ref.tempIndex();
        return i < buffer.m_resolved.size() ? buffer.m_resolved[i] : Entity();
    };
    for (u32 i = 0; i < n; ++i) {
        if (impl.fused[i]) continue;
        CommandBuffer::Command& cmd = cmds[i];
        if (cmd.kind == CommandKind::Spawn) {
            const u32 t = static_cast<u32>(cmd.arg);
            const u32 g = impl.spawnGroup[t];
            if (g != ~0u) {
                if (!impl.groups[g].created) spawnGroup(buffer, g);
                continue;
            }
            // Ops of spawn t are contiguous in flatOps (the fusion lists were flattened per spawn).
            const u32 begin = impl.spawnOpBegin[t];
            const u32 end = impl.spawnOpEnd[t];
            buffer.m_resolved[t] = spawnImpl(buffer.m_spawns[t],
                                             std::span<const SpawnOp>(impl.flatOps.data() + begin, end - begin),
                                             impl.spawnIds[t]);
            continue;
        }
        const Entity target = resolve(cmd.target);
        // flecs recycles ids with a new generation, so a stale Entity is never "alive". Component id
        // 0 comes from a type that was not registered (asserted at record time in dev builds).
        const bool needsComponent = cmd.kind == CommandKind::Add || cmd.kind == CommandKind::Remove ||
                                    cmd.kind == CommandKind::Set;
        if (!target || !ecs_is_alive(m_flecs, fe(target)) || (needsComponent && cmd.arg == 0)) {
            ++impl.discarded;
            continue;
        }
        switch (cmd.kind) {
        case CommandKind::Destroy: destroy(target); break;
        case CommandKind::Add: addId(target, cmd.arg); break;
        case CommandKind::Remove: removeId(target, cmd.arg); break;
        case CommandKind::Set: setRaw(target, cmd.arg, cmd.payload, cmd.payloadSize); break;
        case CommandKind::SetParent: {
            const Entity parent = resolve(cmd.other);
            if (!setParent(target, parent)) ++impl.discarded;
            break;
        }
        case CommandKind::ClearParent: clearParent(target); break;
        case CommandKind::SetFrame: {
            if (!setFrame(target, Entity(cmd.arg))) ++impl.discarded;
            break;
        }
        case CommandKind::Dock: {
            if (!dock(target, resolve(cmd.other))) ++impl.discarded;
            break;
        }
        case CommandKind::Undock: undock(target); break;
        case CommandKind::Spawn: break;
        }
    }
    // Run payload destructors, drop commands; resolved temps stay readable until the next record.
    for (CommandBuffer::Command& cmd : cmds) {
        if (cmd.destroyPayload) cmd.destroyPayload(cmd.payload);
    }
    cmds.clear();
    buffer.m_spawns.clear();
    buffer.m_blockIndex = 0;
    buffer.m_blockOffset = 0;
}

void World::apply(CommandBuffer& buffer) {
    CommandBuffer* one = &buffer;
    apply(std::span<CommandBuffer* const>(&one, 1));
}

void World::apply(std::span<CommandBuffer* const> buffers) {
    assertNotInStage();
    // Commands apply immediately and in order (each sees the effects of the previous ones); spawns
    // are fused with their component sets, so the common case costs one table insertion.
    for (CommandBuffer* b : buffers) {
        if (b && !b->empty()) applyOne(*b);
    }
}

// ---------------------------------------------------------------------------------------------
// flecs native path, stats
// ---------------------------------------------------------------------------------------------

void World::setFlecsTaskThreads(u32 count) {
    assertNotInStage();
    if (count > 1 && !m_jobs) {
        HELIOS_LOG_WARN(LogEcs, "setFlecsTaskThreads({}) needs a JobSystem; staying single-threaded", count);
        count = 0;
    }
    if (m_jobs) {
        // Stage 0 runs on the caller; every other stage is one job that blocks its worker.
        count = std::min(count, m_jobs->workerCount() + 1);
    }
    if (count <= 1) count = 0;
    m_impl->flecsTaskThreads = count;
    ecs_set_task_threads(m_flecs, static_cast<int32_t>(count));
}

void World::progressFlecs(f32 dt) {
    assertNotInStage();
    HELIOS_ASSERT(m_impl->flecsTaskThreads <= 1 || !m_jobs || !m_jobs->isWorkerThread(),
                  "progressFlecs with task threads must not run on a job worker");
    ecs_progress(m_flecs, dt);
}

WorldStats World::stats() const {
    WorldStats s;
    const ecs_world_info_t* info = ecs_get_world_info(m_flecs);
    s.tableCount = static_cast<u32>(info->table_count);
    s.entityCount = static_cast<u32>(m_registry.size());
    s.componentCount = static_cast<u32>(m_impl->components.size());
    s.systemCount = static_cast<u32>(m_impl->systems.size());
    s.structuralOpsApplied = m_impl->structuralOps;
    s.commandsDiscarded = m_impl->discarded;
    s.ecsHeapLiveBytes = ecsHeap().liveBytes();
    return s;
}

} // namespace helios::ecs
