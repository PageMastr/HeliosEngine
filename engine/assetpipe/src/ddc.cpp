// Derived-data cache v0: key, entry format and the local store (see ddc.h).

#include "helios/assetpipe/ddc.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "helios/core/guid.h"
#include "helios/core/log.h"

namespace helios::assetpipe {

namespace {

constexpr u32 kKeyMagic = 0x4B444448u; // "HDDK" as little-endian bytes
constexpr u32 kKeyFormat = 0;
constexpr usize kKeyFixedBytes = 80;
constexpr std::string_view kTempMarker = ".tmp-";

void storeHash(u8* p, const Hash128& h) noexcept {
    storeLE<u64>(p, h.low);
    storeLE<u64>(p + 8, h.high);
}

Hash128 loadHash(const u8* p) noexcept {
    return Hash128{loadLE<u64>(p), loadLE<u64>(p + 8)};
}

bool hexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/// "<32 lower-case hex digits>.hddc": the only file names the store reads as entries or deletes.
bool isEntryName(std::string_view name) noexcept {
    return name.size() == 32 + ddc::kEntryExtension.size() && name.ends_with(ddc::kEntryExtension) &&
           std::all_of(name.begin(), name.begin() + 32, hexDigit);
}

/// "<entry name>.tmp-<guid>": a writer's temp file.
bool isTempName(std::string_view name) noexcept {
    const usize at = name.find(kTempMarker);
    return at != std::string_view::npos && isEntryName(name.substr(0, at)) &&
           Guid::parse(name.substr(at + kTempMarker.size())).ok();
}

bool isFanoutDir(std::string_view name) noexcept {
    return name.size() == 2 && hexDigit(name[0]) && hexDigit(name[1]);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------------------------

Hash128 makeDdcKey(const DdcKeyInputs& in) noexcept {
    u8 fixed[kKeyFixedBytes] = {};
    storeLE<u32>(fixed + 0, kKeyMagic);
    storeLE<u32>(fixed + 4, kKeyFormat);
    storeLE<u32>(fixed + 8, in.builderVersion);
    storeLE<u32>(fixed + 12, in.cookerVersion);
    storeLE<u32>(fixed + 16, static_cast<u32>(in.platform));
    storeHash(fixed + 24, in.sourceHash);
    storeHash(fixed + 40, hash128(in.settings));
    storeLE<u64>(fixed + 56, in.settingsLayout);
    storeLE<u64>(fixed + 64, static_cast<u64>(in.builder.size()));
    storeLE<u64>(fixed + 72, static_cast<u64>(in.dependencies.size()));
    Hasher128 h;
    h.update(fixed, sizeof(fixed));
    h.update(in.builder);
    for (const Hash128& dep : in.dependencies) {
        u8 bytes[16];
        storeHash(bytes, dep);
        h.update(bytes, sizeof(bytes));
    }
    return h.digest();
}

Result<Hash128> hashSourceFile(const fs::Path& path) {
    HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(path, fs::OpenMode::Read));
    std::vector<u8> buffer(1 * kMiB);
    Hasher128 h;
    for (;;) {
        HELIOS_TRY_ASSIGN(const usize got, file.read(buffer.data(), buffer.size()));
        if (got == 0) break;
        h.update(buffer.data(), got);
    }
    return h.digest();
}

// ---------------------------------------------------------------------------------------------
// Entry format
// ---------------------------------------------------------------------------------------------

namespace ddc {

void encodeEntryHeader(const Hash128& key, std::span<const u8> payload,
                       std::span<u8, kEntryHeaderBytes> out) noexcept {
    u8* p = out.data();
    std::memset(p, 0, kEntryHeaderBytes);
    storeLE<u32>(p + 0, kEntryMagic);
    storeLE<u16>(p + 4, kEntryVersion);
    storeLE<u16>(p + 6, static_cast<u16>(kEntryHeaderBytes));
    storeHash(p + 8, key);
    storeLE<u64>(p + 24, static_cast<u64>(payload.size()));
    storeHash(p + 32, hash128(payload.data(), payload.size()));
    storeLE<u64>(p + 56, hash64(p, kEntryHeaderHashedBytes));
}

Result<std::vector<u8>> encodeEntry(const Hash128& key, std::span<const u8> payload) {
    if (payload.size() > kMaxPayload) {
        return makeError(ErrorCode::LimitExceeded, "DDC payload of {} bytes (at most {})", payload.size(),
                         kMaxPayload);
    }
    std::vector<u8> out(kEntryHeaderBytes + payload.size());
    encodeEntryHeader(key, payload, std::span<u8, kEntryHeaderBytes>(out.data(), kEntryHeaderBytes));
    if (!payload.empty()) std::memcpy(out.data() + kEntryHeaderBytes, payload.data(), payload.size());
    return out;
}

Result<EntryHeader> readEntryHeader(std::span<const u8> bytes, const Hash128& expectedKey) {
    if (bytes.size() < kEntryHeaderBytes) {
        return makeError(ErrorCode::EndOfFile, "DDC entry of {} bytes is shorter than its {}-byte header",
                         bytes.size(), kEntryHeaderBytes);
    }
    const u8* p = bytes.data();
    if (loadLE<u32>(p) != kEntryMagic) return Error{ErrorCode::Corrupt, "not a DDC entry (bad magic)"};
    if (const u16 version = loadLE<u16>(p + 4); version != kEntryVersion) {
        return makeError(ErrorCode::VersionMismatch, "DDC entry version {} (this build reads {})", version,
                         kEntryVersion);
    }
    if (loadLE<u16>(p + 6) != kEntryHeaderBytes)
        return Error{ErrorCode::Corrupt, "DDC entry header size is not 64"};
    if (loadLE<u64>(p + 56) != hash64(p, kEntryHeaderHashedBytes)) {
        return Error{ErrorCode::Corrupt, "DDC entry header checksum mismatch"};
    }
    if (loadLE<u32>(p + 48) != 0 || loadLE<u32>(p + 52) != 0) {
        return Error{ErrorCode::Corrupt, "DDC entry has flags or reserved bytes set"};
    }
    EntryHeader h;
    h.key = loadHash(p + 8);
    h.payloadSize = loadLE<u64>(p + 24);
    h.payloadHash = loadHash(p + 32);
    if (h.payloadSize > kMaxPayload) {
        return makeError(ErrorCode::LimitExceeded, "DDC entry claims {} payload bytes (at most {})",
                         h.payloadSize, kMaxPayload);
    }
    if (h.key != expectedKey) {
        return makeError(ErrorCode::Corrupt, "DDC entry holds key {}, not {}", h.key.toHex(),
                         expectedKey.toHex());
    }
    return h;
}

Result<void> checkEntryPayload(const EntryHeader& header, std::span<const u8> payload) {
    if (payload.size() < header.payloadSize) {
        return makeError(ErrorCode::EndOfFile, "DDC entry truncated: {} of {} payload bytes", payload.size(),
                         header.payloadSize);
    }
    if (payload.size() > header.payloadSize) {
        return makeError(ErrorCode::Corrupt, "DDC entry has {} bytes after its {}-byte payload",
                         payload.size() - header.payloadSize, header.payloadSize);
    }
    if (hash128(payload.data(), payload.size()) != header.payloadHash) {
        return Error{ErrorCode::Corrupt, "DDC entry payload checksum mismatch"};
    }
    return {};
}

Result<std::span<const u8>> readEntry(std::span<const u8> entry, const Hash128& expectedKey) {
    HELIOS_TRY_ASSIGN(const EntryHeader header, readEntryHeader(entry, expectedKey));
    const std::span<const u8> payload = entry.subspan(kEntryHeaderBytes);
    HELIOS_TRY(checkEntryPayload(header, payload));
    return payload;
}

} // namespace ddc

// ---------------------------------------------------------------------------------------------
// LocalDdc
// ---------------------------------------------------------------------------------------------

Result<std::unique_ptr<LocalDdc>> LocalDdc::open(const LocalDdcOptions& options) {
    if (options.root.empty()) return Error{ErrorCode::InvalidArgument, "LocalDdc: empty root"};
    if (options.capBytes == 0) return Error{ErrorCode::InvalidArgument, "LocalDdc: a cap of 0 bytes"};
    if (options.trimTargetPercent == 0 || options.trimTargetPercent > 100) {
        return makeError(ErrorCode::InvalidArgument, "LocalDdc: trim target {}% is outside 1..100",
                         options.trimTargetPercent);
    }
    HELIOS_TRY(fs::createDirectories(options.root));
    std::unique_ptr<LocalDdc> ddc(new LocalDdc(options));
    // Measure without evicting: open() never deletes; the first put past the cap trims.
    u64 bytes = 0;
    HELIOS_TRY_ASSIGN(const auto dirs,
                      fs::listDirectory(options.root, fs::ListOptions{.includeFiles = false}));
    for (const fs::DirEntry& d : dirs) {
        if (!isFanoutDir(d.relativePath)) continue;
        auto files = fs::listDirectory(d.path, fs::ListOptions{.includeDirectories = false});
        if (!files) continue;
        for (const fs::DirEntry& f : *files) {
            if (isEntryName(f.relativePath)) bytes += f.size;
        }
    }
    ddc->m_bytes.store(bytes, std::memory_order_relaxed);
    return ddc;
}

fs::Path LocalDdc::entryPath(const Hash128& key) const {
    const std::string hex = key.toHex();
    return m_options.root / fs::pathFromUtf8(hex.substr(0, 2)) /
           fs::pathFromUtf8(hex + std::string(ddc::kEntryExtension));
}

Result<std::vector<u8>> LocalDdc::get(const Hash128& key) {
    const fs::Path path = entryPath(key);
    auto file = fs::File::open(path, fs::OpenMode::Read);
    if (!file) {
        if (file.error().code == ErrorCode::NotFound) m_misses.fetch_add(1, std::memory_order_relaxed);
        return std::move(file).error();
    }
    // Header first: the payload is allocated only for a valid header whose size the file really has.
    const auto bad = [&](Error e) {
        m_bad.fetch_add(1, std::memory_order_relaxed);
        return Error{e.code, std::format("{}: {}", fs::pathToGenericUtf8(path), e.message)};
    };
    u8 header[ddc::kEntryHeaderBytes];
    HELIOS_TRY_ASSIGN(const usize got, file->readAt(0, header, sizeof(header)));
    auto h = ddc::readEntryHeader(std::span<const u8>(header, got), key);
    if (!h) return bad(h.error());
    HELIOS_TRY_ASSIGN(const u64 size, file->size());
    if (size != ddc::kEntryHeaderBytes + h->payloadSize) {
        return bad(
            Error{size < ddc::kEntryHeaderBytes + h->payloadSize ? ErrorCode::EndOfFile : ErrorCode::Corrupt,
                  std::format("file is {} bytes, the header says {}", size,
                              ddc::kEntryHeaderBytes + h->payloadSize)});
    }
    std::vector<u8> payload(static_cast<usize>(h->payloadSize));
    usize done = 0;
    while (done < payload.size()) {
        HELIOS_TRY_ASSIGN(const usize n, file->readAt(ddc::kEntryHeaderBytes + done, payload.data() + done,
                                                      payload.size() - done));
        if (n == 0) break;
        done += n;
    }
    payload.resize(done); // shorter only if the file shrank under us: then the check below fails
    if (auto ok = ddc::checkEntryPayload(*h, payload); !ok) return bad(ok.error());
    file->close();
    m_hits.fetch_add(1, std::memory_order_relaxed);
    // LRU: refresh the entry's time at most once per touchInterval. A failure costs only recency.
    const auto now = std::filesystem::file_time_type::clock::now();
    if (auto t = fs::lastWriteTime(path); t && now - *t >= m_options.touchInterval)
        (void)fs::setLastWriteTime(path, now);
    return payload;
}

Result<void> LocalDdc::put(const Hash128& key, std::span<const u8> payload) {
    if (payload.size() > ddc::kMaxPayload) {
        return makeError(ErrorCode::LimitExceeded, "DDC payload of {} bytes (at most {})", payload.size(),
                         ddc::kMaxPayload);
    }
    const fs::Path path = entryPath(key);
    HELIOS_TRY(fs::createDirectories(path.parent_path()));
    fs::Path temp = path;
    temp += fs::pathFromUtf8(std::string(kTempMarker) + Guid::generate().toString());
    u8 header[ddc::kEntryHeaderBytes];
    ddc::encodeEntryHeader(key, payload, header);
    Result<void> written = [&]() -> Result<void> {
        HELIOS_TRY_ASSIGN(fs::File file, fs::File::open(temp, fs::OpenMode::Write));
        HELIOS_TRY(file.write(header, sizeof(header)));
        if (!payload.empty()) HELIOS_TRY(file.write(payload.data(), payload.size()));
        return {};
    }();
    if (written) written = fs::rename(temp, path);
    if (!written) {
        (void)fs::remove(temp);
        // Another writer of the same key may hold the name (Windows refuses to replace a file in some
        // sharing states): its complete entry is as good as ours.
        auto existing = fs::readFile(path);
        if (!existing || !ddc::readEntry(*existing, key)) return written;
    }
    m_puts.fetch_add(1, std::memory_order_relaxed);
    const u64 total = m_bytes.fetch_add(ddc::kEntryHeaderBytes + payload.size(), std::memory_order_relaxed) +
                      ddc::kEntryHeaderBytes + payload.size();
    if (total > m_options.capBytes) {
        if (auto trimmed = trim(); !trimmed)
            HELIOS_LOG_WARN("DDC trim failed: {}", trimmed.error().toString());
    }
    return {};
}

Result<TrimResult> LocalDdc::trim() {
    std::lock_guard lock(m_trimMutex);
    return trimLocked();
}

Result<TrimResult> LocalDdc::trimLocked() {
    struct Entry {
        fs::Path path;
        u64 size = 0;
        std::filesystem::file_time_type time;
    };
    TrimResult result;
    std::vector<Entry> entries;
    const auto now = std::filesystem::file_time_type::clock::now();
    HELIOS_TRY_ASSIGN(const auto dirs,
                      fs::listDirectory(m_options.root, fs::ListOptions{.includeFiles = false}));
    for (const fs::DirEntry& d : dirs) {
        if (!isFanoutDir(d.relativePath)) continue;
        auto files = fs::listDirectory(d.path, fs::ListOptions{.includeDirectories = false});
        if (!files) continue;
        for (const fs::DirEntry& f : *files) {
            auto time = fs::lastWriteTime(f.path);
            if (!time) continue; // evicted or renamed by another process meanwhile
            if (isEntryName(f.relativePath)) {
                entries.push_back(Entry{f.path, f.size, *time});
            } else if (isTempName(f.relativePath) && now - *time >= m_options.staleTempAge) {
                if (fs::remove(f.path)) ++result.staleTemps;
            }
        }
    }
    u64 total = 0;
    for (const Entry& e : entries) total += e.size;
    // Integer arithmetic: target = cap * percent / 100 without overflow for any u64 cap.
    const u64 target = m_options.capBytes / 100 * m_options.trimTargetPercent +
                       m_options.capBytes % 100 * m_options.trimTargetPercent / 100;
    if (total > target) {
        // Least recently used first; ties by path so a trim is deterministic.
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.time != b.time ? a.time < b.time : a.path < b.path;
        });
        for (const Entry& e : entries) {
            if (total <= target) break;
            if (auto removed = fs::remove(e.path); !removed && removed.error().code != ErrorCode::NotFound) {
                HELIOS_LOG_WARN("DDC trim: {}", removed.error().toString());
                continue;
            }
            total -= e.size;
            ++result.evicted;
            result.evictedBytes += e.size;
        }
    }
    result.entries = entries.size() - result.evicted;
    result.bytes = total;
    m_bytes.store(total, std::memory_order_relaxed);
    m_evicted.fetch_add(result.evicted, std::memory_order_relaxed);
    m_evictedBytes.fetch_add(result.evictedBytes, std::memory_order_relaxed);
    return result;
}

DdcStats LocalDdc::stats() const noexcept {
    DdcStats s;
    s.hits = m_hits.load(std::memory_order_relaxed);
    s.misses = m_misses.load(std::memory_order_relaxed);
    s.bad = m_bad.load(std::memory_order_relaxed);
    s.puts = m_puts.load(std::memory_order_relaxed);
    s.evicted = m_evicted.load(std::memory_order_relaxed);
    s.evictedBytes = m_evictedBytes.load(std::memory_order_relaxed);
    return s;
}

} // namespace helios::assetpipe
