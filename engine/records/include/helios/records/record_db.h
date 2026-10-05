#pragma once
// RecordDb: the zero-copy loader of a cooked `.hrdb` (02 §3.3, §3.7), with lookups by RecordId and
// by $name, the tag table, and typed views of cooked values.
//
// open*() validates the whole file before it returns: the header and both checksums; the layout hash
// against the cooked layouts of the types in `registry` (VersionMismatch: the database was cooked
// against another schema, recook it); every table; and every record's value, walking its layout: every
// RelSpan and RelPtr is in bounds, 16-byte aligned and points forward, bools are 0 or 1, enum values
// are declared, text is UTF-8, tag indices exist (and, in a client cook, name a tag whose name the
// cook carries). Out-of-line data is walked at most once per byte of the file and at most
// hrdb::kMaxNesting levels deep, so validation is linear in the file size whatever the input. A
// RecordDb that opened is therefore safe to read with the unchecked views below; hostile bytes fail
// with Corrupt (or LimitExceeded / VersionMismatch / NotFound), never with undefined behaviour. The
// guarantee covers the bytes that were validated: openFile() maps the file, so a file modified in place
// while it is open is outside it (writeCookOutput() replaces files by renaming, never in place).
// Fuzzed by fuzz/fuzz_hrdb_loader.cpp.
//
// Threading: a RecordDb is immutable after open; every const method is safe to call from any thread.
// Views borrow the RecordDb's bytes and layouts and must not outlive it (moving the RecordDb keeps them
// valid).

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/guid.h"
#include "helios/core/result.h"
#include "helios/records/hrdb_format.h"
#include "helios/records/layout.h"
#include "helios/reflect/registry.h"
#include "helios/reflect/types.h"

namespace helios::records {

/// A cooked value of a known layout (no ownership; valid while its RecordDb lives, moves included).
/// Accessors require a valid view and assert that the layout fits the call.
class ValueView {
public:
    ValueView() noexcept = default;
    ValueView(const CookedLayout* layout, const u8* data) noexcept : m_layout(layout), m_data(data) {}

    bool isValid() const noexcept { return m_layout != nullptr; }
    const CookedLayout& layout() const noexcept { return *m_layout; }
    const refl::TypeInfo& type() const noexcept { return *m_layout->type; }
    Enc enc() const noexcept { return m_layout->enc; }

    bool asBool() const noexcept;
    /// Enc::Int (integers, enums, flags), EntityId, NetHandle, Duration, RecordRef.
    i64 asInt() const noexcept;
    u64 asUInt() const noexcept { return static_cast<u64>(asInt()); }
    /// Enc::F32 or Enc::F64.
    f64 asFloat() const noexcept;
    f32 asF32() const noexcept;
    /// Text, or the source of an HxlExpr.
    std::string_view asText() const noexcept;
    /// HXL1 bytecode of an HxlExpr (empty for an empty expression); hxl::Program::decode() verifies it.
    std::span<const u8> hxlBytecode() const noexcept;
    /// Tag indices of a TagSet, ascending.
    std::span<const TagIndex> tags() const noexcept;
    Guid asGuid() const noexcept;
    refl::RecordId asRecordId() const noexcept { return asUInt(); }

    /// Struct field (an invalid view if the field does not exist or this cook dropped it).
    ValueView field(std::string_view name) const noexcept;
    ValueView field(const CookedLayout::Field& f) const noexcept { return {f.layout, m_data + f.offset}; }

    /// Element count of a List, KeyedList, Set, Map or Array.
    usize size() const noexcept;
    /// Element i of a List, KeyedList, Set or Array.
    ValueView operator[](usize i) const noexcept;
    /// Key GUID of element i of a KeyedList.
    Guid keyAt(usize i) const noexcept;
    /// Entry i of a Map (canonical key order).
    ValueView mapKey(usize i) const noexcept;
    ValueView mapValue(usize i) const noexcept;

    /// Optional: is a value present, and the value.
    bool hasValue() const noexcept;
    ValueView value() const noexcept;
    /// Variant: active alternative index (refl::TypeInfo::alternatives order) and its payload.
    u32 alternative() const noexcept;
    ValueView alternativeValue() const noexcept;

    const u8* data() const noexcept { return m_data; }

private:
    std::span<const u8> span(usize at, usize stride) const noexcept;

    const CookedLayout* m_layout = nullptr;
    const u8* m_data = nullptr;
};

/// One cooked record.
struct RecordView {
    refl::RecordId id = 0;
    std::string_view name;
    const refl::TypeInfo* type = nullptr;
    ValueView value;
    explicit operator bool() const noexcept { return type != nullptr; }
};

/// One entry of a database's tag table.
struct TagView {
    std::string_view name;   ///< Empty when the name is withheld from this cook.
    TagIndex parent = kNoTag;
    TagIndex subtreeEnd = 0; ///< Exclusive end of the descendant range (index, subtreeEnd).
    u8 depth = 0;
    u8 audience = 0;         ///< gameplay::Audience value: 0 Server, 1 Owner, 2 All.
    bool declared = false;
    bool withheld = false;       ///< Client cook: this cook withholds the name.
    bool clientWithheld = false; ///< Server cook: the client cook withholds the name.
};

struct RecordDbOptions {
    /// When set, a database of the other audience is refused (InvalidArgument).
    std::optional<CookAudience> audience;
};

class RecordDb {
public:
    RecordDb() noexcept;
    ~RecordDb();
    RecordDb(RecordDb&&) noexcept;
    RecordDb& operator=(RecordDb&&) noexcept;

    /// Memory-maps and validates a database file. `registry` must hold every record type the file
    /// names; it must outlive the RecordDb.
    static Result<RecordDb> openFile(const fs::Path& path, const refl::TypeRegistry& registry, const RecordDbOptions& options = {});
    /// Validates a database held in memory (copied into 16-byte aligned storage the RecordDb owns).
    static Result<RecordDb> openBytes(std::span<const u8> bytes, const refl::TypeRegistry& registry,
                                      const RecordDbOptions& options = {});

    CookAudience audience() const noexcept;
    /// The header's layout hash (meant to be part of the records' DDC key, 02 §3.4).
    u64 layoutHash() const noexcept;
    /// The header's tag-table hash (hrdb_format.h): equal in the client and the server cook of one cook
    /// run, different when their TagIndex numbering differs. Compare it to pair a client cook with its
    /// server cook; the loader does not recompute it (a server cook does not say which names a client
    /// cook withholds).
    u64 tagTableHash() const noexcept;
    std::span<const u8> bytes() const noexcept;

    usize recordCount() const noexcept;
    /// Record i in RecordId order.
    RecordView record(usize index) const noexcept;
    /// Binary search by RecordId; an invalid RecordView when absent.
    RecordView find(refl::RecordId id) const noexcept;
    /// Binary search by $name; an invalid RecordView when absent.
    RecordView findByName(std::string_view name) const noexcept;

    usize tagCount() const noexcept;
    TagView tag(TagIndex index) const noexcept;
    /// kNoTag when the name is unknown or withheld from this cook.
    TagIndex findTag(std::string_view name) const noexcept;

    /// Copies a cooked record into a default-constructed object of its type (`object` must be a
    /// default-constructed `*record.type`). Fields this cook dropped keep their defaults; tag indices
    /// become names again and HxlExpr values get their source text back.
    Result<void> decode(const RecordView& record, void* object) const;

    struct Impl; ///< Implementation detail (record_db.cpp).

private:
    explicit RecordDb(std::unique_ptr<Impl> impl) noexcept;
    static Result<RecordDb> finishOpen(std::unique_ptr<Impl> impl, const refl::TypeRegistry& registry,
                                       const RecordDbOptions& options);
    std::unique_ptr<Impl> m;
};

} // namespace helios::records
