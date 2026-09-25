// Wire contracts: the Go services' JSON (byte-exact against Go's own encoder), connect-token user
// data, keyrings, base64, RFC 3339, and the trunk / client binary messages.
#include <doctest/doctest.h>

#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <sstream>

#include "helios/server/keys.h"
#include "helios/server/orch_protocol.h"
#include "helios/server/protocol.h"

using namespace helios;
using namespace helios::server;

namespace {
std::map<std::string, std::string> loadGoVectors() {
    std::map<std::string, std::string> out;
    std::ifstream in(std::string(HELIOS_SERVER_TEST_DATA) + "/go_contract_vectors.tsv");
    REQUIRE(in.good());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        REQUIRE(tab != std::string::npos);
        out[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return out;
}

std::string text(const std::vector<u8>& b) { return std::string(b.begin(), b.end()); }
std::span<const u8> bytes(const std::string& s) { return {reinterpret_cast<const u8*>(s.data()), s.size()}; }

std::vector<u8> fromHex(std::string_view h) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<u8>(std::stoi(std::string(h.substr(i, 2)), nullptr, 16)));
    return out;
}
} // namespace

TEST_CASE("server.contracts: requests the C++ side sends are byte-identical to Go's encoding") {
    const auto go = loadGoVectors();
    orch::ProcessInfo cell;
    cell.name = "cell-a";
    cell.kind = "cell";
    cell.address = "127.0.0.1:7810";
    cell.version = "0.1.0";
    cell.zones = {"tallis"};
    CHECK(text(orch::encode(cell)) == go.at("ProcessInfoCell"));

    orch::ProcessInfo gw;
    gw.name = "gw-1";
    gw.kind = "gateway";
    gw.address = "127.0.0.1:7777";
    gw.keyId = 3;
    gw.capacity = 256;
    CHECK(text(orch::encode(gw)) == go.at("ProcessInfoGateway"));

    CHECK(text(orch::encode(orch::HeartbeatRequest{9007199254740993ull, 2, orch::Load{3, 0, 0.25}})) == go.at("HeartbeatRequest"));
    CHECK(text(orch::encode(orch::AllocateIdBlocksRequest{9007199254740993ull, 2, 2})) == go.at("AllocateIdBlocksRequest"));
    CHECK(text(orch::encode(orch::DeregisterRequest{9007199254740993ull, 2})) == go.at("DeregisterRequest"));
    CHECK(text(orch::encode(orch::ResolveZoneRequest{1002, {}})) == go.at("ResolveZoneRequestId"));
    CHECK(text(orch::encode(orch::ResolveZoneRequest{0, "tallis"})) == go.at("ResolveZoneRequestName"));

    orch::SealRequest seal;
    seal.sessions.push_back(orch::SealItem{9223372036854775807ull, 1002, 4, "abc"});
    seal.sessions.push_back(orch::SealItem{12, 0, 0, {}});
    CHECK(text(orch::encode(seal)) == go.at("SealRequest"));

    // The fakes answer with the Go shapes too.
    orch::RegisterResult rr;
    rr.processId = 9007199254740993ull;
    rr.epoch = 2;
    rr.leaseTtlMs = 12000;
    rr.heartbeatIntervalMs = 1000;
    rr.assignments = {orch::Assignment{1002, "tallis", 7}};
    rr.idShard = 3;
    rr.idBlocks = {101, 102};
    CHECK(text(orch::encode(rr)) == go.at("RegisterResult"));
    CHECK(text(orch::encode(orch::Route{1002, "tallis", 7, 9007199254740993ull, "cell-a", 2, "127.0.0.1:7810"})) == go.at("Route"));
    CHECK(text(orch::encode(orch::ControlMessage{42, 0, "superseded"})) == go.at("ControlKick"));
    CHECK(text(orch::encode(orch::ControlMessage{42, 5, "reconnect"})) == go.at("ControlEpoch"));
}

TEST_CASE("server.contracts: replies written by Go decode") {
    const auto go = loadGoVectors();
    auto rr = orch::decodeRegisterResult(bytes(go.at("RegisterResult")));
    REQUIRE(rr);
    CHECK(rr->processId == 9007199254740993ull); // above 2^53: must travel as a string
    CHECK(rr->epoch == 2);
    CHECK(rr->heartbeatIntervalMs == 1000);
    REQUIRE(rr->assignments.size() == 1);
    CHECK(rr->assignments[0].zoneId == 1002);
    CHECK(rr->assignments[0].zoneName == "tallis");
    CHECK(rr->assignments[0].leaseGen == 7);
    CHECK(rr->idShard == 3);
    CHECK(rr->idBlocks == std::vector<u64>{101, 102});

    auto gwr = orch::decodeRegisterResult(bytes(go.at("RegisterResultGateway")));
    REQUIRE(gwr);
    CHECK(gwr->assignments.empty());
    CHECK(gwr->idBlocks.empty());

    auto hb = orch::decodeHeartbeatResult(bytes(go.at("HeartbeatResult")));
    REQUIRE(hb);
    CHECK(hb->mode == "normal");
    CHECK(hb->leaseExpires == "2026-09-25T12:34:56.789Z");
    REQUIRE(hb->assignments.size() == 1);

    auto ab = orch::decodeAllocateIdBlocksResponse(bytes(go.at("AllocateIdBlocksResponse")));
    REQUIRE(ab);
    CHECK(ab->idShard == 3);
    CHECK(ab->prefixes == std::vector<u64>{103, 104});

    auto route = orch::decodeRoute(bytes(go.at("Route")));
    REQUIRE(route);
    CHECK(route->address == "127.0.0.1:7810");
    CHECK(route->leaseGen == 7);
    CHECK(route->process == "cell-a");

    auto seal = orch::decodeSealResponse(bytes(go.at("SealResponse")));
    REQUIRE(seal);
    REQUIRE(seal->tickets.size() == 1);
    CHECK(seal->tickets[0].sessionId == 9223372036854775807ull);
    CHECK(seal->missing == std::vector<u64>{12});
    CHECK(orch::parseRfc3339UnixMs(seal->tickets[0].expiresAt).value() == orch::parseRfc3339UnixMs("2026-09-25T12:34:56.789Z").value());

    auto kick = orch::decodeControlMessage(bytes(go.at("ControlKick")));
    REQUIRE(kick);
    CHECK(kick->sessionId == 42);
    CHECK(kick->epoch == 0);
    CHECK(kick->reason == "superseded");
    auto epoch = orch::decodeControlMessage(bytes(go.at("ControlEpoch")));
    REQUIRE(epoch);
    CHECK(epoch->epoch == 5);

    // Requests decode too (the fakes rely on it), and every vector round-trips.
    CHECK(orch::decodeProcessInfo(bytes(go.at("ProcessInfoCell")))->zones == std::vector<std::string>{"tallis"});
    CHECK(orch::decodeProcessInfo(bytes(go.at("ProcessInfoGateway")))->keyId == 3);
    CHECK(orch::decodeHeartbeatRequest(bytes(go.at("HeartbeatRequest")))->load.tickP99Ms == doctest::Approx(0.25));
    CHECK(orch::decodeSealRequest(bytes(go.at("SealRequest")))->sessions.size() == 2);
}

TEST_CASE("server.contracts: decoders are lenient about numbers and strict about types") {
    CHECK(orch::decodeRoute(orch::bytesOf(R"({"zoneId":1002,"leaseGen":"3","unknownField":{"x":[1]}})"))->zoneId == 1002);
    CHECK_FALSE(orch::decodeRoute(orch::bytesOf(R"({"zoneId":"12x"})")));
    CHECK_FALSE(orch::decodeRoute(orch::bytesOf(R"({"zoneId":true})")));
    CHECK_FALSE(orch::decodeRoute(orch::bytesOf(R"({"zoneId":-4})")));
    CHECK_FALSE(orch::decodeRoute(orch::bytesOf("[1,2]")));
    CHECK_FALSE(orch::decodeRoute(orch::bytesOf("not json")));
    CHECK_FALSE(orch::decodeRegisterResult(orch::bytesOf("{}"))); // a registration needs a process id
    CHECK_FALSE(orch::decodeSealResponse(orch::bytesOf(R"({"tickets":{}})")));
    CHECK(orch::orchestratorSubject("eu1", "Heartbeat") == "rpc.eu1.orch.Heartbeat");
    CHECK(orch::sealTicketsSubject("dev") == "rpc.dev.session.SealReconnectTickets");
    CHECK(orch::gatewayControlSubject("dev", "session_epoch") == "ctl.dev.gateway.all.session_epoch");
}

TEST_CASE("server.contracts: connect-token user data matches Go's layout v1") {
    const auto go = loadGoVectors();
    const std::vector<u8> raw = fromHex(go.at("UserDataV1"));
    REQUIRE(raw.size() == net::kUserDataBytes);
    net::UserData ud{};
    std::copy(raw.begin(), raw.end(), ud.begin());
    auto u = proto::parseUserData(ud);
    REQUIRE(u);
    CHECK(u->flags == (proto::kUserFlagReconnect | proto::kUserFlagBot));
    CHECK(u->accountId == 0x0102030405060708ull);
    CHECK(u->characterId == 77);
    CHECK(u->sessionEpoch == 5);
    CHECK(u->contentBuild == 1234);
    CHECK(u->zoneId == 1002);
    CHECK(u->placementTicket == 99);
    CHECK(u->entitlements == 0xF0F0);
    CHECK(u->attestationHash[31] == 93);
    CHECK(proto::writeUserData(*u) == ud); // and back, byte for byte
    ud[0] = 2;
    CHECK_FALSE(proto::parseUserData(ud));
}

TEST_CASE("server.keys: base64, keyrings and protocol ids") {
    for (usize n = 0; n < 40; ++n) {
        std::vector<u8> data(n);
        for (usize i = 0; i < n; ++i) data[i] = static_cast<u8>(i * 37 + n);
        const std::string enc = base64Encode(data);
        CHECK(enc.size() % 4 == 0);
        CHECK(base64Decode(enc).value() == data);
    }
    CHECK(base64Encode(std::vector<u8>{'M', 'a', 'n'}) == "TWFu");
    CHECK(base64Encode(std::vector<u8>{'M', 'a'}) == "TWE=");
    CHECK(base64Decode("TWE").value() == std::vector<u8>{'M', 'a'}); // padding optional
    CHECK_FALSE(base64Decode("T!E="));
    CHECK_FALSE(base64Decode("TWF")); // "TWF" has non-zero leftover bits (non-canonical)

    const std::string ring = R"({"version":1,"purpose":"netcode-shard","keys":[
        {"id":2,"secret":"AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA=","created":"2026-09-25T00:00:00Z"},
        {"id":1,"secret":"ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICA=","created":"2026-09-24T00:00:00Z"}]})";
    auto kr = parseKeyring(ring);
    REQUIRE(kr);
    CHECK(kr->purpose == "netcode-shard");
    CHECK(kr->current().id == 2);
    CHECK(kr->current().secretBase64 == "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA=");
    auto key = netcodeKey(kr->current());
    REQUIRE(key);
    CHECK((*key)[0] == 1);
    CHECK((*key)[31] == 32);
    CHECK(kr->find(1) != nullptr);
    CHECK_FALSE(parseKeyring(R"({"keys":[]})"));
    CHECK_FALSE(parseKeyring(R"({"keys":[{"id":1,"secret":"%%"}]})"));
    KeyringEntry shortKey{9, "AQI=", {1, 2}};
    CHECK_FALSE(netcodeKey(shortKey));

    CHECK(parseProtocolId("0x48454c494f530001").value() == kDefaultClientProtocolId);
    CHECK(parseProtocolId("12345").value() == 12345);
    CHECK_FALSE(parseProtocolId("0xZZ"));
    // The backend passes session.protocol_id verbatim (HELIOS_PROTOCOL_ID) and reads it with Go's
    // strconv.ParseUint(strings.TrimSpace(s), 0, 64): the gateway must read the same number.
    CHECK(parseProtocolId("1234567890123456").value() == 1234567890123456ull); // 16 digits, still decimal
    CHECK_FALSE(parseProtocolId("48454c494f530001"));                         // Go refuses bare hex
    CHECK(parseProtocolId(" 0X48454C494F530001\n").value() == kDefaultClientProtocolId);
    CHECK(parseProtocolId("0755").value() == 0755);
    CHECK(parseProtocolId("0o17").value() == 15);
    CHECK(parseProtocolId("0b101").value() == 5);
    CHECK(parseProtocolId("1_000_000").value() == 1000000);
    CHECK(parseProtocolId("0x_ff").value() == 255);
    CHECK(parseProtocolId("0").value() == 0);
    CHECK_FALSE(parseProtocolId("_1"));
    CHECK_FALSE(parseProtocolId("1__0"));
    CHECK_FALSE(parseProtocolId("10_"));
    CHECK_FALSE(parseProtocolId("09"));
    CHECK_FALSE(parseProtocolId(""));
    CHECK_FALSE(parseProtocolId("18446744073709551616"));
    CHECK(insecureDevKey("a") == insecureDevKey("a"));
    CHECK(insecureDevKey("a") != insecureDevKey("b"));
}

TEST_CASE("server.contracts: RFC 3339 times") {
    CHECK(orch::parseRfc3339UnixMs("1970-01-01T00:00:00Z").value() == 0);
    CHECK(orch::parseRfc3339UnixMs("2026-01-01T00:00:00Z").value() == 1767225600000ll);
    CHECK(orch::parseRfc3339UnixMs("2026-01-01T02:00:00+02:00").value() == 1767225600000ll);
    CHECK(orch::parseRfc3339UnixMs("2025-12-31T19:00:00.5-05:00").value() == 1767225600500ll);
    CHECK(orch::parseRfc3339UnixMs("2026-09-25T12:34:56.789123456Z").value() % 1000 == 789);
    CHECK_FALSE(orch::parseRfc3339UnixMs("2026-09-25 12:34:56Z"));
    CHECK_FALSE(orch::parseRfc3339UnixMs("2026-09-25T12:34:56"));
    CHECK_FALSE(orch::parseRfc3339UnixMs("2026-13-25T12:34:56Z"));
    for (i64 ms : {0ll, 1767225600000ll, 1790000045123ll, 946684799999ll})
        CHECK(orch::parseRfc3339UnixMs(orch::formatRfc3339UnixMs(ms)).value() == ms);
    CHECK(orch::formatRfc3339UnixMs(1767225600500ll) == "2026-01-01T00:00:00.500Z");
}

TEST_CASE("server.protocol: trunk messages round-trip") {
    auto roundTrip = [](const std::vector<u8>& wire, proto::TrunkType type, u64 stream) {
        auto m = proto::parseTrunk(wire);
        REQUIRE(m);
        CHECK(m->type == type);
        CHECK(m->stream == stream);
        return std::vector<u8>(m->body.begin(), m->body.end());
    };
    {
        auto body = roundTrip(proto::encode(0, proto::Hello{1, 77, "gw-1"}), proto::TrunkType::Hello, 0);
        auto h = proto::decodeHello(body);
        REQUIRE(h);
        CHECK(h->processId == 77);
        CHECK(h->name == "gw-1");
    }
    {
        auto body = roundTrip(proto::encode(0, proto::Welcome{1, 9, 3, "cell-a"}), proto::TrunkType::Welcome, 0);
        CHECK(proto::decodeWelcome(body)->epoch == 3);
    }
    {
        auto body = roundTrip(proto::encode(0, proto::ZoneFenced{1002, 300}), proto::TrunkType::ZoneFenced, 0);
        CHECK(proto::decodeZoneFenced(body)->leaseGen == 300);
    }
    const u64 sid = 0x7FFF'FFFF'FFFF'FFFFull;
    {
        auto body = roundTrip(proto::encode(sid, proto::Attach{5, 1002, 11, 12, 3}), proto::TrunkType::Attach, sid);
        auto a = proto::decodeAttach(body);
        REQUIRE(a);
        CHECK(a->sessionEpoch == 5);
        CHECK(a->zoneId == 1002);
        CHECK(a->characterId == 12);
        CHECK(a->flags == 3);
    }
    {
        proto::AttachAck ack{5, 1002, 7, 1234, 20, 530000, "tallis"};
        auto body = roundTrip(proto::encode(sid, ack), proto::TrunkType::AttachAck, sid);
        auto a = proto::decodeAttachAck(body);
        REQUIRE(a);
        CHECK(a->tick == 1234);
        CHECK(a->dilationPpm == 530000);
        CHECK(a->zoneName == "tallis");
    }
    {
        auto body = roundTrip(proto::encode(sid, proto::AttachNack{5, 1002, proto::NackReason::StaleEpoch}), proto::TrunkType::AttachNack, sid);
        CHECK(proto::decodeAttachNack(body)->reason == proto::NackReason::StaleEpoch);
    }
    {
        auto body = roundTrip(proto::encode(sid, proto::Detach{5, proto::DetachReason::TimedOut}), proto::TrunkType::Detach, sid);
        CHECK(proto::decodeDetach(body)->reason == proto::DetachReason::TimedOut);
    }
    {
        const std::vector<u8> payload{1, 2, 3};
        auto body = roundTrip(proto::encodeForward(sid, net::Channel::Input, payload), proto::TrunkType::Forward, sid);
        auto f = proto::decodeForward(body);
        REQUIRE(f);
        CHECK(f->channel == net::Channel::Input);
        CHECK(std::vector<u8>(f->payload.begin(), f->payload.end()) == payload);
        body = roundTrip(proto::encodeDeliver(sid, 9, net::Channel::State, payload), proto::TrunkType::Deliver, sid);
        auto d = proto::decodeDeliver(body);
        REQUIRE(d);
        CHECK(d->leaseGen == 9);
        CHECK(d->channel == net::Channel::State);
    }
    CHECK(proto::trunkChannelFor(net::Channel::Control) == net::Channel::EventReliable);
    CHECK(proto::trunkChannelFor(net::Channel::Bulk) == net::Channel::EventReliable);
    CHECK(proto::trunkChannelFor(net::Channel::Input) == net::Channel::EventUnreliable);
    CHECK(proto::trunkChannelFor(net::Channel::State) == net::Channel::EventUnreliable);
    CHECK(proto::kMaxForwardPayload + proto::kTrunkOverhead == proto::kMaxEventPayload);
}

TEST_CASE("server.protocol: malformed trunk and client messages are rejected, never crash") {
    CHECK_FALSE(proto::parseTrunk(std::vector<u8>{}));
    CHECK_FALSE(proto::parseTrunk(std::vector<u8>{0x00, 0x10}));        // session message on stream 0
    CHECK_FALSE(proto::parseTrunk(std::vector<u8>{0x05, 0x01}));        // trunk message on a session stream
    CHECK_FALSE(proto::parseTrunk(std::vector<u8>{0x05, 0x77}));        // unknown type
    CHECK_FALSE(proto::parseTrunk(std::vector<u8>{0x80, 0x00, 0x10})); // non-canonical varint
    CHECK_FALSE(proto::decodeAttach(std::vector<u8>{1, 2, 3}));
    auto good = proto::encode(9, proto::Attach{5, 1002, 11, 12, 3});
    good.push_back(0); // trailing garbage
    CHECK_FALSE(proto::decodeAttach(proto::parseTrunk(good)->body));
    CHECK_FALSE(proto::decodeForward(std::vector<u8>{15, 1}));        // channel 15
    CHECK_FALSE(proto::decodeAttachNack(std::vector<u8>(17, 0)));     // reason 0
    CHECK_FALSE(proto::decodeClientWelcome(proto::encode(proto::ClientKick{})));
    CHECK_FALSE(proto::decodeClientKick(std::vector<u8>{0x04, 1, 0xFF}));
    CHECK_FALSE(proto::clientControlType(std::vector<u8>{0x44}).has_value());
    // Random bytes through every decoder.
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
        std::vector<u8> b(rng() % 48);
        for (u8& x : b) x = static_cast<u8>(rng());
        if (auto m = proto::parseTrunk(b)) {
            (void)proto::decodeHello(m->body);
            (void)proto::decodeWelcome(m->body);
            (void)proto::decodeZoneFenced(m->body);
            (void)proto::decodeAttach(m->body);
            (void)proto::decodeAttachAck(m->body);
            (void)proto::decodeAttachNack(m->body);
            (void)proto::decodeDetach(m->body);
            (void)proto::decodeKick(m->body);
            (void)proto::decodeForward(m->body);
            (void)proto::decodeDeliver(m->body);
        }
        (void)proto::decodeClientWelcome(b);
        (void)proto::decodeClientReconnectTicket(b);
        (void)proto::decodeClientTimeDilation(b);
        (void)proto::decodeClientRouteState(b);
        (void)proto::decodePingOrPong(b);
        (void)proto::decodeEchoRequest(b);
        (void)proto::decodeEchoReply(b);
        (void)proto::decodeTickState(b);
    }
}

TEST_CASE("server.protocol: client messages round-trip") {
    auto w = proto::decodeClientWelcome(proto::encode(proto::ClientWelcome{1, 2, 3, 4, 20, 999000, "tallis"}));
    REQUIRE(w);
    CHECK(w->zoneName == "tallis");
    CHECK(w->dilationPpm == 999000);
    auto t = proto::decodeClientReconnectTicket(proto::encode(proto::ClientReconnectTicket{1767225600500ll, "sealed"}));
    REQUIRE(t);
    CHECK(t->ticket == "sealed");
    CHECK(t->expiresAtUnixMs == 1767225600500ll);
    CHECK(proto::decodeClientTimeDilation(proto::encode(proto::ClientTimeDilation{42, 500000}))->effectiveTick == 42);
    CHECK(proto::decodeClientKick(proto::encode(proto::ClientKick{proto::KickReason::Superseded, "x"}))->reason ==
          proto::KickReason::Superseded);
    CHECK(proto::decodePingOrPong(proto::encodePong(99))->nonce == 99);
    const std::vector<u8> data{9, 8, 7};
    auto er = proto::decodeEchoReply(proto::encodeEchoReply(5, 77, data));
    REQUIRE(er);
    CHECK(er->seq == 5);
    CHECK(er->tick == 77);
    auto ts = proto::decodeTickState(proto::encode(proto::TickState{10, 20, 30, 40}));
    REQUIRE(ts);
    CHECK(ts->sessions == 40);
    CHECK(proto::kickReasonFromString("reconnect") == proto::KickReason::Superseded);
    CHECK(proto::kickReasonFromString("logout") == proto::KickReason::Logout);
}

TEST_CASE("server.contracts: non-finite and out-of-range numbers stay defined and valid JSON") {
    // Encoding: JSON (and Go) have no NaN or infinity; a broken measurement is reported as 0
    // instead of producing a document the orchestrator cannot parse.
    for (const f64 bad : {std::numeric_limits<f64>::quiet_NaN(), std::numeric_limits<f64>::infinity(),
                          -std::numeric_limits<f64>::infinity()}) {
        const std::vector<u8> json = orch::encode(orch::HeartbeatRequest{1, 1, orch::Load{2, 0, bad}});
        REQUIRE_FALSE(json.empty());
        CHECK(text(json).find("\"tickP99Ms\":0") != std::string::npos);
        auto back = orch::decodeHeartbeatRequest(json);
        REQUIRE(back);
        CHECK(back->load.tickP99Ms == 0.0);
        CHECK(back->load.players == 2);
    }
    // Huge finite values are written as reals and read back.
    auto big = orch::decodeHeartbeatRequest(orch::encode(orch::HeartbeatRequest{1, 1, orch::Load{0, 0, 1e300}}));
    REQUIRE(big);
    CHECK(big->load.tickP99Ms == 1e300);
    // Decoding: a real where Go has an integer must be integral and in range (Go refuses the rest).
    CHECK_FALSE(orch::decodeProcessInfo(bytes(R"({"name":"a","kind":"cell","pid":1e300})")));
    CHECK_FALSE(orch::decodeProcessInfo(bytes(R"({"name":"a","kind":"cell","pid":-1e30})")));
    CHECK_FALSE(orch::decodeProcessInfo(bytes(R"({"name":"a","kind":"cell","pid":1.5})")));
    auto whole = orch::decodeProcessInfo(bytes(R"({"name":"a","kind":"cell","pid":12.0})"));
    REQUIRE(whole);
    CHECK(whole->pid == 12);
    CHECK_FALSE(orch::decodeRegisterResult(bytes(R"({"processId":"7","leaseTtlMs":1e19})")));
}
