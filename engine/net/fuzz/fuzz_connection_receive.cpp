// Fuzz target: everything behind netcode's decryption on one connection — reliable's packet and
// fragment headers, fragment reassembly, the HTP parser, per-channel ordering buffers, BULK slice
// reassembly, ack processing — driven by a sequence of packets. Input: mode byte (client /
// server / trunk profile) then [u16 length][packet] records.

#include <cstdlib>

#include "fuzz_common.h"
#include "helios/net/connection.h"

using namespace helios;
using namespace helios::net;

namespace {

void nullSend(void*, std::span<const u8>) {}

struct Capture {
    std::vector<std::vector<u8>> packets;
    static void send(void* ctx, std::span<const u8> p) { static_cast<Capture*>(ctx)->packets.emplace_back(p.begin(), p.end()); }
};

ConnectionConfig configFor(u8 mode) {
    ConnectionConfig c = mode % 3 == 0 ? ConnectionConfig::client()
                         : mode % 3 == 1 ? ConnectionConfig::server()
                                         : ConnectionConfig::trunk();
    c.inboundPacketsPerSecond = 0.0; // policing would hide most inputs
    c.inboundBytesPerSecond = 0.0;
    if (mode % 3 == 1) {
        // Server profile: run the per-message game/voice byte accounting with limits that never bind.
        c.inboundBytesPerSecond = 1e12;
        c.inboundByteBurst = 1e12;
        c.inboundVoiceBytesPerSecond = 1e12;
        c.inboundVoiceByteBurst = 1e12;
    }
    return c;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const ConnectionConfig cfg = configFor(data[0]);
    auto created = Connection::create(cfg, &nullSend, nullptr, 0.0, "fuzz");
    if (!created) std::abort();
    Connection& conn = *created.value();
    f64 t = 0.0;
    for (const auto& rec : fuzz::splitRecords(data + 1, size - 1, 4096)) {
        if (rec.empty()) continue;
        conn.receivePacket(rec, t);
        t += 0.0005 * (rec[0] & 7);
        if (rec.size() & 1) conn.update(t);
        if ((rec.size() & 3) == 0) {
            (void)conn.send(Channel::EventReliable, rec.first(std::min<usize>(rec.size(), 64)));
            conn.flush(t);
        }
    }
    usize total = 0;
    conn.forEachMessage([&](const Connection::ReceivedMessage& m) {
        total += m.payload.size();
        if (m.channel == Channel::Bulk ? m.payload.size() > cfg.maxBulkBlobBytes : m.payload.size() > cfg.maxJumboMessageBytes)
            std::abort();
    });
    conn.clearInbox();
    (void)conn.stats();
    (void)total;
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    // Real traffic from a sender Connection: every channel, a 16 KB CONTROL burst (17 reliable
    // fragments), a 5 KB BULK blob (5 slices), ack-only packets.
    for (u8 mode = 0; mode < 3; ++mode) {
        Capture cap;
        ConnectionConfig cfg = configFor(mode);
        cfg.budgetBps = cfg.maxBudgetBps = 100'000'000;
        cfg.maxPacketsPerFlush = 1000;
        cfg.pacingInterval = 0.0;
        auto sender = Connection::create(cfg, &Capture::send, &cap, 0.0, "seed").value();
        for (u32 c = 0; c < kChannelCount; ++c) {
            const Channel ch = static_cast<Channel>(c);
            std::vector<u8> p(8 + c, static_cast<u8>(c));
            (void)sender->send(ch, p);
        }
        (void)sender->send(Channel::Control, std::vector<u8>(mode == 0 ? 16 * 1024 : 3000, 0x5A));
        (void)sender->send(Channel::Bulk, std::vector<u8>(5000, 0xB1));
        sender->flush(0.0);
        for (u32 i = 0; i < 4; ++i) {
            (void)sender->send(Channel::EventReliable, std::vector<u8>(20, static_cast<u8>(i)));
            sender->flush(0.01 * (i + 1));
        }
        std::vector<u8> input{mode};
        for (const auto& p : cap.packets) fuzz::appendRecord(input, p);
        out.push_back(input);
        // Also a reordered copy.
        std::vector<u8> reversed{mode};
        for (auto it = cap.packets.rbegin(); it != cap.packets.rend(); ++it) fuzz::appendRecord(reversed, *it);
        out.push_back(reversed);
    }
}
