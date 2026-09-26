#include "helios/ecs/registry.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "prefetch.h"

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

bool U64Map::insertNew(u64 key, u64 value) {
    HELIOS_ASSERT(key != 0, "U64Map: key 0 is reserved");
    if ((m_size + 1) * 2 > m_capacity) rehash(m_capacity * 2);
    usize i = slotOf(key);
    for (;;) {
        Slot& s = m_slots[i];
        if (s.key == key) return false;
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
    m_generations.resize(1, u8{1});
}

NetHandleTable::NetHandleTable() : NetHandleTable(Desc{}) {}

void NetHandleTable::grow(u32 index) {
    m_slots.resize(static_cast<usize>(index) + 1);
    m_generations.resize(static_cast<usize>(index) + 1, u8{1});
}

NetHandle NetHandleTable::issue(u32 index, EntityId id, u64 owner) noexcept {
    if (index >= m_slots.size()) grow(index);
    Slot& s = m_slots[index];
    HELIOS_ASSERT(!s.id.isValid());
    s.id = id;
    s.owner = owner;
    ++m_live;
    return NetHandle::make(index, m_generations[index]);
}

Result<NetHandle> NetHandleTable::allocate(EntityId id, u64 owner) {
    if (!id.isValid()) return Error{ErrorCode::InvalidArgument, "NetHandleTable: invalid EntityId"};
    const NetHandle h = tryAllocate(id, owner);
    if (!h.isValid()) return Error{ErrorCode::LimitExceeded, "NetHandleTable is full"};
    return h;
}

u32 NetHandleTable::nextIndex() noexcept {
    const usize waiting = m_freeTail - m_freeHead;
    const bool freshLeft = m_nextFresh <= m_max && m_nextFresh > m_reserved;
    if (waiting > 0 && (waiting > m_reuseDelay || !freshLeft)) {
        const u32 index = m_freeRing[m_freeHead++];
        // Bursts pop runs of scattered slots: fetch the slot a few pops ahead.
        if (constexpr usize kAhead = 8; m_freeHead + kAhead < m_freeTail) {
            detail::prefetch(&m_slots[m_freeRing[m_freeHead + kAhead]]);
        }
        compactRing();
        return index;
    }
    return freshLeft ? m_nextFresh++ : 0;
}

void NetHandleTable::compactRing() noexcept {
    if (m_freeHead >= 4096 && m_freeHead * 2 >= m_freeTail) {
        std::copy(m_freeRing.begin() + static_cast<isize>(m_freeHead), m_freeRing.begin() + static_cast<isize>(m_freeTail),
                  m_freeRing.begin());
        m_freeTail -= m_freeHead;
        m_freeHead = 0;
    }
}

NetHandle NetHandleTable::tryAllocate(EntityId id, u64 owner) noexcept {
    if (!id.isValid()) return NetHandle();
    const u32 index = nextIndex();
    return index != 0 ? issue(index, id, owner) : NetHandle();
}

void NetHandleTable::tryAllocateN(std::span<const EntityId> ids, const u64* owners, NetHandle* out) noexcept {
    const usize n = ids.size();
    if (std::any_of(ids.begin(), ids.end(), [](EntityId id) { return !id.isValid(); })) {
        for (usize i = 0; i < n; ++i) out[i] = tryAllocate(ids[i], owners[i]);
        return;
    }
    // tryAllocate()'s policy resolved once per run of slots from one source: FIFO slots while more
    // than reuseDelay wait (or no fresh slot is left), else fresh slots.
    usize i = 0;
    while (i < n) {
        const usize waiting = m_freeTail - m_freeHead;
        const bool freshLeft = m_nextFresh <= m_max && m_nextFresh > m_reserved;
        if (waiting > 0 && (waiting > m_reuseDelay || !freshLeft)) {
            const usize run = std::min(n - i, freshLeft ? waiting - m_reuseDelay : waiting);
            const u32* ring = m_freeRing.data() + m_freeHead;
            const usize ahead = std::min<usize>(8, waiting);
            for (usize k = 0; k < run; ++k, ++i) {
                if (k + ahead < waiting) detail::prefetch(&m_slots[ring[k + ahead]]);
                const u32 index = ring[k];
                Slot& s = m_slots[index];
                HELIOS_ASSERT(!s.id.isValid());
                s.id = ids[i];
                s.owner = owners[i];
                out[i] = NetHandle::make(index, m_generations[index]);
            }
            m_freeHead += run;
            m_live += static_cast<u32>(run);
            compactRing();
        } else if (freshLeft) {
            const usize run = std::min<usize>(n - i, m_max - m_nextFresh + 1);
            const u32 first = m_nextFresh;
            if (first + run > m_slots.size()) grow(static_cast<u32>(first + run - 1));
            for (usize k = 0; k < run; ++k, ++i) {
                const u32 index = first + static_cast<u32>(k);
                m_slots[index] = Slot{ids[i], owners[i]};
                out[i] = NetHandle::make(index, m_generations[index]);
            }
            m_nextFresh += static_cast<u32>(run);
            m_live += static_cast<u32>(run);
        } else {
            for (; i < n; ++i) out[i] = NetHandle(); // full
        }
    }
}

Result<NetHandle> NetHandleTable::allocateAt(u32 index, EntityId id, u64 owner) {
    if (index == 0 || index > m_reserved) {
        return makeError(ErrorCode::OutOfRange, "content handle index {} outside [1, {}]", index, m_reserved);
    }
    if (!id.isValid()) return Error{ErrorCode::InvalidArgument, "NetHandleTable: invalid EntityId"};
    if (index < m_slots.size() && m_slots[index].id.isValid()) {
        return makeError(ErrorCode::AlreadyExists, "content handle index {} in use", index);
    }
    return issue(index, id, owner);
}

bool NetHandleTable::release(NetHandle handle) { return releaseIssuedTo(handle, resolve(handle)); }

void NetHandleTable::growRing() {
    if (m_freeHead * 4 >= m_freeTail && m_freeHead > 0) { // mostly consumed: move the waiting part down
        std::copy(m_freeRing.begin() + static_cast<isize>(m_freeHead), m_freeRing.begin() + static_cast<isize>(m_freeTail),
                  m_freeRing.begin());
        m_freeTail -= m_freeHead;
        m_freeHead = 0;
        return;
    }
    m_freeRing.resize(std::max<usize>(64, m_freeRing.size() * 2));
}

NetHandle NetHandleTable::handleAt(u32 index) const noexcept {
    if (index == 0 || index >= m_slots.size() || !m_slots[index].id.isValid()) return NetHandle();
    return NetHandle::make(index, m_generations[index]);
}

// ---------------------------------------------------------------------------------------------
// EntityRegistry
// ---------------------------------------------------------------------------------------------

EntityRegistry::EntityRegistry(const NetHandleTable::Desc& handles, MemoryTag tag)
    : m_tag(tag), m_byId(tag, 1024), m_pageOf(tag, 64), m_handles(handles) {}

void EntityRegistry::ChunkFree::operator()(Page* chunk) const noexcept { alignedFree(chunk); }

EntityRegistry::Page* EntityRegistry::findPage(EntityId id) const noexcept {
    const u64 page = m_pageOf.find((id.value >> kPageBits) + 1);
    return page != 0 ? &pageAt(static_cast<u32>(page - 1)) : nullptr;
}

EntityRegistry::Page& EntityRegistry::ensurePage(EntityId id) {
    const u64 key = (id.value >> kPageBits) + 1;
    if (key == m_lastPageKey) return *m_lastPagePtr;
    if (const u64 page = m_pageOf.find(key); page != 0) {
        m_lastPageKey = key;
        m_lastPage = static_cast<u32>(page - 1);
        m_lastPagePtr = &pageAt(m_lastPage);
        return *m_lastPagePtr;
    }
    u32 index = 0;
    if (!m_freePages.empty()) {
        index = m_freePages.back();
        m_freePages.pop_back();
    } else {
        index = m_pageCount++;
        if (index / kChunkPages >= m_chunks.size()) {
            auto* chunk = static_cast<Page*>(alignedAlloc(sizeof(Page) * kChunkPages, alignof(Page), m_tag));
            HELIOS_VERIFY(chunk != nullptr, "EntityRegistry: out of memory");
            m_chunks.emplace_back(chunk);
        }
    }
    Page& p = pageAt(index);
    std::memset(static_cast<void*>(&p), 0, sizeof(Page));
    m_pageOf.insert(key, index + 1);
    m_lastPageKey = key;
    m_lastPage = index;
    m_lastPagePtr = &p;
    return p;
}

void EntityRegistry::reserve(usize additional) {
    m_byId.reserve(m_byId.size() + additional);
    m_pageOf.reserve(m_pageOf.size() + additional / kPageIds + 2);
}

Result<void> EntityRegistry::add(EntityId id, Entity entity) {
    if (!id.isValid() || !entity.isValid()) return Error{ErrorCode::InvalidArgument, "invalid id or entity"};
    if (!addNew(id, entity)) return makeError(ErrorCode::AlreadyExists, "EntityId {:#x} already registered", id.value);
    return {};
}

bool EntityRegistry::addNewSlow(EntityId id, Entity entity) {
    if (!id.isValid() || !entity.isValid()) return false;
    if (!paged(id)) return m_byId.insertNew(id.value, entity.id);
    Page& p = ensurePage(id);
    u64& slot = p.entity[id.value & (kPageIds - 1)];
    if (slot != 0) return false;
    slot = entity.id;
    ++p.live;
    ++m_pagedCount;
    return true;
}

Entity EntityRegistry::find(EntityId id) const noexcept {
    if (!paged(id)) return Entity(m_byId.find(id.value));
    const Page* p = findPage(id);
    return p ? Entity(p->entity[id.value & (kPageIds - 1)]) : Entity();
}

Result<NetHandle> EntityRegistry::assignHandle(EntityId id, u32 contentIndex) {
    const Entity entity = find(id);
    if (!entity) return Error{ErrorCode::NotFound, "assignHandle: unknown EntityId"};
    return assignHandleFor(id, entity, contentIndex);
}

NetHandle EntityRegistry::tryAssignHandle(EntityId id, Entity entity) noexcept {
    HELIOS_ASSERT(find(id) == entity, "tryAssignHandle: id is not registered for this entity");
    return m_handles.tryAllocate(id, entity.id);
}

void EntityRegistry::tryAssignHandles(std::span<const EntityId> ids, const u64* entities, NetHandle* out) noexcept {
#if HELIOS_ENABLE_ASSERTS
    for (usize i = 0; i < ids.size(); ++i) {
        HELIOS_ASSERT(find(ids[i]) == Entity(entities[i]), "tryAssignHandles: id is not registered for this entity");
    }
#endif
    m_handles.tryAllocateN(ids, entities, out);
}

Result<NetHandle> EntityRegistry::assignHandleFor(EntityId id, Entity entity, u32 contentIndex) {
    HELIOS_ASSERT(find(id) == entity, "assignHandleFor: id is not registered for this entity");
    return contentIndex != 0 ? m_handles.allocateAt(contentIndex, id, entity.id) : m_handles.allocate(id, entity.id);
}

bool EntityRegistry::removeSlow(EntityId id, NetHandle handle) {
    if (!paged(id)) {
        if (!m_byId.erase(id.value)) return false;
    } else {
        const u64 key = (id.value >> kPageBits) + 1;
        u32 index = m_lastPage;
        if (key != m_lastPageKey) {
            const u64 page = m_pageOf.find(key);
            if (page == 0) return false;
            index = static_cast<u32>(page - 1);
            m_lastPageKey = key;
            m_lastPage = index;
            m_lastPagePtr = &pageAt(index);
        }
        Page& p = pageAt(index);
        u64& slot = p.entity[id.value & (kPageIds - 1)];
        if (slot == 0) return false;
        slot = 0;
        --m_pagedCount;
        if (--p.live == 0) { // recycle the page
            m_freePages.push_back(index);
            m_pageOf.erase(key);
            m_lastPageKey = 0;
        }
    }
    if (handle.isValid()) m_handles.releaseIssuedTo(handle, id);
    return true;
}

usize EntityRegistry::memoryBytes() const noexcept {
    return m_byId.memoryBytes() + m_pageOf.memoryBytes() + m_chunks.size() * kChunkPages * sizeof(Page) +
           m_handles.memoryBytes();
}

} // namespace helios::ecs
