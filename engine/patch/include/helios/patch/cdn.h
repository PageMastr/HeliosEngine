#pragma once
// The CDN layout of 05 §7 and the client-side read path over it: object paths, a local-directory fetcher,
// chunk objects (one zstd frame each) and verifyChannel(), which runs the whole trust chain (trust.h) for a
// product, channel and platform. Go's services/pkg/patchcdn writes this layout (`helios-patch publish`)
// and reads it the same way.
//
//   chunks/<aa>/<bb>/<blake2b-hex>.zst                 immutable
//   manifests/<product>/<build-id>/<platform>.hman     immutable, signed
//   channels/<product>/<channel>/<platform>.json       the signed pointer
//   keys/<product>/keyset.json                         the root-signed keyset
//
// Packs and patches are not read in v0 (publish stores every chunk loose); the launcher's HTTP fetchers,
// planner and install.db are WP-0.17.
//
// Threading: pure functions; a CdnFetch is called from the calling thread only.

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/patch/fastcdc.h"
#include "helios/patch/manifest.h"
#include "helios/patch/trust.h"

namespace helios::patch {

namespace cdn {

/// The largest chunk object: zstd's bound for a kMaxSize chunk fits.
inline constexpr u64 kMaxChunkObject = fastcdc::kMaxSize + 4096;
/// The largest zstd window a chunk object may declare: 2^18 = fastcdc::kMaxSize. Go's EncodeChunk declares
/// at most max(content size, 1 KiB), so every object publish writes fits, and a hostile one cannot make the
/// decoder reserve a larger window.
inline constexpr int kMaxChunkWindowLog = 18;
static_assert(u64(1) << kMaxChunkWindowLog == fastcdc::kMaxSize);
/// The largest .hman object: the header, the largest body and zstd's worst-case expansion.
inline constexpr u64 kMaxManifestObject =
    hman::kHeaderSize + hman::kMaxBodySize + hman::kMaxBodySize / 128 + 4096;

std::string keysetPath(std::string_view product);
std::string pointerPath(std::string_view product, std::string_view channel, std::string_view platform);
std::string manifestPath(std::string_view product, std::string_view build, std::string_view platform);
/// chunks/<aa>/<bb>/<64 hex digits>.zst
std::string chunkPath(const Hash256& chunk);

/// True for a relative '/'-separated object path without empty, "." or ".." segments, '\' or ':'.
bool isValidObjectPath(std::string_view path) noexcept;

} // namespace cdn

/// Reads a CDN object by layout path, failing with NotFound if it does not exist and LimitExceeded if it is
/// larger than `limit` bytes.
using CdnFetch = std::function<Result<std::vector<u8>>(std::string_view path, u64 limit)>;

/// A CdnFetch over a CDN directory, through the platform layer (core fs).
CdnFetch localCdn(fs::Path root);

/// Decodes a chunk object that must hold exactly `rawSize` bytes; output is bounded by rawSize + 1 bytes and
/// the zstd window by cdn::kMaxChunkWindowLog (256 KiB), so a hostile object cannot make it allocate more.
/// Corrupt otherwise.
Result<std::vector<u8>> decodeChunkObject(std::span<const u8> stored, u32 rawSize);

/// Fetches a loose chunk and checks it: chunk-missing, chunk-corrupt (stored size, decoding), chunk-hash.
Result<std::vector<u8>> fetchChunk(const CdnFetch& fetch, const ManifestChunk& chunk);

/// What verifyChannel() accepted.
struct VerifiedChannel {
    Keyset keyset;
    Pointer pointer;
    Manifest manifest;
    TrustState state; ///< After accepting the keyset and pointer (saved to the store).
    u64 chunks = 0;   ///< Chunk objects checked.
    u64 rawBytes = 0; ///< Their decoded bytes.
};

/// The client-side check of a channel (05 §7 launcher step 2, 08 §2.5 step 1), in order: the keyset
/// against the root pair, the pointer against the keyset, the manifest the pointer names, then every
/// chunk. The ratchets are loaded from `store` and, once the manifest verifies, saved advanced (before the
/// chunks, which an install fetches over hours). Rejections carry their TrustCheck (trust.h); a missing
/// keyset, pointer or manifest fails with NotFound, and a packed chunk with Unsupported.
Result<VerifiedChannel> verifyChannel(const CdnFetch& fetch, const TrustVerifier& verifier, u64 now,
                                      TrustStateStore& store);

} // namespace helios::patch
