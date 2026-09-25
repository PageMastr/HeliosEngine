// HTP message framing: varints, writer, validating parser, trunk stream framing.

#include <doctest/doctest.h>

#include <vector>

#include "helios/net/wire.h"

using namespace helios;
using namespace helios::net;
using namespace helios::net::wire;

namespace {

ParseLimits permissive() {
    ParseLimits l;
    for (auto& v : l.maxPayload) v = 64 * 1024;
    return l;
}

} // namespace

TEST_SUITE("net.wire") {
    TEST_CASE("varint round trip and canonical form") {
        const u32 values[] = {0u, 1u, 127u, 128u, 300u, 16383u, 16384u, 0x0FFFFFFFu, 0xFFFFFFFFu};
        for (u32 v : values) {
            u8 buf[kMaxVarintBytes];
            const usize n = writeVarint(buf, v);
            CHECK(n == varintSize(v));
            u32 out = 0;
            CHECK(readVarint(std::span<const u8>(buf, n), out) == n);
            CHECK(out == v);
            if (n > 1) {
                u32 dummy = 0;
                CHECK(readVarint(std::span<const u8>(buf, n - 1), dummy) == 0); // truncated
            }
        }
        u32 v = 0;
        const u8 nonMinimal[] = {0x80, 0x00};
        CHECK(readVarint(nonMinimal, v) == 0);
        const u8 overflow[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F}; // 35 bits
        CHECK(readVarint(overflow, v) == 0);
        const u8 tooLong[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
        CHECK(readVarint(tooLong, v) == 0);
        const u8 max32[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
        CHECK(readVarint(max32, v) == 5);
        CHECK(v == 0xFFFFFFFFu);

        u8 buf64[kMaxVarint64Bytes];
        const u64 big = 0xFFFFFFFFFFFFFFFFull;
        const usize n = writeVarint64(buf64, big);
        CHECK(n == 10);
        u64 out64 = 0;
        CHECK(readVarint64(std::span<const u8>(buf64, n), out64) == 10);
        CHECK(out64 == big);
    }

    TEST_CASE("message round trip on every channel") {
        std::vector<u8> packet(kMaxPacketPayload);
        usize used = 0;
        std::vector<std::vector<u8>> payloads;
        for (u32 c = 0; c < kChannelCount; ++c) {
            std::vector<u8> p(10 + c * 3);
            for (usize i = 0; i < p.size(); ++i) p[i] = static_cast<u8>(c * 17 + i);
            const Channel ch = static_cast<Channel>(c);
            const usize n = writeMessage(std::span<u8>(packet.data() + used, packet.size() - used), ch,
                                         static_cast<u16>(1000 + c), 2, 5, p);
            REQUIRE(n == encodedSize(ch, static_cast<u32>(p.size())));
            used += n;
            payloads.push_back(p);
        }
        std::vector<MessageView> views;
        REQUIRE(parsePacket(std::span<const u8>(packet.data(), used), permissive(), views) == ParseError::None);
        REQUIRE(views.size() == kChannelCount);
        for (u32 c = 0; c < kChannelCount; ++c) {
            const MessageView& v = views[c];
            const ChannelInfo& info = channelInfo(static_cast<Channel>(c));
            CHECK(v.channel == static_cast<Channel>(c));
            CHECK(std::equal(v.payload.begin(), v.payload.end(), payloads[c].begin(), payloads[c].end()));
            if (info.sequenced) CHECK(v.seq == 1000 + c);
            if (info.sliced) {
                CHECK(v.sliceIndex == 2);
                CHECK(v.sliceCount == 5);
            }
        }
    }

    TEST_CASE("ack-only and padding") {
        std::vector<MessageView> views;
        const u8 ackOnly[] = {kPadByte};
        CHECK(parsePacket(ackOnly, permissive(), views) == ParseError::None);
        CHECK(views.empty());
        const u8 padded[] = {0x04, 0x01, 0xAA, kPadByte, 0, 0, 0};
        REQUIRE(parsePacket(padded, permissive(), views) == ParseError::None);
        REQUIRE(views.size() == 1);
        CHECK(views[0].channel == Channel::EventUnreliable);
        const u8 dirtyPad[] = {kPadByte, 0, 1};
        CHECK(parsePacket(dirtyPad, permissive(), views) == ParseError::NonZeroPadding);
        const u8 flaggedPad[] = {0x1F};
        CHECK(parsePacket(flaggedPad, permissive(), views) == ParseError::ReservedBits);
        CHECK(parsePacket({}, permissive(), views) == ParseError::Empty);
    }

    TEST_CASE("parser rejects malformed messages") {
        std::vector<MessageView> views;
        struct Case {
            std::vector<u8> bytes;
            ParseError expected;
        };
        const Case cases[] = {
            {{0x44, 0x00}, ParseError::ReservedBits},              // bit 6 set
            {{0x84, 0x00}, ParseError::ReservedBits},              // bit 7 set
            {{0x09, 0x00}, ParseError::UnknownChannel},            // channel 9 reserved
            {{0x0E, 0x00}, ParseError::UnknownChannel},            // channel 14 reserved
            {{0x00, 0x00}, ParseError::FlagMismatch},              // CONTROL without sequenced bit
            {{0x24, 0x00, 0x00, 0x00}, ParseError::FlagMismatch},  // EVENT_U with sequenced bit
            {{0x33, 0, 0, 0, 0, 1, 0}, ParseError::FlagMismatch},  // slice bit on EVENT_R
            {{0x26, 0, 0, 0, 0, 1, 0}, ParseError::FlagMismatch},  // BULK without slice bit
            {{0x36, 0, 0, 3, 2, 1, 0xAA}, ParseError::BadSlice},   // slice index >= count
            {{0x36, 0, 0, 0, 1, 0}, ParseError::BadSlice},         // empty slice of a 2-slice blob
            {{0x20, 0x01}, ParseError::Truncated},                 // missing msg_seq byte
            {{0x36, 0x01, 0x00, 0x00}, ParseError::Truncated},     // missing slice count
            {{0x04, 0x05, 0x01, 0x02}, ParseError::Truncated},     // len 5, 2 bytes left
            {{0x04, 0x80}, ParseError::BadVarint},                 // truncated varint
            {{0x04, 0x80, 0x00}, ParseError::BadVarint},           // non-minimal varint
            {{0x04}, ParseError::BadVarint},                       // no length at all
        };
        for (const Case& c : cases) {
            INFO(static_cast<int>(c.expected));
            CHECK(parsePacket(c.bytes, permissive(), views) == c.expected);
        }
        // Per-channel size limit.
        ParseLimits limits = permissive();
        limits.maxPayload[channelId(Channel::EventUnreliable)] = 2;
        const u8 big[] = {0x04, 0x03, 1, 2, 3};
        CHECK(parsePacket(big, limits, views) == ParseError::TooLarge);
        // Message count limit.
        limits = permissive();
        limits.maxMessages = 2;
        const u8 three[] = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00};
        CHECK(parsePacket(three, limits, views) == ParseError::TooManyMessages);
        // DEBUG disabled (shipping).
        limits = permissive();
        limits.allowDebug = false;
        const u8 debug[] = {0x27, 0x00, 0x00, 0x00};
        CHECK(parsePacket(debug, limits, views) == ParseError::ChannelDisabled);
    }

    TEST_CASE("maxPayloadFor fills a packet exactly") {
        for (u32 c = 0; c < kChannelCount; ++c) {
            const Channel ch = static_cast<Channel>(c);
            const u32 p = maxPayloadFor(ch, kMaxPacketPayload);
            CHECK(encodedSize(ch, p) <= kMaxPacketPayload);
            CHECK(encodedSize(ch, p + 1) > kMaxPacketPayload);
        }
        CHECK(kMaxPacketPayload == 1191);
        CHECK(maxPayloadFor(Channel::State, kMaxPacketPayload) >= 1100); // replication chunks (04 §2.6)
    }

    TEST_CASE("writeMessage refuses to overflow") {
        u8 small[8];
        const u8 payload[16] = {};
        CHECK(writeMessage(small, Channel::EventUnreliable, 0, 0, 1, payload) == 0);
        CHECK(writeMessage(small, Channel::EventUnreliable, 0, 0, 1, std::span<const u8>(payload, 6)) == 8);
    }

    TEST_CASE("trunk stream framing") {
        u8 buf[64];
        const u8 payload[] = {1, 2, 3};
        const usize n = writeStreamMessage(buf, 0x123456789ull, payload);
        REQUIRE(n == varint64Size(0x123456789ull) + 3);
        u64 id = 0;
        std::span<const u8> out;
        REQUIRE(readStreamMessage(std::span<const u8>(buf, n), id, out));
        CHECK(id == 0x123456789ull);
        CHECK(out.size() == 3);
        CHECK(out[2] == 3);
        const u8 bad[] = {0x80};
        CHECK_FALSE(readStreamMessage(bad, id, out));
        CHECK(writeStreamMessage(std::span<u8>(buf, 2), 0x123456789ull, payload) == 0);
    }
}
