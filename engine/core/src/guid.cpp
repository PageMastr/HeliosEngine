#include "helios/core/guid.h"

#include <atomic>

#include "helios/core/log.h"
#include "helios/core/random.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "platform/os.h"

namespace helios {

namespace {

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Last-resort entropy if the OS CSPRNG fails (should never happen): unique, not unpredictable.
void fallbackRandom(u8* out, usize size) noexcept {
    static std::atomic<u64> counter{0};
    SplitMix64 rng(static_cast<u64>(unixTimeNanos()) ^ (monotonicNanos() << 17) ^ (currentThreadId() << 40) ^
                   counter.fetch_add(0x9e3779b97f4a7c15ull, std::memory_order_relaxed));
    for (usize i = 0; i < size; i += 8) {
        u8 block[8];
        storeLE<u64>(block, rng.next());
        for (usize k = 0; k < 8 && i + k < size; ++k) out[i + k] = block[k];
    }
}

} // namespace

bool secureRandomBytes(void* buffer, usize size) noexcept { return os::secureRandomBytes(buffer, size); }

Guid Guid::generate() {
    std::array<u8, 16> bytes{};
    if (!os::secureRandomBytes(bytes.data(), bytes.size())) {
        HELIOS_LOG_ERROR(LogCore, "OS random source failed; using fallback GUID entropy");
        fallbackRandom(bytes.data(), bytes.size());
    }
    bytes[6] = static_cast<u8>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<u8>((bytes[8] & 0x3F) | 0x80); // RFC variant (10xx)
    return fromBytes(bytes);
}

Guid Guid::fromBytes(const std::array<u8, 16>& bytes) noexcept {
    return Guid(loadBE<u64>(bytes.data()), loadBE<u64>(bytes.data() + 8));
}

std::array<u8, 16> Guid::toBytes() const noexcept {
    std::array<u8, 16> bytes{};
    storeBE<u64>(bytes.data(), high);
    storeBE<u64>(bytes.data() + 8, low);
    return bytes;
}

void Guid::toChars(char* out) const noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::array<u8, 16> bytes = toBytes();
    usize o = 0;
    for (usize i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = kHex[bytes[i] >> 4];
        out[o++] = kHex[bytes[i] & 0xF];
    }
}

std::string Guid::toString() const {
    std::string s(36, '\0');
    toChars(s.data());
    return s;
}

Result<Guid> Guid::parse(std::string_view text) {
    std::string_view s = text;
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') s = s.substr(1, s.size() - 2);
    if (s.size() != 36 && s.size() != 32) {
        return makeError(ErrorCode::ParseError, "'{}' is not a GUID (expected 32 or 36 characters)", text);
    }
    std::array<u8, 16> bytes{};
    usize nibble = 0;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (s.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (c != '-') return makeError(ErrorCode::ParseError, "'{}' is not a GUID (misplaced '-')", text);
            continue;
        }
        const int v = hexValue(c);
        if (v < 0) return makeError(ErrorCode::ParseError, "'{}' is not a GUID (bad hex digit)", text);
        bytes[nibble / 2] = static_cast<u8>(bytes[nibble / 2] | (nibble % 2 == 0 ? v << 4 : v));
        ++nibble;
    }
    return fromBytes(bytes);
}

} // namespace helios
