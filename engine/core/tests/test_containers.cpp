#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/containers.h"

using namespace helios;

TEST_CASE("small vector: inline storage then heap growth") {
    const i64 before = memoryTagStats(MemoryTag::Containers).liveBytes;
    {
        SmallVector<int, 4> v;
        CHECK(v.empty());
        CHECK(v.capacity() == 4);
        for (int i = 0; i < 4; ++i) v.push_back(i);
        CHECK(v.isInline());
        CHECK(memoryTagStats(MemoryTag::Containers).liveBytes == before);
        v.push_back(4);
        CHECK(!v.isInline());
        CHECK(v.capacity() >= 5);
        CHECK(memoryTagStats(MemoryTag::Containers).liveBytes > before); // tracked heap
        for (int i = 0; i < 5; ++i) CHECK(v[static_cast<usize>(i)] == i);
        CHECK(v.front() == 0);
        CHECK(v.back() == 4);
        v.pop_back();
        CHECK(v.size() == 4);
        int sum = 0;
        for (int x : v) sum += x;
        CHECK(sum == 6);
    }
    CHECK(memoryTagStats(MemoryTag::Containers).liveBytes == before);
}

TEST_CASE("small vector: non-trivial elements, copy and move") {
    SmallVector<std::string, 2> a{"one", "two"};
    CHECK(a.isInline());
    SmallVector<std::string, 2> b = a;
    b.push_back("three");
    CHECK(a.size() == 2);
    CHECK(b.size() == 3);
    CHECK(b[2] == "three");
    SmallVector<std::string, 2> c = std::move(b); // heap buffer is stolen
    CHECK(c.size() == 3);
    CHECK(b.empty());
    CHECK(b.isInline());
    SmallVector<std::string, 2> d = std::move(a); // inline elements are moved
    CHECK(d == SmallVector<std::string, 2>{"one", "two"});
    c = d;
    CHECK(c == d);
    d = SmallVector<std::string, 2>{"x"};
    CHECK(d.size() == 1);

    // Self-referencing push_back during growth must copy before reallocating.
    SmallVector<std::string, 2> s{"alpha", "beta"};
    s.push_back(s[0]);
    CHECK(s[2] == "alpha");
}

TEST_CASE("small vector: insert, erase, resize") {
    SmallVector<int, 8> v{1, 2, 4, 5};
    v.insert(v.begin() + 2, 3);
    CHECK(v == SmallVector<int, 8>{1, 2, 3, 4, 5});
    v.erase(v.begin());
    CHECK(v == SmallVector<int, 8>{2, 3, 4, 5});
    v.eraseUnordered(0);
    CHECK(v == SmallVector<int, 8>{5, 3, 4});
    v.resize(6);
    CHECK(v.size() == 6);
    CHECK(v[5] == 0);
    v.resize(1);
    CHECK(v.size() == 1);
    v.clear();
    CHECK(v.empty());
    SmallVector<int, 2> filled(5, 9);
    CHECK(filled.size() == 5);
    CHECK(filled[4] == 9);
}

TEST_CASE("ring buffer: FIFO with fixed capacity") {
    RingBuffer<std::string> r(3);
    CHECK(r.empty());
    CHECK(r.pushBack("a"));
    CHECK(r.pushBack("b"));
    CHECK(r.pushBack("c"));
    CHECK(r.full());
    CHECK(!r.pushBack("d"));
    CHECK(r[0] == "a");
    CHECK(r.back() == "c");
    r.pushBackOverwrite("d");
    CHECK(r.front() == "b");
    CHECK(r[2] == "d");
    std::string out;
    CHECK(r.popFront(out));
    CHECK(out == "b");
    r.popBack();
    CHECK(r.size() == 1);
    CHECK(r.front() == "c");
    std::string joined;
    r.pushBack("e");
    r.pushBack("f");
    r.forEach([&](const std::string& s) { joined += s; });
    CHECK(joined == "cef");
    r.clear();
    CHECK(!r.popFront(out));
}

TEST_CASE("mpmc queue: bounded, lossless under contention") {
    MpmcQueue<u64> q(1000);
    CHECK(q.capacity() == 1024);
    u64 v = 0;
    CHECK(!q.tryPop(v));
    for (u64 i = 0; i < 1024; ++i) CHECK(q.tryPush(i));
    CHECK(!q.tryPush(u64{9999}));
    for (u64 i = 0; i < 1024; ++i) {
        CHECK(q.tryPop(v));
        CHECK(v == i);
    }

    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr u64 kPerProducer = 100'000;
    std::atomic<u64> sum{0};
    std::atomic<u64> popped{0};
    std::vector<std::atomic<u8>> seen(kProducers * kPerProducer);
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p] {
            for (u64 i = 0; i < kPerProducer; ++i) {
                const u64 value = static_cast<u64>(p) * kPerProducer + i;
                while (!q.tryPush(value)) std::this_thread::yield();
            }
        });
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&] {
            u64 value;
            while (popped.load() < kProducers * kPerProducer) {
                if (q.tryPop(value)) {
                    seen[value].fetch_add(1);
                    sum.fetch_add(value);
                    popped.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    const u64 n = kProducers * kPerProducer;
    CHECK(sum.load() == n * (n - 1) / 2);
    bool exactlyOnce = true;
    for (auto& s : seen) exactlyOnce &= s.load() == 1;
    CHECK(exactlyOnce);

    MpmcQueue<std::string> strings(4);
    CHECK(strings.tryPush(std::string("left in queue"))); // destroyed by the queue destructor
}

TEST_CASE("spsc queue: ordered hand-off between two threads") {
    SpscQueue<u64> q(256);
    CHECK(q.capacity() == 256);
    u64 v;
    CHECK(!q.tryPop(v));
    constexpr u64 kItems = 1'000'000;
    std::thread producer([&] {
        for (u64 i = 0; i < kItems; ++i) {
            while (!q.tryPush(i)) std::this_thread::yield();
        }
    });
    bool inOrder = true;
    for (u64 expected = 0; expected < kItems;) {
        if (q.tryPop(v)) {
            inOrder &= v == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(inOrder);
    CHECK(q.sizeApprox() == 0);
    for (u64 i = 0; i < 256; ++i) CHECK(q.tryPush(i));
    CHECK(!q.tryPush(u64{1}));

    SpscQueue<std::string> strings(2);
    CHECK(strings.tryPush(std::string("leftover")));
}
