// Replication full-state primitives (helios/reflect/repl.h): bit streams, quantizers, protocol hash.

#include <doctest/doctest.h>

#include <cmath>

#include "helios/core/random.h"
#include "helios/reflect/repl.h"

using namespace helios;
using namespace helios::refl;
using namespace helios::refl::repl;

namespace {

TEST_CASE("repl: bit streams round-trip any widths and reject reads past the end") {
    SplitMix64 rng(0xb175);
    BitWriter w;
    std::vector<std::pair<u64, u32>> written;
    for (int i = 0; i < 2000; ++i) {
        const u32 bits = static_cast<u32>(rng.next() % 65);
        const u64 v = rng.next() & (bits == 64 ? ~u64(0) : (u64(1) << bits) - 1);
        w.write(v | (bits < 64 ? rng.next() << bits : 0), bits); // bits above `bits` are ignored
        written.emplace_back(v, bits);
    }
    w.writeVarint(0);
    w.writeVarint(300);
    w.writeVarint(~u64(0));
    BitReader r(w.bytes(), w.bitCount());
    for (const auto& [v, bits] : written) {
        auto got = r.read(bits);
        REQUIRE(got);
        CHECK(*got == v);
    }
    CHECK(*r.readVarint() == 0);
    CHECK(*r.readVarint() == 300);
    CHECK(*r.readVarint() == ~u64(0));
    CHECK(r.bitsLeft() == 0);
    CHECK_FALSE(r.read(1));
    CHECK_FALSE(r.readBool());
    CHECK_FALSE(BitReader(w.bytes(), 3).read(4)); // the bit count bounds reads, not the byte size
    CHECK_FALSE(r.read(65));
}

TEST_CASE("repl: hostile varints fail cleanly") {
    auto varint = [](std::initializer_list<int> b) {
        std::vector<u8> bytes;
        for (int v : b) bytes.push_back(static_cast<u8>(v));
        BitReader r(bytes);
        return r.readVarint();
    };
    CHECK(*varint({0x7f}) == 127);
    CHECK_FALSE(varint({0x80}));                                                           // truncated
    CHECK_FALSE(varint({0x80, 0x00}));                                                     // overlong zero
    CHECK_FALSE(varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}));     // > 64 bits
    CHECK_FALSE(varint({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x81, 0x00})); // > 10 groups
}

TEST_CASE("repl: range quantization clamps, maps NaN to min and stays within half a step") {
    CHECK(quantizeRange(-100, -64, 64, 12) == 0);
    CHECK(quantizeRange(100, -64, 64, 12) == 4095);
    CHECK(quantizeRange(std::nan(""), -64, 64, 12) == 0);
    CHECK(quantizeRange(-64, -64, 64, 12) == 0);
    CHECK(quantizeRange(64, -64, 64, 12) == 4095);
    CHECK(dequantizeRange(0, -64, 64, 12) == -64.0);
    CHECK(dequantizeRange(4095, -64, 64, 12) == 64.0);
    CHECK(dequantizeRange(99999, -64, 64, 12) == 64.0); // out-of-range input is clamped, not extrapolated
    SplitMix64 rng(7);
    const f64 step = 128.0 / 4095.0;
    for (int i = 0; i < 10000; ++i) {
        const f64 v = -64.0 + 128.0 * static_cast<f64>(rng.next() >> 11) / 9007199254740992.0;
        CHECK(std::fabs(dequantizeRange(quantizeRange(v, -64, 64, 12), -64, 64, 12) - v) <= step / 2 + 1e-12);
    }
}

TEST_CASE("repl: smallest-three quaternions round-trip within their precision") {
    SplitMix64 rng(0x5a11);
    for (int i = 0; i < 5000; ++i) {
        f64 c[4];
        f64 n = 0;
        for (f64& x : c) {
            x = static_cast<f64>(rng.next() >> 11) / 9007199254740992.0 * 2 - 1;
            n += x * x;
        }
        n = std::sqrt(n);
        if (n < 1e-3) continue;
        const Quat q(static_cast<f32>(c[0] / n), static_cast<f32>(c[1] / n), static_cast<f32>(c[2] / n), static_cast<f32>(c[3] / n));
        BitWriter w;
        writeSmallest3(w, q, 10);
        CHECK(w.bitCount() == 32);
        BitReader r(w.bytes(), w.bitCount());
        auto back = readSmallest3(r, 10);
        REQUIRE(back);
        // q and -q are one rotation: compare |dot|.
        const f64 dot = std::fabs(f64(q.x) * back->x + f64(q.y) * back->y + f64(q.z) * back->z + f64(q.w) * back->w);
        CHECK(dot > 0.9999);
    }
    std::vector<u8> none;
    BitReader empty(none);
    CHECK_FALSE(readSmallest3(empty, 10));
    // Hostile input: three components at +1/√2 square-sum to 3/2, which no unit quaternion sends.
    BitWriter hostile;
    hostile.write(3, 2); // w is the largest
    for (int i = 0; i < 3; ++i) hostile.write((1u << 10) - 1, 10);
    BitReader hr(hostile.bytes(), hostile.bitCount());
    auto rejected = readSmallest3(hr, 10);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("exceed a unit quaternion") != std::string::npos);
    // The largest honest square sum, 3/4 (w = 1/2), still decodes to a unit quaternion.
    BitWriter edge;
    writeSmallest3(edge, Quat(0.5f, 0.5f, 0.5f, 0.5f), 10);
    BitReader er(edge.bytes(), edge.bitCount());
    auto unit = readSmallest3(er, 10);
    REQUIRE(unit);
    CHECK(std::fabs(f64(unit->x) * unit->x + f64(unit->y) * unit->y + f64(unit->z) * unit->z + f64(unit->w) * unit->w - 1.0) < 1e-3);
}

TEST_CASE("repl: readSmallest3 accepts whatever writeSmallest3 writes") {
    // The round-2 review: rounding up pushed three components past a unit quaternion (every input at
    // bits=1, 3,314 of 20,000 at bits=2), and NaN and unnormalised input did so at any width.
    for (const Quat q : {Quat(NAN, NAN, NAN, NAN), Quat(1, 1, 1, 1), Quat(0, 0, 0, 0), Quat(INFINITY, 0, 0, 0), Quat(1e-30f, 0, 0, 0)}) {
        BitWriter w;
        writeSmallest3(w, q, 10);
        BitReader r(w.bytes(), w.bitCount());
        CHECK(readSmallest3(r, 10));
    }
    auto decode = [](const Quat& q, u32 bits) {
        BitWriter w;
        writeSmallest3(w, q, bits);
        BitReader r(w.bytes(), w.bitCount());
        return readSmallest3(r, bits);
    };
    // Non-finite and zero quaternions are the identity; others are normalised first.
    for (const Quat q : {Quat(NAN, 0, 0, 1), Quat(0, 0, 0, 0)}) {
        auto id = decode(q, 10);
        REQUIRE(id);
        CHECK(std::fabs(id->w) > 0.999f);
    }
    auto half = decode(Quat(2, 2, 2, 2), 10);
    REQUIRE(half);
    CHECK(std::fabs(half->x - 0.5f) < 2e-3f);
    CHECK(std::fabs(half->w - 0.5f) < 2e-3f);
    // Every width from 2 bits up, on random unit and unnormalised quaternions.
    SplitMix64 rng(0x5a13);
    for (u32 bits = 2; bits <= 8; ++bits) {
        u32 rejected = 0;
        for (int i = 0; i < 20000; ++i) {
            f32 c[4];
            for (f32& x : c) x = static_cast<f32>(static_cast<f64>(rng.next() >> 11) / 9007199254740992.0 * 2 - 1);
            if (!decode(Quat(c[0], c[1], c[2], c[3]), bits)) ++rejected;
        }
        INFO("bits=" << bits);
        CHECK(rejected == 0);
    }
}

TEST_CASE("repl: frame-cell positions are exact to half the resolution, even at 1e13 m") {
    const f64 cell = 4096, res = 1.0 / 256;
    for (const f64 v : {0.0, 0.001, -0.001, 4095.999, 4096.0, -4096.0, 123456.789, -9876543.21, 1e13, -1e13, 1e13 + 0.5}) {
        INFO(v);
        const WorldPos p{DVec3(v, -v, v / 3)};
        BitWriter w;
        writeFrameCell(w, p, cell, res, 20);
        BitReader r(w.bytes(), w.bitCount());
        auto back = readFrameCell(r, cell, res, 20);
        REQUIRE(back);
        CHECK(std::fabs(back->local.x - p.local.x) <= res / 2);
        CHECK(std::fabs(back->local.y - p.local.y) <= res / 2);
        CHECK(std::fabs(back->local.z - p.local.z) <= res / 2);
        CHECK(r.bitsLeft() == 0);
    }
    // Non-finite input is written as 0; an offset of a whole cell or more is rejected.
    BitWriter w;
    writeFrameCell(w, WorldPos{DVec3(std::nan(""), INFINITY, 1)}, cell, res, 20);
    BitReader r(w.bytes(), w.bitCount());
    auto back = readFrameCell(r, cell, res, 20);
    REQUIRE(back);
    CHECK(back->local.x == 0.0);
    CHECK(back->local.z == 1.0);
    BitWriter bad;
    for (int axis = 0; axis < 3; ++axis) {
        bad.writeVarint(0);
        bad.write((1u << 20) - 1, 20); // 2^20 - 1 is fine: cell/res = 2^20 steps
    }
    BitReader okReader(bad.bytes(), bad.bitCount());
    CHECK(readFrameCell(okReader, cell, res, 20));
    // With cell/res = 3 steps, offsets 0..2 are fine and 3 is out of range. All three axes are
    // written, so the read reaches the check instead of failing as truncated.
    for (const u64 offset : {u64(2), u64(3)}) {
        BitWriter over;
        for (int axis = 0; axis < 3; ++axis) {
            over.writeVarint(0);
            over.write(offset, 2);
        }
        BitReader overReader(over.bytes(), over.bitCount());
        auto read = readFrameCell(overReader, 3.0, 1.0, 2);
        CHECK(read.ok() == (offset == 2));
    }
    BitWriter far;
    far.writeVarint(zigzagEncode(std::numeric_limits<i64>::max()));
    far.write(0, 20);
    BitReader farReader(far.bytes(), far.bitCount());
    CHECK_FALSE(readFrameCell(farReader, cell, res, 20));
}

TEST_CASE("repl: raw floats keep their bits and the protocol hash ignores order") {
    BitWriter w;
    writeF32(w, -0.0f);
    writeF64(w, std::nan(""));
    BitReader r(w.bytes(), w.bitCount());
    CHECK(std::signbit(*readF32(r)));
    CHECK(std::isnan(*readF64(r)));
    const u64 a[] = {1, 2, 3};
    const u64 b[] = {3, 1, 2};
    const u64 c[] = {1, 2, 4};
    CHECK(protocolHash(a) == protocolHash(b));
    CHECK(protocolHash(a) != protocolHash(c));
}

} // namespace
