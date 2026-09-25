// U64Map, NetHandleTable and EntityRegistry.

#include <doctest/doctest.h>

#include <unordered_map>
#include <vector>

#include "helios/core/random.h"
#include "helios/ecs/registry.h"

using namespace helios;
using namespace helios::ecs;

TEST_CASE("ecs registry: U64Map matches std::unordered_map under random churn") {
    U64Map map(MemoryTag::Unknown, 4);
    std::unordered_map<u64, u64> ref;
    SplitMix64 rng(42);
    for (int i = 0; i < 200'000; ++i) {
        // Small key space forces long probe chains, collisions and backward-shift deletions.
        const u64 key = (rng.next() % 4096) + 1;
        const u64 op = rng.next() % 3;
        if (op == 0) {
            const bool fresh = map.insert(key, key * 7 + static_cast<u64>(i));
            CHECK(fresh == (ref.find(key) == ref.end()));
            ref[key] = key * 7 + static_cast<u64>(i);
        } else if (op == 1) {
            CHECK(map.erase(key) == (ref.erase(key) == 1));
        } else {
            auto it = ref.find(key);
            CHECK(map.find(key, ~0ull) == (it == ref.end() ? ~0ull : it->second));
        }
    }
    CHECK(map.size() == ref.size());
    usize visited = 0;
    map.forEach([&](u64 k, u64 v) {
        ++visited;
        CHECK(ref.at(k) == v);
    });
    CHECK(visited == ref.size());
    CHECK(map.find(0, 99) == 99);
    CHECK_FALSE(map.contains(0));
    map.clear();
    CHECK(map.size() == 0);
    CHECK_FALSE(map.contains(1));
}

TEST_CASE("ecs registry: U64Map memory is attributed to its tag") {
    const MemoryTag tag = registerMemoryTag("EcsTest.U64Map");
    const i64 before = memoryTagStats(tag).liveBytes;
    {
        U64Map map(tag, 1000);
        CHECK(memoryTagStats(tag).liveBytes > before);
        U64Map moved(std::move(map));
        moved.insert(5, 6);
        CHECK(moved.find(5) == 6);
    }
    CHECK(memoryTagStats(tag).liveBytes == before);
}

TEST_CASE("ecs registry: NetHandleTable issues FIFO slots with generations") {
    NetHandleTable table({.maxHandles = 4});
    const EntityId a(101), b(102), c(103), d(104), e(105);
    NetHandle ha = *table.allocate(a);
    NetHandle hb = *table.allocate(b);
    CHECK(ha.index() == 1);
    CHECK(hb.index() == 2);
    CHECK(ha.generation() == 1);
    CHECK(table.resolve(ha) == a);
    CHECK(table.liveCount() == 2);

    CHECK(table.release(ha));
    CHECK_FALSE(table.release(ha)); // stale
    CHECK_FALSE(table.resolve(ha).isValid());
    CHECK(table.release(hb));

    // Fresh slots come first (fewer than reuseDelay are waiting), then freed slots in FIFO order.
    NetHandle hc = *table.allocate(c);
    NetHandle hd = *table.allocate(d);
    CHECK(hc.index() == 3);
    CHECK(hd.index() == 4);
    NetHandle he = *table.allocate(e);
    CHECK(he.index() == 1);
    CHECK(he.generation() == 2);
    CHECK(he != ha);
    CHECK(table.resolve(ha) == EntityId()); // old handle to the same slot stays stale
    CHECK(table.resolve(he) == e);
    NetHandle hf = *table.allocate(EntityId(106));
    CHECK(hf.index() == 2);
    CHECK(table.allocate(EntityId(107)).errorCode() == ErrorCode::LimitExceeded);
}

TEST_CASE("ecs registry: NetHandle generations wrap without issuing generation 0") {
    NetHandleTable table({.maxHandles = 1, .reuseDelay = 0});
    for (int i = 0; i < 600; ++i) {
        NetHandle h = *table.allocate(EntityId(1000 + static_cast<u64>(i)));
        CHECK(h.generation() != 0);
        CHECK(h.isValid());
        CHECK(table.release(h));
    }
}

TEST_CASE("ecs registry: freed slots are recycled once the reuse delay is exceeded") {
    NetHandleTable table({.maxHandles = 1000, .reuseDelay = 3});
    std::vector<NetHandle> hs;
    for (u64 i = 0; i < 10; ++i) hs.push_back(*table.allocate(EntityId(i + 1)));
    for (int i = 0; i < 4; ++i) CHECK(table.release(hs[static_cast<size_t>(i)])); // 4 waiting > 3
    CHECK(table.allocate(EntityId(50))->index() == hs[0].index()); // oldest freed slot first
    CHECK(table.allocate(EntityId(51))->index() == 11);            // 3 waiting: fresh again
}

TEST_CASE("ecs registry: content-placed slots are reserved") {
    NetHandleTable table({.maxHandles = 100, .reservedCount = 10});
    NetHandle dyn = *table.allocate(EntityId(1));
    CHECK(dyn.index() == 11); // dynamic handles never land in the reserved range
    NetHandle placed = *table.allocateAt(5, EntityId(2));
    CHECK(placed.index() == 5);
    CHECK(table.allocateAt(5, EntityId(3)).errorCode() == ErrorCode::AlreadyExists);
    CHECK(table.allocateAt(11, EntityId(3)).errorCode() == ErrorCode::OutOfRange);
    CHECK(table.allocateAt(0, EntityId(3)).errorCode() == ErrorCode::OutOfRange);
    CHECK(table.handleAt(5) == placed);
    CHECK(table.release(placed));
    CHECK_FALSE(table.handleAt(5).isValid());
    // A released content slot is not recycled for dynamic entities.
    for (int i = 0; i < 20; ++i) CHECK(table.allocate(EntityId(100 + static_cast<u64>(i)))->index() > 10);
    NetHandle again = *table.allocateAt(5, EntityId(4));
    CHECK(again.generation() == placed.generation() + 1);
}

TEST_CASE("ecs registry: EntityRegistry bidirectional maps") {
    EntityRegistry reg({.maxHandles = 1000});
    const EntityId id1(0x1111), id2(0x2222);
    CHECK(reg.add(id1, Entity(501)).hasValue());
    CHECK(reg.add(id2, Entity(502)).hasValue());
    CHECK(reg.add(id1, Entity(503)).errorCode() == ErrorCode::AlreadyExists);
    CHECK(reg.add(EntityId(), Entity(504)).errorCode() == ErrorCode::InvalidArgument);
    const NetHandle h1 = *reg.assignHandle(id1);
    CHECK(reg.find(id1) == Entity(501));
    CHECK(reg.find(h1) == Entity(501));
    CHECK(reg.resolve(h1) == id1);
    CHECK(reg.assignHandle(EntityId(0x9999)).errorCode() == ErrorCode::NotFound);
    CHECK(reg.size() == 2);

    CHECK(reg.remove(id1, h1));
    CHECK_FALSE(reg.remove(id1, h1));
    CHECK_FALSE(reg.find(id1).isValid());
    CHECK_FALSE(reg.find(h1).isValid());
    CHECK(reg.find(id2) == Entity(502));
    CHECK(reg.memoryBytes() > 0);
}
