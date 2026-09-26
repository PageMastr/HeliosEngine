#pragma once
// Identity maps (02 §4.1): EntityId <-> flecs Entity <-> NetHandle.
//
// * U64Map: open-addressing (linear probing, backward-shift deletion) u64 -> u64 map. Key 0 is
//   reserved (EntityId 0 and Entity 0 are invalid anyway). Memory is attributed to a MemoryTag.
// * NetHandleTable: the dense zone-instance handle table. Slots [1, reservedCount] belong to
//   content-placed entities and are only issued through allocateAt() (their container's index
//   table); the rest are issued by allocate(): freed slots wait in a FIFO and are reused once more
//   than `reuseDelay` are waiting (or the table has no fresh slot left), so stale handles on
//   clients are unlikely to alias (the 8-bit generation catches the rest). Issue order is a pure
//   function of the call sequence (deterministic). Each slot also stores an opaque owner value
//   (EntityRegistry keeps the entity there, so a handle's id and entity share one slot).
// * EntityRegistry: combines both; Entity -> EntityId is not stored here because every entity
//   carries NetIdentity (World::entityId()).
//
// Threading: none of these are internally synchronized. The World mutates them only at sync
// points (main thread); concurrent const lookups from parallel stages are safe.

#include <memory>
#include <span>
#include <vector>

#include "helios/core/memory.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

class U64Map {
public:
    explicit U64Map(MemoryTag tag = MemoryTag::Unknown, usize initialCapacity = 64);
    ~U64Map();
    U64Map(const U64Map&) = delete;
    U64Map& operator=(const U64Map&) = delete;
    U64Map(U64Map&& other) noexcept;
    U64Map& operator=(U64Map&& other) noexcept;

    /// Inserts or overwrites. `key` must not be 0. Returns true if the key was new.
    bool insert(u64 key, u64 value);
    /// Inserts only if `key` is absent (one probe sequence). `key` must not be 0. Returns false,
    /// leaving the map unchanged, if the key exists.
    bool insertNew(u64 key, u64 value);
    /// Returns the value or `missing`.
    u64 find(u64 key, u64 missing = 0) const noexcept;
    bool contains(u64 key) const noexcept;
    /// Returns true if the key was present.
    bool erase(u64 key) noexcept;
    void clear() noexcept;
    void reserve(usize count);

    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    usize memoryBytes() const noexcept { return m_capacity * sizeof(Slot); }

    /// Visits every (key, value) in unspecified order. The map must not change during the visit.
    template <class F>
    void forEach(F&& fn) const {
        for (usize i = 0; i < m_capacity; ++i) {
            if (m_slots[i].key != 0) fn(m_slots[i].key, m_slots[i].value);
        }
    }

private:
    struct Slot {
        u64 key;
        u64 value;
    };
    void rehash(usize newCapacity);
    usize slotOf(u64 key) const noexcept;

    Slot* m_slots = nullptr;
    usize m_capacity = 0; // power of two
    usize m_size = 0;
    MemoryTag m_tag;
};

class NetHandleTable {
public:
    struct Desc {
        u32 maxHandles = NetHandle::kMaxIndex; ///< Highest slot index that may be issued.
        u32 reservedCount = 0;                 ///< Slots [1, reservedCount] are content-placed only.
        /// A freed dynamic slot is reused only once more than this many slots wait in the FIFO (or
        /// no fresh slot is left), so a client's stale handle rarely meets a recycled slot.
        u32 reuseDelay = 1024;
        MemoryTag tag = MemoryTag::Unknown;
    };

    explicit NetHandleTable(const Desc& desc);
    NetHandleTable();

    /// Issues a dynamic handle (FIFO slot reuse) and stores `owner` with it. Fails with
    /// LimitExceeded when the table is full and InvalidArgument for an invalid id.
    Result<NetHandle> allocate(EntityId id, u64 owner = 0);
    /// allocate() without the error object (bulk spawn paths): an invalid handle on failure.
    NetHandle tryAllocate(EntityId id, u64 owner = 0) noexcept;
    /// tryAllocate(ids[i], owners[i]) for every i, in order, into out[i] (the same handles).
    void tryAllocateN(std::span<const EntityId> ids, const u64* owners, NetHandle* out) noexcept;
    /// Issues the content-placed slot `index` (1..reservedCount). Fails if out of range or in use.
    Result<NetHandle> allocateAt(u32 index, EntityId id, u64 owner = 0);
    /// Frees a live handle (bumps the slot generation). Returns false for stale/invalid handles.
    bool release(NetHandle handle);
    /// release() only if `handle` is live and was issued to `id` (one validation for both checks).
    bool releaseIssuedTo(NetHandle handle, EntityId id) {
        const u32 index = handle.index();
        if (index == 0 || index >= m_slots.size() || !id.isValid()) return false;
        if (m_state[index] != (kLive | handle.generation()) || m_slots[index].id != id) return false;
        freeSlot(index);
        return true;
    }

    /// EntityId of a live handle, or an invalid id for stale/invalid handles.
    EntityId resolve(NetHandle handle) const noexcept {
        const Slot* s = liveSlot(handle);
        return s ? s->id : EntityId();
    }
    /// Owner value stored with a live handle, or 0 for stale/invalid handles.
    u64 ownerOf(NetHandle handle) const noexcept {
        const Slot* s = liveSlot(handle);
        return s ? s->owner : 0;
    }
    bool isLive(NetHandle handle) const noexcept {
        const u32 index = handle.index();
        return index != 0 && index < m_state.size() && m_state[index] == (kLive | handle.generation());
    }
    /// Handle currently issued for slot `index` (invalid if the slot is free).
    NetHandle handleAt(u32 index) const noexcept;

    u32 liveCount() const noexcept { return m_live; }
    u32 reservedCount() const noexcept { return m_reserved; }
    u32 maxHandles() const noexcept { return m_max; }
    usize memoryBytes() const noexcept {
        return m_slots.capacity() * sizeof(Slot) + m_state.capacity() * sizeof(u16) + m_freeRing.capacity() * sizeof(u32);
    }

private:
    friend class EntityRegistry;

    // Spawn and destroy bursts visit slots in FIFO order, which is scattered over the table. A
    // slot's id and owner (16 bytes, one cache line per visit) are only written when it is issued
    // and read by lookups; whether it is live and its generation sit in a small array that stays
    // cached, so a release validated by the caller (EntityRegistry keeps each id's handle) touches
    // only that array and the FIFO.
    struct Slot {
        EntityId id; // meaningful while the slot is live
        u64 owner = 0;
    };
    static_assert(sizeof(Slot) == 16);
    static constexpr u16 kLive = 0x100; // m_state: kLive | generation of the issued handle, or the
                                        // generation the next handle gets while free
    const Slot* liveSlot(NetHandle handle) const noexcept { return isLive(handle) ? &m_slots[handle.index()] : nullptr; }
    /// release() of a handle the caller knows is live and issued by it (EntityRegistry::remove()).
    bool releaseKnown(NetHandle handle) noexcept {
        const u32 index = handle.index();
        if (index == 0 || index >= m_state.size() || m_state[index] != (kLive | handle.generation())) return false;
        freeSlot(index);
        return true;
    }
    void freeSlot(u32 index) {
        const u16 generation = m_state[index] & 0xFF;
        m_state[index] = static_cast<u16>(generation == 0xFF ? 1 : generation + 1); // 0 is never issued
        --m_live;
        if (index > m_reserved) {
            if (m_freeTail == m_freeRing.size()) growRing();
            m_freeRing[m_freeTail++] = index;
        }
    }
    NetHandle issue(u32 index, EntityId id, u64 owner) noexcept;
    u32 nextIndex() noexcept; // the slot tryAllocate() issues next (0: full), taken from the FIFO
    void compactRing() noexcept;
    void growRing();
    void grow(u32 index);

    std::vector<Slot> m_slots; // index 0 unused
    std::vector<u16> m_state;  // per slot: kLive while issued | generation
    std::vector<u32> m_freeRing; // FIFO of freed dynamic slots: [m_freeHead, m_freeTail) wait
    usize m_freeHead = 0;
    usize m_freeTail = 0;
    u32 m_nextFresh;
    u32 m_max;
    u32 m_reserved;
    u32 m_reuseDelay;
    u32 m_live = 0;
};

/// EntityId <-> Entity and NetHandle <-> Entity maps plus the handle table.
///
/// Runtime (block) ids are minted consecutively, so they are kept in pages of 64 consecutive ids
/// (a small U64Map finds the page): a spawn or destroy burst of consecutive ids then touches a few
/// cache lines instead of one random line per id in a large hash table, which evicted the tables
/// that the rest of a sync point works on (ADR-004a, SPIKES.md §5). Content-placed and client-local
/// ids are hashes and stay in a U64Map. A page is recycled once its last id is removed, so ids
/// scattered over many blocks (restored entities) cost at most one 776-byte page each. A page also
/// records the NetHandle issued for each of its ids, which validates that handle's release.
class EntityRegistry {
public:
    explicit EntityRegistry(const NetHandleTable::Desc& handles = {}, MemoryTag tag = MemoryTag::Unknown);

    /// Registers `entity` under `id` (which must be unused). Returns AlreadyExists otherwise.
    Result<void> add(EntityId id, Entity entity);
    /// add() for the bulk spawn paths: one lookup and no error object. Returns false, registering
    /// nothing, if `id` or `entity` is invalid or `id` is taken.
    bool addNew(EntityId id, Entity entity) {
        // Consecutive ids of a spawn burst land in the page the previous add used.
        if (((id.value >> kPageBits) + 1) == m_lastPageKey && paged(id) && entity.isValid()) {
            u64& slot = m_lastPagePtr->entity[id.value & (kPageIds - 1)];
            if (slot != 0) return false;
            slot = entity.id;
            ++m_lastPagePtr->live;
            ++m_pagedCount;
            return true;
        }
        return addNewSlow(id, entity);
    }
    /// Reserves capacity for `additional` more ids (one rehash for a whole spawn group).
    void reserve(usize additional);
    /// Issues a NetHandle for a registered id. `contentIndex` != 0 uses allocateAt().
    Result<NetHandle> assignHandle(EntityId id, u32 contentIndex = 0);
    /// assignHandle() for an id the caller has just registered for `entity` (skips the lookup).
    Result<NetHandle> assignHandleFor(EntityId id, Entity entity, u32 contentIndex = 0);
    /// assignHandleFor() of a dynamic handle without the error object (bulk spawn paths): an
    /// invalid handle when the table is full.
    NetHandle tryAssignHandle(EntityId id, Entity entity) noexcept;
    /// tryAssignHandle(ids[i], Entity(entities[i])) for every i, in order, into out[i].
    void tryAssignHandles(std::span<const EntityId> ids, const u64* entities, NetHandle* out) noexcept;
    /// Removes id (and releases `handle` if valid). Returns false if id was unknown.
    bool remove(EntityId id, NetHandle handle) {
        // A destroy burst of consecutive ids stays in one page (the page is recycled on the slow path).
        if (((id.value >> kPageBits) + 1) == m_lastPageKey && paged(id) && m_lastPagePtr->live > 1) {
            const u32 offset = static_cast<u32>(id.value & (kPageIds - 1));
            u64& slot = m_lastPagePtr->entity[offset];
            if (slot == 0) return false;
            slot = 0;
            --m_lastPagePtr->live;
            --m_pagedCount;
            releaseHandle(id, handle, m_lastPagePtr->handle[offset]);
            return true;
        }
        return removeSlow(id, handle);
    }

    Entity find(EntityId id) const noexcept;
    Entity find(NetHandle handle) const noexcept { return Entity(m_handles.ownerOf(handle)); }
    EntityId resolve(NetHandle handle) const noexcept { return m_handles.resolve(handle); }
    bool contains(EntityId id) const noexcept { return find(id).isValid(); }

    usize size() const noexcept { return m_byId.size() + m_pagedCount; }
    const NetHandleTable& handles() const noexcept { return m_handles; }
    usize memoryBytes() const noexcept;

private:
    static constexpr u32 kPageBits = 6;
    static constexpr u32 kPageIds = 1u << kPageBits;
    static constexpr u32 kChunkPages = 64;
    struct Page {
        u64 entity[kPageIds]; // Entity ids by offset within the page; 0 = free
        u32 handle[kPageIds]; // NetHandle value the registry issued for the id, 0 = none
        u32 live;
    };
    /// Releases `handle` for `id` (if valid). When it is the handle this registry recorded for the
    /// id, it is known to be live and issued to `id`, and the release skips the handle table's
    /// slot (a scattered cache line in a destroy burst); any other handle takes the checked path.
    void releaseHandle(EntityId id, NetHandle handle, u32& recorded) {
        if (handle.isValid()) {
            if (handle.value == recorded) {
                m_handles.releaseKnown(handle);
            } else {
                m_handles.releaseIssuedTo(handle, id);
            }
        }
        recorded = 0;
    }
    void recordHandle(EntityId id, NetHandle handle);
    struct ChunkFree {
        void operator()(Page* chunk) const noexcept;
    };
    static bool paged(EntityId id) noexcept { return id.kind() == EntityIdKind::Runtime; }
    Page& pageAt(u32 index) const noexcept { return m_chunks[index / kChunkPages].get()[index % kChunkPages]; }
    Page* findPage(EntityId id) const noexcept;
    Page& ensurePage(EntityId id);
    bool addNewSlow(EntityId id, Entity entity);
    bool removeSlow(EntityId id, NetHandle handle);

    MemoryTag m_tag;
    U64Map m_byId;   // content-placed and client-local ids
    U64Map m_pageOf; // (runtime id >> kPageBits) + 1 -> page index + 1
    std::vector<std::unique_ptr<Page, ChunkFree>> m_chunks; // kChunkPages pages each (stable addresses)
    std::vector<u32> m_freePages;
    u32 m_pageCount = 0; // pages handed out from the chunks so far
    usize m_pagedCount = 0;
    // The page last used by addNew()/remove() (bursts touch consecutive ids); const lookups do not
    // use it, so they stay safe to run concurrently.
    u64 m_lastPageKey = 0;
    u32 m_lastPage = 0;
    Page* m_lastPagePtr = nullptr; // pageAt(m_lastPage) while m_lastPageKey != 0
    NetHandleTable m_handles; // handle -> EntityId, with the Entity as the slot owner
};

} // namespace helios::ecs
