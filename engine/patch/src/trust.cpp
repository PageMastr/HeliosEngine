// The patch trust chain (trust.h): canonical keyset and pointer documents, Ed25519 over Monocypher 4.0.3
// (optional/monocypher-ed25519: RFC 8032 Ed25519 with SHA-512, as Go's crypto/ed25519), and the verifier.
// engine/patch/README.md "Trust chain" is the specification; Go's services/pkg/patchtrust is the twin.
#include "helios/patch/trust.h"

#include <monocypher-ed25519.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>


namespace helios::patch {

namespace {

constexpr std::string_view kKeysetContext = "HELIOS-KEYSET-V0\n";
constexpr std::string_view kPointerContext = "HELIOS-POINTER-V0\n";
constexpr usize kMaxIdText = 64; // product IDs, channels, platforms and build IDs are shorter

constexpr std::string_view kCheckNames[] = {
    "keyset-malformed",      "keyset-root",          "keyset-root-ratchet", "keyset-signature",
    "keyset-product",        "keyset-version",       "pointer-malformed",   "pointer-key-unknown",
    "pointer-key-role",      "pointer-signature",    "pointer-key-window",  "pointer-lifetime",
    "pointer-product",       "pointer-channel",      "pointer-platform",    "pointer-expired",
    "pointer-sequence",      "manifest-malformed",   "manifest-hash",       "manifest-key-unknown",
    "manifest-key-role",     "manifest-signature",   "manifest-key-window", "manifest-product",
    "manifest-platform",     "manifest-build",       "manifest-compat-epoch", "manifest-expired",
    "manifest-body",         "chunk-missing",        "chunk-corrupt",       "chunk-hash",
};
static_assert(std::size(kCheckNames) == static_cast<usize>(TrustCheck::Count));

template <typename... Args>
Error reject(TrustCheck check, std::format_string<Args...> fmt, Args&&... args) {
    const bool malformed = check == TrustCheck::KeysetMalformed || check == TrustCheck::PointerMalformed ||
                           check == TrustCheck::ManifestMalformed || check == TrustCheck::ManifestBody ||
                           check == TrustCheck::ChunkCorrupt;
    ErrorCode code = malformed ? ErrorCode::Corrupt : ErrorCode::PermissionDenied;
    if (check == TrustCheck::ChunkMissing) code = ErrorCode::NotFound;
    return Error{code, std::format("{}: {}", trustCheckName(check), std::format(fmt, std::forward<Args>(args)...))};
}

std::string hexOf(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '0');
    for (usize i = 0; i < bytes.size(); ++i) {
        out[2 * i] = kDigits[bytes[i] >> 4];
        out[2 * i + 1] = kDigits[bytes[i] & 0xF];
    }
    return out;
}

constexpr bool isLowerHex(u8 c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
constexpr u8 hexNibble(u8 c) noexcept { return c <= '9' ? static_cast<u8>(c - '0') : static_cast<u8>(c - 'a' + 10); }
constexpr bool inStringAlphabet(u8 c) noexcept { return c >= 0x20 && c <= 0x7E && c != '"' && c != '\\'; }

// ---------------------------------------------------------------------------------------------
// Canonical reader: schema-directed, so the parsers below expect each member name in order and there is
// nothing to recurse into. The first failure sticks; later calls do nothing.
// ---------------------------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::span<const u8> doc) noexcept : m_doc(doc) {}

    bool ok() const noexcept { return m_error.empty(); }
    const std::string& error() const noexcept { return m_error; }

    template <typename... Args>
    void fail(std::format_string<Args...> fmt, Args&&... args) {
        if (ok()) m_error = std::format("at byte {}: {}", m_pos, std::format(fmt, std::forward<Args>(args)...));
    }

    bool peek(std::string_view s) const noexcept {
        return ok() && m_doc.size() - m_pos >= s.size() && std::memcmp(m_doc.data() + m_pos, s.data(), s.size()) == 0;
    }

    void lit(std::string_view s) {
        if (!ok()) return;
        if (!peek(s)) return fail("expected \"{}\"", s);
        m_pos += s.size();
    }

    /// `"name":`, after ',' unless first.
    void key(std::string_view name, bool first = false) {
        if (!first) lit(",");
        lit("\"");
        lit(name);
        lit("\":");
    }

    std::string str(usize max) {
        lit("\"");
        if (!ok()) return {};
        const usize start = m_pos;
        while (m_pos < m_doc.size() && m_doc[m_pos] != '"') {
            const u8 c = m_doc[m_pos];
            if (!inStringAlphabet(c)) {
                fail("byte 0x{:02x} is not allowed in a string", c);
                return {};
            }
            if (m_pos - start >= max) {
                fail("string longer than {} bytes", max);
                return {};
            }
            ++m_pos;
        }
        std::string out(reinterpret_cast<const char*>(m_doc.data() + start), m_pos - start);
        lit("\"");
        return out;
    }

    u64 uint() {
        if (!ok()) return 0;
        const usize start = m_pos;
        while (m_pos < m_doc.size() && m_doc[m_pos] >= '0' && m_doc[m_pos] <= '9') {
            if (m_pos - start >= 16) {
                fail("integer above {}", kMaxJsonInt);
                return 0;
            }
            ++m_pos;
        }
        const usize n = m_pos - start;
        if (n == 0) {
            fail("expected an unsigned integer");
            return 0;
        }
        if (n > 1 && m_doc[start] == '0') {
            fail("integer with a leading zero");
            return 0;
        }
        u64 v = 0;
        for (usize i = start; i < m_pos; ++i) v = v * 10 + (m_doc[i] - '0'); // ≤ 16 digits: no overflow
        if (v > kMaxJsonInt) {
            fail("integer above {}", kMaxJsonInt);
            return 0;
        }
        return v;
    }

    u32 u32Value(std::string_view name) {
        const u64 v = uint();
        if (ok() && v > 0xFFFFFFFFull) fail("{} {} does not fit 32 bits", name, v);
        return static_cast<u32>(v);
    }

    bool boolean() {
        if (peek("true")) {
            m_pos += 4;
            return true;
        }
        if (peek("false")) {
            m_pos += 5;
            return false;
        }
        fail("expected true or false");
        return false;
    }

    /// A string of exactly 2 * out.size() lowercase hex digits.
    void hex(std::span<u8> out) {
        const std::string s = str(2 * out.size());
        if (!ok()) return;
        if (s.size() != 2 * out.size()) return fail("expected {} hex digits, got {}", 2 * out.size(), s.size());
        for (usize i = 0; i < out.size(); ++i) {
            const u8 hi = static_cast<u8>(s[2 * i]), lo = static_cast<u8>(s[2 * i + 1]);
            if (!isLowerHex(hi) || !isLowerHex(lo)) return fail("\"{}\" is not lowercase hex", s);
            out[i] = static_cast<u8>(hexNibble(hi) << 4 | hexNibble(lo));
        }
    }

    void end() {
        if (ok() && m_pos != m_doc.size()) fail("{} bytes after the document", m_doc.size() - m_pos);
    }

private:
    std::span<const u8> m_doc;
    usize m_pos = 0;
    std::string m_error;
};

class Writer {
public:
    void raw(std::string_view s) { m_out.insert(m_out.end(), s.begin(), s.end()); }
    void key(std::string_view name, bool first = false) {
        if (!first) raw(",");
        raw("\"");
        raw(name);
        raw("\":");
    }
    void str(std::string_view s) {
        raw("\"");
        raw(s);
        raw("\"");
    }
    void uint(u64 v) {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        raw(std::string_view(buf, static_cast<usize>(r.ptr - buf)));
    }
    void boolean(bool v) { raw(v ? "true" : "false"); }
    void hex(std::span<const u8> bytes) { str(hexOf(bytes)); }
    std::vector<u8> take() { return std::move(m_out); }

private:
    std::vector<u8> m_out;
};

// ---------------------------------------------------------------------------------------------
// Validation (Go: Keyset.Validate, Pointer.Validate)
// ---------------------------------------------------------------------------------------------

template <typename... Args>
Error invalid(std::format_string<Args...> fmt, Args&&... args) {
    return Error{ErrorCode::Corrupt, std::format(fmt, std::forward<Args>(args)...)};
}

bool validRole(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxRoleLength || s[0] < 'a' || s[0] > 'z') return false;
    return std::all_of(s.begin() + 1, s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool validChannel(std::string_view s) noexcept { return s.size() >= 2 && validRole(s); }

bool validVersionText(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxVersionText) return false;
    usize parts = 0;
    usize start = 0;
    for (usize i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (i == start || i - start > 9 || ++parts > 4) return false;
            start = i + 1;
        } else if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

bool validHost(std::string_view s) noexcept {
    if (s.empty() || s.size() > kMaxHostLength) return false;
    for (const char c : s)
        if (!inStringAlphabet(static_cast<u8>(c)) || c == ' ') return false;
    if (s.starts_with("https://")) return s.size() > 8;
    for (const std::string_view loop : {"http://localhost", "http://127.0.0.1", "http://[::1]"}) {
        if (s.starts_with(loop)) {
            const std::string_view rest = s.substr(loop.size());
            if (rest.empty() || rest[0] == ':' || rest[0] == '/') return true;
        }
    }
    return false;
}

Result<void> validateKeyset(const Keyset& k) {
    if (!isValidProductId(k.productId)) return invalid("productId \"{}\" is not a product ID", k.productId);
    if (k.version == 0 || k.version > kMaxJsonInt) return invalid("version {} is outside 1..{}", k.version, kMaxJsonInt);
    if (k.rootEpoch == 0 || k.rootEpoch == 0xFFFFFFFFu)
        return invalid("rootEpoch {} is outside 1..{}", k.rootEpoch, 0xFFFFFFFEu);
    if (k.keys.empty() || k.keys.size() > kMaxKeys) return invalid("{} keys, expected 1..{}", k.keys.size(), kMaxKeys);
    for (usize i = 0; i < k.keys.size(); ++i) {
        const KeysetKey& key = k.keys[i];
        const std::string id = hexOf(key.id);
        if (key.id != keyFingerprint(key.pub)) return invalid("key {}: the ID is not the fingerprint of its public key", id);
        if (i > 0 && !(k.keys[i - 1].id < key.id)) return invalid("key {}: keys are not sorted by ID or repeat", id);
        if (!validRole(key.role)) return invalid("key {}: role \"{}\" is not a role name", id, key.role);
        if (key.notBefore >= key.notAfter || key.notAfter > kMaxJsonInt)
            return invalid("key {}: validity [{}, {}) is empty or above {}", id, key.notBefore, key.notAfter, kMaxJsonInt);
    }
    return {};
}

Result<void> validatePointer(const Pointer& p) {
    if (!isValidProductId(p.productId)) return invalid("product_id \"{}\" is not a product ID", p.productId);
    if (!validChannel(p.channel)) return invalid("channel \"{}\" is not a channel name", p.channel);
    if (!isValidPlatform(p.platform)) return invalid("platform \"{}\" is not a platform name", p.platform);
    if (p.sequence == 0 || p.sequence > kMaxJsonInt)
        return invalid("sequence {} is outside 1..{}", p.sequence, kMaxJsonInt);
    if (!validVersionText(p.minLauncher)) return invalid("min_launcher \"{}\" is not a dotted version", p.minLauncher);
    if (!validVersionText(p.minClient)) return invalid("min_client \"{}\" is not a dotted version", p.minClient);
    if (p.cdnHosts.size() > kMaxCdnHosts) return invalid("{} cdn_hosts, the limit is {}", p.cdnHosts.size(), kMaxCdnHosts);
    if (p.rolloutPct > 100) return invalid("rollout_pct {} is above 100", p.rolloutPct);
    if (p.signedAt > kMaxJsonInt || p.expires > kMaxJsonInt) return invalid("signed_at or expires is above {}", kMaxJsonInt);
    if (!isValidBuildId(p.ref.buildId)) return invalid("build_id \"{}\" is not a build ID", p.ref.buildId);
    for (const std::string& h : p.cdnHosts)
        if (!validHost(h)) return invalid("cdn_hosts entry \"{}\" is not an https:// or loopback http:// URL", h);
    if (p.next) {
        if (!isValidBuildId(p.next->ref.buildId)) return invalid("next.build_id \"{}\" is not a build ID", p.next->ref.buildId);
        if (p.next->availableAt > kMaxJsonInt) return invalid("next.available_at is above {}", kMaxJsonInt);
    }
    return {};
}

bool verifySignature(const PublicKey& pub, std::span<const u8> message, const Signature& sig) noexcept {
    static constexpr u8 kEmpty = 0;
    return crypto_ed25519_check(sig.data(), pub.data(), message.empty() ? &kEmpty : message.data(), message.size()) == 0;
}

std::vector<u8> withContext(std::string_view context, std::vector<u8> doc) {
    doc.insert(doc.begin(), context.begin(), context.end());
    return doc;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Keys, documents
// ---------------------------------------------------------------------------------------------

KeyId keyFingerprint(const PublicKey& pub) noexcept {
    const Hash256 h = blake2b256(pub);
    KeyId id;
    std::copy_n(h.bytes.begin(), id.size(), id.begin());
    return id;
}

bool isTestOnlyKey(const PublicKey& pub) noexcept {
    // Go's patchtrust.testOnlyRoots; TrustVerifier tests check both lists against the seed derivation.
    static constexpr std::string_view kTestOnlyRoots[] = {
        "157c930ac055ce5cbc6dc770910d6f6a1a816776e3e5c6460cdea0002bb76ccf",
        "724fc80638768e34218d5f9877627890b4caa0a6ab2a7894f573f6ede14bae7a",
        "7171f6eba59c768db892730795613f1abe75b89052a05fcc6e315b05689d7e7e",
    };
    const std::string hex = hexOf(pub);
    return std::find(std::begin(kTestOnlyRoots), std::end(kTestOnlyRoots), hex) != std::end(kTestOnlyRoots);
}

const KeysetKey* Keyset::findKey(const KeyId& id) const noexcept {
    const auto it = std::lower_bound(keys.begin(), keys.end(), id,
                                     [](const KeysetKey& k, const KeyId& want) { return k.id < want; });
    return it != keys.end() && it->id == id ? &*it : nullptr;
}

std::vector<u8> encodeKeyset(const Keyset& k, bool withSig) {
    Writer w;
    w.raw("{");
    w.key("keys", true);
    w.raw("[");
    for (usize i = 0; i < k.keys.size(); ++i) {
        const KeysetKey& key = k.keys[i];
        if (i > 0) w.raw(",");
        w.raw("{");
        w.key("id", true);
        w.hex(key.id);
        w.key("notAfter");
        w.uint(key.notAfter);
        w.key("notBefore");
        w.uint(key.notBefore);
        w.key("pub");
        w.hex(key.pub);
        w.key("role");
        w.str(key.role);
        w.raw("}");
    }
    w.raw("]");
    w.key("productId");
    w.str(k.productId);
    w.key("rootEpoch");
    w.uint(k.rootEpoch);
    if (withSig) {
        w.key("sig");
        w.hex(k.sig);
    }
    w.key("version");
    w.uint(k.version);
    w.raw("}");
    return w.take();
}

std::vector<u8> encodePointer(const Pointer& p, bool withSig) {
    Writer w;
    w.raw("{");
    w.key("build_id", true);
    w.str(p.ref.buildId);
    w.key("cdn_hosts");
    w.raw("[");
    for (usize i = 0; i < p.cdnHosts.size(); ++i) {
        if (i > 0) w.raw(",");
        w.str(p.cdnHosts[i]);
    }
    w.raw("]");
    w.key("channel");
    w.str(p.channel);
    w.key("compat_epoch");
    w.uint(p.ref.compatEpoch);
    w.key("expires");
    w.uint(p.expires);
    w.key("key_id");
    w.hex(p.keyId);
    w.key("manifest_hash");
    w.hex(p.ref.manifestHash.bytes);
    w.key("min_client");
    w.str(p.minClient);
    w.key("min_launcher");
    w.str(p.minLauncher);
    if (p.next) {
        w.key("next");
        w.raw("{");
        w.key("available_at", true);
        w.uint(p.next->availableAt);
        w.key("build_id");
        w.str(p.next->ref.buildId);
        w.key("compat_epoch");
        w.uint(p.next->ref.compatEpoch);
        w.key("manifest_hash");
        w.hex(p.next->ref.manifestHash.bytes);
        w.raw("}");
    }
    w.key("platform");
    w.str(p.platform);
    w.key("product_id");
    w.str(p.productId);
    w.key("rollback");
    w.boolean(p.rollback);
    w.key("rollout_pct");
    w.uint(p.rolloutPct);
    w.key("sequence");
    w.uint(p.sequence);
    if (withSig) {
        w.key("sig");
        w.hex(p.sig);
    }
    w.key("signed_at");
    w.uint(p.signedAt);
    w.raw("}");
    return w.take();
}

std::vector<u8> keysetSignedMessage(const Keyset& keyset) {
    return withContext(kKeysetContext, encodeKeyset(keyset, false));
}

std::vector<u8> pointerSignedMessage(const Pointer& pointer) {
    return withContext(kPointerContext, encodePointer(pointer, false));
}

Result<Keyset> parseKeyset(std::span<const u8> doc) {
    if (doc.size() > kMaxKeysetSize)
        return makeError(ErrorCode::LimitExceeded, "{} bytes, the limit is {}", doc.size(), kMaxKeysetSize);
    Reader r(doc);
    Keyset k;
    r.lit("{");
    r.key("keys", true);
    r.lit("[");
    while (r.ok() && !r.peek("]")) {
        if (k.keys.size() == kMaxKeys) {
            r.fail("more than {} keys", kMaxKeys);
            break;
        }
        if (!k.keys.empty()) r.lit(",");
        KeysetKey key;
        r.lit("{");
        r.key("id", true);
        r.hex(key.id);
        r.key("notAfter");
        key.notAfter = r.uint();
        r.key("notBefore");
        key.notBefore = r.uint();
        r.key("pub");
        r.hex(key.pub);
        r.key("role");
        key.role = r.str(kMaxRoleLength);
        r.lit("}");
        k.keys.push_back(std::move(key));
    }
    r.lit("]");
    r.key("productId");
    k.productId = r.str(kMaxIdText);
    r.key("rootEpoch");
    const u64 epoch = r.uint();
    r.key("sig");
    r.hex(k.sig);
    r.key("version");
    k.version = r.uint();
    r.lit("}");
    r.end();
    if (!r.ok()) return Error{ErrorCode::Corrupt, r.error()};
    if (epoch > 0xFFFFFFFFull) return makeError(ErrorCode::Corrupt, "rootEpoch {} is outside 1..{}", epoch, 0xFFFFFFFEu);
    k.rootEpoch = static_cast<u32>(epoch);
    HELIOS_TRY(validateKeyset(k));
    return k;
}

Result<Pointer> parsePointer(std::span<const u8> doc) {
    if (doc.size() > kMaxPointerSize)
        return makeError(ErrorCode::LimitExceeded, "{} bytes, the limit is {}", doc.size(), kMaxPointerSize);
    Reader r(doc);
    Pointer p;
    r.lit("{");
    r.key("build_id", true);
    p.ref.buildId = r.str(kMaxIdText);
    r.key("cdn_hosts");
    r.lit("[");
    while (r.ok() && !r.peek("]")) {
        if (p.cdnHosts.size() == kMaxCdnHosts) {
            r.fail("more than {} cdn_hosts", kMaxCdnHosts);
            break;
        }
        if (!p.cdnHosts.empty()) r.lit(",");
        p.cdnHosts.push_back(r.str(kMaxHostLength));
    }
    r.lit("]");
    r.key("channel");
    p.channel = r.str(kMaxIdText);
    r.key("compat_epoch");
    p.ref.compatEpoch = r.u32Value("compat_epoch");
    r.key("expires");
    p.expires = r.uint();
    r.key("key_id");
    r.hex(p.keyId);
    r.key("manifest_hash");
    r.hex(p.ref.manifestHash.bytes);
    r.key("min_client");
    p.minClient = r.str(kMaxVersionText);
    r.key("min_launcher");
    p.minLauncher = r.str(kMaxVersionText);
    if (r.peek(",\"next\":")) {
        PointerNext n;
        r.key("next");
        r.lit("{");
        r.key("available_at", true);
        n.availableAt = r.uint();
        r.key("build_id");
        n.ref.buildId = r.str(kMaxIdText);
        r.key("compat_epoch");
        n.ref.compatEpoch = r.u32Value("next.compat_epoch");
        r.key("manifest_hash");
        r.hex(n.ref.manifestHash.bytes);
        r.lit("}");
        p.next = std::move(n);
    }
    r.key("platform");
    p.platform = r.str(kMaxIdText);
    r.key("product_id");
    p.productId = r.str(kMaxIdText);
    r.key("rollback");
    p.rollback = r.boolean();
    r.key("rollout_pct");
    const u64 pct = r.uint();
    if (r.ok() && pct > 100) r.fail("rollout_pct {} is above 100", pct);
    p.rolloutPct = static_cast<u8>(std::min<u64>(pct, 100));
    r.key("sequence");
    p.sequence = r.uint();
    r.key("sig");
    r.hex(p.sig);
    r.key("signed_at");
    p.signedAt = r.uint();
    r.lit("}");
    r.end();
    if (!r.ok()) return Error{ErrorCode::Corrupt, r.error()};
    HELIOS_TRY(validatePointer(p));
    return p;
}

std::string_view trustCheckName(TrustCheck check) noexcept {
    const auto i = static_cast<usize>(check);
    return i < std::size(kCheckNames) ? kCheckNames[i] : std::string_view("unknown");
}

std::optional<TrustCheck> trustCheckOf(const Error& error) noexcept {
    const std::string_view m = error.message;
    for (usize i = 0; i < std::size(kCheckNames); ++i) {
        const std::string_view name = kCheckNames[i];
        if (m.size() > name.size() && m.starts_with(name) && m[name.size()] == ':') return static_cast<TrustCheck>(i);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

TrustState advanceTrustState(const TrustState& state, const Keyset& keyset, const Pointer& pointer) noexcept {
    TrustState out = state;
    out.rootEpoch = std::max(state.rootEpoch, keyset.rootEpoch);
    out.keysetVersion = std::max(state.keysetVersion, keyset.version);
    out.pointerSequence = pointer.rollback ? pointer.sequence : std::max(state.pointerSequence, pointer.sequence);
    return out;
}

namespace {
constexpr u32 kStateMagic = 0x53525448u; // "HTRS"
}

std::array<u8, kTrustStateSize> encodeTrustState(const TrustState& state) noexcept {
    std::array<u8, kTrustStateSize> out{};
    storeLE<u32>(out.data(), kStateMagic);
    storeLE<u32>(out.data() + 4, 0); // version 0, reserved
    storeLE<u32>(out.data() + 8, state.rootEpoch);
    storeLE<u64>(out.data() + 12, state.keysetVersion);
    storeLE<u64>(out.data() + 20, state.pointerSequence);
    const Hash256 h = blake2b256(std::span<const u8>(out.data(), 28));
    std::copy_n(h.bytes.begin(), 4, out.begin() + 28);
    return out;
}

Result<TrustState> decodeTrustState(std::span<const u8> record) {
    if (record.size() != kTrustStateSize || loadLE<u32>(record.data()) != kStateMagic || loadLE<u32>(record.data() + 4) != 0)
        return Error{ErrorCode::Corrupt, "not a trust state record"};
    const Hash256 h = blake2b256(record.first(28));
    if (!std::equal(h.bytes.begin(), h.bytes.begin() + 4, record.begin() + 28))
        return Error{ErrorCode::Corrupt, "the trust state record's checksum does not match"};
    TrustState s;
    s.rootEpoch = loadLE<u32>(record.data() + 8);
    s.keysetVersion = loadLE<u64>(record.data() + 12);
    s.pointerSequence = loadLE<u64>(record.data() + 20);
    return s;
}

Result<TrustState> FileTrustStateStore::load() {
    if (!fs::exists(m_path)) return TrustState{};
    HELIOS_TRY_ASSIGN(const std::vector<u8> bytes, fs::readFile(m_path));
    Result<TrustState> s = decodeTrustState(bytes);
    if (!s) return makeError(ErrorCode::Corrupt, "{}: {}", fs::pathToGenericUtf8(m_path), s.error().message);
    return s;
}

Result<void> FileTrustStateStore::save(const TrustState& state) {
    const std::array<u8, kTrustStateSize> record = encodeTrustState(state);
    return fs::writeFile(m_path, record, fs::WriteMode::Atomic);
}

// ---------------------------------------------------------------------------------------------
// Verifier
// ---------------------------------------------------------------------------------------------

Result<TrustVerifier> TrustVerifier::create(TrustTarget target, const RootPair& roots, const TrustOptions& options) {
    if (!isValidProductId(target.productId) || !validChannel(target.channel) || !isValidPlatform(target.platform))
        return makeError(ErrorCode::InvalidArgument, "invalid target {}/{}/{}", target.productId, target.channel,
                         target.platform);
    if (roots.epoch == 0 || roots.epoch == 0xFFFFFFFFu)
        return makeError(ErrorCode::InvalidArgument, "root epoch {} is outside 1..{}", roots.epoch, 0xFFFFFFFEu);
    if (roots.current == roots.next)
        return Error{ErrorCode::InvalidArgument, "the current and next roots are the same key"};
    if (!options.allowTestKeys && (isTestOnlyKey(roots.current) || isTestOnlyKey(roots.next)))
        return Error{ErrorCode::InvalidArgument, "the root pair contains a test-only key"};
    return TrustVerifier(std::move(target), roots);
}

Result<Keyset> TrustVerifier::verifyKeyset(std::span<const u8> doc, const TrustState& state) const {
    Result<Keyset> parsed = parseKeyset(doc);
    if (!parsed) {
        Error e = reject(TrustCheck::KeysetMalformed, "{}", parsed.error().message);
        if (parsed.errorCode() == ErrorCode::LimitExceeded) e.code = ErrorCode::LimitExceeded;
        return e;
    }
    Keyset& ks = *parsed;
    const PublicKey* root = nullptr;
    if (ks.rootEpoch == m_roots.epoch)
        root = &m_roots.current;
    else if (ks.rootEpoch == m_roots.epoch + 1)
        root = &m_roots.next;
    else
        return reject(TrustCheck::KeysetRoot, "rootEpoch {} is neither the current root's ({}) nor the next's",
                      ks.rootEpoch, m_roots.epoch);
    if (ks.rootEpoch < state.rootEpoch)
        return reject(TrustCheck::KeysetRootRatchet, "signed by root epoch {}; this install accepts {} and later",
                      ks.rootEpoch, state.rootEpoch);
    if (!verifySignature(*root, keysetSignedMessage(ks), ks.sig))
        return reject(TrustCheck::KeysetSignature, "the root of epoch {} did not sign this keyset", ks.rootEpoch);
    if (ks.productId != m_target.productId)
        return reject(TrustCheck::KeysetProduct, "keyset of \"{}\", expected \"{}\"", ks.productId, m_target.productId);
    if (ks.version < state.keysetVersion)
        return reject(TrustCheck::KeysetVersion, "version {} is below the stored {}", ks.version, state.keysetVersion);
    return parsed;
}

namespace {
Result<const KeysetKey*> signingKey(const Keyset& ks, const KeyId& id, TrustCheck unknown, TrustCheck role) {
    const KeysetKey* key = ks.findKey(id);
    if (!key) return reject(unknown, "key {} is not in keyset version {}", hexOf(id), ks.version);
    if (key->role != kRoleManifest)
        return reject(role, "key {} has role \"{}\", not \"{}\"", hexOf(id), key->role, kRoleManifest);
    return key;
}
} // namespace

Result<Pointer> TrustVerifier::verifyPointer(std::span<const u8> doc, const Keyset& keyset, u64 now,
                                             const TrustState& state) const {
    Result<Pointer> parsed = parsePointer(doc);
    if (!parsed) {
        Error e = reject(TrustCheck::PointerMalformed, "{}", parsed.error().message);
        if (parsed.errorCode() == ErrorCode::LimitExceeded) e.code = ErrorCode::LimitExceeded;
        return e;
    }
    const Pointer& p = *parsed;
    HELIOS_TRY_ASSIGN(const KeysetKey* key,
                      signingKey(keyset, p.keyId, TrustCheck::PointerKeyUnknown, TrustCheck::PointerKeyRole));
    if (!verifySignature(key->pub, pointerSignedMessage(p), p.sig))
        return reject(TrustCheck::PointerSignature, "key {} did not sign this pointer", hexOf(p.keyId));
    if (p.signedAt < key->notBefore || p.signedAt >= key->notAfter)
        return reject(TrustCheck::PointerKeyWindow, "signed at {}, key {} signs in [{}, {})", p.signedAt,
                      hexOf(p.keyId), key->notBefore, key->notAfter);
    if (p.expires <= p.signedAt || p.expires - p.signedAt > kMaxPointerLifetime)
        return reject(TrustCheck::PointerLifetime, "expires {} is not within {} s after signed_at {}", p.expires,
                      kMaxPointerLifetime, p.signedAt);
    if (p.productId != m_target.productId)
        return reject(TrustCheck::PointerProduct, "pointer of \"{}\", expected \"{}\"", p.productId, m_target.productId);
    if (p.channel != m_target.channel)
        return reject(TrustCheck::PointerChannel, "pointer of channel \"{}\", expected \"{}\"", p.channel,
                      m_target.channel);
    if (p.platform != m_target.platform)
        return reject(TrustCheck::PointerPlatform, "pointer of platform \"{}\", expected \"{}\"", p.platform,
                      m_target.platform);
    if (now >= p.expires) return reject(TrustCheck::PointerExpired, "expired at {}, now is {}", p.expires, now);
    if (p.sequence < state.pointerSequence && !p.rollback)
        return reject(TrustCheck::PointerSequence, "sequence {} is below the stored {} and not a rollback",
                      p.sequence, state.pointerSequence);
    return parsed;
}

Result<ManifestHeaderInfo> TrustVerifier::verifyManifestHeader(std::span<const u8> file, const Keyset& keyset,
                                                               const ManifestRef& ref, u64 now) const {
    Result<ManifestHeaderInfo> info = readManifestHeader(file);
    if (!info) return reject(TrustCheck::ManifestMalformed, "{}", info.error().message);
    const ManifestHeader& h = info->header;
    if (info->headerHash != ref.manifestHash)
        return reject(TrustCheck::ManifestHash, "header hash {}, the pointer names {}", info->headerHash.toHex(),
                      ref.manifestHash.toHex());
    HELIOS_TRY_ASSIGN(const KeysetKey* key,
                      signingKey(keyset, h.keyId, TrustCheck::ManifestKeyUnknown, TrustCheck::ManifestKeyRole));
    Signature sig;
    std::copy(h.signature.begin(), h.signature.end(), sig.begin());
    if (!verifySignature(key->pub, file.first(hman::kSignedBytes), sig))
        return reject(TrustCheck::ManifestSignature, "key {} did not sign this manifest", hexOf(h.keyId));
    if (h.createdAt < key->notBefore || h.createdAt >= key->notAfter)
        return reject(TrustCheck::ManifestKeyWindow, "created at {}, key {} signs in [{}, {})", h.createdAt,
                      hexOf(h.keyId), key->notBefore, key->notAfter);
    if (h.productId != m_target.productId)
        return reject(TrustCheck::ManifestProduct, "manifest of \"{}\", expected \"{}\"", h.productId,
                      m_target.productId);
    if (h.platform != m_target.platform)
        return reject(TrustCheck::ManifestPlatform, "manifest of platform \"{}\", expected \"{}\"",
                      h.platform, m_target.platform);
    if (h.buildId != ref.buildId)
        return reject(TrustCheck::ManifestBuild, "manifest of build \"{}\", the pointer names \"{}\"",
                      h.buildId, ref.buildId);
    if (h.compatEpoch != ref.compatEpoch)
        return reject(TrustCheck::ManifestCompatEpoch, "compat epoch {}, the pointer names {}", h.compatEpoch,
                      ref.compatEpoch);
    if (h.expiresAt != 0 && now >= h.expiresAt)
        return reject(TrustCheck::ManifestExpired, "expired at {}, now is {}", h.expiresAt, now);
    return info;
}

Result<Manifest> TrustVerifier::verifyManifest(std::span<const u8> file, const Keyset& keyset, const ManifestRef& ref,
                                               u64 now) const {
    HELIOS_TRY(verifyManifestHeader(file, keyset, ref, now));
    Result<Manifest> m = readManifest(file);
    if (!m) return reject(TrustCheck::ManifestBody, "{}", m.error().message);
    return m;
}

Result<void> TrustVerifier::verifyChunk(const ManifestChunk& chunk, std::span<const u8> raw) {
    if (raw.size() != chunk.rawSize)
        return reject(TrustCheck::ChunkCorrupt, "chunk {} is {} bytes, the manifest says {}", chunk.hash.toHex(),
                      raw.size(), chunk.rawSize);
    const Hash256 got = blake2b256(raw);
    if (got != chunk.hash)
        return reject(TrustCheck::ChunkHash, "chunk {} hashes to {}", chunk.hash.toHex(), got.toHex());
    return {};
}

} // namespace helios::patch
