#include "helios/ecs/command_buffer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

#include "helios/ecs/heap.h"
#include "helios/ecs/world.h"

namespace helios::ecs {

namespace {
constexpr usize kBlockSize = 16 * 1024;
std::atomic<u32> g_nextTypeSlot{0};
} // namespace

u32 detail::nextTypeSlot() noexcept { return g_nextTypeSlot.fetch_add(1, std::memory_order_relaxed); }

ComponentId componentIdForSlot(const World& world, u32 slot) noexcept { return world.idForSlot(slot); }

CommandBuffer::~CommandBuffer() {
    clear();
    releaseBlocks();
}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
    : m_world(other.m_world), m_commands(std::move(other.m_commands)), m_spawns(std::move(other.m_spawns)),
      m_resolved(std::move(other.m_resolved)), m_blocks(std::move(other.m_blocks)),
      m_blockIndex(std::exchange(other.m_blockIndex, 0)), m_blockOffset(std::exchange(other.m_blockOffset, 0)) {
    other.m_commands.clear();
    other.m_blocks.clear();
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        releaseBlocks();
        m_world = other.m_world;
        m_commands = std::move(other.m_commands);
        m_spawns = std::move(other.m_spawns);
        m_resolved = std::move(other.m_resolved);
        m_blocks = std::move(other.m_blocks);
        m_blockIndex = std::exchange(other.m_blockIndex, 0);
        m_blockOffset = std::exchange(other.m_blockOffset, 0);
        other.m_commands.clear();
        other.m_blocks.clear();
    }
    return *this;
}

TempEntity CommandBuffer::spawn(const SpawnDesc& desc) {
    const u32 index = static_cast<u32>(m_spawns.size());
    m_spawns.push_back(desc);
    push(CommandKind::Spawn, EntityRef(TempEntity{index}), index);
    return TempEntity{index};
}

void CommandBuffer::setRaw(EntityRef e, ComponentId id, const void* value, u32 size) {
    void* mem = allocatePayload(size, alignof(std::max_align_t));
    std::memcpy(mem, value, size);
    Command& cmd = push(CommandKind::Set, e, id);
    cmd.payload = mem;
    cmd.payloadSize = size;
}

void* CommandBuffer::allocatePayload(usize size, usize alignment) {
    if (size == 0) size = 1;
    for (;;) {
        if (m_blockIndex < m_blocks.size()) {
            Block& b = m_blocks[m_blockIndex];
            const usize begin = alignUp<usize>(m_blockOffset, alignment);
            if (begin + size <= b.capacity) {
                m_blockOffset = begin + size;
                return b.data + begin;
            }
            ++m_blockIndex;
            m_blockOffset = 0;
            continue;
        }
        const usize capacity = std::max(kBlockSize, alignUp<usize>(size + alignment, 64));
        auto* data = static_cast<std::byte*>(ecsHeap().allocate(capacity, 64));
        HELIOS_VERIFY(data != nullptr, "CommandBuffer: out of memory");
        m_blocks.push_back(Block{data, capacity});
        m_blockIndex = m_blocks.size() - 1;
        m_blockOffset = 0;
    }
}

void CommandBuffer::clear() {
    for (Command& cmd : m_commands) {
        if (cmd.destroyPayload) cmd.destroyPayload(cmd.payload);
    }
    m_commands.clear();
    m_spawns.clear();
    m_resolved.clear();
    m_blockIndex = 0;
    m_blockOffset = 0;
}

void CommandBuffer::releaseBlocks() noexcept {
    for (Block& b : m_blocks) ecsHeap().free(b.data);
    m_blocks.clear();
    m_blockIndex = 0;
    m_blockOffset = 0;
}

} // namespace helios::ecs
