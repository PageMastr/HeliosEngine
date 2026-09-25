// HTP message framing: varints, message writer and the validating packet parser.

#include "helios/net/wire.h"

#include <cstring>

namespace helios::net::wire {

usize writeVarint(u8* out, u32 value) noexcept {
    usize n = 0;
    while (value >= 0x80) {
        out[n++] = static_cast<u8>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out[n++] = static_cast<u8>(value);
    return n;
}

usize writeVarint64(u8* out, u64 value) noexcept {
    usize n = 0;
    while (value >= 0x80) {
        out[n++] = static_cast<u8>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out[n++] = static_cast<u8>(value);
    return n;
}

namespace {
template <class T, usize MaxBytes>
usize readVarintImpl(std::span<const u8> in, T& value) noexcept {
    constexpr usize kBits = sizeof(T) * 8;
    T result = 0;
    for (usize i = 0; i < MaxBytes; ++i) {
        if (i >= in.size()) return 0;
        const u8 byte = in[i];
        const usize shift = i * 7;
        const T bits = static_cast<T>(byte & 0x7F);
        // Reject bits beyond the type's width in the last group.
        if (shift + 7 > kBits && (bits >> (kBits - shift)) != 0) return 0;
        result |= static_cast<T>(bits << shift);
        if ((byte & 0x80) == 0) {
            // Canonical form only: a multi-byte varint must not end in a zero group.
            if (i > 0 && byte == 0) return 0;
            value = result;
            return i + 1;
        }
    }
    return 0; // too long
}
} // namespace

usize readVarint(std::span<const u8> in, u32& value) noexcept { return readVarintImpl<u32, kMaxVarintBytes>(in, value); }
usize readVarint64(std::span<const u8> in, u64& value) noexcept {
    return readVarintImpl<u64, kMaxVarint64Bytes>(in, value);
}

usize writeMessage(std::span<u8> out, Channel channel, u16 seq, u8 sliceIndex, u32 sliceCount,
                   std::span<const u8> payload) noexcept {
    const ChannelInfo& info = channelInfo(channel);
    const u32 length = static_cast<u32>(payload.size());
    const usize total = encodedSize(channel, length);
    if (total > out.size() || payload.size() > 0xFFFFFFFFull) return 0;
    u8* p = out.data();
    u8 hdr = static_cast<u8>(channelId(channel) & kChannelMask);
    if (info.sliced) hdr |= kSliceBit;
    if (info.sequenced) hdr |= kSequencedBit;
    *p++ = hdr;
    if (info.sequenced) {
        *p++ = static_cast<u8>(seq & 0xFF);
        *p++ = static_cast<u8>(seq >> 8);
    }
    if (info.sliced) {
        *p++ = sliceIndex;
        *p++ = static_cast<u8>(sliceCount - 1);
    }
    p += writeVarint(p, length);
    if (length != 0) std::memcpy(p, payload.data(), length);
    return total;
}

std::string_view parseErrorName(ParseError e) noexcept {
    switch (e) {
    case ParseError::None: return "None";
    case ParseError::Empty: return "Empty";
    case ParseError::Truncated: return "Truncated";
    case ParseError::ReservedBits: return "ReservedBits";
    case ParseError::UnknownChannel: return "UnknownChannel";
    case ParseError::BadVarint: return "BadVarint";
    case ParseError::FlagMismatch: return "FlagMismatch";
    case ParseError::BadSlice: return "BadSlice";
    case ParseError::TooLarge: return "TooLarge";
    case ParseError::NonZeroPadding: return "NonZeroPadding";
    case ParseError::TooManyMessages: return "TooManyMessages";
    case ParseError::ChannelDisabled: return "ChannelDisabled";
    }
    return "?";
}

ParseError parsePacket(std::span<const u8> packet, const ParseLimits& limits, std::vector<MessageView>& out) {
    out.clear();
    if (packet.empty()) return ParseError::Empty;
    usize pos = 0;
    while (pos < packet.size()) {
        const u8 hdr = packet[pos];
        const u8 id = hdr & kChannelMask;
        if (id == (kPadByte & kChannelMask)) {
            // Padding: the header must be exactly 0x0F and every following byte zero.
            if (hdr != kPadByte) return ParseError::ReservedBits;
            for (usize i = pos + 1; i < packet.size(); ++i)
                if (packet[i] != 0) return ParseError::NonZeroPadding;
            return ParseError::None;
        }
        if (hdr & kReservedMask) return ParseError::ReservedBits;
        if (!isValidChannelId(id)) return ParseError::UnknownChannel;
        const Channel channel = static_cast<Channel>(id);
        const ChannelInfo& info = channelInfo(channel);
        if (channel == Channel::Debug && !limits.allowDebug) return ParseError::ChannelDisabled;
        const bool sliced = (hdr & kSliceBit) != 0;
        const bool sequenced = (hdr & kSequencedBit) != 0;
        if (sliced != info.sliced || sequenced != info.sequenced) return ParseError::FlagMismatch;
        ++pos;
        MessageView view;
        view.channel = channel;
        if (sequenced) {
            if (packet.size() - pos < 2) return ParseError::Truncated;
            view.seq = static_cast<u16>(packet[pos] | (packet[pos + 1] << 8));
            pos += 2;
        }
        if (sliced) {
            if (packet.size() - pos < 2) return ParseError::Truncated;
            view.sliceIndex = packet[pos];
            view.sliceCount = static_cast<u16>(packet[pos + 1] + 1);
            pos += 2;
            if (view.sliceIndex >= view.sliceCount) return ParseError::BadSlice;
        }
        u32 length = 0;
        const usize n = readVarint(packet.subspan(pos), length);
        if (n == 0) return ParseError::BadVarint;
        pos += n;
        if (length > limits.maxPayload[id]) return ParseError::TooLarge;
        if (sliced && length == 0 && view.sliceCount != 1) return ParseError::BadSlice; // only an empty blob
        if (length > packet.size() - pos) return ParseError::Truncated;
        view.payload = packet.subspan(pos, length);
        pos += length;
        if (out.size() >= limits.maxMessages) return ParseError::TooManyMessages;
        out.push_back(view);
    }
    return ParseError::None;
}

usize writeStreamMessage(std::span<u8> out, u64 streamId, std::span<const u8> payload) noexcept {
    const usize total = varint64Size(streamId) + payload.size();
    if (total > out.size()) return 0;
    const usize n = writeVarint64(out.data(), streamId);
    if (!payload.empty()) std::memcpy(out.data() + n, payload.data(), payload.size());
    return total;
}

bool readStreamMessage(std::span<const u8> in, u64& streamId, std::span<const u8>& payload) noexcept {
    const usize n = readVarint64(in, streamId);
    if (n == 0) return false;
    payload = in.subspan(n);
    return true;
}

} // namespace helios::net::wire
