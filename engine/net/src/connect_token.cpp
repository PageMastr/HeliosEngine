// Connect token generation (netcode's API), public-part parsing and private-part decryption.

#include "helios/net/connect_token.h"

#include <cstring>
#include <string>

#include <netcode.h>

#include "net_internal.h"

// netcode's private-token decryptor is not in netcode.h, but it is an external symbol of the
// vendored library (netcode.c defines it without `static`). It is the exact code path the server
// runs, so reusing it keeps Helios free of cryptography of its own. If a netcode update makes it
// static, this fails to link rather than silently diverging.
extern "C" int netcode_decrypt_connect_token_private(uint8_t* buffer, int buffer_length, uint8_t* version_info,
                                                     uint64_t protocol_id, uint64_t expire_timestamp, uint8_t* nonce,
                                                     uint8_t* key);

namespace helios::net {
namespace {

constexpr u8 kVersionInfo[13] = {'N', 'E', 'T', 'C', 'O', 'D', 'E', ' ', '1', '.', '0', '2', '\0'};

/// Little-endian reader with bounds checking.
struct Reader {
    std::span<const u8> data;
    usize pos = 0;
    bool ok = true;

    bool need(usize n) {
        if (!ok || data.size() - pos < n) ok = false;
        return ok;
    }
    u8 u8v() {
        if (!need(1)) return 0;
        return data[pos++];
    }
    u16 u16v() {
        if (!need(2)) return 0;
        const u16 v = static_cast<u16>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | data[pos + static_cast<usize>(i)];
        pos += 4;
        return v;
    }
    u64 u64v() {
        if (!need(8)) return 0;
        u64 v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | data[pos + static_cast<usize>(i)];
        pos += 8;
        return v;
    }
    void bytes(u8* out, usize n) {
        if (!need(n)) return;
        std::memcpy(out, data.data() + pos, n);
        pos += n;
    }
};

/// netcode address list: count u32 then (type u8, ipv4[4] | ipv6 8 x u16le, port u16le).
bool readAddresses(Reader& r, std::vector<Address>& out) {
    const u32 count = r.u32v();
    if (!r.ok || count == 0 || count > kMaxServersPerToken) return false;
    out.clear();
    for (u32 i = 0; i < count; ++i) {
        const u8 type = r.u8v();
        if (type == 1) {
            std::array<u8, 4> v4{};
            r.bytes(v4.data(), 4);
            const u16 port = r.u16v();
            out.push_back(Address::ipv4(v4, port));
        } else if (type == 2) {
            std::array<u16, 8> g{};
            for (u16& x : g) x = r.u16v();
            const u16 port = r.u16v();
            out.push_back(Address::ipv6(g, port));
        } else {
            return false;
        }
        if (!r.ok) return false;
    }
    return true;
}

} // namespace

Result<ConnectTokenBytes> generateConnectToken(const ConnectTokenParams& params) {
    const usize n = params.publicAddresses.size();
    if (n == 0 || n > kMaxServersPerToken) return Error{ErrorCode::InvalidArgument, "connect token: 1..32 addresses"};
    const std::vector<Address>& internal = params.internalAddresses.empty() ? params.publicAddresses : params.internalAddresses;
    if (internal.size() != n) return Error{ErrorCode::InvalidArgument, "connect token: internal/public count mismatch"};
    detail::LibraryRef library;
    if (!library.ok()) return Error{ErrorCode::Unsupported, "netcode_init failed"};
    std::vector<std::string> pub(n), priv(n);
    std::vector<const char*> pubPtr(n), privPtr(n);
    for (usize i = 0; i < n; ++i) {
        if (!params.publicAddresses[i].isValid() || !internal[i].isValid())
            return Error{ErrorCode::InvalidArgument, "connect token: invalid address"};
        pub[i] = params.publicAddresses[i].toString();
        priv[i] = internal[i].toString();
        pubPtr[i] = pub[i].c_str();
        privPtr[i] = priv[i].c_str();
    }
    ConnectTokenBytes token{};
    UserData userData = params.userData;
    if (netcode_generate_connect_token(static_cast<int>(n), pubPtr.data(), privPtr.data(), params.expireSeconds,
                                       params.timeoutSeconds, params.clientId, params.protocolId,
                                       params.privateKey.data(), userData.data(), token.data()) != NETCODE_OK) {
        return Error{ErrorCode::InvalidArgument, "netcode_generate_connect_token failed"};
    }
    return token;
}

Result<ConnectTokenInfo> parseConnectToken(std::span<const u8> token) {
    if (token.size() != kConnectTokenBytes) return Error{ErrorCode::InvalidArgument, "connect token must be 2048 bytes"};
    Reader r{token};
    u8 version[13] = {};
    r.bytes(version, sizeof(version));
    if (std::memcmp(version, kVersionInfo, sizeof(version)) != 0)
        return Error{ErrorCode::VersionMismatch, "connect token: not NETCODE 1.02"};
    ConnectTokenInfo info;
    info.protocolId = r.u64v();
    info.createTimestamp = r.u64v();
    info.expireTimestamp = r.u64v();
    if (info.createTimestamp > info.expireTimestamp) return Error{ErrorCode::Corrupt, "connect token: create > expire"};
    r.bytes(info.nonce.data(), info.nonce.size());
    r.pos += kConnectTokenPrivateBytes; // sealed; see openPrivateConnectToken
    info.timeoutSeconds = static_cast<i32>(r.u32v());
    if (!readAddresses(r, info.serverAddresses)) return Error{ErrorCode::Corrupt, "connect token: bad address list"};
    r.bytes(info.clientToServerKey.data(), kKeyBytes);
    r.bytes(info.serverToClientKey.data(), kKeyBytes);
    if (!r.ok) return Error{ErrorCode::Corrupt, "connect token: truncated"};
    for (usize i = r.pos; i < token.size(); ++i)
        if (token[i] != 0) return Error{ErrorCode::Corrupt, "connect token: non-zero padding"};
    return info;
}

Result<PrivateConnectToken> openPrivateConnectToken(std::span<const u8> token, const Key& privateKey) {
    HELIOS_TRY_ASSIGN(ConnectTokenInfo info, parseConnectToken(token));
    detail::LibraryRef library;
    if (!library.ok()) return Error{ErrorCode::Unsupported, "netcode_init failed"};
    std::array<u8, kConnectTokenPrivateBytes> sealed{};
    constexpr usize kPrivateOffset = 13 + 8 + 8 + 8 + kConnectTokenNonceBytes;
    std::memcpy(sealed.data(), token.data() + kPrivateOffset, sealed.size());
    u8 version[13];
    std::memcpy(version, kVersionInfo, sizeof(version));
    Key key = privateKey;
    std::array<u8, kConnectTokenNonceBytes> nonce = info.nonce;
    if (netcode_decrypt_connect_token_private(sealed.data(), static_cast<int>(sealed.size()), version, info.protocolId,
                                              info.expireTimestamp, nonce.data(), key.data()) != NETCODE_OK) {
        return Error{ErrorCode::Corrupt, "connect token: private part failed authentication"};
    }
    Reader r{std::span<const u8>(sealed.data(), sealed.size() - 16)};
    PrivateConnectToken out;
    out.clientId = r.u64v();
    out.timeoutSeconds = static_cast<i32>(r.u32v());
    if (!readAddresses(r, out.serverAddresses)) return Error{ErrorCode::Corrupt, "connect token: bad private address list"};
    r.bytes(out.clientToServerKey.data(), kKeyBytes);
    r.bytes(out.serverToClientKey.data(), kKeyBytes);
    r.bytes(out.userData.data(), kUserDataBytes);
    if (!r.ok) return Error{ErrorCode::Corrupt, "connect token: truncated private part"};
    return out;
}

Key generateKey() {
    detail::LibraryRef library;
    Key key{};
    netcode_random_bytes(key.data(), static_cast<int>(key.size()));
    return key;
}

} // namespace helios::net
