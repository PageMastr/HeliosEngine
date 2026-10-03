// Fuzz target: the `.hpak` v0 reader (02 §6.3; 02 §8.3 and 09 §6 name the `.hpak` reader among the
// fuzzed readers, AAA-SEC-7).
//
// Each input runs twice: as given, and with its checksums recomputed by assetpipe::resealHpak, so that
// mutated header and TOC fields reach the field checks behind the header and TOC checksums.
// Properties: open() and every read return a Result (no crash, sanitizer report or out-of-bounds
// access); an opened pak's entries are strictly sorted, 4 KiB aligned and inside the blob region;
// a successful read returns exactly rawSize bytes that match the cooked hash, and reading again gives
// the same outcome; the re-fetch hook runs at most once per pak block (it answers Repaired, Pending
// or Failed by block index, so every path runs); a second mount of the same bytes overlays the first.

#include <cstdlib>
#include <memory>
#include <vector>

#include "helios/asset/hpak_reader.h"
#include "helios/asset/mount_table.h"
#include "helios/assetpipe/hpak_writer.h"
#include "helios/core/hash.h"
#include "helios/core/log.h"
#include "helios/core/random.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::asset;

constexpr usize kMaxInput = 8 * kMiB; // bounds memory per run; the 2 GiB limit has its own unit test
constexpr u64 kMaxAssetRead = 4 * kMiB;
constexpr usize kMaxAssetsRead = 64;

[[noreturn]] void fail() { std::abort(); }

// Every damaged block logs a warning; at fuzzing rates that is noise and time.
const bool g_quietLogs = (log::setLevel(log::Level::Error), true);

class OncePerBlockHook final : public IBlockRefetcher {
public:
    explicit OncePerBlockHook(usize blocks) : m_seen(blocks, false) {}
    RefetchStatus refetch(const HpakBadBlock& block) override {
        if (block.index >= m_seen.size() || m_seen[block.index]) fail();
        m_seen[block.index] = true;
        return static_cast<RefetchStatus>(block.index % 3);
    }

private:
    std::vector<bool> m_seen;
};

void exercise(const u8* data, usize size) {
    auto hook = std::make_shared<OncePerBlockHook>(size / hpak::kPakBlockSize + 2);
    HpakOpenOptions options;
    options.refetcher = hook;
    auto opened = HpakReader::open(makeHpakMemorySource(std::vector<u8>(data, data + size), "fuzz"), options);
    if (!opened) return;
    HpakReader& r = **opened;
    const HpakInfo& info = r.info();
    if (info.fileSize != size || info.dataEnd > size || info.dataEnd < hpak::kHeaderBlockSize) fail();
    const std::span<const HpakEntry> entries = r.entries();
    if (entries.size() != info.assetCount) fail();
    for (usize i = 0; i < entries.size(); ++i) {
        const HpakEntry& e = entries[i];
        if (i > 0 && !(entries[i - 1].id < e.id)) fail();
        if (e.offset % hpak::kBlobAlignment != 0 || e.offset < hpak::kHeaderBlockSize ||
            e.offset > info.dataEnd || e.compSize > info.dataEnd - e.offset ||
            e.rawSize > hpak::kMaxAssetSize)
            fail();
        if (r.find(e.id) != &e) fail();
    }
    usize reads = 0;
    for (const HpakEntry& e : entries) {
        if (reads == kMaxAssetsRead) break;
        if (e.rawSize > kMaxAssetRead) continue;
        ++reads;
        const Result<std::vector<u8>> got = r.read(e);
        if (got && (got->size() != e.rawSize || hash128(got->data(), got->size()) != e.cookedHash)) fail();
        const Result<std::vector<u8>> again = r.read(e);
        if (got.ok() != again.ok() || (got && *got != *again)) fail();
    }
    (void)r.verifyAll();
    r.retryBlocks(0, size);

    // The same bytes mounted on top resolve every id to the top mount.
    auto second = HpakReader::open(makeHpakMemorySource(std::vector<u8>(data, data + size), "fuzz-top"));
    if (!second) fail();
    PakMountTable table;
    const Result<PakMountId> bottom = table.mount(*opened);
    const Result<PakMountId> top = table.mount(*second);
    if (!bottom || !top || table.assetCount() != entries.size()) fail();
    for (usize i = 0; i < entries.size() && i < kMaxAssetsRead; ++i) {
        const std::optional<AssetLocation> loc = table.find(entries[i].id);
        if (!loc || loc->mount != *top || loc->entry->id != entries[i].id) fail();
    }
    if (!table.unmount(*top) ||
        table.find(entries.empty() ? AssetId{1} : entries[0].id).has_value() == entries.empty())
        fail();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInput) return 0;
    exercise(data, size);
    std::vector<u8> sealed(data, data + size);
    assetpipe::resealHpak(sealed);
    exercise(sealed.data(), sealed.size());
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const auto guid = [](u64 n) {
        return Guid(0x4000000000004000ull | (n << 16), 0x8000000000000000ull | n);
    };
    const auto text = [](usize size, u32 seed) {
        std::vector<u8> v(size);
        for (usize i = 0; i < size; ++i) v[i] = static_cast<u8>("fuzz seed asset "[(i + seed) % 16]);
        return v;
    };
    const auto noise = [](usize size, u64 seed) {
        std::vector<u8> v(size);
        Xoshiro256 rng(seed);
        for (u8& b : v) b = static_cast<u8>(rng.next() >> 56);
        return v;
    };
    struct In {
        u64 n;
        std::vector<u8> bytes;
        assetpipe::HpakAssetOrder order;
    };
    const auto pak = [&](const std::vector<In>& assets, asset::HpakPlatform platform, asset::HpakTags tags) {
        assetpipe::HpakWriterOptions o;
        o.platform = platform;
        o.contentBuild = 0xC0FFEE;
        o.tags = tags;
        assetpipe::HpakWriter w = assetpipe::HpakWriter::create(o).value();
        for (const In& in : assets)
            if (!w.add(guid(in.n), in.bytes, in.order)) fail();
        return w.build().value();
    };
    using P = asset::HpakPlatform;
    out.push_back(pak({}, P::PcClient, {}));                     // empty pak
    out.push_back(pak({{1, text(40, 1), {}}}, P::PcClient, {})); // one small zstd asset
    out.push_back(pak({{1, text(900, 1), {}}, {2, {}, {}}, {3, noise(3000, 3), {}}, {4, {7}, {}}}, P::Server,
                      {2, 0xABCDEF, 3})); // mixed codecs, empty asset
    out.push_back(
        pak({{5, text(300 * 1024, 5), {}}, {6, text(70, 6), {}}}, P::Editor, {1, 9, 0})); // 2 zstd blocks
    out.push_back(pak({{7, noise(30 * 1024, 7), {0, 0, 0, 2}},
                       {8, noise(30 * 1024, 8), {0, 0, 0, 1}},
                       {9, noise(30 * 1024, 9), {0, 0, 0, 0}}},
                      P::PcClient, {})); // spans two pak blocks
    std::vector<u8> damaged = out.back();
    damaged[hpak::kHeaderBlockSize + 70 * 1024] ^= 0x10; // second pak block fails
    out.push_back(std::move(damaged));
    std::vector<u8> truncated = out[2];
    truncated.resize(hpak::kHeaderBlockSize);
    out.push_back(std::move(truncated)); // header only
}
