// FastCDC: the vectors shared with Go (services/testdata/vectors/fastcdc.json), streaming equivalence,
// size bounds, resynchronization and file chunking.
#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <set>

#include "patch_test_util.h"

using namespace helios;
using namespace helios::patch;

namespace {

std::string hex64(u64 v) { return std::format("{:016x}", v); }

std::string gearDigest() {
    std::vector<u8> b(256 * 8);
    for (usize i = 0; i < 256; ++i) storeLE<u64>(b.data() + i * 8, fastcdc::gearTable()[i]);
    return blake2b256(b).toHex();
}

/// Feeds `data` to a StreamChunker in pieces of varying size (1 byte up to ~200 KB).
ChunkedFile streamChunks(std::span<const u8> data, u64 seed) {
    StreamChunker chunker;
    std::vector<Chunk> chunks;
    SplitMix64 rng(seed);
    while (!data.empty()) {
        usize n = static_cast<usize>(rng.next() % 200'000) + 1;
        if (rng.next() % 4 == 0) n = static_cast<usize>(rng.next() % 7) + 1;
        n = std::min(n, data.size());
        chunker.update(data.first(n), chunks);
        data = data.subspan(n);
    }
    ChunkedFile file = chunker.finish(chunks);
    file.chunks = std::move(chunks);
    return file;
}

TEST_CASE("fastcdc: the shared vectors (Go pkg/cdc reproduces the same file)") {
    const test::Json j = test::loadJson(test::vectorsDir() / "fastcdc.json");
    yyjson_val* root = j.root();
    REQUIRE(test::getU64(root, "minSize") == fastcdc::kMinSize);
    REQUIRE(test::getU64(root, "avgSize") == fastcdc::kAvgSize);
    REQUIRE(test::getU64(root, "maxSize") == fastcdc::kMaxSize);
    REQUIRE(test::getStr(root, "maskS") == hex64(fastcdc::kMaskS));
    REQUIRE(test::getStr(root, "maskL") == hex64(fastcdc::kMaskL));
    REQUIRE(test::getStr(root, "gearSeed") == hex64(fastcdc::kGearSeed));
    REQUIRE(test::getStr(root, "gearDigest") == gearDigest());
    yyjson_val* gear = test::get(root, "gear");
    REQUIRE(yyjson_arr_size(gear) == 256);
    for (usize i = 0; i < 256; ++i)
        REQUIRE(std::string(yyjson_get_str(yyjson_arr_get(gear, i))) == hex64(fastcdc::gearTable()[i]));

    yyjson_val* cases = test::get(root, "cases");
    REQUIRE(yyjson_arr_size(cases) >= 15);
    usize idx, max;
    yyjson_val* c;
    yyjson_arr_foreach(cases, idx, max, c) {
        const std::string name = test::getStr(c, "name");
        CAPTURE(name);
        const std::vector<u8> data = test::generateInput(test::get(c, "input"));
        REQUIRE(data.size() == test::getU64(c, "size"));
        REQUIRE(blake2b256(data).toHex() == test::getStr(c, "fileHash"));
        const ChunkedFile file = splitBuffer(data);
        yyjson_val* chunks = test::get(c, "chunks");
        REQUIRE(file.chunks.size() == yyjson_arr_size(chunks));
        for (usize k = 0; k < file.chunks.size(); ++k) {
            yyjson_val* want = yyjson_arr_get(chunks, k);
            CAPTURE(k);
            REQUIRE(file.chunks[k].offset == test::getU64(want, "offset"));
            REQUIRE(file.chunks[k].size == test::getU64(want, "size"));
            REQUIRE(file.chunks[k].hash.toHex() == test::getStr(want, "hash"));
        }
        CHECK(file.size == data.size());
        CHECK(file.hash == blake2b256(data));
        const std::vector<u32> lengths = chunkBoundaries(data);
        REQUIRE(lengths.size() == file.chunks.size());
        for (usize k = 0; k < lengths.size(); ++k) CHECK(lengths[k] == file.chunks[k].size);
        CHECK(streamChunks(data, data.size()) == file);
    }
}

TEST_CASE("fastcdc: chunks tile the input within the size bounds") {
    for (u64 seed = 100; seed < 106; ++seed) {
        const std::vector<u8> data = test::randomBytes(seed, (3u << 20) + static_cast<usize>(seed) * 7919);
        const std::vector<u32> lengths = chunkBoundaries(data);
        u64 total = 0;
        for (usize i = 0; i < lengths.size(); ++i) {
            CHECK(lengths[i] >= 1);
            CHECK(lengths[i] <= fastcdc::kMaxSize);
            if (i + 1 < lengths.size()) CHECK(lengths[i] > fastcdc::kMinSize);
            total += lengths[i];
        }
        CHECK(total == data.size());
    }
    CHECK(fastcdc::cut({}) == 0);
    CHECK(chunkBoundaries({}).empty());
    const ChunkedFile empty = splitBuffer({});
    CHECK(empty.size == 0);
    CHECK(empty.chunks.empty());
    CHECK(empty.hash == blake2b256({}));
}

TEST_CASE("fastcdc: the mean chunk on random data lands near the normal size") {
    const std::vector<u8> data = test::randomBytes(42, 64u << 20);
    const std::vector<u32> lengths = chunkBoundaries(data);
    const double mean = static_cast<double>(data.size()) / static_cast<double>(lengths.size());
    MESSAGE("mean chunk " << mean << " bytes over " << lengths.size() << " chunks");
    CHECK(mean >= 48 * 1024);
    CHECK(mean <= 96 * 1024);
}

TEST_CASE("fastcdc: an insertion changes only the chunks around it") {
    const std::vector<u8> base = test::randomBytes(7, 16u << 20);
    std::vector<u8> edited = base;
    const std::vector<u8> ins = test::randomBytes(8, 100);
    edited.insert(edited.begin() + (5 << 20), ins.begin(), ins.end());
    std::set<Hash256> have;
    for (const Chunk& c : splitBuffer(base).chunks) have.insert(c.hash);
    usize changed = 0;
    for (const Chunk& c : splitBuffer(edited).chunks) changed += have.count(c.hash) == 0;
    CHECK(changed >= 1);
    CHECK(changed <= 3);
}

TEST_CASE("fastcdc: a StreamChunker restarts after finish() and agrees on edge sizes") {
    StreamChunker chunker;
    for (const usize size : {usize(0), usize(1), usize(fastcdc::kMinSize), usize(fastcdc::kMaxSize),
                             usize(fastcdc::kMaxSize) + 1, usize(2) * fastcdc::kMaxSize, usize(5'000'000)}) {
        CAPTURE(size);
        const std::vector<u8> data = test::randomBytes(size + 1, size);
        std::vector<Chunk> chunks;
        chunker.update(data, chunks);
        CHECK(chunker.bytesIn() == size);
        ChunkedFile file = chunker.finish(chunks);
        file.chunks = std::move(chunks);
        CHECK(file == splitBuffer(data));
        CHECK(chunker.bytesIn() == 0);
    }
}

TEST_CASE("fastcdc: chunkFile reads through the platform layer") {
    const Result<fs::Path> dir = fs::createUniqueTempDirectory("patch-tests");
    REQUIRE(dir.ok());
    const std::vector<u8> data = test::randomBytes(77, 3'500'001); // not a multiple of the 1 MiB read size
    const fs::Path path = *dir / "content.bin";
    REQUIRE(fs::writeFile(path, data).ok());
    const Result<ChunkedFile> file = chunkFile(path);
    REQUIRE(file.ok());
    CHECK(*file == splitBuffer(data));
    const fs::Path empty = *dir / "empty.bin";
    REQUIRE(fs::writeFile(empty, std::span<const u8>()).ok());
    const Result<ChunkedFile> none = chunkFile(empty);
    REQUIRE(none.ok());
    CHECK(none->chunks.empty());
    CHECK(none->hash == blake2b256({}));
    CHECK(chunkFile(*dir / "missing.bin").errorCode() == ErrorCode::NotFound);
    (void)fs::removeAll(*dir);
}

} // namespace
