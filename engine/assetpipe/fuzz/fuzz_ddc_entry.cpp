// Fuzz target: the DDC entry reader (ddc::readEntry = readEntryHeader + checkEntryPayload) and the local
// store's get(), which reads the same format from a file. A DDC entry comes back from a cache directory
// that any local process can write, so it is hostile input (02 §8.3).
//
// Each input runs twice: as given, and resealed (the header checksum and the payload hash recomputed over
// what follows the header), so that mutated fields reach the size, key and reserved-byte checks behind
// the checksums. The key asked for is the one the header claims (when there is one), which is what lets a
// read succeed. Properties:
//   - readEntry returns a Result, and a payload view inside the input;
//   - the format has no slack, so a successful read means the input is exactly encodeEntry(key, payload):
//     it re-encodes byte-identically, and the same bytes under any other key fail;
//   - LocalDdc::get() on a file holding the input agrees: a hit exactly when readEntry succeeds, with the
//     same payload (a failure is never data).
// A header claiming up to 2 GiB is rejected before anything is allocated (the file is shorter than its
// claim), so -malloc_limit_mb catches a reader that allocates the claim.

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#include "helios/assetpipe/ddc.h"
#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/random.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::assetpipe;

constexpr usize kMaxInput = 4 * kMiB;

[[noreturn]] void fail() {
    std::abort();
}

// A damaged entry logs nothing, but keep the target quiet like the others.
const bool g_quietLogs = (log::setLevel(log::Level::Error), true);

/// A store in a private temp directory, removed at exit.
struct Store {
    fs::Path root;
    std::unique_ptr<LocalDdc> ddc;
    Store() {
        root = fs::createUniqueTempDirectory("helios-ddc-fuzz").value();
        LocalDdcOptions options;
        options.root = root;
        ddc = LocalDdc::open(options).value();
    }
    ~Store() { (void)fs::removeAll(root); }
};

Store& store() {
    static Store s;
    return s;
}

Hash128 claimedKey(const u8* data, usize size) {
    if (size < 24) return Hash128{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
    return Hash128{loadLE<u64>(data + 8), loadLE<u64>(data + 16)};
}

void exercise(const u8* data, usize size) {
    const std::span<const u8> input(data, size);
    const Hash128 key = claimedKey(data, size);
    const Result<std::span<const u8>> read = ddc::readEntry(input, key);
    if (read) {
        const std::span<const u8> payload = *read;
        if (payload.data() != data + ddc::kEntryHeaderBytes ||
            payload.size() != size - ddc::kEntryHeaderBytes)
            fail();
        const Result<std::vector<u8>> again = ddc::encodeEntry(key, payload);
        if (!again || again->size() != size || !std::equal(again->begin(), again->end(), data)) fail();
        const Hash128 other{key.low ^ 1, key.high};
        if (ddc::readEntry(input, other)) fail();
    }
    // The store's file path must agree with the in-memory reader.
    LocalDdc& ddc = *store().ddc;
    const fs::Path path = ddc.entryPath(key);
    if (!fs::createDirectories(path.parent_path()) || !fs::writeFile(path, input, fs::WriteMode::Direct))
        fail();
    const Result<std::vector<u8>> got = ddc.get(key);
    if (got.ok() != read.ok()) fail();
    if (got && !std::equal(got->begin(), got->end(), read->begin(), read->end())) fail();
    (void)fs::remove(path);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInput) return 0;
    exercise(data, size);
    if (size >= ddc::kEntryHeaderBytes) {
        std::vector<u8> sealed(data, data + size);
        u8* p = sealed.data();
        const Hash128 payloadHash = hash128(p + ddc::kEntryHeaderBytes, size - ddc::kEntryHeaderBytes);
        storeLE<u64>(p + 32, payloadHash.low);
        storeLE<u64>(p + 40, payloadHash.high);
        storeLE<u64>(p + 56, hash64(p, ddc::kEntryHeaderHashedBytes));
        exercise(sealed.data(), sealed.size());
    }
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const Hash128 key{0x1111222233334444ull, 0x5555666677778888ull};
    const auto entry = [&](std::span<const u8> payload) { return ddc::encodeEntry(key, payload).value(); };
    const auto reseal = [](std::vector<u8>& e) {
        storeLE<u64>(e.data() + 56, hash64(e.data(), ddc::kEntryHeaderHashedBytes));
    };
    std::vector<u8> text(40);
    for (usize i = 0; i < text.size(); ++i) text[i] = static_cast<u8>("cooked product bytes "[i % 21]);
    std::vector<u8> noise(70 * 1024);
    Xoshiro256 rng(7);
    for (u8& b : noise) b = static_cast<u8>(rng.next() >> 56);

    out.push_back(entry(text));  // 0: a valid entry
    out.push_back(entry({}));    // 1: a valid empty entry
    out.push_back(entry(noise)); // 2: a 70 KB entry
    std::vector<u8> e = out[0];
    e.resize(ddc::kEntryHeaderBytes + 10);
    out.push_back(e); // 3: truncated payload
    e = out[0];
    e.push_back(0);
    out.push_back(e); // 4: a byte after the payload
    e = out[0];
    storeLE<u16>(e.data() + 4, 1);
    reseal(e);
    out.push_back(e); // 5: a newer entry version
    e = out[1];
    storeLE<u64>(e.data() + 24, ddc::kMaxPayload);
    reseal(e);
    out.push_back(e); // 6: header only, claiming 2 GiB
    e = out[0];
    e[ddc::kEntryHeaderBytes + 3] ^= 0x20;
    out.push_back(e); // 7: payload checksum mismatch
}
