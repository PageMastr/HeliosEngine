// Fuzz target: the `.hman` v0 reader (05 §7; 02 §8.3 and 09 §6 list manifests among the fuzzed readers;
// CL-14 asks for fuzzed parsers).
//
// Each input runs twice: as given, and resealed (payloadSize, bodySize and bodyHash recomputed from the
// payload, then the header hash), so that mutated fields reach the validators behind the two hashes.
// Properties: readManifestHeader() and readManifest() return a Result (no crash, sanitizer report or
// out-of-bounds access); a manifest that reads is valid, its canonical body hashes to the header's
// bodyHash (one manifest has one body), it writes again (codec none) to bytes that read back equal,
// and its lookups agree with its tables. The reader's body cap is 16 MiB here, so -malloc_limit_mb
// catches a zstd payload that allocates ahead of what it decodes.

#include <zstd.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "helios/core/random.h"
#include "helios/patch/manifest.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::patch;

constexpr usize kMaxInput = 4 * kMiB;
constexpr u64 kMaxBody = 16 * kMiB;
constexpr usize kMaxLookups = 256;

[[noreturn]] void fail() { std::abort(); }

void exercise(const u8* data, usize size) {
    const std::span<const u8> in(data, size);
    ManifestReadOptions options;
    options.maxBodySize = kMaxBody;
    const Result<ManifestHeaderInfo> info = readManifestHeader(in, options);
    const Result<Manifest> m = readManifest(in, options);
    if (m.ok() && !info.ok()) fail();
    if (!m) return;
    if (info->header != m->header || !validateManifest(*m)) fail();
    const Result<std::vector<u8>> body = encodeManifestBody(*m);
    if (!body || body->size() != info->bodySize || blake2b256(*body) != info->bodyHash) fail();
    ManifestWriteOptions raw;
    raw.codec = ManifestCodec::None;
    const Result<std::vector<u8>> again = writeManifest(*m, raw);
    if (!again) fail();
    const Result<Manifest> back = readManifest(*again);
    if (!back || *back != *m) fail();
    for (usize i = 0; i < m->files.size() && i < kMaxLookups; ++i) {
        const ManifestFile& f = m->files[i];
        if (m->findFile(f.path) != &f || m->fileRefs(f).size() != f.refCount) fail();
        for (const ManifestChunkRef& r : m->fileRefs(f))
            if (m->findChunk(m->chunks[r.chunk].hash) != r.chunk) fail();
    }
}

/// Makes the header's sizes and hashes match the payload, so mutations reach the field checks.
void reseal(std::vector<u8>& bytes) {
    using namespace hman;
    if (bytes.size() < kHeaderSize) return;
    const std::span<u8, kHeaderSize> head(bytes.data(), kHeaderSize);
    RawHeader h = decodeHeader(head);
    const std::span<const u8> payload(bytes.data() + kHeaderSize, bytes.size() - kHeaderSize);
    h.payloadSize = payload.size();
    if (h.codec == 1) {
        // Decode with zstd directly (bounded) to learn the body; leave the header alone if that fails, or
        // if the first frame declares more than the cap (deep-paths.hman: 72 MB), without decoding it.
        const unsigned long long declared = ZSTD_getFrameContentSize(payload.data(), payload.size());
        if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != ZSTD_CONTENTSIZE_ERROR && declared > kMaxBody)
            return;
        static std::vector<u8> body(static_cast<usize>(kMaxBody)); // allocated once, never cleared
        const usize n = ZSTD_decompress(body.data(), body.size(), payload.data(), payload.size());
        if (ZSTD_isError(n)) return;
        h.bodySize = n;
        h.bodyHash = blake2b256(std::span<const u8>(body.data(), n));
    } else {
        h.bodySize = payload.size();
        h.bodyHash = blake2b256(payload);
    }
    encodeHeader(h, head);
    h.headerHash = computeHeaderHash(head);
    encodeHeader(h, head);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInput) return 0;
    exercise(data, size);
    std::vector<u8> sealed(data, data + size);
    reseal(sealed);
    exercise(sealed.data(), sealed.size());
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const auto bytes = [](u64 seed, usize size) {
        std::vector<u8> v(size);
        SplitMix64 rng(seed);
        for (u8& b : v) b = static_cast<u8>(rng.next() >> 56);
        return v;
    };
    ManifestHeader header;
    header.productId = "sample-game";
    header.platform = "linux64";
    header.buildId = "fuzz-1";
    header.sequence = 7;
    header.createdAt = 1'790'000'000;
    header.expiresAt = 1'790'604'800;
    header.compatEpoch = 2;
    const auto write = [&](const ManifestBuilder& b, ManifestCodec codec) {
        ManifestWriteOptions o;
        o.codec = codec;
        o.zstdLevel = 3;
        const Result<Manifest> m = b.build();
        if (!m) fail();
        Result<std::vector<u8>> file = writeManifest(*m, o);
        if (!file) fail();
        return std::move(*file);
    };
    const auto add = [](ManifestBuilder& b, const char* path, const std::vector<u8>& data, u8 tier,
                        ManifestFileFlags flags = ManifestFileFlags::None) {
        ManifestFileInput f;
        f.path = path;
        f.content = splitBuffer(data);
        f.tier = tier;
        f.flags = flags;
        if (!b.addFile(std::move(f))) fail();
    };

    ManifestBuilder empty(header);
    out.push_back(write(empty, ManifestCodec::None)); // no files

    ManifestBuilder small(header);
    const std::vector<u8> exe = bytes(1, 90'000);
    add(small, "bin/Client.exe", exe, 0, ManifestFileFlags::Executable);
    add(small, "content/common.hpak", bytes(2, 200'000), 1);
    add(small, "content/zone/a.hpak", bytes(2, 200'000), 2, ManifestFileFlags::Optional); // same chunks
    add(small, "empty.txt", {}, 1);
    add(small, "launcher.json", bytes(3, 300), 0);
    const ChunkedFile tiny = splitBuffer(bytes(3, 300));
    const Hash256 pack = blake2b256(bytes(9, 32));
    small.addPack(pack, 8 * kMiB);
    small.placeChunk(tiny.chunks[0].hash, pack, 4096, 200);
    small.setStoredSize(splitBuffer(exe).chunks[0].hash, 60'000);
    small.addPatch("bin/Client.exe", blake2b256(bytes(4, 64)), blake2b256(bytes(5, 64)), 1234);
    out.push_back(write(small, ManifestCodec::None));
    out.push_back(write(small, ManifestCodec::Zstd));

    ManifestBuilder many(header); // many short files: dense path and ref tables
    for (int i = 0; i < 40; ++i) {
        const std::string path = "data/f" + std::to_string(i) + ".bin";
        ManifestFileInput f;
        f.path = path;
        f.content = splitBuffer(bytes(100 + static_cast<u64>(i), static_cast<usize>(i) * 37));
        f.tier = static_cast<u8>(i % 3);
        f.group = static_cast<u64>(i) * 0x9E3779B97F4A7C15ull;
        f.language = static_cast<u32>(i % 5);
        if (!many.addFile(std::move(f))) fail();
    }
    out.push_back(write(many, ManifestCodec::Zstd));

    // Deep paths (as deep-paths.hman, whose 72 MB body is above this target's cap): 64 files of 1024-byte
    // paths 508 directories deep, so mutations meet the path-collision check at depth.
    std::string deep;
    for (int i = 0; i < 508; ++i) deep += "a/";
    Manifest deepPaths;
    deepPaths.header = header;
    for (int i = 0; i < 64; ++i) {
        ManifestFile f;
        f.path = deep + std::format("f{:07}", i);
        f.hash = blake2b256(std::span<const u8>{});
        deepPaths.files.push_back(std::move(f));
    }
    ManifestWriteOptions zstd3;
    zstd3.codec = ManifestCodec::Zstd;
    zstd3.zstdLevel = 3;
    Result<std::vector<u8>> deepFile = writeManifest(deepPaths, zstd3);
    if (!deepFile) fail();

    // A header that claims a 16 MiB + 1 body (LimitExceeded) and a zstd payload of zeros (Corrupt).
    std::vector<u8> claim = out[1];
    hman::RawHeader h =
        hman::decodeHeader(std::span<const u8, hman::kHeaderSize>(claim.data(), hman::kHeaderSize));
    h.bodySize = kMaxBody + 1;
    h.codec = 1;
    const std::span<u8, hman::kHeaderSize> head(claim.data(), hman::kHeaderSize);
    hman::encodeHeader(h, head);
    h.headerHash = hman::computeHeaderHash(head);
    hman::encodeHeader(h, head);
    out.push_back(std::move(claim));
    out.push_back(std::move(*deepFile)); // last, so the earlier seeds keep their file names
}
