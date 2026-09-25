#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "helios/core/handle.h"

using namespace helios;

namespace {

struct Tracked {
    static inline int alive = 0;
    std::string name;
    explicit Tracked(std::string n) : name(std::move(n)) { ++alive; }
    ~Tracked() { --alive; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};

struct MeshTag;
struct TextureTag;

struct MaybeThrows {
    int value;
    explicit MaybeThrows(int v) : value(v) {
        if (v < 0) throw std::runtime_error("constructor failed");
    }
};

} // namespace

TEST_CASE("handle: value semantics and packing") {
    Handle<MeshTag> h;
    CHECK(!h.isValid());
    CHECK(!h);
    const Handle<MeshTag> a(5, 3);
    CHECK(a.isValid());
    CHECK(a.index() == 5);
    CHECK(a.generation() == 3);
    CHECK(Handle<MeshTag>::fromBits(a.toBits()) == a);
    CHECK(a.toBits() == ((3ull << 32) | 5));
    static_assert(!std::is_convertible_v<Handle<MeshTag>, Handle<TextureTag>>);
    std::unordered_set<Handle<MeshTag>> set{a, Handle<MeshTag>(5, 4), a};
    CHECK(set.size() == 2);
}

TEST_CASE("handle pool: create, get, destroy and stale detection") {
    HandlePool<std::string> pool;
    const auto a = pool.create("alpha");
    const auto b = pool.create("beta");
    CHECK(pool.size() == 2);
    REQUIRE(pool.get(a));
    CHECK(*pool.get(a) == "alpha");
    CHECK(*pool.get(b) == "beta");

    CHECK(pool.destroy(a));
    CHECK(!pool.destroy(a)); // double destroy is detected
    CHECK(pool.get(a) == nullptr);
    CHECK(!pool.isValid(a));

    // The freed slot is reused with a new generation; the old handle stays stale.
    const auto c = pool.create("gamma");
    CHECK(c.index() == a.index());
    CHECK(c.generation() != a.generation());
    CHECK(pool.get(a) == nullptr);
    CHECK(*pool.get(c) == "gamma");
    CHECK(pool.get(HandlePool<std::string>::HandleType()) == nullptr);
    CHECK(pool.get(HandlePool<std::string>::HandleType(999, 1)) == nullptr);
}

TEST_CASE("handle pool: element addresses are stable while the pool grows") {
    HandlePool<int> pool;
    const auto first = pool.create(1);
    int* p = pool.get(first);
    std::vector<HandlePool<int>::HandleType> handles;
    for (int i = 0; i < 5000; ++i) handles.push_back(pool.create(i));
    CHECK(pool.get(first) == p);
    CHECK(*p == 1);
    CHECK(pool.size() == 5001);
    for (int i = 0; i < 5000; ++i) CHECK(*pool.get(handles[static_cast<usize>(i)]) == i);
}

TEST_CASE("handle pool: iteration visits live elements only") {
    HandlePool<int> pool;
    std::vector<HandlePool<int>::HandleType> handles;
    for (int i = 0; i < 10; ++i) handles.push_back(pool.create(i));
    for (int i = 0; i < 10; i += 2) CHECK(pool.destroy(handles[static_cast<usize>(i)]));
    int sum = 0;
    int count = 0;
    for (auto [handle, value] : pool) {
        CHECK(pool.get(handle) == &value);
        sum += value;
        ++count;
    }
    CHECK(count == 5);
    CHECK(sum == 1 + 3 + 5 + 7 + 9);
    int visits = 0;
    pool.forEach([&](auto, int& v) {
        v *= 10;
        ++visits;
    });
    CHECK(visits == 5);
    CHECK(*pool.get(handles[1]) == 10);
    const HandlePool<int>& cpool = pool;
    int constCount = 0;
    for (auto item : cpool) constCount += item.value > 0 ? 1 : 0;
    CHECK(constCount == 5);
}

TEST_CASE("handle pool: clear and destruction run destructors and invalidate handles") {
    Tracked::alive = 0;
    {
        HandlePool<Tracked, MeshTag> pool;
        const auto a = pool.create("a");
        const auto b = pool.create("b");
        CHECK(Tracked::alive == 2);
        pool.clear();
        CHECK(Tracked::alive == 0);
        CHECK(pool.empty());
        CHECK(pool.get(a) == nullptr);
        CHECK(pool.get(b) == nullptr);
        const auto c = pool.create("c");
        CHECK(pool.get(c)->name == "c");

        HandlePool<Tracked, MeshTag> moved = std::move(pool);
        CHECK(moved.get(c)->name == "c");
        CHECK(Tracked::alive == 1);
    }
    CHECK(Tracked::alive == 0);
}

TEST_CASE("handle pool: a throwing constructor leaves the pool unchanged") {
    // Regression: the slot was taken off the free list (or counted) before T's constructor ran, so a
    // throwing constructor leaked the slot and corrupted the free list.
    HandlePool<MaybeThrows> pool;
    const auto first = pool.create(1);
    CHECK(pool.destroy(first)); // slot 0 is now on the free list
    CHECK_THROWS_AS(pool.create(-1), std::runtime_error);
    CHECK(pool.size() == 0);
    const auto reused = pool.create(2);
    CHECK(reused.index() == first.index()); // the free slot was not lost
    CHECK(reused.generation() == first.generation() + 1);
    CHECK(pool.get(reused)->value == 2);

    CHECK_THROWS_AS(pool.create(-2), std::runtime_error); // fresh-slot path
    CHECK(pool.slotCount() == 1);
    const auto fresh = pool.create(3);
    CHECK(fresh.index() == 1);
    CHECK(pool.size() == 2);
    u32 visited = 0;
    for (auto [handle, value] : pool) {
        (void)handle;
        visited += value.value > 0 ? 1u : 0u;
    }
    CHECK(visited == 2);
}
