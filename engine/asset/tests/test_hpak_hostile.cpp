// Hostile and damaged paks: every corrupted-field class fails open() or read() with a Result error,
// and a bad pak block calls the re-fetch hook once per block (02 §6.3 "Integrity", 08 §2.6).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <thread>

#include "hpak_test_util.h"

namespace {

using namespace helios;
using namespace helios::asset;
using namespace helios::asset::test;

std::vector<TestAsset> hostileAssets() {
    return {
        {guidOf(1), compressible(1000), {}},
        {guidOf(2), {}, {}},
        {guidOf(3), incompressible(5000), {}},
        {guidOf(4), compressible(3 * 256 * 1024 + 17, 3), {}},
    };
}

u64 tocAt(const std::vector<u8>& p) { return loadLE<u64>(p.data() + 40); }
u32 assetCountOf(const std::vector<u8>& p) { return loadLE<u32>(p.data() + 56); }
u64 entryAt(const std::vector<u8>& p, u32 i) { return tocAt(p) + u64(i) * hpak::kTocEntryBytes; }
u64 blockSizeAt(const std::vector<u8>& p, u32 block) {
    return tocAt(p) + u64(assetCountOf(p)) * hpak::kTocEntryBytes + u64(block) * hpak::kBlockSizeBytes;
}

/// Index of the TOC entry of `guid` in `pak`.
u32 entryIndex(const std::vector<u8>& pak, const Guid& guid) {
    const AssetId id = AssetId::fromGuid(guid);
    for (u32 i = 0; i < assetCountOf(pak); ++i)
        if (loadLE<u64>(pak.data() + entryAt(pak, i)) == id.value()) return i;
    HELIOS_VERIFY(false, "no such entry");
    return 0;
}

struct Case {
    const char* name;
    std::function<void(std::vector<u8>&)> mutate;
    ErrorCode expected;
    bool reseal = true; ///< Recompute the checksums so the field check, not the checksum, must catch it.
};

void runOpenCases(const std::vector<u8>& base, const std::vector<Case>& cases) {
    for (const Case& c : cases) {
        INFO(c.name);
        std::vector<u8> bytes = base;
        c.mutate(bytes);
        if (c.reseal) assetpipe::resealHpak(bytes);
        const auto opened = openPak(std::move(bytes));
        CHECK(opened.errorCode() == c.expected);
    }
}

TEST_CASE("hpak hostile: every corrupted header and layout field is rejected at open") {
    const std::vector<u8> base = buildPak(hostileAssets());
    REQUIRE(openPak(base).ok());
    const u64 size = base.size();
    const std::vector<Case> cases = {
        {"shorter than the header block", [](auto& p) { p.resize(100); }, ErrorCode::Corrupt, false},
        {"bad magic", [](auto& p) { p[0] = 'X'; }, ErrorCode::Corrupt},
        {"other format version", [](auto& p) { storeLE<u16>(p.data() + 4, 1); }, ErrorCode::VersionMismatch},
        {"header checksum", [](auto& p) { p[17] ^= 1; }, ErrorCode::Corrupt, false},
        {"TOC checksum", [](auto& p) { p[tocAt(p) + 3] ^= 1; }, ErrorCode::Corrupt, false},
        {"reserved u16", [](auto& p) { p[6] = 1; }, ErrorCode::Corrupt},
        {"flags", [](auto& p) { storeLE<u32>(p.data() + 12, 1); }, ErrorCode::Corrupt},
        {"reserved after the tier", [](auto& p) { p[38] = 1; }, ErrorCode::Corrupt},
        {"reserved u32", [](auto& p) { p[68] = 1; }, ErrorCode::Corrupt},
        {"header block tail", [](auto& p) { p[100] = 1; }, ErrorCode::Corrupt},
        {"platform 0", [](auto& p) { storeLE<u32>(p.data() + 8, 0); }, ErrorCode::Corrupt},
        {"unknown platform", [](auto& p) { storeLE<u32>(p.data() + 8, 9); }, ErrorCode::Corrupt},
        {"tier 3", [](auto& p) { p[36] = 3; }, ErrorCode::Corrupt},
        {"TOC offset misaligned", [](auto& p) { storeLE<u64>(p.data() + 40, tocAt(p) + 8); }, ErrorCode::Corrupt},
        {"TOC offset inside the header", [](auto& p) { storeLE<u64>(p.data() + 40, 0); }, ErrorCode::Corrupt},
        {"TOC offset past the end", [](auto& p) { storeLE<u64>(p.data() + 40, ~u64(0) & ~u64(4095)); },
         ErrorCode::Corrupt},
        {"TOC size", [](auto& p) { storeLE<u64>(p.data() + 48, loadLE<u64>(p.data() + 48) + 8); }, ErrorCode::Corrupt},
        {"TOC size wraps", [](auto& p) { storeLE<u64>(p.data() + 48, ~u64(0)); }, ErrorCode::Corrupt},
        {"asset count", [](auto& p) { storeLE<u32>(p.data() + 56, assetCountOf(p) + 1); }, ErrorCode::Corrupt},
        {"asset count huge", [](auto& p) { storeLE<u32>(p.data() + 56, 0xFFFFFFFFu); }, ErrorCode::Corrupt},
        {"asset block count", [](auto& p) { storeLE<u32>(p.data() + 60, loadLE<u32>(p.data() + 60) + 2); },
         ErrorCode::Corrupt},
        {"pak block count", [](auto& p) { storeLE<u32>(p.data() + 64, loadLE<u32>(p.data() + 64) + 1); },
         ErrorCode::Corrupt},
        {"truncated", [](auto& p) { p.pop_back(); }, ErrorCode::Corrupt},
        {"trailing byte", [](auto& p) { p.push_back(0); }, ErrorCode::Corrupt},
        {"all zeros", [size](auto& p) { p.assign(size, 0); }, ErrorCode::Corrupt, false},
    };
    runOpenCases(base, cases);
}

TEST_CASE("hpak hostile: every corrupted TOC entry field is rejected at open") {
    const std::vector<u8> base = buildPak(hostileAssets());
    const u32 raw = entryIndex(base, guidOf(3));   // codec None, 1 block
    const u32 zstd = entryIndex(base, guidOf(4));  // codec Zstd, 4 blocks
    const u32 empty = entryIndex(base, guidOf(2)); // 0 blocks
    const auto field = [](u32 entry, usize at) {
        return [entry, at](std::vector<u8>& p) -> u8* { return p.data() + entryAt(p, entry) + at; };
    };
    const auto zstdFirst = [&](const std::vector<u8>& p) { return loadLE<u32>(p.data() + entryAt(p, zstd) + 48); };
    const auto rawFirst = [&](const std::vector<u8>& p) { return loadLE<u32>(p.data() + entryAt(p, raw) + 48); };
    const std::vector<Case> cases = {
        {"AssetId 0", [&](auto& p) { storeLE<u64>(field(0, 0)(p), 0); }, ErrorCode::Corrupt},
        {"ids out of order",
         [&](auto& p) { storeLE<u64>(field(1, 0)(p), loadLE<u64>(field(0, 0)(p))); }, ErrorCode::Corrupt},
        {"unknown codec", [&](auto& p) { *field(zstd, 56)(p) = 2; }, ErrorCode::Corrupt},
        {"reserved entry byte", [&](auto& p) { *field(raw, 60)(p) = 1; }, ErrorCode::Corrupt},
        {"decoded size above 2 GiB",
         [&](auto& p) { storeLE<u64>(field(zstd, 40)(p), hpak::kMaxAssetSize + 1); }, ErrorCode::Corrupt},
        {"decoded size disagrees with the block count",
         [&](auto& p) { storeLE<u64>(field(zstd, 40)(p), 256 * 1024); }, ErrorCode::Corrupt},
        {"block count", [&](auto& p) { storeLE<u32>(field(zstd, 52)(p), 5); }, ErrorCode::Corrupt},
        {"first block", [&](auto& p) { storeLE<u32>(field(zstd, 48)(p), zstdFirst(p) + 1); }, ErrorCode::Corrupt},
        {"blob offset misaligned", [&](auto& p) { storeLE<u64>(field(raw, 24)(p), loadLE<u64>(field(raw, 24)(p)) + 1); },
         ErrorCode::Corrupt},
        {"blob offset inside the header", [&](auto& p) { storeLE<u64>(field(raw, 24)(p), 0); }, ErrorCode::Corrupt},
        {"blob offset past the blob region", [&](auto& p) { storeLE<u64>(field(empty, 24)(p), tocAt(p) + 4096); },
         ErrorCode::Corrupt},
        {"blob runs past the blob region",
         [&](auto& p) { storeLE<u64>(field(raw, 24)(p), tocAt(p) - 4096); }, ErrorCode::Corrupt},
        {"stored size disagrees with the block sizes",
         [&](auto& p) { storeLE<u64>(field(zstd, 32)(p), loadLE<u64>(field(zstd, 32)(p)) + 1); }, ErrorCode::Corrupt},
        {"empty asset with stored bytes", [&](auto& p) { storeLE<u64>(field(empty, 32)(p), 1); }, ErrorCode::Corrupt},
        {"zstd block of 0 bytes", [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, zstdFirst(p)), 0); },
         ErrorCode::Corrupt},
        {"zstd block above the zstd bound",
         [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, zstdFirst(p)), 0x7FFFFFFFu); }, ErrorCode::Corrupt},
        {"raw block size differs from the raw size",
         [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, rawFirst(p)), 4999); }, ErrorCode::Corrupt},
    };
    runOpenCases(base, cases);
}

TEST_CASE("hpak hostile: a pak above 2 GiB is refused before anything is read") {
    struct Huge final : IHpakSource {
        u64 size() const override { return hpak::kMaxPakSize + 1; }
        Result<void> readAt(u64, std::span<u8>) const override { return Error{ErrorCode::IoError, "not read"}; }
        std::string describe() const override { return "huge"; }
    };
    CHECK(HpakReader::open(std::make_unique<Huge>()).errorCode() == ErrorCode::LimitExceeded);
}

TEST_CASE("hpak hostile: damaged blob bytes behind valid checksums fail the read, not the open") {
    const std::vector<u8> base = buildPak(hostileAssets());
    const u32 zstd = entryIndex(base, guidOf(4));
    const u32 raw = entryIndex(base, guidOf(3));
    const u64 zstdBlob = loadLE<u64>(base.data() + entryAt(base, zstd) + 24);
    const u64 rawBlob = loadLE<u64>(base.data() + entryAt(base, raw) + 24);

    SUBCASE("a zstd frame") {
        std::vector<u8> p = base;
        for (u64 i = 0; i < 64; ++i) p[zstdBlob + 4 + i] ^= 0x5A;
        assetpipe::resealHpak(p);
        auto pak = openPak(std::move(p));
        REQUIRE(pak.ok());
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(4))).errorCode() == ErrorCode::Corrupt);
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(1))).ok());
    }
    SUBCASE("raw bytes (only the cooked hash notices)") {
        std::vector<u8> p = base;
        p[rawBlob + 10] ^= 1;
        assetpipe::resealHpak(p);
        auto pak = openPak(std::move(p));
        REQUIRE(pak.ok());
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(3))).errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("the cooked hash") {
        std::vector<u8> p = base;
        p[entryAt(p, raw) + 8] ^= 1;
        assetpipe::resealHpak(p);
        auto pak = openPak(std::move(p));
        REQUIRE(pak.ok());
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(3))).errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("a pak block without resealing fails its checksum") {
        std::vector<u8> p = base;
        p[rawBlob + 10] ^= 1;
        auto pak = openPak(std::move(p));
        REQUIRE(pak.ok());
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(3))).errorCode() == ErrorCode::Corrupt);
        CHECK((*pak)->verifyAll().errorCode() == ErrorCode::Corrupt);
    }
}

/// Counts hook calls per block and answers with a scripted status.
class CountingRefetcher final : public IBlockRefetcher {
public:
    RefetchStatus refetch(const HpakBadBlock& block) override {
        ++calls[block.index];
        last = block;
        if (onRefetch) onRefetch(block);
        return status;
    }
    std::map<u32, int> calls;
    HpakBadBlock last;
    RefetchStatus status = RefetchStatus::Failed;
    std::function<void(const HpakBadBlock&)> onRefetch;
};

/// Two raw assets in separate pak blocks of one pak, behind a mutable source.
struct Damaged {
    std::shared_ptr<CountingRefetcher> hook = std::make_shared<CountingRefetcher>();
    MutableSource* source = nullptr;
    std::shared_ptr<HpakReader> pak;
    const HpakEntry* a = nullptr; // pak block 0
    const HpakEntry* b = nullptr; // pak block 2

    Damaged() {
        const std::vector<TestAsset> assets = {{guidOf(1), incompressible(1000, 1), {0, 0, 0, 0}},
                                               {guidOf(9), incompressible(130 * 1024, 3), {0, 0, 0, 1}},
                                               {guidOf(2), incompressible(1000, 2), {0, 0, 0, 2}}};
        auto owned = std::make_unique<MutableSource>(buildPak(assets));
        source = owned.get();
        HpakOpenOptions options;
        options.refetcher = hook;
        pak = HpakReader::open(std::move(owned), options).value();
        a = pak->find(AssetId::fromGuid(guidOf(1)));
        b = pak->find(AssetId::fromGuid(guidOf(2)));
        HELIOS_VERIFY(a->offset == hpak::kHeaderBlockSize && b->offset == hpak::kHeaderBlockSize + 136 * 1024);
    }
    void flip(const HpakEntry* e) { source->set(e->offset + 5, static_cast<u8>(source->get(e->offset + 5) ^ 0xFF)); }
};

TEST_CASE("hpak integrity: a bad block calls the hook once per block, whatever reads it") {
    Damaged d;
    d.flip(d.a);
    d.flip(d.b);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
    CHECK(d.hook->last.pak == d.pak.get());
    CHECK(d.hook->last.offset == hpak::kHeaderBlockSize);
    CHECK(d.hook->last.size == hpak::kPakBlockSize);
    CHECK(d.hook->last.expected != d.hook->last.actual);
    CHECK(d.pak->blockState(0) == HpakBlockState::Bad);
    CHECK(d.pak->read(*d.b).errorCode() == ErrorCode::Corrupt);
    CHECK(d.pak->verifyAll().errorCode() == ErrorCode::Corrupt);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}, {2, 1}});
    // The undamaged middle asset spans blocks 0..2: it fails on block 0 without another hook call.
    CHECK(d.pak->read(AssetId::fromGuid(guidOf(9))).errorCode() == ErrorCode::Corrupt);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}, {2, 1}});
}

TEST_CASE("hpak integrity: a hook that repairs the block lets the read succeed") {
    Damaged d;
    const u8 good = d.source->get(d.a->offset + 5);
    d.flip(d.a);
    d.hook->status = RefetchStatus::Repaired;
    d.hook->onRefetch = [&](const HpakBadBlock&) { d.source->set(d.a->offset + 5, good); };
    const auto got = d.pak->read(*d.a);
    REQUIRE(got.ok());
    CHECK(*got == incompressible(1000, 1));
    CHECK(d.pak->blockState(0) == HpakBlockState::Verified);
    CHECK(d.pak->read(*d.a).ok());
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
}

TEST_CASE("hpak integrity: a hook that claims a repair it did not make leaves the block bad") {
    Damaged d;
    d.flip(d.a);
    d.hook->status = RefetchStatus::Repaired;
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    CHECK(d.pak->blockState(0) == HpakBlockState::Bad);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
}

TEST_CASE("hpak integrity: a pending re-fetch reads as Busy until retryBlocks") {
    Damaged d;
    const u8 good = d.source->get(d.a->offset + 5);
    d.flip(d.a);
    d.hook->status = RefetchStatus::Pending;
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Busy);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Busy);
    CHECK(d.pak->blockState(0) == HpakBlockState::Pending);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});

    // The re-fetch lands with the bytes still bad: a retry re-verifies and calls the hook again.
    d.pak->retryBlocks(d.a->offset, 1);
    CHECK(d.pak->blockState(0) == HpakBlockState::Unverified);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Busy);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 2}});

    // Now it really lands.
    d.source->set(d.a->offset + 5, good);
    d.pak->retryBlocks(0, ~u64(0)); // a range past the end is clamped
    REQUIRE(d.pak->read(*d.a).ok());
    CHECK(d.pak->blockState(0) == HpakBlockState::Verified);
    CHECK(d.hook->calls == std::map<u32, int>{{0, 2}});
    // retryBlocks leaves verified blocks alone.
    d.pak->retryBlocks(0, ~u64(0));
    CHECK(d.pak->blockState(0) == HpakBlockState::Verified);
}

TEST_CASE("hpak integrity: without a hook a bad block is Corrupt") {
    std::vector<u8> bytes = buildPak({{guidOf(1), incompressible(1000, 1), {}}});
    bytes[hpak::kHeaderBlockSize + 5] ^= 1;
    auto pak = openPak(std::move(bytes));
    REQUIRE(pak.ok());
    CHECK((*pak)->read(AssetId::fromGuid(guidOf(1))).errorCode() == ErrorCode::Corrupt);
    CHECK((*pak)->blockState(0) == HpakBlockState::Bad);
}

TEST_CASE("hpak integrity: concurrent readers of one bad block call the hook once") {
    Damaged d;
    d.flip(d.b);
    std::atomic<int> hookCalls{0};
    d.hook->onRefetch = [&](const HpakBadBlock&) {
        hookCalls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // widen the race window
    };
    std::vector<std::thread> threads;
    std::atomic<int> corrupt{0};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10; ++i)
                if (d.pak->read(*d.b).errorCode() == ErrorCode::Corrupt) corrupt.fetch_add(1);
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(hookCalls.load() == 1);
    CHECK(corrupt.load() == 80);
}

} // namespace
