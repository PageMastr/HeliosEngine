// ecs_bench — RT-01 (02 §8.2), the benchmark that decides flecs vs a custom ECS (ADR-004):
//
//   RT-01 | 50k-entity zone, SERVER, 20 Hz (20k replicated + 30k placed, ~150 ECS archetypes, 5k
//   bodies in 12 grids). Engine stages <= 12 ms p99; 9k structural ops <= 1.5 ms; 50k×3 iteration
//   <= 0.4 ms on one thread; ECS <= 400 MB; <= 5,000 tables. Failure opens the custom-ECS ADR.
//
// The zone runs single-threaded (no JobSystem) and with 1, 2 and 4 job-system workers; every
// configuration must end in the same state hash. The 9k structural-op burst is measured once cold
// and kWarmBursts times warm (the verdict uses the warm median; the max is printed next to it), and
// the same operations are replayed on the raw flecs C API as the floor for any flecs wrapper. Also
// runs the Phase 0 spikes (SPIKES.md).
//
// ADR-004a's indicator M1 is printed with the verdict: the World burst divided by the raw-flecs
// burst of the same run, per configuration, for tag and for DontFragment toggles; the worst of
// them counts. Measure it with a Release build (asserts off): the linux-bench preset.
//
// Usage: ecs_bench [--ticks=N] [--warmup=N] [--quick] [--workers=0,1,2,4] [--no-spikes]
//                  [--spikes-only] [--seed=N] [--inframe-dontfragment] [--no-warmup-pass] [--m1-gate]
// Exit code: 0 = RT-01 PASS, 1 = FAIL. With --m1-gate: 0 = M1 PASS, 1 = FAIL (RT-01 still printed).

#include <mimalloc.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "bench_spikes.h"
#include "bench_zone.h"
#include "helios/core/cmdline.h"
#include "helios/core/jobs.h"
#include "helios/core/log.h"
#include "helios/core/memory.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "helios/ecs/heap.h"
#include "helios/ecs/os_api.h"
#include "helios/ecs/world.h"

using namespace helios;

namespace {

// RT-01 budgets.
constexpr f64 kTickP99BudgetMs = 12.0;
constexpr f64 kStructuralBudgetMs = 1.5;
constexpr f64 kIterateBudgetMs = 0.4;
constexpr f64 kEcsMemoryBudgetMb = 400.0;
constexpr u32 kTableBudget = 5000;
/// Warm 9k-op bursts measured per configuration (the verdict uses their median).
constexpr u32 kWarmBursts = 7;
/// ADR-004a M1: World burst / raw-flecs burst, same run, worst configuration and toggle storage.
constexpr f64 kM1Budget = 1.6;

void out(const std::string& s) {
    std::fputs(s.c_str(), stdout);
    std::fflush(stdout);
}

f64 percentile(std::vector<f64> v, f64 p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const f64 idx = p * static_cast<f64>(v.size() - 1);
    const usize lo = static_cast<usize>(idx);
    const usize hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - static_cast<f64>(lo));
}

struct RunResult {
    u32 workers = 0;
    f64 buildMs = 0;
    f64 p50 = 0, p95 = 0, p99 = 0, maxMs = 0, meanMs = 0;
    f64 iterateMs = 0;
    f64 iterateCollectMs = 0;
    u32 iterateChunks = 0;
    f64 structuralMs = 0;    // median warm 9k-op burst total
    f64 structuralMaxMs = 0; // worst warm 9k-op burst total
    std::vector<f64> warmTotals, warmToggles;
    bench::BurstResult burstRaw; // median of kWarmBursts raw flecs bursts
    f64 structuralRawMs = 0;
    f64 structuralDfMs = 0;    // median warm burst with DontFragment toggles
    f64 structuralRawDfMs = 0; // the same on raw flecs
    f64 m1Tag() const noexcept { return structuralRawMs > 0 ? structuralMs / structuralRawMs : 0; }
    f64 m1DontFragment() const noexcept { return structuralRawDfMs > 0 ? structuralDfMs / structuralRawDfMs : 0; }
    u32 tablesAfterBursts = 0;
    bench::BurstResult burstCold, burstWarm, burstWarmMax; // warm = per-phase median of kWarmBursts
    f64 ecsMb = 0, ecsPeakMb = 0;
    f64 rssMb = 0, rssPeakMb = 0;
    u32 tables = 0;
    u32 archetypes = 0;
    u32 entities = 0;
    u32 bodies = 0;
    u32 replicated = 0;
    u64 changesPerTick = 0;
    u64 changedEntitiesPerTick = 0;
    u64 structuralOpsPerTick = 0;
    u32 liveProjectiles = 0;
    u64 hash = 0;
    std::vector<ecs::SystemStats> systems;
    f64 syncMs[ecs::kStageCount] = {};
};

RunResult runZone(u32 workers, u32 ticks, u32 warmup, u64 seed, bool inFrameDontFragment, bool verbose) {
    RunResult r;
    r.workers = workers;
    std::unique_ptr<jobs::JobSystem> js;
    if (workers > 0) js = std::make_unique<jobs::JobSystem>(jobs::JobSystemDesc{.workerCount = workers});
    const MemoryTagStats memBefore = memoryTagStats(ecs::ecsMemoryTag());

    auto clock = std::make_shared<u64>(0);
    ecs::WorldDesc desc;
    desc.name = "rt01";
    desc.jobs = js.get();
    desc.shard = 1;
    desc.idClock = [clock] { return *clock; }; // simulated time: deterministic block ids
    desc.handles.reservedCount = 32768; // content-placed slots (container index tables)
    desc.relations.inFrameDontFragment = inFrameDontFragment;
    {
        ecs::World world(desc);
        bench::ZoneConfig cfg;
        cfg.seed = seed;
        bench::BenchZone zone(world, cfg);
        const Stopwatch build;
        zone.build();
        zone.registerSystems();
        r.buildMs = build.elapsedMillis();
        const bench::ZoneInfo& info = zone.info();
        r.archetypes = info.archetypes;
        r.entities = info.entities;
        r.bodies = info.bodies;
        r.replicated = info.replicated;

        std::vector<f64> tickMs;
        tickMs.reserve(ticks);
        u64 changes = 0, changedEntities = 0;
        u64 opsBefore = 0;
        for (u32 t = 0; t < warmup + ticks; ++t) {
            *clock += 50; // simulated 20 Hz clock for the ID blocks (deterministic)
            if (t == warmup) opsBefore = world.stats().structuralOpsApplied;
            const Stopwatch sw;
            zone.tick(0.05f);
            const f64 ms = sw.elapsedMillis();
            if (t >= warmup) {
                tickMs.push_back(ms);
                changes += zone.lastChanges().changes.size();
                changedEntities += zone.lastChanges().entityCount;
                for (u32 s = 0; s < ecs::kStageCount; ++s) {
                    r.syncMs[s] += static_cast<f64>(world.lastSyncNs(static_cast<ecs::Stage>(s))) * 1e-6;
                }
            }
        }
        r.structuralOpsPerTick = (world.stats().structuralOpsApplied - opsBefore) / std::max(1u, ticks);
        for (f64& s : r.syncMs) s /= std::max(1u, ticks);
        r.p50 = percentile(tickMs, 0.50);
        r.p95 = percentile(tickMs, 0.95);
        r.p99 = percentile(tickMs, 0.99);
        r.maxMs = tickMs.empty() ? 0 : *std::max_element(tickMs.begin(), tickMs.end());
        f64 sum = 0;
        for (f64 v : tickMs) sum += v;
        r.meanMs = tickMs.empty() ? 0 : sum / static_cast<f64>(tickMs.size());
        r.changesPerTick = changes / std::max(1u, ticks);
        r.changedEntitiesPerTick = changedEntities / std::max(1u, ticks);
        r.systems = world.allSystemStats();
        r.liveProjectiles = zone.liveProjectiles();
        r.hash = zone.stateHash();

        // 50k x 3 iteration, one thread (best of 30 after warm-up).
        f64 best = 1e30;
        u32 visited = 0;
        for (int rep = 0; rep < 40; ++rep) {
            u64 collectNs = 0;
            const Stopwatch sw;
            visited = zone.iterate3(&collectNs, &r.iterateChunks);
            const f64 ms = sw.elapsedMillis();
            if (rep >= 10 && ms < best) {
                best = ms;
                r.iterateCollectMs = static_cast<f64>(collectNs) * 1e-6;
            }
        }
        r.iterateMs = best;
        if (verbose) out(std::format("    iterate3 visited {} entities\n", visited));

        r.tables = world.stats().tableCount;
        const MemoryTagStats mem = memoryTagStats(ecs::ecsMemoryTag());
        r.ecsMb = static_cast<f64>(mem.liveBytes - memBefore.liveBytes) / (1024.0 * 1024.0);
        r.ecsPeakMb = static_cast<f64>(mem.peakBytes - memBefore.liveBytes) / (1024.0 * 1024.0);

        // 9k structural ops (after the measured ticks so they are unaffected).
        // Cold = the first burst (creates tag-combination tables); warm = median of the next
        // kWarmBursts sync points (steady state; the median filters scheduler noise on shared
        // machines, the max is reported next to it).
        // Callgrind (SPIKES.md §5.2) collects warm rounds 2..kWarmBursts and the raw bursts with the
        // same parities: round 1 still creates a few tables (NPCs regaining a different species).
        r.burstCold = zone.structuralBurst(0, false);
        std::vector<bench::BurstResult> warm;
        for (u32 round = 1; round <= kWarmBursts; ++round) warm.push_back(zone.structuralBurst(round, round >= 2));
        auto median = [&](f64 bench::BurstResult::*field) {
            std::vector<f64> v;
            for (const bench::BurstResult& b : warm) v.push_back(b.*field);
            return percentile(v, 0.5);
        };
        auto maximum = [&](f64 bench::BurstResult::*field) {
            f64 m = 0;
            for (const bench::BurstResult& b : warm) m = std::max(m, b.*field);
            return m;
        };
        r.burstWarm = warm.front();
        r.burstWarmMax = warm.front();
        for (f64 bench::BurstResult::*field : {&bench::BurstResult::createMs, &bench::BurstResult::destroyMs,
                                                &bench::BurstResult::toggleMs, &bench::BurstResult::toggleDontFragmentMs,
                                                &bench::BurstResult::recordMs}) {
            r.burstWarm.*field = median(field);
            r.burstWarmMax.*field = maximum(field);
        }
        std::vector<f64> totals;
        for (const bench::BurstResult& b : warm) totals.push_back(b.totalMs());
        r.structuralMs = percentile(totals, 0.5);
        std::vector<f64> dfTotals;
        for (const bench::BurstResult& b : warm) dfTotals.push_back(b.totalDontFragmentMs());
        r.structuralDfMs = percentile(dfTotals, 0.5);
        r.structuralMaxMs = *std::max_element(totals.begin(), totals.end());
        r.warmTotals = totals;
        r.warmToggles.clear();
        for (const bench::BurstResult& b : warm) r.warmToggles.push_back(b.toggleMs);
        r.tablesAfterBursts = world.stats().tableCount;
        // The same operations on the raw flecs C API (floor for any flecs-based wrapper).
        std::vector<bench::BurstResult> raw;
        // Rounds alternate between applying and reverting the toggles, which cost differently (a
        // DontFragment set is dearer than its remove). One discarded raw burst puts the raw bursts
        // on the same parity sequence as the World's warm bursts 1..kWarmBursts, so both medians
        // come from the same mix.
        (void)zone.rawFlecsBurst(kWarmBursts + 1, false);
        for (u32 round = kWarmBursts + 2; round <= 2 * kWarmBursts + 1; ++round) {
            raw.push_back(zone.rawFlecsBurst(round, round >= kWarmBursts + 3));
        }
        r.burstRaw = raw.front();
        for (f64 bench::BurstResult::*field : {&bench::BurstResult::createMs, &bench::BurstResult::destroyMs,
                                                &bench::BurstResult::toggleMs, &bench::BurstResult::toggleDontFragmentMs}) {
            std::vector<f64> v;
            for (const bench::BurstResult& b : raw) v.push_back(b.*field);
            r.burstRaw.*field = percentile(v, 0.5);
        }
        std::vector<f64> rawTotals;
        for (const bench::BurstResult& b : raw) rawTotals.push_back(b.totalMs());
        r.structuralRawMs = percentile(rawTotals, 0.5);
        std::vector<f64> rawDfTotals;
        for (const bench::BurstResult& b : raw) rawDfTotals.push_back(b.totalDontFragmentMs());
        r.structuralRawDfMs = percentile(rawDfTotals, 0.5);

        usize elapsed = 0, user = 0, sys = 0, rss = 0, peakRss = 0, commit = 0, peakCommit = 0, faults = 0;
        mi_process_info(&elapsed, &user, &sys, &rss, &peakRss, &commit, &peakCommit, &faults);
        r.rssMb = static_cast<f64>(rss) / (1024.0 * 1024.0);
        r.rssPeakMb = static_cast<f64>(peakRss) / (1024.0 * 1024.0);
    }
    return r;
}

std::vector<u32> parseWorkers(std::string_view s) {
    std::vector<u32> v;
    while (!s.empty()) {
        const usize comma = s.find(',');
        const std::string_view item = s.substr(0, comma);
        u32 n = 0;
        if (std::from_chars(item.data(), item.data() + item.size(), n).ec == std::errc()) v.push_back(n);
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return v;
}

const char* verdict(bool ok) { return ok ? "PASS" : "FAIL"; }

} // namespace

int main(int argc, char** argv) {
    log::setLevel(log::Level::Warn);
    const CommandLine cmd = CommandLine::parse(argc, argv);
    const bool quick = cmd.has("quick");
    const u32 ticks = static_cast<u32>(cmd.getInt("ticks", quick ? 60 : 400));
    const u32 warmup = static_cast<u32>(cmd.getInt("warmup", quick ? 10 : 40));
    const u64 seed = static_cast<u64>(cmd.getInt("seed", 0x5EED));
    std::vector<u32> workerList = parseWorkers(cmd.getString("workers", "0,1,2,4"));
    if (workerList.empty()) workerList = {0, 1, 2, 4};
    const bool spikesOnly = cmd.has("spikes-only");
    const bool spikes = !cmd.has("no-spikes");
    const bool inFrameDontFragment = cmd.has("inframe-dontfragment");

    ecs::installFlecsOsApi();
    out(std::format("ecs_bench — RT-01 50k-entity zone on flecs {} ({} hardware threads; InFrame {})\n", "4.1.6",
                    hardwareThreadCount(), inFrameDontFragment ? "DontFragment" : "fragmenting"));

    bool pass = true;
    bool m1Pass = true;
    if (!spikesOnly) {
        // Warm-up pass (discarded): the first world in a process pays one-time costs — the allocator
        // committing and first-touching fresh OS pages as tables and sparse sets grow — that a
        // long-running cell has already paid. Without it the first configuration is not comparable.
        if (!cmd.has("no-warmup-pass")) {
            out("\n(warm-up pass: one discarded zone run)\n");
            (void)runZone(0, std::min(ticks, 20u), 5, seed, inFrameDontFragment, false);
        }
        std::vector<RunResult> results;
        for (u32 w : workerList) {
            out(std::format("\n-- configuration: {} --\n", w == 0 ? std::string("single-threaded (no JobSystem)")
                                                             : std::format("{} job worker(s) + main thread", w)));
            results.push_back(runZone(w, ticks, warmup, seed, inFrameDontFragment, false));
            const RunResult& r = results.back();
            out(std::format("  zone: {} entities ({} replicated, {} placed), {} archetypes, {} tables, {} bodies, "
                            "built in {:.0f} ms\n",
                            r.entities, r.replicated, r.entities - r.replicated, r.archetypes, r.tables, r.bodies,
                            r.buildMs));
            out(std::format("  tick ms: p50 {:.2f}  p95 {:.2f}  p99 {:.2f}  max {:.2f}  mean {:.2f}   ({} ticks)\n",
                            r.p50, r.p95, r.p99, r.maxMs, r.meanMs, ticks));
            out(std::format("  per tick: {} component changes on {} entities, {} structural ops, {} live projectiles\n",
                            r.changesPerTick, r.changedEntitiesPerTick, r.structuralOpsPerTick, r.liveProjectiles));
            out("  systems (avg us / rows / jobs):");
            for (const ecs::SystemStats& s : r.systems) {
                out(std::format(" {} {:.0f}/{}/{};", s.name, s.avgNs * 1e-3, s.lastRows, s.lastJobs));
            }
            out("\n  sync points (avg us):");
            for (u32 s = 0; s < ecs::kStageCount; ++s) {
                if (r.syncMs[s] > 0.0005) out(std::format(" {} {:.0f};", ecs::stageName(static_cast<ecs::Stage>(s)), r.syncMs[s] * 1000));
            }
            out(std::format("\n  50k x 3 iteration (1 thread): {:.3f} ms ({} chunks; chunk list {:.3f} ms)\n", r.iterateMs,
                            r.iterateChunks, r.iterateCollectMs));
            out(std::format("  9k structural ops ({} commands; recorded in {:.2f} ms), ms:  create 3k  destroy 3k  "
                            "toggle 3k  = total   | toggle 3k DontFragment\n",
                            r.burstWarm.commands, r.burstWarm.recordMs));
            for (const auto& [label, b, total] :
                 {std::tuple{"warm median", &r.burstWarm, r.structuralMs},
                  std::tuple{"warm max   ", &r.burstWarmMax, r.structuralMaxMs},
                  std::tuple{"cold       ", &r.burstCold, r.burstCold.totalMs()},
                  std::tuple{"raw flecs  ", &r.burstRaw, r.structuralRawMs}}) {
                out(std::format("    {}: {:>10.3f} {:>10.3f} {:>10.3f}  = {:>7.3f}   | {:>8.3f}\n", label, b->createMs,
                                b->destroyMs, b->toggleMs, total, b->toggleDontFragmentMs));
            }
            out("    warm bursts total (toggle) ms:");
            for (usize i = 0; i < r.warmTotals.size(); ++i) {
                out(std::format(" {:.2f} ({:.2f})", r.warmTotals[i], r.warmToggles[i]));
            }
            out(std::format("; tables after bursts {}\n", r.tablesAfterBursts));
            out(std::format("    M1 (World / raw flecs, warm medians): tag toggles {:.3f} / {:.3f} = {:.2f}x; "
                            "DontFragment toggles {:.3f} / {:.3f} = {:.2f}x\n",
                            r.structuralMs, r.structuralRawMs, r.m1Tag(), r.structuralDfMs, r.structuralRawDfMs,
                            r.m1DontFragment()));
            out(std::format("  memory: ECS tag {:.1f} MB (peak {:.1f} MB); process RSS {:.1f} MB (peak {:.1f} MB)\n",
                            r.ecsMb, r.ecsPeakMb, r.rssMb, r.rssPeakMb));
            out(std::format("  state hash: {:016x}\n", r.hash));
        }

        // ---- RT-01 verdict ------------------------------------------------------------------
        const RunResult* server = &results.back();
        for (const RunResult& r : results) {
            if (r.workers == 4) server = &r;
        }
        f64 iterate = 1e30, structural = 0, structuralMax = 0, structuralRaw = 0, ecsMb = 0, m1Tag = 0, m1Df = 0;
        u32 tables = 0;
        bool deterministic = true;
        for (const RunResult& r : results) {
            iterate = std::min(iterate, r.iterateMs);
            structural = std::max(structural, r.structuralMs);
            structuralMax = std::max(structuralMax, r.structuralMaxMs);
            structuralRaw = std::max(structuralRaw, r.structuralRawMs);
            m1Tag = std::max(m1Tag, r.m1Tag());
            m1Df = std::max(m1Df, r.m1DontFragment());
            ecsMb = std::max(ecsMb, r.ecsPeakMb);
            tables = std::max(tables, r.tables);
            deterministic = deterministic && r.hash == results.front().hash;
        }
        out("\n== Summary ==\n");
        out("  workers   p50 ms   p95 ms   p99 ms   iterate ms   9k-ops ms   ECS MB   tables\n");
        for (const RunResult& r : results) {
            out(std::format("  {:>7}   {:>6.2f}   {:>6.2f}   {:>6.2f}   {:>10.3f}   {:>9.3f}   {:>6.1f}   {:>6}\n", r.workers,
                            r.p50, r.p95, r.p99, r.iterateMs, r.structuralMs, r.ecsPeakMb, r.tables));
        }
        const bool tickOk = server->p99 <= kTickP99BudgetMs;
        const bool structOk = structural <= kStructuralBudgetMs;
        const bool iterOk = iterate <= kIterateBudgetMs;
        const bool memOk = ecsMb <= kEcsMemoryBudgetMb;
        const bool tableOk = tables <= kTableBudget;
        const bool archOk = server->archetypes >= 120;
        out("\n== RT-01 (02 §8.2) ==\n");
        out(std::format("  engine stages p99 ({} workers)   {:>8.2f} ms  <= {:.0f} ms     {}\n", server->workers,
                        server->p99, kTickP99BudgetMs, verdict(tickOk)));
        out(std::format("  9k structural ops (warm median) {:>8.3f} ms  <= {:.1f} ms    {}   (worst config; max burst {:.3f} ms)\n",
                        structural, kStructuralBudgetMs, verdict(structOk), structuralMax));
        out(std::format("    same ops on the raw flecs C API {:>7.3f} ms  (floor of any flecs wrapper; median, worst config)\n",
                        structuralRaw));
        out(std::format("  50k x 3 iteration, one thread   {:>8.3f} ms  <= {:.1f} ms    {}\n", iterate,
                        kIterateBudgetMs, verdict(iterOk)));
        out(std::format("  ECS memory (peak)               {:>8.1f} MB  <= {:.0f} MB    {}\n", ecsMb,
                        kEcsMemoryBudgetMb, verdict(memOk)));
        out(std::format("  tables (max)                    {:>8}     <= {}      {}\n", tables, kTableBudget,
                        verdict(tableOk)));
        out(std::format("  ~150 ECS archetypes             {:>8}                  {}\n", server->archetypes,
                        verdict(archOk)));
        out(std::format("  deterministic across configs    {:>8}                  {}\n", deterministic ? "yes" : "no",
                        verdict(deterministic)));
        const f64 m1 = std::max(m1Tag, m1Df);
        const bool m1Ok = m1 <= kM1Budget;
        out(std::format("  ADR-004a M1 (World / raw flecs)  {:>8.2f} x   <= {:.1f} x     {}   (worst config; tag {:.2f}x, "
                        "DontFragment {:.2f}x; {})\n",
                        m1, kM1Budget, verdict(m1Ok), m1Tag, m1Df,
                        HELIOS_ENABLE_ASSERTS ? "asserts ON: not a valid M1 build" : "asserts off"));
        pass = tickOk && structOk && iterOk && memOk && tableOk && archOk && deterministic;
        if (cmd.has("m1-gate")) m1Pass = m1Ok;
        out(std::format("  RT-01: {} — {}\n", verdict(pass),
                        pass ? "flecs stays the runtime ECS (ADR-004)" : "opens the custom-ECS ADR (ADR-004)"));
        out("  note: stages here are the ECS-side stand-ins (no Jolt step, no replication encode); the\n"
            "        full-engine p99 is re-measured when physics and networking land.\n");
    }

    if (spikes || spikesOnly) {
        bench::runHeapSpike(quick);
        bench::runFragmentSpike(quick);
    }
    if (cmd.has("m1-gate")) return m1Pass ? 0 : 1;
    return pass ? 0 : 1;
}
