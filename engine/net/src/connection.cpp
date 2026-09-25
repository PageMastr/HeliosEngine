// Connection: HTP channels, packing, message reliability, notify, budget/AIMD over `reliable`.

#include "helios/net/connection.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <vector>

#include <reliable.h>

#include "helios/core/assert.h"
#include "helios/net/wire.h"
#include "net_internal.h"

namespace helios::net {

using detail::seqDiff;
using detail::seqGreater;

// ---------------------------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------------------------

namespace {
/// Public-link bounds on what an authenticated but hostile peer can make the other side hold.
/// 64 KB of unacked payload per reliable channel is 2 s of the 256 kbit/s budget (still ~0.5 s at
/// Phase 5's 1 Mbit/s), so it never limits a legitimate sender, and it caps the receiver's
/// out-of-order buffer at 4 x (64 + 16) KB instead of ~1.1 MB. Jumbo CONTROL packets are rare and
/// resent as new packets, so 8 concurrent reassemblies (8 x 17 KB) are plenty.
void applyClientLinkLimits(ConnectionConfig& c) {
    c.maxWindowBytes = 64u * 1024;
    c.maxReorderBytes = 4 * (c.maxWindowBytes + c.maxJumboMessageBytes);
    c.fragmentReassemblyBufferSize = 8;
}
} // namespace

ConnectionConfig ConnectionConfig::client() {
    ConnectionConfig c;
    // 04 §2.5: upstream min(tick_hz, 60) pps under a 64 kbit/s token bucket. The client flushes
    // once per client tick, so the packet rate follows the tick; the bucket is fixed (no AIMD).
    c.budgetBps = 64'000;
    c.minBudgetBps = 64'000;
    c.maxBudgetBps = 64'000;
    c.aimd = false;
    c.maxPacketsPerFlush = 1;
    c.maxPacketsPerSecond = 60.0;
    c.burstSeconds = 0.25;
    // Acks ride the client's tick packets (<= 60 Hz); an ack-only packet only when no tick packet
    // left for 20 ms, so upstream stays within the server's 120 pps policing (04 §9).
    c.maxAckDelay = 0.020;
    c.maxLazyAckDelay = 0.020;
    c.peerMaxAckDelay = 0.010;
    c.peerMaxLazyAckDelay = 0.100;
    applyClientLinkLimits(c);
    return c;
}

ConnectionConfig ConnectionConfig::server() {
    ConnectionConfig c;
    c.budgetBps = 256'000;
    c.minBudgetBps = 64'000;
    c.maxBudgetBps = 256'000;
    c.aimd = true;
    // 04 §9: <= 120 pps (240 burst) and 64 kbit/s up per session, plus the 40 kbit/s VOICE bucket.
    c.inboundPacketsPerSecond = 120.0;
    c.inboundPacketBurst = 240.0;
    c.inboundBytesPerSecond = 64'000.0 / 8.0;
    c.inboundByteBurst = 32.0 * 1024.0; // a 16 KB CONTROL burst fits
    c.inboundVoiceBytesPerSecond = 40'000.0 / 8.0;
    c.inboundVoiceByteBurst = 4.0 * 1024.0;
    // Reliable client messages are acked within 10 ms; a client's INPUT-only packets (60 pps) are
    // acked by the next tick packet, so the gateway sends one packet per tick (04 §2.5, §2.6: 20-30
    // pps down per session) instead of an extra ack-only packet per client packet.
    c.maxAckDelay = 0.010;
    c.maxLazyAckDelay = 0.100;
    c.peerMaxAckDelay = 0.020; // clients ack at their tick rate
    c.peerMaxLazyAckDelay = 0.020;
    applyClientLinkLimits(c);
    return c;
}

ConnectionConfig ConnectionConfig::trunk() {
    ConnectionConfig c;
    // 04 §2.6 trunk profile: 4,096-entry packet buffers (256 would cover 13 ms at 20k pps),
    // fragments reassemble up to 256 KB.
    c.sentPacketsBufferSize = 4096;
    c.receivedPacketsBufferSize = 4096;
    c.fragmentReassemblyBufferSize = 64;
    c.maxJumboMessageBytes = 256 * 1024 - 16;
    c.maxWindowBytes = 16u * 1024 * 1024;
    c.maxReorderBytes = 4 * (c.maxWindowBytes + c.maxJumboMessageBytes);
    c.budgetBps = 2'000'000'000u;
    c.minBudgetBps = 2'000'000'000u;
    c.maxBudgetBps = 2'000'000'000u;
    c.aimd = false;
    c.burstSeconds = 0.01;
    c.maxPacketsPerFlush = 1u << 20;
    c.pacingInterval = 0.0;
    c.ackEveryPackets = 16;
    c.maxAckDelay = 0.002;
    c.maxLazyAckDelay = 0.002;
    c.peerMaxAckDelay = 0.002;
    c.peerMaxLazyAckDelay = 0.002;
    c.unreliableMaxAge = 0.05;
    return c;
}

// ---------------------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------------------

namespace {
constexpr u32 kJumboSlack = 16; // header bytes around a jumbo message
constexpr f64 kReliableStatsInterval = 0.010;
constexpr usize kMaxPooledBuffers = 4096;
} // namespace

struct Connection::Impl {
    // ---- send side ----------------------------------------------------------------------------
    struct TxMessage {
        u16 seq = 0;
        u8 sliceIndex = 0;
        u16 sliceCount = 1;
        bool sent = false;
        bool acked = false;
        f64 lastSent = 0.0;
        u32 sendCount = 0;
        std::vector<u8> data;
    };
    struct ReliableTx {
        u16 nextSeq = 0;
        std::deque<TxMessage> window; // front = oldest unacked
        usize firstUnsent = 0;        // window[firstUnsent..] never sent
        u32 inFlight = 0;             // sent and unacked
        usize unsentBytes = 0;
        usize windowSentBytes = 0;    // payload of window[0..firstUnsent): sent, not yet popped
        f64 earliestResend = std::numeric_limits<f64>::infinity();
    };
    struct QueuedMessage {
        u16 seq = 0;
        u64 notifySeq = 0;
        f64 queuedAt = 0.0;
        std::vector<u8> data;
    };
    struct InputMessage {
        u16 seq = 0;
        bool everSent = false;
        std::vector<u8> data;
    };
    struct MsgRef {
        u8 channel;
        u16 seq;
    };
    enum class PacketState : u8 { Pending, Delivered, Lost };
    struct SentPacket {
        bool valid = false;
        bool ackEliciting = false;
        bool urgent = false; // carried reliable messages: the peer acks it within its maxAckDelay
        bool hasInput = false;
        PacketState state = PacketState::Pending;
        u16 seq = 0;
        u16 maxInputSeq = 0;
        f64 sendTime = 0.0;
        std::vector<MsgRef> reliable;
        std::vector<u64> states;
    };

    // ---- receive side -------------------------------------------------------------------------
    struct RxPending {
        u8 sliceIndex = 0;
        u16 sliceCount = 1;
        std::vector<u8> data;
    };
    struct ReliableRx {
        u16 nextExpected = 0;
        u64 nextIndex = 0; // nextExpected without wrap-around (keys `pending`)
        // Out-of-order messages keyed by unwrapped sequence. An ordered map keeps every insert and
        // drain O(log n): a sorted vector cost O(n) per message, which a peer sending a window's
        // worth of messages in descending order turned into ~200 us of CPU per packet.
        std::map<u64, RxPending> pending;
        std::vector<u8> blob;           // BULK reassembly
        u16 blobSlices = 0;
        u16 blobNext = 0;
    };
    struct InboxEntry {
        Channel channel;
        usize offset;
        usize size;
    };

    ConnectionConfig cfg;
    std::string name;
    PacketSendFn sendFn = nullptr;
    void* sendContext = nullptr;
    detail::LibraryRef library;
    reliable_endpoint_t* endpoint = nullptr;
    f64 now = 0.0;
    f64 lastReliableUpdate = -1.0;

    std::array<ReliableTx, kChannelCount> rtx;
    std::array<std::deque<QueuedMessage>, kChannelCount> utx;
    std::array<u16, kChannelCount> sequencedNext{};
    std::deque<InputMessage> inputs;
    u16 nextInputSeq = 0;
    bool inputAckedValid = false;
    u16 inputAcked = 0;
    u64 nextStateSeq = 1;
    int priorityChannel = -1; // channel holding a due jumbo message that must lead the next packet

    std::vector<SentPacket> sent;
    // Ack-eliciting packets in send order, possibly resolved: [0] urgent, [1] unreliable-only.
    // Separate queues keep each one's loss deadline monotone in send order.
    std::array<std::deque<u16>, 2> pendingOrder;
    bool hasHighestAcked = false;
    u16 highestAcked = 0;
    SentPacket scratch;

    std::array<ReliableRx, kChannelCount> rrx;
    usize reorderBytes = 0; // out-of-order reliable payload buffered across channels
    std::array<bool, kChannelCount> hasLastSeq{};
    std::array<u16, kChannelCount> lastSeq{};
    std::vector<wire::MessageView> parsed;
    wire::ParseLimits limits;
    std::vector<InboxEntry> inbox;
    std::vector<u8> inboxBytes;
    std::vector<PacketNotify> notifies;

    bool ackPending = false;
    bool urgentAckPending = false;
    u32 unackedReceived = 0;
    f64 firstUnackedTime = 0.0;
    f64 firstUrgentTime = 0.0;

    TokenBucket bucket;
    TokenBucket packetRate; // maxPacketsPerSecond
    AimdController aimd;
    TokenBucket voiceBucket;
    TokenBucket inPackets;
    TokenBucket inBytes;      // per datagram: game + voice rates
    TokenBucket inGameBytes;  // per parsed packet: everything but VOICE
    TokenBucket inVoiceBytes; // per parsed packet: VOICE messages
    RttEstimator rtt;
    LossWindow lossWindow;
    u32 pacedRemaining = 0;
    f64 nextPacedTime = 0.0;
    f64 lastFlushTime = -1.0;
    f64 flushInterval = 0.0;

    ConnectionStats st;
    u32 strikes = 0;
    u64 seenInvalid = 0;
    u64 seenTooLarge = 0;
    u64 datagramsThisSend = 0;
    std::vector<u8> packetBuffer;
    std::vector<std::vector<u8>> bufferPool;

    // ------------------------------------------------------------------------------------------
    std::vector<u8> takeBuffer() {
        if (bufferPool.empty()) return {};
        std::vector<u8> b = std::move(bufferPool.back());
        bufferPool.pop_back();
        b.clear();
        return b;
    }
    void recycle(std::vector<u8>&& b) {
        if (bufferPool.size() < kMaxPooledBuffers && b.capacity() != 0 && b.capacity() <= 4096) {
            bufferPool.push_back(std::move(b));
        }
    }

    u32 maxPayload(Channel ch) const noexcept { return limits.maxPayload[channelId(ch)]; }
    u32 maxSendBytes(Channel ch) const noexcept {
        if (ch == Channel::Bulk) return cfg.maxBulkBlobBytes;
        return maxPayload(ch);
    }
    /// Unreliable-only packets feed RTT samples only when the peer acks them as promptly as
    /// reliable ones; otherwise their ack delay (a peer tick) would inflate srtt and the RTO.
    bool lazyAcksInRtt() const noexcept { return cfg.peerMaxLazyAckDelay <= cfg.peerMaxAckDelay; }

    /// Time threshold for declaring an unacked packet lost: 1.25 x srtt plus the peer's ack
    /// delay (04 §2.2), widened to srtt + 4 rttvar when RTT samples vary (a lost ack-only packet
    /// delays the next ack by a peer tick; that must not turn a delivered packet into a false loss).
    /// Unreliable-only packets whose ack delay is not part of the RTT samples get it added.
    f64 lossDelay(bool urgent) const noexcept {
        const bool delayInRtt = urgent || lazyAcksInRtt();
        const f64 ackDelay = delayInRtt ? cfg.peerMaxAckDelay : std::max(cfg.peerMaxAckDelay, cfg.peerMaxLazyAckDelay);
        if (!rtt.hasSample()) return rtt.rto() + ackDelay;
        const f64 base = cfg.lossTimeFactor * std::max(rtt.srtt(), rtt.latest()) + ackDelay;
        const f64 variance = rtt.srtt() + 4.0 * rtt.rttvar() + (delayInRtt ? 0.0 : ackDelay);
        return std::max({base, variance, cfg.rtoMin});
    }
    f64 budgetCapacity(f64 bps) const noexcept {
        // Deep enough for one full packet even at the smallest budget.
        return std::max(bps / 8.0 * cfg.burstSeconds, static_cast<f64>(wire::kNetcodeMaxPayload + cfg.wireOverheadBytes));
    }
    void applyBudget(u32 bps) {
        bucket.setRate(static_cast<f64>(bps) / 8.0, budgetCapacity(static_cast<f64>(bps)));
    }

    // ---- reliable callbacks ----------------------------------------------------------------
    static void transmitThunk(void* context, uint64_t, uint16_t, uint8_t* data, int bytes) {
        auto* self = static_cast<Impl*>(context);
        ++self->datagramsThisSend;
        self->sendFn(self->sendContext, std::span<const u8>(data, static_cast<usize>(bytes)));
    }
    static int processThunk(void* context, uint64_t, uint16_t sequence, uint8_t* data, int bytes) {
        auto* self = static_cast<Impl*>(context);
        return self->onPacket(sequence, std::span<const u8>(data, static_cast<usize>(bytes))) ? 1 : 0;
    }

    // ---- receive path ------------------------------------------------------------------------
    void strike(std::string_view why) {
        ++strikes;
        ++st.malformedPackets;
        HELIOS_LOG_DEBUG(LogNet, "[{}] malformed packet ({}), strike {}", name, why, strikes);
    }

    void pushInbox(Channel ch, std::span<const u8> payload) {
        const usize offset = inboxBytes.size();
        inboxBytes.insert(inboxBytes.end(), payload.begin(), payload.end());
        inbox.push_back(InboxEntry{ch, offset, payload.size()});
        ChannelStats& cs = st.channels[channelId(ch)];
        ++cs.messagesReceived;
        cs.bytesReceived += payload.size();
    }

    void resetBlob(ReliableRx& rx) {
        rx.blob.clear();
        rx.blobSlices = 0;
        rx.blobNext = 0;
    }

    void deliverReliable(Channel ch, u8 sliceIndex, u16 sliceCount, std::span<const u8> payload) {
        if (ch != Channel::Bulk) {
            pushInbox(ch, payload);
            return;
        }
        ReliableRx& rx = rrx[channelId(ch)];
        if (rx.blobSlices == 0) {
            if (sliceIndex != 0) {
                strike("bulk slice out of order");
                resetBlob(rx);
                return;
            }
            rx.blobSlices = sliceCount;
            rx.blobNext = 0;
            rx.blob.clear();
        }
        if (sliceIndex != rx.blobNext || sliceCount != rx.blobSlices) {
            strike("bulk slice mismatch");
            resetBlob(rx);
            return;
        }
        if (rx.blob.size() + payload.size() > cfg.maxBulkBlobBytes) {
            strike("bulk blob too large");
            resetBlob(rx);
            return;
        }
        rx.blob.insert(rx.blob.end(), payload.begin(), payload.end());
        ++rx.blobNext;
        if (rx.blobNext == rx.blobSlices) {
            pushInbox(ch, rx.blob);
            resetBlob(rx);
        }
    }

    static bool isPending(const ReliableRx& rx, u16 seq) {
        const i32 d = seqDiff(seq, rx.nextExpected);
        return d > 0 && rx.pending.count(rx.nextIndex + static_cast<u64>(d)) != 0;
    }

    void advanceExpected(ReliableRx& rx) {
        ++rx.nextExpected;
        ++rx.nextIndex;
    }

    void receiveReliable(const wire::MessageView& v) {
        const u8 id = channelId(v.channel);
        ReliableRx& rx = rrx[id];
        const i32 d = seqDiff(v.seq, rx.nextExpected);
        if (d < 0) {
            ++st.channels[id].duplicates;
            return;
        }
        if (d == 0) {
            deliverReliable(v.channel, v.sliceIndex, v.sliceCount, v.payload);
            advanceExpected(rx);
            // Drain buffered successors.
            while (!rx.pending.empty() && rx.pending.begin()->first == rx.nextIndex) {
                auto node = rx.pending.extract(rx.pending.begin());
                RxPending& p = node.mapped();
                reorderBytes -= std::min(reorderBytes, p.data.size());
                deliverReliable(v.channel, p.sliceIndex, p.sliceCount, p.data);
                recycle(std::move(p.data));
                advanceExpected(rx);
            }
            return;
        }
        // Out of order: keep a copy until the gap fills.
        auto [it, inserted] = rx.pending.try_emplace(rx.nextIndex + static_cast<u64>(d));
        if (!inserted) {
            ++st.channels[id].duplicates;
            return;
        }
        RxPending& p = it->second;
        p.sliceIndex = v.sliceIndex;
        p.sliceCount = v.sliceCount;
        p.data = takeBuffer();
        p.data.assign(v.payload.begin(), v.payload.end());
        reorderBytes += p.data.size();
    }

    bool onPacket(u16 sequence, std::span<const u8> data) {
        (void)sequence;
        const wire::ParseError err = wire::parsePacket(data, limits, parsed);
        if (err != wire::ParseError::None) {
            strike(wire::parseErrorName(err));
            return false;
        }
        // Semantic pre-check before anything is delivered: reliable sequences must lie inside the
        // window the sender can have in flight.
        bool ackEliciting = false;
        bool urgent = false;
        usize reorderAdd = 0;
        for (const wire::MessageView& v : parsed) {
            if (v.channel != Channel::Voice) ackEliciting = true;
            if (isReliable(v.channel)) {
                urgent = true;
                const i32 d = seqDiff(v.seq, rrx[channelId(v.channel)].nextExpected);
                if (d >= static_cast<i32>(cfg.maxReliableInFlight)) {
                    strike("reliable sequence outside window");
                    return false;
                }
                if (d > 0 && !isPending(rrx[channelId(v.channel)], v.seq)) reorderAdd += v.payload.size();
            }
        }
        // A sender's window span per channel is bounded (maxWindowBytes), so a legitimate peer can
        // never make us buffer more than maxReorderBytes out of order. Beyond it the peer is
        // withholding a sequence to pin our memory: refuse the packet (unacked) and strike.
        if (reorderBytes + reorderAdd > cfg.maxReorderBytes) {
            ++st.reorderOverflows;
            strike("reorder buffer overflow");
            return false;
        }
        if (!admitBytes(data.size())) {
            ++st.inboundRateLimited;
            return false; // unacked: reliable content is resent later
        }
        for (const wire::MessageView& v : parsed) {
            const u8 id = channelId(v.channel);
            switch (channelInfo(v.channel).reliability) {
            case Reliability::ReliableOrdered: receiveReliable(v); break;
            case Reliability::UnreliableRedundant:
            case Reliability::UnreliableSequenced:
                if (!hasLastSeq[id] || seqGreater(v.seq, lastSeq[id])) {
                    hasLastSeq[id] = true;
                    lastSeq[id] = v.seq;
                    pushInbox(v.channel, v.payload);
                } else {
                    ++st.channels[id].dropped;
                }
                break;
            case Reliability::Unreliable: pushInbox(v.channel, v.payload); break;
            }
        }
        ++st.packetsReceived;
        if (ackEliciting) {
            if (!ackPending) firstUnackedTime = now;
            ackPending = true;
            ++unackedReceived;
            if (urgent && !urgentAckPending) {
                urgentAckPending = true;
                firstUrgentTime = now;
            }
        }
        return true;
    }

    /// Charges a parsed packet to the game and voice buckets (04 §9). All or nothing.
    bool admitBytes(usize packetBytes) {
        if (cfg.inboundBytesPerSecond <= 0.0) return true;
        usize voice = 0;
        for (const wire::MessageView& v : parsed) {
            if (v.channel == Channel::Voice) voice += wire::encodedSize(v.channel, static_cast<u32>(v.payload.size()));
        }
        const f64 game = static_cast<f64>(packetBytes - std::min(packetBytes, voice));
        inGameBytes.refill(now);
        inVoiceBytes.refill(now);
        if (inGameBytes.tokens() < game || inVoiceBytes.tokens() < static_cast<f64>(voice)) return false;
        inGameBytes.consume(game);
        inVoiceBytes.consume(static_cast<f64>(voice));
        return true;
    }

    /// An ack-only packet is due: reliable data waited maxAckDelay, unreliable-only data waited
    /// maxLazyAckDelay, or reliable's 33-packet ack window is about to slide past unacked packets.
    bool ackOnlyDue() const noexcept {
        if (!ackPending) return false;
        return unackedReceived >= cfg.ackEveryPackets ||
               (urgentAckPending && now - firstUrgentTime >= cfg.maxAckDelay) ||
               now - firstUnackedTime >= cfg.maxLazyAckDelay;
    }

    // ---- ack / loss handling ------------------------------------------------------------------
    void markReliableAcked(u8 id, u16 seq) {
        ReliableTx& tx = rtx[id];
        if (tx.window.empty()) return;
        const u16 d = static_cast<u16>(seq - tx.window.front().seq);
        if (d >= tx.window.size()) return;
        TxMessage& msg = tx.window[d];
        if (msg.acked) return;
        msg.acked = true;
        if (msg.sent && tx.inFlight > 0) --tx.inFlight;
        while (!tx.window.empty() && tx.window.front().acked) {
            tx.windowSentBytes -= std::min(tx.windowSentBytes, tx.window.front().data.size());
            recycle(std::move(tx.window.front().data));
            tx.window.pop_front();
            if (tx.firstUnsent > 0) --tx.firstUnsent;
        }
    }

    void notifyStates(SentPacket& p, bool delivered) {
        for (u64 s : p.states) notifies.push_back(PacketNotify{s, delivered});
        if (delivered) st.stateDelivered += p.states.size();
        else st.stateLost += p.states.size();
        p.states.clear();
    }

    void declareLost(SentPacket& p) {
        p.state = PacketState::Lost;
        ++st.packetsLost;
        lossWindow.record(true, now);
        notifyStates(p, false);
        // Reliable messages from a lost packet are resent at the next opportunity.
        for (const MsgRef& r : p.reliable) {
            ReliableTx& tx = rtx[r.channel];
            if (tx.window.empty()) continue;
            const u16 d = static_cast<u16>(r.seq - tx.window.front().seq);
            if (d >= tx.window.size()) continue;
            TxMessage& msg = tx.window[d];
            if (!msg.acked && msg.sent) {
                msg.lastSent = -std::numeric_limits<f64>::infinity();
                tx.earliestResend = std::min(tx.earliestResend, now);
            }
        }
    }

    void onAck(u16 seq) {
        SentPacket& p = sent[seq % sent.size()];
        if (!p.valid || p.seq != seq) return;
        if (p.state == PacketState::Pending) {
            p.state = PacketState::Delivered;
            // Only ack-eliciting packets give RTT samples: the peer acks those within its ack
            // delay, while an ack-only packet may wait for the peer's next data packet.
            if (p.ackEliciting) {
                if (p.urgent || lazyAcksInRtt()) rtt.addSample(now - p.sendTime, now);
                ++st.packetsAcked;
                lossWindow.record(false, now);
            }
            notifyStates(p, true);
        } else if (p.state == PacketState::Lost) {
            ++st.spuriousLosses;
        }
        for (const MsgRef& r : p.reliable) markReliableAcked(r.channel, r.seq);
        p.reliable.clear();
        if (p.hasInput) {
            if (!inputAckedValid || seqGreater(p.maxInputSeq, inputAcked)) {
                inputAcked = p.maxInputSeq;
                inputAckedValid = true;
            }
            while (!inputs.empty() && !seqGreater(inputs.front().seq, inputAcked)) {
                recycle(std::move(inputs.front().data));
                inputs.pop_front();
            }
        }
        if (p.ackEliciting && (!hasHighestAcked || seqGreater(seq, highestAcked))) {
            highestAcked = seq;
            hasHighestAcked = true;
        }
    }

    void detectLoss() {
        for (usize q = 0; q < pendingOrder.size(); ++q) {
            std::deque<u16>& order = pendingOrder[q];
            const f64 delay = lossDelay(/*urgent=*/q == 0);
            while (!order.empty()) {
                const u16 seq = order.front();
                SentPacket& p = sent[seq % sent.size()];
                if (!p.valid || p.seq != seq || p.state != PacketState::Pending) {
                    order.pop_front();
                    continue;
                }
                const bool byReorder =
                    hasHighestAcked && seqDiff(highestAcked, seq) >= static_cast<i32>(cfg.lossReorderThreshold);
                const bool byTime = now - p.sendTime >= delay;
                if (!byReorder && !byTime) break; // both criteria are monotone in send order
                declareLost(p);
                order.pop_front();
            }
        }
    }

    void processAcks() {
        int count = 0;
        const uint16_t* acks = reliable_endpoint_get_acks(endpoint, &count);
        for (int i = 0; i < count; ++i) onAck(acks[i]);
        reliable_endpoint_clear_acks(endpoint);
        if (count > 0) detectLoss();
    }

    // ---- send path -----------------------------------------------------------------------------
    /// Flow control: may the next unsent message of `tx` go out? (BULK slice window; byte span
    /// of the window, which bounds what the receiver may have to buffer out of order.)
    bool windowOpen(Channel ch, const ReliableTx& tx) const {
        if (ch == Channel::Bulk && tx.inFlight >= cfg.bulkWindow) return false;
        const usize next = tx.window[tx.firstUnsent].data.size();
        return tx.firstUnsent == 0 || tx.windowSentBytes + next <= cfg.maxWindowBytes;
    }

    bool hasReliableWork(u8 id, bool includeResends) const {
        const ReliableTx& tx = rtx[id];
        if (tx.firstUnsent < tx.window.size() && windowOpen(static_cast<Channel>(id), tx)) return true;
        return includeResends && tx.firstUnsent > 0 && now >= tx.earliestResend;
    }

    bool hasSendable(bool firstPacket) const {
        for (Channel ch : kPackOrder) {
            const u8 id = channelId(ch);
            switch (channelInfo(ch).reliability) {
            case Reliability::ReliableOrdered:
                if (hasReliableWork(id, true)) return true;
                break;
            case Reliability::UnreliableRedundant:
                for (const InputMessage& msg : inputs) {
                    if (!msg.everSent || firstPacket) return true;
                }
                break;
            default:
                if (!utx[id].empty()) return true;
                break;
            }
        }
        return false;
    }

    /// Writes a message of `ch` into packetBuffer at `used` if it fits in `cap`.
    bool put(usize& used, usize cap, Channel ch, u16 seq, u8 sliceIndex, u16 sliceCount, std::span<const u8> payload) {
        const usize n = wire::writeMessage(std::span<u8>(packetBuffer.data() + used, cap - used), ch, seq, sliceIndex,
                                           sliceCount, payload);
        if (n == 0) return false;
        used += n;
        return true;
    }

    enum class PackResult : u8 { Continue, Jumbo };

    PackResult packReliable(Channel ch, usize& used, usize cap, SentPacket& rec) {
        const u8 id = channelId(ch);
        ReliableTx& tx = rtx[id];
        if (tx.window.empty()) return PackResult::Continue;
        const f64 rto = rtt.rto();
        const bool jumboCapable = channelInfo(ch).jumbo;
        // Resends first (oldest first), then new messages in order.
        if (tx.firstUnsent > 0 && now >= tx.earliestResend) {
            f64 earliest = std::numeric_limits<f64>::infinity();
            for (usize i = 0; i < tx.firstUnsent; ++i) {
                TxMessage& msg = tx.window[i];
                if (msg.acked) continue;
                const f64 due = msg.lastSent + rto;
                if (now < due) {
                    earliest = std::min(earliest, due);
                    continue;
                }
                const usize size = wire::encodedSize(ch, static_cast<u32>(msg.data.size()));
                if (size > wire::kMaxPacketPayload && jumboCapable) {
                    if (used == 0) {
                        writeJumbo(ch, msg, used, rec);
                        tx.earliestResend = now; // later messages were not examined: rescan next time
                        return PackResult::Jumbo;
                    }
                    priorityChannel = id;
                    earliest = std::min(earliest, now);
                    continue;
                }
                if (size > cap - used) {
                    earliest = std::min(earliest, now); // still due
                    continue;
                }
                put(used, cap, ch, msg.seq, msg.sliceIndex, msg.sliceCount, msg.data);
                rec.reliable.push_back(MsgRef{id, msg.seq});
                msg.lastSent = now;
                ++msg.sendCount;
                ++st.channels[id].resends;
                earliest = std::min(earliest, now + rto);
            }
            tx.earliestResend = earliest;
        }
        while (tx.firstUnsent < tx.window.size()) {
            if (!windowOpen(ch, tx)) break;
            TxMessage& msg = tx.window[tx.firstUnsent];
            const usize size = wire::encodedSize(ch, static_cast<u32>(msg.data.size()));
            if (size > cap - used) {
                if (size > wire::kMaxPacketPayload && jumboCapable) {
                    if (used == 0) {
                        writeJumbo(ch, msg, used, rec);
                        markSent(tx, msg);
                        return PackResult::Jumbo;
                    }
                    priorityChannel = id;
                }
                break; // keep send order within the channel
            }
            put(used, cap, ch, msg.seq, msg.sliceIndex, msg.sliceCount, msg.data);
            rec.reliable.push_back(MsgRef{id, msg.seq});
            markSent(tx, msg);
        }
        return PackResult::Continue;
    }

    void markSent(ReliableTx& tx, TxMessage& msg) {
        msg.sent = true;
        msg.lastSent = now;
        msg.sendCount = 1;
        ++tx.inFlight;
        ++tx.firstUnsent;
        tx.unsentBytes -= std::min(tx.unsentBytes, msg.data.size());
        tx.windowSentBytes += msg.data.size();
        tx.earliestResend = std::min(tx.earliestResend, now + rtt.rto());
    }

    void writeJumbo(Channel ch, TxMessage& msg, usize& used, SentPacket& rec) {
        const usize size = wire::encodedSize(ch, static_cast<u32>(msg.data.size()));
        if (packetBuffer.size() < size) packetBuffer.resize(size);
        const usize n = wire::writeMessage(std::span<u8>(packetBuffer.data(), size), ch, msg.seq, msg.sliceIndex,
                                           msg.sliceCount, msg.data);
        HELIOS_ASSERT(n == size);
        used = n;
        rec.reliable.push_back(MsgRef{channelId(ch), msg.seq});
        if (msg.sent) {
            msg.lastSent = now;
            ++msg.sendCount;
            ++st.channels[channelId(ch)].resends;
        }
        if (priorityChannel == channelId(ch)) priorityChannel = -1;
    }

    void packInputs(usize& used, usize cap, SentPacket& rec) {
        for (InputMessage& msg : inputs) {
            if (inputAckedValid && !seqGreater(msg.seq, inputAcked)) continue;
            if (!put(used, cap, Channel::Input, msg.seq, 0, 1, msg.data)) break;
            if (msg.everSent) ++st.channels[channelId(Channel::Input)].resends;
            msg.everSent = true;
            if (!rec.hasInput || seqGreater(msg.seq, rec.maxInputSeq)) rec.maxInputSeq = msg.seq;
            rec.hasInput = true;
        }
    }

    usize packQueue(Channel ch, usize& used, usize cap, SentPacket& rec) {
        const u8 id = channelId(ch);
        auto& q = utx[id];
        usize bytes = 0;
        while (!q.empty()) {
            QueuedMessage& msg = q.front();
            if (!put(used, cap, ch, msg.seq, 0, 1, msg.data)) break;
            bytes += wire::encodedSize(ch, static_cast<u32>(msg.data.size()));
            if (ch == Channel::State) rec.states.push_back(msg.notifySeq);
            recycle(std::move(msg.data));
            q.pop_front();
        }
        return bytes;
    }

    /// Builds and sends one packet from queued data. Returns false if nothing was queued.
    bool buildPacket(bool includeVoice) {
        SentPacket& rec = scratch;
        rec.reliable.clear();
        rec.states.clear();
        rec.hasInput = false;
        rec.maxInputSeq = 0;
        usize used = 0;
        const usize cap = wire::kMaxPacketPayload;
        if (packetBuffer.size() < cap) packetBuffer.resize(cap);
        if (priorityChannel >= 0) {
            const Channel ch = static_cast<Channel>(priorityChannel);
            priorityChannel = -1;
            if (packReliable(ch, used, cap, rec) == PackResult::Jumbo) return sendPacket(used, rec, true, 0);
        }
        for (Channel ch : kPackOrder) {
            if (ch == Channel::Debug && !cfg.debugChannel) continue;
            switch (channelInfo(ch).reliability) {
            case Reliability::ReliableOrdered:
                if (packReliable(ch, used, cap, rec) == PackResult::Jumbo) return sendPacket(used, rec, true, 0);
                break;
            case Reliability::UnreliableRedundant: packInputs(used, cap, rec); break;
            default: packQueue(ch, used, cap, rec); break;
            }
        }
        const bool hasGame = used > 0;
        usize voiceBytes = 0;
        if (includeVoice) voiceBytes = packQueue(Channel::Voice, used, cap, rec);
        if (used == 0) return false;
        return sendPacket(used, rec, hasGame, voiceBytes);
    }

    bool sendVoiceOnly() {
        SentPacket& rec = scratch;
        rec.reliable.clear();
        rec.states.clear();
        rec.hasInput = false;
        usize used = 0;
        if (packetBuffer.size() < wire::kMaxPacketPayload) packetBuffer.resize(wire::kMaxPacketPayload);
        const usize voiceBytes = packQueue(Channel::Voice, used, wire::kMaxPacketPayload, rec);
        if (used == 0) return false;
        ++st.voicePackets;
        return sendPacket(used, rec, false, voiceBytes);
    }

    void sendAckOnly() {
        SentPacket& rec = scratch;
        rec.reliable.clear();
        rec.states.clear();
        rec.hasInput = false;
        if (packetBuffer.empty()) packetBuffer.resize(wire::kMaxPacketPayload);
        packetBuffer[0] = wire::kPadByte;
        ++st.ackOnlyPackets;
        sendPacket(1, rec, false, 0);
    }

    bool sendPacket(usize used, SentPacket& rec, bool ackEliciting, usize voiceBytes) {
        const u16 seq = reliable_endpoint_next_packet_sequence(endpoint);
        SentPacket& slot = sent[seq % sent.size()];
        if (slot.valid && slot.state == PacketState::Pending && slot.ackEliciting) declareLost(slot);
        slot.valid = true;
        slot.seq = seq;
        slot.sendTime = now;
        slot.state = PacketState::Pending;
        slot.ackEliciting = ackEliciting;
        slot.urgent = !rec.reliable.empty();
        slot.hasInput = rec.hasInput;
        slot.maxInputSeq = rec.maxInputSeq;
        std::swap(slot.reliable, rec.reliable);
        std::swap(slot.states, rec.states);
        if (ackEliciting) pendingOrder[slot.urgent ? 0 : 1].push_back(seq);

        datagramsThisSend = 0;
        reliable_endpoint_send_packet(endpoint, packetBuffer.data(), static_cast<int>(used));
        const u64 datagrams = datagramsThisSend;
        if (datagrams > 1) {
            ++st.jumboPackets;
            st.fragmentsSent += datagrams;
        }
        ++st.packetsSent;
        st.bytesSent += used;
        // Wire cost: payload + reliable header + per-datagram fragment header and UDP/netcode overhead.
        const f64 wireBytes = static_cast<f64>(used + wire::kReliableMaxHeader) +
                              static_cast<f64>(datagrams) * static_cast<f64>(cfg.wireOverheadBytes + (datagrams > 1 ? 5 : 0));
        st.wireBytesSent += static_cast<u64>(wireBytes);
        // VOICE rides in its own budget (charged at send()).
        bucket.consume(std::max(0.0, wireBytes - static_cast<f64>(voiceBytes)));
        ackPending = false;
        urgentAckPending = false;
        unackedReceived = 0;
        return true;
    }

    void expireQueued() {
        const f64 cutoff = now - cfg.unreliableMaxAge;
        for (Channel ch : {Channel::State, Channel::EventUnreliable, Channel::Latest, Channel::Voice}) {
            auto& q = utx[channelId(ch)];
            while (!q.empty() && q.front().queuedAt < cutoff) {
                if (ch == Channel::State) {
                    notifies.push_back(PacketNotify{q.front().notifySeq, false});
                    ++st.stateLost;
                }
                ++st.channels[channelId(ch)].dropped;
                recycle(std::move(q.front().data));
                q.pop_front();
            }
        }
    }

    void advance(f64 t) {
        now = std::max(now, t);
        bucket.refill(now);
        voiceBucket.refill(now);
        if (now - lastReliableUpdate >= kReliableStatsInterval) {
            reliable_endpoint_update(endpoint, now);
            lastReliableUpdate = now;
        }
    }

    /// Timers, AIMD, paced packets and VOICE. Ack-only packets only when `sendAcks` (flush()
    /// passes false: its own packets carry the acks).
    void service(f64 t, bool sendAcks) {
        advance(t);
        detectLoss();
        expireQueued();
        if (cfg.aimd) {
            aimd.update(now, lossWindow.lossRate(now), lossWindow.samples(now), rtt);
            if (static_cast<f64>(aimd.budgetBps()) / 8.0 != bucket.rate()) applyBudget(aimd.budgetBps());
        }
        // Paced follow-up packets of the last flush.
        while (pacedRemaining > 0 && now >= nextPacedTime && bucket.tokens() > 0.0 && packetRateAllows() &&
               hasSendable(false)) {
            if (!buildPacket(true)) break;
            chargePacketRate();
            --pacedRemaining;
            nextPacedTime += cfg.pacingInterval;
        }
        if (pacedRemaining > 0 && !hasSendable(false)) pacedRemaining = 0;
        // VOICE is not tick-packed: send now unless a flush is expected within the coalescing window.
        if (!utx[channelId(Channel::Voice)].empty()) {
            const bool flushSoon = lastFlushTime >= 0.0 && flushInterval > 0.0 &&
                                   (lastFlushTime + flushInterval) - now <= cfg.voiceCoalesce;
            if (!flushSoon) {
                while (sendVoiceOnly()) {
                }
            }
        }
        if (sendAcks && ackOnlyDue()) sendAckOnly();
    }

    bool packetRateAllows() noexcept {
        if (cfg.maxPacketsPerSecond <= 0.0) return true;
        packetRate.refill(now);
        return packetRate.tokens() >= 1.0;
    }
    void chargePacketRate() noexcept {
        if (cfg.maxPacketsPerSecond > 0.0) packetRate.consume(1.0);
    }
};

// ---------------------------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------------------------

Result<std::unique_ptr<Connection>> Connection::create(const ConnectionConfig& config, PacketSendFn send, void* context,
                                                       f64 now, std::string_view name) {
    if (send == nullptr) return Error{ErrorCode::InvalidArgument, "Connection: send function required"};
    if (config.sentPacketsBufferSize == 0 || config.receivedPacketsBufferSize == 0 || config.fragmentSize == 0 ||
        config.maxReliableInFlight == 0 || config.maxReliableInFlight > 16384 || config.bulkSliceBytes == 0 ||
        config.bulkWindow == 0 || config.inputRedundancy == 0) {
        return Error{ErrorCode::InvalidArgument, "Connection: invalid config"};
    }
    // Four reliable channels, each with at most maxWindowBytes plus one message in flight.
    if (config.maxReorderBytes < 4 * (config.maxWindowBytes + config.maxJumboMessageBytes)) {
        return Error{ErrorCode::InvalidArgument, "Connection: maxReorderBytes below what a peer's windows allow"};
    }
    if (config.bulkSliceBytes > wire::maxPayloadFor(Channel::Bulk, wire::kMaxPacketPayload)) {
        return Error{ErrorCode::InvalidArgument, "Connection: BULK slices must fit one packet"};
    }
    if (static_cast<u64>(config.maxBulkBlobBytes) > static_cast<u64>(config.bulkSliceBytes) * wire::kMaxSlicesPerBlob) {
        return Error{ErrorCode::InvalidArgument, "Connection: BULK blob exceeds 256 slices"};
    }
    auto impl = std::make_unique<Impl>();
    Impl& m = *impl;
    m.cfg = config;
    m.name = std::string(name);
    m.sendFn = send;
    m.sendContext = context;
    m.now = now;

    // Parse limits mirror the send limits.
    for (u32 i = 0; i < kChannelCount; ++i) {
        const Channel ch = static_cast<Channel>(i);
        if (channelInfo(ch).jumbo) m.limits.maxPayload[i] = config.maxJumboMessageBytes;
        else if (ch == Channel::Bulk) m.limits.maxPayload[i] = config.bulkSliceBytes;
        else m.limits.maxPayload[i] = wire::maxPayloadFor(ch, wire::kMaxPacketPayload);
    }
    m.limits.allowDebug = config.debugChannel;

    reliable_config_t rc;
    reliable_default_config(&rc);
    std::memset(rc.name, 0, sizeof(rc.name));
    std::memcpy(rc.name, m.name.data(), std::min(m.name.size(), sizeof(rc.name) - 1));
    rc.context = &m;
    rc.id = 0;
    const u32 maxPacket = std::max<u32>(static_cast<u32>(wire::kMaxPacketPayload), config.maxJumboMessageBytes + kJumboSlack);
    rc.max_packet_size = static_cast<int>(maxPacket);
    rc.fragment_above = static_cast<int>(wire::kMaxPacketPayload);
    rc.fragment_size = static_cast<int>(config.fragmentSize);
    rc.max_fragments = static_cast<int>((maxPacket + config.fragmentSize - 1) / config.fragmentSize);
    rc.ack_buffer_size = 256;
    rc.sent_packets_buffer_size = static_cast<int>(config.sentPacketsBufferSize);
    rc.received_packets_buffer_size = static_cast<int>(config.receivedPacketsBufferSize);
    rc.fragment_reassembly_buffer_size = static_cast<int>(std::max<u32>(1, config.fragmentReassemblyBufferSize));
    rc.rtt_history_size = static_cast<int>(std::max<u32>(1, config.rttHistorySize));
    rc.packet_header_size = static_cast<int>(config.wireOverheadBytes);
    rc.transmit_packet_function = &Impl::transmitThunk;
    rc.process_packet_function = &Impl::processThunk;
    rc.allocator_context = nullptr;
    rc.allocate_function = &detail::netAllocate;
    rc.free_function = &detail::netFree;
    if (rc.max_fragments > 256 || static_cast<usize>(config.fragmentSize) + 14 > wire::kNetcodeMaxPayload) {
        return Error{ErrorCode::InvalidArgument, "Connection: fragment configuration exceeds reliable limits"};
    }
    m.endpoint = reliable_endpoint_create(&rc, now);
    if (m.endpoint == nullptr) return Error{ErrorCode::InvalidArgument, "Connection: reliable_endpoint_create failed"};
    m.lastReliableUpdate = now;

    m.sent.resize(config.sentPacketsBufferSize);
    m.packetBuffer.resize(wire::kMaxPacketPayload);
    RttEstimator::Config rttCfg;
    rttCfg.initialRto = config.initialRto;
    rttCfg.minRto = config.rtoMin;
    rttCfg.maxRto = config.rtoMax;
    m.rtt = RttEstimator(rttCfg);
    AimdController::Config ac = config.aimdConfig;
    ac.initialBps = config.budgetBps;
    ac.minBps = std::min(config.minBudgetBps, config.budgetBps);
    ac.maxBps = std::max(config.maxBudgetBps, config.budgetBps);
    m.aimd = AimdController(ac);
    const f64 bps = static_cast<f64>(config.budgetBps);
    m.bucket = TokenBucket(bps / 8.0, m.budgetCapacity(bps), now);
    m.voiceBucket = TokenBucket(static_cast<f64>(config.voiceBudgetBps) / 8.0,
                                std::max(static_cast<f64>(config.voiceBudgetBps) / 8.0 * 0.25, 2.0 * 1200.0), now);
    m.packetRate = TokenBucket(config.maxPacketsPerSecond, 2.0, now);
    m.inPackets = TokenBucket(config.inboundPacketsPerSecond, std::max(1.0, config.inboundPacketBurst), now);
    m.inBytes = TokenBucket(config.inboundBytesPerSecond + config.inboundVoiceBytesPerSecond,
                            std::max(1300.0, config.inboundByteBurst + config.inboundVoiceByteBurst), now);
    m.inGameBytes = TokenBucket(config.inboundBytesPerSecond, std::max(1300.0, config.inboundByteBurst), now);
    m.inVoiceBytes = TokenBucket(config.inboundVoiceBytesPerSecond, std::max(1300.0, config.inboundVoiceByteBurst), now);
    return std::unique_ptr<Connection>(new Connection(std::move(impl)));
}

Connection::Connection(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Connection::~Connection() {
    if (m && m->endpoint) reliable_endpoint_destroy(m->endpoint);
}

SendResult Connection::send(Channel channel, std::span<const u8> payload) {
    Impl& s = *m;
    const u8 id = channelId(channel);
    if (!isValidChannelId(id)) return SendResult::Unsupported;
    if (channel == Channel::Debug && !s.cfg.debugChannel) return SendResult::Unsupported;
    ChannelStats& cs = s.st.channels[id];
    if (payload.size() > s.maxSendBytes(channel)) return SendResult::TooLarge;
    switch (channelInfo(channel).reliability) {
    case Reliability::ReliableOrdered: {
        Impl::ReliableTx& tx = s.rtx[id];
        const usize sliceBytes = s.cfg.bulkSliceBytes;
        const usize slices = channel == Channel::Bulk ? std::max<usize>(1, (payload.size() + sliceBytes - 1) / sliceBytes) : 1;
        if (tx.window.size() + slices > s.cfg.maxReliableInFlight) {
            ++cs.wouldBlock;
            return SendResult::WouldBlock;
        }
        for (usize i = 0; i < slices; ++i) {
            Impl::TxMessage msg;
            msg.seq = tx.nextSeq++;
            msg.sliceIndex = static_cast<u8>(i);
            msg.sliceCount = static_cast<u16>(slices);
            msg.data = s.takeBuffer();
            if (channel == Channel::Bulk) {
                const usize begin = i * sliceBytes;
                const usize end = std::min(payload.size(), begin + sliceBytes);
                msg.data.assign(payload.begin() + static_cast<std::ptrdiff_t>(begin),
                                payload.begin() + static_cast<std::ptrdiff_t>(end));
            } else {
                msg.data.assign(payload.begin(), payload.end());
            }
            tx.unsentBytes += msg.data.size();
            tx.window.push_back(std::move(msg));
        }
        break;
    }
    case Reliability::UnreliableRedundant: {
        Impl::InputMessage msg;
        msg.seq = s.nextInputSeq++;
        msg.data = s.takeBuffer();
        msg.data.assign(payload.begin(), payload.end());
        s.inputs.push_back(std::move(msg));
        while (s.inputs.size() > s.cfg.inputRedundancy) {
            if (!s.inputs.front().everSent) ++cs.dropped;
            s.recycle(std::move(s.inputs.front().data));
            s.inputs.pop_front();
        }
        break;
    }
    case Reliability::UnreliableSequenced:
    case Reliability::Unreliable: {
        if (channel == Channel::Voice) {
            s.voiceBucket.refill(s.now);
            const f64 cost = static_cast<f64>(wire::encodedSize(channel, static_cast<u32>(payload.size())) +
                                              s.cfg.wireOverheadBytes);
            if (!s.voiceBucket.tryConsume(cost)) {
                ++s.st.voiceRateLimited;
                ++cs.dropped;
                return SendResult::RateLimited;
            }
        }
        Impl::QueuedMessage msg;
        if (channelInfo(channel).sequenced) msg.seq = s.sequencedNext[id]++;
        msg.queuedAt = s.now;
        msg.data = s.takeBuffer();
        msg.data.assign(payload.begin(), payload.end());
        if (channel == Channel::State) msg.notifySeq = s.nextStateSeq++;
        s.utx[id].push_back(std::move(msg));
        break;
    }
    }
    ++cs.messagesSent;
    cs.bytesSent += payload.size();
    return SendResult::Ok;
}

SendResult Connection::sendState(std::span<const u8> chunk, u64* outSeq) {
    const u64 seq = m->nextStateSeq;
    const SendResult r = send(Channel::State, chunk);
    if (r == SendResult::Ok && outSeq) *outSeq = seq;
    return r;
}

void Connection::receivePacket(std::span<const u8> packet, f64 now) {
    Impl& s = *m;
    if (packet.empty()) return;
    s.advance(now);
    if (s.cfg.inboundPacketsPerSecond > 0.0) {
        s.inPackets.refill(s.now);
        if (!s.inPackets.tryConsume(1.0)) {
            ++s.st.inboundRateLimited;
            return;
        }
    }
    if (s.cfg.inboundBytesPerSecond > 0.0) {
        s.inBytes.refill(s.now);
        if (!s.inBytes.tryConsume(static_cast<f64>(packet.size()))) {
            ++s.st.inboundRateLimited;
            return;
        }
    }
    s.st.bytesReceived += packet.size();
    // reliable never writes through this pointer; its API is just not const-correct.
    reliable_endpoint_receive_packet(s.endpoint, const_cast<u8*>(packet.data()), static_cast<int>(packet.size()));
    s.processAcks();
    // Garbage that only a malicious peer can produce (netcode already authenticated the bytes).
    const uint64_t* counters = reliable_endpoint_counters(s.endpoint);
    const u64 invalid = counters[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_INVALID];
    const u64 tooLarge = counters[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_TOO_LARGE_TO_RECEIVE];
    if (invalid != s.seenInvalid || tooLarge != s.seenTooLarge) {
        s.seenInvalid = invalid;
        s.seenTooLarge = tooLarge;
        s.strike("invalid reliable header");
    }
    // reliable acks the newest packet plus 32 before it: under a burst, acks must leave before
    // that window slides past packets we have not acked yet.
    if (s.ackPending && s.unackedReceived >= s.cfg.ackEveryPackets) s.sendAckOnly();
}

void Connection::update(f64 now) { m->service(now, /*sendAcks=*/true); }

void Connection::flush(f64 now) {
    Impl& s = *m;
    s.service(now, /*sendAcks=*/false);
    if (s.lastFlushTime >= 0.0) {
        const f64 dt = s.now - s.lastFlushTime;
        s.flushInterval = s.flushInterval <= 0.0 ? dt : 0.8 * s.flushInterval + 0.2 * dt;
    }
    s.lastFlushTime = s.now;
    s.pacedRemaining = 0;
    const bool rateOk = s.packetRateAllows();
    u32 built = 0;
    while (rateOk && built < s.cfg.maxPacketsPerFlush) {
        if (!s.hasSendable(built == 0)) break;
        if (s.bucket.tokens() <= 0.0) break;
        if (built >= 1 && (s.cfg.pacingInterval > 0.0 || !s.packetRateAllows())) {
            if (s.cfg.pacingInterval > 0.0) {
                s.pacedRemaining = s.cfg.maxPacketsPerFlush - built;
                s.nextPacedTime = s.now + s.cfg.pacingInterval;
            }
            break;
        }
        if (!s.buildPacket(true)) break;
        s.chargePacketRate();
        ++built;
    }
    // Leftover voice goes out on its own (own budget). Pending acks leave once per flush, in an
    // ack-only packet if no data packet carried them, unless the packet rate cap deferred this
    // flush: then they wait for the ack timers in update().
    while (s.sendVoiceOnly()) {
    }
    if (!rateOk) {
        ++s.st.packetRateDeferred;
        if (s.ackOnlyDue()) s.sendAckOnly();
    } else if (s.ackPending) {
        s.sendAckOnly();
        if (built == 0) s.chargePacketRate();
    }
}

usize Connection::inboxSize() const noexcept { return m->inbox.size(); }

Connection::ReceivedMessage Connection::inboxAt(usize index) const noexcept {
    const Impl::InboxEntry& e = m->inbox[index];
    return ReceivedMessage{e.channel, std::span<const u8>(m->inboxBytes.data() + e.offset, e.size)};
}

void Connection::clearInbox() noexcept {
    m->inbox.clear();
    m->inboxBytes.clear();
}

std::span<const PacketNotify> Connection::notifications() const noexcept { return m->notifies; }
void Connection::clearNotifications() noexcept { m->notifies.clear(); }

usize Connection::pendingReliableBytes() const noexcept {
    usize bytes = 0;
    for (const auto& tx : m->rtx) bytes += tx.unsentBytes;
    return bytes;
}

usize Connection::stateBudgetBytes(u32 tickHz) const noexcept {
    const f64 perTick = static_cast<f64>(budgetBps()) / 8.0 / static_cast<f64>(std::max<u32>(1, tickHz));
    const f64 backlog = static_cast<f64>(pendingReliableBytes());
    return perTick > backlog ? static_cast<usize>(perTick - backlog) : 0;
}

u32 Connection::budgetBps() const noexcept {
    return m->cfg.aimd ? m->aimd.budgetBps() : static_cast<u32>(m->bucket.rate() * 8.0);
}

void Connection::setBudgetBps(u32 bps) noexcept {
    m->aimd.setBudget(bps, m->now);
    m->applyBudget(m->cfg.aimd ? m->aimd.budgetBps() : bps);
}

void Connection::setMaxBudgetBps(u32 bps) noexcept {
    m->aimd.setMaxBps(bps);
    if (m->cfg.aimd) m->applyBudget(m->aimd.budgetBps());
}

u32 Connection::strikes() const noexcept { return m->strikes; }
bool Connection::isMalformed() const noexcept { return m->strikes >= m->cfg.malformedStrikeLimit; }

bool Connection::hasPendingSends() const noexcept { return m->ackPending || m->hasSendable(true); }

const ConnectionConfig& Connection::config() const noexcept { return m->cfg; }
f64 Connection::now() const noexcept { return m->now; }

ConnectionStats Connection::stats() const {
    const Impl& s = *m;
    ConnectionStats out = s.st;
    out.hasRtt = s.rtt.hasSample();
    out.rttMs = s.rtt.srtt() * 1000.0;
    out.rttVarMs = s.rtt.rttvar() * 1000.0;
    out.minRttMs = s.rtt.minRtt() * 1000.0;
    out.jitterMs = s.rtt.jitter() * 1000.0;
    out.rtoMs = s.rtt.rto() * 1000.0;
    out.lossPercent = s.lossWindow.lossRate(s.now) * 100.0;
    out.budgetBps = budgetBps();
    out.aimdDecreases = s.aimd.decreases();
    out.reliableRttMs = reliable_endpoint_rtt(s.endpoint);
    out.reliableLossPercent = reliable_endpoint_packet_loss(s.endpoint);
    reliable_endpoint_bandwidth(s.endpoint, &out.sentKbps, &out.receivedKbps, &out.ackedKbps);
    const uint64_t* c = reliable_endpoint_counters(s.endpoint);
    out.fragmentsReceived = c[RELIABLE_ENDPOINT_COUNTER_NUM_FRAGMENTS_RECEIVED];
    out.fragmentsInvalid = c[RELIABLE_ENDPOINT_COUNTER_NUM_FRAGMENTS_INVALID];
    out.packetsStale = c[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_STALE];
    out.packetsDuplicate = c[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_DUPLICATE];
    out.packetsInvalid = c[RELIABLE_ENDPOINT_COUNTER_NUM_PACKETS_INVALID];
    return out;
}

} // namespace helios::net
