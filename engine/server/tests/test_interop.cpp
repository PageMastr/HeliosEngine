// NS-0.3 (04 §11.4): Windows client <-> Linux gateway interop, in two halves.
//
// * The bytes. Every client-facing and trunk message, and the trunk token's user data, encodes to
//   the bytes recorded in tests/data/wire_vectors.tsv and decodes back to the same values. Every
//   toolchain that runs server_tests (GCC and Clang here; MSVC, clang-cl in CI) checks the same
//   file, so a Windows build and a Linux build put identical bytes on the wire. Regenerate the
//   file only for a deliberate wire change: HELIOS_WRITE_WIRE_VECTORS=<path> server_tests
//   --test-case='server.interop: NS-0.3 wire*' writes it.
// * The hosts. With HELIOS_NS03_GATEWAY=<ip:port> and HELIOS_NS03_KEYS=<the gateway's netcode
//   keyring, e.g. the backend's keys/netcode-shard.json> set, the remote case connects a probe
//   over real UDP to that gateway (on the other host: helios-gateway plus a helios-cell serving
//   zone 0's default) and checks the Welcome, echo round trips and tick states. It is its own
//   CTest entry, server_tests_ns03_remote, which reports Skipped without the variables.
#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/time.h"
#include "helios/server/app_env.h"
#include "helios/server/keys.h"
#include "helios/server/probe_client.h"
#include "helios/server/protocol.h"

using namespace helios;
using namespace helios::server;

namespace {
using Vectors = std::vector<std::pair<std::string, std::vector<u8>>>;

std::vector<u8> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }

// Fixed values that exercise every field width: multi-byte varints, u64 above 2^53, non-ASCII
// UTF-8 (written as escapes, so the source character set of a compiler cannot change it).
constexpr u64 kBig = 9007199254740993ull;
constexpr u64 kTick = 123456789012ull;
const std::string kZoneName = "Tallis \xCE\xA9";

Vectors wireVectors() {
    namespace p = proto;
    const std::vector<u8> echo = p::encodeEchoRequest(7, bytesOf("echo 0"));
    Vectors v;
    v.emplace_back("TrunkHello", p::encode(0, p::Hello{p::kTrunkVersion, kBig, "gw-1"}));
    v.emplace_back("TrunkWelcome", p::encode(0, p::Welcome{p::kTrunkVersion, 77, 3, "cell-a"}));
    v.emplace_back("TrunkZoneFenced", p::encode(0, p::ZoneFenced{1002, 300}));
    v.emplace_back("TrunkAttach", p::encode(42, p::Attach{5, 1002, 0x0102030405060708ull, 77, p::kUserFlagReconnect}));
    v.emplace_back("TrunkAttachAck", p::encode(42, p::AttachAck{5, 1002, 7, kTick, 20, 560'000, kZoneName}));
    v.emplace_back("TrunkAttachNack", p::encode(42, p::AttachNack{5, 1002, p::NackReason::StaleEpoch}));
    v.emplace_back("TrunkDetach", p::encode(42, p::Detach{5, p::DetachReason::Superseded}));
    v.emplace_back("TrunkKick", p::encode(42, p::Kick{7, "cell asked"}));
    v.emplace_back("TrunkForward", p::encodeForward(1ull << 40, net::Channel::EventReliable, echo));
    v.emplace_back("TrunkDeliver", p::encodeDeliver(42, 300, net::Channel::State, p::encode(p::TickState{kTick, 560'000, 6'172'839'450'600'000'000ull, 3})));
    const net::UserData trunkUser = p::writeTrunkUserData(p::kTrunkPeerGateway, kBig);
    v.emplace_back("TrunkUserData", std::vector<u8>(trunkUser.begin(), trunkUser.end()));
    v.emplace_back("ClientWelcome", p::encode(p::ClientWelcome{42, 5, 1002, kTick, 20, 1'000'000, kZoneName}));
    v.emplace_back("ClientReconnectTicket", p::encode(p::ClientReconnectTicket{1'790'000'000'123, "tkt+/="}));
    v.emplace_back("ClientTimeDilation", p::encode(p::ClientTimeDilation{kTick + 2, 560'000}));
    v.emplace_back("ClientKick", p::encode(p::ClientKick{p::KickReason::Superseded, "reconnected elsewhere"}));
    v.emplace_back("ClientRouteState", p::encode(p::ClientRouteState{0, 1002}));
    v.emplace_back("ClientPing", p::encodePing(0x0123456789ABCDEFull));
    v.emplace_back("ClientPong", p::encodePong(0x0123456789ABCDEFull));
    v.emplace_back("EchoRequest", echo);
    v.emplace_back("EchoReply", p::encodeEchoReply(7, kTick, bytesOf("echo 0")));
    v.emplace_back("TickState", p::encode(p::TickState{kTick, 560'000, 6'172'839'450'600'000'000ull, 3}));
    return v;
}

std::string toHex(std::span<const u8> b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (u8 x : b) {
        out.push_back(kDigits[x >> 4]);
        out.push_back(kDigits[x & 15]);
    }
    return out;
}

std::vector<u8> fromHex(std::string_view h) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<u8>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    return out;
}

std::map<std::string, std::vector<u8>> loadWireVectors() {
    std::map<std::string, std::vector<u8>> out;
    std::ifstream in(std::string(HELIOS_SERVER_TEST_DATA) + "/wire_vectors.tsv");
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // a CRLF checkout on Windows
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        out[line.substr(0, tab)] = fromHex(line.substr(tab + 1));
    }
    return out;
}

bool remoteConfigured() { return envVar("HELIOS_NS03_GATEWAY").has_value() && envVar("HELIOS_NS03_KEYS").has_value(); }
} // namespace

TEST_CASE("server.interop: NS-0.3 wire bytes are the recorded ones on every toolchain") {
    const Vectors vectors = wireVectors();
    if (auto path = envVar("HELIOS_WRITE_WIRE_VECTORS")) {
        std::ofstream out(*path, std::ios::binary);
        out << "# NS-0.3 wire vectors (04 §11.4): the bytes of wireVectors() in engine/server/tests/test_interop.cpp,\n"
               "# which every toolchain must produce. Regenerate only for a deliberate wire change. name<TAB>hex\n";
        for (const auto& [name, bytes] : vectors) out << name << '\t' << toHex(bytes) << '\n';
        MESSAGE("wrote " << *path);
    }
    const auto recorded = loadWireVectors();
    CHECK(recorded.size() == vectors.size());
    for (const auto& [name, bytes] : vectors) {
        INFO(name);
        REQUIRE(recorded.contains(name));
        CHECK(toHex(bytes) == toHex(recorded.at(name)));
    }
}

TEST_CASE("server.interop: NS-0.3 the recorded bytes decode to the same values") {
    namespace p = proto;
    const auto rec = loadWireVectors();
    auto trunk = [&](const char* name, p::TrunkType type, u64 stream) {
        INFO(name);
        auto m = p::parseTrunk(rec.at(name));
        REQUIRE(m);
        CHECK(m->type == type);
        CHECK(m->stream == stream);
        return m->body;
    };
    auto hello = p::decodeHello(trunk("TrunkHello", p::TrunkType::Hello, 0));
    REQUIRE(hello);
    CHECK(hello->processId == kBig);
    CHECK(hello->name == "gw-1");
    auto welcome = p::decodeWelcome(trunk("TrunkWelcome", p::TrunkType::Welcome, 0));
    REQUIRE(welcome);
    CHECK((welcome->processId == 77 && welcome->epoch == 3 && welcome->name == "cell-a"));
    CHECK(p::decodeZoneFenced(trunk("TrunkZoneFenced", p::TrunkType::ZoneFenced, 0))->leaseGen == 300);
    auto attach = p::decodeAttach(trunk("TrunkAttach", p::TrunkType::Attach, 42));
    REQUIRE(attach);
    CHECK((attach->sessionEpoch == 5 && attach->zoneId == 1002 && attach->accountId == 0x0102030405060708ull &&
           attach->characterId == 77 && attach->flags == p::kUserFlagReconnect));
    auto ack = p::decodeAttachAck(trunk("TrunkAttachAck", p::TrunkType::AttachAck, 42));
    REQUIRE(ack);
    CHECK((ack->leaseGen == 7 && ack->tick == kTick && ack->tickHz == 20 && ack->dilationPpm == 560'000));
    CHECK(ack->zoneName == kZoneName);
    CHECK(p::decodeAttachNack(trunk("TrunkAttachNack", p::TrunkType::AttachNack, 42))->reason == p::NackReason::StaleEpoch);
    CHECK(p::decodeDetach(trunk("TrunkDetach", p::TrunkType::Detach, 42))->reason == p::DetachReason::Superseded);
    CHECK(p::decodeKick(trunk("TrunkKick", p::TrunkType::Kick, 42))->message == "cell asked");
    auto fwd = p::decodeForward(trunk("TrunkForward", p::TrunkType::Forward, 1ull << 40));
    REQUIRE(fwd);
    CHECK(fwd->channel == net::Channel::EventReliable);
    auto echo = p::decodeEchoRequest(fwd->payload);
    REQUIRE(echo);
    CHECK(echo->seq == 7);
    auto del = p::decodeDeliver(trunk("TrunkDeliver", p::TrunkType::Deliver, 42));
    REQUIRE(del);
    CHECK((del->leaseGen == 300 && del->channel == net::Channel::State));
    CHECK(p::decodeTickState(del->payload)->gameTimeNs == 6'172'839'450'600'000'000ull);

    auto cw = p::decodeClientWelcome(rec.at("ClientWelcome"));
    REQUIRE(cw);
    CHECK((cw->sessionId == 42 && cw->sessionEpoch == 5 && cw->zoneId == 1002 && cw->tick == kTick && cw->tickHz == 20));
    CHECK(cw->zoneName == kZoneName);
    auto ticket = p::decodeClientReconnectTicket(rec.at("ClientReconnectTicket"));
    REQUIRE(ticket);
    CHECK((ticket->expiresAtUnixMs == 1'790'000'000'123 && ticket->ticket == "tkt+/="));
    CHECK(p::decodeClientTimeDilation(rec.at("ClientTimeDilation"))->dilationPpm == 560'000);
    CHECK(p::decodeClientKick(rec.at("ClientKick"))->reason == p::KickReason::Superseded);
    CHECK(p::decodeClientRouteState(rec.at("ClientRouteState"))->zoneId == 1002);
    CHECK(p::clientControlType(rec.at("ClientPing")) == p::ClientControlType::Ping);
    CHECK(p::decodePingOrPong(rec.at("ClientPong"))->nonce == 0x0123456789ABCDEFull);
    auto reply = p::decodeEchoReply(rec.at("EchoReply"));
    REQUIRE(reply);
    CHECK((reply->seq == 7 && reply->tick == kTick));
    auto ts = p::decodeTickState(rec.at("TickState"));
    REQUIRE(ts);
    CHECK((ts->tick == kTick && ts->dilationPpm == 560'000 && ts->sessions == 3));
}

TEST_CASE("server.interop: NS-0.3 a probe on this host is served by the remote gateway in HELIOS_NS03_GATEWAY" *
          doctest::skip(!remoteConfigured())) {
    // Run on the Windows host against a Linux gateway (or the other way round): the criterion's
    // cross-host half, which needs two machines on one network and so is not part of CI.
    auto target = parseAddress(*envVar("HELIOS_NS03_GATEWAY"), "HELIOS_NS03_GATEWAY");
    REQUIRE(target);
    auto ring = loadKeyring(fs::pathFromUtf8(*envVar("HELIOS_NS03_KEYS")));
    REQUIRE(ring);
    auto key = netcodeKey(ring->current());
    REQUIRE(key);
    DevTokenParams t;
    t.protocolId = parseProtocolId(envVar("HELIOS_PROTOCOL_ID").value_or("0x48454c494f530001")).value();
    t.key = *key;
    t.sessionId = (monotonicNanos() & 0x7FFF'FFFF'FFFFull) | 1;
    t.gateways = {*target};
    t.user.accountId = t.sessionId;
    t.user.sessionEpoch = 1;
    auto p = ProbeClient::create(ProbeConfig{}, static_cast<i64>(monotonicNanos())).value();
    REQUIRE(p->connect(mintDevToken(t).value(), static_cast<i64>(monotonicNanos())));
    const u64 deadline = monotonicNanos() + 20'000'000'000ull;
    auto pumpUntil = [&](auto pred) {
        while (!pred() && monotonicNanos() < deadline) {
            p->update(static_cast<i64>(monotonicNanos()));
            sleepMillis(1);
        }
        return pred();
    };
    REQUIRE(pumpUntil([&] { return p->isWelcomed(); }));
    MESSAGE("NS-0.3: welcomed by " << target->toString() << " into zone " << p->stats().welcome->zoneId << " '"
                                   << p->stats().welcome->zoneName << "' at " << p->stats().welcome->tickHz << " Hz");
    for (u32 i = 0; i < 10; ++i) p->sendEcho(bytesOf("ns03 " + std::to_string(i)), static_cast<i64>(monotonicNanos()));
    CHECK(pumpUntil([&] { return p->stats().echoesReceived == 10 && p->stats().tickStates >= 20; }));
    CHECK(p->stats().echoMismatches == 0);
    CHECK(p->stats().tickRegressions == 0);
    MESSAGE("NS-0.3: echoes " << p->stats().echoesReceived << "/10, rtt max " << p->stats().maxRttMs << " ms, tick states "
                              << p->stats().tickStates);
    p->disconnect();
    p->update(static_cast<i64>(monotonicNanos()));
}
