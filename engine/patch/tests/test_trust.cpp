// The trust chain (trust.h, cdn.h): the shared CL-14 vectors (services/testdata/vectors/trust/, written by
// Go's pkg/patchcdn), the shared syntax vectors, single-byte tampering of every signed byte, canonical round
// trips of Go-written documents, the test-only roots, the ratchet record and the CDN read path.
#include <doctest/doctest.h>
#include <monocypher-ed25519.h>
#include <zstd.h>

#include <map>
#include <optional>
#include <set>

#include "helios/patch/cdn.h"
#include "helios/patch/trust.h"
#include "patch_test_util.h"

using namespace helios;
using namespace helios::patch;

namespace {

fs::Path trustDir() { return test::vectorsDir() / "trust"; }

std::vector<u8> readVector(std::string_view name) {
    const Result<std::vector<u8>> b = fs::readFile(trustDir() / fs::pathFromUtf8(name));
    REQUIRE_MESSAGE(b.ok(), name);
    return *b;
}

std::span<const u8> bytesOf(std::string_view s) { return {reinterpret_cast<const u8*>(s.data()), s.size()}; }

/// The public key of the test-only key `name` (Go: package trusttest): Ed25519 from the seed
/// BLAKE2b-256("helios test-only key: " + name).
PublicKey testKey(std::string_view name) {
    const std::string preimage = std::string("helios test-only key: ") + std::string(name);
    Hash256 seed = blake2b256(bytesOf(preimage));
    u8 secret[64];
    PublicKey pub;
    crypto_ed25519_key_pair(secret, pub.data(), seed.bytes.data()); // wipes the seed copy
    crypto_wipe(secret, sizeof(secret));
    return pub;
}

PublicKey keyFromHex(const std::string& hex) {
    const std::vector<u8> b = test::fromHex(hex);
    REQUIRE(b.size() == kPublicKeySize);
    PublicKey k;
    std::copy(b.begin(), b.end(), k.begin());
    return k;
}

struct Vectors {
    test::Json json;
    TrustTarget target;
    RootPair roots;
    u64 now = 0;
    TrustState state;
};

TrustState stateOf(yyjson_val* v) {
    TrustState s;
    s.rootEpoch = static_cast<u32>(test::getU64(v, "rootEpoch"));
    s.keysetVersion = test::getU64(v, "keysetVersion");
    s.pointerSequence = test::getU64(v, "pointerSequence");
    return s;
}

Vectors loadVectors() {
    Vectors v;
    v.json = test::loadJson(trustDir() / "cases.json");
    yyjson_val* root = v.json.root();
    v.target = {test::getStr(root, "product"), test::getStr(root, "channel"), test::getStr(root, "platform")};
    yyjson_val* roots = test::get(root, "roots");
    v.roots.epoch = static_cast<u32>(test::getU64(roots, "epoch"));
    v.roots.current = keyFromHex(test::getStr(roots, "current"));
    v.roots.next = keyFromHex(test::getStr(roots, "next"));
    v.now = test::getU64(root, "now");
    v.state = stateOf(test::get(root, "state"));
    return v;
}

TrustVerifier verifierFor(const Vectors& v) {
    REQUIRE_FALSE(TrustVerifier::create(v.target, v.roots).ok()); // test-only roots need allowTestKeys
    TrustOptions o;
    o.allowTestKeys = true;
    Result<TrustVerifier> verifier = TrustVerifier::create(v.target, v.roots, o);
    REQUIRE(verifier.ok());
    return std::move(*verifier);
}

/// A case's artefacts over the CDN tree (Go: the overlay in pkg/patchcdn's vector test).
struct Overlay {
    std::vector<u8> keyset, pointer;
    std::optional<std::vector<u8>> manifest;
    std::map<std::string, std::optional<std::vector<u8>>> chunks; // ID -> bytes, or nullopt = missing
};

CdnFetch overlayFetch(const Overlay& o, const TrustTarget& t) {
    const CdnFetch base = localCdn(trustDir() / "cdn");
    return [&o, t, base](std::string_view path, u64 limit) -> Result<std::vector<u8>> {
        const std::vector<u8>* b = nullptr;
        if (path == cdn::keysetPath(t.productId)) {
            b = &o.keyset;
        } else if (path == cdn::pointerPath(t.productId, t.channel, t.platform)) {
            b = &o.pointer;
        } else if (path.starts_with("manifests/") && o.manifest) {
            b = &*o.manifest;
        } else if (path.starts_with("chunks/")) {
            const std::string_view file = path.substr(path.rfind('/') + 1);
            const auto it = o.chunks.find(std::string(file.substr(0, file.size() - 4)));
            if (it != o.chunks.end()) {
                if (!it->second) return makeError(ErrorCode::NotFound, "{}: not found", path);
                b = &*it->second;
            }
        }
        if (!b) return base(path, limit);
        if (b->size() > limit) return makeError(ErrorCode::LimitExceeded, "{} is too large", path);
        return *b;
    };
}

std::vector<u8> cdnFile(const std::string& path) {
    const Result<std::vector<u8>> b = localCdn(trustDir() / "cdn")(path, u64(1) << 30);
    REQUIRE_MESSAGE(b.ok(), path);
    return *b;
}

Overlay caseOverlay(const Vectors& v, yyjson_val* c) {
    Overlay o;
    const auto fileOr = [&](const char* key, const std::string& cdnPath) {
        yyjson_val* f = test::get(c, key);
        return f ? readVector(yyjson_get_str(f)) : cdnFile(cdnPath);
    };
    o.keyset = fileOr("keyset", cdn::keysetPath(v.target.productId));
    o.pointer = fileOr("pointer", cdn::pointerPath(v.target.productId, v.target.channel, v.target.platform));
    if (yyjson_val* m = test::get(c, "manifest")) o.manifest = readVector(yyjson_get_str(m));
    if (yyjson_val* chunks = test::get(c, "chunks")) {
        usize idx, max;
        yyjson_val *k, *val;
        yyjson_obj_foreach(chunks, idx, max, k, val) {
            const std::string file(yyjson_get_str(val));
            o.chunks[yyjson_get_str(k)] = file.empty() ? std::nullopt : std::optional(readVector(file));
        }
    }
    const Result<Pointer> cdnPointer =
        parsePointer(cdnFile(cdn::pointerPath(v.target.productId, v.target.channel, v.target.platform)));
    REQUIRE(cdnPointer.ok());
    if (yyjson_val* edits = test::get(c, "edits")) {
        usize idx, max;
        yyjson_val* e;
        yyjson_arr_foreach(edits, idx, max, e) {
            const std::string art = test::getStr(e, "artefact");
            std::vector<u8>* target = nullptr;
            if (art == "keyset") {
                target = &o.keyset;
            } else if (art == "pointer") {
                target = &o.pointer;
            } else if (art == "manifest") {
                if (!o.manifest)
                    o.manifest = cdnFile(
                        cdn::manifestPath(v.target.productId, cdnPointer->ref.buildId, v.target.platform));
                target = &*o.manifest;
            } else {
                REQUIRE(art == "chunk");
                const std::string id = test::getStr(e, "chunk");
                if (!o.chunks.contains(id)) o.chunks[id] = cdnFile(cdn::chunkPath(*Hash256::fromHex(id)));
                REQUIRE(o.chunks[id].has_value());
                target = &*o.chunks[id];
            }
            const std::vector<u8> bytes = test::fromHex(test::getStr(e, "hex"));
            const usize at = static_cast<usize>(test::getU64(e, "at"));
            yyjson_val* ins = test::get(e, "insert");
            if (ins && yyjson_get_bool(ins)) {
                target->insert(target->begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
            } else {
                REQUIRE(at + bytes.size() <= target->size());
                std::copy(bytes.begin(), bytes.end(), target->begin() + static_cast<std::ptrdiff_t>(at));
            }
        }
    }
    return o;
}

} // namespace

TEST_CASE("trust: the shared CL-14 vectors (each rejection fails exactly its check)") {
    const Vectors v = loadVectors();
    const TrustVerifier verifier = verifierFor(v);
    std::set<std::string> seen;
    usize cases = 0;
    yyjson_val* list = test::get(v.json.root(), "cases");
    usize idx, max;
    yyjson_val* c;
    yyjson_arr_foreach(list, idx, max, c) {
        const std::string name = test::getStr(c, "name");
        const std::string want = test::getStr(c, "check");
        CAPTURE(name);
        const Overlay o = caseOverlay(v, c);
        MemoryTrustStateStore store(test::get(c, "state") ? stateOf(test::get(c, "state")) : v.state);
        const u64 now = test::getU64(c, "now", v.now);
        const Result<VerifiedChannel> r = verifyChannel(overlayFetch(o, v.target), verifier, now, store);
        if (want.empty()) {
            REQUIRE_MESSAGE(r.ok(), r.error().message);
            CHECK(r->chunks == r->manifest.chunks.size());
            CHECK(r->chunks > 0);
            if (yyjson_val* ws = test::get(c, "wantState")) CHECK(store.state() == stateOf(ws));
        } else {
            REQUIRE_FALSE(r.ok());
            const std::optional<TrustCheck> check = trustCheckOf(r.error());
            REQUIRE_MESSAGE(check.has_value(), r.error().message);
            CHECK_MESSAGE(trustCheckName(*check) == want, r.error().message);
        }
        seen.insert(want);
        ++cases;
    }
    CHECK(cases >= 50);
    // Every check is the one some case fails, so removing any check fails this test.
    for (usize i = 0; i < static_cast<usize>(TrustCheck::Count); ++i)
        CHECK_MESSAGE(seen.contains(std::string(trustCheckName(static_cast<TrustCheck>(i)))),
                      trustCheckName(static_cast<TrustCheck>(i)));
}

TEST_CASE("trust: the shared syntax vectors are refused by both parsers") {
    const Vectors v = loadVectors();
    const TrustVerifier verifier = verifierFor(v);
    const std::vector<u8> ksDoc = cdnFile(cdn::keysetPath(v.target.productId));
    const std::vector<u8> ptrDoc =
        cdnFile(cdn::pointerPath(v.target.productId, v.target.channel, v.target.platform));
    const Result<Keyset> ks = verifier.verifyKeyset(ksDoc, {});
    REQUIRE(ks.ok());
    const test::Json j = test::loadJson(trustDir() / "syntax.json");
    const auto apply = [](const std::vector<u8>& base, yyjson_val* c) {
        const usize at = static_cast<usize>(test::getU64(c, "at"));
        const usize del = static_cast<usize>(test::getU64(c, "delete"));
        REQUIRE(at + del <= base.size());
        std::vector<u8> put;
        if (yyjson_val* p = test::get(c, "put"))
            put.assign(yyjson_get_str(p), yyjson_get_str(p) + yyjson_get_len(p));
        if (yyjson_val* p = test::get(c, "putHex")) put = test::fromHex(yyjson_get_str(p));
        std::vector<u8> out(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(at));
        out.insert(out.end(), put.begin(), put.end());
        out.insert(out.end(), base.begin() + static_cast<std::ptrdiff_t>(at + del), base.end());
        const usize padTo = static_cast<usize>(test::getU64(c, "padTo"));
        if (out.size() < padTo) out.resize(padTo, ' ');
        return out;
    };
    usize n = 0;
    for (const char* kind : {"keyset", "pointer"}) {
        usize idx, max;
        yyjson_val* c;
        yyjson_arr_foreach(test::get(j.root(), kind), idx, max, c) {
            const std::string name = test::getStr(c, "name");
            CAPTURE(kind);
            CAPTURE(name);
            if (std::string_view(kind) == "keyset") {
                const std::vector<u8> doc = apply(ksDoc, c);
                CHECK_FALSE(parseKeyset(doc).ok());
                const Result<Keyset> r = verifier.verifyKeyset(doc, {});
                REQUIRE_FALSE(r.ok());
                CHECK(trustCheckOf(r.error()) == TrustCheck::KeysetMalformed);
            } else {
                const std::vector<u8> doc = apply(ptrDoc, c);
                CHECK_FALSE(parsePointer(doc).ok());
                const Result<Pointer> r = verifier.verifyPointer(doc, *ks, v.now, {});
                REQUIRE_FALSE(r.ok());
                CHECK(trustCheckOf(r.error()) == TrustCheck::PointerMalformed);
            }
            ++n;
        }
    }
    CHECK(n >= 60);
}

TEST_CASE("trust: every single-byte change of a signed byte is rejected") {
    const Vectors v = loadVectors();
    const TrustVerifier verifier = verifierFor(v);
    Overlay base;
    base.keyset = cdnFile(cdn::keysetPath(v.target.productId));
    base.pointer = cdnFile(cdn::pointerPath(v.target.productId, v.target.channel, v.target.platform));
    const Result<Pointer> p = parsePointer(base.pointer);
    REQUIRE(p.ok());
    base.manifest = cdnFile(cdn::manifestPath(v.target.productId, p->ref.buildId, v.target.platform));
    usize tried = 0;
    for (int art = 0; art < 3; ++art) {
        std::vector<u8>& doc = art == 0 ? base.keyset : art == 1 ? base.pointer : *base.manifest;
        const usize n = art == 2 ? hman::kHeaderSize : doc.size();
        for (usize i = 0; i < n; ++i) {
            for (const u8 x : {u8(0x01), u8(0x20), u8(0x80)}) {
                doc[i] = static_cast<u8>(doc[i] ^ x);
                MemoryTrustStateStore store(v.state);
                const Result<VerifiedChannel> r =
                    verifyChannel(overlayFetch(base, v.target), verifier, v.now, store);
                doc[i] = static_cast<u8>(doc[i] ^ x);
                CAPTURE(art);
                CAPTURE(i);
                REQUIRE_FALSE(r.ok());
                REQUIRE(trustCheckOf(r.error()).has_value());
                ++tried;
            }
        }
    }
    MemoryTrustStateStore store(v.state);
    CHECK(verifyChannel(overlayFetch(base, v.target), verifier, v.now, store).ok()); // restored
    CHECK(tried > 3 * (900 + 700 + 352));
}

TEST_CASE("trust: Go's documents round-trip through the C++ parser and encoder byte for byte") {
    usize docs = 0;
    const Result<std::vector<fs::DirEntry>> files = fs::listDirectory(trustDir());
    REQUIRE(files.ok());
    for (const fs::DirEntry& e : *files) {
        const std::string name = fs::pathToUtf8(e.path.filename());
        if (!name.ends_with(".json") || name == "cases.json" || name == "syntax.json") continue;
        CAPTURE(name);
        const std::vector<u8> doc = readVector(name);
        if (name.starts_with("keyset")) {
            const Result<Keyset> k = parseKeyset(doc);
            REQUIRE_MESSAGE(k.ok(), k.error().message);
            CHECK(encodeKeyset(*k) == doc);
        } else {
            const Result<Pointer> p = parsePointer(doc);
            REQUIRE_MESSAGE(p.ok(), p.error().message);
            CHECK(encodePointer(*p) == doc);
            const std::vector<u8> msg = pointerSignedMessage(*p);
            CHECK(std::string_view(reinterpret_cast<const char*>(msg.data()), 18) == "HELIOS-POINTER-V0\n");
        }
        ++docs;
    }
    CHECK(docs >= 30);
}

TEST_CASE("trust: the test-only roots are refused unless a test allows them") {
    const Vectors v = loadVectors();
    CHECK(testKey("root-1") == v.roots.current); // C++ and Go derive the same keys from the same seeds
    CHECK(testKey("root-2") == v.roots.next);
    for (const char* n : {"root-1", "root-2", "root-x"}) CHECK(isTestOnlyKey(testKey(n)));
    CHECK_FALSE(isTestOnlyKey(testKey("manifest-a")));
    RootPair rp;
    rp.epoch = 1;
    rp.current = testKey("some product root");
    rp.next = testKey("root-x");
    CHECK_FALSE(TrustVerifier::create(v.target, rp).ok());
    rp.next = testKey("another product root");
    CHECK(TrustVerifier::create(v.target, rp).ok());
    // Invalid targets and pairs.
    CHECK_FALSE(TrustVerifier::create({"Bad", "live", "win64"}, rp).ok());
    CHECK_FALSE(TrustVerifier::create({"vector-game", "l", "win64"}, rp).ok());
    rp.next = rp.current;
    CHECK_FALSE(TrustVerifier::create(v.target, rp).ok());
    rp.next = testKey("another product root");
    rp.epoch = 0;
    CHECK_FALSE(TrustVerifier::create(v.target, rp).ok());
    rp.epoch = 0xFFFFFFFFu;
    CHECK_FALSE(TrustVerifier::create(v.target, rp).ok());
}

TEST_CASE("trust: ratchet state record, file store and advance") {
    TrustState s;
    s.rootEpoch = 3;
    s.keysetVersion = 9;
    s.pointerSequence = u64(1) << 40;
    const auto rec = encodeTrustState(s);
    REQUIRE(decodeTrustState(rec).ok());
    CHECK(*decodeTrustState(rec) == s);
    for (usize i = 0; i < rec.size(); ++i) {
        auto bad = rec;
        bad[i] = static_cast<u8>(bad[i] ^ 0x10);
        CHECK_FALSE(decodeTrustState(bad).ok());
    }
    CHECK_FALSE(decodeTrustState(std::span<const u8>(rec.data(), 31)).ok());

    const Result<fs::Path> dir = fs::createUniqueTempDirectory("helios-trust");
    REQUIRE(dir.ok());
    FileTrustStateStore store(*dir / "sub" / "state.bin");
    CHECK(*store.load() == TrustState{});
    REQUIRE(fs::createDirectories(*dir / "sub").ok());
    REQUIRE(store.save(s).ok());
    CHECK(*store.load() == s);
    REQUIRE(fs::writeFile(*dir / "sub" / "state.bin", std::span<const u8>(rec.data(), 10)).ok());
    CHECK_FALSE(store.load().ok()); // never a silent reset
    (void)fs::removeAll(*dir);

    Keyset ks;
    ks.rootEpoch = 2;
    ks.version = 4;
    Pointer p;
    p.sequence = 8;
    const TrustState st{3, 5, 10};
    CHECK(advanceTrustState(st, ks, p) == st);
    p.rollback = true;
    CHECK(advanceTrustState(st, ks, p) == TrustState{3, 5, 8});
    p.rollback = false;
    p.sequence = 12;
    CHECK(advanceTrustState({}, ks, p) == TrustState{2, 4, 12});
}

TEST_CASE("cdn: paths, the local fetcher and chunk objects") {
    CHECK(cdn::chunkPath(blake2b256({})) ==
          "chunks/0e/57/0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8.zst"); // as Go's
    CHECK(cdn::pointerPath("p-x", "live", "win64") == "channels/p-x/live/win64.json");
    CHECK(cdn::manifestPath("p-x", "b1", "win64") == "manifests/p-x/b1/win64.hman");
    CHECK(cdn::keysetPath("p-x") == "keys/p-x/keyset.json");
    for (const char* bad : {"", "/etc/passwd", "../x", "a/../../x", "a//b", "a\\b", "c:/x", "./a", "a/"})
        CHECK_FALSE(cdn::isValidObjectPath(bad));
    CHECK(cdn::isValidObjectPath("manifests/p/1..2/win64.hman"));

    const CdnFetch fetch = localCdn(trustDir() / "cdn");
    const std::string ks = cdn::keysetPath("vector-game");
    CHECK(fetch(ks, kMaxKeysetSize).ok());
    CHECK(fetch(ks, 10).errorCode() == ErrorCode::LimitExceeded);
    CHECK(fetch("keys/none/keyset.json", 10).errorCode() == ErrorCode::NotFound);
    CHECK(fetch("../trust/cases.json", 1 << 20).errorCode() == ErrorCode::InvalidArgument);

    // A chunk object of the vectors decodes; hostile ones do not.
    const Result<std::vector<u8>> sub = fs::readFile(trustDir() / "chunk-substituted.zst");
    REQUIRE(sub.ok());
    const std::vector<u8>& stored = *sub;
    const unsigned long long rawSize = ZSTD_getFrameContentSize(stored.data(), stored.size());
    REQUIRE(rawSize < (1u << 20));
    const u32 n = static_cast<u32>(rawSize);
    CHECK(decodeChunkObject(stored, n).ok());
    CHECK_FALSE(decodeChunkObject(stored, n - 1).ok());
    CHECK_FALSE(decodeChunkObject(stored, n + 1).ok());
    CHECK_FALSE(decodeChunkObject({}, 1).ok());
    CHECK_FALSE(decodeChunkObject(std::span<const u8>(stored.data(), stored.size() / 2), n).ok());
    std::vector<u8> twice = stored;
    twice.insert(twice.end(), stored.begin(), stored.end());
    CHECK_FALSE(decodeChunkObject(twice, n).ok());
    const std::vector<u8> garbage(100, 0x42);
    CHECK_FALSE(decodeChunkObject(garbage, 100).ok());
}
