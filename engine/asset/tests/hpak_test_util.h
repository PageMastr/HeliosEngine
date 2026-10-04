#pragma once
// Shared helpers for the engine/asset tests: deterministic payloads, a pak builder over assetpipe's
// writer, and a pak source whose bytes a test can change after the reader opened it.

#include <algorithm>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/asset/hpak_reader.h"
#include "helios/assetpipe/hpak_writer.h"
#include "helios/core/random.h"

namespace helios::asset::test {

/// A GUID made from a small number (deterministic test identities).
inline Guid guidOf(u64 n) { return Guid(0x4000000000004000ull | (n << 16), 0x8000000000000000ull | n); }

/// Bytes zstd shrinks well (repeating text with a counter).
inline std::vector<u8> compressible(usize size, u32 seed = 1) {
    std::vector<u8> out(size);
    const std::string_view text = "helios asset pipeline test payload ";
    for (usize i = 0; i < size; ++i)
        out[i] = static_cast<u8>(text[(i + seed) % text.size()] + (i / 4096) % 3);
    return out;
}

/// Bytes zstd cannot shrink.
inline std::vector<u8> incompressible(usize size, u64 seed = 7) {
    std::vector<u8> out(size);
    Xoshiro256 rng(seed);
    for (usize i = 0; i < size; ++i) out[i] = static_cast<u8>(rng.next() >> 56);
    return out;
}

struct TestAsset {
    Guid guid;
    std::vector<u8> bytes;
    assetpipe::HpakAssetOrder order;
};

/// Builds a pak from `assets` with `options`; fails the calling test on error.
inline std::vector<u8> buildPak(const std::vector<TestAsset>& assets,
                                const assetpipe::HpakWriterOptions& options = {}) {
    auto writer = assetpipe::HpakWriter::create(options);
    HELIOS_VERIFY(writer.ok(), "HpakWriter::create failed");
    for (const TestAsset& a : assets)
        HELIOS_VERIFY(writer->add(a.guid, a.bytes, a.order).ok(), "HpakWriter::add failed");
    auto bytes = writer->build();
    HELIOS_VERIFY(bytes.ok(), "HpakWriter::build failed");
    return std::move(bytes).value();
}

/// Opens an in-memory copy of `pak`.
inline Result<std::shared_ptr<HpakReader>> openPak(std::vector<u8> pak, const HpakOpenOptions& options = {}) {
    return HpakReader::open(makeHpakMemorySource(std::move(pak), "test"), options);
}

/// A source over bytes the test may change after open (to corrupt or repair blocks).
class MutableSource final : public IHpakSource {
public:
    explicit MutableSource(std::vector<u8> bytes) : m_bytes(std::move(bytes)) {}
    u64 size() const override { return m_bytes.size(); }
    Result<void> readAt(u64 offset, std::span<u8> out) const override {
        std::lock_guard lock(m_mutex);
        if (offset > m_bytes.size() || out.size() > m_bytes.size() - offset)
            return Error{ErrorCode::EndOfFile};
        std::copy_n(m_bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        ++m_reads;
        return {};
    }
    std::string describe() const override { return "mutable"; }
    void set(u64 offset, u8 value) {
        std::lock_guard lock(m_mutex);
        m_bytes[offset] = value;
    }
    u8 get(u64 offset) const {
        std::lock_guard lock(m_mutex);
        return m_bytes[offset];
    }
    u64 reads() const {
        std::lock_guard lock(m_mutex);
        return m_reads;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<u8> m_bytes;
    mutable u64 m_reads = 0;
};

} // namespace helios::asset::test
