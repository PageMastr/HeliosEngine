// `.hman` v0: header codec, validation, the canonical body writer and the hostile-input reader.
#include "helios/patch/manifest.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>

namespace helios::patch {

namespace hman {

void encodeHeader(const RawHeader& h, std::span<u8, kHeaderSize> out) noexcept {
    u8* p = out.data();
    std::memset(p, 0, kHeaderSize);
    storeLE<u32>(p + 0, h.magic);
    storeLE<u16>(p + 4, h.version);
    storeLE<u16>(p + 6, h.headerSize);
    storeLE<u32>(p + 8, h.flags);
    p[12] = h.codec;
    std::memcpy(p + 13, h.reserved0, 3);
    storeLE<u64>(p + 16, h.sequence);
    storeLE<u64>(p + 24, h.createdAt);
    storeLE<u64>(p + 32, h.expiresAt);
    storeLE<u32>(p + 40, h.compatEpoch);
    storeLE<u32>(p + 44, h.reserved1);
    storeLE<u64>(p + 48, h.bodySize);
    storeLE<u64>(p + 56, h.payloadSize);
    std::memcpy(p + 64, h.bodyHash.bytes.data(), 32);
    std::memcpy(p + 96, h.productId.data(), kProductIdMax);
    std::memcpy(p + 128, h.platform.data(), kPlatformMax);
    std::memcpy(p + 160, h.buildId.data(), kBuildIdMax);
    std::memcpy(p + 224, h.keyId.data(), kKeyIdSize);
    std::memcpy(p + 240, h.reserved2, 16);
    std::memcpy(p + kHeaderHashOffset, h.headerHash.bytes.data(), 32);
    std::memcpy(p + kSignatureOffset, h.signature.data(), kSignatureSize);
}

RawHeader decodeHeader(std::span<const u8, kHeaderSize> in) noexcept {
    const u8* p = in.data();
    RawHeader h;
    h.magic = loadLE<u32>(p + 0);
    h.version = loadLE<u16>(p + 4);
    h.headerSize = loadLE<u16>(p + 6);
    h.flags = loadLE<u32>(p + 8);
    h.codec = p[12];
    std::memcpy(h.reserved0, p + 13, 3);
    h.sequence = loadLE<u64>(p + 16);
    h.createdAt = loadLE<u64>(p + 24);
    h.expiresAt = loadLE<u64>(p + 32);
    h.compatEpoch = loadLE<u32>(p + 40);
    h.reserved1 = loadLE<u32>(p + 44);
    h.bodySize = loadLE<u64>(p + 48);
    h.payloadSize = loadLE<u64>(p + 56);
    std::memcpy(h.bodyHash.bytes.data(), p + 64, 32);
    std::memcpy(h.productId.data(), p + 96, kProductIdMax);
    std::memcpy(h.platform.data(), p + 128, kPlatformMax);
    std::memcpy(h.buildId.data(), p + 160, kBuildIdMax);
    std::memcpy(h.keyId.data(), p + 224, kKeyIdSize);
    std::memcpy(h.reserved2, p + 240, 16);
    std::memcpy(h.headerHash.bytes.data(), p + kHeaderHashOffset, 32);
    std::memcpy(h.signature.data(), p + kSignatureOffset, kSignatureSize);
    return h;
}

Hash256 computeHeaderHash(std::span<const u8, kHeaderSize> encoded) noexcept {
    return blake2b256(encoded.first(kSignedBytes));
}

} // namespace hman

namespace {

using namespace hman;

constexpr u32 kMaxStoredSize = fastcdc::kMaxSize + 4096; // a stored chunk: one zstd frame of ≤ 256 KiB

/// The largest payload a body of `bodySize` bytes may have (codec Zstd): above zstd's compress bound.
constexpr u64 maxPayload(u64 bodySize) noexcept { return bodySize + bodySize / 128 + 4096; }

/// The body cap a read applies: the option, at most kMaxBodySize; 0 means kMaxBodySize (as in Go).
u64 effectiveMaxBody(const ManifestReadOptions& options) noexcept {
    return options.maxBodySize == 0 ? kMaxBodySize : std::min(options.maxBodySize, kMaxBodySize);
}

constexpr bool inRange(char c, char lo, char hi) noexcept { return c >= lo && c <= hi; }
constexpr bool isLowerAlnum(char c) noexcept { return inRange(c, 'a', 'z') || inRange(c, '0', '9'); }
constexpr bool isAlnum(char c) noexcept { return isLowerAlnum(c) || inRange(c, 'A', 'Z'); }

bool validProductId(std::string_view s) noexcept {
    if (s.size() < 3 || s.size() > kProductIdMax || !inRange(s[0], 'a', 'z')) return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) { return isLowerAlnum(c) || c == '-'; });
}
bool validPlatform(std::string_view s) noexcept {
    if (s.size() < 2 || s.size() > kPlatformMax || !inRange(s[0], 'a', 'z')) return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return isLowerAlnum(c) || c == '-' || c == '_'; });
}
bool validBuildId(std::string_view s) noexcept {
    if (s.empty() || s.size() > kBuildIdMax || !isAlnum(s[0])) return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return isAlnum(c) || c == '.' || c == '_' || c == '-'; });
}

constexpr char asciiLower(char c) noexcept {
    return inRange(c, 'A', 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// CON, PRN, AUX, NUL, COM0-9 and LPT0-9, with any extension: names Windows reserves for devices.
bool windowsDeviceName(std::string_view segment) noexcept {
    if (segment.size() < 3) return false;
    const std::string_view stem = segment.substr(0, segment.find('.'));
    if (stem.size() != 3 && stem.size() != 4) return false;
    char s[4] = {};
    for (usize i = 0; i < stem.size(); ++i) s[i] = asciiLower(stem[i]);
    const std::string_view low(s, stem.size());
    if (low == "con" || low == "prn" || low == "aux" || low == "nul") return true;
    return stem.size() == 4 && (low.substr(0, 3) == "com" || low.substr(0, 3) == "lpt") &&
           inRange(s[3], '0', '9');
}

/// The bytes a path segment may hold: [A-Za-z0-9._+-].
constexpr std::array<bool, 256> kPathByte = [] {
    std::array<bool, 256> t{};
    for (int c = 0; c < 256; ++c) {
        const char ch = static_cast<char>(c);
        t[static_cast<usize>(c)] = isAlnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == '+';
    }
    return t;
}();

/// A path's collision-key byte: ASCII letters lowered and '/' mapped to 0x00, which no valid path holds.
/// Byte order on keys sorts paths segment by segment with the separator lowest, so the paths inside a
/// directory follow the directory's own key directly.
constexpr std::array<char, 256> kCollisionFold = [] {
    std::array<char, 256> t{};
    for (int c = 0; c < 256; ++c) t[static_cast<usize>(c)] = asciiLower(static_cast<char>(c));
    t[static_cast<usize>('/')] = '\0';
    return t;
}();

template <class... Args>
Error invalid(std::format_string<Args...> fmt, Args&&... args) {
    return makeError(ErrorCode::InvalidArgument, fmt, std::forward<Args>(args)...);
}
template <class... Args>
Error corrupt(std::format_string<Args...> fmt, Args&&... args) {
    return makeError(ErrorCode::Corrupt, fmt, std::forward<Args>(args)...);
}

/// Body size for these counts (each bounded by its limit, so the sum cannot overflow).
u64 bodySizeFor(u64 files, u64 refs, u64 chunks, u64 packs, u64 patches, u64 stringBytes) noexcept {
    return kBodyHeaderSize + files * kFileEntrySize + refs * kRefEntrySize + chunks * kChunkEntrySize +
           packs * kPackEntrySize + patches * kPatchEntrySize + stringBytes;
}

Result<void> validateHeaderFields(const ManifestHeader& h) {
    if (!validProductId(h.productId))
        return invalid("productId '{}' is not ^[a-z][a-z0-9-]{{2,31}}$", h.productId);
    if (!validPlatform(h.platform))
        return invalid("platform '{}' is not ^[a-z][a-z0-9_-]{{1,31}}$", h.platform);
    if (!validBuildId(h.buildId))
        return invalid("buildId '{}' is not ^[A-Za-z0-9][A-Za-z0-9._-]{{0,63}}$", h.buildId);
    if (h.expiresAt != 0 && h.expiresAt <= h.createdAt)
        return invalid("expiresAt {} is not after createdAt {}", h.expiresAt, h.createdAt);
    return {};
}

/// Copies an identifier into its zero-padded header field (the caller validated its length).
template <usize N>
void putId(std::array<char, N>& field, std::string_view s) noexcept {
    field.fill(0);
    std::memcpy(field.data(), s.data(), std::min(N, s.size()));
}

/// Reads a zero-padded identifier field: the bytes before the first zero, all later bytes zero.
template <usize N>
std::optional<std::string> getId(const std::array<char, N>& field) {
    const auto end = std::find(field.begin(), field.end(), '\0');
    if (!std::all_of(end, field.end(), [](char c) { return c == '\0'; })) return std::nullopt;
    return std::string(field.begin(), end);
}

bool allZero(const u8* p, usize n) noexcept {
    return std::all_of(p, p + n, [](u8 b) { return b == 0; });
}

/// A decoded zstd payload in a malloc'd block. It grows with realloc, which (glibc, for large blocks) moves
/// the pages instead of copying and re-faulting them: doubling a std::vector to 72 MB cost 140 ms.
struct DecodedBody {
    struct Free {
        void operator()(u8* p) const noexcept { std::free(p); }
    };
    std::unique_ptr<u8, Free> data;
    usize size = 0;
    std::span<const u8> bytes() const noexcept { return {data.get(), size}; }
};

Result<DecodedBody> decompress(std::span<const u8> payload, u64 bodySize) {
    struct DCtx {
        ZSTD_DCtx* p = ZSTD_createDCtx();
        ~DCtx() { ZSTD_freeDCtx(p); }
    } d;
    if (!d.p) return Error{ErrorCode::OutOfMemory, "ZSTD_createDCtx failed"};
    if (ZSTD_isError(ZSTD_DCtx_setParameter(d.p, ZSTD_d_windowLogMax, kMaxZstdWindowLog)))
        return Error{ErrorCode::Unknown, "ZSTD_d_windowLogMax refused"};
    // Grow only as bytes decode (the claim alone costs nothing); one byte of room past bodySize
    // catches a payload that decodes to more.
    const u64 cap = bodySize + 1;
    DecodedBody out;
    usize room = 0;
    usize produced = 0;
    ZSTD_inBuffer in{payload.data(), payload.size(), 0};
    for (;;) {
        if (produced == room) {
            if (room == cap) return corrupt("the zstd payload decodes to more than bodySize ({})", bodySize);
            const usize grown = static_cast<usize>(std::min<u64>(cap, room == 0 ? 1 * kMiB : u64(room) * 2));
            u8* p = static_cast<u8*>(std::realloc(out.data.get(), grown));
            if (!p) return Error{ErrorCode::OutOfMemory, "no memory for a decoded manifest body"};
            (void)out.data.release();
            out.data.reset(p);
            room = grown;
        }
        ZSTD_outBuffer o{out.data.get(), room, produced};
        const usize before = in.pos;
        const usize ret = ZSTD_decompressStream(d.p, &o, &in);
        if (ZSTD_isError(ret)) return corrupt("zstd payload: {}", ZSTD_getErrorName(ret));
        const bool progressed = in.pos != before || o.pos != produced;
        produced = o.pos;
        if (in.pos == in.size && produced < room) {
            if (ret != 0) return corrupt("the zstd payload ends inside a frame");
            break;
        }
        if (!progressed) return corrupt("the zstd payload does not decode");
    }
    if (produced != bodySize)
        return corrupt("the zstd payload decodes to {} bytes, bodySize is {}", produced, bodySize);
    out.size = produced;
    return out;
}

/// Decodes the body tables (layout and reserved-field checks only; validateManifest does the rest).
Result<void> decodeBody(std::span<const u8> body, Manifest& m) {
    if (body.size() < kBodyHeaderSize)
        return corrupt("the body is {} bytes, shorter than its header", body.size());
    const u8* p = body.data();
    const u32 fileCount = loadLE<u32>(p + 0);
    const u32 chunkCount = loadLE<u32>(p + 4);
    const u32 refCount = loadLE<u32>(p + 8);
    const u32 packCount = loadLE<u32>(p + 12);
    const u32 patchCount = loadLE<u32>(p + 16);
    const u32 stringBytes = loadLE<u32>(p + 20);
    if (loadLE<u64>(p + 24) != 0) return corrupt("reserved body-header bytes are not zero");
    if (fileCount > kMaxFiles || chunkCount > kMaxChunks || refCount > kMaxRefs || packCount > kMaxPacks ||
        patchCount > kMaxPatches || stringBytes > kMaxStringBytes)
        return makeError(
            ErrorCode::LimitExceeded,
            "counts {} files, {} chunks, {} refs, {} packs, {} patches, {} path bytes exceed the "
            "limits",
            fileCount, chunkCount, refCount, packCount, patchCount, stringBytes);
    const u64 expected = bodySizeFor(fileCount, refCount, chunkCount, packCount, patchCount, stringBytes);
    if (expected != body.size())
        return corrupt("the body is {} bytes, its counts need {}", body.size(), expected);

    const u8* files = p + kBodyHeaderSize;
    const u8* refs = files + u64(fileCount) * kFileEntrySize;
    const u8* chunks = refs + u64(refCount) * kRefEntrySize;
    const u8* packs = chunks + u64(chunkCount) * kChunkEntrySize;
    const u8* patches = packs + u64(packCount) * kPackEntrySize;
    const u8* strings = patches + u64(patchCount) * kPatchEntrySize;

    m.files.resize(fileCount);
    u64 pathAt = 0;
    for (u32 i = 0; i < fileCount; ++i) {
        const u8* e = files + u64(i) * kFileEntrySize;
        ManifestFile& f = m.files[i];
        const u32 pathOffset = loadLE<u32>(e + 0);
        const u32 pathLength = loadLE<u32>(e + 4);
        if (pathOffset != pathAt)
            return corrupt("file {}: path offset {} is not {} (paths in file order)", i, pathOffset, pathAt);
        if (pathLength == 0 || pathLength > kMaxPathBytes || pathLength > stringBytes - pathAt)
            return corrupt("file {}: path length {} is out of range", i, pathLength);
        f.path.assign(reinterpret_cast<const char*>(strings + pathAt), pathLength);
        pathAt += pathLength;
        f.size = loadLE<u64>(e + 8);
        std::memcpy(f.hash.bytes.data(), e + 16, 32);
        f.firstRef = loadLE<u32>(e + 48);
        f.refCount = loadLE<u32>(e + 52);
        f.group = loadLE<u64>(e + 56);
        f.language = loadLE<u32>(e + 64);
        f.tier = e[68];
        f.flags = static_cast<ManifestFileFlags>(loadLE<u16>(e + 70));
        if (e[69] != 0 || loadLE<u64>(e + 72) != 0) return corrupt("file {}: reserved bytes are not zero", i);
    }
    if (pathAt != stringBytes) return corrupt("{} path bytes are not any file's path", stringBytes - pathAt);

    m.refs.resize(refCount);
    for (u32 i = 0; i < refCount; ++i) {
        const u8* e = refs + u64(i) * kRefEntrySize;
        m.refs[i].chunk = loadLE<u32>(e + 0);
        m.refs[i].offset = loadLE<u64>(e + 8);
        if (loadLE<u32>(e + 4) != 0) return corrupt("ref {}: reserved bytes are not zero", i);
    }
    m.chunks.resize(chunkCount);
    for (u32 i = 0; i < chunkCount; ++i) {
        const u8* e = chunks + u64(i) * kChunkEntrySize;
        ManifestChunk& c = m.chunks[i];
        std::memcpy(c.hash.bytes.data(), e, 32);
        c.rawSize = loadLE<u32>(e + 32);
        c.storedSize = loadLE<u32>(e + 36);
        c.pack = loadLE<u32>(e + 40);
        c.packOffset = loadLE<u64>(e + 48);
        if (loadLE<u32>(e + 44) != 0) return corrupt("chunk {}: reserved bytes are not zero", i);
    }
    m.packs.resize(packCount);
    for (u32 i = 0; i < packCount; ++i) {
        const u8* e = packs + u64(i) * kPackEntrySize;
        std::memcpy(m.packs[i].hash.bytes.data(), e, 32);
        m.packs[i].size = loadLE<u64>(e + 32);
    }
    m.patches.resize(patchCount);
    for (u32 i = 0; i < patchCount; ++i) {
        const u8* e = patches + u64(i) * kPatchEntrySize;
        ManifestPatch& q = m.patches[i];
        q.file = loadLE<u32>(e + 0);
        if (loadLE<u32>(e + 4) != 0) return corrupt("patch {}: reserved bytes are not zero", i);
        std::memcpy(q.fromHash.bytes.data(), e + 8, 32);
        std::memcpy(q.patchHash.bytes.data(), e + 40, 32);
        q.patchSize = loadLE<u64>(e + 72);
    }
    return {};
}

bool patchLess(const ManifestPatch& a, const ManifestPatch& b) noexcept {
    return a.file != b.file ? a.file < b.file : a.fromHash < b.fromHash;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------------

bool isValidManifestPath(std::string_view path) noexcept {
    // One pass over the bytes, no allocation: readers run this on every path (up to 64 MiB of them).
    if (path.empty() || path.size() > kMaxPathBytes) return false;
    // path[start, end), a run of path bytes, is a segment: not empty, not ending in '.' (so not "." or
    // ".."), not a device name.
    const auto segmentEnds = [path](usize start, usize end) {
        return end > start && path[end - 1] != '.' &&
               (end - start < 3 || !windowsDeviceName(path.substr(start, end - start)));
    };
    usize start = 0;
    for (usize i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (kPathByte[static_cast<u8>(c)]) continue;
        if (c != '/' || !segmentEnds(start, i)) return false;
        start = i + 1;
    }
    return segmentEnds(start, path.size());
}

namespace {

/// Rejects two (valid) paths that are equal ignoring ASCII case, and a file that is also a directory of
/// another file (`a` and `A/b`), so a manifest installs the same tree on NTFS and ext4. It sorts the
/// collision keys (kCollisionFold) and compares neighbours: O(P + n log n · ℓ) byte operations for P path
/// bytes, n files and common prefixes of ℓ ≤ kMaxPathBytes, without hashing attacker-chosen strings.
/// (Looking up every '/'-prefix of every path in a set costs about len²/4 per path: seconds for 64 MiB of
/// deep paths.)
Result<void> checkPathCollisions(const std::vector<ManifestFile>& files, u64 stringBytes) {
    struct Key {
        std::string_view key;
        u32 file;
    };
    const auto buffer = std::make_unique_for_overwrite<char[]>(static_cast<usize>(stringBytes));
    std::vector<Key> keys(files.size());
    char* at = buffer.get();
    for (usize i = 0; i < files.size(); ++i) {
        const std::string& p = files[i].path;
        for (usize j = 0; j < p.size(); ++j) at[j] = kCollisionFold[static_cast<u8>(p[j])];
        keys[i] = Key{std::string_view(at, p.size()), static_cast<u32>(i)};
        at += p.size();
    }
    const auto less = [](const Key& a, const Key& b) {
        const int c = a.key.compare(b.key);
        return c != 0 ? c < 0 : a.file < b.file;
    };
    // Paths arrive sorted by bytes, so their keys often are too (unless case or a byte below '/' reorders
    // them): checking costs one comparison per file, sorting n log n.
    if (!std::is_sorted(keys.begin(), keys.end(), less)) std::sort(keys.begin(), keys.end(), less);
    for (usize k = 1; k < keys.size(); ++k) {
        const Key& a = keys[k - 1];
        const Key& b = keys[k];
        if (a.key == b.key)
            return invalid("'{}' and '{}' differ only in case", files[a.file].path, files[b.file].path);
        if (b.key.size() > a.key.size() && b.key[a.key.size()] == '\0' && b.key.starts_with(a.key))
            return invalid("'{}' is a file and also a directory of '{}'", files[a.file].path,
                           files[b.file].path);
    }
    return {};
}

} // namespace

Result<void> validateManifest(const Manifest& m) {
    HELIOS_TRY(validateHeaderFields(m.header));
    const u64 files = m.files.size(), refs = m.refs.size(), chunks = m.chunks.size(), packs = m.packs.size(),
              patches = m.patches.size();
    if (files > kMaxFiles || chunks > kMaxChunks || refs > kMaxRefs || packs > kMaxPacks ||
        patches > kMaxPatches)
        return makeError(ErrorCode::LimitExceeded,
                         "{} files, {} chunks, {} refs, {} packs, {} patches exceed the limits", files,
                         chunks, refs, packs, patches);
    u64 stringBytes = 0;
    for (const ManifestFile& f : m.files) stringBytes += f.path.size();
    if (stringBytes > kMaxStringBytes)
        return makeError(ErrorCode::LimitExceeded, "{} path bytes exceed the limit", stringBytes);
    const u64 bodySize = bodySizeFor(files, refs, chunks, packs, patches, stringBytes);
    if (bodySize > kMaxBodySize)
        return makeError(ErrorCode::LimitExceeded, "the body would be {} bytes, above {}", bodySize,
                         kMaxBodySize);

    // Files: valid paths in strictly increasing byte order, no two equal ignoring ASCII case, no file
    // that is also another file's directory; tags in range; refs contiguous and tiling the file.
    std::vector<bool> chunkUsed(m.chunks.size(), false);
    u64 refAt = 0;
    for (usize i = 0; i < m.files.size(); ++i) {
        const ManifestFile& f = m.files[i];
        if (!isValidManifestPath(f.path))
            return invalid("file {}: '{}' is not a valid manifest path", i, f.path);
        if (i > 0 && !(m.files[i - 1].path < f.path))
            return invalid("file {}: '{}' is not after '{}' (sorted, unique paths)", i, f.path,
                           m.files[i - 1].path);
        if (f.tier > kMaxTier) return invalid("file '{}': tier {} is not 0, 1 or 2", f.path, f.tier);
        if ((toUnderlying(f.flags) & ~kManifestFileFlagsKnown) != 0)
            return invalid("file '{}': unknown flags {:#x}", f.path, toUnderlying(f.flags));
        if (f.firstRef != refAt)
            return invalid("file '{}': first ref {} is not {}", f.path, f.firstRef, refAt);
        if (f.refCount > refs - refAt)
            return invalid("file '{}': {} refs run past the ref table", f.path, f.refCount);
        u64 offset = 0;
        for (u32 k = 0; k < f.refCount; ++k) {
            const ManifestChunkRef& r = m.refs[refAt + k];
            if (r.chunk >= chunks)
                return invalid("file '{}' ref {}: chunk {} does not exist", f.path, k, r.chunk);
            if (r.offset != offset)
                return invalid("file '{}' ref {}: offset {} is not {}", f.path, k, r.offset, offset);
            offset += m.chunks[r.chunk].rawSize;
            chunkUsed[r.chunk] = true;
        }
        if (offset != f.size)
            return invalid("file '{}': its chunks hold {} bytes, its size is {}", f.path, offset, f.size);
        refAt += f.refCount;
    }
    if (refAt != refs) return invalid("{} refs belong to no file", refs - refAt);
    HELIOS_TRY(checkPathCollisions(m.files, stringBytes));

    std::vector<bool> packUsed(m.packs.size(), false);
    for (usize i = 0; i < m.chunks.size(); ++i) {
        const ManifestChunk& c = m.chunks[i];
        if (i > 0 && !(m.chunks[i - 1].hash < c.hash))
            return invalid("chunk {}: IDs are not sorted and unique", i);
        if (c.rawSize == 0 || c.rawSize > fastcdc::kMaxSize)
            return invalid("chunk {}: raw size {} is out of range", i, c.rawSize);
        if (c.storedSize > kMaxStoredSize)
            return invalid("chunk {}: stored size {} is out of range", i, c.storedSize);
        if (!chunkUsed[i]) return invalid("chunk {} belongs to no file", c.hash.toHex());
        if (c.pack == kNoPack) {
            if (c.packOffset != 0) return invalid("chunk {}: a loose chunk has a pack offset", i);
        } else {
            if (c.pack >= packs) return invalid("chunk {}: pack {} does not exist", i, c.pack);
            const u64 packSize = m.packs[c.pack].size;
            if (c.storedSize == 0 || c.packOffset > packSize || c.storedSize > packSize - c.packOffset)
                return invalid("chunk {}: {} stored bytes at {} do not fit pack {} ({} bytes)", i,
                               c.storedSize, c.packOffset, c.pack, packSize);
            packUsed[c.pack] = true;
        }
    }
    for (usize i = 0; i < m.packs.size(); ++i) {
        const ManifestPack& p = m.packs[i];
        if (i > 0 && !(m.packs[i - 1].hash < p.hash))
            return invalid("pack {}: IDs are not sorted and unique", i);
        if (p.size == 0 || p.size > kMaxPackSize)
            return invalid("pack {}: size {} is out of range", i, p.size);
        if (!packUsed[i]) return invalid("pack {} holds no chunk", p.hash.toHex());
    }
    for (usize i = 0; i < m.patches.size(); ++i) {
        const ManifestPatch& q = m.patches[i];
        if (q.file >= files) return invalid("patch {}: file {} does not exist", i, q.file);
        if (i > 0 && !patchLess(m.patches[i - 1], q))
            return invalid("patch {}: not sorted by (file, fromHash) and unique", i);
        if (q.fromHash == m.files[q.file].hash)
            return invalid("patch {}: it patches '{}' into itself", i, m.files[q.file].path);
        if (q.patchSize == 0 || q.patchSize > kMaxPatchSize)
            return invalid("patch {}: size {} is out of range", i, q.patchSize);
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Manifest helpers
// ---------------------------------------------------------------------------------------------

std::span<const ManifestChunkRef> Manifest::fileRefs(const ManifestFile& file) const noexcept {
    if (file.firstRef > refs.size() || file.refCount > refs.size() - file.firstRef) return {};
    return std::span<const ManifestChunkRef>(refs).subspan(file.firstRef, file.refCount);
}

const ManifestFile* Manifest::findFile(std::string_view path) const noexcept {
    const auto it = std::lower_bound(files.begin(), files.end(), path,
                                     [](const ManifestFile& f, std::string_view p) { return f.path < p; });
    return it != files.end() && it->path == path ? &*it : nullptr;
}

std::optional<u32> Manifest::findChunk(const Hash256& hash) const noexcept {
    const auto it = std::lower_bound(chunks.begin(), chunks.end(), hash,
                                     [](const ManifestChunk& c, const Hash256& h) { return c.hash < h; });
    if (it == chunks.end() || it->hash != hash) return std::nullopt;
    return static_cast<u32>(it - chunks.begin());
}

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------

Result<std::vector<u8>> encodeManifestBody(const Manifest& m) {
    HELIOS_TRY(validateManifest(m));
    u64 stringBytes = 0;
    for (const ManifestFile& f : m.files) stringBytes += f.path.size();
    const u64 size = bodySizeFor(m.files.size(), m.refs.size(), m.chunks.size(), m.packs.size(),
                                 m.patches.size(), stringBytes);
    std::vector<u8> body(static_cast<usize>(size), 0);
    u8* p = body.data();
    storeLE<u32>(p + 0, static_cast<u32>(m.files.size()));
    storeLE<u32>(p + 4, static_cast<u32>(m.chunks.size()));
    storeLE<u32>(p + 8, static_cast<u32>(m.refs.size()));
    storeLE<u32>(p + 12, static_cast<u32>(m.packs.size()));
    storeLE<u32>(p + 16, static_cast<u32>(m.patches.size()));
    storeLE<u32>(p + 20, static_cast<u32>(stringBytes));
    u8* e = p + kBodyHeaderSize;
    u8* strings = p + (size - stringBytes);
    u32 pathAt = 0;
    for (const ManifestFile& f : m.files) {
        storeLE<u32>(e + 0, pathAt);
        storeLE<u32>(e + 4, static_cast<u32>(f.path.size()));
        storeLE<u64>(e + 8, f.size);
        std::memcpy(e + 16, f.hash.bytes.data(), 32);
        storeLE<u32>(e + 48, f.firstRef);
        storeLE<u32>(e + 52, f.refCount);
        storeLE<u64>(e + 56, f.group);
        storeLE<u32>(e + 64, f.language);
        e[68] = f.tier;
        storeLE<u16>(e + 70, toUnderlying(f.flags));
        std::memcpy(strings + pathAt, f.path.data(), f.path.size());
        pathAt += static_cast<u32>(f.path.size());
        e += kFileEntrySize;
    }
    for (const ManifestChunkRef& r : m.refs) {
        storeLE<u32>(e + 0, r.chunk);
        storeLE<u64>(e + 8, r.offset);
        e += kRefEntrySize;
    }
    for (const ManifestChunk& c : m.chunks) {
        std::memcpy(e, c.hash.bytes.data(), 32);
        storeLE<u32>(e + 32, c.rawSize);
        storeLE<u32>(e + 36, c.storedSize);
        storeLE<u32>(e + 40, c.pack);
        storeLE<u64>(e + 48, c.packOffset);
        e += kChunkEntrySize;
    }
    for (const ManifestPack& k : m.packs) {
        std::memcpy(e, k.hash.bytes.data(), 32);
        storeLE<u64>(e + 32, k.size);
        e += kPackEntrySize;
    }
    for (const ManifestPatch& q : m.patches) {
        storeLE<u32>(e + 0, q.file);
        std::memcpy(e + 8, q.fromHash.bytes.data(), 32);
        std::memcpy(e + 40, q.patchHash.bytes.data(), 32);
        storeLE<u64>(e + 72, q.patchSize);
        e += kPatchEntrySize;
    }
    HELIOS_ASSERT(e == strings, "manifest body layout");
    return body;
}

Result<std::vector<u8>> writeManifest(const Manifest& m, const ManifestWriteOptions& options) {
    if (options.codec != ManifestCodec::None && options.codec != ManifestCodec::Zstd)
        return invalid("unknown codec {}", toUnderlying(options.codec));
    const int level = options.zstdLevel == 0 ? 19 : options.zstdLevel; // as Go: 0 is the default level
    if (options.codec == ManifestCodec::Zstd && (level < 1 || level > 19))
        return invalid("zstd level {} is not 1..19", options.zstdLevel);
    HELIOS_TRY_ASSIGN(std::vector<u8> body, encodeManifestBody(m));

    std::vector<u8> out;
    if (options.codec == ManifestCodec::None) {
        out.resize(kHeaderSize + body.size());
        std::memcpy(out.data() + kHeaderSize, body.data(), body.size());
    } else {
        out.resize(kHeaderSize + ZSTD_compressBound(body.size()));
        const usize n = ZSTD_compress(out.data() + kHeaderSize, out.size() - kHeaderSize, body.data(),
                                      body.size(), level);
        if (ZSTD_isError(n)) return makeError(ErrorCode::Unknown, "zstd: {}", ZSTD_getErrorName(n));
        out.resize(kHeaderSize + n);
    }

    RawHeader h;
    h.codec = toUnderlying(options.codec);
    h.sequence = m.header.sequence;
    h.createdAt = m.header.createdAt;
    h.expiresAt = m.header.expiresAt;
    h.compatEpoch = m.header.compatEpoch;
    h.bodySize = body.size();
    h.payloadSize = out.size() - kHeaderSize;
    h.bodyHash = blake2b256(body);
    putId(h.productId, m.header.productId);
    putId(h.platform, m.header.platform);
    putId(h.buildId, m.header.buildId);
    h.keyId = m.header.keyId;
    h.signature = m.header.signature;
    const std::span<u8, kHeaderSize> head(out.data(), kHeaderSize);
    encodeHeader(h, head);
    h.headerHash = computeHeaderHash(head);
    std::memcpy(out.data() + kHeaderHashOffset, h.headerHash.bytes.data(), 32);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------------------------

Result<ManifestHeaderInfo> readManifestHeader(std::span<const u8> file, const ManifestReadOptions& options) {
    if (file.size() < kHeaderSize) return corrupt("{} bytes is shorter than the .hman header", file.size());
    const std::span<const u8, kHeaderSize> head(file.data(), kHeaderSize);
    const RawHeader h = decodeHeader(head);
    if (h.magic != kMagic) return corrupt("not a .hman file (magic {:#010x})", h.magic);
    if (h.version != kVersion)
        return makeError(ErrorCode::VersionMismatch, ".hman version {} (this reader: {})", h.version,
                         kVersion);
    if (h.headerSize != kHeaderSize) return corrupt("header size {} is not {}", h.headerSize, kHeaderSize);
    if (computeHeaderHash(head) != h.headerHash) return corrupt("the header hash does not match");
    if (h.flags != 0) return makeError(ErrorCode::Unsupported, "unknown header flags {:#x}", h.flags);
    if (h.codec > toUnderlying(ManifestCodec::Zstd))
        return makeError(ErrorCode::Unsupported, "unknown codec {}", h.codec);
    if (!allZero(h.reserved0, 3) || h.reserved1 != 0 || !allZero(h.reserved2, 16))
        return corrupt("reserved header bytes are not zero");

    ManifestHeaderInfo info;
    const auto productId = getId(h.productId);
    const auto platform = getId(h.platform);
    const auto buildId = getId(h.buildId);
    if (!productId || !platform || !buildId) return corrupt("an identifier field has bytes after its end");
    info.header.productId = *productId;
    info.header.platform = *platform;
    info.header.buildId = *buildId;
    info.header.sequence = h.sequence;
    info.header.createdAt = h.createdAt;
    info.header.expiresAt = h.expiresAt;
    info.header.compatEpoch = h.compatEpoch;
    info.header.keyId = h.keyId;
    info.header.signature = h.signature;
    if (auto ok = validateHeaderFields(info.header); !ok) return corrupt("{}", ok.error().message);

    const u64 maxBody = effectiveMaxBody(options);
    if (h.bodySize < kBodyHeaderSize)
        return corrupt("bodySize {} is shorter than the body header", h.bodySize);
    if (h.bodySize > maxBody)
        return makeError(ErrorCode::LimitExceeded, "bodySize {} is above {}", h.bodySize, maxBody);
    if (h.payloadSize != file.size() - kHeaderSize)
        return corrupt("payloadSize {} but {} bytes follow the header", h.payloadSize,
                       file.size() - kHeaderSize);
    if (h.codec == toUnderlying(ManifestCodec::None)
            ? h.payloadSize != h.bodySize
            : (h.payloadSize == 0 || h.payloadSize > maxPayload(h.bodySize)))
        return corrupt("payloadSize {} does not fit bodySize {} for codec {}", h.payloadSize, h.bodySize,
                       h.codec);
    info.codec = static_cast<ManifestCodec>(h.codec);
    info.bodySize = h.bodySize;
    info.payloadSize = h.payloadSize;
    info.bodyHash = h.bodyHash;
    info.headerHash = h.headerHash;
    return info;
}

Result<Manifest> readManifest(std::span<const u8> file, const ManifestReadOptions& options) {
    HELIOS_TRY_ASSIGN(ManifestHeaderInfo info, readManifestHeader(file, options));
    const std::span<const u8> payload = file.subspan(kHeaderSize);
    DecodedBody decoded;
    std::span<const u8> body = payload;
    if (info.codec == ManifestCodec::Zstd) {
        HELIOS_TRY_ASSIGN(decoded, decompress(payload, info.bodySize));
        body = decoded.bytes();
    }
    if (blake2b256(body) != info.bodyHash) return corrupt("the body hash does not match");
    Manifest m;
    m.header = std::move(info.header);
    HELIOS_TRY(decodeBody(body, m));
    if (auto ok = validateManifest(m); !ok) {
        if (ok.errorCode() == ErrorCode::LimitExceeded) return std::move(ok).error();
        return corrupt("{}", ok.error().message);
    }
    return m;
}

Result<Manifest> readManifestFile(const fs::Path& path, const ManifestReadOptions& options) {
    HELIOS_TRY_ASSIGN(fs::File f, fs::File::open(path, fs::OpenMode::Read));
    HELIOS_TRY_ASSIGN(const u64 size, f.size());
    const u64 maxBody = effectiveMaxBody(options);
    if (size > kHeaderSize + maxPayload(maxBody))
        return makeError(ErrorCode::LimitExceeded,
                         "'{}': {} bytes is larger than any manifest this reader accepts",
                         fs::pathToUtf8(path), size);
    std::vector<u8> bytes(static_cast<usize>(size));
    HELIOS_TRY(f.readExact(bytes.data(), bytes.size()));
    auto m = readManifest(bytes, options);
    if (!m) return Error{m.errorCode(), std::format("'{}': {}", fs::pathToUtf8(path), m.error().message)};
    return m;
}

// ---------------------------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------------------------

Result<void> ManifestBuilder::addFile(ManifestFileInput file) {
    if (!isValidManifestPath(file.path)) return invalid("'{}' is not a valid manifest path", file.path);
    if (file.tier > kMaxTier) return invalid("'{}': tier {} is not 0, 1 or 2", file.path, file.tier);
    if ((toUnderlying(file.flags) & ~kManifestFileFlagsKnown) != 0)
        return invalid("'{}': unknown flags {:#x}", file.path, toUnderlying(file.flags));
    u64 offset = 0;
    for (const Chunk& c : file.content.chunks) {
        if (c.offset != offset || c.size == 0 || c.size > fastcdc::kMaxSize)
            return invalid("'{}': chunk at {} ({} bytes) does not continue at {}", file.path, c.offset,
                           c.size, offset);
        offset += c.size;
    }
    if (offset != file.content.size)
        return invalid("'{}': chunks cover {} bytes of {}", file.path, offset, file.content.size);
    m_files.push_back(std::move(file));
    return {};
}

void ManifestBuilder::addPack(const Hash256& hash, u64 size) { m_packs.push_back(ManifestPack{hash, size}); }

void ManifestBuilder::placeChunk(const Hash256& chunk, const Hash256& pack, u64 offset, u32 storedSize) {
    m_placements.push_back(Placement{chunk, pack, offset, storedSize, true});
}

void ManifestBuilder::setStoredSize(const Hash256& chunk, u32 storedSize) {
    m_placements.push_back(Placement{chunk, Hash256{}, 0, storedSize, false});
}

void ManifestBuilder::addPatch(std::string path, const Hash256& fromHash, const Hash256& patchHash,
                               u64 patchSize) {
    m_patches.push_back(PendingPatch{std::move(path), fromHash, patchHash, patchSize});
}

Result<Manifest> ManifestBuilder::build() const {
    Manifest m;
    m.header = m_header;
    std::vector<const ManifestFileInput*> order;
    order.reserve(m_files.size());
    for (const ManifestFileInput& f : m_files) order.push_back(&f);
    std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->path < b->path; });
    for (usize i = 1; i < order.size(); ++i)
        if (order[i - 1]->path == order[i]->path) return invalid("'{}' is added twice", order[i]->path);

    // Distinct chunks by ID; one ID with two sizes is a collision or a broken input.
    std::vector<ManifestChunk> all;
    for (const ManifestFileInput* f : order)
        for (const Chunk& c : f->content.chunks) all.push_back(ManifestChunk{c.hash, c.size, 0, kNoPack, 0});
    std::sort(all.begin(), all.end(), [](const ManifestChunk& a, const ManifestChunk& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.rawSize < b.rawSize;
    });
    for (const ManifestChunk& c : all) {
        if (!m.chunks.empty() && m.chunks.back().hash == c.hash) {
            if (m.chunks.back().rawSize != c.rawSize)
                return invalid("chunk {} has sizes {} and {}", c.hash.toHex(), m.chunks.back().rawSize,
                               c.rawSize);
            continue;
        }
        m.chunks.push_back(c);
    }
    for (const ManifestFileInput* f : order) {
        ManifestFile mf;
        mf.path = f->path;
        mf.size = f->content.size;
        mf.hash = f->content.hash;
        mf.firstRef = static_cast<u32>(std::min<usize>(m.refs.size(), kMaxRefs));
        mf.refCount = static_cast<u32>(std::min<usize>(f->content.chunks.size(), kMaxRefs));
        mf.group = f->group;
        mf.language = f->language;
        mf.tier = f->tier;
        mf.flags = f->flags;
        for (const Chunk& c : f->content.chunks)
            m.refs.push_back(ManifestChunkRef{*m.findChunk(c.hash), c.offset});
        m.files.push_back(std::move(mf));
    }

    m.packs = m_packs;
    std::sort(m.packs.begin(), m.packs.end(),
              [](const ManifestPack& a, const ManifestPack& b) { return a.hash < b.hash; });
    for (usize i = 1; i < m.packs.size(); ++i)
        if (m.packs[i - 1].hash == m.packs[i].hash)
            return invalid("pack {} is added twice", m.packs[i].hash.toHex());
    std::vector<bool> placed(m.chunks.size(), false);
    for (const Placement& p : m_placements) {
        const std::optional<u32> chunk = m.findChunk(p.chunk);
        if (!chunk) return invalid("chunk {} is placed but belongs to no file", p.chunk.toHex());
        if (placed[*chunk]) return invalid("chunk {} is placed twice", p.chunk.toHex());
        placed[*chunk] = true;
        ManifestChunk& c = m.chunks[*chunk];
        c.storedSize = p.storedSize;
        if (p.packed) {
            const auto it =
                std::lower_bound(m.packs.begin(), m.packs.end(), p.pack,
                                 [](const ManifestPack& k, const Hash256& h) { return k.hash < h; });
            if (it == m.packs.end() || it->hash != p.pack)
                return invalid("pack {} is not declared", p.pack.toHex());
            c.pack = static_cast<u32>(it - m.packs.begin());
            c.packOffset = p.offset;
        }
    }
    for (const PendingPatch& q : m_patches) {
        const ManifestFile* f = m.findFile(q.path);
        if (!f) return invalid("a patch names '{}', which is not a file", q.path);
        m.patches.push_back(
            ManifestPatch{static_cast<u32>(f - m.files.data()), q.fromHash, q.patchHash, q.patchSize});
    }
    std::sort(m.patches.begin(), m.patches.end(), patchLess);
    HELIOS_TRY(validateManifest(m));
    return m;
}

} // namespace helios::patch
