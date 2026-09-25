// flecs OS API integration: memory through the ECS mimalloc heap, logging through HELIOS_LOG,
// monotonic time, and flecs task threads running as JobSystem jobs.

#include <doctest/doctest.h>

#include <mimalloc.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "flecs_internal.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/core/time.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/os_api.h"
#include "helios/ecs/world.h"
#include "test_types.h"

using namespace helios;
using namespace helios::ecs;

TEST_CASE("ecs os api: installed by World and idempotent") {
    World world;
    CHECK(flecsOsApiInstalled());
    CHECK(installFlecsOsApi());
    CHECK(ecs_os_has_heap());
    CHECK(ecs_os_has_threading());
    CHECK(ecs_os_has_task_support());
    CHECK(ecs_os_has_time());
    CHECK(ecs_get_ctx(world.flecsWorld()) == &world);
}

TEST_CASE("ecs os api: flecs memory comes from the ECS heap and is tagged") {
    installFlecsOsApi();
    const i64 before = memoryTagStats(ecsMemoryTag()).liveBytes;
    void* p = ecs_os_malloc(1000);
    REQUIRE(p);
    CHECK(ecsHeap().owns(p));
    CHECK(mi_heap_of(p) == static_cast<mi_heap_t*>(ecsHeap().nativeHeap()));
    CHECK(memoryTagStats(ecsMemoryTag()).liveBytes >= before + 1000);
    p = ecs_os_realloc(p, 5000);
    CHECK(ecsHeap().owns(p));
    char* s = ecs_os_strdup("helios");
    CHECK(std::string(s) == "helios");
    ecs_os_free(s);
    ecs_os_free(p);
    CHECK(memoryTagStats(ecsMemoryTag()).liveBytes == before);

    const FlecsOsApiStats stats = flecsOsApiStats();
    CHECK(stats.mallocs >= 2);
    CHECK(stats.frees >= 2);
}

TEST_CASE("ecs os api: a world's memory returns to the tag when it is destroyed") {
    const i64 before = memoryTagStats(ecsMemoryTag()).liveBytes;
    i64 peak = 0;
    {
        World world;
        ecs_test::registerCommon(world);
        for (int i = 0; i < 5000; ++i) {
            const Entity e = world.spawn();
            world.set(e, ecs_test::Velocity{});
        }
        peak = memoryTagStats(ecsMemoryTag()).liveBytes;
        CHECK(peak > before + 5000 * static_cast<i64>(sizeof(ecs_test::Velocity)));
        CHECK(world.stats().ecsHeapLiveBytes > 0);
    }
    CHECK(memoryTagStats(ecsMemoryTag()).liveBytes == before);
}

TEST_CASE("ecs os api: flecs log messages reach Helios sinks on the ECS channel") {
    installFlecsOsApi();
    struct Captured {
        std::mutex m;
        std::vector<std::pair<log::Level, std::string>> records;
    };
    auto captured = std::make_shared<Captured>();
    auto sink = std::make_shared<log::CallbackSink>([captured](const log::Record& r) {
        if (r.channel && std::string_view(r.channel->name()) == "ECS") {
            std::lock_guard lock(captured->m);
            captured->records.emplace_back(r.level, std::string(r.message));
        }
    });
    log::addSink(sink);
    const u64 warningsBefore = flecsOsApiStats().logWarnings;
    ecs_os_api.log_(-2, __FILE__, __LINE__, "helios-flecs-warning");
    ecs_os_api.log_(-3, __FILE__, __LINE__, "helios-flecs-error");
    log::removeSink(sink.get());
    std::lock_guard lock(captured->m);
    REQUIRE(captured->records.size() == 2);
    CHECK(captured->records[0].first == log::Level::Warn);
    CHECK(captured->records[0].second == "helios-flecs-warning");
    CHECK(captured->records[1].first == log::Level::Error);
    CHECK(flecsOsApiStats().logWarnings == warningsBefore + 1);
}

TEST_CASE("ecs os api: flecs time is the Helios monotonic clock") {
    installFlecsOsApi();
    const u64 a = ecs_os_now();
    const u64 h = monotonicNanos();
    const u64 b = ecs_os_now();
    CHECK(a <= h);
    CHECK(h <= b);
    ecs_time_t t{};
    ecs_os_get_time(&t);
    CHECK(t.nanosec < 1'000'000'000u);
    const u64 s0 = monotonicNanos();
    ecs_os_sleep(0, 2'000'000);
    CHECK(monotonicNanos() - s0 >= 1'500'000);
}

namespace {
struct TaskProbe {
    std::mutex m;
    std::set<i32> workers;
    std::atomic<u64> rows{0};
    jobs::JobSystem* js = nullptr;
};

void probeSystem(ecs_iter_t* it) {
    auto* probe = static_cast<TaskProbe*>(it->ctx);
    {
        std::lock_guard lock(probe->m);
        probe->workers.insert(probe->js->currentWorkerIndex());
    }
    auto* counters = static_cast<ecs_test::Counter*>(ecs_field_w_size(it, sizeof(ecs_test::Counter), 0));
    for (int32_t i = 0; i < it->count; ++i) counters[i].value += 1;
    probe->rows.fetch_add(static_cast<u64>(it->count));
}
} // namespace

TEST_CASE("ecs os api: flecs task threads run as JobSystem jobs") {
    jobs::JobSystem js({.workerCount = 3});
    World world({.jobs = &js});
    world.registerComponent<ecs_test::Counter>();
    for (int i = 0; i < 4000; ++i) world.set(world.spawn(), ecs_test::Counter{});

    TaskProbe probe;
    probe.js = &js;
    ecs_system_desc_t sd = {};
    ecs_entity_desc_t ed = {};
    ed.name = "ProbeSystem";
    sd.entity = ecs_entity_init(world.flecsWorld(), &ed);
    sd.phase = EcsOnUpdate;
    sd.query.terms[0].id = world.id<ecs_test::Counter>();
    sd.callback = probeSystem;
    sd.ctx = &probe;
    sd.multi_threaded = true;
    REQUIRE(ecs_system_init(world.flecsWorld(), &sd) != 0);

    const FlecsOsApiStats before = flecsOsApiStats();
    world.setFlecsTaskThreads(4); // stage 0 on this thread + 3 task jobs
    for (int frame = 0; frame < 5; ++frame) world.progressFlecs(0.016f);
    const FlecsOsApiStats after = flecsOsApiStats();

    CHECK(probe.rows.load() == 5u * 4000u); // every entity processed exactly once per frame
    CHECK(after.tasksStarted - before.tasksStarted == 15);
    CHECK(after.tasksJoined - before.tasksJoined == 15);
    // Stage 0 runs on the (non-worker) test thread; the other stages run on job workers.
    CHECK(probe.workers.count(-1) == 1);
    CHECK(probe.workers.size() >= 2);
    u64 sum = 0;
    Query q(world, {Term{world.id<ecs_test::Counter>(), TermAccess::Read}});
    q.forEachChunk([&](ChunkView& c) {
        for (u32 r = 0; r < c.count(); ++r) sum += c.read<ecs_test::Counter>(0)[r].value;
    });
    CHECK(sum == 5u * 4000u);

    world.setFlecsTaskThreads(0);
    world.progressFlecs(0.016f);
    CHECK(probe.rows.load() == 6u * 4000u);
}

TEST_CASE("ecs os api: task threads are clamped to the job system and refused without one") {
    World single;
    single.setFlecsTaskThreads(8); // no JobSystem: stays single-threaded
    CHECK(ecs_get_stage_count(single.flecsWorld()) == 1);
    jobs::JobSystem js({.workerCount = 2});
    World world({.jobs = &js});
    world.setFlecsTaskThreads(16);
    CHECK(ecs_get_stage_count(world.flecsWorld()) == 3); // 2 workers + the calling thread
    world.progressFlecs(0.016f);
}
