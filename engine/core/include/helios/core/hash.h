#pragma once
// Hashing: XXH3-64 / XXH3-128 (vendored xxHash 0.8.3, compiled in one translation unit) for content
// and runtime hashing, constexpr FNV-1a for compile-time identifiers, and hash combining.
//
// XXH3 output is stable across platforms, compilers and endianness, so it is safe for persistent
// content IDs and network protocol values. std::hash is not (never serialize it).
// Threading: all functions are pure; Hasher objects are not shared between threads.

#include <compare>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "helios/core/platform.h"
#include "helios/core/types.h"

namespace helios {

/// 128-bit hash value (XXH128 layout: low and high 64-bit halves).
struct Hash128 {
    u64 low = 0;
    u64 high = 0;

    friend constexpr bool operator==(const Hash128&, const Hash128&) = default;
    friend constexpr std::strong_ordering operator<=>(const Hash128& a, const Hash128& b) {
        if (auto c = a.high <=> b.high; c != 0) return c;
        return a.low <=> b.low;
    }
    /// 32 lowercase hex digits, high half first (matches xxhsum's canonical XXH128 output).
    std::string toHex() const;
};

/// XXH3 64-bit hash of a byte range.
u64 hash64(const void* data, usize size, u64 seed = 0) noexcept;
inline u64 hash64(std::string_view text, u64 seed = 0) noexcept { return hash64(text.data(), text.size(), seed); }
inline u64 hash64(std::span<const std::byte> bytes, u64 seed = 0) noexcept {
    return hash64(bytes.data(), bytes.size(), seed);
}

/// XXH3 128-bit hash of a byte range.
Hash128 hash128(const void* data, usize size, u64 seed = 0) noexcept;
inline Hash128 hash128(std::string_view text, u64 seed = 0) noexcept {
    return hash128(text.data(), text.size(), seed);
}
inline Hash128 hash128(std::span<const std::byte> bytes, u64 seed = 0) noexcept {
    return hash128(bytes.data(), bytes.size(), seed);
}

/// Hashes the object representation of a value without padding bits (ints, enums, packed PODs).
template <class T>
    requires std::has_unique_object_representations_v<T>
inline u64 hashValue(const T& value, u64 seed = 0) noexcept {
    return hash64(&value, sizeof(T), seed);
}

namespace detail {
// Size/alignment of XXH3_state_t in xxHash 0.8.x (checked by static_assert in hash.cpp).
inline constexpr usize kXxh3StateSize = 576;
inline constexpr usize kXxh3StateAlign = 64;
} // namespace detail

/// Streaming XXH3-64: produces the same value as hash64() over the concatenated input.
class Hasher64 {
public:
    explicit Hasher64(u64 seed = 0) noexcept { reset(seed); }
    void reset(u64 seed = 0) noexcept;
    void update(const void* data, usize size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    template <class T>
        requires std::has_unique_object_representations_v<T>
    void updateValue(const T& value) noexcept {
        update(&value, sizeof(T));
    }
    u64 digest() const noexcept;

private:
    alignas(detail::kXxh3StateAlign) unsigned char m_state[detail::kXxh3StateSize];
};

/// Streaming XXH3-128.
class Hasher128 {
public:
    explicit Hasher128(u64 seed = 0) noexcept { reset(seed); }
    void reset(u64 seed = 0) noexcept;
    void update(const void* data, usize size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    Hash128 digest() const noexcept;

private:
    alignas(detail::kXxh3StateAlign) unsigned char m_state[detail::kXxh3StateSize];
};

inline constexpr u64 kFnv1a64Offset = 0xcbf29ce484222325ull;
inline constexpr u64 kFnv1a64Prime = 0x100000001b3ull;
inline constexpr u32 kFnv1a32Offset = 0x811c9dc5u;
inline constexpr u32 kFnv1a32Prime = 0x01000193u;

/// FNV-1a 64-bit; constexpr so identifiers can be computed at compile time. Not for large data.
constexpr u64 fnv1a64(std::string_view text, u64 hash = kFnv1a64Offset) noexcept {
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kFnv1a64Prime;
    }
    return hash;
}

constexpr u32 fnv1a32(std::string_view text, u32 hash = kFnv1a32Offset) noexcept {
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kFnv1a32Prime;
    }
    return hash;
}

/// Strong 64-bit finalizer (SplitMix64 / Stafford variant 13): bijective, full avalanche.
constexpr u64 mix64(u64 x) noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

/// Order-dependent combination of two hashes: hashCombine(a, b) != hashCombine(b, a).
constexpr u64 hashCombine(u64 seed, u64 value) noexcept {
    return mix64(seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

namespace literals {
/// "name"_fnv — compile-time FNV-1a 64.
consteval u64 operator""_fnv(const char* text, usize size) { return fnv1a64(std::string_view(text, size)); }
} // namespace literals

} // namespace helios

template <>
struct std::hash<helios::Hash128> {
    std::size_t operator()(const helios::Hash128& h) const noexcept {
        return static_cast<std::size_t>(helios::hashCombine(h.low, h.high));
    }
};
