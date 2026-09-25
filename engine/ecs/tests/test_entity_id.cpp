// EntityId kinds, the block-id layout (golden vectors shared with Go's pkg/idgen), the
// AllocateIdBlocks rule, EntityIdMinter (uniqueness, ordering, refills, retirement, stalls,
// failures, async delivery, concurrency, determinism) and NetHandle packing.

#include <doctest/doctest.h>

#include <yyjson.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/guid.h"
#include "helios/ecs/entity_id.h"

using namespace helios;
using namespace helios::ecs;

namespace {

/// Scripted IdBlockSource: the AllocateIdBlocks rule on a caller-controlled clock, with injectable
/// failures, Busy answers and bad prefixes.
class TestSource final : public IdBlockSource {
public:
    u64 nowMs = 1000;
    i64 last = -1;
    ErrorCode fail = ErrorCode::Ok;
    bool nonIncreasing = false;
    u32 calls = 0;
    std::vector<u32> requested;

    Result<void> allocateIdBlocks(u32 n, std::vector<u64>& out) override {
        ++calls;
        requested.push_back(n);
        if (fail != ErrorCode::Ok) return Error{fail, "scripted failure"};
        std::vector<u64> p(n);
        last = allocateBlockPrefixes(last, nowMs, p);
        if (nonIncreasing) p[0] = 0;
        out.insert(out.end(), p.begin(), p.end());
        return {};
    }
};

u64 parseU64(std::string_view s) {
    u64 v = 0;
    REQUIRE(std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc());
    return v;
}
i64 parseI64(std::string_view s) {
    i64 v = 0;
    REQUIRE(std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc());
    return v;
}
std::string_view str(yyjson_val* v) {
    REQUIRE(yyjson_is_str(v));
    return std::string_view(yyjson_get_str(v), yyjson_get_len(v));
}

} // namespace

TEST_CASE("ecs ids: kinds and packing") {
    CHECK(EntityId().kind() == EntityIdKind::Invalid);
    CHECK(EntityId(1).kind() == EntityIdKind::Runtime);
    CHECK(EntityId((1ull << 63) - 1).kind() == EntityIdKind::Runtime);
    CHECK(EntityId::contentPlaced(0x1234).kind() == EntityIdKind::ContentPlaced);
    CHECK(EntityId::contentPlaced(0).isValid()); // zero payload remapped
    CHECK(EntityId::clientLocal(7).kind() == EntityIdKind::ClientLocal);
    CHECK((EntityId::contentPlaced(~0ull).value >> 62) == 2);

    const NetHandle h = NetHandle::make(0xABCDEF, 0x42);
    CHECK(h.index() == 0xABCDEFu);
    CHECK(h.generation() == 0x42);
    CHECK(h.isValid());
    CHECK_FALSE(NetHandle().isValid());
    CHECK(sizeof(NetHandle) == 4);
    CHECK(sizeof(EntityId) == 8);
}

TEST_CASE("ecs ids: content-placed ids are stable hashes of the GUID") {
    const Guid g(0x0123456789abcdefull, 0xfedcba9876543210ull);
    const EntityId a = EntityId::fromContentGuid(g);
    const EntityId b = EntityId::fromContentGuid(g);
    CHECK(a == b);
    CHECK(a.kind() == EntityIdKind::ContentPlaced);
    CHECK(EntityId::fromContentGuid(Guid(1, 2)) != EntityId::fromContentGuid(Guid(1, 3)));
    // Golden value: content ids are persisted in cooked containers, so the hash must never change.
    CHECK(a.value == EntityId::contentPlaced(helios::hash64(g.toBytes().data(), 16, helios::fnv1a64("helios.entity"))).value);
}

TEST_CASE("ecs ids: block-id layout matches the shared golden vectors") {
    using L = BlockIdLayout;
    static_assert(L::kBlockSize == 131072);
    static_assert(L::kEpochUnixMs == 1767225600000);
    // services/testdata/vectors/block_ids.json (Go pkg/idgen checks the same values).
    struct Vec {
        u64 prefix;
        u32 shard;
        u32 offset;
        u64 id;
    };
    const Vec vectors[] = {
        {0, 0, 0, 0},
        {1, 0, 0, 4194304},
        {0, 1, 0, 131072},
        {0, 0, 1, 1},
        {0, 0, 131071, 131071},
        {22809600000, 3, 12345, 95670396518805561},
        {2199023255551, 31, 131071, 9223372036854775807},
    };
    for (const Vec& v : vectors) {
        CAPTURE(v.id);
        CHECK(composeBlockId(v.prefix, v.shard, v.offset).value == v.id);
        CHECK(decodeBlockId(EntityId(v.id)) == BlockIdParts{v.prefix, v.shard, v.offset});
    }
    // Out-of-range fields are truncated to their width (Go idgen.Compose).
    CHECK(composeBlockId(0, 32, 131072).value == 0);
    CHECK((composeBlockId(L::kMaxPrefix, L::kMaxShard, L::kBlockSize - 1).value >> 63) == 0);
    CHECK(blockPrefixNow() > 0);
    CHECK(blockPrefixNow() < L::kMaxPrefix);

    // The AllocateIdBlocks rule: last = max(last + n, now); prefixes = last-n+1 .. last.
    u64 p[16] = {};
    CHECK(allocateBlockPrefixes(-1, 1000, std::span<u64>(p, 2)) == 1000);
    CHECK((p[0] == 999 && p[1] == 1000));
    CHECK(allocateBlockPrefixes(1000, 1000, std::span<u64>(p, 2)) == 1002);
    CHECK((p[0] == 1001 && p[1] == 1002));
    CHECK(allocateBlockPrefixes(1002, 900, std::span<u64>(p, 1)) == 1003); // clock stepped back
    CHECK(p[0] == 1003);
    CHECK(allocateBlockPrefixes(1003, 5000, std::span<u64>(p, 16)) == 5000);
    CHECK((p[0] == 4985 && p[15] == 5000));
}

#ifdef HELIOS_ECS_BLOCK_ID_VECTORS
TEST_CASE("ecs ids: golden vector file shared with Go is honoured") {
    if (!fs::exists(fs::pathFromUtf8(HELIOS_ECS_BLOCK_ID_VECTORS))) {
        MESSAGE("golden vector file not present in this checkout; the inline copy above still ran");
        return;
    }
    Result<std::string> text = fs::readTextFile(fs::pathFromUtf8(HELIOS_ECS_BLOCK_ID_VECTORS));
    REQUIRE(text.hasValue());
    yyjson_doc* doc = yyjson_read(text->data(), text->size(), 0);
    REQUIRE(doc);
    yyjson_val* root = yyjson_doc_get_root(doc);
    CHECK(parseI64(str(yyjson_obj_get(root, "epoch_unix_ms"))) == BlockIdLayout::kEpochUnixMs);
    CHECK(yyjson_get_uint(yyjson_obj_get(root, "block_size")) == BlockIdLayout::kBlockSize);
    usize vectors = 0, allocations = 0;
    yyjson_val* v = nullptr;
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(root, "vectors"));
    while ((v = yyjson_arr_iter_next(&it))) {
        const u64 prefix = parseU64(str(yyjson_obj_get(v, "prefix")));
        const u32 shard = static_cast<u32>(yyjson_get_uint(yyjson_obj_get(v, "shard")));
        const u32 offset = static_cast<u32>(yyjson_get_uint(yyjson_obj_get(v, "offset")));
        const u64 id = parseU64(str(yyjson_obj_get(v, "id")));
        CAPTURE(id);
        CHECK(composeBlockId(prefix, shard, offset).value == id);
        CHECK(decodeBlockId(EntityId(id)) == BlockIdParts{prefix, shard, offset});
        ++vectors;
    }
    it = yyjson_arr_iter_with(yyjson_obj_get(root, "allocations"));
    while ((v = yyjson_arr_iter_next(&it))) {
        const i64 last = parseI64(str(yyjson_obj_get(v, "last")));
        const u32 n = static_cast<u32>(yyjson_get_uint(yyjson_obj_get(v, "n")));
        const u64 now = parseU64(str(yyjson_obj_get(v, "now")));
        std::vector<u64> got(n);
        CHECK(allocateBlockPrefixes(last, now, got) == parseI64(str(yyjson_obj_get(v, "new_last"))));
        std::vector<u64> want;
        yyjson_val* pv = nullptr;
        yyjson_arr_iter pit = yyjson_arr_iter_with(yyjson_obj_get(v, "prefixes"));
        while ((pv = yyjson_arr_iter_next(&pit))) want.push_back(parseU64(str(pv)));
        CHECK(got == want);
        ++allocations;
    }
    yyjson_doc_free(doc);
    CHECK(vectors >= 7);
    CHECK(allocations >= 4);
}
#endif

TEST_CASE("ecs ids: minter validation") {
    TestSource src;
    CHECK(EntityIdMinter::validate({.shard = 31, .source = &src}).hasValue());
    CHECK(EntityIdMinter::validate({.shard = 32, .source = &src}).errorCode() == ErrorCode::OutOfRange);
    CHECK(EntityIdMinter::validate({.source = &src, .hold = 0}).errorCode() == ErrorCode::OutOfRange);
    CHECK(EntityIdMinter::validate({.source = &src, .hold = 17}).errorCode() == ErrorCode::OutOfRange);
    CHECK(EntityIdMinter::validate({.shard = 1}).errorCode() == ErrorCode::InvalidArgument);
}

TEST_CASE("ecs ids: minting walks blocks in order and refills at half use") {
    TestSource src;
    src.nowMs = 1000;
    EntityIdMinter ids({.shard = 3, .source = &src, .clock = [&] { return src.nowMs; }});
    // First allocation fetches `hold` blocks (prefixes 999, 1000) and starts at offset 0.
    const EntityId first = ids.allocate();
    CHECK(decodeBlockId(first) == BlockIdParts{999, 3, 0});
    CHECK(src.calls == 1);
    CHECK(ids.stats().stalls == 1);
    CHECK(ids.remaining() == 2 * BlockIdLayout::kBlockSize - 1);

    EntityId prev = first;
    u32 gaps = 0;
    for (u32 i = 1; i < BlockIdLayout::kBlockSize / 2; ++i) {
        const EntityId id = ids.allocate();
        gaps += id.value == prev.value + 1 ? 0u : 1u;
        prev = id;
    }
    CHECK(gaps == 0);
    ids.maintain(); // current block now half used: one fresh spare left -> ask for one more
    CHECK(src.calls == 2);
    CHECK(src.requested.back() == 1);
    CHECK(ids.remaining() == 2 * BlockIdLayout::kBlockSize + BlockIdLayout::kBlockSize / 2);
    ids.maintain(); // enough fresh blocks: no call
    CHECK(src.calls == 2);

    // Crossing into the next block: strictly increasing, new prefix, offset 0.
    for (u32 i = BlockIdLayout::kBlockSize / 2; i < BlockIdLayout::kBlockSize; ++i) prev = ids.allocate();
    CHECK(decodeBlockId(prev) == BlockIdParts{999, 3, BlockIdLayout::kBlockSize - 1});
    const EntityId next = ids.allocate();
    CHECK(next.value > prev.value);
    CHECK(decodeBlockId(next) == BlockIdParts{1000, 3, 0});
    CHECK(ids.stats().allocated == BlockIdLayout::kBlockSize + 1);
    CHECK(ids.stats().stalls == 1);
    CHECK(ids.lastPrefix() == 1001);
}

TEST_CASE("ecs ids: blocks retire after an hour once a fresher block is held") {
    TestSource src;
    src.nowMs = 10'000;
    EntityIdMinter ids({.source = &src, .hold = 1, .retireMs = 3'600'000, .clock = [&] { return src.nowMs; }});
    CHECK(ids.prime().hasValue());
    const EntityId a = ids.allocate();
    CHECK(decodeBlockId(a).prefix == 10'000);
    // The only block is past its lifetime but nothing fresher is held: keep minting from it.
    src.fail = ErrorCode::IoError;
    src.nowMs += 3'600'000;
    ids.maintain();
    CHECK(ids.stats().refillFailures == 1);
    CHECK(decodeBlockId(ids.allocate()).prefix == 10'000);
    ids.maintain(); // back-off: no second call within a second
    CHECK(src.calls == 2);
    // The control plane recovers: the fresh block replaces the retired one at the next maintain.
    src.fail = ErrorCode::Ok;
    src.nowMs += 1000;
    ids.maintain();
    CHECK(ids.stats().refills == 2);
    ids.maintain();
    const EntityId b = ids.allocate();
    CHECK(decodeBlockId(b).prefix == src.last);
    CHECK(decodeBlockId(b).offset == 0);
    CHECK(b.value > a.value);
    CHECK(ids.stats().blocksRetired == 1);
    CHECK(ids.stats().allocated == 3);
}

TEST_CASE("ecs ids: exhaustion, failed sources and asynchronous delivery") {
    TestSource src;
    EntityIdMinter ids({.source = &src, .hold = 1, .clock = [&] { return src.nowMs; }});
    src.fail = ErrorCode::Timeout;
    CHECK_FALSE(ids.allocate().isValid()); // no block at all: invalid id, counted
    CHECK(ids.stats().exhausted == 1);

    // A source returning non-increasing prefixes is rejected as a whole.
    src.fail = ErrorCode::Ok;
    CHECK(ids.prime().hasValue());
    const EntityId a = ids.allocate();
    CHECK(a.isValid());
    const u64 bad[] = {5, 4};
    CHECK_FALSE(ids.addBlocks(bad).hasValue());
    const u64 overflow[] = {BlockIdLayout::kMaxPrefix + 1};
    CHECK(ids.addBlocks(overflow).errorCode() == ErrorCode::OutOfRange);

    // Busy = asynchronous request in flight: no re-ask until delivered or timed out.
    src.fail = ErrorCode::Busy;
    src.nowMs += 2000; // past the back-off of the failure above
    ids.maintain(); // current block is fresh and unused: nothing to do
    for (u32 i = 1; i < BlockIdLayout::kBlockSize; ++i) (void)ids.allocate();
    const u32 callsBefore = src.calls;
    ids.maintain();
    CHECK(src.calls == callsBefore + 1);
    ids.maintain();
    ids.maintain();
    CHECK(src.calls == callsBefore + 1); // still in flight
    CHECK_FALSE(ids.allocate().isValid()); // spent, delivery pending
    const u64 delivered[] = {static_cast<u64>(src.last) + 50};
    CHECK(ids.addBlocks(delivered).hasValue());
    const EntityId b = ids.allocate();
    CHECK(decodeBlockId(b).prefix == delivered[0]);
    CHECK(b.value > a.value);
}

TEST_CASE("ecs ids: concurrent minting is unique and per-thread increasing") {
    LocalIdBlockSource src; // wall clock
    EntityIdMinter ids({.shard = 5, .source = &src, .hold = 2});
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100'000; // crosses several blocks under contention
    std::vector<std::vector<u64>> out(kThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> monotonic{true};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            out[t].reserve(kPerThread);
            u64 prev = 0;
            for (int i = 0; i < kPerThread; ++i) {
                const u64 v = ids.allocate().value;
                if (v <= prev) monotonic = false;
                prev = v;
                out[t].push_back(v);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(monotonic.load());
    std::vector<u64> all;
    for (auto& v : out) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
    CHECK(all.size() == static_cast<size_t>(kThreads * kPerThread));
    CHECK(std::count_if(all.begin(), all.end(), [](u64 v) { return v == 0 || decodeBlockId(EntityId(v)).shard != 5; }) == 0);
    CHECK(ids.stats().allocated == static_cast<u64>(kThreads * kPerThread));
}

TEST_CASE("ecs ids: a simulated clock makes the id sequence deterministic; restarts never reissue") {
    auto run = [](i64 lastPrefix, i64* lastOut) {
        u64 sim = 0;
        LocalIdBlockSource src({.clock = [&] { return sim; }, .lastPrefix = lastPrefix});
        EntityIdMinter ids({.source = &src, .clock = [&] { return sim; }});
        std::vector<u64> seq;
        for (int tick = 0; tick < 20; ++tick) {
            sim += 50;
            ids.maintain();
            for (int i = 0; i < 20'000; ++i) seq.push_back(ids.allocate().value);
        }
        if (lastOut) *lastOut = src.lastPrefix();
        return seq;
    };
    i64 last = -1;
    const std::vector<u64> a = run(-1, &last);
    const std::vector<u64> b = run(-1, nullptr);
    CHECK(a == b);
    CHECK(std::is_sorted(a.begin(), a.end()));
    // A restart with the persisted row value mints strictly above everything before, even though the
    // simulated clock starts over at 0.
    const std::vector<u64> c = run(last, nullptr);
    CHECK(c.front() > a.back());
    // LocalIdBlockSource enforces the per-call limit.
    LocalIdBlockSource src;
    std::vector<u64> out;
    CHECK(src.allocateIdBlocks(0, out).errorCode() == ErrorCode::InvalidArgument);
    CHECK(src.allocateIdBlocks(17, out).errorCode() == ErrorCode::InvalidArgument);
    CHECK(src.allocateIdBlocks(16, out).hasValue());
    CHECK(out.size() == 16);
    CHECK(std::is_sorted(out.begin(), out.end()));
}

TEST_CASE("ecs ids: a clock at zero never yields EntityId 0") {
    // Prefix 0 on shard 0 starts with the all-zero id, which is EntityId's invalid value.
    LocalIdBlockSource src({.clock = [] { return u64(0); }});
    EntityIdMinter ids({.source = &src, .clock = [] { return u64(0); }});
    const EntityId first = ids.allocate();
    CHECK(first.isValid());
    CHECK(decodeBlockId(first) == BlockIdParts{0, 0, 1});
    CHECK(ids.allocate().value == first.value + 1);
    CHECK(ids.remaining() == 2ull * BlockIdLayout::kBlockSize - 3); // offset 0 is skipped, not reissued
    // Other shards use offset 0 of prefix 0 normally.
    LocalIdBlockSource src2({.clock = [] { return u64(0); }});
    EntityIdMinter shard1({.shard = 1, .source = &src2, .clock = [] { return u64(0); }});
    CHECK(decodeBlockId(shard1.allocate()) == BlockIdParts{0, 1, 0});
}

TEST_CASE("ecs ids: client-local allocator") {
    ClientLocalIdAllocator local;
    const EntityId a = local.allocate();
    const EntityId b = local.allocate();
    CHECK(a.kind() == EntityIdKind::ClientLocal);
    CHECK(b.value == a.value + 1);
}
