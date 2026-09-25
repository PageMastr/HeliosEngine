#pragma once
// Deferred structural changes (02 §4.3). During parallel stages the ECS is structurally read-only:
// systems record creates/destroys/adds/removes/sets/relationship changes into their job's
// CommandBuffer, and the World applies all buffers of a stage at the stage's sync point in
// (stage, system, job, sequence) order. Because job partitions are contiguous row ranges in
// deterministic query order, the applied sequence is the same for any worker count, so creation
// order (and therefore EntityId/NetHandle/flecs id assignment) never depends on thread timing.
//
// Entities created in a buffer are referenced through TempEntity until the buffer is applied;
// TempEntity values are only meaningful within the buffer that created them.
//
// Threading: a CommandBuffer is owned by one job/thread at a time. Payload memory comes from the
// ECS TaggedHeap in 16 KiB blocks that are kept across clear() (no steady-state allocation).

#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/types.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

class World;

/// Parameters for World::spawn / CommandBuffer::spawn.
struct SpawnDesc {
    EntityId id;                 ///< Explicit id (content-placed, restored); invalid = mint a block id.
    Entity prefab;               ///< Instantiate from this prefab (IsA).
    Entity parent;               ///< Transform parent (ChildOf / non-fragmenting Parent).
    Entity frame;                ///< Reference frame (InFrame).
    AgId ag = 0;                 ///< Authority group.
    bool netHandle = true;       ///< Issue a NetHandle (false for server-only entities).
    u32 contentHandleIndex = 0;  ///< Content-placed NetHandle slot (0 = dynamic).
};

/// An entity created in a CommandBuffer that is not applied yet.
struct TempEntity {
    u32 index = ~0u;
    constexpr bool isValid() const noexcept { return index != ~0u; }
};

/// Either a live Entity or a TempEntity of the same buffer.
class EntityRef {
public:
    static constexpr u64 kTempBit = 1ull << 63; // never set in flecs entity ids

    constexpr EntityRef() noexcept = default;
    constexpr EntityRef(Entity e) noexcept : m_bits(e.id) {}
    constexpr EntityRef(TempEntity t) noexcept : m_bits(kTempBit | t.index) {}

    constexpr bool isTemp() const noexcept { return (m_bits & kTempBit) != 0; }
    constexpr Entity entity() const noexcept { return isTemp() ? Entity() : Entity(m_bits); }
    constexpr u32 tempIndex() const noexcept { return static_cast<u32>(m_bits); }
    constexpr bool isValid() const noexcept { return m_bits != 0; }

private:
    u64 m_bits = 0;
};

enum class CommandKind : u8 {
    Spawn,
    Destroy,
    Add,
    Remove,
    Set,
    SetParent,
    ClearParent,
    SetFrame,
    Dock,
    Undock,
};

/// Resolves a component type to its id in `world` (out of line so this header stays light).
ComponentId componentIdForSlot(const World& world, u32 typeSlot) noexcept;

namespace detail {
u32 nextTypeSlot() noexcept;
}

/// Process-wide dense index of C++ type T (used for fast per-world type -> ComponentId lookup).
template <class T>
u32 typeSlot() noexcept {
    static const u32 slot = detail::nextTypeSlot();
    return slot;
}

class CommandBuffer {
public:
    /// `world` is needed only for the template helpers (type -> ComponentId).
    explicit CommandBuffer(const World* world = nullptr) noexcept : m_world(world) {}
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&& other) noexcept;
    CommandBuffer& operator=(CommandBuffer&& other) noexcept;

    void setWorld(const World* world) noexcept { m_world = world; }

    TempEntity spawn(const SpawnDesc& desc = {});
    void destroy(EntityRef e) { push(CommandKind::Destroy, e, 0); }
    void addId(EntityRef e, ComponentId id) { push(CommandKind::Add, e, id); }
    void removeId(EntityRef e, ComponentId id) { push(CommandKind::Remove, e, id); }
    /// Copies `size` bytes (a trivially copyable value) for a later set.
    void setRaw(EntityRef e, ComponentId id, const void* value, u32 size);

    template <class T>
    void add(EntityRef e) {
        addId(e, idOf<T>());
    }
    template <class T>
    void remove(EntityRef e) {
        removeId(e, idOf<T>());
    }
    /// Copy-constructs `value` into the buffer; it is copy-assigned into the entity at apply time.
    template <class T>
    void set(EntityRef e, const T& value) {
        static_assert(!std::is_empty_v<T>, "use add<T>() for tags");
        void* mem = allocatePayload(sizeof(T), alignof(T));
        ::new (mem) T(value);
        Command& cmd = push(CommandKind::Set, e, idOf<T>());
        cmd.payload = mem;
        cmd.payloadSize = static_cast<u32>(sizeof(T));
        if constexpr (!std::is_trivially_destructible_v<T>) {
            cmd.destroyPayload = [](void* p) { static_cast<T*>(p)->~T(); };
        }
    }

    /// Transform hierarchy (cycle-checked at apply; an offending command is dropped and counted).
    void setParent(EntityRef child, EntityRef parent) { push(CommandKind::SetParent, child, 0).other = parent; }
    void clearParent(EntityRef child) { push(CommandKind::ClearParent, child, 0); }
    /// Moves `e` into reference frame `frame` (InFrame). The World's reparent hook runs at apply
    /// time so the world module can preserve world position and velocity (02 §5.3).
    void reparent(EntityRef e, Entity frame) { push(CommandKind::SetFrame, e, frame.id); }
    void dock(EntityRef e, EntityRef host) { push(CommandKind::Dock, e, 0).other = host; }
    void undock(EntityRef e) { push(CommandKind::Undock, e, 0); }

    usize size() const noexcept { return m_commands.size(); }
    bool empty() const noexcept { return m_commands.empty(); }
    u32 spawnCount() const noexcept { return static_cast<u32>(m_spawns.size()); }
    /// Entity created for `t` by the last apply (invalid before apply, after clear() and once new
    /// commands are recorded).
    Entity resolved(TempEntity t) const noexcept {
        return t.index < m_resolved.size() ? m_resolved[t.index] : Entity();
    }
    /// Drops all commands (payload destructors run) but keeps memory for reuse.
    void clear();

private:
    friend class World;

    struct Command {
        CommandKind kind = CommandKind::Destroy;
        u32 payloadSize = 0;
        EntityRef target;
        EntityRef other;
        u64 arg = 0; // ComponentId, frame entity, or spawn index
        void* payload = nullptr;
        void (*destroyPayload)(void*) = nullptr;
    };

    template <class T>
    ComponentId idOf() const {
        HELIOS_ASSERT(m_world != nullptr, "CommandBuffer needs a World for typed commands");
        const ComponentId id = componentIdForSlot(*m_world, typeSlot<T>());
        HELIOS_ASSERT(id != 0, "component type is not registered in this world");
        return id;
    }
    Command& push(CommandKind kind, EntityRef target, u64 arg) {
        if (m_commands.empty() && !m_resolved.empty()) m_resolved.clear(); // new recording after apply
        Command& cmd = m_commands.emplace_back();
        cmd.kind = kind;
        cmd.target = target;
        cmd.arg = arg;
        return cmd;
    }
    void* allocatePayload(usize size, usize alignment);
    void releaseBlocks() noexcept;

    struct Block {
        std::byte* data;
        usize capacity;
    };

    const World* m_world = nullptr;
    std::vector<Command> m_commands;
    std::vector<SpawnDesc> m_spawns;
    std::vector<Entity> m_resolved;
    std::vector<Block> m_blocks;
    usize m_blockIndex = 0;
    usize m_blockOffset = 0;
};

} // namespace helios::ecs
