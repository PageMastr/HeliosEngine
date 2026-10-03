// Replication full-state helpers (helios/reflect/repl.h).

#include "helios/reflect/repl.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace helios::refl::repl {

namespace {

Error truncated() { return Error(ErrorCode::Corrupt, "replication full state: truncated"); }

u64 mask(u32 bits) noexcept { return bits >= 64 ? ~u64(0) : (u64(1) << bits) - 1; }

/// Round half up in f64: deterministic for the same inputs on every IEEE platform.
u64 roundToU64(f64 x) noexcept { return static_cast<u64>(std::floor(x + 0.5)); }

constexpr f64 kSmallest3Max = 0.70710678118654752440; // 1/√2: bound of the three smaller components

} // namespace

void BitWriter::write(u64 value, u32 bits) {
    value &= mask(bits);
    while (bits > 0) {
        const usize bitInByte = m_bits & 7;
        if (bitInByte == 0) m_bytes.push_back(0);
        const u32 take = std::min<u32>(bits, 8 - static_cast<u32>(bitInByte));
        m_bytes.back() = static_cast<u8>(m_bytes.back() | ((value & mask(take)) << bitInByte));
        value >>= take;
        bits -= take;
        m_bits += take;
    }
}

void BitWriter::writeVarint(u64 value) {
    do {
        const u64 group = value & 0x7F;
        value >>= 7;
        write(group | (value != 0 ? 0x80 : 0), 8);
    } while (value != 0);
}

Result<u64> BitReader::read(u32 bits) {
    if (bits > 64 || bits > bitsLeft()) return truncated();
    u64 out = 0;
    u32 done = 0;
    while (done < bits) {
        const usize bitInByte = m_pos & 7;
        const u32 take = std::min<u32>(bits - done, 8 - static_cast<u32>(bitInByte));
        const u64 chunk = (static_cast<u64>(m_bytes[m_pos >> 3]) >> bitInByte) & mask(take);
        out |= chunk << done;
        done += take;
        m_pos += take;
    }
    return out;
}

Result<bool> BitReader::readBool() {
    auto v = read(1);
    if (!v) return v.error();
    return *v != 0;
}

Result<u64> BitReader::readVarint() {
    u64 out = 0;
    for (u32 i = 0; i < 10; ++i) {
        auto group = read(8);
        if (!group) return group.error();
        if (i == 9 && (*group & 0x7E) != 0) return Error(ErrorCode::Corrupt, "replication full state: varint overflows 64 bits");
        out |= (*group & 0x7F) << (7 * i);
        if ((*group & 0x80) == 0) {
            if (i > 0 && (*group & 0x7F) == 0) return Error(ErrorCode::Corrupt, "replication full state: overlong varint");
            return out;
        }
    }
    return Error(ErrorCode::Corrupt, "replication full state: varint longer than 10 bytes");
}

u64 quantizeRange(f64 v, f64 min, f64 max, u32 bits) noexcept {
    const u64 steps = mask(bits);
    if (!(v > min)) return 0; // (NaN too)
    if (v >= max) return steps;
    return std::min(roundToU64((v - min) / (max - min) * static_cast<f64>(steps)), steps);
}

f64 dequantizeRange(u64 q, f64 min, f64 max, u32 bits) noexcept {
    const u64 steps = mask(bits);
    return min + (max - min) * static_cast<f64>(std::min(q, steps)) / static_cast<f64>(steps);
}

void writeSmallest3(BitWriter& w, const Quat& q, u32 bits) {
    f64 c[4] = {q.x, q.y, q.z, q.w};
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
    }
    const f64 sign = c[largest] < 0 ? -1.0 : 1.0; // q and -q are the same rotation: send the positive one
    w.write(largest, 2);
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) w.write(quantizeRange(c[i] * sign, -kSmallest3Max, kSmallest3Max, bits), bits);
    }
}

Result<Quat> readSmallest3(BitReader& r, u32 bits) {
    auto largest = r.read(2);
    if (!largest) return largest.error();
    f64 c[4] = {};
    f64 sum = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (i == *largest) continue;
        auto q = r.read(bits);
        if (!q) return q.error();
        c[i] = dequantizeRange(*q, -kSmallest3Max, kSmallest3Max, bits);
        sum += c[i] * c[i];
    }
    // An encoder sends the three smallest components of a unit quaternion, so their squares sum to at
    // most 3/4 (plus rounding); more than 1 cannot be a unit quaternion and is corrupt input.
    if (sum > 1.0) return Error(ErrorCode::Corrupt, "replication full state: smallest-three components exceed a unit quaternion");
    c[*largest] = std::sqrt(std::max(0.0, 1.0 - sum));
    return Quat(static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2]), static_cast<f32>(c[3]));
}

namespace {

/// Steps of `res` in one cell (validated by helios-schemac: a whole number from 2 to 2^32).
i64 stepsPerCell(f64 cell, f64 res) noexcept { return std::max<i64>(1, static_cast<i64>(std::floor(cell / res + 0.5))); }

constexpr f64 kMaxSteps = 4.0e18; // < 2^62: index * steps + offset stays in i64

i64 floorDiv(i64 a, i64 b) noexcept { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

} // namespace

void writeFrameCell(BitWriter& w, const WorldPos& p, f64 cell, f64 res, u32 bits) {
    const i64 steps = stepsPerCell(cell, res);
    for (const f64 v : {p.local.x, p.local.y, p.local.z}) {
        const f64 n = std::isfinite(v) ? std::clamp(std::floor(v / res + 0.5), -kMaxSteps, kMaxSteps) : 0.0;
        const i64 total = static_cast<i64>(n);
        const i64 index = floorDiv(total, steps);
        w.writeVarint(zigzagEncode(index));
        w.write(static_cast<u64>(total - index * steps), bits);
    }
}

Result<WorldPos> readFrameCell(BitReader& r, f64 cell, f64 res, u32 bits) {
    const i64 steps = stepsPerCell(cell, res);
    f64 axes[3] = {};
    for (f64& v : axes) {
        auto index = r.readVarint();
        if (!index) return index.error();
        auto offset = r.read(bits);
        if (!offset) return offset.error();
        const i64 i = zigzagDecode(*index);
        if (*offset >= static_cast<u64>(steps) || std::fabs(static_cast<f64>(i) * static_cast<f64>(steps)) > kMaxSteps)
            return Error(ErrorCode::Corrupt, "replication full state: frame cell position out of range");
        v = static_cast<f64>(i * steps + static_cast<i64>(*offset)) * res;
    }
    return WorldPos{DVec3(axes[0], axes[1], axes[2])};
}

void writeF32(BitWriter& w, f32 v) { w.write(std::bit_cast<u32>(v), 32); }
Result<f32> readF32(BitReader& r) {
    auto v = r.read(32);
    if (!v) return v.error();
    return std::bit_cast<f32>(static_cast<u32>(*v));
}
void writeF64(BitWriter& w, f64 v) { w.write(std::bit_cast<u64>(v), 64); }
Result<f64> readF64(BitReader& r) {
    auto v = r.read(64);
    if (!v) return v.error();
    return std::bit_cast<f64>(*v);
}

u64 protocolHash(std::span<const u64> hashes) {
    std::vector<u64> sorted(hashes.begin(), hashes.end());
    std::sort(sorted.begin(), sorted.end());
    u64 h = 0xcbf29ce484222325ull;
    for (const u64 d : sorted) {
        for (u32 i = 0; i < 8; ++i) {
            h ^= (d >> (8 * i)) & 0xFF;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

} // namespace helios::refl::repl
