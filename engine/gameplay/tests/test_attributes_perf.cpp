// GP-1 performance budgets (06 §12.2): a 300-modifier ship recompute <= 50 us, one incremental change
// <= 5 us, 10k entities x 40 attributes at 5 % dirty <= 1 ms on 8 workers. Timings are reported
// always and asserted only in optimized builds without sanitizers (like tools/schemac's perf test);
// the 8-worker budget only on machines with 8 hardware threads (see that test).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "helios/core/jobs.h"
#include "helios/core/thread.h"
#include "helios/gameplay/attributes.h"

using namespace helios;
using namespace helios::gameplay;

#if defined(NDEBUG) && !defined(HELIOS_SANITIZERS_ENABLED) && !defined(__SANITIZE_ADDRESS__)
#define HELIOS_GAMEPLAY_ASSERT_BUDGETS 1
#else
#define HELIOS_GAMEPLAY_ASSERT_BUDGETS 0
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::shared_ptr<const AttributeLayout> shipLayout() {
    std::vector<AttributeLayout::Input> in;
    for (int i = 0; i < 40; ++i) {
        AttributeLayout::Input a;
        a.id = "Ship.A" + std::to_string(i);
        a.defaultValue = 100.0 + i;
        a.stackingPenalised = (i % 2) == 0;
        if (i % 8 == 7) a.derived = "attr(self, Ship.A" + std::to_string(i - 1) + ") * 1.5";
        in.push_back(a);
    }
    auto l = AttributeLayout::build(in);
    REQUIRE(l.ok());
    return *l;
}

void addShipModifiers(AttributeSet& s, u32 count, u32 seed) {
    std::mt19937 rng(seed);
    const ModOp ops[] = {ModOp::PostMul, ModOp::PostPercent, ModOp::ModAdd, ModOp::PreMul, ModOp::PostMul};
    for (u32 k = 0; k < count; ++k) {
        const auto slot = static_cast<AttrSlot>(rng() % 40); // one draw per statement (evaluation order)
        const ModOp op = ops[rng() % 5];
        const f64 value = 1.0 + (rng() % 100) / 400.0;
        Modifier m = Modifier::constant(slot, op, value);
        m.penaltyGroup = static_cast<u16>(rng() % 2);
        REQUIRE(s.addModifier(m).ok());
    }
}

} // namespace

TEST_CASE("perf: 300-modifier ship: full recompute <= 50 us, incremental change <= 5 us") {
    auto layout = shipLayout();
    AttributeSet s(layout);
    addShipModifiers(s, 300, 9);
    AttributeScratch scratch;
    s.recompute(nullptr, &scratch);
    // Best of 10 batches of 200 (robust against other processes on a shared CI machine).
    constexpr int kBatch = 200;
    f64 fullUs = 1e30, incUs = 1e30;
    for (int batch = 0; batch < 10; ++batch) {
        auto t0 = Clock::now();
        for (int i = 0; i < kBatch; ++i) {
            s.markAllDirty();
            s.recompute(nullptr, &scratch);
        }
        fullUs = std::min(fullUs, std::chrono::duration<f64, std::micro>(Clock::now() - t0).count() / kBatch);
        t0 = Clock::now();
        for (int i = 0; i < kBatch; ++i) {
            // A value never used before, so every iteration is a real change (with `i & 7`, 4 of 5
            // iterations repeated a slot's value and measured a no-op).
            s.setBase(static_cast<AttrSlot>(i % 40), 50.0 + static_cast<f64>(batch * kBatch + i));
            s.recompute(nullptr, &scratch);
        }
        incUs = std::min(incUs, std::chrono::duration<f64, std::micro>(Clock::now() - t0).count() / kBatch);
    }
    MESSAGE("300-modifier ship: full recompute " << fullUs << " us, incremental " << incUs << " us");
#if HELIOS_GAMEPLAY_ASSERT_BUDGETS
    CHECK(fullUs <= 50.0);
    CHECK(incUs <= 5.0);
#endif
}

// The budget is 1 ms on 8 workers: 8 threads resolving (7 job workers plus the calling thread,
// which runs chunks inside parallelFor). It is asserted only where it can be measured, on a machine
// with at least 8 hardware threads; elsewhere the parallel time is reported on min(8, cores) threads
// and only the single-thread proxy is asserted. The proxy (1 thread <= 8 ms) is necessary for the
// budget but not sufficient: it assumes perfect scaling to 8 workers. PR CI skips `perf` tests, and
// the nightly runner has 4 vCPUs, so the budget itself still needs a run on 8-core hardware (09 §8.1).
TEST_CASE("perf: 10k entities x 40 attributes at 5 % dirty <= 1 ms on 8 workers") {
    auto layout = shipLayout();
    constexpr int kEntities = 10000;
    constexpr u32 kBudgetThreads = 8;
    std::vector<std::unique_ptr<AttributeSet>> sets;
    std::vector<AttributeSet*> ptrs;
    sets.reserve(kEntities);
    for (int e = 0; e < kEntities; ++e) {
        sets.push_back(std::make_unique<AttributeSet>(layout));
        addShipModifiers(*sets.back(), 20, static_cast<u32>(e));
        ptrs.push_back(sets.back().get());
    }
    const u32 hw = hardwareThreadCount();
    const u32 threads = std::max(2u, std::min(kBudgetThreads, hw));
    jobs::JobSystemDesc desc;
    desc.workerCount = threads - 1;
    desc.name = "AttrPerf";
    jobs::JobSystem js(desc);
    REQUIRE(js.workerCount() + 1 == threads);
    resolveAttributes(ptrs, {}, &js);
    std::mt19937 rng(3);
    auto dirty5 = [&] {
        for (auto* s : ptrs) {
            for (int k = 0; k < 2; ++k) { // 2/40 = 5 %
                const auto slot = static_cast<AttrSlot>(rng() % 40);
                s->setBase(slot, 10.0 + (rng() % 1000));
            }
        }
    };
    // Best of N rounds: the minimum is robust against other processes on a shared CI machine.
    constexpr int kRounds = 30;
    f64 seqMs = 1e30, parMs = 1e30;
    for (int r = 0; r < kRounds; ++r) {
        dirty5();
        auto t0 = Clock::now();
        resolveAttributes(ptrs, {}, nullptr);
        seqMs = std::min(seqMs, std::chrono::duration<f64, std::milli>(Clock::now() - t0).count());
        dirty5();
        t0 = Clock::now();
        resolveAttributes(ptrs, {}, &js);
        parMs = std::min(parMs, std::chrono::duration<f64, std::milli>(Clock::now() - t0).count());
    }
    MESSAGE("10k x 40 at 5 % dirty: 1 thread " << seqMs << " ms, " << threads << " threads " << parMs << " ms on "
                                               << hw << " hardware threads (budget: 1 ms on " << kBudgetThreads
                                               << " threads)");
#if HELIOS_GAMEPLAY_ASSERT_BUDGETS
    CHECK(seqMs <= 8.0); // proxy: the single-thread work fits 8 workers x 1 ms
    if (hw >= kBudgetThreads) {
        CHECK(parMs <= 1.0); // the budget itself
    } else {
        MESSAGE("the 1 ms budget itself is not measured here: it needs >= " << kBudgetThreads
                                                                            << " hardware threads");
    }
#endif
}
