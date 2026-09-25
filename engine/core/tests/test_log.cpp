#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/log.h"
#include "helios/core/thread.h"

using namespace helios;

HELIOS_LOG_CHANNEL(LogTestAlpha, "TestAlpha");

namespace {

// Not registered until first use: exercises name-based overrides applied at registration.
constinit log::Channel g_lateChannel{"TestLateChannel"};

/// Replaces all sinks for the duration of a test and restores them afterwards.
struct SinkScope {
    std::vector<std::shared_ptr<log::Sink>> saved = log::sinks();
    log::Level savedLevel = log::level();
    SinkScope() { log::clearSinks(); }
    ~SinkScope() {
        log::clearSinks();
        for (auto& s : saved) log::addSink(s);
        log::setLevel(savedLevel);
    }
};

struct Captured {
    log::Level level;
    std::string channel;
    std::string message;
    std::string file;
    u32 line;
    u64 threadId;
    std::string threadName;
};

struct CaptureSink final : log::Sink {
    std::mutex mutex;
    std::vector<Captured> records;
    void write(const log::Record& r) override {
        std::lock_guard lock(mutex);
        records.push_back({r.level, r.channel ? r.channel->name() : "", std::string(r.message), r.location.file,
                           r.location.line, r.threadId, std::string(r.threadName)});
    }
};

} // namespace

TEST_CASE("log: messages reach sinks with channel, level, location and thread") {
    SinkScope scope;
    log::setLevel(log::Level::Trace);
    auto sink = std::make_shared<CaptureSink>();
    log::addSink(sink);

    HELIOS_LOG_INFO("hello {} {}", 42, "world");
    const u32 expectedLine = __LINE__ + 1;
    HELIOS_LOG_WARN(LogTestAlpha, "value={:.2f}", 1.5);
    HELIOS_LOG_ERROR(LogTestAlpha, "plain");

    REQUIRE(sink->records.size() == 3);
    CHECK(sink->records[0].level == log::Level::Info);
    CHECK(sink->records[0].channel == "General");
    CHECK(sink->records[0].message == "hello 42 world");
    CHECK(sink->records[1].channel == "TestAlpha");
    CHECK(sink->records[1].message == "value=1.50");
    CHECK(sink->records[1].line == expectedLine);
    CHECK(sink->records[1].file.find("test_log.cpp") != std::string::npos);
    CHECK(sink->records[2].level == log::Level::Error);
    CHECK(sink->records[0].threadId == currentThreadId());
}

TEST_CASE("log: global level, channel overrides and sink levels filter") {
    SinkScope scope;
    auto sink = std::make_shared<CaptureSink>();
    log::addSink(sink);

    log::setLevel(log::Level::Warn);
    HELIOS_LOG_INFO(LogTestAlpha, "dropped");
    HELIOS_LOG_WARN(LogTestAlpha, "kept");
    CHECK(sink->records.size() == 1);

    // Channel override may lower the threshold below the global level.
    log::setChannelLevel("testalpha", log::Level::Debug);
    CHECK(LogTestAlpha.levelOverride() == log::Level::Debug);
    HELIOS_LOG_INFO(LogTestAlpha, "kept by override");
    HELIOS_LOG_INFO("dropped on General");
    CHECK(sink->records.size() == 2);
    log::clearChannelLevel("TestAlpha");
    CHECK(!LogTestAlpha.levelOverride().has_value());

    sink->setLevel(log::Level::Error);
    HELIOS_LOG_WARN(LogTestAlpha, "dropped by sink level");
    CHECK(sink->records.size() == 2);

    log::setLevel(log::Level::Off);
    HELIOS_LOG_ERROR("nothing when off");
    CHECK(sink->records.size() == 2);
}

TEST_CASE("log: level overrides set by name apply to channels that register later") {
    SinkScope scope;
    log::setLevel(log::Level::Trace);
    auto sink = std::make_shared<CaptureSink>();
    log::addSink(sink);
    CHECK(log::findChannel("TestLateChannel") == nullptr);
    log::setChannelLevel("TestLateChannel", log::Level::Error);
    HELIOS_LOG_WARN(g_lateChannel, "filtered by the pending override");
    HELIOS_LOG_ERROR(g_lateChannel, "passes");
    CHECK(sink->records.size() == 1);
    CHECK(log::findChannel("testlatechannel") == &g_lateChannel);
    log::clearChannelLevel("TestLateChannel");

    bool sawCore = false;
    log::forEachChannel([&](log::Channel& c) { sawCore |= std::string_view(c.name()) == "Core"; });
    CHECK(sawCore);
}

TEST_CASE("log: stripped levels are type-checked but not evaluated") {
    int evaluated = 0;
    HELIOS_LOG_DISCARD_(Info, "never {}", ++evaluated);
    CHECK(evaluated == 0);
#if HELIOS_LOG_COMPILE_LEVEL > HELIOS_LOG_LEVEL_TRACE
    HELIOS_LOG_TRACE("stripped {}", ++evaluated);
    CHECK(evaluated == 0);
#endif
}

TEST_CASE("log: level names parse and print") {
    CHECK(log::levelName(log::Level::Warn) == "WARN");
    CHECK(log::parseLevel("warning") == log::Level::Warn);
    CHECK(log::parseLevel("ERROR") == log::Level::Error);
    CHECK(log::parseLevel("off") == log::Level::Off);
    CHECK(!log::parseLevel("loud").has_value());
}

TEST_CASE("log: formatRecord renders UTC date, time, level and channel") {
    log::Record r;
    r.level = log::Level::Warn;
    r.channel = &LogTestAlpha;
    r.message = "engine on fire";
    r.location = {"/src/engine/thing.cpp", 12, "f"};
    r.timestampNs = 1'700'000'000'123'000'000; // 2023-11-14 22:13:20.123 UTC
    r.threadId = 77;
    r.threadName = "Main";
    const std::string line = log::formatRecord(r);
    CHECK(line == "2023-11-14 22:13:20.123 WARN  [TestAlpha] <77:Main> engine on fire  (thing.cpp:12)");
    log::FormatOptions brief;
    brief.date = false;
    brief.thread = false;
    brief.sourceForWarnings = false;
    CHECK(log::formatRecord(r, brief) == "22:13:20.123 WARN  [TestAlpha] engine on fire");
}

TEST_CASE("log: ring buffer sink keeps the newest entries with sequence numbers") {
    SinkScope scope;
    log::setLevel(log::Level::Trace);
    auto ring = std::make_shared<log::RingBufferSink>(4);
    log::addSink(ring);
    for (int i = 0; i < 10; ++i) HELIOS_LOG_INFO(LogTestAlpha, "msg {}", i);
    CHECK(ring->size() == 4);
    CHECK(ring->lastSequence() == 10);
    auto all = ring->snapshot();
    REQUIRE(all.size() == 4);
    CHECK(all.front().message == "msg 6");
    CHECK(all.back().message == "msg 9");
    CHECK(all.back().sequence == 10);
    CHECK(all.back().channel == "TestAlpha");
    auto recent = ring->snapshot(8);
    REQUIRE(recent.size() == 2);
    CHECK(recent[0].message == "msg 8");
    ring->clear();
    CHECK(ring->size() == 0);
}

TEST_CASE("log: file sink writes formatted lines") {
    auto dir = fs::createUniqueTempDirectory("helios-log");
    REQUIRE(dir);
    const fs::Path path = *dir / "test.log";
    {
        SinkScope scope;
        log::setLevel(log::Level::Trace);
        auto file = std::make_shared<log::FileSink>(path);
        REQUIRE(file->isOpen());
        log::addSink(file);
        HELIOS_LOG_INFO(LogTestAlpha, "first line");
        HELIOS_LOG_ERROR(LogTestAlpha, "second line");
        log::flush();
    }
    auto text = fs::readTextFile(path);
    REQUIRE(text);
    CHECK(text->find("INFO  [TestAlpha]") != std::string::npos);
    CHECK(text->find("first line\n") != std::string::npos);
    CHECK(text->find("second line") != std::string::npos);
    (void)fs::removeAll(*dir);
}

TEST_CASE("log: concurrent logging from many threads loses nothing") {
    SinkScope scope;
    log::setLevel(log::Level::Trace);
    std::atomic<int> count{0};
    log::addSink(std::make_shared<log::CallbackSink>([&](const log::Record&) { count.fetch_add(1); }));
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) HELIOS_LOG_DEBUG(LogTestAlpha, "t{} i{}", t, i);
        });
    }
    for (auto& th : threads) th.join();
#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_DEBUG
    CHECK(count.load() == kThreads * kPerThread);
#endif
}

TEST_CASE("log: a sink that logs does not recurse or deadlock") {
    SinkScope scope;
    log::setLevel(log::Level::Trace);
    std::atomic<int> calls{0};
    log::addSink(std::make_shared<log::CallbackSink>([&](const log::Record&) {
        calls.fetch_add(1);
        HELIOS_LOG_ERROR("from inside a sink"); // dropped
    }));
    HELIOS_LOG_INFO("outer");
    CHECK(calls.load() == 1);
}

TEST_CASE("log: sink management") {
    SinkScope scope;
    auto a = std::make_shared<CaptureSink>();
    auto b = std::make_shared<CaptureSink>();
    log::addSink(a);
    log::addSink(b);
    CHECK(log::sinks().size() == 2);
    CHECK(log::removeSink(a.get()));
    CHECK(!log::removeSink(a.get()));
    CHECK(log::sinks().size() == 1);
    log::addSink(std::make_shared<log::DebuggerSink>());
    log::setLevel(log::Level::Info);
    HELIOS_LOG_INFO("to b and the debugger sink");
    CHECK(b->records.size() == 1);
    CHECK(a->records.empty());
}
