// BLAKE2b-256 over Monocypher 4.0.3 (vendored, third_party/monocypher).
#include "helios/patch/blake2b.h"

#include <monocypher.h>

#include <algorithm>

namespace helios::patch {

static_assert(sizeof(crypto_blake2b_ctx) == detail::kBlake2bStateSize,
              "Monocypher's BLAKE2b context changed size: update detail::kBlake2bStateSize");
static_assert(alignof(crypto_blake2b_ctx) <= 8, "Monocypher's BLAKE2b context needs more alignment");

namespace {
crypto_blake2b_ctx* ctx(unsigned char* state) noexcept { return reinterpret_cast<crypto_blake2b_ctx*>(state); }

// Monocypher may offset the message pointer even for an empty message; never hand it a null pointer.
const u8* nonNull(std::span<const u8> data) noexcept {
    static constexpr u8 kEmpty = 0;
    return data.empty() ? &kEmpty : data.data();
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string Hash256::toHex() const {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (usize i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0xF];
    }
    return out;
}

std::optional<Hash256> Hash256::fromHex(std::string_view hex) noexcept {
    Hash256 h;
    if (hex.size() != h.bytes.size() * 2) return std::nullopt;
    for (usize i = 0; i < h.bytes.size(); ++i) {
        const int hi = hexValue(hex[2 * i]);
        const int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        h.bytes[i] = static_cast<u8>(hi << 4 | lo);
    }
    return h;
}

bool Hash256::isZero() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](u8 b) { return b == 0; });
}

Hash256 blake2b256(std::span<const u8> data) noexcept {
    Hash256 h;
    crypto_blake2b(h.bytes.data(), h.bytes.size(), nonNull(data), data.size());
    return h;
}

void Blake2b256::reset() noexcept { crypto_blake2b_init(ctx(m_state), 32); }

void Blake2b256::update(std::span<const u8> data) noexcept {
    crypto_blake2b_update(ctx(m_state), nonNull(data), data.size());
}

Hash256 Blake2b256::finish() noexcept {
    Hash256 h;
    crypto_blake2b_final(ctx(m_state), h.bytes.data()); // also wipes the context
    reset();
    return h;
}

} // namespace helios::patch
