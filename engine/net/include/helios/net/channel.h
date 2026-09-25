#pragma once
// HTP channels (04 §2.2). The channel set, ids and semantics are part of the wire protocol:
//
//   id  channel   semantics                                              use
//   0   CONTROL   reliable-ordered, packed first; may exceed one          time sync, TiDi, routes,
//                 datagram (reliable fragments bursts <= 16 KB)          reconnect tickets, kick
//   1   INPUT     unreliable, sequenced; the last 4 inputs ride           command frames c->s
//                 redundantly until acked
//   2   STATE     unreliable + delivery notify (PacketNotify per chunk)   replication chunks
//   3   EVENT_R   reliable-ordered                                        RPCs, inventory, UI, chat
//   4   EVENT_U   unreliable                                              VFX/audio cues, hit markers
//   5   LATEST    unreliable-sequenced (older than the newest dropped)    aim/camera look
//   6   BULK      reliable-ordered, 1 KB slices, window 64, lowest        blobs <= 256 KB
//                 priority
//   7   DEBUG     reliable-ordered; compiled out of shipping builds       inspector, dev cheats
//   8   VOICE     unreliable, not tick-packed, own budget                 Opus frames (Phase 3)
//   9-15          reserved (15 is the in-packet padding marker, see wire.h)
//
// Packing order within a packet: CONTROL > EVENT_R > INPUT/STATE > EVENT_U > LATEST > DEBUG > BULK;
// VOICE is sent on arrival in its own datagram or appended to a game packet leaving within 5 ms.
//
// Threading: constant data, safe everywhere.

#include <array>
#include <string_view>

#include "helios/core/types.h"

#ifndef HELIOS_NET_DEBUG_CHANNEL
#if defined(HELIOS_SHIPPING)
#define HELIOS_NET_DEBUG_CHANNEL 0
#else
#define HELIOS_NET_DEBUG_CHANNEL 1
#endif
#endif

namespace helios::net {

enum class Channel : u8 {
    Control = 0,
    Input = 1,
    State = 2,
    EventReliable = 3,
    EventUnreliable = 4,
    Latest = 5,
    Bulk = 6,
    Debug = 7,
    Voice = 8,
};

inline constexpr u32 kChannelCount = 9;

enum class Reliability : u8 {
    Unreliable,          ///< Delivered at most once, in arrival order.
    UnreliableSequenced, ///< At most once; anything older than the newest delivered is dropped.
    UnreliableRedundant, ///< Sequenced; each message is repeated in later packets until acked
                         ///< or superseded by `inputRedundancy` newer ones.
    ReliableOrdered,     ///< Exactly once, in send order.
};

struct ChannelInfo {
    Channel channel;
    std::string_view name;
    Reliability reliability;
    bool deliveryNotify; ///< STATE: each send is later reported delivered or lost.
    bool sliced;         ///< BULK: messages are blobs split into acked slices.
    bool jumbo;          ///< May exceed one datagram (sent as one reliable-fragmented packet).
    bool tickPacked;     ///< False for VOICE (sent on arrival).
    bool sequenced;      ///< Messages carry a u16 msg_seq on the wire.
};

inline constexpr std::array<ChannelInfo, kChannelCount> kChannelInfos = {{
    {Channel::Control, "CONTROL", Reliability::ReliableOrdered, false, false, true, true, true},
    {Channel::Input, "INPUT", Reliability::UnreliableRedundant, false, false, false, true, true},
    {Channel::State, "STATE", Reliability::Unreliable, true, false, false, true, false},
    {Channel::EventReliable, "EVENT_R", Reliability::ReliableOrdered, false, false, false, true, true},
    {Channel::EventUnreliable, "EVENT_U", Reliability::Unreliable, false, false, false, true, false},
    {Channel::Latest, "LATEST", Reliability::UnreliableSequenced, false, false, false, true, true},
    {Channel::Bulk, "BULK", Reliability::ReliableOrdered, false, true, false, true, true},
    {Channel::Debug, "DEBUG", Reliability::ReliableOrdered, false, false, true, true, true},
    {Channel::Voice, "VOICE", Reliability::Unreliable, false, false, false, false, false},
}};

/// Order in which channels fill a packet (04 §2.2).
inline constexpr std::array<Channel, 8> kPackOrder = {
    Channel::Control, Channel::EventReliable, Channel::Input,  Channel::State,
    Channel::EventUnreliable, Channel::Latest, Channel::Debug, Channel::Bulk,
};

constexpr u8 channelId(Channel c) noexcept { return static_cast<u8>(c); }
constexpr bool isValidChannelId(u32 id) noexcept { return id < kChannelCount; }
constexpr const ChannelInfo& channelInfo(Channel c) noexcept { return kChannelInfos[static_cast<usize>(c)]; }
constexpr bool isReliable(Channel c) noexcept { return channelInfo(c).reliability == Reliability::ReliableOrdered; }
constexpr std::string_view channelName(Channel c) noexcept { return channelInfo(c).name; }

/// Result of queuing a message.
enum class SendResult : u8 {
    Ok,
    WouldBlock,    ///< Reliable window full (1,024 messages or BULK slices in flight per channel).
    TooLarge,      ///< Larger than the channel allows.
    NotConnected,  ///< No such session, or not connected yet.
    Unsupported,   ///< DEBUG in a shipping build.
    RateLimited,   ///< VOICE over its own token bucket.
};

constexpr std::string_view sendResultName(SendResult r) noexcept {
    switch (r) {
    case SendResult::Ok: return "Ok";
    case SendResult::WouldBlock: return "WouldBlock";
    case SendResult::TooLarge: return "TooLarge";
    case SendResult::NotConnected: return "NotConnected";
    case SendResult::Unsupported: return "Unsupported";
    case SendResult::RateLimited: return "RateLimited";
    }
    return "?";
}

} // namespace helios::net
