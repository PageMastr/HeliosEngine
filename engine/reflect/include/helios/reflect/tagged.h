#pragma once
// Tagged binary format (02 §3.7): protobuf wire format with schema-lock field IDs as field numbers.
// It is compact, versioned by construction (readers skip unknown fields and keep defaults for
// missing ones) and needs no libprotobuf; the Go generator emits a byte-identical codec.
//
// Encoding summary (normative; see tools/schemac/README.md "Wire format"):
//   bool, u8..u64, enums, flags, NetHandle, Tick   VARINT (LEB128)
//   i8..i64, Duration                              VARINT, zigzag
//   f32                                            I32 (little-endian IEEE-754)
//   f64                                            I64 (readers also accept I32 and widen)
//   EntityId, RecordRef                            I64 (fixed 64-bit)
//   string, Name, LocString, TagQuery, HxlExpr     LEN (UTF-8)
//   Guid, AssetRef                                 LEN, 16 bytes (canonical big-endian byte order)
//   vecNf/quatf/color, vec3d/quatd/WorldPos        LEN, packed f32/f64 components
//   struct / message / TagSet                      LEN, nested message
//   list<scalar>                                   LEN, packed
//   list<LEN type>                                 one field occurrence per element
//   list<list|optional|map> element                LEN wrapper message {1: element}
//   map<K,V>                                       one LEN entry {1: key, 2: value} per element, key order
//   KeyedList<T>                                   one LEN entry {1: key guid, 2: value} per element
//   variant                                        LEN message with exactly one field (alternative id)
// Struct fields equal to their schema default are omitted; readers start from default values.
//
// Threading: writers/readers are single-threaded cursors over caller-owned buffers.

#include <span>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::refl {

enum class WireType : u8 {
    Varint = 0,
    I64 = 1,
    Len = 2,
    I32 = 5,
};

std::string_view wireTypeName(WireType type) noexcept;

/// Largest valid field number (protobuf limit).
inline constexpr u32 kMaxFieldId = (1u << 29) - 1;
/// Nesting limit for readers (hostile input must not overflow the stack).
inline constexpr u32 kMaxTaggedDepth = 64;

constexpr u64 zigzagEncode(i64 v) noexcept {
    return (static_cast<u64>(v) << 1) ^ static_cast<u64>(v >> 63);
}
constexpr i64 zigzagDecode(u64 v) noexcept {
    return static_cast<i64>(v >> 1) ^ -static_cast<i64>(v & 1);
}

/// Appends tagged data to a byte vector.
class TaggedWriter {
public:
    explicit TaggedWriter(std::vector<u8>& out) noexcept : m_out(&out) {}

    void writeVarint(u64 v);
    void writeZigZag(i64 v) { writeVarint(zigzagEncode(v)); }
    void writeFixed32(u32 v);
    void writeFixed64(u64 v);
    void writeF32(f32 v);
    void writeF64(f64 v);
    void writeTag(u32 fieldId, WireType type);
    void writeRaw(const void* data, usize size);
    /// Length prefix + bytes.
    void writeLenBytes(const void* data, usize size);
    void writeString(std::string_view s) { writeLenBytes(s.data(), s.size()); }

    /// Starts a length-delimited payload whose size is not known yet. Returns a marker for endLen().
    usize beginLen();
    /// Patches the length prefix of the payload started at `marker` (minimal varint length).
    void endLen(usize marker);

    std::vector<u8>& buffer() noexcept { return *m_out; }
    usize size() const noexcept { return m_out->size(); }

private:
    std::vector<u8>* m_out;
};

struct FieldTag {
    u32 id = 0;
    WireType wire = WireType::Varint;
};

/// Bounds-checked reader over one message (or one packed/LEN payload). Every read reports
/// truncated or malformed input as ErrorCode::Corrupt instead of asserting.
class TaggedReader {
public:
    TaggedReader() noexcept = default;
    explicit TaggedReader(std::span<const u8> data, u32 depth = 0) noexcept : m_data(data), m_depth(depth) {}

    bool atEnd() const noexcept { return m_pos >= m_data.size(); }
    usize position() const noexcept { return m_pos; }
    usize remaining() const noexcept { return m_data.size() - m_pos; }
    u32 depth() const noexcept { return m_depth; }

    Result<FieldTag> readTag();
    Result<u64> readVarint();
    Result<i64> readZigZag();
    Result<u32> readFixed32();
    Result<u64> readFixed64();
    Result<f32> readF32();
    Result<f64> readF64();
    /// Length prefix + payload view.
    Result<std::span<const u8>> readLen();
    /// Length-delimited nested message; fails beyond kMaxTaggedDepth.
    Result<TaggedReader> readMessage();
    /// Skips one value of the given wire type.
    Result<void> skip(WireType type);

private:
    std::span<const u8> m_data;
    usize m_pos = 0;
    u32 m_depth = 0;
};

/// Error helper used by codecs: "<what>: wire type <got>, expected <expected>".
Error wireTypeMismatch(std::string_view what, WireType got, WireType expected);

} // namespace helios::refl
