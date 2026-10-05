// The CDN layout and the client-side read path (cdn.h). Go's services/pkg/patchcdn is the twin.
#include "helios/patch/cdn.h"

#include <zstd.h>

#include <format>
#include <memory>

namespace helios::patch {

namespace cdn {

std::string keysetPath(std::string_view product) { return std::format("keys/{}/keyset.json", product); }

std::string pointerPath(std::string_view product, std::string_view channel, std::string_view platform) {
    return std::format("channels/{}/{}/{}.json", product, channel, platform);
}

std::string manifestPath(std::string_view product, std::string_view build, std::string_view platform) {
    return std::format("manifests/{}/{}/{}.hman", product, build, platform);
}

std::string chunkPath(const Hash256& chunk) {
    const std::string hex = chunk.toHex();
    return std::format("chunks/{}/{}/{}.zst", hex.substr(0, 2), hex.substr(2, 2), hex);
}

bool isValidObjectPath(std::string_view path) noexcept {
    usize start = 0;
    for (usize i = 0; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            if (path[i] == '\\' || path[i] == ':' || path[i] == '\0') return false;
            continue;
        }
        const std::string_view seg = path.substr(start, i - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        start = i + 1;
    }
    return true;
}

} // namespace cdn

CdnFetch localCdn(fs::Path root) {
    return [root = std::move(root)](std::string_view path, u64 limit) -> Result<std::vector<u8>> {
        if (!cdn::isValidObjectPath(path)) return makeError(ErrorCode::InvalidArgument, "bad object path \"{}\"", path);
        const fs::Path full = root / fs::pathFromUtf8(path);
        if (!fs::isFile(full)) return makeError(ErrorCode::NotFound, "{}: not found", path);
        HELIOS_TRY_ASSIGN(const u64 size, fs::fileSize(full));
        if (size > limit) return makeError(ErrorCode::LimitExceeded, "{} is larger than {} bytes", path, limit);
        HELIOS_TRY_ASSIGN(std::vector<u8> bytes, fs::readFile(full));
        if (bytes.size() > limit) return makeError(ErrorCode::LimitExceeded, "{} is larger than {} bytes", path, limit);
        return bytes;
    };
}

Result<std::vector<u8>> decodeChunkObject(std::span<const u8> stored, u32 rawSize) {
    struct FreeDCtx {
        void operator()(ZSTD_DCtx* d) const noexcept { ZSTD_freeDCtx(d); }
    };
    const std::unique_ptr<ZSTD_DCtx, FreeDCtx> d(ZSTD_createDCtx());
    if (!d) return Error{ErrorCode::OutOfMemory, "ZSTD_createDCtx failed"};
    if (ZSTD_isError(ZSTD_DCtx_setParameter(d.get(), ZSTD_d_windowLogMax, hman::kMaxZstdWindowLog)))
        return Error{ErrorCode::Unknown, "ZSTD_d_windowLogMax refused"};
    // One byte of room past rawSize catches an object that decodes to more.
    std::vector<u8> out(usize(rawSize) + 1);
    ZSTD_inBuffer in{stored.data(), stored.size(), 0};
    ZSTD_outBuffer o{out.data(), out.size(), 0};
    for (;;) {
        const usize before = in.pos;
        const usize producedBefore = o.pos;
        const usize ret = ZSTD_decompressStream(d.get(), &o, &in);
        if (ZSTD_isError(ret)) return makeError(ErrorCode::Corrupt, "zstd: {}", ZSTD_getErrorName(ret));
        if (o.pos > rawSize) return makeError(ErrorCode::Corrupt, "decodes to more than {} bytes", rawSize);
        if (in.pos == in.size) {
            if (ret != 0) return Error{ErrorCode::Corrupt, "the object ends inside a zstd frame"};
            break;
        }
        if (in.pos == before && o.pos == producedBefore) return Error{ErrorCode::Corrupt, "the object does not decode"};
    }
    if (o.pos != rawSize) return makeError(ErrorCode::Corrupt, "decodes to {} bytes, expected {}", o.pos, rawSize);
    out.resize(rawSize);
    return out;
}

namespace {
Error rejection(TrustCheck check, std::string detail) {
    return Error{check == TrustCheck::ChunkMissing ? ErrorCode::NotFound : ErrorCode::Corrupt,
                 std::format("{}: {}", trustCheckName(check), detail)};
}
} // namespace

Result<std::vector<u8>> fetchChunk(const CdnFetch& fetch, const ManifestChunk& chunk) {
    Result<std::vector<u8>> stored = fetch(cdn::chunkPath(chunk.hash), cdn::kMaxChunkObject);
    if (!stored) {
        if (stored.errorCode() == ErrorCode::NotFound) return rejection(TrustCheck::ChunkMissing, stored.error().message);
        return makeError(stored.errorCode(), "chunk {}: {}", chunk.hash.toHex(), stored.error().message);
    }
    if (chunk.storedSize != 0 && stored->size() != chunk.storedSize)
        return rejection(TrustCheck::ChunkCorrupt, std::format("chunk {} is stored in {} bytes, the manifest says {}",
                                                               chunk.hash.toHex(), stored->size(), chunk.storedSize));
    Result<std::vector<u8>> raw = decodeChunkObject(*stored, chunk.rawSize);
    if (!raw)
        return rejection(TrustCheck::ChunkCorrupt, std::format("chunk {}: {}", chunk.hash.toHex(), raw.error().message));
    HELIOS_TRY(TrustVerifier::verifyChunk(chunk, *raw));
    return raw;
}

Result<VerifiedChannel> verifyChannel(const CdnFetch& fetch, const TrustVerifier& verifier, u64 now,
                                      TrustStateStore& store) {
    HELIOS_TRY_ASSIGN(const TrustState state, store.load());
    const TrustTarget& t = verifier.target();
    VerifiedChannel out;
    {
        HELIOS_TRY_ASSIGN(const std::vector<u8> doc, fetch(cdn::keysetPath(t.productId), kMaxKeysetSize));
        HELIOS_TRY_ASSIGN(out.keyset, verifier.verifyKeyset(doc, state));
    }
    {
        HELIOS_TRY_ASSIGN(const std::vector<u8> doc,
                          fetch(cdn::pointerPath(t.productId, t.channel, t.platform), kMaxPointerSize));
        HELIOS_TRY_ASSIGN(out.pointer, verifier.verifyPointer(doc, out.keyset, now, state));
    }
    {
        HELIOS_TRY_ASSIGN(const std::vector<u8> file, fetch(cdn::manifestPath(t.productId, out.pointer.ref.buildId,
                                                                                t.platform),
                                                              cdn::kMaxManifestObject));
        HELIOS_TRY_ASSIGN(out.manifest, verifier.verifyManifest(file, out.keyset, out.pointer.ref, now));
    }
    out.state = advanceTrustState(state, out.keyset, out.pointer);
    HELIOS_TRY(store.save(out.state));
    for (const ManifestChunk& c : out.manifest.chunks) {
        if (c.pack != hman::kNoPack)
            return makeError(ErrorCode::Unsupported, "chunk {} is packed; packs are not supported in v0", c.hash.toHex());
        HELIOS_TRY_ASSIGN(const std::vector<u8> raw, fetchChunk(fetch, c));
        ++out.chunks;
        out.rawBytes += raw.size();
    }
    return out;
}

} // namespace helios::patch
