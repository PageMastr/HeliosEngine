// `.hpak` v0 writer (see hpak_writer.h and engine/asset's hpak_format.h for the layout).

#include "helios/assetpipe/hpak_writer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

#include <zstd.h>

#include "helios/core/hash.h"

namespace helios::assetpipe {
namespace {

using namespace asset::hpak;
using asset::HpakCodec;
using asset::HpakEntry;

constexpr std::array<u8, kBlobAlignment> kZeros{};

/// XXH3-64 of each 64 KiB pak block of a byte stream fed in pieces.
class PakBlockHasher {
public:
    void feed(const u8* p, u64 n) noexcept {
        while (n > 0) {
            const u64 take = std::min(n, kPakBlockSize - m_fill);
            m_hasher.update(p, static_cast<usize>(take));
            p += take;
            n -= take;
            m_fill += take;
            if (m_fill == kPakBlockSize) flush();
        }
    }
    void zeros(u64 n) noexcept {
        while (n > 0) {
            const u64 take = std::min<u64>(n, kZeros.size());
            feed(kZeros.data(), take);
            n -= take;
        }
    }
    std::vector<u64> finish() {
        if (m_fill > 0) flush();
        return std::move(m_hashes);
    }

private:
    void flush() {
        m_hashes.push_back(m_hasher.digest());
        m_hasher.reset();
        m_fill = 0;
    }
    Hasher64 m_hasher;
    u64 m_fill = 0;
    std::vector<u64> m_hashes;
};

/// One compression context per thread (ZSTD_compress2 resets the session; the parameters are set on
/// every call, so a context carries nothing from one writer to the next).
struct CCtxHolder {
    ZSTD_CCtx* ctx = nullptr;
    ~CCtxHolder() { ZSTD_freeCCtx(ctx); }
};

ZSTD_CCtx* threadCCtx() noexcept {
    thread_local CCtxHolder holder;
    if (!holder.ctx) holder.ctx = ZSTD_createCCtx();
    return holder.ctx;
}

u64 pakSizeFor(u64 blobBytes, u64 assets, u64 assetBlocks) noexcept {
    return kHeaderBlockSize + blobBytes + tocSize(assets, assetBlocks, pakBlockCount(blobBytes));
}

Error limitError(u64 size) {
    return makeError(
        ErrorCode::LimitExceeded,
        "the pak would be {} bytes, above the 2 GiB .hpak limit ({} bytes, 02 §6.3); split the content "
        "across more paks",
        size, kMaxPakSize);
}

} // namespace

Result<HpakLayout> planHpakLayout(std::span<const u64> blobSizes, u64 assetBlocks) {
    if (blobSizes.size() > std::numeric_limits<u32>::max() || assetBlocks > std::numeric_limits<u32>::max())
        return makeError(ErrorCode::LimitExceeded,
                         "{} assets and {} asset blocks do not fit a pak's u32 counts", blobSizes.size(),
                         assetBlocks);
    HpakLayout layout;
    layout.blobOffsets.reserve(blobSizes.size());
    u64 pos = kHeaderBlockSize;
    for (const u64 size : blobSizes) {
        if (size > kMaxPakSize) return limitError(kHeaderBlockSize + size);
        layout.blobOffsets.push_back(pos);
        pos += alignUp(size, kBlobAlignment);
        if (pos > kMaxPakSize) return limitError(pos);
    }
    const u64 pakBlocks = pakBlockCount(pos - kHeaderBlockSize);
    layout.dataEnd = pos;
    layout.tocSize = tocSize(blobSizes.size(), assetBlocks, pakBlocks);
    layout.fileSize = pos + layout.tocSize;
    layout.pakBlockCount = static_cast<u32>(pakBlocks);
    if (layout.fileSize > kMaxPakSize) return limitError(layout.fileSize);
    return layout;
}

Result<HpakWriter> HpakWriter::create(const HpakWriterOptions& options) {
    const u32 platform = toUnderlying(options.platform);
    if (platform < toUnderlying(asset::HpakPlatform::PcClient) ||
        platform > toUnderlying(asset::HpakPlatform::Editor))
        return makeError(ErrorCode::InvalidArgument, "unknown pak platform {}", platform);
    if (options.tags.tier > kMaxTier)
        return makeError(ErrorCode::InvalidArgument, "pak tier {} (tiers are 0..{})", options.tags.tier,
                         kMaxTier);
    if (options.zstdLevel < 1 || options.zstdLevel > 19)
        return makeError(ErrorCode::InvalidArgument, "zstd level {} (1..19)", options.zstdLevel);
    return HpakWriter(options);
}

Result<asset::AssetId> HpakWriter::add(const Guid& guid, std::span<const u8> cooked,
                                       const HpakAssetOrder& order) {
    HELIOS_TRY_ASSIGN(const asset::AssetId id, m_ids.check(guid));
    if (order.tier > kMaxTier)
        return makeError(ErrorCode::InvalidArgument, "asset {}: tier {} (tiers are 0..{})", guid, order.tier,
                         kMaxTier);
    if (cooked.size() > kMaxAssetSize)
        return makeError(ErrorCode::LimitExceeded, "asset {} is {} bytes, above the 2 GiB asset limit", guid,
                         cooked.size());

    Asset a;
    a.guid = guid;
    a.id = id;
    a.order = order;
    a.cookedHash = hash128(cooked.data(), cooked.size());
    a.rawSize = cooked.size();
    const u64 blocks = assetBlockCount(a.rawSize);
    if (blocks > 0) {
        ZSTD_CCtx* cctx = threadCCtx();
        if (!cctx) return Error{ErrorCode::OutOfMemory, "ZSTD_createCCtx failed"};
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, m_options.zstdLevel);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 0); // the pak has its own checksums
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_dictIDFlag, 0);
        a.blockSizes.reserve(static_cast<usize>(blocks));
        for (u64 k = 0; k < blocks; ++k) {
            const u64 at = k * kAssetBlockSize;
            const usize len = static_cast<usize>(std::min(kAssetBlockSize, a.rawSize - at));
            const usize bound = ZSTD_compressBound(len);
            const usize old = a.stored.size();
            a.stored.resize(old + bound);
            const size_t n = ZSTD_compress2(cctx, a.stored.data() + old, bound, cooked.data() + at, len);
            if (ZSTD_isError(n))
                return makeError(ErrorCode::Unknown, "asset {}: zstd: {}", guid, ZSTD_getErrorName(n));
            a.stored.resize(old + n);
            a.blockSizes.push_back(static_cast<u32>(n));
        }
        a.codec = HpakCodec::Zstd;
        if (a.stored.size() >= a.rawSize) { // zstd does not help: store the bytes
            a.codec = HpakCodec::None;
            a.stored.assign(cooked.begin(), cooked.end());
            for (u64 k = 0; k < blocks; ++k)
                a.blockSizes[k] =
                    static_cast<u32>(std::min(kAssetBlockSize, a.rawSize - k * kAssetBlockSize));
        }
    }

    const u64 blobBytes = m_blobBytes + alignUp<u64>(a.stored.size(), kBlobAlignment);
    const u64 size = pakSizeFor(blobBytes, m_assets.size() + 1, m_assetBlocks + blocks);
    if (size > kMaxPakSize)
        return makeError(
            ErrorCode::LimitExceeded,
            "adding asset {} ({} bytes, {} stored) would make the pak {} bytes, above the 2 GiB .hpak "
            "limit ({} bytes, 02 §6.3); split the content across more paks",
            guid, a.rawSize, a.stored.size(), size, kMaxPakSize);
    HELIOS_TRY(m_ids.insert(guid)); // cannot fail: check() passed and nothing was added since
    m_blobBytes = blobBytes;
    m_assetBlocks += blocks;
    m_size = size;
    m_assets.push_back(std::move(a));
    return id;
}

Result<HpakWriter::Plan> HpakWriter::plan() const {
    Plan p;
    p.blobOrder.reserve(m_assets.size());
    for (const Asset& a : m_assets) p.blobOrder.push_back(&a);
    p.tocOrder = p.blobOrder;
    // 02 §6.3: tier → group → language → recorded first use → GUID (unique, so the order is total).
    std::sort(p.blobOrder.begin(), p.blobOrder.end(), [](const Asset* a, const Asset* b) {
        return std::tie(a->order.tier, a->order.group, a->order.language, a->order.firstUse, a->guid) <
               std::tie(b->order.tier, b->order.group, b->order.language, b->order.firstUse, b->guid);
    });
    std::sort(p.tocOrder.begin(), p.tocOrder.end(),
              [](const Asset* a, const Asset* b) { return a->id < b->id; });

    std::vector<u64> sizes;
    sizes.reserve(p.blobOrder.size());
    for (const Asset* a : p.blobOrder) sizes.push_back(a->stored.size());
    HELIOS_TRY_ASSIGN(p.layout, planHpakLayout(sizes, m_assetBlocks));

    std::vector<u64> offsetOf(m_assets.size());
    PakBlockHasher hasher;
    for (usize i = 0; i < p.blobOrder.size(); ++i) {
        const Asset* a = p.blobOrder[i];
        offsetOf[static_cast<usize>(a - m_assets.data())] = p.layout.blobOffsets[i];
        hasher.feed(a->stored.data(), a->stored.size());
        hasher.zeros(alignUp<u64>(a->stored.size(), kBlobAlignment) - a->stored.size());
    }
    const std::vector<u64> blockHashes = hasher.finish();
    HELIOS_ASSERT(blockHashes.size() == p.layout.pakBlockCount);

    p.toc.assign(static_cast<usize>(p.layout.tocSize), 0);
    u8* sizeAt = p.toc.data() + p.tocOrder.size() * kTocEntryBytes;
    u32 firstBlock = 0;
    for (usize i = 0; i < p.tocOrder.size(); ++i) {
        const Asset* a = p.tocOrder[i];
        const HpakEntry e{a->id,
                          a->cookedHash,
                          offsetOf[static_cast<usize>(a - m_assets.data())],
                          a->stored.size(),
                          a->rawSize,
                          firstBlock,
                          static_cast<u32>(a->blockSizes.size()),
                          a->codec};
        encodeEntry(e, std::span<u8, kTocEntryBytes>(p.toc.data() + i * kTocEntryBytes, kTocEntryBytes));
        for (const u32 s : a->blockSizes) {
            storeLE<u32>(sizeAt, s);
            sizeAt += kBlockSizeBytes;
        }
        firstBlock += static_cast<u32>(a->blockSizes.size());
    }
    for (const u64 h : blockHashes) {
        storeLE<u64>(sizeAt, h);
        sizeAt += kBlockHashBytes;
    }

    Header h;
    h.platform = toUnderlying(m_options.platform);
    h.contentBuild = m_options.contentBuild;
    h.tags = m_options.tags;
    h.tocOffset = p.layout.dataEnd;
    h.tocSize = p.layout.tocSize;
    h.assetCount = static_cast<u32>(m_assets.size());
    h.assetBlockCount = static_cast<u32>(m_assetBlocks);
    h.pakBlockCount = p.layout.pakBlockCount;
    h.tocHash = hash64(p.toc.data(), p.toc.size());
    p.header.assign(static_cast<usize>(kHeaderBlockSize), 0);
    const std::span<u8, kHeaderBytes> fields(p.header.data(), kHeaderBytes);
    encodeHeader(h, fields);
    storeLE<u64>(p.header.data() + kHeaderHashOffset, computeHeaderHash(fields));
    return p;
}

Result<void> HpakWriter::emit(const std::function<Result<void>(std::span<const u8>)>& sink) const {
    HELIOS_TRY_ASSIGN(const Plan p, plan());
    HELIOS_TRY(sink(p.header));
    for (const Asset* a : p.blobOrder) {
        if (!a->stored.empty()) HELIOS_TRY(sink(a->stored));
        const u64 pad = alignUp<u64>(a->stored.size(), kBlobAlignment) - a->stored.size();
        if (pad > 0) HELIOS_TRY(sink(std::span<const u8>(kZeros.data(), static_cast<usize>(pad))));
    }
    if (!p.toc.empty()) HELIOS_TRY(sink(p.toc));
    return {};
}

Result<std::vector<u8>> HpakWriter::build() const {
    std::vector<u8> out;
    out.reserve(static_cast<usize>(m_size));
    HELIOS_TRY(emit([&out](std::span<const u8> bytes) -> Result<void> {
        out.insert(out.end(), bytes.begin(), bytes.end());
        return {};
    }));
    HELIOS_ASSERT(out.size() == m_size);
    return out;
}

Result<void> HpakWriter::writeFile(const fs::Path& path) const {
    fs::Path tmp = path;
    tmp += ".tmp";
    Result<void> written;
    {
        HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(tmp, fs::OpenMode::Write));
        written = emit([&file](std::span<const u8> bytes) { return file.write(bytes.data(), bytes.size()); });
        if (written) written = file.sync();
    }
    if (written) written = fs::rename(tmp, path);
    if (!written) (void)fs::remove(tmp);
    return written;
}

void resealHpak(std::span<u8> pak) noexcept {
    if (pak.size() < kHeaderBlockSize) return;
    const std::span<u8, kHeaderBytes> fields(pak.data(), kHeaderBytes);
    const Header h = decodeHeader(fields);
    const u64 size = pak.size();
    if (h.tocOffset >= kHeaderBlockSize && h.tocOffset <= size && h.tocSize <= size - h.tocOffset) {
        const u64 hashesAt = u64(h.assetCount) * kTocEntryBytes + u64(h.assetBlockCount) * kBlockSizeBytes;
        if (hashesAt <= h.tocSize && h.pakBlockCount <= (h.tocSize - hashesAt) / kBlockHashBytes) {
            for (u64 b = 0; b < h.pakBlockCount; ++b) {
                const u64 at = kHeaderBlockSize + b * kPakBlockSize;
                if (at >= h.tocOffset) break;
                const u64 len = std::min(kPakBlockSize, h.tocOffset - at);
                storeLE<u64>(pak.data() + h.tocOffset + hashesAt + b * kBlockHashBytes,
                             hash64(pak.data() + at, static_cast<usize>(len)));
            }
        }
        storeLE<u64>(pak.data() + kTocHashOffset,
                     hash64(pak.data() + h.tocOffset, static_cast<usize>(h.tocSize)));
    }
    storeLE<u64>(pak.data() + kHeaderHashOffset, computeHeaderHash(fields));
}

} // namespace helios::assetpipe
