// Fuzz target: the keyset and pointer parsers and the trust checks behind them (trust.h; CL-14 asks for
// fuzzed parsers, 09 §6).
//
// Each input runs as a keyset, as a pointer and, from 352 bytes, as a .hman header. Properties: every call
// returns a Result (no crash, sanitizer report or out-of-bounds access); a document that parses re-encodes
// to exactly the input (one encoding per document, so a signature covers the only form that verifies);
// what verifies also parses; trustCheckOf() names a check for every verification failure. Verification
// uses fixed test-only keys (the shared vectors' root-1 and root-2, Go package trusttest), so the seeds'
// signatures verify and mutations reach every check behind them.

#include <monocypher-ed25519.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "helios/patch/trust.h"

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace {

using namespace helios;
using namespace helios::patch;

constexpr usize kMaxInput = 128 * 1024;
constexpr u64 kNow = 1'790'000'000;

[[noreturn]] void fail() { std::abort(); }

struct TestKey {
    PublicKey pub{};
    u8 secret[64] = {};
};

TestKey testKey(std::string_view name) {
    const std::string preimage = std::string("helios test-only key: ") + std::string(name);
    Hash256 seed = blake2b256(std::span<const u8>(reinterpret_cast<const u8*>(preimage.data()), preimage.size()));
    TestKey k;
    crypto_ed25519_key_pair(k.secret, k.pub.data(), seed.bytes.data());
    return k;
}

template <typename Doc>
void sign(Doc& doc, const std::vector<u8>& message, const TestKey& key) {
    crypto_ed25519_sign(doc.sig.data(), key.secret, message.data(), message.size());
}

KeysetKey subkey(std::string_view name, std::string role) {
    KeysetKey k;
    k.pub = testKey(name).pub;
    k.id = keyFingerprint(k.pub);
    k.role = std::move(role);
    k.notBefore = kNow - 90 * 86400;
    k.notAfter = kNow + 90 * 86400;
    return k;
}

/// The fixed world: a keyset (root-1, version 3) with a manifest, a news and an unknown-role key.
struct World {
    Keyset keyset;
    std::vector<u8> keysetDoc;
    Pointer pointer;
    TrustVerifier verifier;
};

const World& world() {
    static const World w = [] {
        Keyset ks;
        ks.productId = "vector-game";
        ks.version = 3;
        ks.rootEpoch = 1;
        ks.keys = {subkey("manifest-a", "manifest"), subkey("news-a", "news"), subkey("store-a", "store")};
        std::sort(ks.keys.begin(), ks.keys.end(), [](const KeysetKey& a, const KeysetKey& b) { return a.id < b.id; });
        sign(ks, keysetSignedMessage(ks), testKey("root-1"));
        Pointer p;
        p.productId = "vector-game";
        p.channel = "live";
        p.platform = "win64";
        p.ref = {"2026.09.21-r1", blake2b256(std::span<const u8>()), 4};
        p.sequence = 7;
        p.minLauncher = "1.2.0";
        p.minClient = "1.2.0";
        p.cdnHosts = {"https://cdn1.vector-game.example", "http://127.0.0.1:7700/cdn"};
        p.next = PointerNext{{"2026.09.28-r1", blake2b256(std::span<const u8>()), 5}, kNow + 7 * 86400};
        p.rolloutPct = 100;
        p.signedAt = kNow - 600;
        p.expires = kNow + 6 * 86400;
        p.keyId = keyFingerprint(testKey("manifest-a").pub);
        sign(p, pointerSignedMessage(p), testKey("manifest-a"));
        RootPair roots;
        roots.epoch = 1;
        roots.current = testKey("root-1").pub;
        roots.next = testKey("root-2").pub;
        TrustOptions o;
        o.allowTestKeys = true;
        Result<TrustVerifier> v = TrustVerifier::create({"vector-game", "live", "win64"}, roots, o);
        if (!v) fail();
        return World{ks, encodeKeyset(ks), p, std::move(*v)};
    }();
    return w;
}

template <typename T>
void requireCheck(const Result<T>& r) {
    if (!r.ok() && !trustCheckOf(r.error())) fail();
}

void exercise(std::span<const u8> in) {
    const World& w = world();
    const Result<Keyset> ks = parseKeyset(in);
    if (ks && encodeKeyset(*ks) != std::vector<u8>(in.begin(), in.end())) fail();
    const Result<Keyset> vks = w.verifier.verifyKeyset(in, {});
    requireCheck(vks);
    if (vks.ok() && !ks.ok()) fail();

    const Result<Pointer> p = parsePointer(in);
    if (p && encodePointer(*p) != std::vector<u8>(in.begin(), in.end())) fail();
    const Result<Pointer> vp = w.verifier.verifyPointer(in, w.keyset, kNow, {});
    requireCheck(vp);
    if (vp.ok() && !p.ok()) fail();

    if (in.size() >= hman::kHeaderSize) {
        const Result<ManifestHeaderInfo> info = readManifestHeader(in);
        ManifestRef ref;
        if (info) ref = {info->header.buildId, info->headerHash, info->header.compatEpoch};
        requireCheck(w.verifier.verifyManifestHeader(in, w.keyset, ref, kNow));
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > kMaxInput) return 0;
    exercise(std::span<const u8>(data, size));
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    const World& w = world();
    out.push_back(w.keysetDoc);
    out.push_back(encodePointer(w.pointer));
    Pointer minimal = w.pointer;
    minimal.next.reset();
    minimal.cdnHosts.clear();
    minimal.rollback = true;
    sign(minimal, pointerSignedMessage(minimal), testKey("manifest-a"));
    out.push_back(encodePointer(minimal));
    Pointer news = w.pointer;
    news.keyId = keyFingerprint(testKey("news-a").pub);
    sign(news, pointerSignedMessage(news), testKey("news-a"));
    out.push_back(encodePointer(news));
    Keyset rotated = w.keyset;
    rotated.rootEpoch = 2;
    rotated.version = 4;
    sign(rotated, keysetSignedMessage(rotated), testKey("root-2"));
    out.push_back(encodeKeyset(rotated));
}
