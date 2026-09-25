# engine/authority — single-writer authority and the zone clock (WP-0.14)

`helios::authority` (target `helios_authority`, HEADLESS, L4, deps `helios::core`) holds the
authority seams of 04 §6.1 and the dilatable zone clock of 04 §3.2 / §11.2. v0 runs one cell per
zone, but every concept exists from Phase 0 (04 design rule 3): authority groups, epochs, the
fence, region lease generations and the holder rule.

| Header (`helios/authority/…`) | What it provides |
|---|---|
| `types.h` | `AgId`, `CellId`, `RegionId` (distinct 64-bit ids), `Epoch`, `LeaseGen`, `StreamSeq`, `Owner{cell, region, leaseGen}`, `AgEpoch`, `AdvanceCause`, `FenceStatus` (`FENCE_STALE`, `FENCE_TREE_MISMATCH`, …), `FenceResult`, `BulkFenceResult` |
| `future.h` | `Future<T>` / `Promise<T>`: completion of an asynchronous control-plane call. No blocking wait exists: a tick polls `isReady()` or registers `then()` to post into its inbox |
| `fence.h` | `IFence`: `advance` (handoff / load / takeover / abort), `park`, `join`, `leave`, `advanceMany`, `advanceOwnedBy` (recovery of *active* rows only), `migrateRegion` — whole-tree CAS operations |
| `memory_fence.h` | `InMemoryFence`: the full 04 §6.1 row semantics in process (trees ≤ 3 deep, members in lockstep with their root, dormant rows, the `fence.advanced` outbox, `released{ag, epoch}` notices to superseded cells), immediate or deferred (`pump()`, models the NATS round trip), fault injection (`setUnavailable`) |
| `ag_table.h` | `AgTable`: the trees one zone instance owns. `load`/`park` go through the fence; `poll()` applies replies at a tick boundary; the **fence gate** (`ledgerGateOpen`) is closed while a fence op on the tree is in flight; `onReleased` drops a superseded tree (a tree still loading from e waits for its own CAS for notices up to e + 1). Each tracking of a tree has an incarnation, so a reply to a call made for a dropped tracking never applies to a later one |
| `lease.h` | `LeaseHolder`: region lease generations and the **holder rule** (05 §1.4.2). Unreachability never fences; `lease_lost`, a missing or re-generated assignment, a higher generation seen elsewhere or a rejected region checkpoint do. A region back at a new generation is `Lost` then `Acquired` (reload, never resume). Per-registration generation floors keep a reply that was in flight from re-acquiring a generation already given up or seen superseded |
| `zone_clock.h` | `ZoneClock` (fixed step on core's `DilatableClock`, 1–60 Hz, dilation `d ∈ [0.1, 1]`, bounded catch-up, `ZoneSchedule` changes at `anchor_tick = now + 2`, `MigrationHold`, `rejoin`) and `TiDiController` (fast attack `L > 0.9 → d·0.85/L`, slow release `+0.05/s` after 2 s of `L < 0.7`, 1 % quantum) |

## TiDi control law

Load `L` is the tick's CPU time over the wall interval it was given, `tick_cpu / (tick_dt / d)`. At
`d = 1` that is 04 §3.2's `tick_cpu_ms / (1000 / tick_hz)`. Measuring against the *dilated*
interval makes the attack converge on the sustainable dilation `0.85 / L₁` instead of ratcheting
down while already dilated (tested: a constant 1.5× overload settles at `d ≈ 0.56` with `L` inside
the 0.7–0.9 hold band and stays there). Holding reports the already-scheduled target, so a pending
attack is never undone while it waits for its anchor tick. `attackTicks` (default 1, the spec) can
require consecutive overloaded ticks. The controller's output is quantised to 1 % so a release
announces a new `d` at most every few ticks. Dilation stretches wall time only: the simulation step
never changes, so ticks compute the same results dilated or not (04 design rule 8).

In v0 each zone is its own leader (`Config::selfLed`): its demand becomes its schedule. A follower
zone (Phase 3) reports `demandPpm()` as `TiDiDemand` and applies the leader's `ZoneSchedule`.

## Threading

`InMemoryFence`, `Future`/`Promise` and `LeaseHolder` document their rules in their headers:
the fence is thread-safe, `AgTable` / `ZoneClock` / `LeaseHolder` belong to one thread (the
zone's tick thread, or the process's control-plane thread for the lease holder).

## Tests

`authority_tests` (doctest, 35 cases): zone clock cadence, validation, catch-up without skipped tick
numbers, dilation stretching wall time only, fast attack two ticks ahead, the 10 % floor,
convergence without ratchet, slow release timing, the hold band, `attackTicks`, determinism, holds,
rejoin, leader schedules; fence load / handoff / park, takeover + `released`, argument checks,
join / leave lockstep, depth-3 trees with subtree leave, all-or-nothing `advanceMany`, recovery of
active rows only (dormant never resurrected, zombies refused), exact-manifest migration, deferred
ordering, unavailability; futures across threads; lease holder events, the holder rule under
unreachability, explicit fencing signals; AG table loads at tick boundaries, the fence gate,
rejected loads, released notices and takeovers.

## Not yet

A NATS-backed `IFence` (the persistence gateway's fence arrives in Phase 1, 05 §1.13); effects,
ghosts, handoff and `Authority::mutate` (v1); multi-cell zone leaders sending `ZoneSchedule`s
(Phase 3).

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
