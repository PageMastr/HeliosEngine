// Tagged (protobuf wire format) writer and bounds-checked reader.

#include "helios/reflect/tagged.h"

#include <bit>
#include <cstring>
#include <format>

namespace helios::refl {

std::string_view wireTypeName(WireType type) noexcept {
    switch (type) {
    case WireType::Varint: return "VARINT";
    case WireType::I64: return "I64";
    case WireType::Len: return "LEN";
    case WireType::I32: return "I32";
    }
    return "?";
}

Error wireTypeMismatch(std::string_view what, WireType got, WireType expected) {
    return makeError(ErrorCode::Corrupt, "{}: wire type {}, expected {}", what, wireTypeName(got), wireTypeName(expected));
}

namespace {
usize varintSize(u64 v) noexcept {
    usize n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}
} // namespace

void TaggedWriter::writeVarint(u64 v) {
    u8 buf[10];
    usize n = 0;
    while (v >= 0x80) {
        buf[n++] = static_cast<u8>(v | 0x80);
        v >>= 7;
    }
    buf[n++] = static_cast<u8>(v);
    m_out->insert(m_out->end(), buf, buf + n);
}

void TaggedWriter::writeFixed32(u32 v) {
    u8 buf[4];
    storeLE<u32>(buf, v);
    m_out->insert(m_out->end(), buf, buf + 4);
}

void TaggedWriter::writeFixed64(u64 v) {
    u8 buf[8];
    storeLE<u64>(buf, v);
    m_out->insert(m_out->end(), buf, buf + 8);
}

void TaggedWriter::writeF32(f32 v) { writeFixed32(std::bit_cast<u32>(v)); }
void TaggedWriter::writeF64(f64 v) { writeFixed64(std::bit_cast<u64>(v)); }

void TaggedWriter::writeTag(u32 fieldId, WireType type) {
    HELIOS_ASSERT(fieldId >= 1 && fieldId <= kMaxFieldId, "invalid tagged field id {}", fieldId);
    writeVarint((static_cast<u64>(fieldId) << 3) | static_cast<u64>(type));
}

void TaggedWriter::writeRaw(const void* data, usize size) {
    const auto* p = static_cast<const u8*>(data);
    m_out->insert(m_out->end(), p, p + size);
}

void TaggedWriter::writeLenBytes(const void* data, usize size) {
    writeVarint(size);
    writeRaw(data, size);
}

usize TaggedWriter::beginLen() {
    m_out->push_back(0); // one-byte placeholder; endLen() widens it if needed
    return m_out->size();
}

void TaggedWriter::endLen(usize marker) {
    HELIOS_ASSERT(marker >= 1 && marker <= m_out->size());
    const u64 len = m_out->size() - marker;
    const usize n = varintSize(len);
    if (n > 1) m_out->insert(m_out->begin() + static_cast<isize>(marker), n - 1, u8{0});
    u8* p = m_out->data() + (marker - 1);
    u64 v = len;
    for (usize i = 0; i < n; ++i) {
        p[i] = static_cast<u8>((v & 0x7F) | (i + 1 < n ? 0x80 : 0));
        v >>= 7;
    }
}

Result<u64> TaggedReader::readVarint() {
    u64 result = 0;
    for (u32 shift = 0; shift < 64; shift += 7) {
        if (m_pos >= m_data.size()) return Error{ErrorCode::Corrupt, "truncated varint"};
        const u8 b = m_data[m_pos++];
        if (shift == 63 && b > 1) return Error{ErrorCode::Corrupt, "varint overflows 64 bits"};
        result |= static_cast<u64>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return result;
    }
    return Error{ErrorCode::Corrupt, "varint longer than 10 bytes"};
}

Result<i64> TaggedReader::readZigZag() {
    HELIOS_TRY_ASSIGN(const u64 v, readVarint());
    return zigzagDecode(v);
}

Result<u32> TaggedReader::readFixed32() {
    if (remaining() < 4) return Error{ErrorCode::Corrupt, "truncated fixed32"};
    const u32 v = loadLE<u32>(m_data.data() + m_pos);
    m_pos += 4;
    return v;
}

Result<u64> TaggedReader::readFixed64() {
    if (remaining() < 8) return Error{ErrorCode::Corrupt, "truncated fixed64"};
    const u64 v = loadLE<u64>(m_data.data() + m_pos);
    m_pos += 8;
    return v;
}

Result<f32> TaggedReader::readF32() {
    HELIOS_TRY_ASSIGN(const u32 v, readFixed32());
    return std::bit_cast<f32>(v);
}

Result<f64> TaggedReader::readF64() {
    HELIOS_TRY_ASSIGN(const u64 v, readFixed64());
    return std::bit_cast<f64>(v);
}

Result<FieldTag> TaggedReader::readTag() {
    HELIOS_TRY_ASSIGN(const u64 key, readVarint());
    const u64 id = key >> 3;
    const u32 wire = static_cast<u32>(key & 7);
    if (id == 0 || id > kMaxFieldId) return makeError(ErrorCode::Corrupt, "invalid field number {}", id);
    if (wire != 0 && wire != 1 && wire != 2 && wire != 5)
        return makeError(ErrorCode::Corrupt, "unsupported wire type {} (field {})", wire, id);
    return FieldTag{static_cast<u32>(id), static_cast<WireType>(wire)};
}

Result<std::span<const u8>> TaggedReader::readLen() {
    HELIOS_TRY_ASSIGN(const u64 len, readVarint());
    if (len > remaining()) return makeError(ErrorCode::Corrupt, "length {} exceeds remaining {} bytes", len, remaining());
    const std::span<const u8> out = m_data.subspan(m_pos, static_cast<usize>(len));
    m_pos += static_cast<usize>(len);
    return out;
}

Result<TaggedReader> TaggedReader::readMessage() {
    if (m_depth + 1 > kMaxTaggedDepth) return Error{ErrorCode::LimitExceeded, "tagged nesting too deep"};
    HELIOS_TRY_ASSIGN(const auto payload, readLen());
    return TaggedReader(payload, m_depth + 1);
}

Result<void> TaggedReader::skip(WireType type) {
    switch (type) {
    case WireType::Varint: {
        HELIOS_TRY(readVarint());
        return {};
    }
    case WireType::I64:
        if (remaining() < 8) return Error{ErrorCode::Corrupt, "truncated fixed64"};
        m_pos += 8;
        return {};
    case WireType::I32:
        if (remaining() < 4) return Error{ErrorCode::Corrupt, "truncated fixed32"};
        m_pos += 4;
        return {};
    case WireType::Len: {
        HELIOS_TRY(readLen());
        return {};
    }
    }
    return Error{ErrorCode::Corrupt, "unsupported wire type"};
}

} // namespace helios::refl
