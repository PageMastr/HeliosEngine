// Record files and record ids.

#include <doctest/doctest.h>

#include <set>

#include "test_types.h"

using namespace helios;
using namespace helios::refl;
using namespace rtest;

TEST_CASE("record: minted ids are non-zero 63-bit and unique") {
    std::set<RecordId> seen;
    for (int i = 0; i < 1000; ++i) {
        const RecordId id = mintRecordId();
        CHECK(isValidRecordId(id));
        CHECK(seen.insert(id).second);
    }
    CHECK_FALSE(isValidRecordId(0));
    CHECK_FALSE(isValidRecordId(1ull << 63));
}

TEST_CASE("record: canonical files put $-keys first and round-trip") {
    RecordHeader header;
    header.rid = 4611686018427387905ull;
    header.name = "hull/kestrel";
    header.parent = "hull/base";
    header.comment = "tuned for PvE";
    const Inner v{3, "fields", 1.5f};
    const std::string text = writeRecord(v, header);
    CHECK(text ==
          "{\n"
          "  \"$rid\": 4611686018427387905,\n"
          "  \"$name\": \"hull/kestrel\",\n"
          "  \"$parent\": \"hull/base\",\n"
          "  \"$comment\": \"tuned for PvE\",\n"
          "  \"a\": 3,\n"
          "  \"s\": \"fields\"\n"
          "}\n");
    Inner back;
    RecordHeader h2;
    ReadCtx ctx;
    REQUIRE(readRecord(text, back, h2, ctx));
    CHECK(back == v);
    CHECK(h2 == header);
    CHECK(ctx.warnings().empty());

    RecordHeader minimal;
    minimal.rid = 7;
    CHECK(writeRecord(Inner{}, minimal) == "{\n  \"$rid\": 7\n}\n");
}

TEST_CASE("record: $rid is mandatory and validated") {
    Inner v;
    RecordHeader h;
    ReadCtx ctx;
    CHECK_FALSE(readRecord("{\"a\": 1}", v, h, ctx));
    CHECK_FALSE(readRecord("{\"$rid\": 0}", v, h, ctx));
    CHECK_FALSE(readRecord("{\"$rid\": 9223372036854775808}", v, h, ctx));
    CHECK_FALSE(readRecord("{\"$rid\": \"12\"}", v, h, ctx));
    CHECK_FALSE(readRecord("{\"$rid\": 5, \"$name\": 3}", v, h, ctx));
    CHECK_FALSE(readRecord("[]", v, h, ctx));
    REQUIRE(readRecord("// comment\n{\"$rid\": 5, \"a\": 2,}", v, h, ctx));
    CHECK(h.rid == 5);
    CHECK(v.a == 2);
}
