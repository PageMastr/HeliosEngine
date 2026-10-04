// Memory bounds of a read (02 §6.3, hostile input): an asset's rawSize is the TOC's claim, so a read
// must not commit it before blocks really decode, and HpakOpenOptions::maxAssetSize caps it.

#include <doctest/doctest.h>

#include "hpak_test_util.h"
#include "helios/asset/mount_table.h"

namespace {

using namespace helios;
using namespace helios::asset;
using namespace helios::asset::test;

/// A zstd frame of 256 KiB (one asset block) of `value`: two 128 KiB RLE blocks, 17 bytes in all.
std::vector<u8> rleFrame(u8 value) {
    std::vector<u8> f = {0x28, 0xB5, 0x2F, 0xFD, 0xA0}; // magic; single segment, 4-byte content size
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<u8>(hpak::kAssetBlockSize >> (8 * i)));
    for (u32 last = 0; last < 2; ++last) {
        const u32 header = ((128u * 1024u) << 3) | (1u << 1) | last; // RLE block of 128 KiB
        for (int i = 0; i < 3; ++i) f.push_back(static_cast<u8>(header >> (8 * i)));
        f.push_back(value);
    }
    return f;
}

/// A pak of a few KB whose one zstd asset claims `rawSize` decoded bytes: its first `validBlocks`
/// blocks are real frames (256 KiB of zeros each), the rest one garbage byte each. Every field and
/// checksum is valid; the cooked hash is not.
std::vector<u8> claimingPak(u64 rawSize, u32 validBlocks = 0) {
    using namespace hpak;
    const u32 blocks = static_cast<u32>(assetBlockCount(rawSize));
    const std::vector<u8> frame = rleFrame(0);
    const u64 stored = u64(validBlocks) * frame.size() + (blocks - validBlocks);
    const u64 blobBytes = alignUp<u64>(stored, kBlobAlignment);
    const u32 pakBlocks = static_cast<u32>(pakBlockCount(blobBytes));
    Header h;
    h.platform = toUnderlying(HpakPlatform::PcClient);
    h.tocOffset = kHeaderBlockSize + blobBytes;
    h.tocSize = tocSize(1, blocks, pakBlocks);
    h.assetCount = 1;
    h.assetBlockCount = blocks;
    h.pakBlockCount = pakBlocks;
    std::vector<u8> p(static_cast<usize>(h.tocOffset + h.tocSize), 0);
    encodeHeader(h, std::span<u8, kHeaderBytes>(p.data(), kHeaderBytes));
    u8* blob = p.data() + kHeaderBlockSize;
    for (u32 k = 0; k < validBlocks; ++k) std::copy(frame.begin(), frame.end(), blob + u64(k) * frame.size());
    std::fill_n(blob + u64(validBlocks) * frame.size(), blocks - validBlocks, u8(0xA5));
    HpakEntry e;
    e.id = AssetId::fromGuid(guidOf(1));
    e.offset = kHeaderBlockSize;
    e.compSize = stored;
    e.rawSize = rawSize;
    e.blockCount = blocks;
    e.codec = HpakCodec::Zstd;
    u8* toc = p.data() + h.tocOffset;
    encodeEntry(e, std::span<u8, kTocEntryBytes>(toc, kTocEntryBytes));
    for (u32 k = 0; k < blocks; ++k)
        storeLE<u32>(toc + kTocEntryBytes + u64(k) * kBlockSizeBytes,
                     k < validBlocks ? static_cast<u32>(frame.size()) : 1u);
    assetpipe::resealHpak(p);
    return p;
}

TEST_CASE("hpak limits: a 2 GiB claim costs only what its blocks really decode") {
    SUBCASE("a garbage first block") {
        const std::vector<u8> bytes = claimingPak(hpak::kMaxAssetSize);
        CHECK(bytes.size() < 64 * kKiB);
        auto pak = openPak(bytes);
        REQUIRE(pak.ok());
        const HpakEntry& e = (*pak)->entries()[0];
        REQUIRE(e.rawSize == hpak::kMaxAssetSize);
        REQUIRE(e.blockCount == 8192);
        std::vector<u8> out;
        CHECK((*pak)->read(e, out).errorCode() == ErrorCode::Corrupt);
        CHECK(out.capacity() <= 1 * kMiB); // read()'s start; preallocating rawSize committed 2 GiB
        CHECK((*pak)->read(e).errorCode() == ErrorCode::Corrupt);
        PakMountTable table;
        REQUIRE(table.mount(*pak).ok());
        CHECK(table.read(e.id).errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("eight good blocks, then garbage") {
        auto pak = openPak(claimingPak(hpak::kMaxAssetSize, 8));
        REQUIRE(pak.ok());
        std::vector<u8> out;
        CHECK((*pak)->read((*pak)->entries()[0], out).errorCode() == ErrorCode::Corrupt);
        CHECK(out.size() == 8 * hpak::kAssetBlockSize); // the blocks that decoded
        CHECK(out.capacity() <= 2 * out.size());
    }
    SUBCASE("the frames are real: a claim they fill decodes up to the cooked hash") {
        auto pak = openPak(claimingPak(3 * hpak::kAssetBlockSize, 3));
        REQUIRE(pak.ok());
        std::vector<u8> out;
        const Result<void> got = (*pak)->read((*pak)->entries()[0], out);
        CHECK(got.errorCode() == ErrorCode::Corrupt);
        CHECK(got.error().message.find("cooked hash") != std::string::npos);
        CHECK(out == std::vector<u8>(3 * hpak::kAssetBlockSize, 0));
    }
}

TEST_CASE("hpak limits: read() grows past its start and reuses a caller's buffer") {
    const std::vector<u8> big = compressible(5 * kMiB + 3, 4); // 21 blocks, past the 1 MiB start
    const std::vector<u8> small = compressible(1000);
    auto pak = openPak(buildPak({{guidOf(1), big, {}}, {guidOf(2), small, {}}}));
    REQUIRE(pak.ok());
    const HpakEntry& bigEntry = *(*pak)->find(AssetId::fromGuid(guidOf(1)));
    CHECK((*pak)->read(bigEntry).value() == big);
    std::vector<u8> out;
    REQUIRE((*pak)->read(bigEntry, out).ok());
    CHECK(out == big);
    CHECK(out.capacity() <= big.size());
    const usize capacity = out.capacity();
    REQUIRE((*pak)->read(*(*pak)->find(AssetId::fromGuid(guidOf(2))), out).ok());
    CHECK(out == small);
    CHECK(out.capacity() == capacity); // reused, not reallocated
}

TEST_CASE("hpak limits: maxAssetSize refuses larger assets with LimitExceeded") {
    HpakOpenOptions options;
    options.maxAssetSize = 4096;
    auto pak = openPak(buildPak({{guidOf(1), compressible(4096), {}}, {guidOf(2), compressible(4097), {}}}),
                       options);
    REQUIRE(pak.ok());
    const HpakEntry& atCap = *(*pak)->find(AssetId::fromGuid(guidOf(1)));
    const HpakEntry& above = *(*pak)->find(AssetId::fromGuid(guidOf(2)));
    CHECK((*pak)->read(atCap).value() == compressible(4096));
    CHECK((*pak)->read(above).errorCode() == ErrorCode::LimitExceeded);
    std::vector<u8> into(above.rawSize);
    CHECK((*pak)->readInto(above, into).errorCode() == ErrorCode::LimitExceeded);

    // Through a mount table, before any block of a hostile claim is touched.
    HpakOpenOptions runtime;
    runtime.maxAssetSize = 64 * kMiB;
    auto claim = openPak(claimingPak(hpak::kMaxAssetSize), runtime);
    REQUIRE(claim.ok());
    PakMountTable table;
    REQUIRE(table.mount(*claim).ok());
    CHECK(table.read((*claim)->entries()[0].id).errorCode() == ErrorCode::LimitExceeded);
    CHECK((*claim)->blockState(0) == HpakBlockState::Unverified);

    // A cap above the format's means the format's.
    HpakOpenOptions huge;
    huge.maxAssetSize = ~u64(0);
    auto unbounded = openPak(claimingPak(hpak::kMaxAssetSize), huge);
    REQUIRE(unbounded.ok());
    CHECK((*unbounded)->read((*unbounded)->entries()[0]).errorCode() == ErrorCode::Corrupt);
}

} // namespace
