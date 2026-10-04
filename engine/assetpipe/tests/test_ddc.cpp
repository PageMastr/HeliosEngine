// DDC v0 (02 §6.2, 07 §3.2): the key's inputs, the entry format against hostile bytes, and the local
// store's atomic put, verified get, concurrent writers and LRU eviction under its cap.

#include <algorithm>
#include <thread>
#include <vector>

#include "assetpipe_test_util.h"
#include "helios/core/random.h"

namespace {

using namespace assetpipe_test;

std::vector<u8> bytesOf(std::string_view text) {
    return std::vector<u8>(text.begin(), text.end());
}

std::vector<u8> noise(usize size, u64 seed) {
    std::vector<u8> out(size);
    Xoshiro256 rng(seed);
    for (u8& b : out) b = static_cast<u8>(rng.next() >> 56);
    return out;
}

Hash128 keyOf(u64 n) {
    return Hash128{0x9E3779B97F4A7C15ull * n, n};
}

std::unique_ptr<LocalDdc> openStore(const fs::Path& root, u64 cap = 1ull << 40) {
    LocalDdcOptions o;
    o.root = root;
    o.capBytes = cap;
    return LocalDdc::open(o).value();
}

TEST_CASE("ddc key: every input changes it, and paths, times and settings spelling do not") {
    const Hash128 deps[] = {keyOf(1), keyOf(2)};
    DdcKeyInputs base;
    base.builder = "png";
    base.builderVersion = 2;
    base.sourceHash = hash128(std::string_view("source bytes"));
    base.settings = R"({"mips":false})";
    base.settingsLayout = refl::typeOf<TextureSettings>().layoutHash;
    base.platform = asset::HpakPlatform::PcClient;
    base.dependencies = deps;
    const Hash128 k = makeDdcKey(base);
    CHECK(makeDdcKey(base) == k); // pure

    std::vector<std::pair<const char*, DdcKeyInputs>> variants;
    auto vary = [&](const char* what, auto&& change) {
        DdcKeyInputs v = base;
        change(v);
        variants.emplace_back(what, v);
    };
    vary("builder", [](DdcKeyInputs& v) { v.builder = "tga"; });
    vary("builder version", [](DdcKeyInputs& v) { v.builderVersion = 3; });
    vary("source", [](DdcKeyInputs& v) { v.sourceHash = hash128(std::string_view("source bytes!")); });
    vary("settings", [](DdcKeyInputs& v) { v.settings = R"({"mips":true})"; });
    vary("settings layout",
         [](DdcKeyInputs& v) { v.settingsLayout = refl::typeOf<TextureSettingsV2>().layoutHash; });
    vary("platform server", [](DdcKeyInputs& v) { v.platform = asset::HpakPlatform::Server; });
    vary("platform editor", [](DdcKeyInputs& v) { v.platform = asset::HpakPlatform::Editor; });
    vary("cooker version", [](DdcKeyInputs& v) { v.cookerVersion = kCookerVersion + 1; });
    static const Hash128 swapped[] = {keyOf(2), keyOf(1)};
    static const Hash128 one[] = {keyOf(1)};
    vary("dependency order", [](DdcKeyInputs& v) { v.dependencies = swapped; });
    vary("dependency removed", [](DdcKeyInputs& v) { v.dependencies = one; });
    vary("no dependencies", [](DdcKeyInputs& v) { v.dependencies = {}; });
    // Length-prefixed: moving bytes between the builder id and what follows it is a different key.
    vary("builder boundary", [](DdcKeyInputs& v) { v.builder = "pn"; });
    std::vector<Hash128> all = {k};
    for (const auto& [what, v] : variants) {
        CAPTURE(what);
        const Hash128 other = makeDdcKey(v);
        CHECK(other != k);
        all.push_back(other);
    }
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());

    // Irrelevant changes: the file's path spelling and its time are not inputs, and settings resolve to
    // one canonical text however they are written.
    TempDir dir;
    writeText(dir.path, "a/hull.png", "source bytes");
    writeText(dir.path, "b/c/Hull Copy.png", "source bytes");
    const Hash128 h1 = hashSourceFile(dir.path / "a" / "hull.png").value();
    const Hash128 h2 = hashSourceFile(dir.path / "b" / "." / "c" / ".." / "c" / "Hull Copy.png").value();
    REQUIRE(fs::setLastWriteTime(dir.path / "a" / "hull.png",
                                 std::filesystem::file_time_type::clock::now() - std::chrono::hours(100)));
    CHECK(hashSourceFile(dir.path / "a" / "hull.png").value() == h1);
    CHECK(h1 == h2);
    CHECK(h1 == base.sourceHash);
    const ImporterRegistry r = makeRegistry();
    const std::string s1 = resolveSettings(*r.find("png"), R"({"mips": false, "maxSize": 2048})").value();
    const std::string s2 = resolveSettings(*r.find("png"), "{ /* c */ \"mips\":false }").value();
    CHECK(s1 == s2);
    DdcKeyInputs viaFile = base;
    viaFile.sourceHash = h2;
    viaFile.settings = s2;
    CHECK(makeDdcKey(viaFile) == k);
    CHECK(hashSourceFile(dir.path / "missing.png").error().code == ErrorCode::NotFound);

    // Streaming matches one-shot hashing across the 1 MiB read size.
    const std::vector<u8> big = noise(2 * kMiB + 17, 3);
    REQUIRE(fs::writeFile(dir.path / "big.bin", big));
    CHECK(hashSourceFile(dir.path / "big.bin").value() == hash128(big.data(), big.size()));
}

TEST_CASE("ddc key: the documented preimage layout") {
    // Recompute the key from the layout in ddc.h, so the documentation and the code cannot drift.
    const Hash128 dep = keyOf(7);
    DdcKeyInputs in;
    in.builder = "gltf";
    in.builderVersion = 5;
    in.sourceHash = Hash128{0x0102030405060708ull, 0x1112131415161718ull};
    in.settings = R"({"a":1})";
    in.settingsLayout = 0xAABBCCDDEEFF0011ull;
    in.platform = asset::HpakPlatform::Server;
    in.cookerVersion = 9;
    in.dependencies = std::span<const Hash128>(&dep, 1);
    std::vector<u8> pre(80, 0);
    storeLE<u32>(pre.data() + 0, 0x4B444448u);
    storeLE<u32>(pre.data() + 4, 0);
    storeLE<u32>(pre.data() + 8, 5);
    storeLE<u32>(pre.data() + 12, 9);
    storeLE<u32>(pre.data() + 16, 2);
    storeLE<u64>(pre.data() + 24, in.sourceHash.low);
    storeLE<u64>(pre.data() + 32, in.sourceHash.high);
    const Hash128 sh = hash128(in.settings);
    storeLE<u64>(pre.data() + 40, sh.low);
    storeLE<u64>(pre.data() + 48, sh.high);
    storeLE<u64>(pre.data() + 56, in.settingsLayout);
    storeLE<u64>(pre.data() + 64, 4);
    storeLE<u64>(pre.data() + 72, 1);
    pre.insert(pre.end(), {'g', 'l', 't', 'f'});
    u8 d[16];
    storeLE<u64>(d, dep.low);
    storeLE<u64>(d + 8, dep.high);
    pre.insert(pre.end(), d, d + 16);
    CHECK(makeDdcKey(in) == hash128(pre.data(), pre.size()));
}

TEST_CASE("ddc entry: encode and read back; every damaged field is an error, never data") {
    const Hash128 key = keyOf(11);
    const std::vector<u8> payload = bytesOf("cooked product");
    const std::vector<u8> entry = ddc::encodeEntry(key, payload).value();
    REQUIRE(entry.size() == ddc::kEntryHeaderBytes + payload.size());
    const auto read = ddc::readEntry(entry, key);
    REQUIRE(read);
    CHECK(std::vector<u8>(read->begin(), read->end()) == payload);
    CHECK(ddc::readEntry(ddc::encodeEntry(key, {}).value(), key).value().empty());

    const auto code = [&](const std::vector<u8>& e) { return ddc::readEntry(e, key).error().code; };
    const auto resealed = [](std::vector<u8> e) {
        storeLE<u64>(e.data() + 56, hash64(e.data(), ddc::kEntryHeaderHashedBytes));
        return e;
    };
    CHECK(ddc::readEntry(entry, keyOf(12)).error().code == ErrorCode::Corrupt); // another key's entry
    for (usize n : {usize(0), usize(1), usize(63)}) {
        CAPTURE(n);
        CHECK(code(std::vector<u8>(entry.begin(), entry.begin() + static_cast<isize>(n))) ==
              ErrorCode::EndOfFile);
    }
    CHECK(code(std::vector<u8>(entry.begin(), entry.end() - 1)) == ErrorCode::EndOfFile); // truncated payload
    std::vector<u8> e = entry;
    e.push_back(0);
    CHECK(code(e) == ErrorCode::Corrupt); // trailing bytes
    e = entry;
    e[0] ^= 1;
    CHECK(code(e) == ErrorCode::Corrupt); // magic
    e = entry;
    storeLE<u16>(e.data() + 4, 1);
    CHECK(code(resealed(e)) == ErrorCode::VersionMismatch);
    e = entry;
    storeLE<u16>(e.data() + 6, 32);
    CHECK(code(resealed(e)) == ErrorCode::Corrupt); // header size
    for (usize at : {usize(8), usize(24), usize(33), usize(47), usize(55)}) {
        CAPTURE(at);
        e = entry;
        e[at] ^= 0x40;
        CHECK(code(e) == ErrorCode::Corrupt); // header checksum
    }
    e = entry;
    storeLE<u32>(e.data() + 48, 1);
    CHECK(code(resealed(e)) == ErrorCode::Corrupt); // flags
    e = entry;
    storeLE<u32>(e.data() + 52, 1);
    CHECK(code(resealed(e)) == ErrorCode::Corrupt); // reserved
    e = entry;
    storeLE<u64>(e.data() + 24, ddc::kMaxPayload + 1);
    CHECK(code(resealed(e)) == ErrorCode::LimitExceeded); // claim above the cap, refused from the header
    e = entry;
    storeLE<u64>(e.data() + 24, ~0ull);
    CHECK(code(resealed(e)) == ErrorCode::LimitExceeded);
    e = entry;
    storeLE<u64>(e.data() + 24, payload.size() + 1);
    CHECK(code(resealed(e)) == ErrorCode::EndOfFile); // claims more than is there
    e = entry;
    e.back() ^= 1;
    CHECK(code(e) == ErrorCode::Corrupt); // payload checksum
}

TEST_CASE("local ddc: put and get, misses, damaged entries are misses, foreign files are ignored") {
    TempDir dir;
    auto ddc = openStore(dir.path / "ddc");
    const Hash128 key = keyOf(21);
    CHECK(ddc->get(key).error().code == ErrorCode::NotFound);
    const std::vector<u8> payload = noise(100'000, 1);
    REQUIRE(ddc->put(key, payload));
    CHECK(ddc->get(key).value() == payload);
    CHECK(ddc->entryPath(key) == dir.path / "ddc" / key.toHex().substr(0, 2) / (key.toHex() + ".hddc"));
    CHECK(fs::fileSize(ddc->entryPath(key)).value() == payload.size() + 64);
    REQUIRE(ddc->put(keyOf(22), {}));
    CHECK(ddc->get(keyOf(22)).value().empty());
    CHECK(ddc->sizeBytes() == payload.size() + 128);
    const DdcStats s = ddc->stats();
    CHECK(s.hits == 2);
    CHECK(s.misses == 1);
    CHECK(s.puts == 2);
    CHECK(ddc->put(key, std::span<const u8>(payload.data(), 0)) /* replace */);
    CHECK(ddc->get(key).value().empty());
    REQUIRE(ddc->put(key, payload));

    // Damage on disk: a miss with an error, never other bytes.
    const fs::Path path = ddc->entryPath(key);
    std::vector<u8> raw = fs::readFile(path).value();
    struct Damage {
        const char* what;
        std::vector<u8> bytes;
        ErrorCode code;
    };
    std::vector<Damage> damages;
    damages.push_back(
        {"truncated payload", std::vector<u8>(raw.begin(), raw.end() - 100), ErrorCode::EndOfFile});
    damages.push_back(
        {"truncated header", std::vector<u8>(raw.begin(), raw.begin() + 30), ErrorCode::EndOfFile});
    damages.push_back({"empty file", {}, ErrorCode::EndOfFile});
    std::vector<u8> flipped = raw;
    flipped[64 + 50'000] ^= 4;
    damages.push_back({"flipped payload bit", flipped, ErrorCode::Corrupt});
    std::vector<u8> longer = raw;
    longer.push_back(7);
    damages.push_back({"trailing byte", longer, ErrorCode::Corrupt});
    damages.push_back(
        {"another key's entry", ddc::encodeEntry(keyOf(99), payload).value(), ErrorCode::Corrupt});
    for (const Damage& d : damages) {
        CAPTURE(d.what);
        REQUIRE(fs::writeFile(path, d.bytes));
        const auto got = ddc->get(key);
        REQUIRE(!got);
        CHECK(got.error().code == d.code);
    }
    CHECK(ddc->stats().bad == damages.size());
    REQUIRE(ddc->put(key, payload)); // the next put replaces a damaged entry
    CHECK(ddc->get(key).value() == payload);

    // A store opened over existing entries measures them; files it does not own are never counted or touched.
    writeText(dir.path / "ddc", "notes.txt", "mine");
    writeText(dir.path / "ddc", "ab/not-an-entry.hddc", "x");
    writeText(dir.path / "ddc", "zz/0123456789abcdef0123456789abcdef.hddc", "x");
    auto reopened = openStore(dir.path / "ddc");
    CHECK(reopened->sizeBytes() == payload.size() + 128);
    CHECK(reopened->get(key).value() == payload);
    LocalDdcOptions bad;
    CHECK(LocalDdc::open(bad).error().code == ErrorCode::InvalidArgument);
    bad.root = dir.path / "x";
    bad.capBytes = 0;
    CHECK(LocalDdc::open(bad).error().code == ErrorCode::InvalidArgument);
    bad.capBytes = 1;
    bad.trimTargetPercent = 101;
    CHECK(LocalDdc::open(bad).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("local ddc: concurrent writers and readers of the same keys never see a partial entry") {
    TempDir dir;
    auto ddc = openStore(dir.path / "ddc");
    constexpr int kThreads = 4;
    constexpr int kRounds = 40;
    // Each key has one payload (content addressed): writers race to store it, readers race to read it.
    std::vector<std::vector<u8>> payloads;
    for (u64 k = 0; k < 4; ++k) payloads.push_back(noise(64 * 1024 + k * 1000, 100 + k));
    std::atomic<int> wrong{0};
    std::atomic<int> failedPuts{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kRounds; ++i) {
                const u64 k = static_cast<u64>((i + t) % 4);
                if (t % 2 == 0) { // threads 0 and 2 write every key, 1 and 3 read them
                    if (!ddc->put(keyOf(1000 + k), payloads[k])) failedPuts.fetch_add(1);
                } else if (auto got = ddc->get(keyOf(1000 + k)); got && *got != payloads[k]) {
                    wrong.fetch_add(1);
                } else if (!got && got.error().code != ErrorCode::NotFound) {
                    wrong.fetch_add(1); // never a damaged entry: renames are atomic
                }
            }
        });
    }
    for (std::thread& t : threads) t.join();
    CHECK(wrong.load() == 0);
    CHECK(failedPuts.load() == 0);
    for (u64 k = 0; k < 4; ++k) CHECK(ddc->get(keyOf(1000 + k)).value() == payloads[k]);
    // No temp file is left behind.
    const std::vector<fs::DirEntry> files = fs::listDirectory(dir.path / "ddc", {.recursive = true}).value();
    for (const fs::DirEntry& e : files) {
        CAPTURE(e.relativePath);
        CHECK(e.relativePath.find(".tmp-") == std::string::npos);
    }
}

TEST_CASE("local ddc: LRU eviction under the cap, hits refresh recency, stale temps go, foreign files stay") {
    TempDir dir;
    const fs::Path root = dir.path / "ddc";
    LocalDdcOptions o;
    o.root = root;
    o.capBytes = 10 * (1000 + 64); // ten 1000-byte entries
    o.trimTargetPercent = 50;      // trim to five
    o.touchInterval = std::chrono::seconds(0);
    auto ddc = LocalDdc::open(o).value();
    const auto now = std::filesystem::file_time_type::clock::now();
    for (u64 k = 0; k < 10; ++k) {
        REQUIRE(ddc->put(keyOf(k), noise(1000, k)));
        // Entry k was last used k minutes after entry 0 (older = smaller k).
        REQUIRE(fs::setLastWriteTime(ddc->entryPath(keyOf(k)), now - std::chrono::minutes(60 - k)));
    }
    CHECK(ddc->sizeBytes() == 10 * 1064);
    // Using entries 0 and 1 makes them the most recent.
    REQUIRE(ddc->get(keyOf(0)));
    REQUIRE(ddc->get(keyOf(1)));
    // Foreign files and a crashed writer's temp files.
    writeText(root, "readme.txt", "not the DDC's");
    writeText(root, "00/keep-me.bin", "not the DDC's");
    const std::string tempName = keyOf(50).toHex() + ".hddc.tmp-" + Guid::generate().toString();
    const std::string freshTemp = keyOf(51).toHex() + ".hddc.tmp-" + Guid::generate().toString();
    writeText(root, keyOf(50).toHex().substr(0, 2) + "/" + tempName, "partial");
    writeText(root, keyOf(51).toHex().substr(0, 2) + "/" + freshTemp, "in flight");
    REQUIRE(
        fs::setLastWriteTime(root / keyOf(50).toHex().substr(0, 2) / tempName, now - std::chrono::hours(2)));

    // The eleventh put passes the cap: least recently used first, down to half the cap.
    REQUIRE(ddc->put(keyOf(10), noise(1000, 10)));
    std::vector<u64> kept;
    for (u64 k = 0; k <= 10; ++k) {
        if (fs::exists(ddc->entryPath(keyOf(k)))) kept.push_back(k);
    }
    CHECK(kept == std::vector<u64>{0, 1, 8, 9, 10});
    CHECK(ddc->sizeBytes() == 5 * 1064);
    CHECK(ddc->stats().evicted == 6);
    CHECK(ddc->stats().evictedBytes == 6 * 1064);
    CHECK(fileExists(root, "readme.txt"));
    CHECK(fileExists(root, "00/keep-me.bin"));
    CHECK(!fileExists(root, keyOf(50).toHex().substr(0, 2) + "/" + tempName)); // stale: removed
    CHECK(fileExists(root, keyOf(51).toHex().substr(0, 2) + "/" + freshTemp)); // may be a live writer's

    // An explicit trim below the target evicts nothing.
    const TrimResult t = ddc->trim().value();
    CHECK(t.evicted == 0);
    CHECK(t.entries == 5);
    CHECK(t.bytes == 5 * 1064);
}

TEST_CASE("local ddc: a hit refreshes recency at most once per touch interval") {
    TempDir dir;
    LocalDdcOptions o;
    o.root = dir.path;
    o.touchInterval = std::chrono::hours(1);
    auto ddc = LocalDdc::open(o).value();
    REQUIRE(ddc->put(keyOf(1), bytesOf("x")));
    const fs::Path path = ddc->entryPath(keyOf(1));
    const auto now = std::filesystem::file_time_type::clock::now();
    REQUIRE(fs::setLastWriteTime(path, now - std::chrono::minutes(30)));
    REQUIRE(ddc->get(keyOf(1)));
    CHECK(fs::lastWriteTime(path).value() < now - std::chrono::minutes(29)); // within the interval: untouched
    REQUIRE(fs::setLastWriteTime(path, now - std::chrono::minutes(90)));
    REQUIRE(ddc->get(keyOf(1)));
    CHECK(fs::lastWriteTime(path).value() >= now - std::chrono::minutes(1)); // older: refreshed
}

} // namespace
