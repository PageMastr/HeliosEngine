#pragma once
// Containers.
//
// * SmallVector<T, N>: vector with N elements of inline storage; spills to the tracked heap
//   (MemoryTag::Containers) beyond that. Not thread-safe.
// * RingBuffer<T>: fixed-capacity FIFO/circular buffer (push fails or overwrites when full).
//   Not thread-safe.
// * MpmcQueue<T>: bounded lock-free multi-producer/multi-consumer queue (Vyukov). Thread-safe.
// * SpscQueue<T>: bounded lock-free single-producer/single-consumer queue. Exactly one producer
//   thread and one consumer thread.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "helios/core/assert.h"
#include "helios/core/memory.h"
#include "helios/core/types.h"

#if defined(HELIOS_COMPILER_MSVC)
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignment specifier (intended: false sharing)
#endif

namespace helios {

template <class T, usize N>
class SmallVector {
    static_assert(N > 0, "use std::vector for N == 0");

public:
    using value_type = T;
    using size_type = usize;
    using iterator = T*;
    using const_iterator = const T*;
    using reference = T&;
    using const_reference = const T&;

    SmallVector() noexcept = default;
    SmallVector(std::initializer_list<T> init) { assign(init.begin(), init.end()); }
    explicit SmallVector(usize count, const T& value = T()) {
        reserve(count);
        for (usize i = 0; i < count; ++i) ::new (static_cast<void*>(m_data + i)) T(value);
        m_size = count;
    }
    SmallVector(const SmallVector& o) { assign(o.begin(), o.end()); }
    SmallVector(SmallVector&& o) noexcept(std::is_nothrow_move_constructible_v<T>) { moveFrom(std::move(o)); }
    ~SmallVector() {
        clear();
        freeHeap();
    }

    SmallVector& operator=(const SmallVector& o) {
        if (this != &o) {
            clear();
            assign(o.begin(), o.end());
        }
        return *this;
    }
    SmallVector& operator=(SmallVector&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &o) {
            clear();
            freeHeap();
            moveFrom(std::move(o));
        }
        return *this;
    }

    T* data() noexcept { return m_data; }
    const T* data() const noexcept { return m_data; }
    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    bool empty() const noexcept { return m_size == 0; }
    /// True while elements live in the inline buffer.
    bool isInline() const noexcept { return m_data == inlineData(); }
    static constexpr usize inlineCapacity() noexcept { return N; }

    T& operator[](usize i) noexcept {
        HELIOS_ASSERT(i < m_size, "SmallVector index {} out of range {}", i, m_size);
        return m_data[i];
    }
    const T& operator[](usize i) const noexcept {
        HELIOS_ASSERT(i < m_size, "SmallVector index {} out of range {}", i, m_size);
        return m_data[i];
    }
    T& front() noexcept { return (*this)[0]; }
    const T& front() const noexcept { return (*this)[0]; }
    T& back() noexcept { return (*this)[m_size - 1]; }
    const T& back() const noexcept { return (*this)[m_size - 1]; }

    T* begin() noexcept { return m_data; }
    T* end() noexcept { return m_data + m_size; }
    const T* begin() const noexcept { return m_data; }
    const T* end() const noexcept { return m_data + m_size; }

    void reserve(usize newCapacity) {
        if (newCapacity > m_capacity) grow(newCapacity);
    }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (m_size == m_capacity) {
            // Construct first: args may reference an element that grow() would move.
            T tmp(std::forward<Args>(args)...);
            grow(m_capacity * 2);
            ::new (static_cast<void*>(m_data + m_size)) T(std::move(tmp));
        } else {
            ::new (static_cast<void*>(m_data + m_size)) T(std::forward<Args>(args)...);
        }
        return m_data[m_size++];
    }

    void pop_back() noexcept {
        HELIOS_ASSERT(m_size > 0);
        m_data[--m_size].~T();
    }

    void clear() noexcept {
        std::destroy(m_data, m_data + m_size);
        m_size = 0;
    }

    void resize(usize count) {
        if (count < m_size) {
            std::destroy(m_data + count, m_data + m_size);
        } else {
            reserve(count);
            for (usize i = m_size; i < count; ++i) ::new (static_cast<void*>(m_data + i)) T();
        }
        m_size = count;
    }

    T* insert(const T* pos, T value) {
        const usize index = static_cast<usize>(pos - m_data);
        HELIOS_ASSERT(index <= m_size);
        emplace_back(std::move(value));
        std::rotate(m_data + index, m_data + m_size - 1, m_data + m_size);
        return m_data + index;
    }

    T* erase(const T* pos) {
        const usize index = static_cast<usize>(pos - m_data);
        HELIOS_ASSERT(index < m_size);
        std::move(m_data + index + 1, m_data + m_size, m_data + index);
        pop_back();
        return m_data + index;
    }

    /// O(1) erase that does not preserve order.
    void eraseUnordered(usize index) {
        HELIOS_ASSERT(index < m_size);
        if (index != m_size - 1) m_data[index] = std::move(m_data[m_size - 1]);
        pop_back();
    }

    friend bool operator==(const SmallVector& a, const SmallVector& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

private:
    T* inlineData() noexcept { return std::launder(reinterpret_cast<T*>(m_inline)); }
    const T* inlineData() const noexcept { return std::launder(reinterpret_cast<const T*>(m_inline)); }

    template <class It>
    void assign(It first, It last) {
        const usize count = static_cast<usize>(std::distance(first, last));
        reserve(count);
        std::uninitialized_copy(first, last, m_data);
        m_size = count;
    }

    void grow(usize newCapacity) {
        if (newCapacity < N) newCapacity = N;
        T* fresh = static_cast<T*>(alignedAlloc(newCapacity * sizeof(T), alignof(T), MemoryTag::Containers));
        if (!fresh) throw std::bad_alloc();
        std::uninitialized_move(m_data, m_data + m_size, fresh);
        std::destroy(m_data, m_data + m_size);
        freeHeap();
        m_data = fresh;
        m_capacity = newCapacity;
    }

    void freeHeap() noexcept {
        if (m_data != inlineData()) {
            alignedFree(m_data);
            m_data = inlineData();
            m_capacity = N;
        }
    }

    void moveFrom(SmallVector&& o) {
        if (o.isInline()) {
            std::uninitialized_move(o.m_data, o.m_data + o.m_size, m_data);
            m_size = o.m_size;
            o.clear();
        } else {
            m_data = std::exchange(o.m_data, o.inlineData());
            m_capacity = std::exchange(o.m_capacity, N);
            m_size = std::exchange(o.m_size, 0);
        }
    }

    alignas(T) unsigned char m_inline[sizeof(T) * N];
    T* m_data = inlineData();
    usize m_size = 0;
    usize m_capacity = N;
};

/// Fixed-capacity circular buffer; index 0 is the oldest element.
template <class T>
class RingBuffer {
public:
    explicit RingBuffer(usize capacity) : m_capacity(capacity) {
        HELIOS_ASSERT(capacity > 0);
        m_items = static_cast<T*>(alignedAlloc(capacity * sizeof(T), alignof(T), MemoryTag::Containers));
        if (!m_items) throw std::bad_alloc();
    }
    ~RingBuffer() {
        clear();
        alignedFree(m_items);
    }
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    usize size() const noexcept { return m_size; }
    usize capacity() const noexcept { return m_capacity; }
    bool empty() const noexcept { return m_size == 0; }
    bool full() const noexcept { return m_size == m_capacity; }

    /// Appends; returns false (unchanged) when full.
    template <class U>
    bool pushBack(U&& value) {
        if (full()) return false;
        ::new (static_cast<void*>(m_items + physical(m_size))) T(std::forward<U>(value));
        ++m_size;
        return true;
    }
    /// Appends, dropping the oldest element when full.
    template <class U>
    void pushBackOverwrite(U&& value) {
        if (full()) popFront();
        pushBack(std::forward<U>(value));
    }
    /// Removes the oldest element into `out`; false if empty.
    bool popFront(T& out) {
        if (empty()) return false;
        out = std::move(front());
        popFront();
        return true;
    }
    void popFront() noexcept {
        HELIOS_ASSERT(!empty());
        m_items[m_head].~T();
        m_head = (m_head + 1) % m_capacity;
        --m_size;
    }
    void popBack() noexcept {
        HELIOS_ASSERT(!empty());
        m_items[physical(m_size - 1)].~T();
        --m_size;
    }

    T& front() noexcept { return m_items[m_head]; }
    const T& front() const noexcept { return m_items[m_head]; }
    T& back() noexcept { return m_items[physical(m_size - 1)]; }
    const T& back() const noexcept { return m_items[physical(m_size - 1)]; }
    T& operator[](usize i) noexcept {
        HELIOS_ASSERT(i < m_size);
        return m_items[physical(i)];
    }
    const T& operator[](usize i) const noexcept {
        HELIOS_ASSERT(i < m_size);
        return m_items[physical(i)];
    }

    void clear() noexcept {
        while (!empty()) popFront();
        m_head = 0;
    }

    template <class F>
    void forEach(F&& fn) const {
        for (usize i = 0; i < m_size; ++i) fn(m_items[physical(i)]);
    }

private:
    usize physical(usize i) const noexcept { return (m_head + i) % m_capacity; }
    T* m_items = nullptr;
    usize m_capacity;
    usize m_head = 0;
    usize m_size = 0;
};

/// Bounded MPMC queue (Dmitry Vyukov's algorithm). Capacity is rounded up to a power of two.
/// tryPush/tryPop never block and are safe from any number of threads.
template <class T>
class MpmcQueue {
public:
    explicit MpmcQueue(usize capacity) {
        m_capacity = nextPowerOfTwo<usize>(capacity < 2 ? 2 : capacity);
        m_mask = m_capacity - 1;
        m_cells = static_cast<Cell*>(alignedAlloc(sizeof(Cell) * m_capacity, alignof(Cell), MemoryTag::Containers));
        if (!m_cells) throw std::bad_alloc();
        for (usize i = 0; i < m_capacity; ++i) ::new (&m_cells[i]) Cell(i);
    }
    ~MpmcQueue() {
        // No concurrent access during destruction: every cell in [dequeue, enqueue) is occupied.
        const usize end = m_enqueuePos.load(std::memory_order_relaxed);
        for (usize pos = m_dequeuePos.load(std::memory_order_relaxed); pos != end; ++pos) {
            std::launder(reinterpret_cast<T*>(m_cells[pos & m_mask].storage))->~T();
        }
        for (usize i = 0; i < m_capacity; ++i) m_cells[i].~Cell();
        alignedFree(m_cells);
    }
    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    usize capacity() const noexcept { return m_capacity; }

    template <class U>
    bool tryPush(U&& value) {
        Cell* cell;
        usize pos = m_enqueuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const usize seq = cell->sequence.load(std::memory_order_acquire);
            const isize diff = static_cast<isize>(seq) - static_cast<isize>(pos);
            if (diff == 0) {
                if (m_enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = m_enqueuePos.load(std::memory_order_relaxed);
            }
        }
        ::new (static_cast<void*>(cell->storage)) T(std::forward<U>(value));
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) {
        Cell* cell;
        usize pos = m_dequeuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const usize seq = cell->sequence.load(std::memory_order_acquire);
            const isize diff = static_cast<isize>(seq) - static_cast<isize>(pos + 1);
            if (diff == 0) {
                if (m_dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = m_dequeuePos.load(std::memory_order_relaxed);
            }
        }
        T* item = std::launder(reinterpret_cast<T*>(cell->storage));
        out = std::move(*item);
        item->~T();
        cell->sequence.store(pos + m_mask + 1, std::memory_order_release);
        return true;
    }

private:
    struct alignas(HELIOS_CACHE_LINE_SIZE) Cell {
        explicit Cell(usize seq) noexcept : sequence(seq) {}
        std::atomic<usize> sequence;
        alignas(T) unsigned char storage[sizeof(T)];
    };

    Cell* m_cells = nullptr;
    usize m_capacity = 0;
    usize m_mask = 0;
    alignas(HELIOS_CACHE_LINE_SIZE) std::atomic<usize> m_enqueuePos{0};
    alignas(HELIOS_CACHE_LINE_SIZE) std::atomic<usize> m_dequeuePos{0};
};

/// Bounded wait-free SPSC queue. Capacity is rounded up to a power of two.
template <class T>
class SpscQueue {
public:
    explicit SpscQueue(usize capacity) {
        m_capacity = nextPowerOfTwo<usize>(capacity < 2 ? 2 : capacity);
        m_mask = m_capacity - 1;
        m_items = static_cast<T*>(alignedAlloc(sizeof(T) * m_capacity, alignof(T), MemoryTag::Containers));
        if (!m_items) throw std::bad_alloc();
    }
    ~SpscQueue() {
        const usize tail = m_tail.load(std::memory_order_relaxed);
        for (usize i = m_head.load(std::memory_order_relaxed); i != tail; ++i) m_items[i & m_mask].~T();
        alignedFree(m_items);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    usize capacity() const noexcept { return m_capacity; }

    /// Producer thread only.
    template <class U>
    bool tryPush(U&& value) {
        const usize tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_cachedHead == m_capacity) {
            m_cachedHead = m_head.load(std::memory_order_acquire);
            if (tail - m_cachedHead == m_capacity) return false;
        }
        ::new (static_cast<void*>(m_items + (tail & m_mask))) T(std::forward<U>(value));
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer thread only.
    bool tryPop(T& out) {
        const usize head = m_head.load(std::memory_order_relaxed);
        if (head == m_cachedTail) {
            m_cachedTail = m_tail.load(std::memory_order_acquire);
            if (head == m_cachedTail) return false;
        }
        T* item = m_items + (head & m_mask);
        out = std::move(*item);
        item->~T();
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Approximate size (exact when called from either endpoint while the other is idle).
    usize sizeApprox() const noexcept {
        return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire);
    }

private:
    T* m_items = nullptr;
    usize m_capacity = 0;
    usize m_mask = 0;
    alignas(HELIOS_CACHE_LINE_SIZE) std::atomic<usize> m_head{0}; // consumer-owned
    usize m_cachedTail = 0;                                        // consumer-local
    alignas(HELIOS_CACHE_LINE_SIZE) std::atomic<usize> m_tail{0}; // producer-owned
    usize m_cachedHead = 0;                                        // producer-local
};

} // namespace helios

#if defined(HELIOS_COMPILER_MSVC)
#pragma warning(pop)
#endif
