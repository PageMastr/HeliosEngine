// IFence semantics on the in-memory fence (04 §6.1).
#include <doctest/doctest.h>

#include <thread>

#include "helios/authority/memory_fence.h"

using namespace helios;
using namespace helios::authority;

namespace {
const AgId kShip{100};
const AgId kPilot{200};
const AgId kFighter{300};
const AgId kCargo{400};

Owner cell(u64 id, u64 region = 1001, LeaseGen gen = 1) { return Owner{CellId{id}, RegionId{region}, gen}; }

FenceResult now(const Future<FenceResult>& f) {
    REQUIRE(f.isReady());
    return f.get();
}
BulkFenceResult now(const Future<BulkFenceResult>& f) {
    REQUIRE(f.isReady());
    return f.get();
}
} // namespace

TEST_CASE("authority.fence: load a dormant row, then handoff and park") {
    InMemoryFence fence;
    REQUIRE(fence.createRow(kShip, 1).ok());
    CHECK_FALSE(fence.createRow(kShip, 1).ok());
    FenceResult r = now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load));
    CHECK(r.ok());
    CHECK(r.epoch == 2);
    CHECK_FALSE(r.previousOwner.isValid());
    auto row = fence.row(kShip);
    REQUIRE(row);
    CHECK(row->state == RowState::Active);
    CHECK(row->owner == cell(7));

    // Handoff to cell 8 at epoch 2 -> 3; a second handoff with the old epoch is stale.
    r = now(fence.advance(kShip, 2, 3, cell(8), AdvanceCause::Handoff));
    CHECK(r.ok());
    CHECK(r.previousOwner == cell(7));
    r = now(fence.advance(kShip, 2, 3, cell(9), AdvanceCause::Handoff));
    CHECK(r.status == FenceStatus::Stale);
    CHECK(r.epoch == 3);
    CHECK(fence.row(kShip)->owner == cell(8));

    // Park needs the owner cell; a zombie (cell 7) cannot park it.
    CHECK(now(fence.park(kShip, 3, cell(7), 55)).status == FenceStatus::Stale);
    r = now(fence.park(kShip, 3, cell(8), 55));
    CHECK(r.ok());
    row = fence.row(kShip);
    CHECK(row->state == RowState::Dormant);
    CHECK_FALSE(row->owner.isValid());
    CHECK(row->epoch == 4);
    CHECK(row->parkedSeq == 55);
    // A dormant row cannot be handed off, only loaded.
    CHECK(now(fence.advance(kShip, 4, 5, cell(9), AdvanceCause::Handoff)).status == FenceStatus::Stale);
    CHECK(now(fence.advance(kShip, 4, 5, cell(9), AdvanceCause::Load)).ok());

    const auto events = fence.events();
    REQUIRE(events.size() == 4);
    CHECK(events[0].cause == AdvanceCause::Load);
    CHECK(events[1].cause == AdvanceCause::Handoff);
    CHECK(events[2].cause == AdvanceCause::Park);
    CHECK(events[3].cause == AdvanceCause::Load);
    CHECK(fence.releasedNotices().empty()); // nobody was superseded
}

TEST_CASE("authority.fence: an active row loads only as a takeover, which releases the old owner") {
    InMemoryFence fence;
    REQUIRE(fence.createRow(kShip).ok());
    std::vector<ReleasedNotice> seen;
    fence.setReleasedHandler([&](const ReleasedNotice& n) { seen.push_back(n); });
    REQUIRE(now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load)).ok());
    CHECK(now(fence.advance(kShip, 2, 3, cell(8), AdvanceCause::Load)).status == FenceStatus::NeedsTakeover);
    const FenceResult r = now(fence.advance(kShip, 2, 3, cell(8), AdvanceCause::Takeover));
    CHECK(r.ok());
    CHECK(r.previousOwner == cell(7));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].cell == CellId{7});
    CHECK(seen[0].ag == kShip);
    CHECK(seen[0].epoch == 3);
}

TEST_CASE("authority.fence: argument checks") {
    InMemoryFence fence;
    REQUIRE(fence.createRow(kShip).ok());
    CHECK(now(fence.advance(kShip, 1, 3, cell(7), AdvanceCause::Load)).status == FenceStatus::InvalidArgument);
    CHECK(now(fence.advance(kShip, 1, 2, Owner{}, AdvanceCause::Load)).status == FenceStatus::InvalidArgument);
    CHECK(now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Park)).status == FenceStatus::InvalidArgument);
    CHECK(now(fence.advance(AgId{999}, 1, 2, cell(7), AdvanceCause::Load)).status == FenceStatus::NotFound);
}

TEST_CASE("authority.fence: join and leave keep a tree in lockstep") {
    InMemoryFence fence;
    REQUIRE(fence.createRow(kShip).ok());
    REQUIRE(fence.createRow(kPilot, 5).ok());
    REQUIRE(now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load)).ok());
    // Join needs both trees owned by the same cell.
    CHECK(now(fence.join(kPilot, 5, kShip, 2, cell(7))).status == FenceStatus::Stale); // pilot dormant
    REQUIRE(now(fence.advance(kPilot, 5, 6, cell(7), AdvanceCause::Load)).ok());
    CHECK(now(fence.join(kPilot, 6, kShip, 1, cell(7))).status == FenceStatus::Stale); // wrong ship epoch
    const FenceResult j = now(fence.join(kPilot, 6, kShip, 2, cell(7)));
    REQUIRE(j.ok());
    CHECK(j.epoch == 7); // max(6, 2) + 1
    CHECK(fence.row(kPilot)->root == kShip);
    CHECK(fence.tree(kShip).size() == 2);
    // The member moves with its root; it cannot be advanced on its own.
    CHECK(now(fence.advance(kPilot, 7, 8, cell(8), AdvanceCause::Handoff)).status == FenceStatus::NotRoot);
    REQUIRE(now(fence.advance(kShip, 7, 8, cell(8), AdvanceCause::Handoff)).ok());
    CHECK(fence.row(kPilot)->epoch == 8);
    CHECK(fence.row(kPilot)->owner == cell(8));
    // The old owner's stale leave fails; the new owner's succeeds and splits the tree.
    CHECK(now(fence.leave(kPilot, 8, cell(7))).status == FenceStatus::Stale);
    const FenceResult l = now(fence.leave(kPilot, 8, cell(8)));
    REQUIRE(l.ok());
    CHECK(l.epoch == 9);
    CHECK(fence.row(kPilot)->root == kPilot);
    CHECK(fence.row(kShip)->epoch == 9);
    CHECK(fence.row(kPilot)->epoch == 9);
    CHECK(fence.tree(kShip).size() == 1);
}

TEST_CASE("authority.fence: trees nest at most three deep and leave carries the subtree") {
    InMemoryFence fence;
    const AgId carrier{10};
    for (AgId a : {carrier, kFighter, kPilot, kCargo}) {
        REQUIRE(fence.createRow(a).ok());
        REQUIRE(now(fence.advance(a, 1, 2, cell(7), AdvanceCause::Load)).ok());
    }
    REQUIRE(now(fence.join(kPilot, 2, kFighter, 2, cell(7))).ok());   // fighter -> pilot
    REQUIRE(now(fence.join(kFighter, 3, carrier, 2, cell(7))).ok());  // carrier -> fighter -> pilot
    CHECK(fence.row(kPilot)->root == carrier);
    CHECK(fence.tree(carrier).size() == 3);
    // A three-deep tree cannot go under another root.
    CHECK(now(fence.join(carrier, 4, kCargo, 2, cell(7))).status == FenceStatus::InvalidArgument);
    // The fighter launches with its pilot aboard.
    REQUIRE(now(fence.leave(kFighter, 4, cell(7))).ok());
    CHECK(fence.row(kFighter)->root == kFighter);
    CHECK(fence.row(kPilot)->root == kFighter);
    CHECK(fence.tree(carrier).size() == 1);
    CHECK(fence.tree(kFighter).size() == 2);
}

TEST_CASE("authority.fence: advanceMany is all-or-nothing") {
    InMemoryFence fence;
    for (AgId a : {kShip, kCargo}) {
        REQUIRE(fence.createRow(a).ok());
        REQUIRE(now(fence.advance(a, 1, 2, cell(7), AdvanceCause::Load)).ok());
    }
    const std::vector<AgEpoch> bad{{kShip, 2}, {kCargo, 1}};
    CHECK(now(fence.advanceMany(bad, cell(8))).status == FenceStatus::Stale);
    CHECK(fence.row(kShip)->epoch == 2);
    CHECK(fence.row(kShip)->owner == cell(7));
    const std::vector<AgEpoch> good{{kShip, 2}, {kCargo, 2}};
    const BulkFenceResult r = now(fence.advanceMany(good, cell(8)));
    REQUIRE(r.ok());
    CHECK(r.advanced.size() == 2);
    CHECK(fence.row(kCargo)->owner == cell(8));
    const std::vector<AgEpoch> dup{{kShip, 3}, {kShip, 3}};
    CHECK(now(fence.advanceMany(dup, cell(9))).status == FenceStatus::InvalidArgument);
}

TEST_CASE("authority.fence: recovery advances only the dead region's active rows") {
    InMemoryFence fence;
    std::vector<ReleasedNotice> seen;
    fence.setReleasedHandler([&](const ReleasedNotice& n) { seen.push_back(n); });
    const AgId parked{500};
    for (AgId a : {kShip, kCargo, parked, kPilot}) REQUIRE(fence.createRow(a).ok());
    REQUIRE(now(fence.advance(kShip, 1, 2, cell(7, 1001, 3), AdvanceCause::Load)).ok());
    REQUIRE(now(fence.advance(kCargo, 1, 2, cell(7, 1001, 3), AdvanceCause::Load)).ok());
    REQUIRE(now(fence.advance(kPilot, 1, 2, cell(9, 1002, 1), AdvanceCause::Load)).ok()); // other region
    REQUIRE(now(fence.advance(parked, 1, 2, cell(7, 1001, 3), AdvanceCause::Load)).ok());
    REQUIRE(now(fence.park(parked, 2, cell(7, 1001, 3), 10)).ok());

    const BulkFenceResult r = now(fence.advanceOwnedBy(RegionId{1001}, 3, cell(8, 1001, 4)));
    REQUIRE(r.ok());
    REQUIRE(r.advanced.size() == 2);
    CHECK(r.advanced[0] == AgEpoch{kShip, 3});
    CHECK(r.advanced[1] == AgEpoch{kCargo, 3});
    CHECK(fence.row(parked)->state == RowState::Dormant); // dormant rows are never resurrected
    CHECK(fence.row(kPilot)->owner == cell(9, 1002, 1));
    CHECK(seen.size() == 2);
    for (const auto& n : seen) CHECK(n.cell == CellId{7});
    // The zombie's writes now fail the CAS.
    CHECK(now(fence.park(kShip, 2, cell(7, 1001, 3), 11)).status == FenceStatus::Stale);
}

TEST_CASE("authority.fence: migrateRegion needs the exact manifest") {
    InMemoryFence fence;
    for (AgId a : {kShip, kCargo}) {
        REQUIRE(fence.createRow(a).ok());
        REQUIRE(now(fence.advance(a, 1, 2, cell(7, 1001, 3), AdvanceCause::Load)).ok());
    }
    AgManifest partial{{{kShip, 2}}};
    CHECK(now(fence.migrateRegion(RegionId{1001}, 4, cell(8, 1001, 4), partial)).status == FenceStatus::Stale);
    AgManifest staleEpoch{{{kShip, 2}, {kCargo, 1}}};
    CHECK(now(fence.migrateRegion(RegionId{1001}, 4, cell(8, 1001, 4), staleEpoch)).status == FenceStatus::Stale);
    CHECK(now(fence.migrateRegion(RegionId{1001}, 4, cell(8, 1001, 5), AgManifest{{{kShip, 2}, {kCargo, 2}}})).status ==
          FenceStatus::InvalidArgument); // owner's lease_gen must be `next`
    const BulkFenceResult r = now(fence.migrateRegion(RegionId{1001}, 4, cell(8, 1001, 4), AgManifest{{{kShip, 2}, {kCargo, 2}}}));
    REQUIRE(r.ok());
    CHECK(fence.row(kShip)->owner == cell(8, 1001, 4));
    CHECK(fence.releasedNotices().empty()); // cooperative: no released
}

TEST_CASE("authority.fence: deferred calls apply in call order when pumped") {
    InMemoryFence fence(InMemoryFence::Config{.deferred = true});
    REQUIRE(fence.createRow(kShip).ok());
    auto a = fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load);
    auto b = fence.advance(kShip, 1, 2, cell(8), AdvanceCause::Load); // loses the race
    CHECK_FALSE(a.isReady());
    CHECK_FALSE(b.isReady());
    CHECK(fence.pendingOps() == 2);
    CHECK(fence.pump(1) == 1);
    REQUIRE(a.isReady());
    CHECK_FALSE(b.isReady());
    CHECK(fence.pump() == 1);
    CHECK(a.get().ok());
    CHECK(b.get().status == FenceStatus::Stale);
    CHECK(fence.row(kShip)->owner == cell(7));
}

TEST_CASE("authority.fence: an unavailable fence changes nothing") {
    InMemoryFence fence;
    REQUIRE(fence.createRow(kShip).ok());
    fence.setUnavailable(true);
    CHECK(now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load)).status == FenceStatus::Unavailable);
    CHECK(fence.row(kShip)->state == RowState::Dormant);
    fence.setUnavailable(false);
    CHECK(now(fence.advance(kShip, 1, 2, cell(7), AdvanceCause::Load)).ok());
}

TEST_CASE("authority.future: continuations run once, before or after set, on the setting thread") {
    Promise<int> p;
    Future<int> f = p.future();
    CHECK(f.isValid());
    CHECK_FALSE(f.isReady());
    int before = 0;
    std::thread::id where;
    f.then([&](const int& v) {
        before = v;
        where = std::this_thread::get_id();
    });
    std::thread t([&] { p.set(42); });
    const std::thread::id setter = t.get_id();
    t.join();
    CHECK(before == 42);
    CHECK(where == setter);
    int after = 0;
    f.then([&](const int& v) { after = v; }); // already ready: runs here
    CHECK(after == 42);
    CHECK(f.get() == 42);
    CHECK(makeReadyFuture(7).get() == 7);
    CHECK_FALSE(Future<int>{}.isValid());
}
