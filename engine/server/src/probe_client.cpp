#include "helios/server/probe_client.h"

#include <algorithm>

namespace helios::server {

Result<net::ConnectTokenBytes> mintDevToken(const DevTokenParams& p) {
    net::ConnectTokenParams t;
    t.protocolId = p.protocolId;
    t.clientId = p.sessionId;
    t.expireSeconds = p.expireSeconds;
    t.timeoutSeconds = p.timeoutSeconds;
    t.publicAddresses = p.gateways;
    t.privateKey = p.key;
    t.userData = proto::writeUserData(p.user);
    return net::generateConnectToken(t);
}

struct ProbeClient::Handler final : net::IEndpointHandler {
    ProbeClient& probe;
    explicit Handler(ProbeClient& p) : probe(p) {}
    void onDisconnected(net::SessionHandle, net::DisconnectReason r) override { probe.m_stats.disconnected = r; }
    void onMessage(net::SessionHandle, net::Channel c, std::span<const u8> p) override { probe.onMessage(c, p); }
};

ProbeClient::~ProbeClient() = default;

Result<std::unique_ptr<ProbeClient>> ProbeClient::create(ProbeConfig config, i64 nowNs) {
    std::unique_ptr<ProbeClient> p(new ProbeClient());
    net::ClientConfig cc;
    cc.transport = std::move(config.transport);
    cc.bindAddress = config.bind;
    cc.name = config.name;
    HELIOS_TRY_ASSIGN(p->m_client, net::Client::create(std::move(cc), static_cast<f64>(nowNs) * 1e-9));
    p->m_handler = std::make_unique<Handler>(*p);
    p->m_now = nowNs;
    return p;
}

Result<void> ProbeClient::connect(std::span<const u8> token, i64 nowNs) {
    m_now = nowNs;
    m_stats.disconnected.reset();
    return m_client->connect(token, static_cast<f64>(nowNs) * 1e-9);
}

void ProbeClient::update(i64 nowNs) {
    m_now = nowNs;
    m_client->update(static_cast<f64>(nowNs) * 1e-9, *m_handler);
    m_client->flush(static_cast<f64>(nowNs) * 1e-9);
}

bool ProbeClient::isConnected() const {
    return m_client->state() == net::ClientState::Connected;
}

u32 ProbeClient::sendEcho(std::span<const u8> data, i64 nowNs) {
    const u32 seq = m_nextSeq++;
    if (m_client->send(net::Channel::EventReliable, proto::encodeEchoRequest(seq, data)) == net::SendResult::Ok) {
        m_inFlight.emplace_back(seq, nowNs);
        ++m_stats.echoesSent;
    }
    return seq;
}

void ProbeClient::sendPing(u64 nonce) {
    (void)m_client->send(net::Channel::Control, proto::encodePing(nonce));
}

net::SendResult ProbeClient::send(net::Channel channel, std::span<const u8> payload) {
    return m_client->send(channel, payload);
}

void ProbeClient::disconnect() {
    m_client->disconnect();
}

void ProbeClient::onMessage(net::Channel channel, std::span<const u8> payload) {
    if (channel == net::Channel::Control) {
        switch (proto::clientControlType(payload).value_or(proto::ClientControlType::Ping)) {
        case proto::ClientControlType::Welcome:
            if (auto w = proto::decodeClientWelcome(payload)) {
                m_stats.welcome = std::move(*w);
                ++m_stats.welcomes;
                m_stats.lastDilationPpm = m_stats.welcome->dilationPpm;
                return;
            }
            break;
        case proto::ClientControlType::ReconnectTicket:
            if (auto t = proto::decodeClientReconnectTicket(payload)) {
                m_stats.tickets.push_back(std::move(*t));
                return;
            }
            break;
        case proto::ClientControlType::TimeDilation:
            if (auto d = proto::decodeClientTimeDilation(payload)) {
                m_stats.dilations.push_back(*d);
                return;
            }
            break;
        case proto::ClientControlType::Kick:
            if (auto k = proto::decodeClientKick(payload)) {
                m_stats.kick = std::move(*k);
                return;
            }
            break;
        case proto::ClientControlType::RouteState:
            if (auto r = proto::decodeClientRouteState(payload)) {
                m_stats.routeStates.push_back(*r);
                return;
            }
            break;
        case proto::ClientControlType::Pong:
            ++m_stats.pongs;
            return;
        case proto::ClientControlType::Ping: break;
        }
        ++m_stats.otherMessages;
        return;
    }
    if (channel == net::Channel::State) {
        if (auto ts = proto::decodeTickState(payload)) {
            if (ts->tick < m_stats.lastTick) ++m_stats.tickRegressions;
            m_stats.lastTick = ts->tick;
            m_stats.lastDilationPpm = ts->dilationPpm;
            ++m_stats.tickStates;
            return;
        }
    }
    if (channel == net::Channel::EventReliable) {
        if (auto e = proto::decodeEchoReply(payload)) {
            auto it = std::find_if(m_inFlight.begin(), m_inFlight.end(), [&](const auto& p) { return p.first == e->seq; });
            if (it == m_inFlight.end()) {
                ++m_stats.echoMismatches;
                return;
            }
            const f64 rtt = static_cast<f64>(m_now - it->second) * 1e-6;
            m_inFlight.erase(it);
            ++m_stats.echoesReceived;
            m_stats.lastRttMs = rtt;
            m_stats.maxRttMs = std::max(m_stats.maxRttMs, rtt);
            m_stats.totalRttMs += rtt;
            return;
        }
    }
    ++m_stats.otherMessages;
}

} // namespace helios::server
