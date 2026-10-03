// `.hpak` v0 reader: round trips through assetpipe's writer, large-asset blocking, first-read block
// verification, the overlay mount table, files through the platform layer.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "hpak_test_util.h"
#include "helios/asset/mount_table.h"
#include "helios/core/fs.h"

namespace {

using namespace helios;
using namespace helios::asset;
using namespace helios::asset::test;

std::vector<TestAsset> sampleAssets() {
    return {
        {guidOf(1), compressible(1000), {}},
        {guidOf(2), {}, {}},                                   // empty asset
        {guidOf(3), incompressible(5000), {}},                 // stored raw
        {guidOf(4), compressible(3 * 256 * 1024 + 17, 3), {}}, // 4 zstd blocks
        {guidOf(5), incompressible(300 * 1024, 11), {}},       // 2 raw blocks
        {guidOf(6), {0x42}, {}},
    };
}

TEST_CASE("hpak: every asset round-trips through the writer and the reader") {
    assetpipe::HpakWriterOptions options;
    options.platform = HpakPlatform::Server;
    options.contentBuild = 0x1234'5678'9ABCull;
    options.tags = HpakTags{2, 0xFEED'0000'0001ull, 7};
    const std::vector<TestAsset> assets = sampleAssets();
    auto pak = openPak(buildPak(assets, options));
    REQUIRE(pak.ok());
    const HpakReader& r = **pak;
    CHECK(r.info().platform == HpakPlatform::Server);
    CHECK(r.info().contentBuild == options.contentBuild);
    CHECK(r.info().tags == options.tags);
    CHECK(r.info().assetCount == assets.size());
    CHECK(r.entries().size() == assets.size());
    for (usize i = 1; i < r.entries().size(); ++i) CHECK(r.entries()[i - 1].id < r.entries()[i].id);

    for (const TestAsset& a : assets) {
        const AssetId id = AssetId::fromGuid(a.guid);
        const HpakEntry* e = r.find(id);
        REQUIRE(e != nullptr);
        CHECK(e->offset % hpak::kBlobAlignment == 0);
        CHECK(e->rawSize == a.bytes.size());
        CHECK(e->blockCount == hpak::assetBlockCount(a.bytes.size()));
        CHECK(e->cookedHash == hash128(a.bytes.data(), a.bytes.size()));
        const Result<std::vector<u8>> got = r.read(id);
        REQUIRE(got.ok());
        CHECK(*got == a.bytes);
        std::vector<u8> into(a.bytes.size());
        REQUIRE(r.readInto(*e, into).ok());
        CHECK(into == a.bytes);
    }
    CHECK(r.find(AssetId::fromGuid(guidOf(3)))->codec == HpakCodec::None);
    CHECK(r.find(AssetId::fromGuid(guidOf(4)))->codec == HpakCodec::Zstd);
    CHECK(r.find(AssetId::fromGuid(guidOf(4)))->blockCount == 4);
    CHECK(r.find(AssetId::fromGuid(guidOf(5)))->blockCount == 2);
    CHECK(r.read(AssetId::fromGuid(guidOf(99))).errorCode() == ErrorCode::NotFound);
    CHECK(r.find(AssetId::fromGuid(guidOf(99))) == nullptr);
    CHECK(r.verifyAll().ok());
}

TEST_CASE("hpak: an empty pak is a header and an empty TOC") {
    const std::vector<u8> bytes = buildPak({});
    CHECK(bytes.size() == hpak::kHeaderBlockSize);
    auto pak = openPak(bytes);
    REQUIRE(pak.ok());
    CHECK((*pak)->entries().empty());
    CHECK((*pak)->info().pakBlockCount == 0);
    CHECK((*pak)->verifyAll().ok());
    CHECK((*pak)->read(AssetId{1}).errorCode() == ErrorCode::NotFound);
}

TEST_CASE("hpak: argument checks on read") {
    auto pak = openPak(buildPak(sampleAssets()));
    REQUIRE(pak.ok());
    const HpakEntry& e = *(*pak)->find(AssetId::fromGuid(guidOf(1)));
    std::vector<u8> wrong(e.rawSize + 1);
    CHECK((*pak)->readInto(e, wrong).errorCode() == ErrorCode::InvalidArgument);
    const HpakEntry copy = e; // not one of the pak's entries
    CHECK((*pak)->read(copy).errorCode() == ErrorCode::InvalidArgument);
    CHECK(HpakReader::open(nullptr).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("hpak: an expected platform rejects a pak cooked for another") {
    assetpipe::HpakWriterOptions options;
    options.platform = HpakPlatform::Server;
    const std::vector<u8> bytes = buildPak(sampleAssets(), options);
    HpakOpenOptions client;
    client.platform = HpakPlatform::PcClient;
    CHECK(openPak(bytes, client).errorCode() == ErrorCode::Unsupported);
    HpakOpenOptions server;
    server.platform = HpakPlatform::Server;
    CHECK(openPak(bytes, server).ok());
}

TEST_CASE("hpak: large assets are 256 KiB blocks, and only the blocks read are verified") {
    // Two raw (incompressible) assets so blob offsets map to pak blocks predictably.
    const std::vector<TestAsset> assets = {{guidOf(1), incompressible(600 * 1024, 1), {0, 0, 0, 0}},
                                           {guidOf(2), incompressible(10 * 1024, 2), {0, 0, 0, 1}}};
    auto pak = openPak(buildPak(assets));
    REQUIRE(pak.ok());
    const HpakReader& r = **pak;
    const HpakEntry& big = *r.find(AssetId::fromGuid(guidOf(1)));
    const HpakEntry& small = *r.find(AssetId::fromGuid(guidOf(2)));
    CHECK(big.blockCount == 3); // 256 + 256 + 88 KiB
    CHECK(big.offset == hpak::kHeaderBlockSize);
    CHECK(small.offset == hpak::kHeaderBlockSize + 600 * 1024);
    // 600 KiB + 12 KiB of blobs = 612 KiB → 10 pak blocks (the last one 36 KiB).
    CHECK(r.info().pakBlockCount == 10);
    for (u32 b = 0; b < r.info().pakBlockCount; ++b) CHECK(r.blockState(b) == HpakBlockState::Unverified);

    REQUIRE(r.read(small).ok());
    // The small blob [600 KiB, 610 KiB) of the region lies in pak block 9 only.
    for (u32 b = 0; b < 9; ++b) CHECK(r.blockState(b) == HpakBlockState::Unverified);
    CHECK(r.blockState(9) == HpakBlockState::Verified);
    REQUIRE(r.read(big).ok());
    for (u32 b = 0; b < 10; ++b) CHECK(r.blockState(b) == HpakBlockState::Verified);
    CHECK(r.blockState(10) == HpakBlockState::Unverified); // out of range
}

TEST_CASE("hpak: concurrent readers of one pak") {
    const std::vector<TestAsset> assets = sampleAssets();
    auto pak = openPak(buildPak(assets));
    REQUIRE(pak.ok());
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20; ++i) {
                const TestAsset& a = assets[static_cast<usize>(t + i) % assets.size()];
                const auto got = (*pak)->read(AssetId::fromGuid(a.guid));
                if (!got.ok() || *got != a.bytes) failures.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(failures.load() == 0);
}

TEST_CASE("hpak: written to disk and read back through the platform layer") {
    const auto dir = fs::createUniqueTempDirectory("helios-hpak");
    REQUIRE(dir.ok());
    const fs::Path path = *dir / "content.hpak";
    auto writer = assetpipe::HpakWriter::create();
    REQUIRE(writer.ok());
    for (const TestAsset& a : sampleAssets()) REQUIRE(writer->add(a.guid, a.bytes).ok());
    REQUIRE(writer->writeFile(path).ok());
    CHECK_FALSE(fs::exists(fs::Path(path).concat(".tmp")));
    const auto onDisk = fs::readFile(path);
    REQUIRE(onDisk.ok());
    CHECK(*onDisk == writer->build().value());

    auto pak = HpakReader::openFile(path);
    REQUIRE(pak.ok());
    for (const TestAsset& a : sampleAssets()) {
        const auto got = (*pak)->read(AssetId::fromGuid(a.guid));
        REQUIRE(got.ok());
        CHECK(*got == a.bytes);
    }
    CHECK(HpakReader::openFile(*dir / "missing.hpak").errorCode() == ErrorCode::NotFound);
    pak = Result<std::shared_ptr<HpakReader>>(Error{}); // close before removing the directory (Windows)
    (void)fs::removeAll(*dir);
}

TEST_CASE("hpak mounts: a later pak overlays earlier ones by AssetId") {
    const auto base = openPak(buildPak({{guidOf(1), compressible(100, 1), {}},
                                        {guidOf(2), compressible(200, 2), {}},
                                        {guidOf(3), compressible(300, 3), {}}}));
    const auto patch =
        openPak(buildPak({{guidOf(2), compressible(222, 9), {}}, {guidOf(4), compressible(400, 4), {}}}));
    REQUIRE(base.ok());
    REQUIRE(patch.ok());
    PakMountTable table;
    const auto baseId = table.mount(*base);
    REQUIRE(baseId.ok());
    CHECK(table.assetCount() == 3);
    CHECK(table.read(AssetId::fromGuid(guidOf(2))).value() == compressible(200, 2));
    const auto patchId = table.mount(*patch);
    REQUIRE(patchId.ok());
    CHECK(table.mount(*patch).errorCode() == ErrorCode::AlreadyExists);
    CHECK(table.mount(nullptr).errorCode() == ErrorCode::InvalidArgument);
    CHECK(table.mounts() == std::vector<PakMountId>{*baseId, *patchId});
    CHECK(table.assetCount() == 4);

    // Overlaid, added, and untouched assets.
    CHECK(table.read(AssetId::fromGuid(guidOf(2))).value() == compressible(222, 9));
    CHECK(table.find(AssetId::fromGuid(guidOf(2)))->mount == *patchId);
    CHECK(table.read(AssetId::fromGuid(guidOf(4))).value() == compressible(400, 4));
    CHECK(table.read(AssetId::fromGuid(guidOf(1))).value() == compressible(100, 1));
    CHECK(table.find(AssetId::fromGuid(guidOf(1)))->mount == *baseId);
    CHECK(table.read(AssetId::fromGuid(guidOf(9))).errorCode() == ErrorCode::NotFound);

    // A resolved location keeps its pak alive across an unmount.
    const std::optional<AssetLocation> held = table.find(AssetId::fromGuid(guidOf(4)));
    REQUIRE(held);
    CHECK(table.unmount(*patchId));
    CHECK_FALSE(table.unmount(*patchId));
    CHECK(held->pak->read(*held->entry).value() == compressible(400, 4));
    CHECK(table.read(AssetId::fromGuid(guidOf(2))).value() == compressible(200, 2));
    CHECK_FALSE(table.find(AssetId::fromGuid(guidOf(4))));
    CHECK(table.assetCount() == 3);

    // Unmounting the bottom pak re-resolves to what remains.
    REQUIRE(table.mount(*patch).ok());
    CHECK(table.unmount(*baseId));
    CHECK(table.assetCount() == 2);
    CHECK(table.read(AssetId::fromGuid(guidOf(2))).value() == compressible(222, 9));
    CHECK_FALSE(table.find(AssetId::fromGuid(guidOf(1))));
}

} // namespace
