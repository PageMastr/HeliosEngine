#pragma once
// Per-field dirty tracking for replication (02 §4.4, 04 §4.2).
//
// Mut<C> is the sanctioned way to write a replicated component:
//   * set<&C::field>(v)  — writes the field and, if the value changed (operator== when available),
//                          sets the field's bit in C::_dirty;
//   * mark<&C::field>()  — marks a field written through another path;
//   * raw()              — returns C& and marks every field dirty (04's quantized comparison then
//                          filters out noise).
// World::set / setRaw / CommandBuffer::set on a component the entity already owns count as raw()
// writes (every field marked, pending bits kept). Adding a component (or spawning with it) resets
// its _dirty mask: the structural log's Create/Add already makes replication send the full value.
// World::getMut / getMutRaw are the only untracked write paths (server-only data, initialization).
// Any newly set bit also ORs the component's replIndex bit into the entity's RepDirty summary and
// stamps RepDirty::changed with the current tick. RepDirty is updated with relaxed atomics
// (std::atomic_ref), so two parallel systems writing *different* components of the same entity are
// race-free; the component itself is exclusively owned by the system that declared the write.
//
// World::gatherChanges() consumes and clears the bits once per tick, producing a ChangeList in a
// deterministic order (query table order, then row), which is what replication serializes.
//
// Threading: a Mut<C> is used by one thread; see above for concurrent Mut<> on one entity.

#include <atomic>
#include <concepts>
#include <utility>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/types.h"
#include "helios/ecs/component.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

template <class C>
class Mut {
public:
    Mut() noexcept = default;
    /// `rep` may be null (the entity has no RepDirty yet); `componentBit` = 1 << replIndex.
    Mut(C* component, RepDirty* rep, u64 componentBit, Tick tick) noexcept
        : m_ptr(component), m_rep(rep), m_componentBit(componentBit), m_tick(tick) {}

    bool isValid() const noexcept { return m_ptr != nullptr; }
    explicit operator bool() const noexcept { return isValid(); }

    const C& get() const noexcept { return *m_ptr; }
    const C* operator->() const noexcept { return m_ptr; }

    /// Writes one replicated field; marks it dirty only if the value changed.
    template <auto Member, class V>
    void set(V&& value) {
        static_assert(ReplicatedComponent<C>, "Mut<C>::set needs a replicated component");
        constexpr u32 index = kFieldIndex<C, Member>;
        static_assert(index != kNoField, "member is not listed in C::kReplicatedFields");
        auto& field = m_ptr->*Member;
        using F = std::remove_cvref_t<decltype(field)>;
        if constexpr (std::equality_comparable<F>) {
            if (field == value) return;
        }
        field = std::forward<V>(value);
        markBits(FieldMask(1) << index);
    }

    /// Marks one field dirty without writing it.
    template <auto Member>
    void mark() {
        constexpr u32 index = kFieldIndex<C, Member>;
        static_assert(index != kNoField, "member is not listed in C::kReplicatedFields");
        markBits(FieldMask(1) << index);
    }

    /// Mutable access to the whole component; conservatively marks every replicated field.
    C& raw() {
        if constexpr (ReplicatedComponent<C>) markBits(allFieldsMask(kReplicatedFieldCount<C>));
        return *m_ptr;
    }

    /// Field bits currently dirty on this component.
    FieldMask dirtyFields() const noexcept {
        if constexpr (ReplicatedComponent<C>) {
            return m_ptr->_dirty;
        } else {
            return 0;
        }
    }

private:
    void markBits(FieldMask bits) {
        if constexpr (ReplicatedComponent<C>) {
            const FieldMask before = m_ptr->_dirty;
            const FieldMask after = before | bits;
            if (after != before) m_ptr->_dirty = after;
            if (!m_rep) return;
            // The summary is checked even when the field bits were already set: bits can exist
            // without it (a value copied in with its _dirty mask), and gatherChanges() only visits
            // entities whose summary bit is set, so skipping it would lose this write.
            std::atomic_ref<u64> mask(m_rep->componentMask);
            const bool summarized = (mask.load(std::memory_order_relaxed) & m_componentBit) != 0;
            if (summarized && after == before) return; // nothing new
            if (!summarized) mask.fetch_or(m_componentBit, std::memory_order_relaxed);
            std::atomic_ref<u64>(m_rep->changed).store(m_tick, std::memory_order_relaxed);
        }
    }

    C* m_ptr = nullptr;
    RepDirty* m_rep = nullptr;
    u64 m_componentBit = 0;
    Tick m_tick = 0;
};

static_assert(std::atomic_ref<u64>::required_alignment <= alignof(RepDirty));

/// A column of C plus the matching RepDirty column: column[row] yields a Mut<C> for that row.
template <class C>
class MutColumn {
public:
    MutColumn(C* column, RepDirty* repColumn, u64 componentBit, Tick tick) noexcept
        : m_column(column), m_rep(repColumn), m_bit(componentBit), m_tick(tick) {}
    Mut<C> operator[](u32 row) const noexcept {
        return Mut<C>(m_column + row, m_rep ? m_rep + row : nullptr, m_bit, m_tick);
    }
    /// Read-only view of the column (no dirty marks).
    const C* data() const noexcept { return m_column; }

private:
    C* m_column;
    RepDirty* m_rep;
    u64 m_bit;
    Tick m_tick;
};

/// One dirty replicated component of one entity.
struct ComponentChange {
    EntityId entity;
    NetHandle handle;
    u8 replIndex = 0;   ///< ComponentInfo::replIndex.
    FieldMask fields = 0;
    friend bool operator==(const ComponentChange&, const ComponentChange&) = default;
};

/// Everything replication needs from one tick's writes (consumed by 04's gather stage).
struct ChangeList {
    Tick tick = 0;
    u32 entityCount = 0;                 ///< Distinct entities with at least one change.
    std::vector<ComponentChange> changes; ///< Grouped by entity; deterministic order.
    void clear() noexcept {
        tick = 0;
        entityCount = 0;
        changes.clear();
    }
};

} // namespace helios::ecs
