// `.hpak` v0 writer (02 §6.3): options, deterministic output, the ordering key, 4 KiB alignment, block
// splitting, the TOC order, checksums and the 2 GiB limit.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "helios/asset/hpak_reader.h"
#include "helios/assetpipe/hpak_writer.h"
#include "helios/core/random.h"

namespace {

using namespace helios;
using namespace helios::asset;
using namespace helios::assetpipe;

Guid guidOf(u64 n) { return Guid(0x4000000000004000ull | (n << 16), 0x8000000000000000ull | n); }

std::vector<u8> text(usize size, u32 seed) {
    std::vector<u8> out(size);
    for (usize i = 0; i < size; ++i) out[i] = static_cast<u8>("cooked asset bytes "[(i + seed) % 19]);
    return out;
}

std::vector<u8> noise(usize size, u64 seed) {
    std::vector<u8> out(size);
    Xoshiro256 rng(seed);
    for (u8& b : out) b = static_cast<u8>(rng.next() >> 56);
    return out;
}

HpakWriter makeWriter(const HpakWriterOptions& options = {}) { return HpakWriter::create(options).value(); }

std::shared_ptr<HpakReader> open(std::vector<u8> bytes) {
    return HpakReader::open(makeHpakMemorySource(std::move(bytes))).value();
}

TEST_CASE("hpak writer: options are validated") {
    HpakWriterOptions o;
    CHECK(HpakWriter::create(o).ok());
    o.platform = static_cast<HpakPlatform>(0);
    CHECK(HpakWriter::create(o).errorCode() == ErrorCode::InvalidArgument);
    o = {};
    o.tags.tier = 3;
    CHECK(HpakWriter::create(o).errorCode() == ErrorCode::InvalidArgument);
    o = {};
    o.zstdLevel = 0;
    CHECK(HpakWriter::create(o).errorCode() == ErrorCode::InvalidArgument);
    o.zstdLevel = 20;
    CHECK(HpakWriter::create(o).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("hpak writer: add rejects bad assets and leaves the writer unchanged") {
    HpakWriter w = makeWriter();
    REQUIRE(w.add(guidOf(1), text(10, 1)).ok());
    const u64 size = w.size();
    CHECK(w.add(guidOf(1), text(10, 2)).errorCode() == ErrorCode::AlreadyExists);
    CHECK(w.add(Guid::nil(), text(10, 2)).errorCode() == ErrorCode::InvalidArgument);
    HpakAssetOrder badTier;
    badTier.tier = 3;
    CHECK(w.add(guidOf(2), text(10, 2), badTier).errorCode() == ErrorCode::InvalidArgument);
    CHECK(w.assetCount() == 1);
    CHECK(w.size() == size);
    REQUIRE(w.add(guidOf(2), text(10, 2)).ok()); // the rejected GUID was not taken
}

TEST_CASE("hpak writer: identical input gives byte-identical paks, in any add order") {
    struct In {
        Guid guid;
        std::vector<u8> bytes;
        HpakAssetOrder order;
    };
    std::vector<In> inputs;
    for (u64 i = 1; i <= 24; ++i) {
        HpakAssetOrder o;
        o.tier = static_cast<u8>(i % 3);
        o.group = i % 4;
        o.language = static_cast<u32>(i % 2);
        o.firstUse = i % 5 == 0 ? HpakAssetOrder::kNotRecorded : static_cast<u32>(100 - i);
        inputs.push_back({guidOf(i), i % 3 ? text(1000 * i, static_cast<u32>(i)) : noise(700 * i, i), o});
    }
    inputs.push_back({guidOf(99), text(600 * 1024, 5), {}});
    HpakWriterOptions options;
    options.contentBuild = 77;
    options.tags = HpakTags{1, 5, 2};
    const auto buildIn = [&](const std::vector<In>& order) {
        HpakWriter w = makeWriter(options);
        for (const In& in : order) REQUIRE(w.add(in.guid, in.bytes, in.order).ok());
        return w.build().value();
    };
    const std::vector<u8> a = buildIn(inputs);
    std::vector<In> reversed(inputs.rbegin(), inputs.rend());
    std::vector<In> shuffled = inputs;
    Xoshiro256 rng(17);
    for (usize i = shuffled.size(); i > 1; --i) std::swap(shuffled[i - 1], shuffled[rng.next() % i]);
    CHECK(buildIn(reversed) == a);
    CHECK(buildIn(shuffled) == a);
    CHECK(buildIn(inputs) == a);
    // Building twice from one writer is identical too.
    HpakWriter w = makeWriter(options);
    for (const In& in : inputs) REQUIRE(w.add(in.guid, in.bytes, in.order).ok());
    CHECK(w.build().value() == w.build().value());
    CHECK(w.size() == a.size());
    // Another level or another header field changes the bytes.
    HpakWriterOptions other = options;
    other.contentBuild = 78;
    HpakWriter w2 = makeWriter(other);
    for (const In& in : inputs) REQUIRE(w2.add(in.guid, in.bytes, in.order).ok());
    CHECK(w2.build().value() != a);
}

TEST_CASE("hpak writer: blobs follow tier, group, language, first use, then GUID") {
    HpakWriter w = makeWriter();
    const auto add = [&](u64 n, u8 tier, u64 group, u32 language, u32 firstUse) {
        HpakAssetOrder o{tier, group, language, firstUse};
        REQUIRE(w.add(guidOf(n), text(100 + n, static_cast<u32>(n)), o).ok());
    };
    // Expected blob order: 7 6 5 4 3 1 2 (added scrambled).
    add(1, 1, 0, 0, HpakAssetOrder::kNotRecorded);
    add(2, 1, 0, 0, HpakAssetOrder::kNotRecorded); // GUID breaks the tie with 1
    add(3, 1, 0, 0, 9);                            // recorded first use comes before unrecorded
    add(4, 0, 9, 0, 0);
    add(5, 0, 2, 3, 0);
    add(6, 0, 2, 1, 50);
    add(7, 0, 1, 9, 99);
    const auto pak = open(w.build().value());
    std::vector<std::pair<u64, u64>> byOffset; // (offset, n)
    for (u64 n = 1; n <= 7; ++n) byOffset.emplace_back(pak->find(AssetId::fromGuid(guidOf(n)))->offset, n);
    std::sort(byOffset.begin(), byOffset.end());
    std::vector<u64> order;
    for (const auto& [offset, n] : byOffset) order.push_back(n);
    CHECK(order == std::vector<u64>{7, 6, 5, 4, 3, 1, 2});
}

TEST_CASE("hpak writer: 4 KiB alignment, TOC sorted by AssetId, blocks of 256 KiB") {
    HpakWriter w = makeWriter();
    REQUIRE(w.add(guidOf(1), text(1, 1)).ok());
    REQUIRE(w.add(guidOf(2), text(4096, 2)).ok());
    REQUIRE(w.add(guidOf(3), noise(4097, 3)).ok());               // raw: 2 pages
    REQUIRE(w.add(guidOf(4), text(1024 * 1024 + 5, 4)).ok());    // zstd: 5 blocks
    REQUIRE(w.add(guidOf(5), noise(256 * 1024 + 1, 5)).ok());     // raw: 2 blocks, 256 KiB + 1
    REQUIRE(w.add(guidOf(6), {}).ok());
    const std::vector<u8> bytes = w.build().value();
    CHECK(bytes.size() == w.size());
    const auto pak = open(bytes);
    const std::span<const HpakEntry> entries = pak->entries();
    for (usize i = 0; i < entries.size(); ++i) {
        CHECK(entries[i].offset % 4096 == 0);
        if (i > 0) CHECK(entries[i - 1].id < entries[i].id);
    }
    CHECK(pak->info().dataEnd % 4096 == 0);
    const HpakEntry& raw = *pak->find(AssetId::fromGuid(guidOf(3)));
    CHECK(raw.codec == HpakCodec::None);
    CHECK(raw.compSize == 4097);
    const HpakEntry& big = *pak->find(AssetId::fromGuid(guidOf(4)));
    CHECK(big.codec == HpakCodec::Zstd);
    CHECK(big.blockCount == 5);
    CHECK(big.compSize < big.rawSize / 10);
    const HpakEntry& two = *pak->find(AssetId::fromGuid(guidOf(5)));
    CHECK(two.codec == HpakCodec::None);
    CHECK(two.blockCount == 2);
    CHECK(pak->find(AssetId::fromGuid(guidOf(6)))->blockCount == 0);
    // The blob region is the 4 KiB-padded blobs back to back.
    u64 padded = 0;
    for (const HpakEntry& e : entries) padded += alignUp<u64>(e.compSize, 4096);
    CHECK(pak->info().dataEnd == hpak::kHeaderBlockSize + padded);
    // Padding is zero and every pak block checksum matches.
    CHECK(pak->verifyAll().ok());
    CHECK(pak->read(AssetId::fromGuid(guidOf(5))).value() == noise(256 * 1024 + 1, 5));
}

TEST_CASE("hpak writer: the layout enforces the 2 GiB limit exactly, without writing 2 GiB") {
    constexpr u64 kHeader = hpak::kHeaderBlockSize;
    // One blob of S bytes and one block: file = 4096 + S + 64 + 4 + 8 * ceil(S / 64 KiB).
    const auto fileFor = [](u64 s) { return planHpakLayout(std::vector<u64>{s}, 1); };
    const u64 blocks = (hpak::kMaxPakSize - kHeader) / hpak::kPakBlockSize;
    const u64 tocBytes = 64 + 4 + 8 * blocks;
    // The largest 4 KiB multiple that fits with its TOC.
    const u64 fit = alignDown<u64>(hpak::kMaxPakSize - kHeader - tocBytes, 4096);
    const auto ok = fileFor(fit);
    REQUIRE(ok.ok());
    CHECK(ok->fileSize <= hpak::kMaxPakSize);
    CHECK(ok->blobOffsets == std::vector<u64>{kHeader});
    CHECK(ok->dataEnd == kHeader + fit);
    const auto over = fileFor(fit + 4096);
    CHECK(over.errorCode() == ErrorCode::LimitExceeded);
    CHECK(over.error().message.find("2 GiB") != std::string::npos);
    // Exactly 2 GiB is allowed, one byte more is not (TOC sized to land on the boundary).
    const u64 dataBytes = 32 * 4096;
    const u64 tocFor2GiB = hpak::kMaxPakSize - kHeader - dataBytes;
    const u64 pakBlocks = hpak::pakBlockCount(dataBytes);
    const u64 assetBlocks = (tocFor2GiB - 64 - 8 * pakBlocks) / 4;
    REQUIRE(hpak::tocSize(1, assetBlocks, pakBlocks) == tocFor2GiB);
    const auto exact = planHpakLayout(std::vector<u64>{dataBytes}, assetBlocks);
    REQUIRE(exact.ok());
    CHECK(exact->fileSize == hpak::kMaxPakSize);
    CHECK(planHpakLayout(std::vector<u64>{dataBytes}, assetBlocks + 1).errorCode() == ErrorCode::LimitExceeded);
    // A single blob above 2 GiB and counts above u32 are refused without overflowing.
    CHECK(planHpakLayout(std::vector<u64>{~u64(0)}, 1).errorCode() == ErrorCode::LimitExceeded);
    CHECK(planHpakLayout(std::vector<u64>{4096, ~u64(0) - 4096}, 2).errorCode() == ErrorCode::LimitExceeded);
    CHECK(planHpakLayout({}, u64(1) << 32).errorCode() == ErrorCode::LimitExceeded);
    // Offsets of several blobs.
    const auto three = planHpakLayout(std::vector<u64>{1, 0, 4097}, 2);
    REQUIRE(three.ok());
    CHECK(three->blobOffsets == std::vector<u64>{kHeader, kHeader + 4096, kHeader + 4096});
    CHECK(three->dataEnd == kHeader + 4096 + 8192);
    CHECK(three->fileSize == three->dataEnd + hpak::tocSize(3, 2, 1));
}

TEST_CASE("hpak writer: size() predicts the built size") {
    HpakWriter w = makeWriter();
    CHECK(w.size() == w.build().value().size());
    for (u64 i = 1; i <= 40; ++i) {
        REQUIRE(w.add(guidOf(i), i % 2 ? text(i * 3000, 1) : noise(i * 2000, i)).ok());
        CHECK(w.size() == w.build().value().size());
    }
}

TEST_CASE("hpak writer: a sink error stops emit") {
    HpakWriter w = makeWriter();
    REQUIRE(w.add(guidOf(1), text(100, 1)).ok());
    int calls = 0;
    const Result<void> r = w.emit([&](std::span<const u8>) -> Result<void> {
        if (++calls == 2) return Error{ErrorCode::IoError, "disk full"};
        return {};
    });
    CHECK(r.errorCode() == ErrorCode::IoError);
    CHECK(calls == 2);
}

TEST_CASE("hpak writer: resealHpak restores the checksums of a patched pak") {
    HpakWriter w = makeWriter();
    REQUIRE(w.add(guidOf(1), noise(5000, 1)).ok());
    std::vector<u8> bytes = w.build().value();
    const u64 blob = hpak::kHeaderBlockSize;
    bytes[blob + 3] ^= 1;                        // a data byte: its pak block checksum now fails
    const u64 contentBuildAt = 16;
    bytes[contentBuildAt] ^= 1;                  // a header field: the header checksum now fails
    CHECK(HpakReader::open(makeHpakMemorySource(bytes)).errorCode() == ErrorCode::Corrupt);
    resealHpak(bytes);
    const auto pak = open(bytes);
    CHECK(pak->verifyAll().ok());
    // The data no longer matches the cooked hash, which reseal does not touch.
    CHECK(pak->read(AssetId::fromGuid(guidOf(1))).errorCode() == ErrorCode::Corrupt);
    // Nonsense headers are left alone instead of being read out of bounds.
    std::vector<u8> junk(5000, 0xFF);
    resealHpak(junk);
    std::vector<u8> tiny(10, 0);
    resealHpak(tiny);
    CHECK(tiny == std::vector<u8>(10, 0));
}

} // namespace
