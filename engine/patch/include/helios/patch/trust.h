#pragma once
// The patch trust chain (05 §7, 08 §2.5 step 1, 08 §2.10.3): a product's root pair signs its keyset, a
// manifest subkey of the keyset signs each channel's pointer and each build's .hman, the pointer names the
// manifest by hash, and the manifest names every chunk by BLAKE2b-256. This is the launcher's verifier
// (x86-64-v1: portable Monocypher Ed25519, no new dependency); Go's services/pkg/patchtrust signs and
// verifies the same bytes, and the shared vectors in services/testdata/vectors/trust/ pin both
// (engine/patch/README.md, "Trust chain", is the specification).
//
// Keysets and pointers are canonical JSON documents: one encoding per document (no whitespace, members in
// key order, printable-ASCII strings without escapes, integers ≤ 2^53-1), so the bytes on the CDN are the
// bytes a signer signed. The parsers accept nothing else, never recurse and stop at kMaxKeysetSize and
// kMaxPointerSize bytes.
//
// Every rejection is a Result error whose message starts with the failed check's name (TrustCheck,
// trustCheckName(): "pointer-expired: ..."), the same names Go's patchtrust.Check uses. Malformed documents
// fail with Corrupt (LimitExceeded when too large); everything else with PermissionDenied.
//
// Threading: everything is a value type or a pure function; a TrustVerifier is immutable and may be shared;
// a TrustStateStore is the caller's to synchronize.

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/patch/blake2b.h"
#include "helios/patch/manifest.h"

namespace helios::patch {

inline constexpr usize kPublicKeySize = 32;
inline constexpr usize kSignatureSize = 64;
inline constexpr usize kKeyIdSize = hman::kKeyIdSize; ///< 16: the .hman header's keyId.
inline constexpr usize kMaxKeysetSize = 64 * 1024;    ///< Bytes of keyset.json.
inline constexpr usize kMaxPointerSize = 16 * 1024;   ///< Bytes of a pointer.
inline constexpr usize kMaxKeys = 64;                 ///< Subkeys in one keyset.
inline constexpr usize kMaxCdnHosts = 16;
inline constexpr usize kMaxHostLength = 256;
inline constexpr usize kMaxRoleLength = 32;
inline constexpr usize kMaxVersionText = 39;              ///< Four dotted parts of at most 9 digits.
inline constexpr u64 kMaxJsonInt = (u64(1) << 53) - 1;    ///< The largest integer a document holds.
inline constexpr u64 kMaxPointerLifetime = 7 * 24 * 3600; ///< expires - signed_at (08 §2.10.3: 7 days).

/// The subkey roles of 08 §2.10.3. A keyset may name other roles (a later phase's); they never match.
inline constexpr std::string_view kRoleManifest = "manifest"; ///< Pointers and manifests.
inline constexpr std::string_view kRoleNews = "news";         ///< News and status banners (08 §2.4).
inline constexpr std::string_view kRoleAddons = "addons";     ///< The addon portal (08 §1.12).

using PublicKey = std::array<u8, kPublicKeySize>; ///< An Ed25519 public key.
using Signature = std::array<u8, kSignatureSize>; ///< An Ed25519 signature.
using KeyId = std::array<u8, kKeyIdSize>;         ///< The first 16 bytes of BLAKE2b-256 of a public key.

/// A public key's ID (its fingerprint): the first 16 bytes of its BLAKE2b-256.
KeyId keyFingerprint(const PublicKey& pub) noexcept;

/// True for the test-only roots of the shared vectors (Go: package trusttest; seeds
/// BLAKE2b-256("helios test-only key: root-1" / "root-2" / "root-x"), public by construction).
/// TrustVerifier::create refuses a root pair containing one unless TrustOptions::allowTestKeys is set.
bool isTestOnlyKey(const PublicKey& pub) noexcept;

/// One subkey of a keyset.
struct KeysetKey {
    KeyId id;          ///< keyFingerprint(pub).
    std::string role;  ///< ^[a-z][a-z0-9-]{0,31}$.
    PublicKey pub{};   ///<
    u64 notBefore = 0; ///< Unix seconds: the first second the key may sign.
    u64 notAfter = 0;  ///< Unix seconds: the first second it may no longer sign (> notBefore).
    friend bool operator==(const KeysetKey&, const KeysetKey&) = default;
};

/// A product's root-signed subkey list, /keys/<productId>/keyset.json (08 §2.10.3):
///   {"keys":[{"id":H16,"notAfter":N,"notBefore":N,"pub":H32,"role":S},...],"productId":S,"rootEpoch":N,
///    "sig":H64,"version":N}
/// rootEpoch names the signing root (the pair's epoch: current; epoch + 1: next).
struct Keyset {
    std::string productId;
    u64 version = 0;             ///< 1..kMaxJsonInt; only grows.
    u32 rootEpoch = 0;           ///< 1..2^32-2.
    std::vector<KeysetKey> keys; ///< 1..kMaxKeys, sorted by id, unique.
    Signature sig{};
    /// The subkey with this ID, or nullptr. O(log n).
    const KeysetKey* findKey(const KeyId& id) const noexcept;
    friend bool operator==(const Keyset&, const Keyset&) = default;
};

/// A build's manifest as a pointer names it.
struct ManifestRef {
    std::string buildId;
    Hash256 manifestHash; ///< The .hman header hash (the manifest's identity).
    u32 compatEpoch = 0;
    friend bool operator==(const ManifestRef&, const ManifestRef&) = default;
};

/// A pointer's pre-download target (05 §7, 08 §2.6).
struct PointerNext {
    ManifestRef ref;
    u64 availableAt = 0;
    friend bool operator==(const PointerNext&, const PointerNext&) = default;
};

/// A channel's signed pointer, /channels/<product>/<channel>/<platform>.json (05 §7):
///   {"build_id":S,"cdn_hosts":[S,...],"channel":S,"compat_epoch":N,"expires":N,"key_id":H16,
///    "manifest_hash":H32,"min_client":S,"min_launcher":S,"next":{"available_at":N,"build_id":S,
///    "compat_epoch":N,"manifest_hash":H32},"platform":S,"product_id":S,"rollback":B,"rollout_pct":N,
///    "sequence":N,"sig":H64,"signed_at":N}
/// with "next" optional.
struct Pointer {
    std::string productId;
    std::string channel; ///< ^[a-z][a-z0-9-]{1,31}$.
    std::string platform;
    ManifestRef ref; ///< build_id, manifest_hash, compat_epoch.
    u64 sequence = 0;
    std::string minLauncher; ///< ^[0-9]{1,9}(\.[0-9]{1,9}){0,3}$.
    std::string minClient;
    std::vector<std::string> cdnHosts; ///< https://, or http:// on a loopback host.
    std::optional<PointerNext> next;
    u8 rolloutPct = 0;
    bool rollback = false; ///< Lets sequence be below an install's stored one.
    u64 signedAt = 0;
    u64 expires = 0;
    KeyId keyId{};
    Signature sig{};
    friend bool operator==(const Pointer&, const Pointer&) = default;
};

/// Parses a canonical keyset document and validates it (not its signature). Corrupt for anything that is
/// not exactly the canonical encoding of a valid keyset; LimitExceeded above kMaxKeysetSize.
Result<Keyset> parseKeyset(std::span<const u8> doc);
/// Parses a canonical pointer document and validates it (not its signature or lifetime).
Result<Pointer> parsePointer(std::span<const u8> doc);
/// The canonical encoding of a valid keyset, with or without its "sig" member.
std::vector<u8> encodeKeyset(const Keyset& keyset, bool withSig = true);
/// The canonical encoding of a valid pointer, with or without its "sig" member.
std::vector<u8> encodePointer(const Pointer& pointer, bool withSig = true);
/// What a root signs: "HELIOS-KEYSET-V0\n", then the keyset without "sig".
std::vector<u8> keysetSignedMessage(const Keyset& keyset);
/// What a subkey signs: "HELIOS-POINTER-V0\n", then the pointer without "sig".
std::vector<u8> pointerSignedMessage(const Pointer& pointer);

/// The checks of the chain, in the order it runs them (README "Verification order"). The names are Go's.
enum class TrustCheck : u8 {
    KeysetMalformed,     ///< keyset-malformed
    KeysetRoot,          ///< keyset-root: rootEpoch names neither root of the pair
    KeysetRootRatchet,   ///< keyset-root-ratchet: a root the install has moved past
    KeysetSignature,     ///< keyset-signature
    KeysetProduct,       ///< keyset-product
    KeysetVersion,       ///< keyset-version: below the stored version
    PointerMalformed,    ///< pointer-malformed
    PointerKeyUnknown,   ///< pointer-key-unknown: not in the keyset (or revoked)
    PointerKeyRole,      ///< pointer-key-role: not a manifest subkey
    PointerSignature,    ///< pointer-signature
    PointerKeyWindow,    ///< pointer-key-window: signed_at outside the subkey's validity
    PointerLifetime,     ///< pointer-lifetime: expires not within 7 days after signed_at
    PointerProduct,      ///< pointer-product
    PointerChannel,      ///< pointer-channel
    PointerPlatform,     ///< pointer-platform
    PointerExpired,      ///< pointer-expired: now >= expires
    PointerSequence,     ///< pointer-sequence: below the stored one without rollback
    ManifestMalformed,   ///< manifest-malformed: the header does not read
    ManifestHash,        ///< manifest-hash: not the manifest the pointer names
    ManifestKeyUnknown,  ///< manifest-key-unknown
    ManifestKeyRole,     ///< manifest-key-role
    ManifestSignature,   ///< manifest-signature: over bytes [0, 256)
    ManifestKeyWindow,   ///< manifest-key-window: createdAt outside the subkey's validity
    ManifestProduct,     ///< manifest-product
    ManifestPlatform,    ///< manifest-platform
    ManifestBuild,       ///< manifest-build
    ManifestCompatEpoch, ///< manifest-compat-epoch
    ManifestExpired,     ///< manifest-expired: expiresAt set and now >= expiresAt
    ManifestBody,        ///< manifest-body: the payload does not decode to a valid body
    ChunkMissing,        ///< chunk-missing
    ChunkCorrupt,        ///< chunk-corrupt: does not decode to rawSize bytes
    ChunkHash,           ///< chunk-hash: the bytes do not hash to the chunk ID
    Count
};

/// The check's name ("pointer-expired").
std::string_view trustCheckName(TrustCheck check) noexcept;
/// The check an error from this header failed (its message's prefix), or nullopt.
std::optional<TrustCheck> trustCheckOf(const Error& error) noexcept;

/// A product's root public keys: `current` signs keysets of root epoch `epoch`, the pre-committed `next`
/// those of epoch + 1. In product builds they come from the stamped product block (08 §2.10.4, later).
struct RootPair {
    u32 epoch = 0; ///< 1..2^32-2.
    PublicKey current{};
    PublicKey next{};
};

/// What an install verifies for.
struct TrustTarget {
    std::string productId;
    std::string channel;
    std::string platform;
};

/// The ratchets an install persists (08 §2.5: install.db keeps them, WP-0.17). Zero is a fresh install.
struct TrustState {
    u32 rootEpoch = 0;       ///< The highest root epoch accepted.
    u64 keysetVersion = 0;   ///< The highest keyset version accepted.
    u64 pointerSequence = 0; ///< The last accepted pointer's sequence (a rollback pointer lowers it).
    friend bool operator==(const TrustState&, const TrustState&) = default;
};

/// The state after accepting `keyset` and `pointer`: root epoch and keyset version only grow; the pointer
/// sequence grows, or becomes a rollback pointer's.
TrustState advanceTrustState(const TrustState& state, const Keyset& keyset, const Pointer& pointer) noexcept;

inline constexpr usize kTrustStateSize = 32;
/// A fixed 32-byte record: 'HTRS', version 0, rootEpoch, keysetVersion, pointerSequence, and the first 4
/// bytes of BLAKE2b-256 of the first 28 (little-endian).
std::array<u8, kTrustStateSize> encodeTrustState(const TrustState& state) noexcept;
/// Decodes encodeTrustState()'s record; Corrupt for anything else (never a silent reset).
Result<TrustState> decodeTrustState(std::span<const u8> record);

/// Persists an install's ratchets. The launcher implements it on install.db (WP-0.17). save() must be
/// durable before the caller acts on what it verified. Implementations need not be thread-safe.
class TrustStateStore {
public:
    virtual ~TrustStateStore() = default;
    /// The stored state; a store never saved returns the zero state.
    virtual Result<TrustState> load() = 0;
    virtual Result<void> save(const TrustState& state) = 0;
};

/// A TrustStateStore in one file (encodeTrustState's record), replaced atomically through the platform
/// layer. A missing file loads as the zero state; a corrupt one is an error.
class FileTrustStateStore final : public TrustStateStore {
public:
    explicit FileTrustStateStore(fs::Path path) : m_path(std::move(path)) {}
    Result<TrustState> load() override;
    Result<void> save(const TrustState& state) override;

private:
    fs::Path m_path;
};

/// A TrustStateStore in memory (tests, one-off verification).
class MemoryTrustStateStore final : public TrustStateStore {
public:
    explicit MemoryTrustStateStore(TrustState state = {}) : m_state(state) {}
    Result<TrustState> load() override { return m_state; }
    Result<void> save(const TrustState& state) override {
        m_state = state;
        return {};
    }
    const TrustState& state() const noexcept { return m_state; }

private:
    TrustState m_state;
};

struct TrustOptions {
    /// Accept the test-only roots (isTestOnlyKey). Only tests set it.
    bool allowTestKeys = false;
};

/// Verifies keysets, pointers, manifests and chunks for one install, in 05 §7 / 08 §2.5's order. Immutable
/// after create(); every method is const and thread-safe.
class TrustVerifier {
public:
    /// Checks the target's identifiers and the root pair (epoch range, distinct keys, no test-only key
    /// unless options allow it). InvalidArgument otherwise.
    static Result<TrustVerifier> create(TrustTarget target, const RootPair& roots,
                                        const TrustOptions& options = {});

    const TrustTarget& target() const noexcept { return m_target; }

    /// keyset-malformed, keyset-root, keyset-root-ratchet, keyset-signature, keyset-product, keyset-version.
    Result<Keyset> verifyKeyset(std::span<const u8> doc, const TrustState& state) const;
    /// Against a verified keyset, at `now` (unix seconds): pointer-malformed, pointer-key-unknown,
    /// pointer-key-role, pointer-signature, pointer-key-window, pointer-lifetime, pointer-product,
    /// pointer-channel, pointer-platform, pointer-expired, pointer-sequence.
    Result<Pointer> verifyPointer(std::span<const u8> doc, const Keyset& keyset, u64 now,
                                  const TrustState& state) const;
    /// A .hman file's header against a verified keyset and the manifest a verified pointer names (its ref or
    /// its next's): manifest-malformed, manifest-hash, manifest-key-unknown, manifest-key-role,
    /// manifest-signature, manifest-key-window, manifest-product, manifest-platform, manifest-build,
    /// manifest-compat-epoch, manifest-expired. Does not decode the payload.
    Result<ManifestHeaderInfo> verifyManifestHeader(std::span<const u8> file, const Keyset& keyset,
                                                    const ManifestRef& ref, u64 now) const;
    /// verifyManifestHeader(), then the whole manifest (manifest-body).
    Result<Manifest> verifyManifest(std::span<const u8> file, const Keyset& keyset, const ManifestRef& ref,
                                    u64 now) const;
    /// A chunk's decoded bytes against its entry: chunk-corrupt (size), chunk-hash.
    static Result<void> verifyChunk(const ManifestChunk& chunk, std::span<const u8> raw);

private:
    TrustVerifier(TrustTarget target, const RootPair& roots) : m_target(std::move(target)), m_roots(roots) {}
    TrustTarget m_target;
    RootPair m_roots;
};

} // namespace helios::patch
