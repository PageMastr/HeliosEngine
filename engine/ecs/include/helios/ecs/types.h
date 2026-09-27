#pragma once
// helios::ecs value types (02 §4.1): the three entity ID kinds and the identity/dirty components.
//
//   | ID          | Width | Scope              | Notes                                              |
//   |-------------|-------|--------------------|----------------------------------------------------|
//   | EntityId    | u64   | global, persistent | 0…: block ids 41/5/17 (entity_id.h);               |
//   |             |       |                    | 10…: hash62(content GUID); 11…: client-local       |
//   | NetHandle   | u32   | zone instance      | 24-bit index + 8-bit generation (04 §4.6)          |
//   | Entity      | u64   | process            | the flecs entity; never serialized                 |
//
// This header is flecs-free: flecs is confined to engine/ecs/src (plus World::flecsWorld() as an
// explicit escape hatch), so gameplay code never compiles the 40k-line flecs header.
// Threading: plain value types.

#include <compare>
#include <cstddef>
#include <functional>

#include "helios/core/hash.h"
#include "helios/core/types.h"

struct ecs_world_t; // flecs world (opaque here)
struct ecs_table_t; // flecs table (opaque here)
struct ecs_record_t; // flecs entity record (opaque here)

namespace helios {
struct Guid;
}

namespace helios::ecs {

/// flecs id of a component, tag or relationship pair. Process-local; never serialize it.
using ComponentId = u64;
/// Simulation tick counter of a World (starts at 0, incremented by World::beginTick()).
using Tick = u64;
/// Authority-group id (ADR-007): ship + contents share one single-writer group.
using AgId = u64;
/// Per-field dirty bits of a replicated component (bit i = field i of kReplicatedFields).
using FieldMask = u64;

inline constexpr u32 kMaxReplicatedFields = 64;
/// At most 64 replicated component types per world (one bit each in RepDirty::componentMask).
inline constexpr u32 kMaxReplicatedComponents = 64;

/// A flecs entity (process-local, includes flecs' 32-bit generation). Never serialize it; use
/// EntityId for persistence and NetHandle on the wire.
struct Entity {
    u64 id = 0;

    constexpr Entity() noexcept = default;
    constexpr explicit Entity(u64 raw) noexcept : id(raw) {}
    constexpr bool isValid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    /// Index part (low 32 bits) of the flecs id.
    constexpr u32 index() const noexcept { return static_cast<u32>(id); }
    friend constexpr bool operator==(Entity, Entity) noexcept = default;
    friend constexpr auto operator<=>(Entity, Entity) noexcept = default;
};

enum class EntityIdKind : u8 {
    Invalid = 0,
    Runtime,       ///< Block id (41/5/17) minted at spawn (top bit 0; entity_id.h).
    ContentPlaced, ///< hash62 of the entity GUID in its .hent (top bits 10).
    ClientLocal,   ///< Client-only entities (top bits 11): VFX, UI proxies, predicted spawns.
};

/// Global, persistent 64-bit entity id (ADR-004).
struct EntityId {
    static constexpr u64 kContentTag = 2ull << 62;
    static constexpr u64 kClientTag = 3ull << 62;
    static constexpr u64 kPayload62 = (1ull << 62) - 1;

    u64 value = 0;

    constexpr EntityId() noexcept = default;
    constexpr explicit EntityId(u64 raw) noexcept : value(raw) {}

    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr EntityIdKind kind() const noexcept {
        if (value == 0) return EntityIdKind::Invalid;
        if ((value >> 63) == 0) return EntityIdKind::Runtime;
        return (value >> 62) == 2 ? EntityIdKind::ContentPlaced : EntityIdKind::ClientLocal;
    }

    /// Content-placed id from a 64-bit hash of the entity GUID (only the low 62 bits are kept; a
    /// zero payload is remapped to 1 so the id is always valid).
    static constexpr EntityId contentPlaced(u64 guidHash) noexcept {
        const u64 payload = guidHash & kPayload62;
        return EntityId(kContentTag | (payload == 0 ? 1 : payload));
    }
    /// Content-placed id of an entity GUID: hash62(XXH3(guid bytes, seed "helios.entity")).
    /// Stable across platforms and processes; the cook rejects collisions.
    static EntityId fromContentGuid(const Guid& guid) noexcept;
    /// Client-local id for sequence number `sequence` (low 62 bits).
    static constexpr EntityId clientLocal(u64 sequence) noexcept {
        return EntityId(kClientTag | (sequence & kPayload62));
    }

    friend constexpr bool operator==(EntityId, EntityId) noexcept = default;
    friend constexpr auto operator<=>(EntityId, EntityId) noexcept = default;
};

/// Zone-instance-scoped 32-bit wire alias of an EntityId: 24-bit slot index + 8-bit generation.
/// Value 0 is invalid (slot 0 is never issued).
struct NetHandle {
    static constexpr u32 kIndexBits = 24;
    static constexpr u32 kMaxIndex = (1u << kIndexBits) - 1;

    u32 value = 0;

    constexpr NetHandle() noexcept = default;
    constexpr explicit NetHandle(u32 raw) noexcept : value(raw) {}
    static constexpr NetHandle make(u32 index, u8 generation) noexcept {
        return NetHandle((static_cast<u32>(generation) << kIndexBits) | (index & kMaxIndex));
    }
    constexpr u32 index() const noexcept { return value & kMaxIndex; }
    constexpr u8 generation() const noexcept { return static_cast<u8>(value >> kIndexBits); }
    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    friend constexpr bool operator==(NetHandle, NetHandle) noexcept = default;
    friend constexpr auto operator<=>(NetHandle, NetHandle) noexcept = default;
};

/// Carried by every entity spawned through World (02 §4.1). Never inherited from prefabs.
struct NetIdentity {
    EntityId id;
    NetHandle handle;
    u32 reserved = 0;
    AgId ag = 0;
};

/// Per-entity replication summary (02 §4.4; "RepState" in 04 §4.2). Added automatically with the
/// first replicated component. Bits are set by Mut<C> writes (atomically, so parallel systems that
/// write *different* components of one entity may both mark it) and cleared by
/// World::gatherChanges().
struct alignas(8) RepDirty {
    u64 componentMask = 0; ///< Bit = ComponentInfo::replIndex of each dirty replicated component.
    Tick changed = 0;      ///< Tick of the most recent write.
};

} // namespace helios::ecs

template <>
struct std::hash<helios::ecs::Entity> {
    std::size_t operator()(helios::ecs::Entity e) const noexcept { return static_cast<std::size_t>(helios::mix64(e.id)); }
};
template <>
struct std::hash<helios::ecs::EntityId> {
    std::size_t operator()(helios::ecs::EntityId e) const noexcept {
        return static_cast<std::size_t>(helios::mix64(e.value));
    }
};
template <>
struct std::hash<helios::ecs::NetHandle> {
    std::size_t operator()(helios::ecs::NetHandle h) const noexcept {
        return static_cast<std::size_t>(helios::mix64(h.value));
    }
};
