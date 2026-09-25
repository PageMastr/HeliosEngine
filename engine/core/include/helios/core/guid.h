#pragma once
// 128-bit globally unique identifiers (RFC 9562 version 4, random).
//
// Canonical text: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (lowercase). parse() also accepts
// uppercase, surrounding braces and the 32-digit form without dashes. The 16 bytes are stored as
// two big-endian 64-bit halves, so ordering by (high, low) equals byte-wise/text ordering.
//
// Threading: Guid is a value type; generate() is thread-safe (OS CSPRNG: BCryptGenRandom /
// getrandom, falling back to /dev/urandom).

#include <array>
#include <compare>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios {

struct Guid {
    u64 high = 0; ///< Bytes 0..7 (big-endian).
    u64 low = 0;  ///< Bytes 8..15 (big-endian).

    constexpr Guid() noexcept = default;
    constexpr Guid(u64 hi, u64 lo) noexcept : high(hi), low(lo) {}

    /// New random (version 4, variant 10) GUID.
    static Guid generate();
    static Result<Guid> parse(std::string_view text);
    static Guid fromBytes(const std::array<u8, 16>& bytes) noexcept;
    std::array<u8, 16> toBytes() const noexcept;

    static constexpr Guid nil() noexcept { return Guid(); }
    constexpr bool isNil() const noexcept { return high == 0 && low == 0; }
    constexpr explicit operator bool() const noexcept { return !isNil(); }
    /// Version nibble (4 for generate()).
    constexpr u32 version() const noexcept { return static_cast<u32>((high >> 12) & 0xF); }

    std::string toString() const;
    /// Writes exactly 36 characters (no terminator).
    void toChars(char* out36) const noexcept;
    u64 hash() const noexcept { return hashCombine(high, low); }

    friend constexpr bool operator==(const Guid&, const Guid&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Guid& a, const Guid& b) noexcept {
        if (auto c = a.high <=> b.high; c != 0) return c;
        return a.low <=> b.low;
    }
};

/// Fills `size` bytes with cryptographically secure random data. Returns false on OS failure.
bool secureRandomBytes(void* buffer, usize size) noexcept;

} // namespace helios

template <>
struct std::hash<helios::Guid> {
    std::size_t operator()(const helios::Guid& g) const noexcept { return static_cast<std::size_t>(g.hash()); }
};

template <>
struct std::formatter<helios::Guid> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(const helios::Guid& g, Ctx& ctx) const {
        char text[36];
        g.toChars(text);
        return std::formatter<std::string_view>::format(std::string_view(text, 36), ctx);
    }
};
