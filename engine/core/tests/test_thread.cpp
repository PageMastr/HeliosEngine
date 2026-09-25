#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "helios/core/thread.h"
#include "helios/core/time.h"

using namespace helios;

TEST_CASE("thread: named threads report their id and name") {
    CHECK(hardwareThreadCount() >= 1);
    CHECK(currentThreadId() != 0);
    CHECK(currentThreadId() == currentThreadId());

    std::string seenName;
    ThreadId seenId = 0;
    {
        Thread t("Helios Test Worker", [&] {
            seenName = std::string(currentThreadName());
            seenId = currentThreadId();
        });
        CHECK(t.id() != 0);
        CHECK(t.id() != currentThreadId());
        CHECK(t.name() == "Helios Test Worker");
        t.join();
        CHECK(seenId == t.id());
    }
    CHECK(seenName == "Helios Test Worker");

    setCurrentThreadName("TestMain");
    CHECK(currentThreadName() == "TestMain");
}

TEST_CASE("thread: move-only callables, options, move and join on destruction") {
    std::atomic<int> ran{0};
    auto owned = std::make_unique<int>(5);
    Thread::Options options;
    options.priority = ThreadPriority::Low;
    options.affinityMask = 1; // best effort; may be refused by the container's cpuset
    Thread a("MoveOnly", options, [&ran, p = std::move(owned)] { ran.fetch_add(*p); });
    Thread b = std::move(a);
    CHECK(!a.joinable());
    CHECK(b.joinable());
    b.join();
    CHECK(ran.load() == 5);
    {
        Thread c("Scoped", [&] { ran.fetch_add(1); });
    } // destructor joins
    CHECK(ran.load() == 6);
    (void)setCurrentThreadPriority(ThreadPriority::Normal);
}

TEST_CASE("thread: spin lock provides mutual exclusion") {
    SpinLock lock;
    u64 counter = 0;
    std::vector<Thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back("Spin", [&] {
            for (int i = 0; i < 50'000; ++i) {
                std::lock_guard guard(lock);
                ++counter;
            }
        });
    }
    threads.clear();
    CHECK(counter == 400'000);
    CHECK(lock.try_lock());
    CHECK(!lock.try_lock());
    lock.unlock();
}

TEST_CASE("thread: semaphore hands out exactly the released permits") {
    Semaphore sem(0);
    CHECK(!sem.tryAcquire());
    CHECK(!sem.tryAcquireFor(std::chrono::milliseconds(5)));
    std::atomic<int> consumed{0};
    std::vector<Thread> consumers;
    for (int t = 0; t < 4; ++t) {
        consumers.emplace_back("Consumer", [&] {
            for (int i = 0; i < 250; ++i) {
                sem.acquire();
                consumed.fetch_add(1);
            }
        });
    }
    for (int i = 0; i < 1000; ++i) sem.release();
    consumers.clear();
    CHECK(consumed.load() == 1000);
    sem.release(3);
    CHECK(sem.tryAcquire());
    CHECK(sem.tryAcquireFor(std::chrono::milliseconds(1)));
    CHECK(sem.tryAcquire());
    CHECK(!sem.tryAcquire());
}

TEST_CASE("thread: manual reset event wakes all waiters and stays set") {
    ManualResetEvent event;
    CHECK(!event.isSet());
    CHECK(!event.waitFor(std::chrono::milliseconds(2)));
    std::atomic<int> woke{0};
    std::vector<Thread> waiters;
    for (int t = 0; t < 4; ++t) {
        waiters.emplace_back("Waiter", [&] {
            event.wait();
            woke.fetch_add(1);
        });
    }
    sleepMillis(5);
    CHECK(woke.load() == 0);
    event.set();
    waiters.clear();
    CHECK(woke.load() == 4);
    CHECK(event.isSet());
    CHECK(event.waitFor(std::chrono::milliseconds(0)));
    event.reset();
    CHECK(!event.isSet());
}
