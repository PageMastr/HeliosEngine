// Hostile and damaged paks: every corrupted-field class fails open() or read() with a Result error,
// and a bad pak block calls the re-fetch hook once per block (02 §6.3 "Integrity", 08 §2.6).

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
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

/// Replaces the one stored block of single-block zstd entry `i` with `frame`, which must fit the blob's
/// 4 KiB slot, and reseals the pak so only the frame itself is wrong.
void replaceFrame(std::vector<u8>& p, u32 i, const std::vector<u8>& frame) {
    const u64 blob = loadLE<u64>(p.data() + entryAt(p, i) + 24);
    const u32 block = loadLE<u32>(p.data() + entryAt(p, i) + 48);
    HELIOS_VERIFY(frame.size() <= hpak::kBlobAlignment && blob + hpak::kBlobAlignment <= tocAt(p));
    std::fill_n(p.begin() + static_cast<std::ptrdiff_t>(blob), hpak::kBlobAlignment, u8(0));
    std::copy(frame.begin(), frame.end(), p.begin() + static_cast<std::ptrdiff_t>(blob));
    storeLE<u64>(p.data() + entryAt(p, i) + 32, frame.size());
    storeLE<u32>(p.data() + blockSizeAt(p, block), static_cast<u32>(frame.size()));
    assetpipe::resealHpak(p);
}

/// A zstd frame (RFC 8878) holding `raw` (< 64 KiB) as one raw block; the header states the decoded
/// size only when `statesSize`.
std::vector<u8> rawZstdFrame(const std::vector<u8>& raw, bool statesSize) {
    HELIOS_VERIFY(raw.size() >= 256 && raw.size() < 16 * 1024);
    std::vector<u8> f = {0x28, 0xB5, 0x2F, 0xFD};
    if (statesSize) {
        f.push_back(0x60); // single segment, 2-byte content size (stored minus 256)
        f.push_back(static_cast<u8>((raw.size() - 256) & 0xFF));
        f.push_back(static_cast<u8>((raw.size() - 256) >> 8));
    } else {
        f.push_back(0x00); // no content size; a window descriptor follows
        f.push_back(0x20); // 16 KiB window
    }
    const u32 blockHeader = static_cast<u32>(raw.size() << 3) | 1u; // raw block, last
    f.push_back(static_cast<u8>(blockHeader & 0xFF));
    f.push_back(static_cast<u8>((blockHeader >> 8) & 0xFF));
    f.push_back(static_cast<u8>(blockHeader >> 16));
    f.insert(f.end(), raw.begin(), raw.end());
    return f;
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
        {"TOC offset misaligned", [](auto& p) { storeLE<u64>(p.data() + 40, tocAt(p) + 8); },
         ErrorCode::Corrupt},
        {"TOC offset inside the header", [](auto& p) { storeLE<u64>(p.data() + 40, 0); }, ErrorCode::Corrupt},
        {"TOC offset past the end", [](auto& p) { storeLE<u64>(p.data() + 40, ~u64(0) & ~u64(4095)); },
         ErrorCode::Corrupt},
        {"TOC size", [](auto& p) { storeLE<u64>(p.data() + 48, loadLE<u64>(p.data() + 48) + 8); },
         ErrorCode::Corrupt},
        {"TOC size wraps", [](auto& p) { storeLE<u64>(p.data() + 48, ~u64(0)); }, ErrorCode::Corrupt},
        {"asset count", [](auto& p) { storeLE<u32>(p.data() + 56, assetCountOf(p) + 1); },
         ErrorCode::Corrupt},
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
    const auto zstdFirst = [&](const std::vector<u8>& p) {
        return loadLE<u32>(p.data() + entryAt(p, zstd) + 48);
    };
    const auto rawFirst = [&](const std::vector<u8>& p) {
        return loadLE<u32>(p.data() + entryAt(p, raw) + 48);
    };
    const std::vector<Case> cases = {
        {"AssetId 0", [&](auto& p) { storeLE<u64>(field(0, 0)(p), 0); }, ErrorCode::Corrupt},
        {"ids out of order", [&](auto& p) { storeLE<u64>(field(1, 0)(p), loadLE<u64>(field(0, 0)(p))); },
         ErrorCode::Corrupt},
        {"unknown codec", [&](auto& p) { *field(zstd, 56)(p) = 2; }, ErrorCode::Corrupt},
        {"reserved entry byte", [&](auto& p) { *field(raw, 60)(p) = 1; }, ErrorCode::Corrupt},
        {"decoded size above 2 GiB",
         [&](auto& p) { storeLE<u64>(field(zstd, 40)(p), hpak::kMaxAssetSize + 1); }, ErrorCode::Corrupt},
        {"decoded size disagrees with the block count",
         [&](auto& p) { storeLE<u64>(field(zstd, 40)(p), 256 * 1024); }, ErrorCode::Corrupt},
        {"block count", [&](auto& p) { storeLE<u32>(field(zstd, 52)(p), 5); }, ErrorCode::Corrupt},
        {"first block", [&](auto& p) { storeLE<u32>(field(zstd, 48)(p), zstdFirst(p) + 1); },
         ErrorCode::Corrupt},
        {"blob offset misaligned",
         [&](auto& p) { storeLE<u64>(field(raw, 24)(p), loadLE<u64>(field(raw, 24)(p)) + 1); },
         ErrorCode::Corrupt},
        {"blob offset inside the header", [&](auto& p) { storeLE<u64>(field(raw, 24)(p), 0); },
         ErrorCode::Corrupt},
        {"blob offset past the blob region",
         [&](auto& p) { storeLE<u64>(field(empty, 24)(p), tocAt(p) + 4096); }, ErrorCode::Corrupt},
        {"blob runs past the blob region", [&](auto& p) { storeLE<u64>(field(raw, 24)(p), tocAt(p) - 4096); },
         ErrorCode::Corrupt},
        {"stored size disagrees with the block sizes",
         [&](auto& p) { storeLE<u64>(field(zstd, 32)(p), loadLE<u64>(field(zstd, 32)(p)) + 1); },
         ErrorCode::Corrupt},
        {"empty asset with stored bytes", [&](auto& p) { storeLE<u64>(field(empty, 32)(p), 1); },
         ErrorCode::Corrupt},
        {"zstd block of 0 bytes", [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, zstdFirst(p)), 0); },
         ErrorCode::Corrupt},
        {"zstd block above the zstd bound",
         [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, zstdFirst(p)), 0x7FFFFFFFu); },
         ErrorCode::Corrupt},
        {"raw block size differs from the raw size",
         [&](auto& p) { storeLE<u32>(p.data() + blockSizeAt(p, rawFirst(p)), 4999); }, ErrorCode::Corrupt},
    };
    runOpenCases(base, cases);
}

TEST_CASE("hpak hostile: a pak above 2 GiB is refused before anything is read") {
    struct Huge final : IHpakSource {
        u64 size() const override { return hpak::kMaxPakSize + 1; }
        Result<void> readAt(u64, std::span<u8>) const override {
            return Error{ErrorCode::IoError, "not read"};
        }
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
    SUBCASE("a zstd frame that does not state its decoded size") {
        const u32 small = entryIndex(base, guidOf(1)); // zstd, 1 block of 1000 bytes
        std::vector<u8> sized = base, unsized = base;
        replaceFrame(sized, small, rawZstdFrame(compressible(1000), true));
        replaceFrame(unsized, small, rawZstdFrame(compressible(1000), false));
        auto ok = openPak(std::move(sized));
        auto bad = openPak(std::move(unsized));
        REQUIRE(ok.ok());
        REQUIRE(bad.ok());
        // The same raw block decodes to the right bytes either way; only the stated size differs.
        CHECK((*ok)->read(AssetId::fromGuid(guidOf(1))).value() == compressible(1000));
        CHECK((*bad)->read(AssetId::fromGuid(guidOf(1))).errorCode() == ErrorCode::Corrupt);
    }
    SUBCASE("bytes after the zstd frame, even an empty second frame") {
        const u32 small = entryIndex(base, guidOf(1));
        const u64 blob = loadLE<u64>(base.data() + entryAt(base, small) + 24);
        const u64 stored = loadLE<u64>(base.data() + entryAt(base, small) + 32);
        std::vector<u8> frame(base.begin() + static_cast<std::ptrdiff_t>(blob),
                              base.begin() + static_cast<std::ptrdiff_t>(blob + stored));
        const std::vector<u8> empty = {0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x00, 0x01, 0x00, 0x00};
        frame.insert(frame.end(), empty.begin(), empty.end());
        std::vector<u8> p = base;
        replaceFrame(p, small, frame);
        auto pak = openPak(std::move(p));
        REQUIRE(pak.ok());
        CHECK((*pak)->read(AssetId::fromGuid(guidOf(1))).errorCode() == ErrorCode::Corrupt);
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

// An installer reaches paks through mounts (AssetLocation) and hooks (HpakBadBlock::pak), which hand
// out const readers, and must be able to retry blocks through them.
static_assert(requires(const HpakReader& r) { r.retryBlocks(0, 1); });

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
        HELIOS_VERIFY(a->offset == hpak::kHeaderBlockSize &&
                      b->offset == hpak::kHeaderBlockSize + 136 * 1024);
    }
    void flip(const HpakEntry* e) {
        source->set(e->offset + 5, static_cast<u8>(source->get(e->offset + 5) ^ 0xFF));
    }
};

TEST_CASE("hpak integrity: a bad block calls the hook once per block, whatever reads it") {
    Damaged d;
    d.flip(d.a);
    d.flip(d.b);
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    const u64 reads = d.source->reads();
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
    CHECK(d.source->reads() == reads); // a block known Bad is not read again
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
    const u64 reads = d.source->reads();
    CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Busy);
    CHECK(d.source->reads() == reads); // a polled Busy does not read the block again
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

TEST_CASE("hpak integrity: a block repaired by another reader after our range read is re-checked") {
    // The middle asset (130 KiB raw) spans pak blocks 0..2, and both block 0 (inside a) and block 2
    // (inside the middle asset) are bad. Thread t2 reads b and repairs block 2; its hook waits until
    // t1 has read the middle asset's whole range, so t1's copy of block 2 predates the repair. t1 then
    // waits for the repair lock on block 0 and finds block 2 Verified: it must not trust its stale copy.
    Damaged d;
    const HpakEntry* mid = d.pak->find(AssetId::fromGuid(guidOf(9)));
    const u64 bad0 = d.a->offset + 5;
    const u64 bad2 = hpak::kHeaderBlockSize + 130 * 1024; // pak block 2, inside the middle asset
    const u8 good0 = d.source->get(bad0), good2 = d.source->get(bad2);
    d.source->set(bad0, static_cast<u8>(good0 ^ 0xFF));
    d.source->set(bad2, static_cast<u8>(good2 ^ 0xFF));
    d.hook->status = RefetchStatus::Repaired;
    std::atomic<bool> inHook2{false};
    d.hook->onRefetch = [&](const HpakBadBlock& block) {
        if (block.index == 2) {
            const u64 reads = d.source->reads();
            inHook2 = true;
            while (d.source->reads() < reads + 1) std::this_thread::yield(); // t1's range read
            d.source->set(bad2, good2);
        } else {
            d.source->set(bad0, good0);
        }
    };
    Result<std::vector<u8>> gotB(Error{}), gotMid(Error{});
    std::thread t2([&] { gotB = d.pak->read(*d.b); });
    while (!inHook2) std::this_thread::yield();
    std::thread t1([&] { gotMid = d.pak->read(*mid); });
    t1.join();
    t2.join();
    CHECK(gotB.ok());
    REQUIRE(gotMid.ok());
    CHECK(*gotMid == incompressible(130 * 1024, 3));
    CHECK(d.hook->calls == std::map<u32, int>{{0, 1}, {2, 1}});
}

/// Bytes a test can change; the read after arm() stalls after copying until release().
class GatedSource final : public IHpakSource {
public:
    explicit GatedSource(std::vector<u8> b) : m_bytes(std::move(b)) {}
    u64 size() const override { return m_bytes.size(); }
    Result<void> readAt(u64 offset, std::span<u8> out) const override {
        std::unique_lock lock(m_mutex);
        if (offset > m_bytes.size() || out.size() > m_bytes.size() - offset)
            return Error{ErrorCode::EndOfFile};
        std::copy_n(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        if (m_armed) {
            m_armed = false;
            m_blocked = true;
            m_cv.notify_all();
            m_cv.wait(lock, [&] { return !m_blocked; });
        }
        return {};
    }
    std::string describe() const override { return "gated"; }
    void set(u64 o, u8 v) {
        std::lock_guard l(m_mutex);
        m_bytes[o] = v;
    }
    u8 get(u64 o) const {
        std::lock_guard l(m_mutex);
        return m_bytes[o];
    }
    void arm() {
        std::lock_guard l(m_mutex);
        m_armed = true;
    }
    void waitBlocked() {
        std::unique_lock l(m_mutex);
        m_cv.wait(l, [&] { return m_blocked; });
    }
    void release() {
        std::lock_guard l(m_mutex);
        m_blocked = false;
        m_cv.notify_all();
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::vector<u8> m_bytes;
    mutable bool m_armed = false, m_blocked = false;
};

TEST_CASE("hpak integrity: a reader whose copy predates a landed re-fetch does not report the block again") {
    // Reader C copies the bad block and stalls; meanwhile another reader reports it (Pending), the
    // re-fetch lands and the installer calls retryBlocks(), as documented. C's stale copy must not reach
    // the hook a second time: the installer could not tell it from a re-fetch that came back bad, and its
    // answer (Failed here, a retry cap) would leave a good block Bad. A reader that starts while the block
    // is Pending fails Busy without reading it ("a pending re-fetch reads as Busy until retryBlocks").
    const std::vector<TestAsset> assets = {{guidOf(1), incompressible(1000, 1), {0, 0, 0, 0}},
                                           {guidOf(2), incompressible(1000, 2), {0, 0, 0, 1}}};
    auto owned = std::make_unique<GatedSource>(buildPak(assets));
    GatedSource* src = owned.get();
    auto hook = std::make_shared<CountingRefetcher>();
    hook->status = RefetchStatus::Pending;
    HpakOpenOptions options;
    options.refetcher = hook;
    auto pak = HpakReader::open(std::move(owned), options).value();
    const HpakEntry* a = pak->find(AssetId::fromGuid(guidOf(1)));
    const u64 at = a->offset + 5;
    const u8 good = src->get(at);
    src->set(at, static_cast<u8>(good ^ 0xFF));

    src->arm(); // reader C copies the bad block and stalls
    Result<std::vector<u8>> gotC(Error{});
    std::thread c([&] { gotC = pak->read(*a); });
    src->waitBlocked();
    CHECK(pak->read(*a).errorCode() == ErrorCode::Busy); // another reader reports it: a re-fetch is queued
    src->set(at, good);                                  // the re-fetch lands
    pak->retryBlocks(a->offset, 1);                      // and the installer retries
    hook->status = RefetchStatus::Failed;                // a block bad after its re-fetch: give up
    src->release();
    c.join();
    CHECK(hook->calls == std::map<u32, int>{{0, 1}}); // d7761af: {{0, 2}}
    CHECK(gotC.ok());                                 // d7761af: Corrupt
    CHECK(pak->read(*a).ok());                        // d7761af: Corrupt until another retryBlocks()
    CHECK(pak->blockState(0) == HpakBlockState::Verified);
}

TEST_CASE("hpak integrity: a block another reader verifies while the hook runs stays Verified") {
    // The hook answers Pending or Failed, but its re-fetch lands before it returns, and reader B
    // verifies the good bytes meanwhile (B does not need the repair lock). The answer must not
    // overwrite Verified: fetch() trusts a Verified block's bytes unhashed.
    for (const RefetchStatus status : {RefetchStatus::Pending, RefetchStatus::Failed}) {
        INFO("hook answers " << static_cast<int>(status));
        Damaged d;
        const u64 at = d.a->offset + 5;
        const u8 good = d.source->get(at);
        d.flip(d.a);
        d.hook->status = status;
        std::atomic<bool> landed{false}, bDone{false};
        d.hook->onRefetch = [&](const HpakBadBlock&) {
            d.source->set(at, good);
            landed = true;
            const auto t0 = std::chrono::steady_clock::now(); // bounded, so a regression fails, not hangs
            while (!bDone && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
                std::this_thread::yield();
        };
        Result<std::vector<u8>> gotB(Error{}), gotC(Error{});
        std::thread c([&] { gotC = d.pak->read(*d.a); });
        while (!landed) std::this_thread::yield();
        std::thread b([&] {
            gotB = d.pak->read(*d.a);
            bDone = true;
        });
        b.join();
        c.join();
        CHECK(gotB.ok());
        CHECK(bDone.load());
        CHECK(gotC.ok()); // its copy was bad, but the block it reports is Verified now: re-read
        CHECK(d.pak->blockState(0) == HpakBlockState::Verified);
        CHECK(d.pak->read(*d.a).ok());
        CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
    }
}

TEST_CASE("hpak integrity: an I/O error while checking a bad block is returned and leaves it Unverified") {
    Damaged d;
    const u64 at = d.a->offset + 5;
    const u8 good = d.source->get(at);
    d.flip(d.a);
    SUBCASE("the re-read after a Repaired answer") {
        d.hook->status = RefetchStatus::Repaired;
        d.hook->onRefetch = [&](const HpakBadBlock&) {
            d.source->set(at, good);
            d.source->failReadAfter(0);
        };
        CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::IoError); // not "still fails its checksum"
        CHECK(d.pak->blockState(0) == HpakBlockState::Unverified);
        REQUIRE(d.pak->read(*d.a).ok()); // the repaired bytes verify without another hook call
        CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
    }
    SUBCASE("the re-read after a Repaired answer whose repair did not take") {
        // The documented exception to "at most once per block until retryBlocks()": nothing verified
        // the repair, so the block stays Unverified and its next read reports the still-bad bytes.
        d.hook->status = RefetchStatus::Repaired;
        d.hook->onRefetch = [&](const HpakBadBlock&) { d.source->failReadAfter(0); };
        CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::IoError);
        CHECK(d.pak->blockState(0) == HpakBlockState::Unverified);
        d.hook->status = RefetchStatus::Failed;
        d.hook->onRefetch = nullptr;
        CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt);
        CHECK(d.hook->calls == std::map<u32, int>{{0, 2}}); // no retryBlocks() in between
        CHECK(d.pak->blockState(0) == HpakBlockState::Bad);
    }
    SUBCASE("the re-read before the hook") {
        d.source->failReadAfter(1); // the range read succeeds, the locked re-read fails
        CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::IoError);
        CHECK(d.pak->blockState(0) == HpakBlockState::Unverified);
        CHECK(d.hook->calls.empty());
        CHECK(d.pak->read(*d.a).errorCode() == ErrorCode::Corrupt); // the hook (Failed) hears of it now
        CHECK(d.hook->calls == std::map<u32, int>{{0, 1}});
    }
    CHECK(d.pak->blockState(0) != HpakBlockState::Pending);
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
