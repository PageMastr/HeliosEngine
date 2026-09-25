#include "helios/ecs/registry.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/hash.h"

namespace helios::ecs {

// ---------------------------------------------------------------------------------------------
// U64Map
// ---------------------------------------------------------------------------------------------

U64Map::U64Map(MemoryTag tag, usize initialCapacity) : m_tag(tag) {
    rehash(std::max<usize>(16, nextPowerOfTwo(initialCapacity)));
}

U64Map::~U64Map() { alignedFree(m_slots); }

U64Map::U64Map(U64Map&& other) noexcept
    : m_slots(std::exchange(other.m_slots, nullptr)), m_capacity(std::exchange(other.m_capacity, 0)),
      m_size(std::exchange(other.m_size, 0)), m_tag(other.m_tag) {}

U64Map& U64Map::operator=(U64Map&& other) noexcept {
    if (this != &other) {
        alignedFree(m_slots);
        m_slots = std::exchange(other.m_slots, nullptr);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_size = std::exchange(other.m_size, 0);
        m_tag = other.m_tag;
    }
    return *this;
}

usize U64Map::slotOf(u64 key) const noexcept { return static_cast<usize>(mix64(key)) & (m_capacity - 1); }

void U64Map::rehash(usize newCapacity) {
    Slot* old = m_slots;
    const usize oldCapacity = m_capacity;
    m_slots = static_cast<Slot*>(alignedAlloc(newCapacity * sizeof(Slot), alignof(Slot), m_tag));
    HELIOS_VERIFY(m_slots != nullptr, "U64Map: out of memory");
    std::memset(static_cast<void*>(m_slots), 0, newCapacity * sizeof(Slot));
    m_capacity = newCapacity;
    m_size = 0;
    for (usize i = 0; i < oldCapacity; ++i) {
        if (old[i].key != 0) insert(old[i].key, old[i].value);
    }
    alignedFree(old);
}

void U64Map::reserve(usize count) {
    // Keep the load factor <= 1/2 (short probe sequences; lookups dominate).
    const usize want = nextPowerOfTwo(std::max<usize>(16, count * 2));
    if (want > m_capacity) rehash(want);
}

bool U64Map::insert(u64 key, u64 value) {
    HELIOS_ASSERT(key != 0, "U64Map: key 0 is reserved");
    if ((m_size + 1) * 2 > m_capacity) rehash(m_capacity * 2);
    usize i = slotOf(key);
    for (;;) {
        Slot& s = m_slots[i];
        if (s.key == key) {
            s.value = value;
            return false;
        }
        if (s.key == 0) {
            s.key = key;
            s.value = value;
            ++m_size;
            return true;
        }
        i = (i + 1) & (m_capacity - 1);
    }
}

u64 U64Map::find(u64 key, u64 missing) const noexcept {
    if (key == 0) return missing;
    usize i = slotOf(key);
    for (;;) {
        const Slot& s = m_slots[i];
        if (s.key == key) return s.value;
        if (s.key == 0) return missing;
        i = (i + 1) & (m_capacity - 1);
    }
}

bool U64Map::contains(u64 key) const noexcept {
    if (key == 0) return false;
    usize i = slotOf(key);
    for (;;) {
        const Slot& s = m_slots[i];
        if (s.key == key) return true;
        if (s.key == 0) return false;
        i = (i + 1) & (m_capacity - 1);
    }
}

bool U64Map::erase(u64 key) noexcept {
    if (key == 0) return false;
    const usize mask = m_capacity - 1;
    usize i = slotOf(key);
    for (;;) {
        if (m_slots[i].key == 0) return false;
        if (m_slots[i].key == key) break;
        i = (i + 1) & mask;
    }
    // Backward-shift deletion: pull later members of the probe chain into the hole so no
    // tombstones are needed.
    usize hole = i;
    usize j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (m_slots[j].key == 0) break;
        const usize home = slotOf(m_slots[j].key);
        // Move j into the hole if its home is not in the cyclic range (hole, j].
        const bool homeInRange = (hole <= j) ? (home > hole && home <= j) : (home > hole || home <= j);
        if (!homeInRange) {
            m_slots[hole] = m_slots[j];
            hole = j;
        }
    }
    m_slots[hole] = Slot{0, 0};
    --m_size;
    return true;
}

void U64Map::clear() noexcept {
    std::memset(static_cast<void*>(m_slots), 0, m_capacity * sizeof(Slot));
    m_size = 0;
}

// ---------------------------------------------------------------------------------------------
// NetHandleTable
// ---------------------------------------------------------------------------------------------

NetHandleTable::NetHandleTable(const Desc& desc)
    : m_max(std::min(desc.maxHandles == 0 ? NetHandle::kMaxIndex : desc.maxHandles, NetHandle::kMaxIndex)),
      m_reserved(std::min(desc.reservedCount, NetHandle::kMaxIndex)), m_reuseDelay(desc.reuseDelay) {
    m_nextFresh = m_reserved + 1;
    m_slots.resize(1); // slot 0 is never issued
}

NetHandleTable::NetHandleTable() : NetHandleTable(Desc{}) {}

NetHandleTable::Slot& NetHandleTable::slot(u32 index) {
    if (index >= m_slots.size()) m_slots.resize(static_cast<usize>(index) + 1);
    return m_slots[index];
}

Result<NetHandle> NetHandleTable::allocate(EntityId id) {
    u32 index = 0;
    const usize waiting = m_freeRing.size() - m_freeHead;
    const bool freshLeft = m_nextFresh <= m_max && m_nextFresh > m_reserved;
    if (waiting > 0 && (waiting > m_reuseDelay || !freshLeft)) {
        index = m_freeRing[m_freeHead++];
        if (m_freeHead >= 4096 && m_freeHead * 2 >= m_freeRing.size()) {
            m_freeRing.erase(m_freeRing.begin(), m_freeRing.begin() + static_cast<isize>(m_freeHead));
            m_freeHead = 0;
        }
    } else if (freshLeft) {
        index = m_nextFresh++;
    } else {
        return Error{ErrorCode::LimitExceeded, "NetHandleTable is full"};
    }
    Slot& s = slot(index);
    HELIOS_ASSERT(!s.live);
    s.live = true;
    s.id = id;
    ++m_live;
    return NetHandle::make(index, s.generation);
}

Result<NetHandle> NetHandleTable::allocateAt(u32 index, EntityId id) {
    if (index == 0 || index > m_reserved) {
        return makeError(ErrorCode::OutOfRange, "content handle index {} outside [1, {}]", index, m_reserved);
    }
    Slot& s = slot(index);
    if (s.live) return makeError(ErrorCode::AlreadyExists, "content handle index {} in use", index);
    s.live = true;
    s.id = id;
    ++m_live;
    return NetHandle::make(index, s.generation);
}

bool NetHandleTable::release(NetHandle handle) {
    const u32 index = handle.index();
    if (index == 0 || index >= m_slots.size()) return false;
    Slot& s = m_slots[index];
    if (!s.live || s.generation != handle.generation()) return false;
    s.live = false;
    s.id = EntityId();
    s.generation = static_cast<u8>(s.generation + 1);
    if (s.generation == 0) s.generation = 1; // generation 0 is never issued
    --m_live;
    if (index > m_reserved) m_freeRing.push_back(index);
    return true;
}

EntityId NetHandleTable::resolve(NetHandle handle) const noexcept {
    const u32 index = handle.index();
    if (index == 0 || index >= m_slots.size()) return EntityId();
    const Slot& s = m_slots[index];
    return (s.live && s.generation == handle.generation()) ? s.id : EntityId();
}

NetHandle NetHandleTable::handleAt(u32 index) const noexcept {
    if (index == 0 || index >= m_slots.size() || !m_slots[index].live) return NetHandle();
    return NetHandle::make(index, m_slots[index].generation);
}

// ---------------------------------------------------------------------------------------------
// EntityRegistry
// ---------------------------------------------------------------------------------------------

EntityRegistry::EntityRegistry(const NetHandleTable::Desc& handles, MemoryTag tag)
    : m_byId(tag, 1024), m_handles(handles) {}

Result<void> EntityRegistry::add(EntityId id, Entity entity) {
    if (!id.isValid() || !entity.isValid()) return Error{ErrorCode::InvalidArgument, "invalid id or entity"};
    if (m_byId.contains(id.value)) return makeError(ErrorCode::AlreadyExists, "EntityId {:#x} already registered", id.value);
    m_byId.insert(id.value, entity.id);
    return {};
}

Result<NetHandle> EntityRegistry::assignHandle(EntityId id, u32 contentIndex) {
    const Entity entity = find(id);
    if (!entity) return Error{ErrorCode::NotFound, "assignHandle: unknown EntityId"};
    Result<NetHandle> handle = contentIndex != 0 ? m_handles.allocateAt(contentIndex, id) : m_handles.allocate(id);
    if (!handle) return handle;
    const u32 index = handle->index();
    if (index >= m_byHandleIndex.size()) m_byHandleIndex.resize(std::max<usize>(index + 1, m_byHandleIndex.size() * 2));
    m_byHandleIndex[index] = entity;
    return handle;
}

bool EntityRegistry::remove(EntityId id, NetHandle handle) {
    if (!m_byId.erase(id.value)) return false;
    if (handle.isValid() && m_handles.resolve(handle) == id) {
        m_handles.release(handle);
        m_byHandleIndex[handle.index()] = Entity();
    }
    return true;
}

Entity EntityRegistry::find(NetHandle handle) const noexcept {
    if (!m_handles.isLive(handle)) return Entity();
    const u32 index = handle.index();
    return index < m_byHandleIndex.size() ? m_byHandleIndex[index] : Entity();
}

usize EntityRegistry::memoryBytes() const noexcept {
    return m_byId.memoryBytes() + m_byHandleIndex.capacity() * sizeof(Entity);
}

} // namespace helios::ecs
