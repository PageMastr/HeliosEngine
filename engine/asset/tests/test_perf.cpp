// `.hpak` v0 budgets (nightly, label perf). The plan sets none for these paths, so the budgets are
// this module's own, stated in engine/asset/README.md, except where noted:
//   * open (header + TOC validation) of a 100k-asset pak ≤ 60 ms; mounting it in a PakMountTable
//     ≤ 40 ms; a lookup through the table ≤ 250 ns mean (random ids, 100k-asset table);
//   * first read (XXH3-64 block verification + zstd decode + XXH3-128 cooked-hash check) ≥ 300 MB/s
//     of decoded bytes on one thread, i.e. ≤ 0.85 ms per 256 KiB block: twice 02 §5.7's sustained I/O
//     budget of 150 MB/s, so one decode thread keeps up with the disk; a later (verified) read ≥ 400 MB/s.
// Timings are reported always and asserted only in optimized builds without sanitizers.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "hpak_test_util.h"
#include "helios/asset/mount_table.h"

#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
#define HELIOS_ASSET_ASSERT_BUDGETS 1
#else
#define HELIOS_ASSET_ASSERT_BUDGETS 0
#endif

namespace {

using namespace helios;
using namespace helios::asset;
using namespace helios::asset::test;
using Clock = std::chrono::steady_clock;

f64 msSince(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }

f64 median(std::vector<f64> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

TEST_CASE("perf: open and mount a 100k-asset pak, then look ids up through the mount table") {
    constexpr u64 kAssets = 100'000;
    auto writer = assetpipe::HpakWriter::create();
    REQUIRE(writer.ok());
    const std::vector<u8> tiny = {1, 2, 3};
    for (u64 i = 1; i <= kAssets; ++i) REQUIRE(writer->add(guidOf(i), i % 10 == 0 ? std::span<const u8>(tiny) : std::span<const u8>()).ok());
    const std::vector<u8> bytes = writer->build().value();

    std::vector<f64> openMs, mountMs;
    std::shared_ptr<HpakReader> pak;
    for (int run = 0; run < 5; ++run) {
        auto source = makeHpakMemorySource(bytes);
        const auto t0 = Clock::now();
        auto opened = HpakReader::open(std::move(source));
        openMs.push_back(msSince(t0));
        REQUIRE(opened.ok());
        pak = *opened;
        PakMountTable table;
        const auto t1 = Clock::now();
        REQUIRE(table.mount(pak).ok());
        mountMs.push_back(msSince(t1));
    }
    PakMountTable table;
    REQUIRE(table.mount(pak).ok());
    std::vector<AssetId> ids;
    ids.reserve(kAssets);
    Xoshiro256 rng(3);
    for (u64 i = 0; i < kAssets; ++i) ids.push_back(AssetId::fromGuid(guidOf(1 + rng.next() % kAssets)));
    constexpr int kRounds = 10;
    u64 found = 0;
    const auto t2 = Clock::now();
    for (int r = 0; r < kRounds; ++r)
        for (const AssetId id : ids) found += table.find(id).has_value() ? 1 : 0;
    const f64 lookupNs = msSince(t2) * 1e6 / f64(kRounds * kAssets);
    CHECK(found == kRounds * kAssets);

    const f64 open = median(openMs), mount = median(mountMs);
    MESSAGE("hpak open (100k assets, TOC " << bytes.size() / 1024 << " KiB): " << open << " ms; table mount: " << mount
                                            << " ms; lookup: " << lookupNs << " ns");
#if HELIOS_ASSET_ASSERT_BUDGETS
    CHECK(open <= 60.0);
    CHECK(mount <= 40.0);
    CHECK(lookupNs <= 250.0);
#endif
}

TEST_CASE("perf: first read verifies and decodes at >= 300 MB/s, later reads at >= 400 MB/s") {
    // 32 MiB of mixed content: half text-like, half low-entropy binary, in assets of 64 KiB..2 MiB.
    std::vector<TestAsset> assets;
    u64 total = 0;
    Xoshiro256 rng(9);
    for (u64 i = 1; total < 32 * kMiB; ++i) {
        const usize size = static_cast<usize>(64 * kKiB + rng.next() % (2 * kMiB));
        std::vector<u8> bytes = i % 2 ? compressible(size, static_cast<u32>(i)) : incompressible(size, i);
        if (i % 2 == 0)
            for (usize k = 0; k < bytes.size(); ++k) bytes[k] = static_cast<u8>(bytes[k] & 0x0F); // ~4 bits/byte
        total += size;
        assets.push_back({guidOf(i), std::move(bytes), {}});
    }
    const std::vector<u8> bytes = buildPak(assets);
    auto pak = openPak(bytes);
    REQUIRE(pak.ok());
    const HpakReader& r = **pak;

    std::vector<u8> out;
    const auto readAll = [&] {
        for (const HpakEntry& e : r.entries()) {
            out.resize(static_cast<usize>(e.rawSize));
            REQUIRE(r.readInto(e, out).ok());
        }
    };
    const auto t0 = Clock::now();
    readAll();
    const f64 firstMs = msSince(t0);
    std::vector<f64> laterMs;
    for (int run = 0; run < 3; ++run) {
        const auto t1 = Clock::now();
        readAll();
        laterMs.push_back(msSince(t1));
    }
    const f64 mb = f64(total) / 1e6;
    const f64 firstMBs = mb / (firstMs / 1e3), laterMBs = mb / (median(laterMs) / 1e3);
    MESSAGE("hpak read " << mb << " MB (pak " << bytes.size() / 1e6 << " MB): first " << firstMBs << " MB/s, later "
                         << laterMBs << " MB/s");
#if HELIOS_ASSET_ASSERT_BUDGETS
    CHECK(firstMBs >= 300.0);
    CHECK(laterMBs >= 400.0);
#endif
}

} // namespace
