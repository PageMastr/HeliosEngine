// LeaseHolder (holder rule, 05 §1.4.2) and AgTable (fence gate, released, 04 §6.1).
#include <doctest/doctest.h>

#include "helios/authority/ag_table.h"
#include "helios/authority/lease.h"
#include "helios/authority/memory_fence.h"

using namespace helios;
using namespace helios::authority;

namespace {
RegionAssignment zone(u64 id, LeaseGen gen, std::string name = "z") { return RegionAssignment{RegionId{id}, std::move(name), gen}; }

u32 count(const std::vector<LeaseEvent>& events, LeaseEventKind kind) {
    u32 n = 0;
    for (const auto& e : events) n += e.kind == kind ? 1u : 0u;
    return n;
}
} // namespace

TEST_CASE("authority.lease: assignments acquire, repeat quietly, and lose on change") {
    LeaseHolder h;
    const std::vector<RegionAssignment> first{zone(1001, 3, "tallis"), zone(1002, 1, "harrow")};
    auto ev = h.onAssignments(first);
    CHECK(count(ev, LeaseEventKind::Acquired) == 2);
    CHECK(h.holds(RegionId{1001}, 3));
    // Heartbeats with the same list produce nothing.
    CHECK(h.onAssignments(first).empty());
    // 1002 disappears, 1001 comes back at a new generation: lost first, then acquired.
    const std::vector<RegionAssignment> second{zone(1001, 5, "tallis")};
    ev = h.onAssignments(second);
    REQUIRE(ev.size() == 3);
    CHECK(ev[0].kind == LeaseEventKind::Lost);
    CHECK(ev[1].kind == LeaseEventKind::Lost);
    CHECK(ev[2].kind == LeaseEventKind::Acquired);
    CHECK(ev[2].assignment.leaseGen == 5);
    bool sawGenChange = false;
    bool sawUnassigned = false;
    for (const auto& e : ev) {
        sawGenChange |= e.reason == LeaseLossReason::GenerationChanged && e.assignment.region == RegionId{1001};
        sawUnassigned |= e.reason == LeaseLossReason::Unassigned && e.assignment.region == RegionId{1002};
    }
    CHECK(sawGenChange);
    CHECK(sawUnassigned);
    // A stale reply listing an older generation is ignored.
    CHECK(h.onAssignments(std::vector<RegionAssignment>{zone(1001, 4, "tallis")}).empty());
    CHECK(h.holds(RegionId{1001}, 5));
    CHECK(h.stats().staleAssignmentsIgnored == 1);
}

TEST_CASE("authority.lease: the holder rule never fences on unreachability") {
    LeaseHolder h;
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 3)});
    for (int i = 0; i < 100; ++i) h.onControlPlaneUnreachable();
    CHECK(h.holds(RegionId{1001}, 3));
    CHECK(h.stats().unreachable == 100);
    CHECK(h.stats().lost == 0);
    // A failed fence CAS for one AG only asks for re-verification.
    h.onAgFenceRejected(RegionId{1001});
    CHECK(h.holds(RegionId{1001}));
    CHECK(h.needsVerification(RegionId{1001}));
    CHECK(h.onAssignments(std::vector<RegionAssignment>{zone(1001, 3)}).empty());
    CHECK_FALSE(h.needsVerification(RegionId{1001}));
}

TEST_CASE("authority.lease: explicit signals fence") {
    LeaseHolder h;
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 3), zone(1002, 1)});
    CHECK(h.onHigherGeneration(RegionId{1001}, 3).empty()); // not higher
    CHECK(h.onHigherGeneration(RegionId{1001}, 2).empty());
    auto ev = h.onHigherGeneration(RegionId{1001}, 4);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].reason == LeaseLossReason::HigherGenerationSeen);
    ev = h.onRegionCheckpointRejected(RegionId{1002});
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].reason == LeaseLossReason::CheckpointRejected);
    CHECK(h.held().empty());
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 6), zone(1003, 2)});
    ev = h.onLeaseLost();
    CHECK(ev.size() == 2);
    CHECK(count(ev, LeaseEventKind::Lost) == 2);
    CHECK(h.held().empty());
    CHECK(h.stats().leaseLost == 1);
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 7)});
    CHECK(h.releaseAll().size() == 1);
}

TEST_CASE("authority.agtable: loads flip at a tick boundary and the fence gate holds ledger calls") {
    InMemoryFence fence(InMemoryFence::Config{.deferred = true});
    const AgId ship{100};
    REQUIRE(fence.createRow(ship, 4).ok());
    const Owner me{CellId{7}, RegionId{1001}, 3};
    AgTable table(me);
    REQUIRE(table.load(fence, ship, 4));
    CHECK_FALSE(table.load(fence, ship, 4)); // already tracked
    CHECK(table.state(ship) == AgLocalState::Loading);
    CHECK_FALSE(table.isAuthoritative(ship));
    CHECK_FALSE(table.ledgerGateOpen(ship));
    CHECK(table.poll() == 0); // reply not here: the tick does not wait
    fence.pump();
    CHECK(table.poll() == 1);
    CHECK(table.isAuthoritative(ship));
    CHECK(table.epoch(ship) == Epoch{5});
    CHECK(table.ledgerGateOpen(ship));
    REQUIRE(table.owned().size() == 1);

    REQUIRE(table.park(fence, ship, 99));
    CHECK_FALSE(table.ledgerGateOpen(ship));
    CHECK_FALSE(table.isAuthoritative(ship));
    fence.pump();
    CHECK(table.poll() == 1);
    CHECK(table.size() == 0);
    CHECK(fence.row(ship)->state == RowState::Dormant);
    CHECK(table.stats().loaded == 1);
    CHECK(table.stats().parked == 1);
}

TEST_CASE("authority.agtable: a rejected load or a released notice drops the tree") {
    InMemoryFence fence;
    const AgId ship{100};
    const AgId cargo{400};
    REQUIRE(fence.createRow(ship).ok());
    REQUIRE(fence.createRow(cargo).ok());
    REQUIRE(fence.advance(cargo, 1, 2, Owner{CellId{9}, RegionId{1}, 1}, AdvanceCause::Load).get().ok());
    AgTable table(Owner{CellId{7}, RegionId{1001}, 3});
    REQUIRE(table.load(fence, ship, 1));
    REQUIRE(table.load(fence, cargo, 2)); // active elsewhere and not a takeover
    CHECK(table.poll() == 2);
    CHECK(table.isAuthoritative(ship));
    CHECK_FALSE(table.state(cargo).has_value());
    CHECK(table.stats().rejected == 1);
    // A released notice for an epoch we already hold is stale; a newer one drops the tree.
    CHECK_FALSE(table.onReleased(ship, 2));
    CHECK(table.onReleased(ship, 3));
    CHECK_FALSE(table.isAuthoritative(ship));
    CHECK(table.stats().released == 1);
}

TEST_CASE("authority.agtable: a takeover supersedes the old owner, whose table drops on released") {
    InMemoryFence fence;
    const AgId ship{100};
    REQUIRE(fence.createRow(ship).ok());
    AgTable oldOwner(Owner{CellId{7}, RegionId{1001}, 3});
    AgTable newOwner(Owner{CellId{8}, RegionId{1001}, 4});
    fence.setReleasedHandler([&](const ReleasedNotice& n) {
        if (n.cell == CellId{7}) oldOwner.onReleased(n.ag, n.epoch);
    });
    REQUIRE(oldOwner.load(fence, ship, 1));
    oldOwner.poll();
    REQUIRE(oldOwner.isAuthoritative(ship));
    REQUIRE(newOwner.load(fence, ship, 2, /*takeover=*/true));
    newOwner.poll();
    CHECK(newOwner.isAuthoritative(ship));
    CHECK_FALSE(oldOwner.isAuthoritative(ship));
    CHECK(fence.row(ship)->owner.cell == CellId{8});
}

// Review regressions -------------------------------------------------------------------------

TEST_CASE("authority.lease: a generation already given up is never re-acquired from a stale reply") {
    LeaseHolder h;
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 5, "tallis"), zone(1002, 2, "harrow")});
    // A gateway RouteUpdate (or the DIRECTORY watch) shows generation 6 of 1001 elsewhere.
    REQUIRE(h.onHigherGeneration(RegionId{1001}, 6).size() == 1);
    CHECK(h.generationFloor(RegionId{1001}) == 6);
    // A heartbeat reply that was in flight before the change still lists 1001 at 5: ignored, so
    // the superseded region does not come back to life on this cell.
    auto ev = h.onAssignments(std::vector<RegionAssignment>{zone(1001, 5, "tallis"), zone(1002, 2, "harrow")});
    CHECK(ev.empty());
    CHECK_FALSE(h.holds(RegionId{1001}));
    CHECK(h.stats().staleAssignmentsIgnored == 1);
    // A persistence rejection of 1002's region checkpoint: the same for its old generation.
    REQUIRE(h.onRegionCheckpointRejected(RegionId{1002}).size() == 1);
    CHECK(h.onAssignments(std::vector<RegionAssignment>{zone(1002, 2, "harrow")}).empty());
    CHECK_FALSE(h.holds(RegionId{1002}));
    // Placed back on this cell at a new generation: acquired (reload, never resume).
    ev = h.onAssignments(std::vector<RegionAssignment>{zone(1001, 7, "tallis"), zone(1002, 3, "harrow")});
    CHECK(count(ev, LeaseEventKind::Acquired) == 2);
    CHECK(h.holds(RegionId{1001}, 7));
    CHECK(h.holds(RegionId{1002}, 3));
    // A higher generation seen for a region this process does not hold also sets its floor.
    CHECK(h.onHigherGeneration(RegionId{1003}, 4).empty());
    CHECK(h.onAssignments(std::vector<RegionAssignment>{zone(1001, 7), zone(1002, 3), zone(1003, 3)}).empty());
    CHECK_FALSE(h.holds(RegionId{1003}));
}

TEST_CASE("authority.lease: floors belong to one registration (lease_lost and release clear them)") {
    LeaseHolder h;
    (void)h.onAssignments(std::vector<RegionAssignment>{zone(1001, 5)});
    (void)h.onHigherGeneration(RegionId{1001}, 9);
    CHECK(h.generationFloor(RegionId{1001}) == 9);
    (void)h.onLeaseLost();
    CHECK(h.generationFloor(RegionId{1001}) == 0);
    // A fresh registration (e.g. a backend whose database was reset) starts over at low
    // generations: they must be accepted.
    CHECK(count(h.onAssignments(std::vector<RegionAssignment>{zone(1001, 1)}), LeaseEventKind::Acquired) == 1);
    (void)h.onHigherGeneration(RegionId{1001}, 2);
    (void)h.releaseAll();
    CHECK(h.generationFloor(RegionId{1001}) == 0);
}

TEST_CASE("authority.agtable: a reply to an earlier tracking of the tree never applies to a new one") {
    InMemoryFence fence(InMemoryFence::Config{.deferred = true});
    const AgId ship{100};
    REQUIRE(fence.createRow(ship, 1).ok());
    AgTable table(Owner{CellId{7}, RegionId{1001}, 3});
    REQUIRE(table.load(fence, ship, 1));  // call 1: 1 -> 2
    REQUIRE(table.onReleased(ship, 9));   // superseded while loading: dropped
    REQUIRE(table.load(fence, ship, 1));  // tracked again; call 2: 1 -> 2 (queued behind call 1)
    REQUIRE(fence.pump(1) == 1);          // only call 1 is answered (Ok, epoch 2)
    table.poll();
    // Call 1's reply belongs to the dropped tracking: the new one is still loading, its gate shut.
    CHECK(table.state(ship) == AgLocalState::Loading);
    CHECK_FALSE(table.isAuthoritative(ship));
    CHECK_FALSE(table.ledgerGateOpen(ship));
    CHECK(table.stats().staleReplies == 1);
    CHECK(table.stats().loaded == 0);
    // Call 2 fails (the row moved to 2 under call 1) and decides for the current tracking.
    REQUIRE(fence.pump() == 1);
    table.poll();
    CHECK_FALSE(table.state(ship).has_value());
    CHECK(table.stats().rejected == 1);
}

TEST_CASE("authority.agtable: while loading from e, released notices up to e + 1 wait for our own CAS") {
    InMemoryFence fence(InMemoryFence::Config{.deferred = true});
    const AgId ship{100};
    REQUIRE(fence.createRow(ship, 4).ok());
    AgTable table(Owner{CellId{7}, RegionId{1001}, 3});
    REQUIRE(table.load(fence, ship, 4));  // 4 -> 5
    CHECK_FALSE(table.onReleased(ship, 4)); // about an earlier ownership
    CHECK_FALSE(table.onReleased(ship, 5)); // our CAS decides: if someone else took 5, ours fails
    CHECK(table.state(ship) == AgLocalState::Loading);
    fence.pump();
    table.poll();
    CHECK(table.isAuthoritative(ship));
    CHECK(table.epoch(ship) == Epoch{5});
    // Once active at 5, only a later epoch supersedes us.
    CHECK_FALSE(table.onReleased(ship, 5));
    CHECK(table.onReleased(ship, 6));
}
