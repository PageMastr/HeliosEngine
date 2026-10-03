// `.hpak` v0 reader: header and TOC validation, block verification on first read, zstd decoding.

#include "helios/asset/hpak_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

#include <zstd.h>

#include "helios/core/hash.h"
#include "helios/core/log.h"

namespace helios::asset {
namespace {

using namespace hpak;

constexpr u64 kMaxStoredBlock = ZSTD_COMPRESSBOUND(kAssetBlockSize);

class FileSource final : public IHpakSource {
public:
    FileSource(fs::File file, u64 size) : m_file(std::move(file)), m_size(size), m_name(fs::pathToGenericUtf8(m_file.path())) {}

    u64 size() const override { return m_size; }

    Result<void> readAt(u64 offset, std::span<u8> out) const override {
        if (offset > m_size || out.size() > m_size - offset)
            return makeError(ErrorCode::EndOfFile, "'{}': read of {} bytes at {} runs past the end ({} bytes)", m_name,
                             out.size(), offset, m_size);
        usize done = 0;
        while (done < out.size()) {
            HELIOS_TRY_ASSIGN(const usize got, m_file.readAt(offset + done, out.data() + done, out.size() - done));
            if (got == 0)
                return makeError(ErrorCode::EndOfFile, "'{}': unexpected end of file at {}", m_name, offset + done);
            done += got;
        }
        return {};
    }

    std::string describe() const override { return m_name; }

private:
    fs::File m_file;
    u64 m_size;
    std::string m_name;
};

class MemorySource final : public IHpakSource {
public:
    MemorySource(std::vector<u8> bytes, std::string name) : m_bytes(std::move(bytes)), m_name("memory:" + std::move(name)) {}

    u64 size() const override { return m_bytes.size(); }

    Result<void> readAt(u64 offset, std::span<u8> out) const override {
        if (offset > m_bytes.size() || out.size() > m_bytes.size() - offset)
            return makeError(ErrorCode::EndOfFile, "'{}': read of {} bytes at {} runs past the end ({} bytes)", m_name,
                             out.size(), offset, m_bytes.size());
        if (!out.empty()) std::memcpy(out.data(), m_bytes.data() + offset, out.size());
        return {};
    }

    std::string describe() const override { return m_name; }

private:
    std::vector<u8> m_bytes;
    std::string m_name;
};

/// One decompression context per thread, kept for the thread's lifetime. Single-shot decoding keeps
/// no state between calls, so a refetch hook that reads another pak on this thread cannot disturb it.
struct DCtxHolder {
    ZSTD_DCtx* ctx = nullptr;
    ~DCtxHolder() { ZSTD_freeDCtx(ctx); }
};

ZSTD_DCtx* threadDCtx() noexcept {
    thread_local DCtxHolder holder;
    if (!holder.ctx) holder.ctx = ZSTD_createDCtx();
    return holder.ctx;
}

bool allZero(const u8* p, usize n) noexcept {
    return std::all_of(p, p + n, [](u8 b) { return b == 0; });
}

} // namespace

Result<std::unique_ptr<IHpakSource>> openHpakFile(const fs::Path& path) {
    HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(path, fs::OpenMode::Read));
    HELIOS_TRY_ASSIGN(const u64 size, file.size());
    return std::unique_ptr<IHpakSource>(std::make_unique<FileSource>(std::move(file), size));
}

std::unique_ptr<IHpakSource> makeHpakMemorySource(std::vector<u8> bytes, std::string name) {
    return std::make_unique<MemorySource>(std::move(bytes), std::move(name));
}

HpakReader::~HpakReader() = default;

Result<std::shared_ptr<HpakReader>> HpakReader::open(std::unique_ptr<IHpakSource> source,
                                                     const HpakOpenOptions& options) {
    if (!source) return Error{ErrorCode::InvalidArgument, "HpakReader::open: null source"};
    std::shared_ptr<HpakReader> reader(new HpakReader());
    reader->m_name = source->describe();
    reader->m_source = std::move(source);
    reader->m_refetcher = options.refetcher;
    HELIOS_TRY(reader->parse(options));
    return reader;
}

Result<std::shared_ptr<HpakReader>> HpakReader::openFile(const fs::Path& path, const HpakOpenOptions& options) {
    HELIOS_TRY_ASSIGN(std::unique_ptr<IHpakSource> source, openHpakFile(path));
    return open(std::move(source), options);
}

Result<void> HpakReader::parse(const HpakOpenOptions& options) {
    const auto corrupt = [this](std::string what) {
        return Error{ErrorCode::Corrupt, std::format("'{}': {}", m_name, what)};
    };
    const u64 fileSize = m_source->size();
    if (fileSize < kHeaderBlockSize)
        return corrupt(std::format("{} bytes is shorter than the {}-byte header block", fileSize, kHeaderBlockSize));
    if (fileSize > kMaxPakSize)
        return makeError(ErrorCode::LimitExceeded, "'{}': {} bytes is above the 2 GiB .hpak limit (02 §6.3)", m_name,
                         fileSize);

    std::vector<u8> block(kHeaderBlockSize);
    HELIOS_TRY(m_source->readAt(0, block));
    const std::span<const u8, kHeaderBytes> fields(block.data(), kHeaderBytes);
    const Header h = decodeHeader(fields);
    if (h.magic != kMagic) return corrupt("not an .hpak file (bad magic)");
    if (h.version != kVersion)
        return makeError(ErrorCode::VersionMismatch, "'{}': .hpak format version {}, this reader reads {}", m_name,
                         h.version, kVersion);
    if (computeHeaderHash(fields) != h.headerHash) return corrupt("header checksum mismatch");
    if (h.reserved0 != 0 || h.flags != 0 || h.reserved2 != 0 || !allZero(h.reserved1, sizeof(h.reserved1)) ||
        !allZero(block.data() + kHeaderBytes, block.size() - kHeaderBytes))
        return corrupt("reserved header bytes or flags are not zero");
    if (h.platform < toUnderlying(HpakPlatform::PcClient) || h.platform > toUnderlying(HpakPlatform::Editor))
        return corrupt(std::format("unknown platform {}", h.platform));
    const auto platform = static_cast<HpakPlatform>(h.platform);
    if (options.platform && *options.platform != platform)
        return makeError(ErrorCode::Unsupported, "'{}': cooked for {}, expected {}", m_name, hpakPlatformName(platform),
                         hpakPlatformName(*options.platform));
    if (h.tags.tier > kMaxTier) return corrupt(std::format("tier {} (tiers are 0..{})", h.tags.tier, kMaxTier));

    // Layout: [header block][blob region][TOC], the TOC 4 KiB aligned and ending the file.
    if (h.tocOffset < kHeaderBlockSize || h.tocOffset > fileSize || h.tocOffset % kBlobAlignment != 0)
        return corrupt(std::format("TOC offset {} is not a 4 KiB aligned offset in [{}, {}]", h.tocOffset,
                                   kHeaderBlockSize, fileSize));
    if (h.tocSize != fileSize - h.tocOffset)
        return corrupt(std::format("TOC [{}, +{}) does not end the file ({} bytes)", h.tocOffset, h.tocSize, fileSize));
    const u64 dataEnd = h.tocOffset;
    if (h.pakBlockCount != hpak::pakBlockCount(dataEnd - kHeaderBlockSize))
        return corrupt(std::format("{} pak block checksums for a {}-byte blob region", h.pakBlockCount,
                                   dataEnd - kHeaderBlockSize));
    if (h.tocSize != hpak::tocSize(h.assetCount, h.assetBlockCount, h.pakBlockCount))
        return corrupt(std::format("TOC is {} bytes, its counts ({} assets, {} asset blocks, {} pak blocks) need {}",
                                   h.tocSize, h.assetCount, h.assetBlockCount, h.pakBlockCount,
                                   hpak::tocSize(h.assetCount, h.assetBlockCount, h.pakBlockCount)));

    // The TOC is read and checked whole; its size is bounded by the file's.
    std::vector<u8> toc(static_cast<usize>(h.tocSize));
    HELIOS_TRY(m_source->readAt(h.tocOffset, toc));
    if (hash64(toc.data(), toc.size()) != h.tocHash) return corrupt("TOC checksum mismatch");

    const u8* entryBytes = toc.data();
    const u8* sizeBytes = entryBytes + u64(h.assetCount) * kTocEntryBytes;
    const u8* hashBytes = sizeBytes + u64(h.assetBlockCount) * kBlockSizeBytes;
    m_blockSizes.resize(h.assetBlockCount);
    for (u32 i = 0; i < h.assetBlockCount; ++i) m_blockSizes[i] = loadLE<u32>(sizeBytes + u64(i) * kBlockSizeBytes);
    m_blockHashes.resize(h.pakBlockCount);
    for (u32 i = 0; i < h.pakBlockCount; ++i) m_blockHashes[i] = loadLE<u64>(hashBytes + u64(i) * kBlockHashBytes);

    m_entries.resize(h.assetCount);
    u64 nextBlock = 0;
    for (u32 i = 0; i < h.assetCount; ++i) {
        const RawEntry raw = decodeEntry(std::span<const u8, kTocEntryBytes>(entryBytes + u64(i) * kTocEntryBytes,
                                                                             kTocEntryBytes));
        const HpakEntry& e = raw.entry;
        const auto bad = [&](std::string what) { return corrupt(std::format("TOC entry {} ({}): {}", i, e.id, what)); };
        if (!e.id.isValid()) return bad("AssetId 0 is reserved");
        if (i > 0 && e.id <= m_entries[i - 1].id) return bad("ids are not strictly ascending");
        if (raw.codec > toUnderlying(HpakCodec::Zstd)) return bad(std::format("unknown codec {}", raw.codec));
        if (!allZero(raw.reserved, sizeof(raw.reserved))) return bad("reserved bytes are not zero");
        if (e.rawSize > kMaxAssetSize) return bad(std::format("decoded size {} is above the 2 GiB asset limit", e.rawSize));
        if (e.blockCount != assetBlockCount(e.rawSize))
            return bad(std::format("{} blocks for {} bytes", e.blockCount, e.rawSize));
        if (e.firstBlock != nextBlock) return bad(std::format("first block {}, expected {}", e.firstBlock, nextBlock));
        nextBlock += e.blockCount;
        if (nextBlock > h.assetBlockCount) return bad("its blocks run past the block-size table");
        if (e.offset < kHeaderBlockSize || e.offset > dataEnd || e.offset % kBlobAlignment != 0)
            return bad(std::format("blob offset {} is not a 4 KiB aligned offset in the blob region", e.offset));
        if (e.compSize > dataEnd - e.offset)
            return bad(std::format("blob [{}, +{}) runs past the blob region (ends at {})", e.offset, e.compSize, dataEnd));
        u64 stored = 0;
        for (u32 k = 0; k < e.blockCount; ++k) {
            const u32 size = m_blockSizes[e.firstBlock + k];
            const u64 rawLen = std::min(kAssetBlockSize, e.rawSize - u64(k) * kAssetBlockSize);
            if (raw.codec == toUnderlying(HpakCodec::None) ? size != rawLen : (size == 0 || size > kMaxStoredBlock))
                return bad(std::format("block {} stores {} bytes for {} raw bytes", k, size, rawLen));
            stored += size;
        }
        if (stored != e.compSize) return bad(std::format("blocks store {} bytes, the blob is {}", stored, e.compSize));
        m_entries[i] = e;
    }
    if (nextBlock != h.assetBlockCount)
        return corrupt(std::format("the block-size table has {} entries, the assets use {}", h.assetBlockCount, nextBlock));

    m_blockStates = std::make_unique<std::atomic<u8>[]>(h.pakBlockCount);
    m_info.platform = platform;
    m_info.contentBuild = h.contentBuild;
    m_info.tags = h.tags;
    m_info.fileSize = fileSize;
    m_info.dataEnd = dataEnd;
    m_info.assetCount = h.assetCount;
    m_info.pakBlockCount = h.pakBlockCount;
    return {};
}

const HpakEntry* HpakReader::find(AssetId id) const noexcept {
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), id,
                                     [](const HpakEntry& e, AssetId v) { return e.id < v; });
    return it != m_entries.end() && it->id == id ? &*it : nullptr;
}

Result<std::vector<u8>> HpakReader::read(AssetId id) const {
    const HpakEntry* e = find(id);
    if (!e) return makeError(ErrorCode::NotFound, "'{}': no asset {}", m_name, id);
    return read(*e);
}

bool HpakReader::owns(const HpakEntry& entry) const noexcept {
    const std::less<const HpakEntry*> before;
    return !m_entries.empty() && !before(&entry, m_entries.data()) &&
           before(&entry, m_entries.data() + m_entries.size());
}

Result<std::vector<u8>> HpakReader::read(const HpakEntry& entry) const {
    if (!owns(entry)) return makeError(ErrorCode::InvalidArgument, "'{}': the entry is not one of this pak's", m_name);
    std::vector<u8> out(static_cast<usize>(entry.rawSize)); // ≤ kMaxAssetSize (checked at open)
    HELIOS_TRY(readInto(entry, out));
    return out;
}

Result<void> HpakReader::readInto(const HpakEntry& entry, std::span<u8> out) const {
    if (!owns(entry)) return makeError(ErrorCode::InvalidArgument, "'{}': the entry is not one of this pak's", m_name);
    if (out.size() != entry.rawSize)
        return makeError(ErrorCode::InvalidArgument, "'{}': asset {} is {} bytes, the buffer {}", m_name, entry.id,
                         entry.rawSize, out.size());
    std::vector<u8> scratch;
    u64 rel = 0;
    for (u32 k = 0; k < entry.blockCount; ++k) {
        const u32 stored = m_blockSizes[entry.firstBlock + k];
        const u64 rawPos = u64(k) * kAssetBlockSize;
        const usize rawLen = static_cast<usize>(std::min(kAssetBlockSize, entry.rawSize - rawPos));
        HELIOS_TRY_ASSIGN(const std::span<const u8> bytes, fetch(entry.offset + rel, stored, scratch));
        u8* dst = out.data() + rawPos;
        if (entry.codec == HpakCodec::None) {
            std::memcpy(dst, bytes.data(), rawLen); // stored == rawLen (checked at open)
        } else {
            ZSTD_DCtx* dctx = threadDCtx();
            if (!dctx) return Error{ErrorCode::OutOfMemory, "ZSTD_createDCtx failed"};
            const size_t n = ZSTD_decompressDCtx(dctx, dst, rawLen, bytes.data(), bytes.size());
            if (ZSTD_isError(n))
                return makeError(ErrorCode::Corrupt, "'{}': asset {} block {}: zstd: {}", m_name, entry.id, k,
                                 ZSTD_getErrorName(n));
            if (n != rawLen)
                return makeError(ErrorCode::Corrupt, "'{}': asset {} block {} decodes to {} bytes, expected {}", m_name,
                                 entry.id, k, n, rawLen);
        }
        rel += stored;
    }
    if (hash128(out.data(), out.size()) != entry.cookedHash)
        return makeError(ErrorCode::Corrupt, "'{}': asset {} does not match its cooked hash", m_name, entry.id);
    return {};
}

Result<std::span<const u8>> HpakReader::fetch(u64 start, u64 size, std::vector<u8>& scratch) const {
    HELIOS_ASSERT(size > 0 && start >= kHeaderBlockSize && start + size <= m_info.dataEnd);
    const u32 first = static_cast<u32>((start - kHeaderBlockSize) / kPakBlockSize);
    const u32 last = static_cast<u32>((start + size - 1 - kHeaderBlockSize) / kPakBlockSize);
    bool verified = true;
    for (u32 b = first; b <= last && verified; ++b)
        verified = m_blockStates[b].load(std::memory_order_acquire) == toUnderlying(HpakBlockState::Verified);
    if (verified) {
        scratch.resize(static_cast<usize>(size));
        HELIOS_TRY(m_source->readAt(start, scratch));
        return std::span<const u8>(scratch.data(), scratch.size());
    }
    // Verification needs whole pak blocks: read the covering range once and check each block in it.
    const u64 rangeStart = blockOffset(first);
    const u64 rangeEnd = std::min(blockOffset(last) + kPakBlockSize, m_info.dataEnd);
    scratch.resize(static_cast<usize>(rangeEnd - rangeStart));
    HELIOS_TRY(m_source->readAt(rangeStart, scratch));
    for (u32 b = first; b <= last; ++b) {
        const u64 from = blockOffset(b) - rangeStart;
        const u64 to = std::min(from + kPakBlockSize, u64(scratch.size()));
        HELIOS_TRY(verifyBlock(b, std::span<u8>(scratch.data() + from, static_cast<usize>(to - from))));
    }
    return std::span<const u8>(scratch.data() + (start - rangeStart), static_cast<usize>(size));
}

Result<void> HpakReader::verifyBlock(u32 index, std::span<u8> bytes) const {
    constexpr u8 kVerified = toUnderlying(HpakBlockState::Verified);
    constexpr u8 kPending = toUnderlying(HpakBlockState::Pending);
    constexpr u8 kBad = toUnderlying(HpakBlockState::Bad);
    std::atomic<u8>& state = m_blockStates[index];
    const u64 offset = blockOffset(index);
    const auto pending = [&] {
        return makeError(ErrorCode::Busy, "'{}': pak block {} (offset {}) is being re-fetched", m_name, index, offset);
    };
    const auto bad = [&](std::string_view why) {
        return makeError(ErrorCode::Corrupt, "'{}': pak block {} (offset {}) {}", m_name, index, offset, why);
    };

    u8 s = state.load(std::memory_order_acquire);
    if (s == kVerified) return {};
    if (s == kPending) return pending();
    if (s == kBad) return bad("failed its checksum");
    const u64 expected = m_blockHashes[index];
    const u64 actual = hash64(bytes.data(), bytes.size());
    if (actual == expected) {
        state.store(kVerified, std::memory_order_release);
        return {};
    }

    // Slow path, serialized per pak so the hook runs at most once per block.
    std::lock_guard lock(m_repairMutex);
    s = state.load(std::memory_order_acquire);
    if (s == kPending) return pending();
    if (s == kBad) return bad("failed its checksum");
    if (s == kVerified) {
        // Repaired and verified by another reader after our read: our copy is stale.
        HELIOS_TRY(m_source->readAt(offset, bytes));
        if (hash64(bytes.data(), bytes.size()) == expected) return {};
        return bad("changed after it was verified");
    }
    HELIOS_LOG_WARN("hpak '{}': pak block {} (offset {}, {} bytes) failed its checksum ({:016x}, expected {:016x})",
                    m_name, index, offset, bytes.size(), actual, expected);
    const HpakBadBlock info{this, index, offset, bytes.size(), expected, actual};
    const RefetchStatus status = m_refetcher ? m_refetcher->refetch(info) : RefetchStatus::Failed;
    if (status == RefetchStatus::Repaired) {
        if (m_source->readAt(offset, bytes) && hash64(bytes.data(), bytes.size()) == expected) {
            state.store(kVerified, std::memory_order_release);
            return {};
        }
        state.store(kBad, std::memory_order_release);
        return bad("still fails its checksum after the re-fetch");
    }
    if (status == RefetchStatus::Pending) {
        state.store(kPending, std::memory_order_release);
        return pending();
    }
    state.store(kBad, std::memory_order_release);
    return bad("failed its checksum");
}

Result<void> HpakReader::verifyAll() const {
    std::vector<u8> scratch;
    for (u32 b = 0; b < m_info.pakBlockCount; ++b) {
        if (m_blockStates[b].load(std::memory_order_acquire) == toUnderlying(HpakBlockState::Verified)) continue;
        const u64 offset = blockOffset(b);
        scratch.resize(static_cast<usize>(std::min(kPakBlockSize, m_info.dataEnd - offset)));
        HELIOS_TRY(m_source->readAt(offset, scratch));
        HELIOS_TRY(verifyBlock(b, scratch));
    }
    return {};
}

void HpakReader::retryBlocks(u64 offset, u64 size) {
    const u64 lo = std::max(offset, kHeaderBlockSize);
    const u64 hi = std::min(size > ~u64(0) - offset ? ~u64(0) : offset + size, m_info.dataEnd);
    if (lo >= hi) return;
    const u32 first = static_cast<u32>((lo - kHeaderBlockSize) / kPakBlockSize);
    const u32 last = static_cast<u32>((hi - 1 - kHeaderBlockSize) / kPakBlockSize);
    std::lock_guard lock(m_repairMutex);
    for (u32 b = first; b <= last; ++b) {
        const u8 s = m_blockStates[b].load(std::memory_order_acquire);
        if (s == toUnderlying(HpakBlockState::Pending) || s == toUnderlying(HpakBlockState::Bad))
            m_blockStates[b].store(toUnderlying(HpakBlockState::Unverified), std::memory_order_release);
    }
}

HpakBlockState HpakReader::blockState(u32 index) const noexcept {
    if (index >= m_info.pakBlockCount) return HpakBlockState::Unverified;
    return static_cast<HpakBlockState>(m_blockStates[index].load(std::memory_order_acquire));
}

} // namespace helios::asset
