#pragma once
// Named per-field mutators for replicated components (02 §4.4, 04 §4.2).
//
// helios-schemac gives every replicated component (`component X replicate(...)`) a hidden
// `u64 _dirty` field mask and `static constexpr auto kReplicatedFields = std::make_tuple(&X::a, ...)`
// in wire order — the contract engine/ecs's `ecs::Mut<C>` (`set<&C::field>(v)`) works with — and
// specializes `reflect::Mut<X>` with named setters:
//
//   reflect::Mut<ShipMotion> m(motion, &repDirty.componentMask, 1ull << replIndex);
//   m.setVel(v);          // writes, and if the value changed sets bit "vel" in motion._dirty and the
//                         // component bit in the entity summary
//   m.raw().pos = p;      // marks every replicated field dirty
//
// Both mutators set the same bits (bit i = FieldInfo::repIndex i = kReplicatedFields index i).
// Server-part fields (`server {}` blocks) live in X::Server and are never replicated.
//
// Threading: a Mut is used by one thread; the entity summary is updated with a plain OR (use
// ecs::Mut for concurrent writers of different components of one entity).

#include "helios/core/types.h"

namespace helios::refl {

/// Specialized by generated code for each replicated component.
template <class C>
class Mut;

/// Dirty field bits of a replicated component.
template <class C>
constexpr u64 dirtyFields(const C& component) noexcept {
    return component._dirty;
}
template <class C>
constexpr void clearDirty(C& component) noexcept {
    component._dirty = 0;
}

namespace detail {
/// Shared implementation of generated Mut<C> specializations.
template <class C>
class MutBase {
public:
    /// `entityMask`/`componentBit`: optional per-entity summary (e.g. RepDirty::componentMask).
    explicit MutBase(C& component, u64* entityMask = nullptr, u64 componentBit = 0) noexcept
        : m_c(&component), m_entityMask(entityMask), m_componentBit(componentBit) {}

    const C& get() const noexcept { return *m_c; }
    /// Field bits currently dirty.
    u64 dirtyFields() const noexcept { return m_c->_dirty; }
    /// Marks field bits dirty without writing.
    void markBits(u64 bits) noexcept {
        m_c->_dirty |= bits;
        if (bits != 0 && m_entityMask) *m_entityMask |= m_componentBit;
    }

protected:
    C* m_c;
    u64* m_entityMask;
    u64 m_componentBit;
};
} // namespace detail

} // namespace helios::refl
