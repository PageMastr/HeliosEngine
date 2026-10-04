// The `.hrdb` loader on hostile input (02 §3.7: loaders validate bounds): truncation, bit flips behind
// valid checksums, crafted spans, shared out-of-line data, wrong schema or audience. Every case must
// fail cleanly (an Error, never a crash or a sanitizer report); a database that opens must be safe to read.
#include <cstring>

#include "test_util.h"

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;

namespace {

constexpr refl::RecordId kBase = 4390381077356775243ull;

/// Opens and, on success, reads everything (decode of every record, every tag): a DB that opened must
/// be safe to use.
bool openAndRead(std::span<const u8> bytes) {
    auto db = RecordDb::openBytes(bytes, registry());
    if (!db) return false;
    for (usize i = 0; i < db->recordCount(); ++i) {
        const RecordView r = db->record(i);
        refl::Value v(*r.type);
        CHECK(db->decode(r, v.data()).ok());
        CHECK(db->find(r.id).id == r.id);
        CHECK(db->findByName(r.name).id == r.id);
    }
    for (usize i = 0; i < db->tagCount(); ++i) {
        const TagView t = db->tag(static_cast<TagIndex>(i));
        if (!t.withheld) CHECK(db->findTag(t.name) == i);
    }
    return true;
}

/// A small database with every encoding (the ship template alone, plus what it references).
std::vector<u8> smallDb(CookAudience audience) {
    std::vector<SourceRecord> sources;
    for (SourceRecord& s : projectSources()) {
        if (s.path.find("ship/base") != std::string::npos || s.path.find("part/") != std::string::npos ||
            s.path.find("loot/") != std::string::npos || s.path.find("skin/") != std::string::npos) {
            sources.push_back(std::move(s));
        }
    }
    auto out = cook(sources);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    return audience == CookAudience::Client ? out->client : out->server;
}

usize offsetOf(const RecordDb& db, const u8* p) { return static_cast<usize>(p - db.bytes().data()); }

TEST_CASE("loader: every truncation fails cleanly, with or without resealed checksums") {
    const std::vector<u8> full = smallDb(CookAudience::Client);
    REQUIRE(openAndRead(full));
    for (usize n = 0; n < full.size(); ++n) {
        std::vector<u8> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK_FALSE(openAndRead(cut));
        hrdb::seal(cut);
        CHECK_FALSE(openAndRead(cut));
    }
}

TEST_CASE("loader: every single-bit flip, resealed, is rejected or still safe to read") {
    for (const CookAudience audience : {CookAudience::Client, CookAudience::Server}) {
        const std::vector<u8> full = smallDb(audience);
        usize opened = 0;
        for (usize bit = 0; bit < full.size() * 8; ++bit) {
            std::vector<u8> m = full;
            m[bit / 8] ^= static_cast<u8>(1u << (bit % 8));
            CHECK_FALSE(openAndRead(m)); // a flip without a reseal never passes the checksums
            hrdb::seal(m);
            if (!openAndRead(m)) continue;
            ++opened;
            // Every header field but the two checksums (which seal() rewrites) and every root-table
            // bit is structural: such a flip never opens.
            const usize byte = bit / 8;
            const bool checksum = (byte >= 32 && byte < 40) || (byte >= 56 && byte < hrdb::kHeaderBytes);
            if (byte < hrdb::kTablesOffset && !checksum) FAIL_CHECK("a flip of header or root byte " << byte << " opened");
        }
        // Flips in text, numbers, ids, padding and the like still open, and read safely.
        MESSAGE(cookAudienceName(audience), ": ", opened, " of ", full.size() * 8, " resealed bit flips still open");
    }
}

TEST_CASE("loader: crafted spans and values are rejected with Corrupt") {
    const std::vector<u8> full = smallDb(CookAudience::Server);
    const RecordDb db = open(full);
    const ValueView ship = db.find(kBase).value;
    const usize label = offsetOf(db, ship.field("label").data());   // RelSpan<char>
    const usize flag = offsetOf(db, ship.field("flag").data());
    const usize grade = offsetOf(db, ship.field("grade").data());
    const usize mode = offsetOf(db, ship.field("mode").data());
    const usize tags = offsetOf(db, ship.field("tags").data());
    const usize maybe = offsetOf(db, ship.field("maybe").data());
    auto expectCorrupt = [&](const char* what, auto&& patch, std::string_view fragment) {
        std::vector<u8> m = full;
        patch(m);
        hrdb::seal(m);
        auto r = RecordDb::openBytes(m, registry());
        REQUIRE_MESSAGE(!r.ok(), what);
        INFO(what << ": " << r.error().message);
        CHECK(r.error().code == ErrorCode::Corrupt);
        CHECK(r.error().message.find(fragment) != std::string::npos);
    };
    auto putI32 = [](std::vector<u8>& m, usize at, i32 v) { std::memcpy(m.data() + at, &v, 4); };
    auto putU32 = [](std::vector<u8>& m, usize at, u32 v) { std::memcpy(m.data() + at, &v, 4); };
    auto getI32 = [&](usize at) {
        i32 v;
        std::memcpy(&v, full.data() + at, 4);
        return v;
    };
    expectCorrupt("backward span", [&](auto& m) { putI32(m, label, -16); }, "does not point forward");
    expectCorrupt("unaligned span", [&](auto& m) { putI32(m, label, getI32(label) + 1); }, "unaligned");
    expectCorrupt("span past the end", [&](auto& m) { putU32(m, label + 4, 0x7FFFFFFFu); }, "out of bounds");
    expectCorrupt("empty span with an offset", [&](auto& m) { putU32(m, label + 4, 0); }, "non-zero offset");
    expectCorrupt("bool 2", [&](auto& m) { m[flag] = 2; }, "bool");
    expectCorrupt("undeclared enum value", [&](auto& m) { m[grade] = 9; }, "not declared in enum");
    expectCorrupt("variant index", [&](auto& m) { putU32(m, mode, 7); }, "variant");
    expectCorrupt("optional flag", [&](auto& m) { m[maybe] = 3; }, "optional");
    expectCorrupt("tag index past the table", [&](auto& m) {
        const usize at = tags + static_cast<usize>(getI32(tags));
        const u16 bad = 0xFFF0;
        std::memcpy(m.data() + at, &bad, 2);
    }, "tag set");
    expectCorrupt("invalid UTF-8", [&](auto& m) { m[label + static_cast<usize>(getI32(label))] = 0xFF; }, "UTF-8");
    // Header-level damage.
    expectCorrupt("bad magic", [&](auto& m) { m[0] ^= 1; }, "magic");
    expectCorrupt("size mismatch", [&](auto& m) { m[24] ^= 1; }, "bytes");
    expectCorrupt("reserved field", [&](auto& m) { m[40] = 1; }, "reserved");
    expectCorrupt("unknown audience", [&](auto& m) { m[6] = 3; }, "audience");
}

TEST_CASE("loader: out-of-line data shared into an exponential DAG is refused in linear time") {
    // A comb: every level has two kids, the first with the next level, the second a leaf.
    std::string json = R"({"value": 0})";
    for (int i = 0; i < 24; ++i) json = R"({"value": 1, "kids": [)" + json + R"(, {"value": 2}]})";
    const std::vector<SourceRecord> s = {source("comb.hrec", type("test.records.TreeDef"), R"({"$rid": 7, "$name": "t/comb", "root": )" + json + "}")};
    auto out = cook(s);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    const RecordDb db = open(out->server);
    // Point every second kid's (empty) list at its sibling's list: forward and in bounds, but walking it
    // would visit 2^24 nodes.
    std::vector<u8> m = out->server;
    ValueView node = db.find(7).value.field("root");
    int shared = 0;
    while (node.field("kids").size() == 2) {
        const ValueView a = node.field("kids")[0];
        const ValueView b = node.field("kids")[1];
        const usize aSpan = offsetOf(db, a.field("kids").data());
        const usize bSpan = offsetOf(db, b.field("kids").data());
        const i32 offA = hrdb::load<i32>(m.data() + aSpan);
        const u32 countA = hrdb::load<u32>(m.data() + aSpan + 4);
        if (countA == 0) break;
        hrdb::store<i32>(m.data() + bSpan, static_cast<i32>(aSpan + static_cast<usize>(offA) - bSpan));
        hrdb::store<u32>(m.data() + bSpan + 4, countA);
        node = a;
        ++shared;
    }
    CHECK(shared == 23);
    hrdb::seal(m);
    auto r = RecordDb::openBytes(m, registry());
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Corrupt);
    CHECK(r.error().message.find("shared or overlaps") != std::string::npos);
}

TEST_CASE("loader: a database cooked against another schema, or for another audience, is refused") {
    const std::vector<u8> full = smallDb(CookAudience::Client);
    // Unknown record type: an empty registry.
    refl::TypeRegistry empty;
    auto none = RecordDb::openBytes(full, empty);
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().code == ErrorCode::NotFound);
    // Another layout: change the first type entry's layout hash.
    std::vector<u8> m = full;
    const usize typesAt = hrdb::kRootOffset + hrdb::kRootTypes + static_cast<usize>(hrdb::load<i32>(m.data() + hrdb::kRootOffset));
    m[typesAt + 8] ^= 1;
    hrdb::seal(m);
    auto other = RecordDb::openBytes(m, registry());
    REQUIRE_FALSE(other.ok());
    CHECK(other.error().code == ErrorCode::VersionMismatch);
    CHECK(other.error().message.find("recook") != std::string::npos);
    // A future format version.
    m = full;
    m[4] = 1;
    hrdb::seal(m);
    auto version = RecordDb::openBytes(m, registry());
    REQUIRE_FALSE(version.ok());
    CHECK(version.error().code == ErrorCode::VersionMismatch);
    // The wrong cook.
    auto wrong = RecordDb::openBytes(full, registry(), {CookAudience::Server});
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().code == ErrorCode::InvalidArgument);
    // Too short / empty.
    CHECK(RecordDb::openBytes({}, registry()).errorCode() == ErrorCode::Corrupt);
}

} // namespace
