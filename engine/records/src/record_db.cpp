// RecordDb: validation of a whole `.hrdb`, lookups, value views and decoding.
#include "helios/records/record_db.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <new>

#include "helios/core/assert.h"
#include "helios/core/utf.h"
#include "helios/records/cook.h"
#include "helios/reflect/record.h"
#include "helios/reflect/serialize.h"

namespace helios::records {

using hrdb::load;

namespace {

Error corrupt(std::string msg) { return Error{ErrorCode::Corrupt, "hrdb: " + std::move(msg)}; }

i64 readInt(refl::Kind k, const u8* p) noexcept {
    switch (k) {
    case refl::Kind::Bool:
    case refl::Kind::U8: return load<u8>(p);
    case refl::Kind::I8: return load<i8>(p);
    case refl::Kind::I16: return load<i16>(p);
    case refl::Kind::U16: return load<u16>(p);
    case refl::Kind::I32: return load<i32>(p);
    case refl::Kind::U32: return load<u32>(p);
    case refl::Kind::I64: return load<i64>(p);
    default: return static_cast<i64>(load<u64>(p));
    }
}

struct AlignedFree {
    void operator()(u8* p) const noexcept { ::operator delete[](p, std::align_val_t{hrdb::kAlignment}); }
};

/// A RelSpan as validated: absolute target offset and element count.
struct SpanRef {
    usize at = 0;
    u32 count = 0;
};

} // namespace

struct RecordDb::Impl {
    fs::MappedFile mapped;
    std::unique_ptr<u8[], AlignedFree> owned;
    std::span<const u8> bytes;
    CookAudience audience = CookAudience::Client;
    u64 layoutHash = 0;
    std::unique_ptr<LayoutCache> layouts;
    struct TypeSlot {
        const refl::TypeInfo* type = nullptr;
        const CookedLayout* layout = nullptr;
    };
    std::vector<TypeSlot> types;
    SpanRef records;
    SpanRef byName;
    SpanRef tags;
    SpanRef visibleTags;

    const u8* at(usize offset) const noexcept { return bytes.data() + offset; }
    std::string_view text(usize fieldPos) const noexcept {
        const i32 off = load<i32>(at(fieldPos));
        const u32 n = load<u32>(at(fieldPos + 4));
        if (n == 0) return {};
        return {reinterpret_cast<const char*>(at(fieldPos)) + off, n};
    }
    usize recordEntry(usize i) const noexcept { return records.at + i * hrdb::kRecordEntryBytes; }
    std::string_view recordName(usize i) const noexcept { return text(recordEntry(i) + 16); }
    usize tagEntry(usize i) const noexcept { return tags.at + i * hrdb::kTagEntryBytes; }
};

// ---------------------------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------------------------

namespace {

class Validator {
public:
    Validator(const RecordDb::Impl& db) : m_db(db), m_size(db.bytes.size()) {}

    Result<SpanRef> span(usize fieldPos, usize stride) const {
        const i32 off = load<i32>(m_db.at(fieldPos));
        const u32 count = load<u32>(m_db.at(fieldPos + 4));
        if (count == 0) {
            if (off != 0) return corrupt(std::format("empty span at {} has a non-zero offset", fieldPos));
            return SpanRef{};
        }
        if (off <= 0) return corrupt(std::format("span at {} does not point forward", fieldPos));
        const u64 target = u64{fieldPos} + static_cast<u64>(off);
        if (target % hrdb::kAlignment != 0) return corrupt(std::format("span at {} targets an unaligned offset {}", fieldPos, target));
        if (target > m_size || u64{count} * stride > m_size - target) {
            return corrupt(std::format("span at {} ({} x {} bytes at {}) is out of bounds", fieldPos, count, stride, target));
        }
        return SpanRef{static_cast<usize>(target), count};
    }

    Result<usize> ptr(usize fieldPos, usize size) const {
        const i32 off = load<i32>(m_db.at(fieldPos));
        if (off <= 0) return corrupt(std::format("pointer at {} does not point forward", fieldPos));
        const u64 target = u64{fieldPos} + static_cast<u64>(off);
        if (target % hrdb::kAlignment != 0) return corrupt(std::format("pointer at {} targets an unaligned offset", fieldPos));
        if (target > m_size || size > m_size - target) return corrupt(std::format("pointer at {} is out of bounds", fieldPos));
        return static_cast<usize>(target);
    }

    Result<std::string_view> text(usize fieldPos) {
        HELIOS_TRY_ASSIGN(const SpanRef s, span(fieldPos, 1));
        HELIOS_TRY(consume(s.count));
        const std::string_view t(reinterpret_cast<const char*>(m_db.at(s.at)), s.count);
        if (!isValidUtf8(t)) return corrupt(std::format("text at {} is not UTF-8", fieldPos));
        return t;
    }

    /// Out-of-line bytes walked so far may not exceed the file: blocks the cooker writes never
    /// overlap, so more means shared or overlapping data that could make validation super-linear.
    Result<void> consume(u64 bytes) {
        m_walked += bytes;
        if (m_walked > m_size) return corrupt("out-of-line data is shared or overlaps");
        return {};
    }

    Result<void> value(const CookedLayout& l, usize at, u32 depth) {
        const u8* p = m_db.at(at);
        switch (l.enc) {
        case Enc::Bool:
            if (*p > 1) return corrupt(std::format("bool at {} is {}", at, *p));
            return {};
        case Enc::Int: {
            const refl::TypeInfo& t = *l.type;
            const i64 v = readInt(l.intKind, p);
            if (t.kind == refl::Kind::Enum && !t.enumByValue(v)) {
                return corrupt(std::format("value {} at {} is not declared in enum {}", v, at, t.qualifiedName));
            }
            if (t.kind == refl::Kind::Flags) {
                i64 all = 0;
                for (const refl::EnumValue& e : t.enumValues) all |= e.value;
                if ((v & ~all) != 0) return corrupt(std::format("flags {} at {} has undeclared bits", v, at));
            }
            return {};
        }
        case Enc::F32:
        case Enc::F64:
        case Enc::Guid:
        case Enc::EntityId:
        case Enc::NetHandle:
        case Enc::Duration: return {};
        case Enc::RecordRef:
            if ((load<u64>(p) & ~refl::kRecordIdMask) != 0) return corrupt(std::format("record id at {} exceeds 63 bits", at));
            return {};
        case Enc::Text: {
            HELIOS_TRY(text(at));
            return {};
        }
        case Enc::TagSet: {
            HELIOS_TRY_ASSIGN(const SpanRef s, span(at, 2));
            HELIOS_TRY(consume(u64{s.count} * 2));
            u32 prev = 0;
            for (u32 i = 0; i < s.count; ++i) {
                const u16 t = load<u16>(m_db.at(s.at + i * 2u));
                if (t >= m_db.tags.count || (i > 0 && t <= prev)) return corrupt(std::format("tag set at {} is not ascending tag indices", at));
                if (load<u8>(m_db.at(m_db.tagEntry(t) + 14)) & hrdb::kTagWithheld) {
                    return corrupt(std::format("tag set at {} names a withheld tag", at));
                }
                prev = t;
            }
            return {};
        }
        case Enc::Hxl: {
            HELIOS_TRY(text(at));
            HELIOS_TRY_ASSIGN(const SpanRef code, span(at + 8, 1));
            return consume(code.count);
        }
        case Enc::Struct:
            for (const CookedLayout::Field& f : l.fields) HELIOS_TRY(value(*f.layout, at + f.offset, depth));
            return {};
        case Enc::List:
        case Enc::Set:
        case Enc::KeyedList:
        case Enc::Map: {
            const u32 stride = l.stride();
            HELIOS_TRY_ASSIGN(const SpanRef s, span(at, stride));
            if (s.count != 0 && depth >= hrdb::kMaxNesting) {
                return Error{ErrorCode::LimitExceeded, std::format("hrdb: non-empty lists nest deeper than {} at {}", hrdb::kMaxNesting, at)};
            }
            HELIOS_TRY(consume(u64{s.count} * stride));
            if (l.enc == Enc::KeyedList) {
                HELIOS_TRY_ASSIGN(const SpanRef keys, span(at + 8, 16));
                if (keys.count != s.count) return corrupt(std::format("keyed list at {} has {} keys for {} elements", at, keys.count, s.count));
                HELIOS_TRY(consume(u64{keys.count} * 16));
            }
            for (u32 i = 0; i < s.count; ++i) {
                const usize e = s.at + usize{i} * stride;
                if (l.enc == Enc::Map) {
                    HELIOS_TRY(value(*l.key, e, depth + 1));
                    HELIOS_TRY(value(*l.element, e + l.payloadOffset, depth + 1));
                } else {
                    HELIOS_TRY(value(*l.element, e, depth + 1));
                }
            }
            return {};
        }
        case Enc::Optional:
            if (*p > 1) return corrupt(std::format("optional flag at {} is {}", at, *p));
            if (*p == 1) return value(*l.element, at + l.payloadOffset, depth);
            return {};
        case Enc::Array:
            for (u32 i = 0; i < l.count; ++i) HELIOS_TRY(value(*l.element, at + usize{i} * l.element->size, depth));
            return {};
        case Enc::Variant: {
            const u32 index = load<u32>(p);
            if (index >= l.alternatives.size()) return corrupt(std::format("variant at {} has alternative {}", at, index));
            return value(*l.alternatives[index], at + l.payloadOffset, depth);
        }
        }
        return corrupt("unknown encoding");
    }

private:
    const RecordDb::Impl& m_db;
    usize m_size;
    u64 m_walked = 0;
};

std::string_view parentTagName(std::string_view name) noexcept {
    const usize dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
}

} // namespace

RecordDb::RecordDb() noexcept = default;
RecordDb::~RecordDb() = default;
RecordDb::RecordDb(RecordDb&&) noexcept = default;
RecordDb& RecordDb::operator=(RecordDb&&) noexcept = default;
RecordDb::RecordDb(std::unique_ptr<Impl> impl) noexcept : m(std::move(impl)) {}

Result<RecordDb> RecordDb::openFile(const fs::Path& path, const refl::TypeRegistry& registry, const RecordDbOptions& options) {
    auto impl = std::make_unique<Impl>();
    HELIOS_TRY_ASSIGN(impl->mapped, fs::MappedFile::open(path));
    impl->bytes = impl->mapped.bytes();
    auto db = finishOpen(std::move(impl), registry, options);
    if (!db) return Error{db.error().code, std::format("{}: {}", fs::pathToGenericUtf8(path), db.error().message)};
    return db;
}

Result<RecordDb> RecordDb::openBytes(std::span<const u8> bytes, const refl::TypeRegistry& registry, const RecordDbOptions& options) {
    auto impl = std::make_unique<Impl>();
    if (bytes.size() > hrdb::kMaxFileSize) return Error{ErrorCode::LimitExceeded, "hrdb: larger than 2 GiB"};
    if (!bytes.empty()) {
        impl->owned.reset(static_cast<u8*>(::operator new[](bytes.size(), std::align_val_t{hrdb::kAlignment})));
        std::memcpy(impl->owned.get(), bytes.data(), bytes.size());
    }
    impl->bytes = std::span<const u8>(impl->owned.get(), bytes.size());
    return finishOpen(std::move(impl), registry, options);
}

Result<RecordDb> RecordDb::finishOpen(std::unique_ptr<Impl> impl, const refl::TypeRegistry& registry, const RecordDbOptions& options) {
    Impl& d = *impl;
    const usize size = d.bytes.size();
    if (size < hrdb::kTablesOffset) return corrupt(std::format("{} bytes is too short for a record database", size));
    if (size > hrdb::kMaxFileSize) return Error{ErrorCode::LimitExceeded, "hrdb: larger than 2 GiB"};
    HELIOS_ASSERT(reinterpret_cast<std::uintptr_t>(d.bytes.data()) % hrdb::kAlignment == 0);
    const std::span<const u8, hrdb::kHeaderBytes> headerBytes(d.bytes.data(), hrdb::kHeaderBytes);
    const hrdb::Header h = hrdb::decodeHeader(headerBytes);
    if (h.magic != hrdb::kMagic) return corrupt("not a record database (bad magic)");
    if (h.formatVersion != hrdb::kFormatVersion) {
        return Error{ErrorCode::VersionMismatch, std::format("hrdb: format version {} (this build reads {})", h.formatVersion, hrdb::kFormatVersion)};
    }
    if (hrdb::computeHeaderHash(headerBytes) != h.headerHash) return corrupt("header checksum mismatch");
    if (h.size != size) return corrupt(std::format("header says {} bytes, the file has {}", h.size, size));
    if (h.rootTypeId != hrdb::kRootTypeId) return corrupt("unknown root type");
    if (h.flags != 0 || h.reserved0 != 0 || h.reserved1 != 0 || h.reserved2 != 0) return corrupt("reserved header fields are set");
    if (h.audience != static_cast<u8>(CookAudience::Client) && h.audience != static_cast<u8>(CookAudience::Server)) {
        return corrupt(std::format("unknown audience {}", h.audience));
    }
    d.audience = static_cast<CookAudience>(h.audience);
    if (options.audience && *options.audience != d.audience) {
        return Error{ErrorCode::InvalidArgument, std::format("hrdb: this is the {} cook, expected the {} cook", cookAudienceName(d.audience),
                                                             cookAudienceName(*options.audience))};
    }
    if (hrdb::computeContentHash(d.bytes) != h.contentHash) return corrupt("content checksum mismatch");
    d.layoutHash = h.layoutHash;
    for (usize i = hrdb::kRootOffset + 40; i < hrdb::kTablesOffset; ++i) {
        if (d.bytes[i] != 0) return corrupt("reserved root fields are set");
    }

    Validator v(d);
    const usize root = hrdb::kRootOffset;
    HELIOS_TRY_ASSIGN(const SpanRef types, v.span(root + hrdb::kRootTypes, hrdb::kTypeEntryBytes));
    HELIOS_TRY_ASSIGN(d.records, v.span(root + hrdb::kRootRecords, hrdb::kRecordEntryBytes));
    HELIOS_TRY_ASSIGN(d.byName, v.span(root + hrdb::kRootByName, 4));
    HELIOS_TRY_ASSIGN(d.tags, v.span(root + hrdb::kRootTags, hrdb::kTagEntryBytes));
    HELIOS_TRY_ASSIGN(d.visibleTags, v.span(root + hrdb::kRootVisibleTags, 2));

    // Types: ascending ids, each registered here with the same name and cooked layout.
    d.layouts = std::make_unique<LayoutCache>(d.audience);
    Hasher64 dbHash(hrdb::kRootTypeId);
    dbHash.updateValue(hrdb::kFormatVersion);
    dbHash.updateValue(h.audience);
    for (u32 i = 0; i < types.count; ++i) {
        const usize e = types.at + usize{i} * hrdb::kTypeEntryBytes;
        const u32 id = load<u32>(d.at(e));
        const u32 fixedSize = load<u32>(d.at(e + 4));
        const u64 layoutHash = load<u64>(d.at(e + 8));
        HELIOS_TRY_ASSIGN(const std::string_view name, v.text(e + 16));
        if (load<u64>(d.at(e + 24)) != 0) return corrupt("reserved type entry fields are set");
        if (i > 0 && id <= load<u32>(d.at(e - hrdb::kTypeEntryBytes))) return corrupt("type table is not sorted by id");
        const refl::TypeInfo* t = registry.find(id);
        if (!t) return Error{ErrorCode::NotFound, std::format("hrdb: record type '{}' (id {:#010x}) is not registered", name, id)};
        if (t->qualifiedName != name) {
            return Error{ErrorCode::VersionMismatch, std::format("hrdb: type id {:#010x} is '{}' here but '{}' in the cook", id, t->qualifiedName, name)};
        }
        if (t->decl != refl::DeclKind::Record) return corrupt(std::format("type '{}' is not a record type", name));
        if (excludesType(*t, d.audience)) {
            return corrupt(std::format("the {} cook holds record type '{}', which it may not contain", cookAudienceName(d.audience), name));
        }
        HELIOS_TRY_ASSIGN(const CookedLayout* layout, d.layouts->get(*t));
        if (layout->hash != layoutHash) {
            return Error{ErrorCode::VersionMismatch,
                         std::format("hrdb: record type '{}' was cooked with another layout; recook the records", name)};
        }
        if (layout->size != fixedSize) return corrupt(std::format("type '{}' has a fixed size of {}, not {}", name, layout->size, fixedSize));
        d.types.push_back(Impl::TypeSlot{t, layout});
        dbHash.updateValue(id);
        dbHash.updateValue(layoutHash);
    }
    if (dbHash.digest() != h.layoutHash) return corrupt("the header's layout hash does not match the type table");

    // Tags: a well-formed forest in index order; the names this cook carries are valid and sorted.
    if (d.tags.count > hrdb::kMaxTags) return corrupt("too many tags");
    u32 visible = 0;
    for (u32 i = 0; i < d.tags.count; ++i) {
        const usize e = d.tagEntry(i);
        HELIOS_TRY_ASSIGN(const std::string_view name, v.text(e));
        const u16 parent = load<u16>(d.at(e + 8));
        const u16 end = load<u16>(d.at(e + 10));
        const u8 depth = load<u8>(d.at(e + 12));
        const u8 audience = load<u8>(d.at(e + 13));
        const u8 flags = load<u8>(d.at(e + 14));
        if (load<u8>(d.at(e + 15)) != 0 || audience > 2 || (flags & ~(hrdb::kTagDeclared | hrdb::kTagWithheld)) != 0) {
            return corrupt(std::format("tag {} has invalid fields", i));
        }
        const bool withheld = (flags & hrdb::kTagWithheld) != 0;
        if (withheld && (d.audience == CookAudience::Server || !name.empty())) return corrupt(std::format("tag {} is withheld wrongly", i));
        if (!withheld && !isValidTagName(name)) return corrupt(std::format("tag {} has an invalid name", i));
        if (end <= i || end > d.tags.count) return corrupt(std::format("tag {} has a bad subtree range", i));
        if (parent == kNoTag) {
            if (depth != 0) return corrupt(std::format("root tag {} has depth {}", i, depth));
        } else {
            const usize pe = d.tagEntry(parent);
            if (parent >= i || i >= load<u16>(d.at(pe + 10)) || end > load<u16>(d.at(pe + 10)) ||
                depth != load<u8>(d.at(pe + 12)) + 1) {
                return corrupt(std::format("tag {} is not inside its parent {}", i, parent));
            }
            if (!withheld) {
                if (load<u8>(d.at(pe + 14)) & hrdb::kTagWithheld) return corrupt(std::format("tag {} has a withheld parent", i));
                if (parentTagName(name) != d.text(pe)) return corrupt(std::format("tag {} does not extend its parent's name", i));
            }
        }
        if (!withheld && static_cast<usize>(std::count(name.begin(), name.end(), '.')) != depth) {
            return corrupt(std::format("tag {} has depth {}", i, depth));
        }
        if (!withheld) ++visible;
    }
    if (d.visibleTags.count != visible) return corrupt("the visible tag list does not match the tag table");
    for (u32 i = 0; i < d.visibleTags.count; ++i) {
        const u16 t = load<u16>(d.at(d.visibleTags.at + i * 2u));
        if (t >= d.tags.count || (load<u8>(d.at(d.tagEntry(t) + 14)) & hrdb::kTagWithheld)) return corrupt("visible tag list names a withheld tag");
        if (i > 0) {
            const u16 prev = load<u16>(d.at(d.visibleTags.at + (i - 1) * 2u));
            if (t <= prev || !(d.text(d.tagEntry(prev)) < d.text(d.tagEntry(t)))) return corrupt("visible tags are not in name order");
        }
    }

    // Records: ascending ids, valid entries, every value valid for its layout.
    for (u32 i = 0; i < d.records.count; ++i) {
        const usize e = d.recordEntry(i);
        const u64 rid = load<u64>(d.at(e));
        if (!refl::isValidRecordId(rid)) return corrupt(std::format("record {} has an invalid id", i));
        if (i > 0 && rid <= load<u64>(d.at(e - hrdb::kRecordEntryBytes))) return corrupt("record table is not sorted by id");
        const u32 ti = load<u32>(d.at(e + 8));
        if (ti >= d.types.size()) return corrupt(std::format("record {} has type index {}", rid, ti));
        HELIOS_TRY_ASSIGN(const std::string_view name, v.text(e + 16));
        if (name.empty()) return corrupt(std::format("record {} has no name", rid));
        const CookedLayout& layout = *d.types[ti].layout;
        HELIOS_TRY_ASSIGN(const usize data, v.ptr(e + 12, layout.size));
        HELIOS_TRY(v.consume(layout.size));
        if (auto r = v.value(layout, data, 0); !r) return Error{r.error().code, std::format("record {}: {}", rid, r.error().message)};
    }
    // Name index: a permutation of the records, sorted by name.
    if (d.byName.count != d.records.count) return corrupt("the name index does not cover the records");
    std::vector<bool> seen(d.records.count, false);
    for (u32 i = 0; i < d.byName.count; ++i) {
        const u32 r = load<u32>(d.at(d.byName.at + i * 4u));
        if (r >= d.records.count || seen[r]) return corrupt("the name index is not a permutation");
        seen[r] = true;
        if (i > 0 && !(d.recordName(load<u32>(d.at(d.byName.at + (i - 1) * 4u))) < d.recordName(r))) {
            return corrupt("the name index is not sorted");
        }
    }
    return RecordDb(std::move(impl));
}

// ---------------------------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------------------------

CookAudience RecordDb::audience() const noexcept { return m->audience; }
u64 RecordDb::layoutHash() const noexcept { return m->layoutHash; }
std::span<const u8> RecordDb::bytes() const noexcept { return m->bytes; }
usize RecordDb::recordCount() const noexcept { return m ? m->records.count : 0; }

RecordView RecordDb::record(usize index) const noexcept {
    HELIOS_ASSERT(index < recordCount());
    const usize e = m->recordEntry(index);
    const Impl::TypeSlot& t = m->types[load<u32>(m->at(e + 8))];
    const usize data = e + 12 + static_cast<usize>(load<i32>(m->at(e + 12)));
    return RecordView{load<u64>(m->at(e)), m->recordName(index), t.type, ValueView(t.layout, m->at(data))};
}

RecordView RecordDb::find(refl::RecordId id) const noexcept {
    usize lo = 0;
    usize hi = recordCount();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        const u64 rid = load<u64>(m->at(m->recordEntry(mid)));
        if (rid == id) return record(mid);
        if (rid < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return {};
}

RecordView RecordDb::findByName(std::string_view name) const noexcept {
    if (!m) return {};
    usize lo = 0;
    usize hi = m->byName.count;
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        const u32 r = load<u32>(m->at(m->byName.at + mid * 4));
        const std::string_view n = m->recordName(r);
        if (n == name) return record(r);
        if (n < name) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return {};
}

usize RecordDb::tagCount() const noexcept { return m ? m->tags.count : 0; }

TagView RecordDb::tag(TagIndex index) const noexcept {
    HELIOS_ASSERT(index < tagCount());
    const usize e = m->tagEntry(index);
    const u8 flags = load<u8>(m->at(e + 14));
    return TagView{m->text(e),
                   load<u16>(m->at(e + 8)),
                   load<u16>(m->at(e + 10)),
                   load<u8>(m->at(e + 12)),
                   load<u8>(m->at(e + 13)),
                   (flags & hrdb::kTagDeclared) != 0,
                   (flags & hrdb::kTagWithheld) != 0};
}

TagIndex RecordDb::findTag(std::string_view name) const noexcept {
    if (!m) return kNoTag;
    usize lo = 0;
    usize hi = m->visibleTags.count;
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        const u16 t = load<u16>(m->at(m->visibleTags.at + mid * 2));
        const std::string_view n = m->text(m->tagEntry(t));
        if (n == name) return t;
        if (n < name) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return kNoTag;
}

// ---------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------

std::span<const u8> ValueView::span(usize at, usize stride) const noexcept {
    const i32 off = load<i32>(m_data + at);
    const u32 n = load<u32>(m_data + at + 4);
    if (n == 0) return {};
    return {m_data + at + off, usize{n} * stride};
}

bool ValueView::asBool() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Bool);
    return *m_data != 0;
}

i64 ValueView::asInt() const noexcept {
    switch (enc()) {
    case Enc::Int: return readInt(m_layout->intKind, m_data);
    case Enc::NetHandle: return load<u32>(m_data);
    case Enc::EntityId:
    case Enc::Duration:
    case Enc::RecordRef: return load<i64>(m_data);
    default: HELIOS_ASSERT(false, "not an integer"); return 0;
    }
}

f64 ValueView::asFloat() const noexcept {
    if (enc() == Enc::F32) return load<f32>(m_data);
    HELIOS_ASSERT(enc() == Enc::F64);
    return load<f64>(m_data);
}

f32 ValueView::asF32() const noexcept {
    HELIOS_ASSERT(enc() == Enc::F32);
    return load<f32>(m_data);
}

std::string_view ValueView::asText() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Text || enc() == Enc::Hxl);
    const std::span<const u8> s = span(0, 1);
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

std::span<const u8> ValueView::hxlBytecode() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Hxl);
    return span(8, 1);
}

std::span<const TagIndex> ValueView::tags() const noexcept {
    HELIOS_ASSERT(enc() == Enc::TagSet);
    const std::span<const u8> s = span(0, 2);
    // Validated 16-byte aligned in a 16-byte aligned buffer.
    return {reinterpret_cast<const TagIndex*>(s.data()), s.size() / 2};
}

Guid ValueView::asGuid() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Guid);
    return Guid(load<u64>(m_data), load<u64>(m_data + 8));
}

ValueView ValueView::field(std::string_view name) const noexcept {
    HELIOS_ASSERT(enc() == Enc::Struct);
    const CookedLayout::Field* f = m_layout->field(name);
    return f ? field(*f) : ValueView();
}

usize ValueView::size() const noexcept {
    switch (enc()) {
    case Enc::List:
    case Enc::KeyedList:
    case Enc::Set:
    case Enc::Map: return load<u32>(m_data + 4);
    case Enc::Array: return m_layout->count;
    default: HELIOS_ASSERT(false, "not a container"); return 0;
    }
}

ValueView ValueView::operator[](usize i) const noexcept {
    HELIOS_ASSERT(i < size() && enc() != Enc::Map);
    const u32 stride = m_layout->stride();
    if (enc() == Enc::Array) return {m_layout->element, m_data + i * stride};
    return {m_layout->element, span(0, stride).data() + i * stride};
}

Guid ValueView::keyAt(usize i) const noexcept {
    HELIOS_ASSERT(enc() == Enc::KeyedList && i < size());
    const u8* k = span(8, 16).data() + i * 16;
    return Guid(load<u64>(k), load<u64>(k + 8));
}

ValueView ValueView::mapKey(usize i) const noexcept {
    HELIOS_ASSERT(enc() == Enc::Map && i < size());
    return {m_layout->key, span(0, m_layout->entrySize).data() + i * m_layout->entrySize};
}

ValueView ValueView::mapValue(usize i) const noexcept {
    HELIOS_ASSERT(enc() == Enc::Map && i < size());
    return {m_layout->element, span(0, m_layout->entrySize).data() + i * m_layout->entrySize + m_layout->payloadOffset};
}

bool ValueView::hasValue() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Optional);
    return *m_data != 0;
}

ValueView ValueView::value() const noexcept {
    HELIOS_ASSERT(hasValue());
    return {m_layout->element, m_data + m_layout->payloadOffset};
}

u32 ValueView::alternative() const noexcept {
    HELIOS_ASSERT(enc() == Enc::Variant);
    return load<u32>(m_data);
}

ValueView ValueView::alternativeValue() const noexcept {
    return {m_layout->alternatives[alternative()], m_data + m_layout->payloadOffset};
}

// ---------------------------------------------------------------------------------------------
// Decoding into objects
// ---------------------------------------------------------------------------------------------

namespace {

void decodeValue(const RecordDb& db, const ValueView& v, void* obj) {
    const CookedLayout& l = v.layout();
    const refl::TypeInfo& t = v.type();
    const refl::TypeOps& ops = *t.ops;
    switch (l.enc) {
    case Enc::Bool: *static_cast<bool*>(obj) = v.asBool(); return;
    case Enc::Int: refl::writeIntegerBits(t, obj, v.asInt()); return;
    case Enc::F32: std::memcpy(obj, v.data(), 4); return;
    case Enc::F64: std::memcpy(obj, v.data(), 8); return;
    case Enc::Text: {
        const std::string_view s = v.asText();
        if (t.kind == refl::Kind::String) {
            static_cast<std::string*>(obj)->assign(s);
        } else if (t.kind == refl::Kind::Name) {
            *static_cast<Name*>(obj) = Name(s);
        } else if (t.qualifiedName == "LocString") {
            static_cast<refl::LocString*>(obj)->key.assign(s);
        } else {
            static_cast<refl::TagQuery*>(obj)->text.assign(s);
        }
        return;
    }
    case Enc::TagSet: {
        auto& set = *static_cast<refl::TagSet*>(obj);
        set.clear();
        for (const TagIndex i : v.tags()) set.add(db.tag(i).name);
        return;
    }
    case Enc::Hxl: static_cast<refl::HxlExpr*>(obj)->text.assign(v.asText()); return;
    case Enc::Guid:
        if (t.qualifiedName == "AssetRef") {
            static_cast<refl::AssetRef*>(obj)->guid = v.asGuid();
        } else {
            *static_cast<Guid*>(obj) = v.asGuid();
        }
        return;
    case Enc::EntityId: static_cast<refl::EntityId*>(obj)->value = v.asUInt(); return;
    case Enc::NetHandle: static_cast<refl::NetHandle*>(obj)->value = static_cast<u32>(v.asUInt()); return;
    case Enc::Duration: static_cast<refl::Duration*>(obj)->nanos = v.asInt(); return;
    case Enc::RecordRef: {
        // Every RecordRef<T> is one RecordId (trivially copyable, standard layout).
        const u64 id = v.asUInt();
        std::memcpy(obj, &id, sizeof(id));
        return;
    }
    case Enc::Struct:
        for (const CookedLayout::Field& f : l.fields) decodeValue(db, v.field(f), f.info->ptr(obj));
        return;
    case Enc::List:
    case Enc::KeyedList: {
        const usize n = v.size();
        ops.resize(obj, n);
        for (usize i = 0; i < n; ++i) {
            decodeValue(db, v[i], ops.element(obj, i));
            if (l.enc == Enc::KeyedList) ops.setKeyAt(obj, i, v.keyAt(i));
        }
        return;
    }
    case Enc::Set: {
        ops.clear(obj);
        for (usize i = 0; i < v.size(); ++i) {
            refl::Value e(*l.element->type);
            decodeValue(db, v[i], e.data());
            ops.findOrInsert(obj, e.data());
        }
        return;
    }
    case Enc::Map: {
        ops.clear(obj);
        for (usize i = 0; i < v.size(); ++i) {
            refl::Value k(*l.key->type);
            decodeValue(db, v.mapKey(i), k.data());
            decodeValue(db, v.mapValue(i), ops.findOrInsert(obj, k.data()));
        }
        return;
    }
    case Enc::Optional:
        if (v.hasValue()) {
            decodeValue(db, v.value(), ops.emplace(obj));
        } else {
            ops.reset(obj);
        }
        return;
    case Enc::Array:
        for (usize i = 0; i < l.count; ++i) decodeValue(db, v[i], ops.element(obj, i));
        return;
    case Enc::Variant: decodeValue(db, v.alternativeValue(), ops.emplaceAlt(obj, v.alternative())); return;
    }
}

} // namespace

Result<void> RecordDb::decode(const RecordView& record, void* object) const {
    if (!record) return Error{ErrorCode::InvalidArgument, "decode: no record"};
    decodeValue(*this, record.value, object);
    return {};
}

} // namespace helios::records
