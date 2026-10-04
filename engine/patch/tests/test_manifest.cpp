// `.hman` v0: the pipeline vector shared with Go (byte-identical writers, cross-language reads), the
// shared hostile cases and names, truncation and bit-flip sweeps, zstd bounds and the builder.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>

#include "patch_test_util.h"

using namespace helios;
using namespace helios::patch;

namespace {

fs::Path hmanDir() { return test::vectorsDir() / "hman"; }

/// The shared pipeline description's builder, with files added in `order` (description order if empty).
ManifestBuilder pipelineBuilder(const std::vector<usize>& order = {}) {
    const test::Json j = test::loadJson(hmanDir() / "pipeline.json");
    yyjson_val* root = j.root();
    yyjson_val* hj = test::get(root, "header");
    ManifestHeader h;
    h.productId = test::getStr(hj, "productId");
    h.platform = test::getStr(hj, "platform");
    h.buildId = test::getStr(hj, "buildId");
    h.sequence = test::getU64(hj, "sequence");
    h.createdAt = test::getU64(hj, "createdAt");
    h.expiresAt = test::getU64(hj, "expiresAt");
    h.compatEpoch = static_cast<u32>(test::getU64(hj, "compatEpoch"));
    const std::vector<u8> key = test::fromHex(test::getStr(hj, "keyId"));
    const std::vector<u8> sig = test::fromHex(test::getStr(hj, "signature"));
    REQUIRE(key.size() == h.keyId.size());
    REQUIRE(sig.size() == h.signature.size());
    std::copy(key.begin(), key.end(), h.keyId.begin());
    std::copy(sig.begin(), sig.end(), h.signature.begin());
    ManifestBuilder b(h);

    yyjson_val* files = test::get(root, "files");
    std::vector<usize> indices = order;
    if (indices.empty())
        for (usize i = 0; i < yyjson_arr_size(files); ++i) indices.push_back(i);
    std::map<std::string, std::vector<Chunk>> chunks;
    for (const usize i : indices) {
        yyjson_val* f = yyjson_arr_get(files, i);
        ManifestFileInput in;
        in.path = test::getStr(f, "path");
        const std::vector<u8> data = test::generateInput(test::get(f, "input"));
        StreamChunker chunker;
        std::vector<Chunk> list;
        chunker.update(data, list);
        in.content = chunker.finish(list);
        in.content.chunks = list;
        chunks[in.path] = std::move(list);
        in.tier = static_cast<u8>(test::getU64(f, "tier"));
        in.flags = static_cast<ManifestFileFlags>(test::getU64(f, "flags"));
        in.group = test::getU64(f, "group");
        in.language = static_cast<u32>(test::getU64(f, "language"));
        REQUIRE(b.addFile(std::move(in)).ok());
    }
    usize idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(test::get(root, "packs"), idx, max, v)
        b.addPack(test::getHash(v, "hash"), test::getU64(v, "size"));
    yyjson_arr_foreach(test::get(root, "placements"), idx, max, v) {
        const Chunk& c = chunks.at(test::getStr(v, "file")).at(static_cast<usize>(test::getU64(v, "chunk")));
        b.placeChunk(c.hash, test::getHash(v, "pack"), test::getU64(v, "offset"),
                     static_cast<u32>(test::getU64(v, "storedSize")));
    }
    yyjson_arr_foreach(test::get(root, "storedSizes"), idx, max, v) {
        const Chunk& c = chunks.at(test::getStr(v, "file")).at(static_cast<usize>(test::getU64(v, "chunk")));
        b.setStoredSize(c.hash, static_cast<u32>(test::getU64(v, "storedSize")));
    }
    yyjson_arr_foreach(test::get(root, "patches"), idx, max, v) {
        b.addPatch(test::getStr(v, "path"), test::getHash(v, "fromHash"), test::getHash(v, "patchHash"),
                   test::getU64(v, "patchSize"));
    }
    return b;
}

Manifest pipeline() {
    Result<Manifest> m = pipelineBuilder().build();
    REQUIRE_MESSAGE(m.ok(), m.error().toString());
    return std::move(*m);
}

/// A manifest of zero-size files with these paths, in this order.
Manifest zeroSizeFiles(const ManifestHeader& h, const std::vector<std::string>& paths) {
    Manifest m;
    m.header = h;
    for (const std::string& p : paths) {
        ManifestFile f;
        f.path = p;
        m.files.push_back(std::move(f));
    }
    return m;
}

ManifestWriteOptions rawOptions() {
    ManifestWriteOptions o;
    o.codec = ManifestCodec::None;
    return o;
}

/// Applies a shared hostile case (services/testdata/vectors/hman/hostile.json) to the golden.
std::vector<u8> applyHostile(std::vector<u8> b, yyjson_val* c) {
    if (const u64 size = test::getU64(c, "size"); size != 0) b.resize(static_cast<usize>(size), 0);
    usize idx, max;
    yyjson_val* e;
    yyjson_arr_foreach(test::get(c, "edits"), idx, max, e) {
        const std::vector<u8> bytes = test::fromHex(test::getStr(e, "hex"));
        const usize at = static_cast<usize>(test::getU64(e, "at"));
        REQUIRE(at + bytes.size() <= b.size());
        std::copy(bytes.begin(), bytes.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
    }
    const std::string reseal = test::getStr(c, "reseal");
    if (reseal == "all") {
        const u64 payload = b.size() - hman::kHeaderSize;
        storeLE<u64>(b.data() + 48, payload);
        storeLE<u64>(b.data() + 56, payload);
        const Hash256 h = blake2b256(std::span<const u8>(b).subspan(hman::kHeaderSize));
        std::copy(h.bytes.begin(), h.bytes.end(), b.begin() + 64);
    }
    if (reseal == "all" || reseal == "header") {
        const Hash256 h = blake2b256(std::span<const u8>(b).first(hman::kSignedBytes));
        std::copy(h.bytes.begin(), h.bytes.end(), b.begin() + hman::kHeaderHashOffset);
    }
    return b;
}

TEST_CASE("manifest: the C++ writer reproduces the shared pipeline.hman byte for byte") {
    const Manifest m = pipeline();
    const Result<std::vector<u8>> raw = writeManifest(m, rawOptions());
    REQUIRE(raw.ok());
    const std::vector<u8> golden = test::readBytes(hmanDir() / "pipeline.hman");
    if (*raw != golden) {
        usize at = 0;
        while (at < raw->size() && at < golden.size() && (*raw)[at] == golden[at]) ++at;
        FAIL("the C++ writer differs from pipeline.hman at byte " << at << " (" << raw->size() << " vs "
                                                                  << golden.size() << " bytes)");
    }
    // Files added in another order build the same manifest.
    const Result<Manifest> reversed = pipelineBuilder({7, 6, 5, 4, 3, 2, 1, 0}).build();
    REQUIRE(reversed.ok());
    CHECK(*reversed == m);
    CHECK(m.files.size() == 8);
    CHECK(m.packs.size() == 1);
    CHECK(m.patches.size() == 2);
    CHECK(m.chunks.size() < m.refs.size()); // the zone pak shares most chunks with the common pak

    // The C++-written zstd vector (HELIOS_PATCH_UPDATE_VECTORS=1 rewrites it).
    if (const char* updateEnv = std::getenv("HELIOS_PATCH_UPDATE_VECTORS"); updateEnv && *updateEnv == '1') {
        ManifestWriteOptions zstd19;
        zstd19.codec = ManifestCodec::Zstd; // level 0: the default, 19
        const Result<std::vector<u8>> z = writeManifest(m, zstd19);
        REQUIRE(z.ok());
        REQUIRE(fs::writeFile(hmanDir() / "pipeline.cpp-zstd.hman", *z).ok());
        MESSAGE("rewrote pipeline.cpp-zstd.hman");
    }
}

TEST_CASE("manifest: every shared golden reads back to the description, whichever language wrote it") {
    const Manifest want = pipeline();
    for (const char* name : {"pipeline.hman", "pipeline.go-zstd.hman", "pipeline.cpp-zstd.hman"}) {
        CAPTURE(name);
        const std::vector<u8> file = test::readBytes(hmanDir() / name);
        const Result<Manifest> got = readManifest(file);
        REQUIRE_MESSAGE(got.ok(), got.error().toString());
        CHECK(*got == want);
        const Result<ManifestHeaderInfo> info = readManifestHeader(file);
        REQUIRE(info.ok());
        CHECK(info->header == want.header);
        CHECK(info->codec ==
              (std::string_view(name) == "pipeline.hman" ? ManifestCodec::None : ManifestCodec::Zstd));
        CHECK(info->headerHash == hman::computeHeaderHash(std::span<const u8, hman::kHeaderSize>(
                                      file.data(), hman::kHeaderSize)));
        const Result<Manifest> viaFile = readManifestFile(hmanDir() / name);
        REQUIRE(viaFile.ok());
        CHECK(*viaFile == want);
    }
}

TEST_CASE("manifest: the shared deep-paths.hman (written by Go) reads back to the deepest-paths manifest") {
    // 64 MiB of paths 508 directories deep: the worst case for the path-collision check within every limit.
    // test_perf.cpp times the read against the 400 ms budget.
    const Result<Manifest> m = readManifestFile(hmanDir() / "deep-paths.hman");
    REQUIRE_MESSAGE(m.ok(), m.error().toString());
    CHECK(*m == test::pathsManifest(test::kDeepPathFiles, true));
}

TEST_CASE("manifest: the shared hostile cases fail the same check with the same error kind as in Go") {
    const test::Json j = test::loadJson(hmanDir() / "hostile.json");
    const std::vector<u8> golden = test::readBytes(hmanDir() / "pipeline.hman");
    yyjson_val* cases = test::get(j.root(), "cases");
    REQUIRE(yyjson_arr_size(cases) >= 50);
    usize idx, max;
    yyjson_val* c;
    yyjson_arr_foreach(cases, idx, max, c) {
        const std::string name = test::getStr(c, "name");
        const std::string rule = test::getStr(c, "rule"); // a message substring: the one check it breaks
        CAPTURE(name);
        CAPTURE(rule);
        const Result<Manifest> m = readManifest(applyHostile(golden, c));
        REQUIRE(!m.ok());
        CHECK(test::errorKind(m.errorCode()) == test::getStr(c, "expect"));
        CHECK_MESSAGE((!rule.empty() && m.error().message.find(rule) != std::string::npos), m.error().message);
    }
}

TEST_CASE("manifest: the shared identifier and path vectors") {
    const test::Json j = test::loadJson(hmanDir() / "names.json");
    yyjson_val* root = j.root();
    const auto list = [](yyjson_val* group, const char* key) {
        std::vector<std::string> out;
        usize idx, max;
        yyjson_val* s;
        yyjson_arr_foreach(test::get(group, key), idx, max, s)
            out.emplace_back(yyjson_get_str(s), yyjson_get_len(s));
        REQUIRE(!out.empty());
        return out;
    };
    yyjson_val* paths = test::get(root, "paths");
    for (const std::string& p : list(paths, "valid")) CHECK_MESSAGE(isValidManifestPath(p), p);
    for (const std::string& p : list(paths, "invalid")) CHECK_MESSAGE(!isValidManifestPath(p), p);
    CHECK(isValidManifestPath(std::string(hman::kMaxPathBytes, 'a')));
    CHECK_FALSE(isValidManifestPath(std::string(hman::kMaxPathBytes + 1, 'a')));

    const ManifestHeader base = pipeline().header;
    const auto check = [&](const char* key, std::string ManifestHeader::*field) {
        yyjson_val* group = test::get(root, key);
        for (const bool valid : {true, false}) {
            for (const std::string& s : list(group, valid ? "valid" : "invalid")) {
                Manifest m;
                m.header = base;
                m.header.*field = s;
                CHECK_MESSAGE(validateManifest(m).ok() == valid, key << " '" << s << "'");
            }
        }
    };
    check("productIds", &ManifestHeader::productId);
    check("platforms", &ManifestHeader::platform);
    check("buildIds", &ManifestHeader::buildId);

    // Whole path lists: no two equal ignoring case, no file that is also a directory of another.
    yyjson_val* sets = test::get(root, "pathSets");
    const auto filesOf = [&](yyjson_val* arr) {
        std::vector<std::string> paths;
        usize idx, max;
        yyjson_val* p;
        yyjson_arr_foreach(arr, idx, max, p) paths.emplace_back(yyjson_get_str(p), yyjson_get_len(p));
        return zeroSizeFiles(base, paths);
    };
    REQUIRE(yyjson_arr_size(test::get(sets, "valid")) > 0);
    REQUIRE(yyjson_arr_size(test::get(sets, "invalid")) > 0);
    usize idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(test::get(sets, "valid"), idx, max, v) {
        const Result<void> ok = validateManifest(filesOf(v));
        CHECK_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
    }
    yyjson_arr_foreach(test::get(sets, "invalid"), idx, max, v) {
        const std::string rule = test::getStr(v, "rule");
        const Result<void> ok = validateManifest(filesOf(test::get(v, "paths")));
        REQUIRE_MESSAGE(!ok.ok(), rule);
        CHECK_MESSAGE(ok.error().message.find(rule) != std::string::npos, ok.error().message);
    }
}

TEST_CASE("manifest: the sorted-key collision check agrees with the obvious one on random path sets") {
    // As Go's TestPathCollisionsMatchNaive: paths over "aAb-./", so case, '-' and '.' (which sort before
    // '/') and nesting all meet; the reference looks up every '/'-prefix of every lowered path.
    constexpr std::string_view kAlphabet = "aAb-./";
    SplitMix64 rng(0x0C011151);
    const ManifestHeader h = pipeline().header;
    usize outcomes[2] = {};
    for (int iter = 0; iter < 20'000; ++iter) {
        std::vector<std::string> paths;
        for (usize n = 2 + rng.next() % 5; paths.size() < n;) {
            std::string p(static_cast<usize>(1 + rng.next() % 6), ' ');
            for (char& c : p) c = kAlphabet[rng.next() % kAlphabet.size()];
            if (isValidManifestPath(p) && std::find(paths.begin(), paths.end(), p) == paths.end())
                paths.push_back(std::move(p));
        }
        std::sort(paths.begin(), paths.end());
        std::set<std::string> lowered;
        bool want = true;
        for (const std::string& p : paths) {
            std::string low = p;
            for (char& c : low) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            want = lowered.insert(low).second && want;
        }
        for (const std::string& low : lowered)
            for (usize i = 0; i < low.size(); ++i)
                if (low[i] == '/' && lowered.count(low.substr(0, i))) want = false;
        const Result<void> got = validateManifest(zeroSizeFiles(h, paths));
        REQUIRE_MESSAGE(got.ok() == want, (got.ok() ? std::string("valid") : got.error().message));
        ++outcomes[want ? 1 : 0];
    }
    CHECK(outcomes[0] >= 1000);
    CHECK(outcomes[1] >= 1000);
}

TEST_CASE("manifest: every truncation and every byte flip fails, except in the uninterpreted signature") {
    const std::vector<u8> golden = test::readBytes(hmanDir() / "pipeline.hman");
    for (usize n = 0; n < golden.size(); ++n) {
        const Result<Manifest> m = readManifest(std::span<const u8>(golden).first(n));
        REQUIRE_MESSAGE(!m.ok(), "a " << n << "-byte prefix read");
    }
    for (usize i = 0; i < golden.size(); ++i) {
        std::vector<u8> b = golden;
        b[i] ^= 0x20;
        const bool inSignature = i >= hman::kSignatureOffset && i < hman::kHeaderSize;
        REQUIRE_MESSAGE(readManifest(b).ok() == inSignature, "flipping byte " << i);
    }
    const std::vector<u8> z = test::readBytes(hmanDir() / "pipeline.cpp-zstd.hman");
    for (usize n = hman::kHeaderSize; n < z.size(); n += 7)
        REQUIRE(!readManifest(std::span<const u8>(z).first(n)).ok());
}

TEST_CASE("manifest: a zstd payload must decode to exactly bodySize, within the body cap") {
    const Manifest m = pipeline();
    ManifestWriteOptions o;
    o.codec = ManifestCodec::Zstd;
    o.zstdLevel = 3;
    const std::vector<u8> z = writeManifest(m, o).value();
    const auto withBodySize = [&](u64 size) {
        std::vector<u8> b = z;
        storeLE<u64>(b.data() + 48, size);
        const Hash256 h = blake2b256(std::span<const u8>(b).first(hman::kSignedBytes));
        std::copy(h.bytes.begin(), h.bytes.end(), b.begin() + hman::kHeaderHashOffset);
        return b;
    };
    const u64 real = loadLE<u64>(z.data() + 48);
    CHECK(readManifest(withBodySize(real - 1)).errorCode() == ErrorCode::Corrupt);
    CHECK(readManifest(withBodySize(real + 1)).errorCode() == ErrorCode::Corrupt);
    CHECK(readManifest(withBodySize(real + kMiB)).errorCode() == ErrorCode::Corrupt);
    ManifestReadOptions tight;
    tight.maxBodySize = 64;
    CHECK(readManifest(z, tight).errorCode() == ErrorCode::LimitExceeded);
    CHECK(readManifestHeader(z, tight).errorCode() == ErrorCode::LimitExceeded);

    // A payload that is not zstd at all, and one with a trailing garbage frame.
    std::vector<u8> garbage = z;
    std::fill(garbage.begin() + hman::kHeaderSize, garbage.end(), u8(0x5A));
    const Hash256 gh = blake2b256(std::span<const u8>(garbage).first(hman::kSignedBytes));
    std::copy(gh.bytes.begin(), gh.bytes.end(), garbage.begin() + hman::kHeaderHashOffset);
    CHECK(readManifest(garbage).errorCode() == ErrorCode::Corrupt);
}

TEST_CASE("manifest: the writer refuses invalid manifests and options") {
    const Manifest base = pipeline();
    const auto refuses = [&](auto&& mutate) {
        Manifest m = base;
        mutate(m);
        return writeManifest(m, rawOptions()).errorCode();
    };
    CHECK(refuses([](Manifest& m) { m.header.productId = "X"; }) == ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) { std::swap(m.files[0], m.files[1]); }) == ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) { m.files[0].tier = 3; }) == ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) {
              ManifestChunk extra;
              extra.hash.bytes.fill(0xFF);
              extra.rawSize = 1;
              m.chunks.push_back(extra);
          }) == ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) { m.patches[0].fromHash = m.files[m.patches[0].file].hash; }) ==
          ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) { ++m.refs[1].offset; }) == ErrorCode::InvalidArgument);
    CHECK(refuses([](Manifest& m) { m.files.resize(hman::kMaxFiles + 1); }) == ErrorCode::LimitExceeded);
    ManifestWriteOptions bad;
    bad.codec = static_cast<ManifestCodec>(7);
    CHECK(writeManifest(base, bad).errorCode() == ErrorCode::InvalidArgument);
    bad.codec = ManifestCodec::Zstd;
    for (const int level : {-1, 20}) {
        bad.zstdLevel = level;
        CHECK(writeManifest(base, bad).errorCode() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("manifest: write options default as Go's zero WriteOptions do") {
    // Default options write codec None; with codec Zstd, level 0 is level 19 (pkg/manifest's
    // TestWriteOptionDefaults checks the same in Go).
    const Manifest m = pipeline();
    const std::vector<u8> plain = writeManifest(m).value();
    CHECK(plain == writeManifest(m, rawOptions()).value());
    CHECK(readManifestHeader(plain)->codec == ManifestCodec::None);
    ManifestWriteOptions level0;
    level0.codec = ManifestCodec::Zstd;
    ManifestWriteOptions level19 = level0;
    level19.zstdLevel = 19;
    CHECK(writeManifest(m, level0).value() == writeManifest(m, level19).value());
}

TEST_CASE("manifest: the builder refuses what the format cannot hold") {
    const ManifestHeader h = pipeline().header;
    const std::vector<u8> data = test::randomBytes(1, 100'000);
    const ChunkedFile file = splitBuffer(data);
    REQUIRE(file.chunks.size() >= 2);
    const auto input = [&](std::string path, u8 tier = 1) {
        ManifestFileInput in;
        in.path = std::move(path);
        in.content = file;
        in.tier = tier;
        return in;
    };
    ManifestBuilder b(h);
    CHECK(b.addFile(input("a/../b")).errorCode() == ErrorCode::InvalidArgument);
    CHECK(b.addFile(input("x", 3)).errorCode() == ErrorCode::InvalidArgument);
    ManifestFileInput untiled = input("x");
    untiled.content.size += 1;
    CHECK(b.addFile(untiled).errorCode() == ErrorCode::InvalidArgument);
    REQUIRE(b.addFile(input("dup")).ok());
    REQUIRE(b.addFile(input("dup")).ok());
    CHECK(b.build().errorCode() == ErrorCode::InvalidArgument);

    const auto fails = [&](auto&& setup) {
        ManifestBuilder x(h);
        REQUIRE(x.addFile(input("a")).ok());
        setup(x);
        return x.build().errorCode();
    };
    Hash256 other;
    other.bytes[0] = 1;
    CHECK(fails([&](ManifestBuilder& x) { x.placeChunk(file.chunks[0].hash, other, 0, 10); }) ==
          ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) { x.setStoredSize(other, 10); }) == ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) {
              x.setStoredSize(file.chunks[0].hash, 10);
              x.setStoredSize(file.chunks[0].hash, 11);
          }) == ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) { x.addPatch("missing", other, other, 3); }) ==
          ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) { REQUIRE(x.addFile(input("A/b")).ok()); }) ==
          ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) {
              REQUIRE(x.addFile(input("docs/readme")).ok());
              REQUIRE(x.addFile(input("Docs/readme")).ok());
          }) == ErrorCode::InvalidArgument);
    CHECK(fails([&](ManifestBuilder& x) { x.addPack(other, 100); }) ==
          ErrorCode::InvalidArgument); // holds no chunk
    CHECK(fails([&](ManifestBuilder& x) {
              ManifestFileInput bad = input("b");
              bad.content.chunks[0].size -= 1;
              bad.content.chunks[1].offset -= 1;
              bad.content.chunks[1].size += 1;
              bad.content.chunks[1].hash = file.chunks[0].hash; // one ID, two sizes
              REQUIRE(x.addFile(bad).ok());
          }) == ErrorCode::InvalidArgument);
}

TEST_CASE("manifest: lookups agree with the tables") {
    const Manifest m = pipeline();
    for (const ManifestFile& f : m.files) {
        CHECK(m.findFile(f.path) == &f);
        CHECK(m.fileRefs(f).size() == f.refCount);
        for (const ManifestChunkRef& r : m.fileRefs(f)) CHECK(m.findChunk(m.chunks[r.chunk].hash) == r.chunk);
    }
    CHECK(m.findFile("nope") == nullptr);
    CHECK_FALSE(m.findChunk(Hash256{}).has_value());
    const ManifestFile* exe = m.findFile("bin/SampleGame.exe");
    REQUIRE(exe != nullptr);
    CHECK(exe->tier == 0);
    CHECK(hasFlag(exe->flags, ManifestFileFlags::Executable));
}

TEST_CASE("manifest: an empty build and a round trip through both codecs") {
    ManifestHeader h = pipeline().header;
    const Manifest empty = ManifestBuilder(h).build().value();
    for (const ManifestCodec codec : {ManifestCodec::None, ManifestCodec::Zstd}) {
        ManifestWriteOptions o;
        o.codec = codec;
        o.zstdLevel = 1;
        const std::vector<u8> bytes = writeManifest(empty, o).value();
        const Result<Manifest> back = readManifest(bytes);
        REQUIRE(back.ok());
        CHECK(*back == empty);
    }
    CHECK(readManifestFile(hmanDir() / "missing.hman").errorCode() == ErrorCode::NotFound);
}

TEST_CASE("manifest: a skippable zstd frame before the payload's frames is allowed, as in Go") {
    const std::vector<u8> z = test::readBytes(hmanDir() / "pipeline.go-zstd.hman");
    const u8 skip[] = {0x50, 0x2A, 0x4D, 0x18, 3, 0, 0, 0, 'h', 'm', 'n'};
    std::vector<u8> b(z.begin(), z.begin() + hman::kHeaderSize);
    b.insert(b.end(), std::begin(skip), std::end(skip));
    b.insert(b.end(), z.begin() + hman::kHeaderSize, z.end());
    storeLE<u64>(b.data() + 56, b.size() - hman::kHeaderSize);
    const Hash256 h = blake2b256(std::span<const u8>(b).first(hman::kSignedBytes));
    std::copy(h.bytes.begin(), h.bytes.end(), b.begin() + hman::kHeaderHashOffset);
    const Result<Manifest> m = readManifest(b);
    REQUIRE_MESSAGE(m.ok(), m.error().toString());
    CHECK(*m == pipeline());
}

} // namespace
