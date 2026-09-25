// Connect tokens: generation, public/private parsing, tamper detection, and interop with the Go
// Session service's golden vectors (services/testdata/vectors, byte-exact netcode 1.02): C++
// parses and decrypts Go-issued tokens and completes a full handshake with one (NS-0.5 side).

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <yyjson.h>

#include "net_test_util.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::test;

namespace {

std::vector<u8> fromHex(std::string_view hex) {
    std::vector<u8> out;
    REQUIRE(hex.size() % 2 == 0);
    for (usize i = 0; i < hex.size(); i += 2) {
        const auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        REQUIRE(hi >= 0);
        REQUIRE(lo >= 0);
        out.push_back(static_cast<u8>(hi * 16 + lo));
    }
    return out;
}

u64 hexU64(std::string_view hex) {
    u64 v = 0;
    for (u8 b : fromHex(hex)) v = (v << 8) | b;
    return v;
}

template <usize N>
std::array<u8, N> hexArray(std::string_view hex) {
    const auto v = fromHex(hex);
    REQUIRE(v.size() == N);
    std::array<u8, N> a{};
    std::copy(v.begin(), v.end(), a.begin());
    return a;
}

/// A parsed golden vector (the subset of fields both vector files share).
struct Vector {
    std::string name;
    u64 protocolId = 0;
    u64 clientId = 0;
    i32 timeoutSeconds = 0;
    std::optional<u64> createTimestamp, expireTimestamp;
    std::optional<i32> expireSeconds;
    std::vector<Address> publicAddresses, internalAddresses;
    Key privateKey{};
    std::optional<std::array<u8, 24>> nonce;
    std::optional<Key> clientToServerKey, serverToClientKey;
    UserData userData{};
    std::vector<u8> token;
};

std::string str(yyjson_val* obj, const char* key) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    REQUIRE(v);
    REQUIRE(yyjson_is_str(v));
    return yyjson_get_str(v);
}

std::vector<Address> addresses(yyjson_val* obj, const char* key) {
    std::vector<Address> out;
    yyjson_val* arr = yyjson_obj_get(obj, key);
    REQUIRE(yyjson_is_arr(arr));
    usize idx, max;
    yyjson_val* item;
    yyjson_arr_foreach(arr, idx, max, item) {
        const auto a = Address::parse(yyjson_get_str(item));
        REQUIRE(a);
        out.push_back(*a);
    }
    return out;
}

Vector parseVector(yyjson_val* v) {
    Vector out;
    if (yyjson_val* n = yyjson_obj_get(v, "name")) out.name = yyjson_get_str(n);
    out.protocolId = hexU64(str(v, "protocol_id"));
    out.clientId = hexU64(str(v, "client_id"));
    out.timeoutSeconds = static_cast<i32>(yyjson_get_sint(yyjson_obj_get(v, "timeout_seconds")));
    if (yyjson_obj_get(v, "create_timestamp")) out.createTimestamp = std::stoull(str(v, "create_timestamp"));
    if (yyjson_obj_get(v, "expire_timestamp")) out.expireTimestamp = std::stoull(str(v, "expire_timestamp"));
    if (yyjson_val* e = yyjson_obj_get(v, "expire_seconds")) out.expireSeconds = static_cast<i32>(yyjson_get_sint(e));
    out.publicAddresses = addresses(v, "public_addresses");
    out.internalAddresses = addresses(v, "internal_addresses");
    out.privateKey = hexArray<32>(str(v, "private_key"));
    if (yyjson_obj_get(v, "nonce")) out.nonce = hexArray<24>(str(v, "nonce"));
    if (yyjson_obj_get(v, "client_to_server_key")) out.clientToServerKey = hexArray<32>(str(v, "client_to_server_key"));
    if (yyjson_obj_get(v, "server_to_client_key")) out.serverToClientKey = hexArray<32>(str(v, "server_to_client_key"));
    out.userData = hexArray<256>(str(v, "user_data"));
    out.token = fromHex(str(v, "token"));
    return out;
}

/// All vectors from netcode_token_fixed.json and netcode_token_api.json (empty if absent).
std::vector<Vector> loadVectors() {
    std::vector<Vector> out;
    const std::filesystem::path dir = HELIOS_NET_TOKEN_VECTORS_DIR;
    for (const char* file : {"netcode_token_fixed.json", "netcode_token_api.json"}) {
        const std::filesystem::path path = dir / file;
        if (!std::filesystem::exists(path)) continue;
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
        REQUIRE(doc);
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (yyjson_val* arr = yyjson_obj_get(root, "vectors")) {
            usize idx, max;
            yyjson_val* item;
            yyjson_arr_foreach(arr, idx, max, item) { out.push_back(parseVector(item)); }
        } else {
            out.push_back(parseVector(root));
            out.back().name = "api";
        }
        yyjson_doc_free(doc);
    }
    return out;
}

} // namespace

TEST_SUITE("net.connect_token") {
    TEST_CASE("generate, parse and open round trip") {
        ConnectTokenParams p;
        p.protocolId = 0x1122334455667788ull;
        p.clientId = 0xDEADBEEF;
        p.timeoutSeconds = 7;
        p.expireSeconds = 45;
        p.publicAddresses = {Address::ipv4(203, 0, 113, 7, 7777), Address::parse("[2001:db8::7]:7778").value()};
        p.internalAddresses = {Address::ipv4(10, 1, 2, 3, 7777), Address::parse("[fd00::3]:7778").value()};
        p.privateKey = generateKey();
        for (usize i = 0; i < p.userData.size(); ++i) p.userData[i] = static_cast<u8>(i * 3);
        const ConnectTokenBytes token = generateConnectToken(p).value();

        const ConnectTokenInfo info = parseConnectToken(token).value();
        CHECK(info.protocolId == p.protocolId);
        CHECK(info.expireTimestamp - info.createTimestamp == 45);
        CHECK(info.timeoutSeconds == 7);
        CHECK(info.serverAddresses == p.publicAddresses);

        const PrivateConnectToken priv = openPrivateConnectToken(token, p.privateKey).value();
        CHECK(priv.clientId == 0xDEADBEEF);
        CHECK(priv.timeoutSeconds == 7);
        CHECK(priv.serverAddresses == p.internalAddresses);
        CHECK(priv.userData == p.userData);
        CHECK(priv.clientToServerKey == info.clientToServerKey); // the client's copy matches the sealed one
        CHECK(priv.serverToClientKey == info.serverToClientKey);
        CHECK(generateKey() != generateKey());
    }

    TEST_CASE("tampering, wrong keys and malformed tokens are rejected") {
        ConnectTokenParams p;
        p.protocolId = 5;
        p.clientId = 6;
        p.publicAddresses = {Address::ipv4(127, 0, 0, 1, 4000)};
        p.privateKey = testKey();
        const ConnectTokenBytes token = generateConnectToken(p).value();
        CHECK(openPrivateConnectToken(token, testKey(1)).errorCode() == ErrorCode::Corrupt);
        for (usize offset : {usize(13), usize(29), usize(40), usize(60), usize(600), usize(1080)}) {
            CAPTURE(offset);
            ConnectTokenBytes t = token; // protocol id, expire, nonce, sealed data: all authenticated
            t[offset] ^= 0x01;
            CHECK_FALSE(openPrivateConnectToken(t, p.privateKey));
        }
        ConnectTokenBytes badVersion = token;
        badVersion[9] = '9';
        CHECK(parseConnectToken(badVersion).errorCode() == ErrorCode::VersionMismatch);
        ConnectTokenBytes badPadding = token;
        badPadding[2047] = 1;
        CHECK(parseConnectToken(badPadding).errorCode() == ErrorCode::Corrupt);
        CHECK(parseConnectToken(std::span<const u8>(token.data(), 2047)).errorCode() == ErrorCode::InvalidArgument);
        ConnectTokenParams none = p;
        none.publicAddresses.clear();
        CHECK_FALSE(generateConnectToken(none));
        ConnectTokenParams mismatch = p;
        mismatch.internalAddresses = {Address::ipv4(1, 1, 1, 1, 1), Address::ipv4(2, 2, 2, 2, 2)};
        CHECK_FALSE(generateConnectToken(mismatch));
    }

    TEST_CASE("Go golden vectors: public and private parts decode byte-exactly") {
        const std::vector<Vector> vectors = loadVectors();
        if (vectors.empty()) {
            MESSAGE("no golden vectors in " HELIOS_NET_TOKEN_VECTORS_DIR "; skipping");
            return;
        }
        CHECK(vectors.size() >= 3);
        for (const Vector& v : vectors) {
            CAPTURE(v.name);
            REQUIRE(v.token.size() == kConnectTokenBytes);
            const ConnectTokenInfo info = parseConnectToken(v.token).value();
            CHECK(info.protocolId == v.protocolId);
            CHECK(info.timeoutSeconds == v.timeoutSeconds);
            CHECK(info.serverAddresses == v.publicAddresses);
            if (v.createTimestamp) CHECK(info.createTimestamp == *v.createTimestamp);
            if (v.expireTimestamp) CHECK(info.expireTimestamp == *v.expireTimestamp);
            if (v.expireSeconds) CHECK(info.expireTimestamp - info.createTimestamp == static_cast<u64>(*v.expireSeconds));
            if (v.nonce) CHECK(info.nonce == *v.nonce);
            if (v.clientToServerKey) CHECK(info.clientToServerKey == *v.clientToServerKey);
            if (v.serverToClientKey) CHECK(info.serverToClientKey == *v.serverToClientKey);

            const PrivateConnectToken priv = openPrivateConnectToken(v.token, v.privateKey).value();
            CHECK(priv.clientId == v.clientId);
            CHECK(priv.timeoutSeconds == v.timeoutSeconds);
            CHECK(priv.serverAddresses == v.internalAddresses);
            CHECK(priv.userData == v.userData);
            CHECK(priv.clientToServerKey == info.clientToServerKey);
            CHECK(priv.serverToClientKey == info.serverToClientKey);
        }
    }

    TEST_CASE("Go golden vector: a never-expiring Go-issued token completes the handshake") {
        const std::vector<Vector> vectors = loadVectors();
        const Vector* v = nullptr;
        for (const Vector& x : vectors)
            if (x.name == "fixed-ipv6-no-timeout") v = &x;
        if (!v) {
            MESSAGE("vector fixed-ipv6-no-timeout not present; skipping");
            return;
        }
        VirtualNetwork net;
        ServerConfig sc;
        sc.protocolId = v->protocolId;
        sc.privateKey = v->privateKey;
        sc.transport = net.bind(v->internalAddresses[0]).value(); // [2001:db8:85a3::8a2e:370:7334]:65535
        auto server = Server::create(std::move(sc), 0.0).value();
        ClientConfig cc;
        cc.transport = net.bind(Address::parse("[2001:db8::99]:5000").value()).value();
        auto client = Client::create(std::move(cc), 0.0).value();
        REQUIRE(client->connect(v->token, 0.0));
        RecordingHandler se, ce;
        f64 t = 0.0;
        for (int i = 0; i < 200 && !(client->isConnected() && !se.connected.empty()); ++i) {
            t += 0.01;
            server->update(t, se);
            client->update(t, ce);
            server->flush(t);
            client->flush(t);
        }
        REQUIRE(client->isConnected());
        REQUIRE(se.connected.size() == 1);
        const SessionInfo* info = server->sessionInfo(se.connected[0]);
        REQUIRE(info);
        CHECK(info->clientId == 0xFEDCBA9876543210ull);
        CHECK(info->userData == v->userData);
        CHECK(info->address == Address::parse("[2001:db8::99]:5000").value());
        REQUIRE(client->send(Channel::EventReliable, bytesOf("from go token")) == SendResult::Ok);
        for (int i = 0; i < 50 && se.messages.empty(); ++i) {
            t += 0.01;
            client->flush(t);
            server->update(t, se);
            client->update(t, ce);
        }
        REQUIRE(se.messages.size() == 1);
        CHECK(se.messages[0].data == bytesOf("from go token"));
    }

    TEST_CASE("Go golden vector: an expired Go-issued token is refused") {
        const std::vector<Vector> vectors = loadVectors();
        const Vector* v = nullptr;
        for (const Vector& x : vectors)
            if (x.name == "fixed-three-addresses") v = &x;
        if (!v) {
            MESSAGE("vector fixed-three-addresses not present; skipping");
            return;
        }
        VirtualNetwork net;
        ServerConfig sc;
        sc.protocolId = v->protocolId;
        sc.privateKey = v->privateKey;
        sc.transport = net.bind(v->internalAddresses[0]).value(); // 127.0.0.1:40000
        auto server = Server::create(std::move(sc), 0.0).value();
        ClientConfig cc;
        cc.transport = net.bind(Address::ipv4(127, 0, 0, 2, 5000)).value();
        auto client = Client::create(std::move(cc), 0.0).value();
        REQUIRE(client->connect(v->token, 0.0));
        RecordingHandler se, ce;
        for (int i = 0; i < 300; ++i) {
            const f64 t = 0.01 * (i + 1);
            server->update(t, se);
            client->update(t, ce);
        }
        CHECK_FALSE(client->isConnected());
        CHECK(server->connectedCount() == 0);
        CHECK(server->stats().preFilter.accepted > 0); // requests arrived; netcode refused the expiry
    }
}
