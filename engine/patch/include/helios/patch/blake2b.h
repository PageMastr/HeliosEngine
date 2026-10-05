#pragma once
// BLAKE2b-256 (RFC 7693, unkeyed, 32-byte digest): the chunk ID, file hash and manifest body hash of the
// patch pipeline (05 §7: "the BLAKE2b-256 of the uncompressed bytes", Monocypher 4.0.3 in C++,
// x/crypto/blake2b in Go). XXH3 is not used here: it detects corruption, not tampering.
//
// The implementation is Monocypher's portable C (no SIMD), so it is safe in the launcher's x86-64-v1
// (`base`) image (02 §1.1, 08 §2.1.1). The self-dispatching SSE4.1/AVX2 compression functions 08 §2.1.1
// lists for verify and repair are later work.
//
// Threading: Hash256 is a value type and blake2b256() is pure; a Blake2b256 hasher belongs to one thread.

#include <array>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "helios/core/types.h"

namespace helios::patch {

/// A 32-byte BLAKE2b-256 digest. Ordered byte-wise (memcmp order), which is the order of the manifest's
/// chunk and pack tables.
struct Hash256 {
    std::array<u8, 32> bytes{};

    /// 64 lowercase hex digits, first byte first (the form of the CDN's chunk paths, 05 §7).
    std::string toHex() const;
    /// Parses 64 hex digits (either case); nullopt on any other input.
    static std::optional<Hash256> fromHex(std::string_view hex) noexcept;
    /// True when every byte is zero.
    bool isZero() const noexcept;

    friend auto operator<=>(const Hash256&, const Hash256&) = default;
    friend bool operator==(const Hash256&, const Hash256&) = default;
};

/// BLAKE2b-256 of `data`.
Hash256 blake2b256(std::span<const u8> data) noexcept;

namespace detail {
// Size of Monocypher 4.0.3's crypto_blake2b_ctx (checked by a static_assert in blake2b.cpp).
inline constexpr usize kBlake2bStateSize = 224;
} // namespace detail

/// Streaming BLAKE2b-256: finish() gives the same digest as blake2b256() over everything update() saw.
/// Not thread-safe; one hasher per thread.
class Blake2b256 {
public:
    Blake2b256() noexcept { reset(); }
    /// Starts a new digest.
    void reset() noexcept;
    /// Absorbs `data`.
    void update(std::span<const u8> data) noexcept;
    /// Returns the digest and resets the hasher.
    Hash256 finish() noexcept;

private:
    alignas(8) unsigned char m_state[detail::kBlake2bStateSize];
};

} // namespace helios::patch
