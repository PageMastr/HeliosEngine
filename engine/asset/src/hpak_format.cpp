// `.hpak` v0 field encoding (see hpak_format.h for the layout).

#include "helios/asset/hpak_format.h"

#include <cstring>

namespace helios::asset {

std::string_view hpakPlatformName(HpakPlatform platform) noexcept {
    switch (platform) {
    case HpakPlatform::PcClient: return "pc-client";
    case HpakPlatform::Server: return "server";
    case HpakPlatform::Editor: return "editor";
    }
    return "unknown";
}

namespace hpak {

void encodeHeader(const Header& h, std::span<u8, kHeaderBytes> out) noexcept {
    u8* p = out.data();
    storeLE<u32>(p + 0, h.magic);
    storeLE<u16>(p + 4, h.version);
    storeLE<u16>(p + 6, h.reserved0);
    storeLE<u32>(p + 8, h.platform);
    storeLE<u32>(p + 12, h.flags);
    storeLE<u64>(p + 16, h.contentBuild);
    storeLE<u64>(p + 24, h.tags.group);
    storeLE<u32>(p + 32, h.tags.language);
    p[36] = h.tags.tier;
    std::memcpy(p + 37, h.reserved1, 3);
    storeLE<u64>(p + 40, h.tocOffset);
    storeLE<u64>(p + 48, h.tocSize);
    storeLE<u32>(p + 56, h.assetCount);
    storeLE<u32>(p + 60, h.assetBlockCount);
    storeLE<u32>(p + 64, h.pakBlockCount);
    storeLE<u32>(p + 68, h.reserved2);
    storeLE<u64>(p + 72, h.tocHash);
    storeLE<u64>(p + 80, h.headerHash);
}

Header decodeHeader(std::span<const u8, kHeaderBytes> in) noexcept {
    const u8* p = in.data();
    Header h;
    h.magic = loadLE<u32>(p + 0);
    h.version = loadLE<u16>(p + 4);
    h.reserved0 = loadLE<u16>(p + 6);
    h.platform = loadLE<u32>(p + 8);
    h.flags = loadLE<u32>(p + 12);
    h.contentBuild = loadLE<u64>(p + 16);
    h.tags.group = loadLE<u64>(p + 24);
    h.tags.language = loadLE<u32>(p + 32);
    h.tags.tier = p[36];
    std::memcpy(h.reserved1, p + 37, 3);
    h.tocOffset = loadLE<u64>(p + 40);
    h.tocSize = loadLE<u64>(p + 48);
    h.assetCount = loadLE<u32>(p + 56);
    h.assetBlockCount = loadLE<u32>(p + 60);
    h.pakBlockCount = loadLE<u32>(p + 64);
    h.reserved2 = loadLE<u32>(p + 68);
    h.tocHash = loadLE<u64>(p + 72);
    h.headerHash = loadLE<u64>(p + 80);
    return h;
}

u64 computeHeaderHash(std::span<const u8, kHeaderBytes> encoded) noexcept {
    return hash64(encoded.data(), kHeaderHashedBytes);
}

void encodeEntry(const HpakEntry& e, std::span<u8, kTocEntryBytes> out) noexcept {
    u8* p = out.data();
    storeLE<u64>(p + 0, e.id.value());
    storeLE<u64>(p + 8, e.cookedHash.low);
    storeLE<u64>(p + 16, e.cookedHash.high);
    storeLE<u64>(p + 24, e.offset);
    storeLE<u64>(p + 32, e.compSize);
    storeLE<u64>(p + 40, e.rawSize);
    storeLE<u32>(p + 48, e.firstBlock);
    storeLE<u32>(p + 52, e.blockCount);
    p[56] = static_cast<u8>(e.codec);
    std::memset(p + 57, 0, 7);
}

RawEntry decodeEntry(std::span<const u8, kTocEntryBytes> in) noexcept {
    const u8* p = in.data();
    RawEntry r;
    r.entry.id = AssetId{loadLE<u64>(p + 0)};
    r.entry.cookedHash.low = loadLE<u64>(p + 8);
    r.entry.cookedHash.high = loadLE<u64>(p + 16);
    r.entry.offset = loadLE<u64>(p + 24);
    r.entry.compSize = loadLE<u64>(p + 32);
    r.entry.rawSize = loadLE<u64>(p + 40);
    r.entry.firstBlock = loadLE<u32>(p + 48);
    r.entry.blockCount = loadLE<u32>(p + 52);
    r.codec = p[56];
    r.entry.codec = static_cast<HpakCodec>(r.codec);
    std::memcpy(r.reserved, p + 57, 7);
    return r;
}

} // namespace hpak
} // namespace helios::asset
