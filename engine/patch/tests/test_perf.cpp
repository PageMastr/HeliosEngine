// engine/patch budgets (nightly, label perf), stated in engine/patch/README.md "Performance":
//   * FastCDC boundary detection ≥ 1,000 MB/s per core (R07 §7 cites FastCDC at "> 1 GB/s/core");
//   * chunking with a BLAKE2b-256 ID per chunk and for the whole file ≥ 250 MB/s per core, with
//     Monocypher's portable BLAKE2b (the SSE4.1/AVX2 compression functions of 08 §2.1.1 are later work):
//     one core chunks a 50 GB build in under 3.5 minutes, and CL-10's full verify of 50 GB in 3 minutes
//     (≈ 280 MB/s) needs two cores at this rate;
//   * a 50 GB install's manifest (700k chunks, 20k files): read (BLAKE2b, decode, validate) ≤ 400 ms and
//     write (codec none) ≤ 400 ms on one core, well inside CL-1's 2 s launcher start;
//   * deep-paths.hman, the deepest paths the limits allow (64 MiB of paths 508 directories deep, a 72 MB
//     body): read ≤ 400 ms, and within 2x of as many bytes of paths without directories.
// Timings are reported always and asserted only in optimized builds without sanitizers.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <format>

#include "patch_test_util.h"

#define HELIOS_PATCH_SANITIZED 0
#if defined(HELIOS_SANITIZERS_ENABLED) || defined(__SANITIZE_ADDRESS__)
#undef HELIOS_PATCH_SANITIZED
#define HELIOS_PATCH_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) ||                       \
    __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#undef HELIOS_PATCH_SANITIZED
#define HELIOS_PATCH_SANITIZED 1
#endif
#endif
#if defined(NDEBUG) && !HELIOS_PATCH_SANITIZED
#define HELIOS_PATCH_ASSERT_BUDGETS 1
#else
#define HELIOS_PATCH_ASSERT_BUDGETS 0
#endif

using namespace helios;
using namespace helios::patch;
using Clock = std::chrono::steady_clock;

namespace {

f64 secondsSince(Clock::time_point t0) { return std::chrono::duration<f64>(Clock::now() - t0).count(); }

/// Best of `runs` (the least disturbed run on a shared machine).
template <class F>
f64 bestSeconds(int runs, F&& f) {
    f64 best = 1e9;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = Clock::now();
        f();
        best = std::min(best, secondsSince(t0));
    }
    return best;
}

TEST_CASE("perf: FastCDC boundary detection and chunking with BLAKE2b-256 IDs, one core") {
    const std::vector<u8> data = test::randomBytes(1, 64u << 20);
    const f64 mb = static_cast<f64>(data.size()) / 1e6;
    usize chunks = 0;
    const f64 cutS = bestSeconds(5, [&] { chunks = chunkBoundaries(data).size(); });
    const f64 splitS = bestSeconds(3, [&] { chunks = splitBuffer(data).chunks.size(); });
    StreamChunker chunker;
    const f64 streamS = bestSeconds(3, [&] {
        std::vector<Chunk> out;
        for (usize at = 0; at < data.size(); at += 1u << 20)
            chunker.update(std::span<const u8>(data).subspan(at, std::min<usize>(1u << 20, data.size() - at)),
                           out);
        (void)chunker.finish(out);
        chunks = out.size();
    });
    MESSAGE(std::format(
        "boundaries {:.0f} MB/s, split + BLAKE2b {:.0f} MB/s, StreamChunker {:.0f} MB/s ({} chunks)",
        mb / cutS, mb / splitS, mb / streamS, chunks));
#if HELIOS_PATCH_ASSERT_BUDGETS
    CHECK(mb / cutS >= 1000.0);
    CHECK(mb / splitS >= 250.0);
    CHECK(mb / streamS >= 250.0);
#endif
}

TEST_CASE("perf: a 50 GB install's manifest reads and writes within budget") {
    // 20,000 files over 700,000 chunk refs (about 74 KiB per chunk), with synthetic chunk IDs.
    constexpr u32 kFiles = 20'000;
    constexpr u32 kChunksPerFile = 35;
    ManifestHeader h;
    h.productId = "sample-game";
    h.platform = "win64";
    h.buildId = "perf";
    ManifestBuilder b(h);
    SplitMix64 rng(9);
    for (u32 f = 0; f < kFiles; ++f) {
        ManifestFileInput in;
        in.path = std::format("content/group{:03}/file{:05}.hpak", f % 500, f);
        in.tier = static_cast<u8>(f % 3);
        for (u32 c = 0; c < kChunksPerFile; ++c) {
            Chunk ch;
            ch.offset = in.content.size;
            ch.size = 60'000 + static_cast<u32>(rng.next() % 30'000);
            for (usize k = 0; k < 32; k += 8) storeLE<u64>(ch.hash.bytes.data() + k, rng.next());
            in.content.size += ch.size;
            in.content.chunks.push_back(ch);
        }
        REQUIRE(b.addFile(std::move(in)).ok());
    }
    const Manifest m = b.build().value();
    ManifestWriteOptions raw;
    raw.codec = ManifestCodec::None;
    std::vector<u8> bytes;
    const f64 writeS = bestSeconds(3, [&] { bytes = writeManifest(m, raw).value(); });
    Manifest back;
    const f64 readS = bestSeconds(3, [&] { back = readManifest(bytes).value(); });
    CHECK(back == m);
    MESSAGE(std::format("{} files, {} chunks, {:.1f} MB body: write {:.0f} ms, read {:.0f} ms",
                        m.files.size(), m.chunks.size(), static_cast<f64>(bytes.size()) / 1e6, writeS * 1e3,
                        readS * 1e3));
#if HELIOS_PATCH_ASSERT_BUDGETS
    CHECK(writeS <= 0.400);
    CHECK(readS <= 0.400);
#endif
}

// The path-collision check is linear in path bytes plus a sort. Looking up every '/'-prefix of every path in
// a set (len²/4 per path) took 5.1-5.6 s on deep-paths.hman; the ratio to flat paths fails that anywhere.
TEST_CASE("perf: the deepest paths the limits allow read within the 400 ms budget") {
    const auto readBest = [](const std::vector<u8>& file) {
        return bestSeconds(3, [&] { REQUIRE(readManifest(file).ok()); });
    };
    const auto zstd = [](const Manifest& m) {
        ManifestWriteOptions o;
        o.codec = ManifestCodec::Zstd;
        o.zstdLevel = 3;
        return writeManifest(m, o).value();
    };
    const f64 vectorS = readBest(test::readBytes(test::vectorsDir() / "hman" / "deep-paths.hman"));
    const f64 deepS = readBest(zstd(test::pathsManifest(test::kDeepPathFiles, true)));
    const f64 flatS = readBest(zstd(test::pathsManifest(test::kDeepPathFiles, false)));
    MESSAGE(std::format("64 MiB of paths: 508 levels deep {:.0f} ms (deep-paths.hman {:.0f} ms), without "
                        "directories {:.0f} ms",
                        deepS * 1e3, vectorS * 1e3, flatS * 1e3));
#if HELIOS_PATCH_ASSERT_BUDGETS
    CHECK(vectorS <= 0.400);
    CHECK(deepS <= 0.400);
    CHECK(deepS <= 2 * flatS);
#endif
}

} // namespace
