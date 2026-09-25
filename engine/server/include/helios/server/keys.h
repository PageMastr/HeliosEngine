#pragma once
// Key material shared with the Go backend (05 §5 `keys/`): base64, Go keyring files and protocol
// ids.
//
// The backend keeps generations of 32-byte secrets in JSON keyrings
//   {"version":1,"purpose":"netcode-shard","keys":[{"id":1,"secret":"<base64>","created":"…"}]}
// and passes their paths to supervised children (HELIOS_NETCODE_KEYS). The highest id is the
// current generation: gateways sign nothing with it, but netcode decrypts connect tokens with it,
// and the gateway reports its id as `keyId` when it registers. The NATS `fleet` password is the
// base64 `secret` string of the current key in keys/nats-fleet.json.
//
// Threading: pure functions.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/connect_token.h"

namespace helios::server {

/// Standard base64 with padding (RFC 4648 §4), as Go's encoding/base64.StdEncoding writes it.
std::string base64Encode(std::span<const u8> data);
/// Decodes standard base64 (padding optional, no whitespace). Fails with ParseError.
Result<std::vector<u8>> base64Decode(std::string_view text);

struct KeyringEntry {
    u32 id = 0;
    std::string secretBase64; ///< As stored (the NATS fleet password is this string).
    std::vector<u8> secret;
};

struct Keyring {
    std::string purpose;
    std::vector<KeyringEntry> keys; ///< Ascending by id.

    /// The newest generation (highest id). The keyring must not be empty.
    const KeyringEntry& current() const { return keys.back(); }
    const KeyringEntry* find(u32 id) const;
};

/// Parses a keyring file's JSON text.
Result<Keyring> parseKeyring(std::string_view json);
Result<Keyring> loadKeyring(const std::filesystem::path& path);
/// The current key as a netcode key (the secret must be exactly 32 bytes).
Result<net::Key> netcodeKey(const KeyringEntry& entry);

/// Parses a protocol id exactly as the backend does (Go strconv.ParseUint(s, 0, 64) after trimming):
/// "0x48454c494f530001", "0o…", "0b…", a leading 0 for octal, else decimal.
Result<u64> parseProtocolId(std::string_view text);

/// Client protocol id when nothing configures one: the backend's default session.protocol_id.
inline constexpr u64 kDefaultClientProtocolId = 0x48454C494F530001ull; // "HELIOS" 0001
/// Trunk protocol id (gateway <-> cell). Different from the client id, so a client token can
/// never open a trunk even if both were sealed with the same key.
inline constexpr u64 kDefaultTrunkProtocolId = 0x48454C494F540001ull; // "HELIOT" 0001

/// A fixed, publicly known key derived from `label`, for loopback-only development runs without
/// the backend (`--dev-insecure-keys`). Never valid off loopback: callers must enforce that.
net::Key insecureDevKey(std::string_view label);

} // namespace helios::server
