#pragma once
// One HTP connection's L2+L3 state (04 §2): a vendored `reliable` endpoint (packet sequence,
// acks, fragmentation) plus Helios channels, message packing, per-message reliability, delivery
// notifications, the send budget with AIMD, inbound policing and RTT/loss/jitter statistics.
//
// A Connection knows nothing about netcode or sockets: it turns queued messages into packets
// handed to a PacketSendFn, and accepts the payload of every packet the peer sent. Server and
// Client (endpoint.h) own one per netcode session; tests can wire two Connections back to back.
//
//   send()/sendState()  queue a message (reliable ones copy into the send window)
//   receivePacket()     reliable header + fragment reassembly -> parse -> per-channel delivery
//   update(now)         loss timers, AIMD, paced packets, voice, delayed acks (call often)
//   flush(now)          once per tick: pack queued messages into up to maxPacketsPerFlush packets
//   forEachMessage()    delivered messages, in order, since clearInbox()
//   notifications()     STATE delivery notifications since clearNotifications()
//
// Reliability model: every packet has a reliable sequence; the packet records which reliable
// messages, INPUTs and STATE chunks it carried. An ack marks them delivered; a packet is declared
// lost when one >= 3 sequences newer is acked or after 1.25 x srtt (+ the peer's ack delay; at
// least srtt + 4 rttvar, so a late ack after a lost ack-only packet is not a false loss).
// Reliable messages are resent after RTO = srtt + 4 rttvar (30 ms .. 1 s), or at once when their
// packet is declared lost.
//
// Acks: a packet carrying reliable messages is acked within maxAckDelay; a packet carrying only
// unreliable data (INPUT, STATE, EVENT_U, LATEST) is acked by the next outgoing packet, at the
// latest after maxLazyAckDelay. A gateway therefore sends one packet per tick (04 §2.5) instead of
// an ack-only packet per client input. RTT samples come from packets whose ack delay is bounded by
// peerMaxAckDelay: always from packets with reliable messages, and from unreliable-only packets
// when peerMaxLazyAckDelay <= peerMaxAckDelay.
//
// Threading: single-threaded; owned by the endpoint's thread.

#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/net/channel.h"
#include "helios/net/congestion.h"

namespace helios::net {

/// Delivery report for one STATE chunk. `seq` is the value sendState() returned.
struct PacketNotify {
    u64 seq = 0;
    bool delivered = false;
};

struct ConnectionConfig {
    // --- reliable (L2) ---------------------------------------------------------------------
    u32 sentPacketsBufferSize = 256;      ///< Packets tracked for acks (trunk: 4,096).
    u32 receivedPacketsBufferSize = 256;
    u32 fragmentSize = 1024;
    /// Packet sequences with a fragment reassembly in progress. reliable allocates the whole
    /// packet (up to maxJumboMessageBytes) on a sequence's first fragment, so this bounds what a
    /// peer can pin with stray fragments (client links: 8 x 17 KB; trunks: 64 x 256 KB).
    u32 fragmentReassemblyBufferSize = 64;
    u32 rttHistorySize = 64;               ///< reliable's own stats (Helios keeps its own RTT).
    /// Largest message that may travel as one reliable-fragmented packet (CONTROL/DEBUG bursts,
    /// 04 §2.2: <= 16 KB on client links; trunks reassemble up to 256 KB).
    u32 maxJumboMessageBytes = 16 * 1024;

    // --- messages (L3) ----------------------------------------------------------------------
    u32 maxReliableInFlight = 1024;        ///< Queued + unacked per reliable channel.
    u32 bulkSliceBytes = 1024;
    u32 bulkWindow = 64;                   ///< BULK slices sent but not yet acked.
    u32 maxBulkBlobBytes = 256 * 1024;
    u32 inputRedundancy = 4;               ///< INPUT: last N inputs ride in every packet.
    /// Flow control per reliable channel: payload sent but not yet cumulatively acked (the span
    /// from the oldest unacked message). One message may exceed it (jumbo CONTROL).
    usize maxWindowBytes = 256 * 1024;
    /// Out-of-order reliable payload a receiver buffers. Must be >= 4 x (maxWindowBytes +
    /// maxJumboMessageBytes), the most a well-behaved peer can cause; packets beyond it are
    /// refused (unacked) and count as malformed, so withholding a sequence cannot pin memory.
    usize maxReorderBytes = 4 * (256 * 1024 + 16 * 1024);
    f64 unreliableMaxAge = 0.1;            ///< Unsent unreliable messages expire (STATE: notified lost).
    f64 rtoMin = 0.030;
    f64 rtoMax = 1.0;
    f64 initialRto = 0.25;
    f64 lossTimeFactor = 1.25;             ///< Time-threshold loss: 1.25 x srtt + maxAckDelay.
    u32 lossReorderThreshold = 3;          ///< Packet-threshold loss.
    f64 maxAckDelay = 0.010;               ///< Packets with reliable messages: ack-only packet after this.
    f64 maxLazyAckDelay = 0.010;           ///< Unreliable-only packets: ack-only packet after this.
    f64 peerMaxAckDelay = 0.010;           ///< The peer's maxAckDelay (loss-detection allowance).
    f64 peerMaxLazyAckDelay = 0.010;       ///< The peer's maxLazyAckDelay.
    u32 ackEveryPackets = 16;              ///< ...or after this many unacked packets (reliable acks 33).

    // --- send budget ------------------------------------------------------------------------
    u32 budgetBps = 256'000;               ///< Starting budget (bits per second on the wire).
    u32 minBudgetBps = 64'000;
    u32 maxBudgetBps = 256'000;
    bool aimd = true;
    AimdController::Config aimdConfig{};   ///< min/max/initial are taken from the fields above.
    f64 burstSeconds = 0.1;                ///< Bucket depth, in seconds of budget.
    u32 wireOverheadBytes = 48;            ///< Per datagram: netcode prefix/sequence/MAC + UDP/IP.
    u32 maxPacketsPerFlush = 4;            ///< "One packet per tick (<= 4 when budget allows)".
    f64 pacingInterval = 0.002;            ///< Gap between packets of one flush (0 = back to back).
    /// Packets flush() may send per second (0 = unlimited). Clients: 60, so a client ticking faster
    /// than 60 Hz still sends min(tick_hz, 60) pps (04 §2.5) and stays under the gateway's 120 pps
    /// policing. Queued data waits for the next flush; a burst of 2 absorbs tick jitter.
    f64 maxPacketsPerSecond = 0.0;

    // --- VOICE ------------------------------------------------------------------------------
    u32 voiceBudgetBps = 40'000;
    f64 voiceCoalesce = 0.005;             ///< Append to a game packet leaving within this window.

    // --- inbound policing (servers, 04 §9) --------------------------------------------------
    // Datagrams are policed on arrival by count and by bytes (game + voice rates combined, which
    // also bounds fragments of packets that never complete); after parsing, each packet's VOICE
    // messages are charged to the voice bucket and everything else to the game bucket, so a client
    // that sends no voice still gets only the game rate (04 §9: 64 kbit/s plus a separate 40
    // kbit/s VOICE bucket). A policed packet is dropped unacked (the sender resends reliable data).
    f64 inboundPacketsPerSecond = 0.0;     ///< 0 disables packet-rate policing.
    f64 inboundPacketBurst = 240.0;
    f64 inboundBytesPerSecond = 0.0;       ///< Game bytes (all but VOICE); 0 disables byte policing.
    f64 inboundByteBurst = 16 * 1024.0;
    f64 inboundVoiceBytesPerSecond = 0.0;  ///< VOICE bytes (used when byte policing is on).
    f64 inboundVoiceByteBurst = 4 * 1024.0;
    u32 malformedStrikeLimit = 3;          ///< Malformed packets before isMalformed() (disconnect).
    bool debugChannel = HELIOS_NET_DEBUG_CHANNEL != 0;

    /// Client -> gateway: fixed 64 kbit/s upstream bucket, at most 60 pps, no AIMD; acks ride the
    /// client's tick packets (20 ms).
    static ConnectionConfig client();
    /// Gateway -> client: 256 kbit/s AIMD (64 .. 256; raise maxBudgetBps to 512k in battles),
    /// inbound policing at 120 pps (240 burst), 64 kbit/s of game data and a separate 40 kbit/s
    /// VOICE bucket; acks for unreliable-only client packets ride the next tick packet (<= 100 ms),
    /// reliable ones leave within 10 ms; client-link memory bounds (64 KB windows, 8 reassemblies).
    static ConnectionConfig server();
    /// Gateway <-> cell and cell <-> cell trunks (04 §2.6): 4,096-entry packet buffers, 256 KB
    /// reassembly, no AIMD, no policing, 2 Gbit/s budget, no pacing.
    static ConnectionConfig trunk();
};

struct ChannelStats {
    u64 messagesSent = 0;     ///< Messages queued by the application.
    u64 messagesReceived = 0; ///< Messages delivered to the application.
    u64 bytesSent = 0;
    u64 bytesReceived = 0;
    u64 resends = 0;          ///< Reliable retransmissions and INPUT redundancy copies.
    u64 dropped = 0;          ///< Expired unsent, or out of date on arrival (sequenced).
    u64 duplicates = 0;       ///< Received more than once (reliable) and discarded.
    u64 wouldBlock = 0;
};

struct ConnectionStats {
    bool hasRtt = false;
    f64 rttMs = 0.0;      ///< Smoothed RTT.
    f64 rttVarMs = 0.0;
    f64 minRttMs = 0.0;
    f64 jitterMs = 0.0;
    f64 rtoMs = 0.0;
    f64 lossPercent = 0.0; ///< Ack-eliciting packets lost over the last second.
    u32 budgetBps = 0;
    u32 aimdDecreases = 0;
    // reliable's own view (smoothed; bandwidth includes packet_header_size overhead).
    f32 reliableRttMs = 0.0f;
    f32 reliableLossPercent = 0.0f;
    f32 sentKbps = 0.0f;
    f32 receivedKbps = 0.0f;
    f32 ackedKbps = 0.0f;
    u64 packetsSent = 0;
    u64 packetsReceived = 0;
    u64 packetsAcked = 0;
    u64 packetsLost = 0;
    u64 spuriousLosses = 0;   ///< Declared lost, acked later.
    u64 ackOnlyPackets = 0;
    u64 packetRateDeferred = 0; ///< flush() calls held back by maxPacketsPerSecond.
    u64 jumboPackets = 0;     ///< Packets reliable had to fragment.
    u64 voicePackets = 0;
    u64 bytesSent = 0;        ///< Helios packet bytes handed to reliable.
    u64 bytesReceived = 0;
    u64 wireBytesSent = 0;    ///< Charged to the budget (with fragment + wire overhead).
    u64 fragmentsSent = 0;
    u64 fragmentsReceived = 0;
    u64 fragmentsInvalid = 0;
    u64 packetsStale = 0;
    u64 packetsDuplicate = 0;
    u64 packetsInvalid = 0;
    u64 malformedPackets = 0;
    u64 inboundRateLimited = 0;
    u64 voiceRateLimited = 0;
    u64 stateDelivered = 0;
    u64 stateLost = 0;
    u64 reorderOverflows = 0; ///< Packets refused because they would exceed maxReorderBytes.
    std::array<ChannelStats, kChannelCount> channels{};
};

/// Receives each packet a Connection sends (the payload of one netcode packet, <= 1,200 bytes).
using PacketSendFn = void (*)(void* context, std::span<const u8> packet);

class Connection {
public:
    struct ReceivedMessage {
        Channel channel;
        std::span<const u8> payload;
    };

    /// Creates the reliable endpoint and channel state. Fails if the config is inconsistent.
    static Result<std::unique_ptr<Connection>> create(const ConnectionConfig& config, PacketSendFn send, void* context,
                                                      f64 now, std::string_view name = "conn");
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Queues a message. STATE sends are reported through notifications() (seq not returned).
    SendResult send(Channel channel, std::span<const u8> payload);
    /// Queues a STATE chunk and returns its notification id in `outSeq`.
    SendResult sendState(std::span<const u8> chunk, u64* outSeq = nullptr);

    /// Feeds the payload of one packet received from the peer at time `now` (seconds).
    void receivePacket(std::span<const u8> packet, f64 now);
    void update(f64 now);
    void flush(f64 now);

    /// Messages delivered since the last clearInbox(), in delivery order. Views stay valid until
    /// clearInbox(); send() may be called from `fn`.
    template <class Fn>
    void forEachMessage(Fn&& fn) const {
        const usize n = inboxSize();
        for (usize i = 0; i < n; ++i) fn(inboxAt(i));
    }
    usize inboxSize() const noexcept;
    ReceivedMessage inboxAt(usize index) const noexcept;
    void clearInbox() noexcept;

    std::span<const PacketNotify> notifications() const noexcept;
    void clearNotifications() noexcept;

    /// Replication writer allowance for one tick: budget / 8 / tickHz minus the reliable backlog.
    usize stateBudgetBytes(u32 tickHz) const noexcept;
    u32 budgetBps() const noexcept;
    void setBudgetBps(u32 bps) noexcept;
    /// Raises/lowers the AIMD ceiling (battle mode, 04 §2.5).
    void setMaxBudgetBps(u32 bps) noexcept;

    /// Malformed packets received so far; isMalformed() once the strike limit is reached.
    u32 strikes() const noexcept;
    bool isMalformed() const noexcept;
    /// Bytes queued in reliable windows that still need a (re)send.
    usize pendingReliableBytes() const noexcept;
    /// True when anything is queued (messages or pending acks).
    bool hasPendingSends() const noexcept;

    ConnectionStats stats() const;
    const ConnectionConfig& config() const noexcept;
    f64 now() const noexcept;

    struct Impl;

private:
    explicit Connection(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

} // namespace helios::net
