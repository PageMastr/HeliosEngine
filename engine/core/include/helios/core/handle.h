#pragma once
// Generational handles and a slot-map pool.
//
// Handle<Tag> is a 64-bit (32-bit index + 32-bit generation) reference to an object owned by a
// HandlePool. When an object is destroyed its slot's generation is bumped, so every outstanding
// handle to it becomes detectably stale instead of dangling. Tags make handles of different pools
// distinct types. Handles are process-local: never serialize them to disk or the network.
//
// HandlePool<T, Tag> stores objects in fixed-size chunks, so element addresses stay stable until
// the element is destroyed (creating more elements never moves existing ones). Freed slots are
// reused LIFO; a slot whose generation would wrap is retired permanently.
//
// Threading: Handle is a value type. HandlePool is NOT thread-safe; guard it externally or confine
// it to one thread (e.g. the owning subsystem's thread).

#include <compare>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/types.h"

namespace helios {

template <class Tag>
class Handle {
public:
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

    constexpr Handle() noexcept = default;
    constexpr Handle(u32 index, u32 generation) noexcept : m_index(index), m_generation(generation) {}

    constexpr u32 index() const noexcept { return m_index; }
    constexpr u32 generation() const noexcept { return m_generation; }
    /// True if the handle was ever issued by a pool (it may still be stale; ask the pool).
    constexpr bool isValid() const noexcept { return m_generation != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }

    /// Packs into 64 bits (generation in the high half) for storage in generic slots.
    constexpr u64 toBits() const noexcept { return (static_cast<u64>(m_generation) << 32) | m_index; }
    static constexpr Handle fromBits(u64 bits) noexcept {
        return Handle(static_cast<u32>(bits & 0xFFFFFFFFu), static_cast<u32>(bits >> 32));
    }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Handle a, Handle b) noexcept {
        return a.toBits() <=> b.toBits();
    }

private:
    u32 m_index = kInvalidIndex;
    u32 m_generation = 0;
};

template <class T, class Tag = T>
class HandlePool {
public:
    using HandleType = Handle<Tag>;
    static constexpr u32 kChunkBits = 10;
    static constexpr u32 kChunkSize = 1u << kChunkBits;

    /// Proxy returned by iteration: `for (auto [handle, value] : pool)`.
    template <class V>
    struct Item {
        HandleType handle;
        V& value;
    };

    template <class Pool, class V>
    class Iterator {
    public:
        Iterator(Pool* pool, u32 index) noexcept : m_pool(pool), m_index(index) { skipDead(); }
        Item<V> operator*() const noexcept {
            auto& slot = m_pool->slot(m_index);
            return Item<V>{HandleType(m_index, slot.generation), *slot.ptr()};
        }
        Iterator& operator++() noexcept {
            ++m_index;
            skipDead();
            return *this;
        }
        bool operator==(const Iterator& o) const noexcept { return m_index == o.m_index; }

    private:
        void skipDead() noexcept {
            while (m_index < m_pool->m_slotCount && !m_pool->slot(m_index).alive) ++m_index;
        }
        Pool* m_pool;
        u32 m_index;
    };

    using iterator = Iterator<HandlePool, T>;
    using const_iterator = Iterator<const HandlePool, const T>;

    HandlePool() = default;
    ~HandlePool() { clear(); }
    HandlePool(const HandlePool&) = delete;
    HandlePool& operator=(const HandlePool&) = delete;
    HandlePool(HandlePool&& o) noexcept { swap(o); }
    HandlePool& operator=(HandlePool&& o) noexcept {
        if (this != &o) {
            clear();
            m_chunks.clear();
            m_slotCount = 0;
            m_freeHead = kNoSlot;
            m_live = 0;
            swap(o);
        }
        return *this;
    }

    /// Constructs a new element in place and returns its handle. If T's constructor throws, the
    /// pool is left unchanged (the slot is not lost).
    template <class... Args>
    HandleType create(Args&&... args) {
        const bool reuse = m_freeHead != kNoSlot;
        u32 index;
        if (reuse) {
            index = m_freeHead;
        } else {
            HELIOS_VERIFY(m_slotCount < HandleType::kInvalidIndex, "HandlePool exhausted");
            index = m_slotCount;
            if ((index >> kChunkBits) >= m_chunks.size()) m_chunks.push_back(std::make_unique<Slot[]>(kChunkSize));
        }
        Slot& s = slot(index);
        ::new (static_cast<void*>(s.storage)) T(std::forward<Args>(args)...);
        // Commit only after construction succeeded.
        if (reuse) {
            m_freeHead = s.nextFree;
        } else {
            ++m_slotCount;
        }
        s.alive = true;
        s.nextFree = kNoSlot;
        ++m_live;
        return HandleType(index, s.generation);
    }

    /// Destroys the element. Returns false (and does nothing) for stale or invalid handles.
    bool destroy(HandleType handle) {
        Slot* s = liveSlot(handle);
        if (!s) return false;
        s->ptr()->~T();
        s->alive = false;
        --m_live;
        if (++s->generation != 0) {
            s->nextFree = m_freeHead;
            m_freeHead = handle.index();
        } // else: generation wrapped; retire the slot forever so old handles can never match again.
        return true;
    }

    /// Element pointer, or nullptr if the handle is stale/invalid.
    T* get(HandleType handle) noexcept {
        Slot* s = liveSlot(handle);
        return s ? s->ptr() : nullptr;
    }
    const T* get(HandleType handle) const noexcept {
        const Slot* s = const_cast<HandlePool*>(this)->liveSlot(handle);
        return s ? s->ptr() : nullptr;
    }

    bool isValid(HandleType handle) const noexcept { return get(handle) != nullptr; }

    /// Number of live elements.
    u32 size() const noexcept { return m_live; }
    bool empty() const noexcept { return m_live == 0; }
    /// Number of slots ever allocated (live + free + retired).
    u32 slotCount() const noexcept { return m_slotCount; }

    /// Destroys all elements; every outstanding handle becomes stale. Keeps slot memory.
    void clear() {
        m_freeHead = kNoSlot;
        for (u32 i = m_slotCount; i-- > 0;) {
            Slot& s = slot(i);
            if (s.alive) {
                s.ptr()->~T();
                s.alive = false;
                ++s.generation;
            }
            if (s.generation != 0) {
                s.nextFree = m_freeHead;
                m_freeHead = i;
            }
        }
        m_live = 0;
    }

    /// Calls fn(handle, value) for each live element in slot order.
    template <class F>
    void forEach(F&& fn) {
        for (u32 i = 0; i < m_slotCount; ++i) {
            Slot& s = slot(i);
            if (s.alive) fn(HandleType(i, s.generation), *s.ptr());
        }
    }

    iterator begin() noexcept { return iterator(this, 0); }
    iterator end() noexcept { return iterator(this, m_slotCount); }
    const_iterator begin() const noexcept { return const_iterator(this, 0); }
    const_iterator end() const noexcept { return const_iterator(this, m_slotCount); }

private:
    static constexpr u32 kNoSlot = 0xFFFFFFFFu;

    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];
        u32 generation = 1;
        u32 nextFree = kNoSlot;
        bool alive = false;

        T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        const T* ptr() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }
    };

    Slot& slot(u32 index) noexcept { return m_chunks[index >> kChunkBits][index & (kChunkSize - 1)]; }
    const Slot& slot(u32 index) const noexcept { return m_chunks[index >> kChunkBits][index & (kChunkSize - 1)]; }

    Slot* liveSlot(HandleType handle) noexcept {
        if (handle.index() >= m_slotCount) return nullptr;
        Slot& s = slot(handle.index());
        return (s.alive && s.generation == handle.generation()) ? &s : nullptr;
    }

    void swap(HandlePool& o) noexcept {
        std::swap(m_chunks, o.m_chunks);
        std::swap(m_slotCount, o.m_slotCount);
        std::swap(m_freeHead, o.m_freeHead);
        std::swap(m_live, o.m_live);
    }

    std::vector<std::unique_ptr<Slot[]>> m_chunks;
    u32 m_slotCount = 0;
    u32 m_freeHead = kNoSlot;
    u32 m_live = 0;
};

} // namespace helios

template <class Tag>
struct std::hash<helios::Handle<Tag>> {
    std::size_t operator()(helios::Handle<Tag> h) const noexcept {
        return static_cast<std::size_t>(helios::mix64(h.toBits()));
    }
};
