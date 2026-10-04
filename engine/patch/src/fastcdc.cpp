// FastCDC (normalized chunking, level 2) with 05 §7's sizes; see fastcdc.h and engine/patch/README.md.
#include "helios/patch/fastcdc.h"

#include <algorithm>
#include <cstring>

#include "helios/core/random.h"

namespace helios::patch {

namespace fastcdc {

namespace {
constexpr std::array<u64, 256> makeGear() noexcept {
    std::array<u64, 256> g{};
    SplitMix64 rng(kGearSeed);
    for (u64& v : g) v = rng.next();
    return g;
}
constexpr std::array<u64, 256> kGear = makeGear();

static_assert(kMinSize < kAvgSize && kAvgSize < kMaxSize);
static_assert(std::popcount(kMaskS) == 18 && std::popcount(kMaskL) == 14, "NC-2 around 2^16");
} // namespace

const std::array<u64, 256>& gearTable() noexcept { return kGear; }

usize cut(std::span<const u8> data) noexcept {
    usize n = data.size();
    if (n <= kMinSize) return n;
    if (n > kMaxSize) n = kMaxSize;
    const usize normal = std::min<usize>(kAvgSize, n);
    const u8* p = data.data();
    u64 fp = 0;
    usize i = kMinSize;
    for (; i < normal; ++i) {
        fp = (fp << 1) + kGear[p[i]];
        if ((fp & kMaskS) == 0) return i + 1;
    }
    for (; i < n; ++i) {
        fp = (fp << 1) + kGear[p[i]];
        if ((fp & kMaskL) == 0) return i + 1;
    }
    return n;
}

} // namespace fastcdc

std::vector<u32> chunkBoundaries(std::span<const u8> data) {
    std::vector<u32> out;
    while (!data.empty()) {
        const usize n = fastcdc::cut(data);
        out.push_back(static_cast<u32>(n));
        data = data.subspan(n);
    }
    return out;
}

ChunkedFile splitBuffer(std::span<const u8> data) {
    ChunkedFile file;
    file.size = data.size();
    file.hash = blake2b256(data);
    file.chunks.reserve(data.size() / fastcdc::kAvgSize + 1);
    u64 offset = 0;
    while (!data.empty()) {
        const usize n = fastcdc::cut(data);
        file.chunks.push_back(Chunk{offset, static_cast<u32>(n), blake2b256(data.first(n))});
        offset += n;
        data = data.subspan(n);
    }
    return file;
}

StreamChunker::StreamChunker() : m_buffer(2 * usize(fastcdc::kMaxSize)) {}

void StreamChunker::emit(usize length, std::vector<Chunk>& out) {
    const std::span<const u8> bytes(m_buffer.data() + m_start, length);
    out.push_back(Chunk{m_offset, static_cast<u32>(length), blake2b256(bytes)});
    m_whole.update(bytes);
    m_start += length;
    m_offset += length;
}

void StreamChunker::update(std::span<const u8> data, std::vector<Chunk>& out) {
    while (!data.empty()) {
        if (m_end == m_buffer.size()) { // full: move the pending bytes (< kMaxSize of them) to the front
            std::memmove(m_buffer.data(), m_buffer.data() + m_start, m_end - m_start);
            m_end -= m_start;
            m_start = 0;
        }
        const usize take = std::min(data.size(), m_buffer.size() - m_end);
        std::memcpy(m_buffer.data() + m_end, data.data(), take);
        m_end += take;
        data = data.subspan(take);
        while (m_end - m_start >= fastcdc::kMaxSize)
            emit(fastcdc::cut(std::span<const u8>(m_buffer.data() + m_start, m_end - m_start)), out);
    }
}

ChunkedFile StreamChunker::finish(std::vector<Chunk>& out) {
    while (m_start < m_end)
        emit(fastcdc::cut(std::span<const u8>(m_buffer.data() + m_start, m_end - m_start)), out);
    ChunkedFile file;
    file.size = m_offset;
    file.hash = m_whole.finish();
    m_start = m_end = 0;
    m_offset = 0;
    return file;
}

Result<ChunkedFile> chunkFile(const fs::Path& path) {
    HELIOS_TRY_ASSIGN(fs::File f, fs::File::open(path, fs::OpenMode::Read));
    StreamChunker chunker;
    std::vector<Chunk> chunks;
    std::vector<u8> block(usize(1) * kMiB);
    for (;;) {
        HELIOS_TRY_ASSIGN(const usize got, f.read(block.data(), block.size()));
        if (got == 0) break;
        chunker.update(std::span<const u8>(block.data(), got), chunks);
    }
    ChunkedFile file = chunker.finish(chunks);
    file.chunks = std::move(chunks);
    return file;
}

} // namespace helios::patch
