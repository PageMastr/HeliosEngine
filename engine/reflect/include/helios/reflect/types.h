#pragma once
// Vocabulary types named by the schema language (02 §3.1 "built-in types") that have no better
// home below the reflection layer: entity/record/asset references, localized strings, tags,
// HXL source, durations, frame-local world positions and keyed lists.
//
// Generated code (helios-schemac) always spells these with their full namespace, so schema types
// may reuse the short names (e.g. a variant alternative called `Duration`).
//
// Layouts deliberately match engine/ecs's identity types (EntityId = u64, NetHandle = u32 with a
// 24-bit index + 8-bit generation, Tick = u64) so the two convert with a plain copy.
//
// Threading: plain value types; no shared state. KeyedList::add() without an explicit key mints a
// GUID with the thread-safe OS CSPRNG.

#include <algorithm>
#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/core/assert.h"
#include "helios/core/guid.h"
#include "helios/core/name.h"
#include "helios/core/types.h"
#include "helios/math/vec.h"

namespace helios::refl {

/// 63-bit record identifier, minted once from secure random bits and stored in the `.hrec` source
/// as `$rid` (02 §3.3). 0 means "no record".
using RecordId = u64;
inline constexpr RecordId kRecordIdMask = (1ull << 63) - 1;

/// Simulation tick (matches helios::ecs::Tick).
using Tick = u64;

/// Global, persistent 64-bit entity identifier (02 §4.1). 0 is invalid.
struct EntityId {
    u64 value = 0;

    constexpr EntityId() noexcept = default;
    constexpr explicit EntityId(u64 raw) noexcept : value(raw) {}
    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    friend constexpr bool operator==(EntityId, EntityId) noexcept = default;
    friend constexpr auto operator<=>(EntityId, EntityId) noexcept = default;
};

/// Zone-instance-scoped 32-bit wire alias of an entity: 24-bit index + 8-bit generation (04 §4.6).
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

/// Signed time span in nanoseconds (same unit as std::chrono::nanoseconds and Go's time.Duration).
/// Text form: an integer with the largest exact unit ("30d", "500ms", "1500us"); readers also accept
/// decimals ("1.5s") and plain numbers (seconds).
struct Duration {
    i64 nanos = 0;

    constexpr Duration() noexcept = default;
    constexpr explicit Duration(i64 ns) noexcept : nanos(ns) {}
    static constexpr Duration fromMillis(i64 ms) noexcept { return Duration(ms * 1'000'000); }
    static constexpr Duration fromSeconds(i64 s) noexcept { return Duration(s * 1'000'000'000); }
    constexpr f64 seconds() const noexcept { return static_cast<f64>(nanos) * 1e-9; }
    friend constexpr bool operator==(Duration, Duration) noexcept = default;
    friend constexpr auto operator<=>(Duration, Duration) noexcept = default;
};

/// Typed reference to a record (`record FooDef` declares `FooRef = RecordRef<FooDef>`).
/// Serialized as the RecordId, so renaming a record's `$name` never breaks references.
template <class T>
struct RecordRef {
    using RecordType = T;
    RecordId id = 0;

    constexpr RecordRef() noexcept = default;
    constexpr explicit RecordRef(RecordId rid) noexcept : id(rid) {}
    constexpr bool isValid() const noexcept { return id != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    friend constexpr bool operator==(RecordRef, RecordRef) noexcept = default;
    friend constexpr auto operator<=>(RecordRef, RecordRef) noexcept = default;
};

/// Reference to an asset by its content GUID (`AssetRef<Prefab>`; the asset kind is schema
/// metadata, see FieldInfo attributes).
struct AssetRef {
    Guid guid;
    constexpr bool isValid() const noexcept { return !guid.isNil(); }
    friend constexpr bool operator==(const AssetRef&, const AssetRef&) noexcept = default;
    friend constexpr auto operator<=>(const AssetRef&, const AssetRef&) noexcept = default;
};

/// Localized string: a key into the string tables ("ship.kestrel.name").
struct LocString {
    std::string key;
    friend bool operator==(const LocString&, const LocString&) = default;
};

/// Set of gameplay tags ("Item.Weapon.Laser"). Kept sorted by text and unique, so equality and
/// serialization are canonical. Tags compile to dense u16 TagIndex values at cook time (06 §1.1).
class TagSet {
public:
    TagSet() = default;
    /// Adds a tag (no-op when present). Returns true if it was added.
    bool add(Name tag);
    bool add(std::string_view tag) { return add(Name(tag)); }
    bool remove(Name tag);
    bool contains(Name tag) const noexcept;
    usize size() const noexcept { return m_tags.size(); }
    bool empty() const noexcept { return m_tags.empty(); }
    void clear() noexcept { m_tags.clear(); }
    const std::vector<Name>& tags() const noexcept { return m_tags; }
    friend bool operator==(const TagSet&, const TagSet&) = default;

private:
    std::vector<Name> m_tags; // sorted by Name::lexicalLess, unique
};

/// Tag query source text (compiled by the gameplay framework, 06 §1.1).
struct TagQuery {
    std::string text;
    friend bool operator==(const TagQuery&, const TagQuery&) = default;
};

/// HXL expression source text (compiled to bytecode by the cook, 02 §3.3).
struct HxlExpr {
    std::string text;
    friend bool operator==(const HxlExpr&, const HxlExpr&) = default;
};

/// Frame-local f64 position (02 §5.2). The frame is the entity's `(InFrame, f)` target.
struct WorldPos {
    DVec3 local;
    friend constexpr bool operator==(const WorldPos&, const WorldPos&) noexcept = default;
};

/// List whose elements carry stable GUID keys (`list<T> @keyed`, 02 §3.2), so merges, prefab
/// overrides and property paths (`entries[#b21c…]/weight`) never depend on the element index.
/// Keys are unique within a list. Iteration visits the values in order.
template <class T>
class KeyedList {
public:
    using value_type = T;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    KeyedList() = default;

    usize size() const noexcept { return m_items.size(); }
    bool empty() const noexcept { return m_items.empty(); }
    T& operator[](usize i) noexcept { return m_items[i]; }
    const T& operator[](usize i) const noexcept { return m_items[i]; }
    const Guid& keyAt(usize i) const noexcept { return m_keys[i]; }
    /// Replaces the key of element i (used by readers and patch application).
    void setKey(usize i, const Guid& key) noexcept { m_keys[i] = key; }

    iterator begin() noexcept { return m_items.begin(); }
    iterator end() noexcept { return m_items.end(); }
    const_iterator begin() const noexcept { return m_items.begin(); }
    const_iterator end() const noexcept { return m_items.end(); }

    /// Appends `value` under a freshly minted random key.
    T& add(T value = T{}) { return add(Guid::generate(), std::move(value)); }
    /// Appends `value` under `key` (which must not be present).
    T& add(const Guid& key, T value) {
        HELIOS_ASSERT(!indexOf(key).has_value(), "duplicate KeyedList key");
        m_keys.push_back(key);
        m_items.push_back(std::move(value));
        return m_items.back();
    }
    /// Inserts before position `index` (index == size() appends).
    T& insert(usize index, const Guid& key, T value) {
        HELIOS_ASSERT(index <= m_items.size());
        m_keys.insert(m_keys.begin() + static_cast<isize>(index), key);
        return *m_items.insert(m_items.begin() + static_cast<isize>(index), std::move(value));
    }
    std::optional<usize> indexOf(const Guid& key) const noexcept {
        for (usize i = 0; i < m_keys.size(); ++i) {
            if (m_keys[i] == key) return i;
        }
        return std::nullopt;
    }
    T* find(const Guid& key) noexcept {
        const auto i = indexOf(key);
        return i ? &m_items[*i] : nullptr;
    }
    const T* find(const Guid& key) const noexcept {
        const auto i = indexOf(key);
        return i ? &m_items[*i] : nullptr;
    }
    void erase(usize index) {
        m_keys.erase(m_keys.begin() + static_cast<isize>(index));
        m_items.erase(m_items.begin() + static_cast<isize>(index));
    }
    bool erase(const Guid& key) {
        const auto i = indexOf(key);
        if (!i) return false;
        erase(*i);
        return true;
    }
    /// Resizes; new elements get nil keys (callers assign real keys with setKey()).
    void resize(usize n) {
        m_items.resize(n);
        m_keys.resize(n);
    }
    void clear() noexcept {
        m_items.clear();
        m_keys.clear();
    }
    void reserve(usize n) {
        m_items.reserve(n);
        m_keys.reserve(n);
    }
    const std::vector<T>& values() const noexcept { return m_items; }
    const std::vector<Guid>& keys() const noexcept { return m_keys; }

    friend bool operator==(const KeyedList& a, const KeyedList& b) {
        return a.m_keys == b.m_keys && a.m_items == b.m_items;
    }

private:
    std::vector<T> m_items;
    std::vector<Guid> m_keys;
};

/// Mints a new RecordId: 63 secure random bits, never 0 (02 §3.3: minted once, stored in source).
RecordId mintRecordId();

} // namespace helios::refl
