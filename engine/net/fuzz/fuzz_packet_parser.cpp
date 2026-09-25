// Fuzz target: the HTP channel parser (wire::parsePacket) and the other wire decoders.
// Property: a packet that parses re-serialises byte-exactly (every field is canonical), and
// whatever follows the messages is the padding marker plus zeros.

#include <cstdlib>

#include "fuzz_common.h"
#include "helios/net/wire.h"

using namespace helios;
using namespace helios::net;

namespace {

const wire::ParseLimits& limits() {
    static const wire::ParseLimits l = [] {
        wire::ParseLimits x;
        for (u32 i = 0; i < kChannelCount; ++i) {
            const Channel ch = static_cast<Channel>(i);
            if (channelInfo(ch).jumbo) x.maxPayload[i] = 16 * 1024;
            else if (ch == Channel::Bulk) x.maxPayload[i] = 1024;
            else x.maxPayload[i] = wire::maxPayloadFor(ch, wire::kMaxPacketPayload);
        }
        return x;
    }();
    return l;
}

[[noreturn]] void fail() { std::abort(); }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::span<const u8> in(data, size);
    std::vector<wire::MessageView> views;
    if (wire::parsePacket(in, limits(), views) == wire::ParseError::None) {
        std::vector<u8> out(size + 16);
        usize used = 0;
        for (const wire::MessageView& v : views) {
            if (v.payload.data() < data || v.payload.data() + v.payload.size() > data + size) fail();
            if (v.payload.size() > limits().maxPayload[channelId(v.channel)]) fail();
            const usize n = wire::writeMessage(std::span<u8>(out.data() + used, out.size() - used), v.channel, v.seq,
                                               v.sliceIndex, v.sliceCount, v.payload);
            if (n == 0) fail();
            used += n;
        }
        if (used > size || std::memcmp(out.data(), data, used) != 0) fail();
        if (used < size) {
            if (data[used] != wire::kPadByte) fail();
            for (usize i = used + 1; i < size; ++i)
                if (data[i] != 0) fail();
        }
        std::vector<wire::MessageView> again;
        if (wire::parsePacket(std::span<const u8>(out.data(), used == 0 ? 0 : used), limits(), again) !=
                (used == 0 ? wire::ParseError::Empty : wire::ParseError::None) ||
            again.size() != views.size())
            fail();
    }
    u32 v32 = 0;
    const usize n32 = wire::readVarint(in, v32);
    if (n32 > wire::kMaxVarintBytes || (n32 && n32 != wire::varintSize(v32))) fail();
    u64 v64 = 0;
    const usize n64 = wire::readVarint64(in, v64);
    if (n64 > wire::kMaxVarint64Bytes || (n64 && n64 != wire::varint64Size(v64))) fail();
    u64 stream = 0;
    std::span<const u8> payload;
    if (wire::readStreamMessage(in, stream, payload) && payload.size() > size) fail();
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    // One message per channel, a multi-message packet, ack-only, padding, and near misses.
    for (u32 c = 0; c < kChannelCount; ++c) {
        std::vector<u8> pkt(64);
        const u8 payload[] = {1, 2, 3, 4, 5};
        const usize n = wire::writeMessage(pkt, static_cast<Channel>(c), static_cast<u16>(c * 1000), 0, 1, payload);
        pkt.resize(n);
        out.push_back(pkt);
    }
    std::vector<u8> multi(wire::kMaxPacketPayload);
    usize used = 0;
    for (u32 i = 0; i < 20; ++i) {
        std::vector<u8> p(i * 7, static_cast<u8>(i));
        used += wire::writeMessage(std::span<u8>(multi.data() + used, multi.size() - used),
                                   static_cast<Channel>(i % kChannelCount), static_cast<u16>(i), 1, 3, p);
    }
    multi.resize(used);
    out.push_back(multi);
    out.push_back({wire::kPadByte});
    out.push_back({0x04, 0x01, 0x42, wire::kPadByte, 0, 0, 0, 0});
    out.push_back({0x36, 0x05, 0x00, 0x02, 0x02, 0x03, 0xAA, 0xBB, 0xCC});
    out.push_back({0x20, 0xFF, 0xFF, 0x80, 0x01});
    out.push_back({0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
}
