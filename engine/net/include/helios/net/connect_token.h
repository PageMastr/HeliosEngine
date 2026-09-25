#pragma once
// netcode 1.02 connect tokens (04 §2.3). Production tokens are minted by the Go Session service
// (services/pkg/connecttoken, byte-exact with vendored netcode); these helpers mint them in C++
// for tests, bots, PIE and orchestrator-style trunk tokens, and inspect them.
//
// Layout (2,048 bytes, little-endian): "NETCODE 1.02\0" | protocol_id u64 | create u64 | expire u64
// | nonce[24] | private[1024] (XChaCha20-Poly1305 under the server key; AD = version, protocol id,
// expire) | timeout i32 | n u32 | n addresses | client_to_server_key[32] | server_to_client_key[32]
// | zero padding. The private part holds client_id, timeout, the server addresses the servers
// accept, both session keys and 256 bytes of user data (character, content pin, placement
// ticket, entitlements, attestation hash).
//
// Helios adds no cryptography: generation calls netcode_generate_connect_token and
// openPrivateConnectToken calls netcode's own decryptor.
//
// Threading: thread-safe (the library initialisation is reference-counted and locked).

#include <array>
#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/address.h"

namespace helios::net {

inline constexpr usize kConnectTokenBytes = 2048;
inline constexpr usize kConnectTokenPrivateBytes = 1024;
inline constexpr usize kConnectTokenNonceBytes = 24;
inline constexpr usize kKeyBytes = 32;
inline constexpr usize kUserDataBytes = 256;
inline constexpr usize kMaxServersPerToken = 32;
/// 04 §2.3: tokens expire after <= 45 s; servers are configured with the same maximum lifetime.
inline constexpr i32 kDefaultTokenExpirySeconds = 45;
inline constexpr i32 kDefaultTokenTimeoutSeconds = 10;

using Key = std::array<u8, kKeyBytes>;
using UserData = std::array<u8, kUserDataBytes>;
using ConnectTokenBytes = std::array<u8, kConnectTokenBytes>;

struct ConnectTokenParams {
    u64 protocolId = 0;
    u64 clientId = 0;
    i32 expireSeconds = kDefaultTokenExpirySeconds; ///< < 0: never expires.
    i32 timeoutSeconds = kDefaultTokenTimeoutSeconds; ///< < 0: no timeout.
    /// Addresses the client tries, in order (1..32).
    std::vector<Address> publicAddresses;
    /// Addresses servers compare against their own (same count); empty = the public ones.
    std::vector<Address> internalAddresses;
    Key privateKey{};
    UserData userData{};
};

/// Mints a token with netcode_generate_connect_token (random nonce and session keys; create time
/// is the current wall clock).
Result<ConnectTokenBytes> generateConnectToken(const ConnectTokenParams& params);

/// The public (unencrypted) part of a token.
struct ConnectTokenInfo {
    u64 protocolId = 0;
    u64 createTimestamp = 0;
    u64 expireTimestamp = 0;
    std::array<u8, kConnectTokenNonceBytes> nonce{};
    i32 timeoutSeconds = 0;
    std::vector<Address> serverAddresses;
    Key clientToServerKey{};
    Key serverToClientKey{};
};

/// Parses and validates the public part (version, create <= expire, 1..32 well-formed addresses,
/// zero padding). Accepts exactly kConnectTokenBytes bytes.
Result<ConnectTokenInfo> parseConnectToken(std::span<const u8> token);

/// The decrypted private part (what the server sees).
struct PrivateConnectToken {
    u64 clientId = 0;
    i32 timeoutSeconds = 0;
    std::vector<Address> serverAddresses;
    Key clientToServerKey{};
    Key serverToClientKey{};
    UserData userData{};
};

/// Decrypts and parses the private part with the server key, using netcode's own AEAD.
/// Fails with Corrupt if authentication fails (wrong key, tampered token or header).
Result<PrivateConnectToken> openPrivateConnectToken(std::span<const u8> token, const Key& privateKey);

/// 32 random bytes from netcode's CSPRNG (libsodium subset).
Key generateKey();

} // namespace helios::net
