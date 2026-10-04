#pragma once
// FastCDC content-defined chunking (05 §7, 08 §2.5): normalized chunking at level 2 with 05 §7's sizes
// (min 16 KiB, average 64 KiB, max 256 KiB) and BLAKE2b-256 chunk IDs. Go's services/pkg/cdc is the same
// algorithm; the shared vectors in services/testdata/vectors/fastcdc.json pin both (engine/patch/README.md
// has the full definition):
//
//   - gear table: the first 256 outputs of SplitMix64 seeded with kGearSeed;
//   - rolling hash: fp = (fp << 1) + gear[byte] (mod 2^64), from zero at the min-size cut-point skip;
//   - a cut follows the first byte at which (fp & kMaskS) == 0 before kAvgSize, or (fp & kMaskL) == 0
//     from kAvgSize on; the masks are the top 18 and 14 bits (normalization level 2 around 2^16);
//   - no chunk exceeds kMaxSize; only a stream's last chunk may be kMinSize bytes or shorter.
//
// A boundary depends only on the bytes from the chunk's start to at most kMaxSize past it, so a stream
// chunked piecewise (StreamChunker) gives exactly the chunks of the whole buffer (splitBuffer).
//
// Budget (README "Performance"): boundary detection ≥ 1 GB/s per core (R07 §7: FastCDC "> 1 GB/s/core"),
// and chunking with BLAKE2b-256 IDs ≥ 300 MB/s per core, in optimized builds (`perf:` cases).
//
// Threading: the free functions are pure and thread-safe; a StreamChunker belongs to one thread.

#include <array>
#include <span>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/patch/blake2b.h"

namespace helios::patch {

namespace fastcdc {

inline constexpr u32 kMinSize = 16 * 1024;  ///< No cut before this many bytes (cut-point skipping).
inline constexpr u32 kAvgSize = 64 * 1024;  ///< The normal size: kMaskS before it, kMaskL from it on.
inline constexpr u32 kMaxSize = 256 * 1024; ///< A chunk never exceeds this.
inline constexpr u64 kMaskS = 0xFFFFC00000000000ull; ///< Top 18 bits: harder to match before kAvgSize.
inline constexpr u64 kMaskL = 0xFFFC000000000000ull; ///< Top 14 bits: easier to match from kAvgSize on.
/// Seeds the SplitMix64 stream whose first 256 outputs are the gear table ("FASTCDC" in ASCII).
inline constexpr u64 kGearSeed = 0x0046415354434443ull;

/// The 256-entry gear table.
const std::array<u64, 256>& gearTable() noexcept;

/// Length of the chunk that starts at data[0]. `data` must hold kMaxSize bytes or run to the end of the
/// input; the result is then the one for the whole input. 0 for empty data.
usize cut(std::span<const u8> data) noexcept;

} // namespace fastcdc

/// One chunk: where it starts in its file, its length (1..kMaxSize) and its ID.
struct Chunk {
    u64 offset = 0;
    u32 size = 0;
    Hash256 hash;
    friend bool operator==(const Chunk&, const Chunk&) = default;
};

/// A whole input's chunking: its size, its BLAKE2b-256 and its chunks in order.
struct ChunkedFile {
    u64 size = 0;
    Hash256 hash;
    std::vector<Chunk> chunks;
    friend bool operator==(const ChunkedFile&, const ChunkedFile&) = default;
};

/// Chunk lengths of `data`, in order (none for empty data). No hashing.
std::vector<u32> chunkBoundaries(std::span<const u8> data);

/// Chunks `data` and hashes every chunk and the whole input.
ChunkedFile splitBuffer(std::span<const u8> data);

/// Chunks a stream fed piecewise. It buffers at most 2 * kMaxSize bytes and emits a chunk as soon as
/// kMaxSize bytes past its start are known. Not thread-safe.
class StreamChunker {
public:
    StreamChunker();
    /// Absorbs `data`; appends every chunk that is now final to `out`.
    void update(std::span<const u8> data, std::vector<Chunk>& out);
    /// Ends the stream: appends the remaining chunks to `out` and returns the stream's size and hash.
    /// The chunker then starts a new stream.
    ChunkedFile finish(std::vector<Chunk>& out);
    /// Bytes absorbed so far in this stream.
    u64 bytesIn() const noexcept { return m_offset + (m_end - m_start); }

private:
    void emit(usize length, std::vector<Chunk>& out);

    std::vector<u8> m_buffer;
    usize m_start = 0; ///< First byte not yet emitted.
    usize m_end = 0;   ///< One past the last byte absorbed.
    u64 m_offset = 0;  ///< Stream offset of m_buffer[m_start].
    Blake2b256 m_whole;
};

/// Chunks the file at `path`, read sequentially through the platform layer (fs::File). Fails with the
/// read's error (IoError, NotFound, PermissionDenied...).
Result<ChunkedFile> chunkFile(const fs::Path& path);

} // namespace helios::patch
