#pragma once
// Fixed-width scalar aliases and small byte/bit helpers used throughout the engine.
// Everything is constexpr, header-only and thread-agnostic (pure functions).

#include <bit>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "helios/core/platform.h"

namespace helios {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;
using uptr = std::uintptr_t;
using iptr = std::intptr_t;
using byte = std::byte;

static_assert(CHAR_BIT == 8, "Helios requires 8-bit bytes");
static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "Helios requires IEEE-754 binary32/binary64");
static_assert(sizeof(usize) == 8, "Helios targets 64-bit platforms only");

inline constexpr u64 kKiB = 1024ull;
inline constexpr u64 kMiB = 1024ull * kKiB;
inline constexpr u64 kGiB = 1024ull * kMiB;

namespace literals {
constexpr u64 operator""_KiB(unsigned long long v) noexcept { return v * kKiB; }
constexpr u64 operator""_MiB(unsigned long long v) noexcept { return v * kMiB; }
constexpr u64 operator""_GiB(unsigned long long v) noexcept { return v * kGiB; }
} // namespace literals

/// True if `v` is a non-zero power of two.
template <std::unsigned_integral T>
constexpr bool isPowerOfTwo(T v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

/// Smallest power of two >= v (v == 0 yields 1).
template <std::unsigned_integral T>
constexpr T nextPowerOfTwo(T v) noexcept {
    return std::bit_ceil(v);
}

/// Rounds `v` up to a multiple of `alignment` (which must be a power of two).
template <std::unsigned_integral T>
constexpr T alignUp(T v, T alignment) noexcept {
    return (v + alignment - 1) & ~(alignment - 1);
}

/// Rounds `v` down to a multiple of `alignment` (which must be a power of two).
template <std::unsigned_integral T>
constexpr T alignDown(T v, T alignment) noexcept {
    return v & ~(alignment - 1);
}

template <std::unsigned_integral T>
constexpr bool isAligned(T v, T alignment) noexcept {
    return (v & (alignment - 1)) == 0;
}

inline bool isAligned(const void* p, usize alignment) noexcept {
    return (reinterpret_cast<uptr>(p) & (alignment - 1)) == 0;
}

/// Rounds a pointer up to `alignment` (power of two).
template <class T>
inline T* alignPointer(T* p, usize alignment) noexcept {
    return reinterpret_cast<T*>(alignUp<uptr>(reinterpret_cast<uptr>(p), alignment));
}

/// std::to_underlying (C++23) for C++20.
template <class E>
    requires std::is_enum_v<E>
constexpr std::underlying_type_t<E> toUnderlying(E e) noexcept {
    return static_cast<std::underlying_type_t<E>>(e);
}

constexpr u16 byteSwap16(u16 v) noexcept { return static_cast<u16>((v >> 8) | (v << 8)); }
constexpr u32 byteSwap32(u32 v) noexcept {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}
constexpr u64 byteSwap64(u64 v) noexcept {
    return (static_cast<u64>(byteSwap32(static_cast<u32>(v))) << 32) | byteSwap32(static_cast<u32>(v >> 32));
}

/// Unaligned little-/big-endian loads and stores. Serialized data in Helios is little-endian.
template <std::unsigned_integral T>
inline T loadLE(const void* src) noexcept {
    T v;
    std::memcpy(&v, src, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        if constexpr (sizeof(T) == 2) v = byteSwap16(v);
        else if constexpr (sizeof(T) == 4) v = byteSwap32(v);
        else if constexpr (sizeof(T) == 8) v = byteSwap64(v);
    }
    return v;
}

template <std::unsigned_integral T>
inline void storeLE(void* dst, T v) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        if constexpr (sizeof(T) == 2) v = byteSwap16(v);
        else if constexpr (sizeof(T) == 4) v = byteSwap32(v);
        else if constexpr (sizeof(T) == 8) v = byteSwap64(v);
    }
    std::memcpy(dst, &v, sizeof(T));
}

template <std::unsigned_integral T>
inline T loadBE(const void* src) noexcept {
    T v;
    std::memcpy(&v, src, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        if constexpr (sizeof(T) == 2) v = byteSwap16(v);
        else if constexpr (sizeof(T) == 4) v = byteSwap32(v);
        else if constexpr (sizeof(T) == 8) v = byteSwap64(v);
    }
    return v;
}

template <std::unsigned_integral T>
inline void storeBE(void* dst, T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        if constexpr (sizeof(T) == 2) v = byteSwap16(v);
        else if constexpr (sizeof(T) == 4) v = byteSwap32(v);
        else if constexpr (sizeof(T) == 8) v = byteSwap64(v);
    }
    std::memcpy(dst, &v, sizeof(T));
}

/// Views any trivially copyable object as bytes.
template <class T>
    requires std::is_trivially_copyable_v<T>
inline std::span<const std::byte, sizeof(T)> objectBytes(const T& value) noexcept {
    return std::span<const std::byte, sizeof(T)>(reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

} // namespace helios

/// Defines bitwise operators for a scoped enum used as a flag set, plus helios::hasFlag / hasAnyFlag.
#define HELIOS_ENUM_FLAGS(E)                                                                            \
    [[maybe_unused]] constexpr E operator|(E a, E b) noexcept { return static_cast<E>(::helios::toUnderlying(a) | ::helios::toUnderlying(b)); } \
    [[maybe_unused]] constexpr E operator&(E a, E b) noexcept { return static_cast<E>(::helios::toUnderlying(a) & ::helios::toUnderlying(b)); } \
    [[maybe_unused]] constexpr E operator^(E a, E b) noexcept { return static_cast<E>(::helios::toUnderlying(a) ^ ::helios::toUnderlying(b)); } \
    [[maybe_unused]] constexpr E operator~(E a) noexcept { return static_cast<E>(~::helios::toUnderlying(a)); }                                  \
    [[maybe_unused]] constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }                                                          \
    [[maybe_unused]] constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }                                                          \
    [[maybe_unused]] constexpr E& operator^=(E& a, E b) noexcept { return a = a ^ b; }                                                          \
    [[maybe_unused]] constexpr bool hasFlag(E set, E flag) noexcept { return (set & flag) == flag; }                                            \
    [[maybe_unused]] constexpr bool hasAnyFlag(E set, E flags) noexcept { return ::helios::toUnderlying(set & flags) != 0; }
