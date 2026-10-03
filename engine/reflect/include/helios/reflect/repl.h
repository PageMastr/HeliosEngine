#pragma once
// Replication descriptors and the Phase 0 full-state codec (02 §3.5 `--emit repl`; 04 §4.1, §4.5;
// 04 §11.3: Phase 0 is "descriptors, full state"). helios-schemac generates, per replicated component,
// a ComponentRepDesc (audience, LOD group, and per replicated field its lock id, offset, change-mask
// index, LOD, prediction and quantizer) plus typed writeFullState/readFullState functions built on the
// helpers here. Change masks, deltas, interest and priority are WP-1.10 and later.
//
// Every encoding is a pure function of the value (no platform state): floats are quantized with
// f64 arithmetic and round-half-up, so a cell and a client produce the same bits (04 §4.5, "both sides
// quantize").
//
// Threading: descriptors are immutable; BitWriter/BitReader are single-threaded values.

#include <algorithm>
#include <span>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/math/quat.h"
#include "helios/math/vec.h"
#include "helios/reflect/type_info.h"
#include "helios/reflect/types.h"

namespace helios::refl::repl {

/// How a field travels in full state. Raw: its bits as stored (floats as IEEE bits, integers at their
/// width). Range: each float component of [min, max] onto `bits` bits (clamped). Smallest3: a unit
/// quaternion as the index of its largest component (2 bits) plus the other three in ±1/√2 at `bits`
/// bits each. FrameCell: a WorldPos rounded to `res` metres, per axis as a zigzag varint index of its
/// `cell`-metre cell plus the offset inside it in `bits` bits (cell/res steps, 04 §4.5).
enum class Quant : u8 { Raw, Range, Smallest3, FrameCell };
enum class Lod : u8 { Core, Near };
enum class Interp : u8 { None, Linear, Slerp };

struct Quantizer {
    Quant kind = Quant::Raw;
    u8 bits = 0;  ///< Range / Smallest3 bits per component; FrameCell offset bits per axis
    f64 min = 0;  ///< Range
    f64 max = 0;  ///< Range
    f64 cell = 0; ///< FrameCell cell size [m]
    f64 res = 0;  ///< FrameCell resolution [m]
};

/// One replicated field.
struct FieldRep {
    std::string_view name;
    u32 fieldId = 0;   ///< lock id
    u32 offset = 0;    ///< byte offset in the component
    u8 repIndex = 0;   ///< dirty bit and change-mask index (FieldInfo::repIndex)
    Lod lod = Lod::Core;
    bool predicted = false;
    Interp interp = Interp::None;
    Quantizer quant;
    u32 maxBits = 0;   ///< worst-case full-state bits of the field
};

struct ComponentRepDesc {
    std::string_view name; ///< qualified schema name
    TypeId typeId = 0;
    Audience audience = Audience::None;
    Lod lod = Lod::Core;
    std::span<const FieldRep> fields; ///< in repIndex order
    u32 maxFullStateBits = 0;
    u64 hash = 0; ///< over the qualified name, type id, audience, LOD and every field's lock id, name, type,
                  ///< change-mask index, LOD, prediction, interpolation, quantizer and worst-case bits, plus
                  ///< the underlying types and values of the enums and flags the fields use; enters the
                  ///< protocol hash
};

enum class RpcDirection : u8 { ClientToServer, ServerToClient, ServerToServer };

/// A top-level rpc (04 §4.6): direction, reliability and the SEC-1 classification.
struct RpcRep {
    std::string_view name;
    TypeId argsTypeId = 0; ///< the generated argument struct
    RpcDirection direction = RpcDirection::ClientToServer;
    bool reliable = true;
    f64 ratePerSecond = 0; ///< @ratelimit (0 = none)
    std::string_view intent;
};

enum class EventAudience : u8 { Owner, Relevant, Party };

/// An entity-scoped event (04 §4.6).
struct EventRep {
    std::string_view name;
    TypeId typeId = 0;
    EventAudience audience = EventAudience::Relevant;
    bool reliable = true; ///< EVENT_R; false (`@unreliable`) is EVENT_U (04 §2.2)
};

/// The replication tables of one schema file (generated `<stem>Replication()`).
struct FileRepTables {
    std::span<const ComponentRepDesc* const> components;
    std::span<const RpcRep> rpcs;
    std::span<const EventRep> events;
    u64 protocolHash = 0; ///< protocolHash() over the descriptor, rpc and event hashes of the file
};

/// Generated for every replicated component C (`<file>.repl.gen.h`): `static const ComponentRepDesc&
/// desc()`, `static void writeFullState(BitWriter&, const C&)` and `static Result<void>
/// readFullState(BitReader&, C&)`.
template <class C>
struct RepOf;

/// Appends bits least-significant first. Never fails; the buffer grows.
class BitWriter {
public:
    /// Writes the low `bits` (0–64) bits of `value`.
    void write(u64 value, u32 bits);
    void writeBool(bool v) { write(v ? 1 : 0, 1); }
    /// Unsigned LEB128, 8 bits per group.
    void writeVarint(u64 value);
    usize bitCount() const noexcept { return m_bits; }
    const std::vector<u8>& bytes() const noexcept { return m_bytes; }

private:
    std::vector<u8> m_bytes;
    usize m_bits = 0;
};

/// Reads what BitWriter wrote; every read is bounds-checked (hostile input fails, never overreads).
class BitReader {
public:
    explicit BitReader(std::span<const u8> bytes, usize bitCount) noexcept : m_bytes(bytes), m_limit(std::min(bitCount, bytes.size() * 8)) {}
    explicit BitReader(std::span<const u8> bytes) noexcept : BitReader(bytes, bytes.size() * 8) {}
    Result<u64> read(u32 bits);
    Result<bool> readBool();
    /// At most 10 groups; longer or overlong encodings fail.
    Result<u64> readVarint();
    usize bitsLeft() const noexcept { return m_limit - m_pos; }

private:
    std::span<const u8> m_bytes;
    usize m_limit = 0;
    usize m_pos = 0;
};

// --- quantizers (the generated codecs call these) -----------------------------------------------

/// `v` clamped to [min, max] on 2^bits - 1 steps (NaN -> min).
u64 quantizeRange(f64 v, f64 min, f64 max, u32 bits) noexcept;
f64 dequantizeRange(u64 q, f64 min, f64 max, u32 bits) noexcept;

/// `q` normalised (in f64) as the index of its largest component plus the other three at `bits`
/// (2–32, asserted; helios-schemac requires 3–32) bits each. A quaternion that is not finite or is
/// near zero is sent as the identity, and a component that rounding would push past a unit
/// quaternion is stepped toward 0, so readSmallest3 always accepts what this writes. Re-encoding a
/// decoded rotation can give other bits (the largest index flips when two components are within a
/// step, and above 24 bits the f32 result is coarser than a step), though it decodes to the same
/// rotation within one step.
void writeSmallest3(BitWriter& w, const Quat& q, u32 bits);
/// Fails on truncated input and on three components whose squares sum past 1 (no unit quaternion).
Result<Quat> readSmallest3(BitReader& r, u32 bits);

/// Each axis rounded to `res` and saturated at ±4e18 steps of `res` (a non-finite axis is sent as 0).
/// readFrameCell accepts exactly that range, so it reads whatever this writes at any cell size, and
/// fails on an offset of a whole cell or more and on a position beyond ±4e18 steps.
void writeFrameCell(BitWriter& w, const WorldPos& p, f64 cell, f64 res, u32 bits);
Result<WorldPos> readFrameCell(BitReader& r, f64 cell, f64 res, u32 bits);

/// Raw IEEE bits.
void writeF32(BitWriter& w, f32 v);
Result<f32> readF32(BitReader& r);
void writeF64(BitWriter& w, f64 v);
Result<f64> readF64(BitReader& r);

/// Combines hashes (descriptors, rpc and event tables, or files' protocol hashes) into one,
/// independent of their order: FNV-1a over the sorted values. helios-schemac computes the files'
/// hashes the same way.
u64 protocolHash(std::span<const u64> hashes);

} // namespace helios::refl::repl
