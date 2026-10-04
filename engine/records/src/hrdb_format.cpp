// `.hrdb` header encoding and checksums.
#include "helios/records/hrdb_format.h"

namespace helios::records {

std::string_view cookAudienceName(CookAudience audience) noexcept {
    switch (audience) {
    case CookAudience::Client: return "client";
    case CookAudience::Server: return "server";
    }
    return "unknown";
}

namespace hrdb {

void encodeHeader(const Header& h, std::span<u8, kHeaderBytes> out) noexcept {
    u8* p = out.data();
    store<u32>(p + 0, h.magic);
    store<u16>(p + 4, h.formatVersion);
    store<u8>(p + 6, h.audience);
    store<u8>(p + 7, h.reserved0);
    store<u32>(p + 8, h.rootTypeId);
    store<u32>(p + 12, h.flags);
    store<u64>(p + 16, h.layoutHash);
    store<u64>(p + 24, h.size);
    store<u64>(p + 32, h.contentHash);
    store<u64>(p + 40, h.tagTableHash);
    store<u64>(p + 48, h.reserved2);
    store<u64>(p + 56, h.headerHash);
}

Header decodeHeader(std::span<const u8, kHeaderBytes> in) noexcept {
    const u8* p = in.data();
    Header h;
    h.magic = load<u32>(p + 0);
    h.formatVersion = load<u16>(p + 4);
    h.audience = load<u8>(p + 6);
    h.reserved0 = load<u8>(p + 7);
    h.rootTypeId = load<u32>(p + 8);
    h.flags = load<u32>(p + 12);
    h.layoutHash = load<u64>(p + 16);
    h.size = load<u64>(p + 24);
    h.contentHash = load<u64>(p + 32);
    h.tagTableHash = load<u64>(p + 40);
    h.reserved2 = load<u64>(p + 48);
    h.headerHash = load<u64>(p + 56);
    return h;
}

u64 computeHeaderHash(std::span<const u8, kHeaderBytes> encoded) noexcept {
    return hash64(encoded.data(), kHeaderHashedBytes);
}

u64 computeContentHash(std::span<const u8> file) noexcept {
    if (file.size() <= kHeaderBytes) return hash64(nullptr, 0);
    return hash64(file.data() + kHeaderBytes, file.size() - kHeaderBytes);
}

void seal(std::span<u8> file) noexcept {
    if (file.size() < kHeaderBytes) return;
    store<u64>(file.data() + 32, computeContentHash(file));
    const std::span<u8, kHeaderBytes> header(file.data(), kHeaderBytes);
    store<u64>(file.data() + 56, computeHeaderHash(header));
}

} // namespace hrdb
} // namespace helios::records
