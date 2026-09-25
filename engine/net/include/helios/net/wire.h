#pragma once
// HTP message framing inside one reliable packet (04 §2.1, bottom row):
//
//   packet  := message* [pad]
//   message := hdr:u8  [msg_seq:u16le]  [slice_index:u8 slice_count_minus_1:u8]  len:varint  payload
//   hdr     := channel (bits 0-3) | slice (bit 4) | sequenced (bit 5) | reserved (bits 6-7, zero)
//   pad     := 0x0F followed only by zero bytes (channel 15 = padding / ack-only marker)
//
// `sequenced` must match the channel (ChannelInfo::sequenced), `slice` is set on BULK and only on
// BULK, `len` is a canonical LEB128 u32. An ack-only packet is the single byte 0x0F: it carries
// reliable's ack header and no messages, so it is not itself ack-eliciting.
//
// parsePacket() validates the whole packet before anything is delivered, never reads out of
// bounds and never asserts on input: it is the fuzzing surface for the channel parser (04 §9).
//
// Threading: pure functions.

#include <span>
#include <string_view>
#include <vector>

#include "helios/core/types.h"
#include "helios/net/channel.h"

namespace helios::net::wire {

/// netcode's largest payload (NETCODE_MAX_PACKET_SIZE).
inline constexpr usize kNetcodeMaxPayload = 1200;
/// reliable's largest packet header (RELIABLE_MAX_PACKET_HEADER_BYTES).
inline constexpr usize kReliableMaxHeader = 9;
/// Largest Helios packet that travels unfragmented: fills one netcode payload exactly.
inline constexpr usize kMaxPacketPayload = kNetcodeMaxPayload - kReliableMaxHeader;

inline constexpr u8 kChannelMask = 0x0F;
inline constexpr u8 kSliceBit = 0x10;
inline constexpr u8 kSequencedBit = 0x20;
inline constexpr u8 kReservedMask = 0xC0;
inline constexpr u8 kPadByte = 0x0F;
inline constexpr usize kMaxVarintBytes = 5;
inline constexpr usize kMaxVarint64Bytes = 10;
inline constexpr u32 kMaxSlicesPerBlob = 256;

/// Encoded size of `value` as LEB128.
constexpr usize varintSize(u32 value) noexcept {
    usize n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}
constexpr usize varint64Size(u64 value) noexcept {
    usize n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

/// Writes LEB128; `out` must have varintSize(value) bytes. Returns bytes written.
usize writeVarint(u8* out, u32 value) noexcept;
usize writeVarint64(u8* out, u64 value) noexcept;
/// Reads a canonical (minimal-length) LEB128. Returns bytes consumed, 0 on truncation, overflow or
/// a non-minimal encoding.
usize readVarint(std::span<const u8> in, u32& value) noexcept;
usize readVarint64(std::span<const u8> in, u64& value) noexcept;

/// Header bytes a message of `length` payload bytes needs on `channel`.
constexpr usize headerSize(Channel channel, u32 length) noexcept {
    const ChannelInfo& info = channelInfo(channel);
    return 1 + (info.sequenced ? 2 : 0) + (info.sliced ? 2 : 0) + varintSize(length);
}
constexpr usize encodedSize(Channel channel, u32 length) noexcept { return headerSize(channel, length) + length; }
/// Largest payload of `channel` that fits a packet of `packetBytes` on its own.
constexpr u32 maxPayloadFor(Channel channel, usize packetBytes) noexcept {
    const ChannelInfo& info = channelInfo(channel);
    const usize fixed = 1 + (info.sequenced ? 2 : 0) + (info.sliced ? 2 : 0);
    if (packetBytes <= fixed + 1) return 0;
    usize payload = packetBytes - fixed - 1;
    while (payload > 0 && fixed + varintSize(static_cast<u32>(payload)) + payload > packetBytes) --payload;
    return static_cast<u32>(payload);
}

/// Writes one message. `sliceCount` is 1..256 (ignored unless the channel is sliced). Returns the
/// bytes written, or 0 when it does not fit in `out`.
usize writeMessage(std::span<u8> out, Channel channel, u16 seq, u8 sliceIndex, u32 sliceCount,
                   std::span<const u8> payload) noexcept;

struct MessageView {
    Channel channel = Channel::Control;
    u16 seq = 0;
    u8 sliceIndex = 0;
    u16 sliceCount = 1;
    std::span<const u8> payload;
};

struct ParseLimits {
    /// Largest payload accepted per channel (BULK: per slice).
    std::array<u32, kChannelCount> maxPayload{};
    /// Upper bound on messages per packet (bounds work per packet).
    u32 maxMessages = 8192;
    /// False rejects DEBUG messages (shipping builds).
    bool allowDebug = true;
};

enum class ParseError : u8 {
    None,
    Empty,
    Truncated,
    ReservedBits,
    UnknownChannel,
    BadVarint,
    FlagMismatch,
    BadSlice,
    TooLarge,
    NonZeroPadding,
    TooManyMessages,
    ChannelDisabled,
};

std::string_view parseErrorName(ParseError e) noexcept;

/// Parses and validates a whole packet. On success `out` holds a view per message (empty for an
/// ack-only packet) pointing into `packet`. On failure `out` is unspecified.
ParseError parsePacket(std::span<const u8> packet, const ParseLimits& limits, std::vector<MessageView>& out);

/// Trunk framing (04 §2.6): a varint stream id (session or AG) followed by the payload.
usize writeStreamMessage(std::span<u8> out, u64 streamId, std::span<const u8> payload) noexcept;
bool readStreamMessage(std::span<const u8> in, u64& streamId, std::span<const u8>& payload) noexcept;

} // namespace helios::net::wire
