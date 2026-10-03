// Replication full-state helpers (helios/reflect/repl.h).

#include "helios/reflect/repl.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "helios/core/assert.h"

namespace helios::refl::repl {

namespace {

Error truncated() { return Error(ErrorCode::Corrupt, "replication full state: truncated"); }

u64 mask(u32 bits) noexcept { return bits >= 64 ? ~u64(0) : (u64(1) << bits) - 1; }

/// Round half up in f64: deterministic for the same inputs on every IEEE platform.
u64 roundToU64(f64 x) noexcept { return static_cast<u64>(std::floor(x + 0.5)); }

constexpr f64 kSmallest3Max = 0.70710678118654752440; // 1/√2: bound of the three smaller components
constexpr f64 kSmallest3Margin = 1e-9;                  // writer's margin below the reader's sum limit

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
    // At 1 bit the stepping loop below cannot bring the three under unit length, so readSmallest3
    // would reject what this writes (round-3 review: all of 1,000). Generated code passes 3 to 32.
    HELIOS_ASSERT(bits >= 2 && bits <= 32, "writeSmallest3 takes 2 to 32 bits, not {}", bits);
    f64 c[4] = {q.x, q.y, q.z, q.w};
    // Normalise in f64 so that what is sent is a unit quaternion; one that is not finite or has no
    // direction (NaN from a physics blow-up, all zero) is sent as the identity rather than as three
    // components no unit quaternion has, which readSmallest3 rejects.
    const f64 norm = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2] + c[3] * c[3]);
    if (!std::isfinite(norm) || norm < 1e-12) {
        c[0] = c[1] = c[2] = 0;
        c[3] = 1;
    } else {
        for (f64& x : c) x /= norm;
    }
    u32 largest = 0;
    for (u32 i = 1; i < 4; ++i) {
        if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
    }
    const f64 sign = c[largest] < 0 ? -1.0 : 1.0; // q and -q are the same rotation: send the positive one
    u64 v[3];
    u32 n = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (i != largest) v[n++] = quantizeRange(c[i] * sign, -kSmallest3Max, kSmallest3Max, bits);
    }
    // Rounding up can push the three squares past 1 (it does at 1 or 2 bits), which the reader rejects
    // as corrupt: step the largest of them one step toward 0 until they fit, a margin below the reader's
    // limit so that a differently contracted sum cannot disagree. With bits >= 2 that always ends, as
    // the steps nearest 0 square-sum to < 1/6.
    const u64 below = mask(bits) / 2; // the steps nearest 0 (2^bits - 1 steps: none is 0 itself)
    const u64 above = below + 1;
    for (;;) {
        f64 sum = 0;
        u32 far = 3;
        f64 farthest = 0;
        for (u32 k = 0; k < 3; ++k) {
            const f64 d = dequantizeRange(v[k], -kSmallest3Max, kSmallest3Max, bits);
            sum += d * d;
            if ((v[k] < below || v[k] > above) && std::fabs(d) > farthest) {
                farthest = std::fabs(d);
                far = k;
            }
        }
        if (sum <= 1.0 - kSmallest3Margin || far == 3) break;
        v[far] = v[far] >= above ? v[far] - 1 : v[far] + 1;
    }
    w.write(largest, 2);
    for (const u64 x : v) w.write(x, bits);
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

// The writer saturates a position at ±kMaxSteps steps of `res`, and the reader accepts exactly that
// range of index * steps + offset, so whatever one writes the other reads at every cell size. Both are
// 4e18 exactly (2^20 * 5^18 < 2^62), so the f64 clamp and the i64 check agree.
constexpr i64 kMaxStepsI = 4'000'000'000'000'000'000;
constexpr f64 kMaxSteps = static_cast<f64>(kMaxStepsI);
static_assert(static_cast<i64>(kMaxSteps) == kMaxStepsI);

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
        // Bound the index first so that i * steps + offset cannot overflow i64, then check the total,
        // not i * steps: the writer floors the index, so for a total near -4e18 index * steps lies up
        // to steps - 1 below it whenever steps does not divide 4e18 (2^21 steps, for one).
        if (*offset >= static_cast<u64>(steps) || i < -(kMaxStepsI / steps) - 1 || i > kMaxStepsI / steps)
            return Error(ErrorCode::Corrupt, "replication full state: frame cell position out of range");
        const i64 total = i * steps + static_cast<i64>(*offset);
        if (total < -kMaxStepsI || total > kMaxStepsI)
            return Error(ErrorCode::Corrupt, "replication full state: frame cell position out of range");
        v = static_cast<f64>(total) * res;
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
