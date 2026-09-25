#include "helios/core/name.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/log.h"
#include "helios/core/memory.h"

namespace helios {

namespace {

// Name table layout:
// * Strings live in never-freed arena blocks, so views handed out stay valid forever.
// * id -> entry lookup goes through a two-level array of atomically published segments, so
//   Name::view() is lock-free.
// * text -> id lookup is sharded by hash; each shard has a reader/writer lock.
struct NameEntry {
    const char* text;
    u32 length;
};

constexpr u32 kSegmentBits = 12;
constexpr u32 kSegmentSize = 1u << kSegmentBits;
constexpr u32 kMaxSegments = 4096; // 16.7M names
constexpr u32 kShardCount = 16;
constexpr usize kArenaBlockSize = 64 * 1024;

struct NameShard {
    std::shared_mutex mutex;
    std::unordered_map<std::string_view, u32> map;
};

class NameTable {
public:
    NameTable() {
        for (auto& s : m_segments) s.store(nullptr, std::memory_order_relaxed);
        // Id 0 is None -> "".
        publish(0, NameEntry{"", 0});
        m_count.store(1, std::memory_order_release);
    }

    u32 intern(std::string_view text) {
        if (text.empty()) return 0;
        NameShard& shard = shardFor(text);
        {
            std::shared_lock lock(shard.mutex);
            if (auto it = shard.map.find(text); it != shard.map.end()) return it->second;
        }
        std::unique_lock lock(shard.mutex);
        if (auto it = shard.map.find(text); it != shard.map.end()) return it->second;
        const NameEntry entry = storeString(text);
        const u32 id = allocateId(entry);
        shard.map.emplace(std::string_view(entry.text, entry.length), id);
        return id;
    }

    u32 find(std::string_view text) {
        if (text.empty()) return 0;
        NameShard& shard = shardFor(text);
        std::shared_lock lock(shard.mutex);
        auto it = shard.map.find(text);
        return it == shard.map.end() ? 0 : it->second;
    }

    const NameEntry* entry(u32 id) const noexcept {
        if (id >= m_count.load(std::memory_order_acquire)) return nullptr;
        const NameEntry* segment = m_segments[id >> kSegmentBits].load(std::memory_order_acquire);
        return segment ? &segment[id & (kSegmentSize - 1)] : nullptr;
    }

    usize count() const noexcept { return m_count.load(std::memory_order_acquire); }

private:
    NameShard& shardFor(std::string_view text) noexcept { return m_shards[hash64(text) % kShardCount]; }

    NameEntry storeString(std::string_view text) {
        std::lock_guard lock(m_arenaMutex);
        const usize needed = text.size() + 1;
        char* dst;
        if (needed > kArenaBlockSize / 4) {
            dst = static_cast<char*>(alignedAlloc(needed, 1, MemoryTag::Strings)); // dedicated block
        } else {
            if (m_arenaRemaining < needed) {
                m_arenaCursor = static_cast<char*>(alignedAlloc(kArenaBlockSize, 16, MemoryTag::Strings));
                m_arenaRemaining = kArenaBlockSize;
            }
            dst = m_arenaCursor;
            m_arenaCursor += needed;
            m_arenaRemaining -= needed;
        }
        HELIOS_VERIFY(dst != nullptr, "out of memory interning a Name");
        std::memcpy(dst, text.data(), text.size());
        dst[text.size()] = '\0';
        return NameEntry{dst, static_cast<u32>(text.size())};
    }

    u32 allocateId(const NameEntry& entry) {
        std::lock_guard lock(m_idMutex);
        const u32 id = static_cast<u32>(m_count.load(std::memory_order_relaxed));
        HELIOS_VERIFY(id < kSegmentSize * kMaxSegments, "Name table full");
        publish(id, entry);
        m_count.store(id + 1, std::memory_order_release);
        return id;
    }

    void publish(u32 id, const NameEntry& entry) {
        auto& slot = m_segments[id >> kSegmentBits];
        NameEntry* segment = slot.load(std::memory_order_relaxed);
        if (!segment) {
            segment = static_cast<NameEntry*>(
                alignedAlloc(sizeof(NameEntry) * kSegmentSize, alignof(NameEntry), MemoryTag::Strings));
            HELIOS_VERIFY(segment != nullptr, "out of memory growing the Name table");
            slot.store(segment, std::memory_order_release);
        }
        segment[id & (kSegmentSize - 1)] = entry;
    }

    std::array<NameShard, kShardCount> m_shards;
    std::array<std::atomic<NameEntry*>, kMaxSegments> m_segments;
    std::atomic<usize> m_count{0};
    std::mutex m_idMutex;
    std::mutex m_arenaMutex;
    char* m_arenaCursor = nullptr;
    usize m_arenaRemaining = 0;
};

// Leaked: Names are used from static initializers and destructors.
NameTable& nameTable() {
    static NameTable* table = new NameTable();
    return *table;
}

struct StringIdRegistry {
    std::mutex mutex;
    std::unordered_map<u64, std::string> strings;
};

StringIdRegistry& stringIdRegistry() {
    static StringIdRegistry* registry = new StringIdRegistry();
    return *registry;
}

} // namespace

Name::Name(std::string_view text) : m_id(nameTable().intern(text)) {}

Name Name::find(std::string_view text) noexcept { return Name(nameTable().find(text), 0); }

Name Name::fromId(u32 id) noexcept { return nameTable().entry(id) ? Name(id, 0) : Name(); }

std::string_view Name::view() const noexcept {
    const NameEntry* e = nameTable().entry(m_id);
    return e ? std::string_view(e->text, e->length) : std::string_view();
}

const char* Name::c_str() const noexcept {
    const NameEntry* e = nameTable().entry(m_id);
    return e ? e->text : "";
}

usize Name::internedCount() noexcept { return nameTable().count(); }

bool registerStringId(std::string_view text) {
    const u64 value = fnv1a64(text);
    StringIdRegistry& registry = stringIdRegistry();
    std::lock_guard lock(registry.mutex);
    auto [it, inserted] = registry.strings.try_emplace(value, text);
    if (!inserted && it->second != text) {
        HELIOS_LOG_ERROR(LogCore, "StringId collision: '{}' and '{}' both hash to {:016x}", it->second, text, value);
        return false;
    }
    return true;
}

StringId StringId::make(std::string_view text) {
    registerStringId(text);
    return StringId(text);
}

std::string StringId::debugString() const {
    {
        StringIdRegistry& registry = stringIdRegistry();
        std::lock_guard lock(registry.mutex);
        if (auto it = registry.strings.find(m_value); it != registry.strings.end()) return it->second;
    }
    return std::format("#{:016x}", m_value);
}

} // namespace helios
