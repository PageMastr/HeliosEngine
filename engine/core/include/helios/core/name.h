#pragma once
// Name: interned string with a 32-bit id. Construction interns the text in a global, never-shrinking
// table; comparison, hashing and copying are integer operations. Use for identifiers created at
// runtime (asset names, bone names, script symbols). Ids are process-local: never serialize them.
//
// StringId: constexpr 64-bit FNV-1a hash of a string, usable in switch statements and as a
// stable persistent/network id. StringId::make() additionally records the text in a debug registry
// (with collision detection) so tools and logs can print it.
//
// Threading: everything is thread-safe. Name::view()/c_str() are lock-free; interning a new string
// takes a sharded lock.

#include <compare>
#include <format>
#include <functional>
#include <string>
#include <string_view>

#include "helios/core/hash.h"
#include "helios/core/types.h"

namespace helios {

class Name {
public:
    /// The "None" name (id 0, empty text).
    constexpr Name() noexcept = default;
    /// Interns `text` (case-sensitive). Empty text yields None.
    explicit Name(std::string_view text);
    explicit Name(const char* text) : Name(std::string_view(text)) {}
    explicit Name(const std::string& text) : Name(std::string_view(text)) {}

    /// Returns the existing Name for `text` without interning (None if never interned).
    static Name find(std::string_view text) noexcept;
    /// Reconstructs a Name from id(); None if the id was never issued.
    static Name fromId(u32 id) noexcept;

    constexpr u32 id() const noexcept { return m_id; }
    constexpr bool isNone() const noexcept { return m_id == 0; }
    constexpr explicit operator bool() const noexcept { return m_id != 0; }

    /// Interned text; valid for the lifetime of the process.
    std::string_view view() const noexcept;
    /// NUL-terminated interned text; valid for the lifetime of the process.
    const char* c_str() const noexcept;
    std::string toString() const { return std::string(view()); }

    /// Id comparison: fast but not alphabetical. Use lexicalLess for sorted UI lists.
    friend constexpr bool operator==(Name a, Name b) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Name a, Name b) noexcept { return a.m_id <=> b.m_id; }
    static bool lexicalLess(Name a, Name b) noexcept { return a.view() < b.view(); }

    /// Number of distinct interned strings (including None).
    static usize internedCount() noexcept;

private:
    constexpr explicit Name(u32 id, int /*tag*/) noexcept : m_id(id) {}
    u32 m_id = 0;
};

class StringId {
public:
    constexpr StringId() noexcept = default;
    constexpr explicit StringId(std::string_view text) noexcept : m_value(fnv1a64(text)) {}

    static constexpr StringId fromValue(u64 value) noexcept {
        StringId id;
        id.m_value = value;
        return id;
    }

    /// Hashes `text` and records it in the debug registry. A collision (two different strings with
    /// the same hash) is logged as an error.
    static StringId make(std::string_view text);

    constexpr u64 value() const noexcept { return m_value; }
    constexpr bool isNone() const noexcept { return m_value == 0; }

    /// Registered text, or "#<16 hex digits>" if the text was never registered.
    std::string debugString() const;

    friend constexpr bool operator==(StringId, StringId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(StringId a, StringId b) noexcept {
        return a.m_value <=> b.m_value;
    }

private:
    u64 m_value = 0;
};

/// Records `text` in the StringId debug registry. Returns false on a hash collision.
bool registerStringId(std::string_view text);

namespace literals {
/// "player.health"_sid — compile-time StringId (not registered for debugString()).
consteval StringId operator""_sid(const char* text, usize size) { return StringId(std::string_view(text, size)); }
} // namespace literals

} // namespace helios

template <>
struct std::hash<helios::Name> {
    std::size_t operator()(helios::Name n) const noexcept { return static_cast<std::size_t>(helios::mix64(n.id())); }
};

template <>
struct std::hash<helios::StringId> {
    std::size_t operator()(helios::StringId id) const noexcept { return static_cast<std::size_t>(id.value()); }
};

template <>
struct std::formatter<helios::Name> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(helios::Name name, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(name.view(), ctx);
    }
};

template <>
struct std::formatter<helios::StringId> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(helios::StringId id, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(id.debugString(), ctx);
    }
};
