// DDC v0 budgets (nightly, label perf). The plan's budgets are per build step and end to end (02 §6.4: a
// hot-reload build step is ≤ 100 ms for Luau up to ≤ 1,000 ms for shaders; 07 §4.1: cold open with a warm
// DDC ≤ 10 s, AAA-ITR-2); it sets none for the cache itself. These are this module's own, stated in
// engine/assetpipe/README.md, chosen so that a DDC hit costs ≤ 1 % of the smallest build-step budget:
//   * key compute (makeDdcKey, canonical settings and 4 dependency keys) ≤ 1 µs mean;
//   * source hashing (hashSourceFile, page-cached) ≥ 1 GB/s;
//   * a hit (LocalDdc::get: open, header, read, XXH3-128 check) of a 256 KiB entry ≤ 1 ms p95, and a put
//     of one (temp file, write, rename; no flush) ≤ 5 ms p95;
//   * a whole cook that hits (sidecar load and validation, source read and hash, key, get) for a 64 KiB
//     source ≤ 1 ms p95.
// Timings are reported always and asserted only in optimized builds without sanitizers.

#include <algorithm>
#include <chrono>
#include <vector>

#include "assetpipe_test_util.h"
#include "helios/core/log.h"
#include "helios/core/random.h"

// Sanitizers can also come from raw compiler flags (the fuzz build), which HELIOS_SANITIZERS_ENABLED
// does not see: GCC defines __SANITIZE_ADDRESS__, Clang answers __has_feature.
#define HELIOS_ASSETPIPE_SANITIZED 0
#if defined(HELIOS_SANITIZERS_ENABLED) || defined(__SANITIZE_ADDRESS__)
#undef HELIOS_ASSETPIPE_SANITIZED
#define HELIOS_ASSETPIPE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) ||                       \
    __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#undef HELIOS_ASSETPIPE_SANITIZED
#define HELIOS_ASSETPIPE_SANITIZED 1
#endif
#endif
#if defined(NDEBUG) && !HELIOS_ASSETPIPE_SANITIZED
#define HELIOS_ASSETPIPE_ASSERT_BUDGETS 1
#else
#define HELIOS_ASSETPIPE_ASSERT_BUDGETS 0
#endif

namespace {

using namespace assetpipe_test;
using Clock = std::chrono::steady_clock;

f64 usSince(Clock::time_point t0) {
    return std::chrono::duration<f64, std::micro>(Clock::now() - t0).count();
}

f64 percentile(std::vector<f64> v, f64 p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<usize>(p * static_cast<f64>(v.size())))];
}

std::vector<u8> noise(usize size, u64 seed) {
    std::vector<u8> out(size);
    Xoshiro256 rng(seed);
    for (u8& b : out) b = static_cast<u8>(rng.next() >> 56);
    return out;
}

TEST_CASE("perf: DDC key compute") {
    const Hash128 deps[] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    DdcKeyInputs in;
    in.builder = "png";
    in.builderVersion = 2;
    in.sourceHash = {0x1234, 0x5678};
    in.settings = R"({"mips":false,"maxSize":1024,"format":"bc5","lods":[0,1,2]})";
    in.settingsType = Hash128{42, 43};
    in.dependencies = deps;
    constexpr int kIters = 1'000'000;
    u64 sink = 0;
    const auto t0 = Clock::now();
    for (int i = 0; i < kIters; ++i) {
        in.sourceHash.low = static_cast<u64>(i);
        sink ^= makeDdcKey(in).low;
    }
    const f64 meanUs = usSince(t0) / kIters;
    MESSAGE("makeDdcKey: " << meanUs * 1000.0 << " ns mean (budget 1 us) [" << (sink & 1) << "]");
#if HELIOS_ASSETPIPE_ASSERT_BUDGETS
    CHECK(meanUs <= 1.0);
#endif
}

TEST_CASE("perf: source hashing throughput") {
    TempDir dir;
    const std::vector<u8> data = noise(64 * kMiB, 1);
    REQUIRE(fs::writeFile(dir.path / "big.bin", data));
    (void)hashSourceFile(dir.path / "big.bin"); // warm the page cache
    f64 best = 0;
    for (int run = 0; run < 3; ++run) {
        const auto t0 = Clock::now();
        REQUIRE(hashSourceFile(dir.path / "big.bin").value() == hash128(data.data(), data.size()));
        const f64 secs = usSince(t0) / 1e6;
        best = std::max(best, static_cast<f64>(data.size()) / secs / 1e9);
    }
    MESSAGE("hashSourceFile: " << best << " GB/s best of 3 (budget >= 1 GB/s)");
#if HELIOS_ASSETPIPE_ASSERT_BUDGETS
    CHECK(best >= 1.0);
#endif
}

TEST_CASE("perf: DDC hit latency (256 KiB entries) and put") {
    TempDir dir;
    LocalDdcOptions o;
    o.root = dir.path;
    auto ddc = LocalDdc::open(o).value();
    constexpr int kEntries = 64;
    std::vector<f64> puts;
    for (int i = 0; i < kEntries; ++i) {
        const std::vector<u8> payload = noise(256 * kKiB, static_cast<u64>(i));
        const auto t0 = Clock::now();
        REQUIRE(ddc->put(Hash128{static_cast<u64>(i) + 1, 7}, payload));
        puts.push_back(usSince(t0));
    }
    std::vector<f64> gets;
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < kEntries; ++i) {
            const auto t0 = Clock::now();
            const auto got = ddc->get(Hash128{static_cast<u64>(i) + 1, 7});
            gets.push_back(usSince(t0));
            REQUIRE(got);
            REQUIRE(got->size() == 256 * kKiB);
        }
    }
    const f64 p50 = percentile(gets, 0.5), p95 = percentile(gets, 0.95);
    MESSAGE("LocalDdc::get 256 KiB: p50 " << p50 << " us, p95 " << p95 << " us (budget p95 1000 us); put p50 "
                                          << percentile(puts, 0.5) << " us, p95 " << percentile(puts, 0.95)
                                          << " us (budget p95 5000 us)");
#if HELIOS_ASSETPIPE_ASSERT_BUDGETS
    CHECK(p95 <= 1000.0);
    CHECK(percentile(puts, 0.95) <= 5000.0);
#endif
}

TEST_CASE("perf: a cook that hits the DDC (64 KiB source)") {
    TempDir dir;
    const ImporterRegistry r = makeRegistry();
    LocalDdcOptions o;
    o.root = dir.path / "ddc";
    auto ddc = LocalDdc::open(o).value();
    constexpr int kAssets = 32;
    for (int i = 0; i < kAssets; ++i) {
        const std::string path = "a/asset" + std::to_string(i) + ".png";
        const std::vector<u8> bytes = noise(64 * kKiB, static_cast<u64>(i));
        REQUIRE(fs::createDirectories(dir.path / "a"));
        REQUIRE(fs::writeFile(dir.path / fs::pathFromUtf8(path), bytes));
        REQUIRE(ensureMeta(dir.path, path, newMeta(R"({"mips": false, "lods": [0, 1]})"), r));
        REQUIRE(cookAsset(CookRequest{&r, ddc.get(), dir.path, path}).value().stored);
    }
    std::vector<f64> cooks;
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < kAssets; ++i) {
            const std::string path = "a/asset" + std::to_string(i) + ".png";
            const auto t0 = Clock::now();
            const auto cooked = cookAsset(CookRequest{&r, ddc.get(), dir.path, path});
            cooks.push_back(usSince(t0));
            REQUIRE(cooked);
            REQUIRE(cooked->hit);
        }
    }
    const f64 p50 = percentile(cooks, 0.5), p95 = percentile(cooks, 0.95);
    MESSAGE("cookAsset hit, 64 KiB source: p50 " << p50 << " us, p95 " << p95 << " us (budget p95 1000 us)");
#if HELIOS_ASSETPIPE_ASSERT_BUDGETS
    CHECK(p95 <= 1000.0);
#endif
}

} // namespace
