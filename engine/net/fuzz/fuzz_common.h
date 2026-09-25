#pragma once
// Shared declarations for engine/net fuzz targets.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

/// Built-in seed inputs of the target (written to fuzz/corpus/<target>/ with --make-seeds).
void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out);

namespace helios::net::fuzz {

/// Splits an input into records of [u16 little-endian length][bytes]; a short tail is one record.
inline std::vector<std::span<const uint8_t>> splitRecords(const uint8_t* data, size_t size, size_t maxRecord = 4096) {
    std::vector<std::span<const uint8_t>> out;
    size_t pos = 0;
    while (pos < size) {
        if (size - pos < 2) {
            out.emplace_back(data + pos, size - pos);
            break;
        }
        size_t len = static_cast<size_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        len = std::min({len, size - pos, maxRecord});
        out.emplace_back(data + pos, len);
        pos += len;
    }
    return out;
}

inline void appendRecord(std::vector<uint8_t>& out, std::span<const uint8_t> record) {
    const size_t len = std::min<size_t>(record.size(), 0xFFFF);
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.insert(out.end(), record.begin(), record.begin() + static_cast<std::ptrdiff_t>(len));
}

} // namespace helios::net::fuzz
