#include "helios/server/keys.h"

#include <algorithm>
#include <charconv>

#include <yyjson.h>

#include "helios/core/fs.h"
#include "helios/core/hash.h"

namespace helios::server {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

std::string base64Encode(std::span<const u8> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    usize i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const u32 v = (u32{data[i]} << 16) | (u32{data[i + 1]} << 8) | data[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    const usize rest = data.size() - i;
    if (rest == 1) {
        const u32 v = u32{data[i]} << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.append("==");
    } else if (rest == 2) {
        const u32 v = (u32{data[i]} << 16) | (u32{data[i + 1]} << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

Result<std::vector<u8>> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    if (text.size() % 4 == 1) return Error{ErrorCode::ParseError, "base64: bad length"};
    std::vector<u8> out;
    out.reserve(text.size() * 3 / 4);
    u32 acc = 0;
    int bits = 0;
    for (char c : text) {
        const int v = decodeChar(c);
        if (v < 0) return Error{ErrorCode::ParseError, "base64: invalid character"};
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero (canonical encoding).
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) return Error{ErrorCode::ParseError, "base64: non-canonical"};
    return out;
}

const KeyringEntry* Keyring::find(u32 id) const {
    for (const KeyringEntry& e : keys)
        if (e.id == id) return &e;
    return nullptr;
}

Result<Keyring> parseKeyring(std::string_view json) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (!doc) return Error{ErrorCode::ParseError, "keyring: invalid JSON"};
    Keyring ring;
    Result<Keyring> result = Error{ErrorCode::ParseError, "keyring: no keys"};
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (yyjson_is_obj(root)) {
        if (const char* purpose = yyjson_get_str(yyjson_obj_get(root, "purpose"))) ring.purpose = purpose;
        yyjson_val* keys = yyjson_obj_get(root, "keys");
        bool bad = false;
        if (yyjson_is_arr(keys)) {
            usize idx = 0;
            usize max = 0;
            yyjson_val* k = nullptr;
            yyjson_arr_foreach(keys, idx, max, k) {
                yyjson_val* id = yyjson_obj_get(k, "id");
                const char* secret = yyjson_get_str(yyjson_obj_get(k, "secret"));
                if (!yyjson_is_uint(id) || !secret) {
                    bad = true;
                    break;
                }
                KeyringEntry e;
                e.id = static_cast<u32>(yyjson_get_uint(id));
                e.secretBase64 = secret;
                auto bytes = base64Decode(e.secretBase64);
                if (!bytes) {
                    bad = true;
                    break;
                }
                e.secret = std::move(*bytes);
                ring.keys.push_back(std::move(e));
            }
        }
        std::sort(ring.keys.begin(), ring.keys.end(), [](const KeyringEntry& a, const KeyringEntry& b) { return a.id < b.id; });
        if (bad) result = Error{ErrorCode::ParseError, "keyring: malformed key entry"};
        else if (!ring.keys.empty()) result = std::move(ring);
    } else {
        result = Error{ErrorCode::ParseError, "keyring: not an object"};
    }
    yyjson_doc_free(doc);
    return result;
}

Result<Keyring> loadKeyring(const std::filesystem::path& path) {
    HELIOS_TRY_ASSIGN(std::string text, fs::readTextFile(path));
    auto ring = parseKeyring(text);
    if (!ring) return makeError(ring.errorCode(), "{}: {}", fs::pathToUtf8(path), ring.error().message);
    return ring;
}

Result<net::Key> netcodeKey(const KeyringEntry& entry) {
    if (entry.secret.size() != net::kKeyBytes)
        return makeError(ErrorCode::InvalidArgument, "key {} has {} bytes, need {}", entry.id, entry.secret.size(), net::kKeyBytes);
    net::Key key{};
    std::copy(entry.secret.begin(), entry.secret.end(), key.begin());
    return key;
}

Result<u64> parseProtocolId(std::string_view text) {
    // Exactly Go's strconv.ParseUint(strings.TrimSpace(s), 0, 64), which the backend applies to
    // session.protocol_id before passing the same string on as HELIOS_PROTOCOL_ID: a 0x/0o/0b
    // prefix or a leading 0 (octal) picks the base, anything else is decimal, and underscores may
    // separate digits. A value the backend reads differently would make the gateway refuse every
    // token, so nothing else (such as bare hex) is accepted.
    const Error bad{ErrorCode::ParseError, "bad protocol id (want 0x<hex>, 0o<octal>, 0b<binary> or decimal)"};
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    if (text.empty()) return bad;
    int base = 10;
    bool prefixed = false;
    if (text.size() >= 2 && text[0] == '0') {
        const char p = static_cast<char>(text[1] | 0x20);
        prefixed = p == 'x' || p == 'o' || p == 'b';
        base = p == 'x' ? 16 : p == 'b' ? 2 : 8; // Go: a bare leading 0 means octal too
    }
    if (prefixed) text.remove_prefix(2);
    // Underscores only between digits (and right after a base prefix), as Go allows with base 0.
    std::string digits;
    bool lastUnderscore = !prefixed; // a leading underscore is allowed only after a prefix
    for (const char c : text) {
        if (c == '_') {
            if (lastUnderscore) return bad;
            lastUnderscore = true;
            continue;
        }
        digits.push_back(c);
        lastUnderscore = false;
    }
    if (digits.empty() || lastUnderscore) return bad;
    u64 value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) return bad;
    return value;
}

net::Key insecureDevKey(std::string_view label) {
    net::Key key{};
    u64 h = fnv1a64(label);
    for (usize i = 0; i < key.size(); ++i) {
        h = mix64(h + i);
        key[i] = static_cast<u8>(h >> 24);
    }
    return key;
}

} // namespace helios::server
