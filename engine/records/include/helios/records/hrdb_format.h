#pragma once
// The `.hrdb` v0 cooked record database (02 §3.3, §3.7): constants, the header, and the
// little-endian field helpers shared by the cooker (cook.h) and the loader (record_db.h), so the two
// cannot disagree on an offset.
//
// A database is one relocatable, little-endian blob, loaded zero-copy (memory-mapped). Every
// reference inside it is self-relative and points forward: a RelSpan is {i32 offset, u32 count}, a
// RelPtr is {i32 offset}, and the target is the address of the RelSpan/RelPtr field plus the offset.
// Every out-of-line block starts 16-byte aligned (02 §3.7), and padding is zero.
//
//   [0, 64)    header (below)
//   [64, 128)  root: types RelSpan<TypeEntry> · records RelSpan<RecordEntry> (sorted by RecordId) ·
//              byName RelSpan<u32> (record indices sorted by $name) · tags RelSpan<TagEntry> (TagIndex
//              order = byte-wise name order, implied ancestors included) · visibleTags RelSpan<u16>
//              (the tags whose names this cook carries, ascending) · 24 reserved bytes
//   [128, …)   the tables, then type and record names, then each record's fixed part followed by
//              its out-of-line data, then tag names
//
// Header (byte offsets): 0 magic 'HRDB' u32 · 4 formatVersion u16 · 6 audience u8 · 7 reserved u8 ·
//   8 rootTypeId u32 · 12 flags u32 (0) · 16 layoutHash u64 · 24 size u64 · 32 contentHash u64 (XXH3-64
//   of [64, size)) · 40 tagTableHash u64 · 48 reserved u64 · 56 headerHash u64 (XXH3-64 of [0, 56)).
// tagTableHash identifies the tag numbering: the cook hashes the client's view of its tag table (each
// entry's parent, subtree end, depth, audience and declared flag, and its name unless the client cook
// withholds it) and writes the same value into both cooks of a run, so a client and a server whose
// TagIndex values disagree can tell (06 §1.1: the registry hash is part of the content version). It
// carries no withheld name, so it cannot confirm a guess of one. The server cook marks the tags whose
// names the client cook withholds (kTagClientWithheld), so both loaders recompute and check it.
// TypeEntry (32 bytes): 0 typeId u32 · 4 fixedSize u32 · 8 layoutHash u64 · 16 name RelSpan<char> ·
//   24 reserved u64.
// RecordEntry (24 bytes): 0 rid u64 · 8 typeIndex u32 · 12 data RelPtr (the record's fixed part) ·
//   16 name RelSpan<char>.
// TagEntry (16 bytes): 0 name RelSpan<char> (empty when the name is withheld from this cook) ·
//   8 parent u16 (0xFFFF for a root tag) · 10 subtreeEnd u16 · 12 depth u8 · 13 audience u8 ·
//   14 flags u8 (kTagDeclared, kTagWithheld, kTagClientWithheld) · 15 reserved u8.
//
// Values are encoded by their cooked layout (layout.h). The header's layoutHash covers the format
// version, the audience and every record type's cooked layout, so a database cooked against another
// schema is refused (VersionMismatch) rather than misread; it is meant to be part of the records' DDC
// key (02 §3.4).
//
// Threading: everything here is a pure function or a constant.

#include <bit>
#include <cstring>
#include <span>
#include <string_view>

#include "helios/core/hash.h"
#include "helios/core/types.h"

namespace helios::records {

static_assert(std::endian::native == std::endian::little,
              "the cooked record format is little-endian and read in place (02 §3.7)");

/// Which cook a database is (02 §3.3, §6.5). 0 is no audience, so a zeroed header is rejected.
enum class CookAudience : u8 {
    Client = 1, ///< records.client.hrdb: shared and client data, never server-only data (AAA-SEC-4).
    Server = 2, ///< records.server.hrdb: shared and server data, never client-only data.
};

/// "client" / "server" ("unknown" otherwise).
std::string_view cookAudienceName(CookAudience audience) noexcept;

/// Dense index of a gameplay tag in a database's tag table (06 §1.1); the same values as
/// gameplay::TagIndex for the same set of tag names.
using TagIndex = u16;
inline constexpr TagIndex kNoTag = 0xFFFF;

namespace hrdb {

inline constexpr u32 kMagic = 0x42445248u; ///< "HRDB" as little-endian bytes.
inline constexpr u16 kFormatVersion = 0;   ///< v0 (WP-0.8).
/// Type id of the root table layout (the header's rootTypeId).
inline constexpr u32 kRootTypeId = fnv1a32("helios.records.RecordDb.v0");
inline constexpr usize kHeaderBytes = 64;
inline constexpr usize kHeaderHashedBytes = 56;
inline constexpr usize kRootOffset = 64;
inline constexpr usize kRootBytes = 64;
inline constexpr usize kTablesOffset = kRootOffset + kRootBytes;
inline constexpr usize kAlignment = 16;     ///< Every out-of-line block (02 §3.7).
inline constexpr u64 kMaxFileSize = 0x7FFF'FFF0ull; ///< i32 self-relative offsets.
inline constexpr usize kTypeEntryBytes = 32;
inline constexpr usize kRecordEntryBytes = 24;
inline constexpr usize kTagEntryBytes = 16;
inline constexpr usize kRelSpanBytes = 8;
inline constexpr u32 kMaxTags = 0xFFFE;     ///< Same as gameplay::kMaxTags.
/// Levels of non-empty lists, sets and maps inside one another that a cook may produce and a loader
/// accepts. JSONC sources cannot nest much deeper anyway (refl::kMaxJsonDepth is 128 objects and arrays,
/// and a recursive struct spends two of them per level).
inline constexpr u32 kMaxNesting = 32;

// Root table field offsets (relative to kRootOffset).
inline constexpr usize kRootTypes = 0;
inline constexpr usize kRootRecords = 8;
inline constexpr usize kRootByName = 16;
inline constexpr usize kRootTags = 24;
inline constexpr usize kRootVisibleTags = 32;

inline constexpr u8 kTagDeclared = 1; ///< Declared by a tag record or used in a TagSet (not only implied).
inline constexpr u8 kTagWithheld = 2; ///< Name withheld: only server-only data uses it (client cook).
inline constexpr u8 kTagClientWithheld = 4; ///< The client cook withholds this name (server cook).

/// The header's tagTableHash: feed every tag entry in index order, with its name as the client cook
/// carries it (empty when withheld). The cooker and the loader both use it.
class TagTableHasher {
public:
    explicit TagTableHasher(u32 count) noexcept : m_hash(kRootTypeId) { m_hash.updateValue(count); }
    void add(std::string_view clientName, u16 parent, u16 subtreeEnd, u8 depth, u8 audience, bool declared) noexcept {
        m_hash.updateValue(static_cast<u32>(clientName.size()));
        m_hash.update(clientName);
        m_hash.updateValue(parent);
        m_hash.updateValue(subtreeEnd);
        m_hash.updateValue(depth);
        m_hash.updateValue(audience);
        m_hash.updateValue(static_cast<u8>(declared ? 1 : 0));
    }
    u64 digest() const noexcept { return m_hash.digest(); }

private:
    Hasher64 m_hash;
};

/// Decoded header fields, without validation.
struct Header {
    u32 magic = kMagic;
    u16 formatVersion = kFormatVersion;
    u8 audience = 0;
    u8 reserved0 = 0;
    u32 rootTypeId = kRootTypeId;
    u32 flags = 0;
    u64 layoutHash = 0;
    u64 size = 0;
    u64 contentHash = 0;
    u64 tagTableHash = 0;
    u64 reserved2 = 0;
    u64 headerHash = 0;
};

template <class T>
inline T load(const u8* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
inline void store(u8* p, T v) noexcept {
    std::memcpy(p, &v, sizeof(T));
}

/// Writes the header fields to `out` (headerHash as given; see seal()).
void encodeHeader(const Header& header, std::span<u8, kHeaderBytes> out) noexcept;
/// Reads the header fields; no validation.
Header decodeHeader(std::span<const u8, kHeaderBytes> in) noexcept;
/// XXH3-64 of the encoded header's first kHeaderHashedBytes bytes.
u64 computeHeaderHash(std::span<const u8, kHeaderBytes> encoded) noexcept;
/// XXH3-64 of everything after the header (bytes [64, size)).
u64 computeContentHash(std::span<const u8> file) noexcept;
/// Recomputes contentHash and headerHash in place. The cooker seals what it writes; fuzzers and
/// hostile-input tests reseal mutated files so the mutations reach the field validators. No-op for a
/// file shorter than the header.
void seal(std::span<u8> file) noexcept;

} // namespace hrdb
} // namespace helios::records
