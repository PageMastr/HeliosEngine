#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/core/random.h"
#include "helios/core/time.h"

using namespace helios;
using namespace helios::jobs;

TEST_CASE("jobs: small lambdas are stored inline, large ones on the heap") {
    int x = 0;
    Job small([&x] { ++x; });
    CHECK(small.isValid());
    CHECK(small.isInline());
    Job moved = std::move(small);
    CHECK(!small.isValid());
    moved.invoke();
    CHECK(x == 1);
    CHECK(!moved.isValid());

    const u64 heapBefore = jobHeapAllocationCount();
    std::array<u64, 32> big{};
    big[31] = 5;
    u64 out = 0;
    Job large([big, &out] { out = big[31]; });
    CHECK(!large.isInline());
    CHECK(jobHeapAllocationCount() == heapBefore + 1);
    Job relocated = std::move(large);
    relocated.invoke();
    CHECK(out == 5);

    // Destroying an un-run job destroys its callable (no leak, no call).
    auto token = std::make_shared<int>(1);
    {
        Job pending([token] { *token = 2; });
        CHECK(token.use_count() == 2);
    }
    CHECK(token.use_count() == 1);
    CHECK(*token == 1);
}

TEST_CASE("jobs: default worker count and thread identity") {
    JobSystem js;
    CHECK(js.workerCount() == std::max<u32>(1, hardwareThreadCount() - 1));
    CHECK(!js.isWorkerThread());
    CHECK(js.currentWorkerIndex() == -1);
    std::atomic<int> insideWorker{0};
    Counter c;
    for (int i = 0; i < 64; ++i) {
        js.run(
            [&] {
                const i32 idx = js.currentWorkerIndex();
                if (js.isWorkerThread() && idx >= 0 && idx < static_cast<i32>(js.workerCount())) {
                    insideWorker.fetch_add(1);
                }
            },
            &c);
    }
    // Poll instead of wait(): this thread must not help, so every job runs on a worker.
    while (!c.isDone()) sleepMillis(1);
    CHECK(insideWorker.load() == 64);
    CHECK(js.stats().executed >= 64);
}

TEST_CASE("jobs: one million tiny jobs from an external thread") {
    JobSystem js;
    const u64 heapBefore = jobHeapAllocationCount();
    std::atomic<u64> sum{0};
    Counter counter;
    constexpr u64 kJobs = 1'000'000;
    Stopwatch sw;
    for (u64 i = 0; i < kJobs; ++i) js.run([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }, &counter);
    js.wait(counter);
    const f64 ms = sw.elapsedMillis();
    CHECK(sum.load() == kJobs * (kJobs - 1) / 2);
    CHECK(jobHeapAllocationCount() == heapBefore); // no per-job heap allocation
    MESSAGE("1M external jobs: " << ms << " ms on " << js.workerCount() << " workers");
}

TEST_CASE("jobs: one million jobs spawned from inside jobs") {
    JobSystem js;
    std::atomic<u64> count{0};
    Counter outer;
    constexpr int kParents = 1000;
    constexpr int kChildren = 1000;
    Stopwatch sw;
    for (int p = 0; p < kParents; ++p) {
        js.run(
            [&] {
                Counter inner;
                for (int c = 0; c < kChildren; ++c) js.run([&count] { count.fetch_add(1, std::memory_order_relaxed); }, &inner);
                js.wait(inner); // nested wait from a worker: must help, never deadlock
            },
            &outer);
    }
    js.wait(outer);
    CHECK(count.load() == static_cast<u64>(kParents) * kChildren);
    MESSAGE("1M nested jobs: " << sw.elapsedMillis() << " ms");
}

namespace {
u64 fib(JobSystem& js, u32 n) {
    if (n < 2) return n;
    if (n < 12) return fib(js, n - 1) + fib(js, n - 2);
    u64 a = 0;
    u64 b = 0;
    Counter c;
    js.run([&] { a = fib(js, n - 1); }, &c);
    js.run([&] { b = fib(js, n - 2); }, &c);
    js.wait(c);
    return a + b;
}
} // namespace

TEST_CASE("jobs: deeply nested waits (recursive fork/join) complete") {
    JobSystem js;
    for (int round = 0; round < 3; ++round) CHECK(fib(js, 24) == 46368);
    JobSystemDesc single;
    single.workerCount = 1; // nested waits must also work with a single worker
    JobSystem one(single);
    CHECK(fib(one, 20) == 6765);
}

TEST_CASE("jobs: parallelFor visits every index exactly once and sums correctly") {
    JobSystem js;
    constexpr u64 kCount = 1'000'003;
    std::vector<std::atomic<u8>> visits(kCount);
    js.parallelFor(0, kCount, 0, [&](u64 i) { visits[i].fetch_add(1, std::memory_order_relaxed); });
    CHECK(std::all_of(visits.begin(), visits.end(), [](const std::atomic<u8>& v) { return v.load() == 1; }));

    for (u64 grain : {u64(1), u64(7), u64(1000), u64(5'000'000)}) {
        std::atomic<u64> sum{0};
        js.parallelFor(0, 100'000, grain, [&](u64 b, u64 e) {
            u64 local = 0;
            for (u64 i = b; i < e; ++i) local += i;
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        CHECK(sum.load() == 100'000ull * 99'999ull / 2);
    }
    // Offset ranges and empty ranges.
    std::atomic<u64> offsetSum{0};
    js.parallelFor(10, 20, 3, [&](u64 i) { offsetSum.fetch_add(i); });
    CHECK(offsetSum.load() == 145);
    js.parallelFor(5, 5, 1, [&](u64) { offsetSum.fetch_add(1000); });
    CHECK(offsetSum.load() == 145);

    // Grains larger than the range (including ~0 for "don't split") run the whole range once.
    // Regression: (count + grain - 1) / grain overflowed and skipped or overran the range.
    for (u64 grain : {u64(10), u64(11), ~u64(0), ~u64(0) - 3}) {
        std::atomic<u64> sum{0};
        std::atomic<u64> calls{0};
        js.parallelFor(5, 15, grain, [&](u64 b, u64 e) {
            calls.fetch_add(1);
            for (u64 i = b; i < e; ++i) sum.fetch_add(i);
        });
        CHECK(sum.load() == 95);
        CHECK(calls.load() == 1);
    }
    std::atomic<u64> tailSum{0};
    js.parallelFor(~u64(0) - 10, ~u64(0), 4, [&](u64 i) { tailSum.fetch_add(~u64(0) - i); });
    CHECK(tailSum.load() == 55); // 10 + 9 + ... + 1, chunk ends computed without overflow

    // parallelFor from inside a job (nested).
    Counter c;
    std::atomic<u64> nested{0};
    js.run([&] { js.parallelFor(0, 1000, 10, [&](u64 i) { nested.fetch_add(i); }); }, &c);
    js.wait(c);
    CHECK(nested.load() == 499'500);
}

TEST_CASE("jobs: higher priorities run first") {
    JobSystemDesc desc;
    desc.workerCount = 1;
    JobSystem js(desc);
    ManualResetEvent gate;
    std::atomic<bool> gateRunning{false};
    Counter counter;
    js.run(
        [&] {
            gateRunning.store(true);
            gate.wait();
        },
        &counter);
    while (!gateRunning.load()) sleepMillis(1);

    std::mutex mutex;
    std::vector<int> order;
    auto record = [&](int v) {
        std::lock_guard lock(mutex);
        order.push_back(v);
    };
    js.run([&] { record(2); }, &counter, Priority::Low);
    js.run([&] { record(1); }, &counter, Priority::Normal);
    js.run([&] { record(0); }, &counter, Priority::High);
    js.run([&] { record(1); }, &counter, Priority::Normal);
    gate.set();
    // Poll without helping so only the single worker executes (and orders) the jobs.
    while (!counter.isDone()) sleepMillis(1);
    CHECK(order == std::vector<int>{0, 1, 1, 2});
}

TEST_CASE("jobs: task graph respects dependencies") {
    JobSystem js;
    std::atomic<u32> clock{0};
    std::array<std::atomic<u32>, 4> stamp{};
    TaskGraph g;
    const auto a = g.addNode("A", [&] { stamp[0] = ++clock; });
    const auto b = g.addNode("B", [&] { stamp[1] = ++clock; }, Priority::High);
    const auto c = g.addNode("C", [&] { stamp[2] = ++clock; });
    const auto d = g.addNode("D", [&] { stamp[3] = ++clock; });
    REQUIRE(g.addEdge(a, b));
    REQUIRE(g.addEdge(a, c));
    REQUIRE(g.addEdge(b, d));
    REQUIRE(g.addEdge(c, d));
    REQUIRE(g.addEdge(c, d)); // duplicate edge ignored
    REQUIRE(g.validate());
    const auto order = g.topologicalOrder();
    REQUIRE(order);
    CHECK(*order == std::vector<TaskGraph::NodeId>{a, b, c, d});
    for (int run = 0; run < 50; ++run) {
        clock = 0;
        REQUIRE(g.run(js));
        const u32 sa = stamp[0].load(), sb = stamp[1].load(), sc = stamp[2].load(), sd = stamp[3].load();
        CHECK(sa < sb);
        CHECK(sa < sc);
        CHECK(sb < sd);
        CHECK(sc < sd);
        CHECK(clock.load() == 4);
    }
    CHECK(g.nodeName(c) == "C");
}

TEST_CASE("jobs: large random DAGs execute in a valid order") {
    JobSystem js;
    Random rng(2024);
    constexpr u32 kNodes = 300;
    TaskGraph g;
    std::vector<std::atomic<u32>> stamp(kNodes);
    std::atomic<u32> clock{0};
    for (u32 i = 0; i < kNodes; ++i) g.addNode("n" + std::to_string(i), [&stamp, &clock, i] { stamp[i] = ++clock; });
    std::vector<std::pair<u32, u32>> edges;
    for (u32 i = 1; i < kNodes; ++i) {
        const int fanIn = static_cast<int>(rng.range(0, 4));
        for (int k = 0; k < fanIn; ++k) {
            const u32 from = static_cast<u32>(rng.index(i));
            edges.emplace_back(from, i);
            REQUIRE(g.addEdge(from, i));
        }
    }
    for (int run = 0; run < 10; ++run) {
        clock = 0;
        REQUIRE(g.run(js));
        CHECK(clock.load() == kNodes);
        bool allOrdered = true;
        for (const auto& [from, to] : edges) allOrdered &= stamp[from].load() < stamp[to].load();
        CHECK(allOrdered);
    }
}

TEST_CASE("jobs: task graph rejects cycles and bad edges") {
    JobSystem js;
    TaskGraph g;
    const auto a = g.addNode("alpha", [] {});
    const auto b = g.addNode("beta", [] {});
    const auto c = g.addNode("gamma", [] {});
    REQUIRE(g.addEdge(a, b));
    REQUIRE(g.addEdge(b, c));
    REQUIRE(g.addEdge(c, b));
    const auto v = g.validate();
    CHECK(!v);
    CHECK(v.error().code == ErrorCode::InvalidState);
    CHECK((v.error().message.find("beta") != std::string::npos || v.error().message.find("gamma") != std::string::npos));
    CHECK(!g.run(js));
    CHECK(g.addEdge(a, a).error().code == ErrorCode::InvalidArgument);
    CHECK(g.addEdge(a, 99).error().code == ErrorCode::InvalidArgument);
    g.clear();
    CHECK(g.nodeCount() == 0);
    CHECK(g.run(js));
}

TEST_CASE("jobs: background pool keeps blocking work off the frame workers") {
    JobSystemDesc desc;
    desc.backgroundThreadCount = 2;
    JobSystem js(desc);
    BackgroundPool* bg = js.background();
    REQUIRE(bg != nullptr);
    CHECK(bg->threadCount() == 2);

    // Background tasks block (simulated IO) until the frame work is done. Gated on an event rather
    // than a sleep so the check cannot flake on a slow or overloaded machine.
    ManualResetEvent ioComplete;
    Counter slow;
    std::atomic<int> slowDone{0};
    for (int i = 0; i < 4; ++i) {
        bg->run(
            [&] {
                ioComplete.wait();
                slowDone.fetch_add(1);
            },
            &slow);
    }
    // Frame jobs complete while every background thread is blocked.
    Counter frame;
    std::atomic<int> frameDone{0};
    for (int i = 0; i < 2000; ++i) js.run([&] { frameDone.fetch_add(1); }, &frame);
    js.wait(frame);
    CHECK(frameDone.load() == 2000);
    CHECK(slowDone.load() == 0);
    ioComplete.set();
    bg->wait(slow);
    CHECK(slowDone.load() == 4);

    std::atomic<int> idleCount{0};
    for (int i = 0; i < 10; ++i) bg->run([&] { idleCount.fetch_add(1); }, nullptr, Priority::Low);
    bg->waitIdle();
    CHECK(idleCount.load() == 10);
    CHECK(bg->pendingCount() == 0);
}

TEST_CASE("jobs: background pool wait sees counters shared with frame jobs") {
    // Regression: BackgroundPool::wait slept on the pool's condition variable only, so a counter
    // finished by a JobSystem job (which never notifies it) hung the waiter forever.
    JobSystem js;
    BackgroundPool pool(1, "SharedCounterBg");
    for (int round = 0; round < 20; ++round) {
        Counter shared;
        ManualResetEvent frameGate;
        pool.run([] {}, &shared);                   // finishes first
        js.run([&] { frameGate.wait(); }, &shared); // finishes last, on a frame worker
        Thread opener("Opener", [&] {
            sleepMillis(2);
            frameGate.set();
        });
        pool.wait(shared);
        CHECK(shared.isDone());
    }
}

TEST_CASE("jobs: standalone background pool drains on destruction") {
    std::atomic<int> done{0};
    {
        BackgroundPool pool(3, "TestBg");
        for (int i = 0; i < 50; ++i) pool.run([&] { done.fetch_add(1); });
    }
    CHECK(done.load() == 50);
}

TEST_CASE("jobs: clean shutdown drains queued work, repeatedly") {
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> done{0};
        {
            JobSystemDesc desc;
            desc.workerCount = 1 + static_cast<u32>(round % 4);
            JobSystem js(desc);
            for (int i = 0; i < 1000; ++i) {
                js.run([&] {
                    done.fetch_add(1);
                    if (done.load() % 100 == 0) js.run([&] { done.fetch_add(1); }); // work spawning work
                });
            }
        } // destructor drains, then joins
        CHECK(done.load() >= 1000);
    }
}

TEST_CASE("jobs: waitIdle and tryRunOne") {
    JobSystem js;
    std::atomic<int> done{0};
    for (int i = 0; i < 500; ++i) js.run([&] { done.fetch_add(1); }, nullptr, Priority::Low);
    js.waitIdle();
    CHECK(done.load() == 500);
    CHECK(!js.tryRunOne()); // nothing queued any more; must not block
    const JobSystemStats s = js.stats();
    CHECK(s.executed >= 500);
}

TEST_CASE("jobs: many jobs waiting on one counter do not overflow the helping thread's stack") {
    // Every job waits on a gate that opens only later. Helping used to run the next waiting job
    // nested inside the current wait, once per job: 10k such jobs overflowed a 1 MiB (Windows
    // default) stack and 200k overflowed Linux's 8 MiB. Nesting is now bounded (limitedWaits).
    for (u32 workers : {1u, 0u}) {
        JobSystemDesc desc;
        desc.workerCount = workers; // 0 = default count
        JobSystem js(desc);
        Counter gate(1);
        Counter all;
        std::atomic<u32> done{0};
        constexpr u32 kJobs = 200'000;
        for (u32 i = 0; i < kJobs; ++i) {
            js.run(
                [&] {
                    js.wait(gate);
                    done.fetch_add(1, std::memory_order_relaxed);
                },
                &all);
        }
        Stopwatch sw;
        while (js.stats().limitedWaits == 0 && sw.elapsedMillis() < 20'000) sleepMillis(1);
        CHECK(js.stats().limitedWaits > 0); // the nesting limit engaged instead of recursing on
        CHECK(done.load() == 0);
        gate.decrement();
        js.wait(all);
        CHECK(done.load() == kJobs);
    }
}

TEST_CASE("jobs: a deeply nested waiter still runs the children of its own job") {
    // Past the nesting limit a waiter only runs jobs its current job spawned; with one worker and
    // no helping caller that is the only way the children can run, so this must not deadlock.
    JobSystemDesc desc;
    desc.workerCount = 1;
    JobSystem js(desc);
    std::atomic<u64> leaves{0};
    std::function<void(u32)> recurse = [&](u32 depth) {
        if (depth == 0) {
            leaves.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Counter c;
        js.run([&, depth] { recurse(depth - 1); }, &c);
        js.run([&, depth] { recurse(depth - 1); }, &c);
        js.wait(c);
    };
    // A chain far deeper than the help limit (kMaxHelpDepth = 256) plus a wide fan-out.
    Counter root;
    std::function<void(u32)> chain = [&](u32 depth) {
        if (depth == 0) {
            recurse(6);
            return;
        }
        Counter c;
        js.run([&, depth] { chain(depth - 1); }, &c);
        js.wait(c);
    };
    js.run([&] { chain(300); }, &root);
    // Poll (do not help) so that only the single worker runs the chain.
    Stopwatch sw;
    while (!root.isDone() && sw.elapsedMillis() < 60'000) sleepMillis(1);
    REQUIRE(root.isDone());
    CHECK(leaves.load() == 64);
    CHECK(js.stats().limitedWaits > 0);
}

TEST_CASE("jobs: stress mix of nested waits, parallelFor and graphs") {
    JobSystem js;
    for (int iteration = 0; iteration < 5; ++iteration) {
        std::atomic<u64> total{0};
        Counter outer;
        for (int p = 0; p < 32; ++p) {
            js.run(
                [&] {
                    js.parallelFor(0, 256, 16, [&](u64 i) { total.fetch_add(i); });
                    Counter inner;
                    for (int c = 0; c < 16; ++c) js.run([&] { total.fetch_add(1); }, &inner, Priority::High);
                    js.wait(inner);
                },
                &outer, static_cast<Priority>(p % 3));
        }
        js.wait(outer);
        CHECK(total.load() == 32ull * (256ull * 255ull / 2 + 16ull));
    }
}
