// The only translation unit that compiles xxHash (XXH_INLINE_ALL keeps every symbol static here,
// so other modules vendoring xxHash cannot clash with it).
#include "helios/core/hash.h"

#include <format>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace helios {

static_assert(sizeof(XXH3_state_t) <= detail::kXxh3StateSize, "Hasher storage too small for XXH3_state_t");
static_assert(alignof(XXH3_state_t) <= detail::kXxh3StateAlign, "Hasher storage under-aligned for XXH3_state_t");

namespace {
XXH3_state_t* asState(unsigned char* storage) noexcept { return reinterpret_cast<XXH3_state_t*>(storage); }
const XXH3_state_t* asState(const unsigned char* storage) noexcept {
    return reinterpret_cast<const XXH3_state_t*>(storage);
}
} // namespace

std::string Hash128::toHex() const { return std::format("{:016x}{:016x}", high, low); }

u64 hash64(const void* data, usize size, u64 seed) noexcept { return XXH3_64bits_withSeed(data, size, seed); }

Hash128 hash128(const void* data, usize size, u64 seed) noexcept {
    const XXH128_hash_t h = XXH3_128bits_withSeed(data, size, seed);
    return Hash128{h.low64, h.high64};
}

void Hasher64::reset(u64 seed) noexcept {
    XXH3_state_t* state = asState(m_state);
    XXH3_INITSTATE(state);
    XXH3_64bits_reset_withSeed(state, seed);
}

void Hasher64::update(const void* data, usize size) noexcept {
    if (size == 0) return;
    XXH3_64bits_update(asState(m_state), data, size);
}

u64 Hasher64::digest() const noexcept { return XXH3_64bits_digest(asState(m_state)); }

void Hasher128::reset(u64 seed) noexcept {
    XXH3_state_t* state = asState(m_state);
    XXH3_INITSTATE(state);
    XXH3_128bits_reset_withSeed(state, seed);
}

void Hasher128::update(const void* data, usize size) noexcept {
    if (size == 0) return;
    XXH3_128bits_update(asState(m_state), data, size);
}

Hash128 Hasher128::digest() const noexcept {
    const XXH128_hash_t h = XXH3_128bits_digest(asState(m_state));
    return Hash128{h.low64, h.high64};
}

} // namespace helios
