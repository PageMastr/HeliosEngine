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
//   function of the call sequence (deterministic).
// * EntityRegistry: combines both; Entity -> EntityId is not stored here because every entity
//   carries NetIdentity (World::entityId()).
//
// Threading: none of these are internally synchronized. The World mutates them only at sync
// points (main thread); concurrent const lookups from parallel stages are safe.

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

    /// Issues a dynamic handle (FIFO slot reuse). Fails with LimitExceeded when the table is full.
    Result<NetHandle> allocate(EntityId id);
    /// Issues the content-placed slot `index` (1..reservedCount). Fails if out of range or in use.
    Result<NetHandle> allocateAt(u32 index, EntityId id);
    /// Frees a live handle (bumps the slot generation). Returns false for stale/invalid handles.
    bool release(NetHandle handle);

    /// EntityId of a live handle, or an invalid id for stale/invalid handles.
    EntityId resolve(NetHandle handle) const noexcept;
    bool isLive(NetHandle handle) const noexcept { return resolve(handle).isValid(); }
    /// Handle currently issued for slot `index` (invalid if the slot is free).
    NetHandle handleAt(u32 index) const noexcept;

    u32 liveCount() const noexcept { return m_live; }
    u32 reservedCount() const noexcept { return m_reserved; }
    u32 maxHandles() const noexcept { return m_max; }

private:
    struct Slot {
        EntityId id;
        u8 generation = 1;
        bool live = false;
    };
    Slot& slot(u32 index);

    std::vector<Slot> m_slots; // index 0 unused
    std::vector<u32> m_freeRing;
    usize m_freeHead = 0; // FIFO over m_freeRing[m_freeHead..]
    u32 m_nextFresh;
    u32 m_max;
    u32 m_reserved;
    u32 m_reuseDelay;
    u32 m_live = 0;
};

/// EntityId <-> Entity and NetHandle <-> Entity maps plus the handle table.
class EntityRegistry {
public:
    explicit EntityRegistry(const NetHandleTable::Desc& handles = {}, MemoryTag tag = MemoryTag::Unknown);

    /// Registers `entity` under `id` (which must be unused). Returns AlreadyExists otherwise.
    Result<void> add(EntityId id, Entity entity);
    /// Issues a NetHandle for a registered id. `contentIndex` != 0 uses allocateAt().
    Result<NetHandle> assignHandle(EntityId id, u32 contentIndex = 0);
    /// Removes id (and releases `handle` if valid). Returns false if id was unknown.
    bool remove(EntityId id, NetHandle handle);

    Entity find(EntityId id) const noexcept { return Entity(m_byId.find(id.value)); }
    Entity find(NetHandle handle) const noexcept;
    EntityId resolve(NetHandle handle) const noexcept { return m_handles.resolve(handle); }
    bool contains(EntityId id) const noexcept { return m_byId.contains(id.value); }

    usize size() const noexcept { return m_byId.size(); }
    const NetHandleTable& handles() const noexcept { return m_handles; }
    usize memoryBytes() const noexcept;

private:
    U64Map m_byId;
    NetHandleTable m_handles;
    std::vector<Entity> m_byHandleIndex;
};

} // namespace helios::ecs
