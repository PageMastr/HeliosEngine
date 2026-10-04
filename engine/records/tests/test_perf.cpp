// Records budgets (nightly, label perf):
//   * cook ≤ 600 ms per 1,000 records: AAA-CNT-5 (01 §3.7) allows 60 s for 100k records;
//   * open (memory map + full validation) ≤ 20 µs per record: AAA-CNT-5 loads 100k records in ≤ 2 s
//     (02 §5.7 repeats it);
//   * find() by RecordId ≤ 1 µs mean (this module's own budget; the plan sets none).
// Measured on 20,000 ShipDef records (every encoding, 10 % of them inheriting from a template) plus
// their parts and tags. Timings are reported always and asserted only in optimized builds without
// sanitizers.
#include <chrono>
#include <format>
#include <random>

#include "test_util.h"

// Sanitizers can also come from raw compiler flags (the fuzz build): GCC defines __SANITIZE_ADDRESS__,
// Clang answers __has_feature.
#define HELIOS_RECORDS_SANITIZED 0
#if defined(HELIOS_SANITIZERS_ENABLED) || defined(__SANITIZE_ADDRESS__)
#undef HELIOS_RECORDS_SANITIZED
#define HELIOS_RECORDS_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer) || __has_feature(memory_sanitizer)
#undef HELIOS_RECORDS_SANITIZED
#define HELIOS_RECORDS_SANITIZED 1
#endif
#endif
#if defined(NDEBUG) && !HELIOS_RECORDS_SANITIZED
#define HELIOS_RECORDS_ASSERT_BUDGETS 1
#else
#define HELIOS_RECORDS_ASSERT_BUDGETS 0
#endif

using namespace helios;
using namespace helios::records;
using namespace helios::records::test;
using Clock = std::chrono::steady_clock;

namespace {

f64 msSince(Clock::time_point t0) { return std::chrono::duration<f64, std::milli>(Clock::now() - t0).count(); }

std::vector<SourceRecord> syntheticProject(usize ships) {
    std::vector<SourceRecord> out;
    const refl::TypeInfo& part = type("test.records.PartDef");
    const refl::TypeInfo& shipT = type("test.records.ShipDef");
    constexpr usize kParts = 100;
    for (usize i = 0; i < kParts; ++i) {
        out.push_back(source(std::format("records/part/p{}.hrec", i), part,
                             std::format(R"({{"$rid": {}, "$name": "part/p{}", "name": "loc:part.p{}", "mass": {}}})", 1000 + i, i, i, i * 2.5)));
    }
    for (usize i = 0; i < ships; ++i) {
        const usize rid = 100000 + i;
        std::string parent;
        if (i % 10 == 9) parent = std::format(R"("$parent": "ship/s{}", )", i - 1);
        out.push_back(source(
            std::format("records/ship/s{}.hrec", i), shipT,
            std::format(R"j({{"$rid": {}, "$name": "ship/s{}", {}"name": "loc:ship.s{}", "grade": "High", "perms": ["Read"], "mass": {},
  "hp": {}, "i32v": {}, "u64v": {}, "flag": true, "label": "Ship number {}", "ident": "s{}",
  "guid": "guid:00000000-0000-4000-8000-{:012x}", "owner": "ent:{}", "cooldown": "{}ms", "pos": [{}, 2, 3],
  "rot": [0, 0, 0, 1], "tint": [1, 1, 1, 1], "tags": ["Ship.Class.C{}", "Ship.Role.R{}"],
  "query": "all(Ship.Class)", "formula": "attr(self, Hull.Mass) * {}",
  "mounts": [{{"$key": "00000000-0000-4000-8000-{:012x}", "bone": "b{}", "force": {}}}],
  "slots": [{{"slot": "nose", "weight": 1}}, {{"slot": "tail", "weight": 2}}], "extras": ["x{}"],
  "parts": [{}, {}], "attrs": {{"Speed": {}, "Agility": 0.5}}, "labels": ["a", "b{}"], "nums": [1, 2, 3],
  "maybe": 1.5, "handling": {{"pitch": 1, "yaw": 2, "roll": 3}}, "mode": {{"Cruise": {{"speed": {}}}}},
  "nested": [[1, 2], [3]], "aiNotes": "notes {}", "aiHints": {{"Range": {}}}, "threat": "attr(self, Threat) + {}",
  "secretCode": {}}})j",
                        rid, i, parent, i, 1000.0 + static_cast<f64>(i), 50.0 + static_cast<f64>(i), -static_cast<i64>(i), i * 7, i, i, i, i,
                        i % 5000, i % 50, i % 20, i % 100, i, i, i, i * 3, i, 1000 + i % kParts, 1000 + (i * 7) % kParts, i, i, i % 300, i,
                        i % 1000, i % 100, i * 13)));
    }
    return out;
}

TEST_CASE("perf: cook 20,000 records <= 600 ms per 1,000; open <= 20 us per record; find <= 1 us") {
    constexpr usize kShips = 20'000;
    const std::vector<SourceRecord> sources = syntheticProject(kShips);
    const usize records = sources.size();

    auto t0 = Clock::now();
    auto out = cook(sources);
    const f64 cookMs = msSince(t0);
    REQUIRE_MESSAGE(out.ok(), (out.ok() ? "" : out.error().message));
    const f64 cookPer1000 = cookMs / (static_cast<f64>(records) / 1000.0);
    MESSAGE(std::format("cook: {} records in {:.1f} ms = {:.1f} ms per 1,000 (budget 600); client {} bytes, server {} bytes, {} tags, {} formulas",
                        records, cookMs, cookPer1000, out->client.size(), out->server.size(), out->stats.tags, out->stats.formulas));

    const fs::Path dir = fs::pathFromUtf8(HELIOS_RECORDS_TEST_WORK_DIR) / "perf";
    REQUIRE(writeCookOutput(*out, dir).ok());
    std::vector<f64> openMs;
    for (int run = 0; run < 3; ++run) {
        t0 = Clock::now();
        auto db = RecordDb::openFile(dir / fs::pathFromUtf8(kServerDbFile), registry());
        openMs.push_back(msSince(t0));
        REQUIRE(db.ok());
        CHECK(db->recordCount() == records);
    }
    std::sort(openMs.begin(), openMs.end());
    const f64 openUsPerRecord = openMs[1] * 1000.0 / static_cast<f64>(records);
    MESSAGE(std::format("open (map + validate) the server cook: {:.2f} ms median = {:.2f} us per record (budget 20)", openMs[1], openUsPerRecord));

    auto db = RecordDb::openFile(dir / fs::pathFromUtf8(kServerDbFile), registry());
    REQUIRE(db.ok());
    std::mt19937_64 rng(3);
    std::vector<refl::RecordId> ids(1 << 16);
    for (refl::RecordId& id : ids) id = 100000 + rng() % kShips;
    constexpr usize kLookups = 1'000'000;
    u64 sink = 0;
    t0 = Clock::now();
    for (usize i = 0; i < kLookups; ++i) sink += db->find(ids[i & (ids.size() - 1)]).id;
    const f64 findNs = msSince(t0) * 1e6 / static_cast<f64>(kLookups);
    CHECK(sink != 0);
    MESSAGE(std::format("find by RecordId: {:.1f} ns mean (budget 1000)", findNs));

#if HELIOS_RECORDS_ASSERT_BUDGETS
    CHECK(cookPer1000 <= 600.0);
    CHECK(openUsPerRecord <= 20.0);
    CHECK(findNs <= 1000.0);
#endif
}

} // namespace
