# ADR-004a: ECS structural-op cost against RT-01

| | |
|---|---|
| **Status** | **Open.** Opened 2026-09-25, in the round-5 minor revisions. Decision so far: option A, time-boxed, re-evaluated at the Phase 1 RT-01 gate. WP-1.1a measured M1 at 1.54× (GCC) and 1.59× (Clang) on the dev VM, against ≤ 1.6×, met on the median of runs without much margin, and recorded M5 (§7) |
| **Amends** | [ADR-004](../plan/00-decisions.md#adr-004-entity-model--reflection): "A custom ECS is the fallback only if the Phase 1 50k-entity zone benchmark fails". ADR-004 stands unchanged while this ADR is open |
| **Trigger** | Risk K2 ([09 §7](../plan/09-roadmap-and-process.md#7-risk-register)) fired. Its trigger is a pre-bench above 2× the budget, and WP-0.6's acceptance opens this ADR in that case |
| **Owner** | Runtime lead. The work is WP-1.1a ([09 §2.2](../plan/09-roadmap-and-process.md#22-phase-1--first-light)), and WP-1.1 runs the formal RT-01 gate |
| **Evidence** | [`engine/ecs/SPIKES.md`](../../engine/ecs/SPIKES.md) §3 and §4.2 (the `ecs_bench` RT-01 zone), §2 (DontFragment) |

## 1. Context

RT-01 ([02 §8.2](../plan/02-engine-runtime.md#82-acceptance-criteria-automated)) is the plan's ECS gate. It uses a
50k-entity zone on SERVER at 20 Hz: 20k replicated and 30k placed entities, about 150 ECS archetypes, and 5k
bodies in 12 grids. Its budgets:
- engine stages ≤ 12 ms p99;
- **9k structural ops ≤ 1.5 ms**;
- 50k×3 iteration ≤ 0.4 ms on one thread;
- ECS memory ≤ 400 MB;
- ≤ 5,000 tables.

"Failure opens the custom-ECS ADR."

**Pre-bench.** WP-0.6b ran the Phase 0 pre-bench, and WP-0.8 built the ECS on flecs 4.1.6 (90 tests).
The zone has 155 archetypes, with 6–12 components each, and ten systems on the deterministic scheduler. The
burst is 3k creates (projectiles, 7 components), 3k destroys and 3k tag toggles on NPCs, all applied at one
sync point. The machine was not the RT-01 reference box: a shared 4-vCPU cloud VM (Xeon at 2.8 GHz, load
2.4–5.3), GCC 13.3 `RelWithDebInfo` with **asserts enabled**, and mimalloc 3.5.3.

| RT-01 clause | Budget | Measured (re-run after review, SPIKES §4.2) | Earlier runs (§3.2) | Verdict |
|---|---|---|---|---|
| Engine stages p99, 4 workers (ECS side only: no Jolt, no replication encode) | ≤ 12 ms | 3.35 ms | 4.54 / 10.20 ms | Pass |
| **9k structural ops, warm median of 7 bursts, worst worker configuration** | **≤ 1.5 ms** | **3.40–4.19 ms** | **6.07–6.71 ms** (max 11.4) | **Fail** |
| The same 9k ops on the raw flecs C API | — | 0.91–0.93 ms | 0.96–1.71 ms | (the floor) |
| 50k×3 iteration, 1 thread | ≤ 0.4 ms | 0.204 ms | 0.200–0.203 ms | Pass |
| ECS memory peak | ≤ 400 MB | 46.6 MB | 44.8–46.6 MB | Pass |
| Tables | ≤ 5,000 | 2,384 | 2,384 | Pass |
| Determinism across 0/1/2/4 workers | identical | identical hash | identical | Pass |

A Clang 18 quick run showed the same pattern: 3.2–4.2 ms, against 0.81 ms on raw flecs.

The burst took 3.4–6.7 ms against 1.5 ms, which is 2.3–4.5× the budget and above K2's 2× trigger.

**Where the time goes** (SPIKES §3.3, §4.2). Warm medians on one worker, after the review:

| | 3k creates | 3k destroys | 3k tag toggles |
|---|---|---|---|
| World | 1.08 ms | 1.19 ms | 1.14 ms |
| Raw flecs | 0.02 ms | 0.18 ms | 0.71 ms |

Callgrind counts 27.4 M instructions for the World's apply of the 12k benchmark commands, against about
13.4 M for raw flecs. The overhead comes from four places:
- **Creates.** `spawnGroup` costs about 780 instructions per entity (NetHandle, registry inserts,
  `NetIdentity` and column writes). EntityId minting adds about 70. There are eight `componentInfo`
  lookups at about 75 each, and command fusion, flattening and grouping cost about 700.
- **Destroys.** `unregisterSubtree` costs about 640: the registry erase, the handle release, the children
  lookup and the structural log.
- **Toggles.** `ecs_owns_id`, the liveness check, `componentInfo` and `logEvent` cost about 500 per op.

**What this means.**
- **flecs is not the problem.** It does the same work in about 0.9 ms, 60 % of the budget, which leaves
  40 % headroom.
- **The overhead is the Helios World wrapper, at 3–4× the flecs work.** It comes from the identity maps
  (EntityId and NetHandle), the structural log, dirty-tracking setup, command-buffer fusion and grouping,
  and `componentInfo` hash lookups.
- **A custom ECS would not remove that overhead.** 02 §4.1 and §4.4 and 04 §4 need the same identity and
  replication bookkeeping from any ECS.
- **Tag toggles are table moves**, whose cost grows with entity width. A custom archetype ECS pays for them
  too, unless it stores such flags outside the archetype. flecs already offers that as `DontFragment`,
  which was about 1.8× cheaper than a tag in the zone.
- **A typical tick is far below the burst.** It has 53 structural ops and 2 sync points, at 35–70 µs each.
  The 9k burst is RT-01's peak case.

## 2. Decision drivers

1. **No threshold is relaxed** ([09 §0](../plan/09-roadmap-and-process.md#0-principles), principle 4).
   RT-01 belongs to 02, and this ADR does not change it.
2. **The structural contract stays exactly as it is.** That contract covers:
   - the deterministic apply order in (stage, system, job, seq) order (02 §4.3), which 04's bit-exact replay
     relies on;
   - the per-tick structural log that 04 and `presentation` consume (02 §4.4);
   - the EntityId, NetHandle and `NetIdentity` identity (02 §4.1);
   - dirty bits.
3. **The custom-ECS fallback stays possible.** No flecs type appears in a public header (02 §1.1), and flecs
   stays confined to `engine/ecs` (K2).
4. **Engineering cost and schedule.** The ECS is on the critical path, which runs schema → ECS → world model
   → replication ([09 §3.1](../plan/09-roadmap-and-process.md#31-graph)).

## 3. Options

**A. Optimize the Helios World wrapper, keeping flecs.** This means SPIKES §3.4's items, in expected-gain
order:
1. **Batch identity bookkeeping per spawn group.** Reserve EntityIds and NetHandles in blocks, bulk-insert
   them into the registry, and write the `NetIdentity` columns in one pass.
2. **Replace the `componentInfo` hash lookup** with a direct array for component ids below 64k, and cache
   the `ComponentInfo*` in the flattened op list.
3. **Skip needless work.** Skip the children walk on destroy when the entity has no hierarchy record
   (`ecs_id_in_use`), and skip `ecs_owns_id` on remove by comparing the table before and after.
4. **Specialize command buffers.**
   - A `spawnN(prefab or signature, values)` path for homogeneous spawns bypasses per-command fusion.
   - Tag toggles are sorted by (source table, id) and move runs of entities in batches. flecs 4.1 has no
     public batched move for entity lists, so this needs an upstream API or a table-level bulk path inside
     `engine/ecs`.
5. **Decide `InFrame` storage together with item 4.** The DontFragment option halves tick and iteration
   cost, but today it triples create and destroy cost (SPIKES §2.3).

Assessment:
- **Pro:** it attacks the measured cost directly, and it keeps every other RT-01 clause, all of which pass
  with large margins. It needs no migration and keeps the structural contract.
- **Con:** SPIKES estimates that a 3× cut of the overhead reaches about 2–2.5 ms on the loaded VM. The last
  step to 1.5 ms depends on the reference hardware, a release build without asserts, and items 4–5.
- **Cost:** the smallest of the three options, contained in `engine/ecs`.

**B. A custom archetype ECS behind the `helios::ecs` API.**
- **Pro:** full control over table moves, bulk paths and memory layout.
- **Con:** it rebuilds what flecs provides today: storage, queries, observers, prefabs, relationship
  storage, the non-fragmenting hierarchy and sparse (DontFragment-like) storage, all of which the 90 tests
  cover. It can only win back the flecs share of the burst, about 0.9 ms. The wrapper's identity and
  structural-log costs remain, so B still needs A's work.
- **Cost:** the largest, and it moves the critical path.

**C. Hybrid: raw flecs for the hot structural paths.** Selected systems or command kinds (homogeneous
spawns, bulk toggles, despawn sweeps) would call raw flecs bulk APIs (`ecs_bulk_init`, table-level moves)
under a reduced contract, and the rest would keep the World's command buffers.
- **Pro:** it reaches the raw-flecs floor for the hottest ops soonest.
- **Con:** there would be two structural paths, both of which must produce the same structural log,
  identity, dirty bits and deterministic order. It also couples systems to flecs internals, which weakens
  the API boundary that keeps B possible.
- **Note:** A's item 4 already uses flecs bulk paths *inside* the World, under the full contract. C differs
  only in letting callers bypass that contract.

## 4. Decision so far

**Option A, time-boxed to WP-1.1a, re-evaluated at the Phase 1 RT-01 gate.** Raw flecs meets the budget
with 40 % headroom. The overhead is Helios bookkeeping that B would also need, and C's gain is available
within A through item 4, without splitting the structural contract. flecs stays the model (ADR-004).

- **Burst budget: per sync point.** RT-01's clause is read as 9k structural ops applied at **one** sync
  point in ≤ 1.5 ms. The "9k ops per two sync points" reading that SPIKES §3.4 mentions is rejected, because
  it would relax a threshold (driver 1). A change to RT-01 could only come from 02's owner, as a declared
  plan change (09 §5.10.2 D1). This ADR proposes none.
- **Storage in the measured burst.** The formal run uses the storage the engine ships for each kind of
  data (the `ComponentFlags` and `RelationConfig` defaults of SPIKES §2.5). It reports the toggle half both
  as fragmenting tags and as DontFragment components, and **the verdict takes the worse of the two**.
- **Phase 1 midpoint check.** If indicator M1 below is still above 2.5× at the Phase 1 midpoint, the
  Director opens a scoping spike for B. The spike estimates B's cost and measures what a custom table move
  would save against the raw-flecs floor, so the gate decision is not a surprise (09 §3.2 #5). The spike
  decides nothing.
- **At the Phase 1 gate:**
  - If the structural clause passes on SERVER, ADR-004a closes as **accepted (A)**, and K2 retires.
  - If it fails with WP-1.1a merged, **B is decided**: a custom ECS behind the same API, planned as a new
    WP with its own ADR-004 amendment. C is not the fallback. It would need its own amendment of this ADR,
    with evidence that its split contract is safe.
  - If SERVER hardware is not installed by the gate (K7), RT-01 is *unmeasured*, which counts as failing
    (09 §5.6). This ADR then stays open rather than choosing B on missing evidence, and the Phase 1 exit
    waits.

## 5. Consequences

- **K2 fired.** Its mitigation now reads "optimize the wrapper (this ADR), custom ECS only if the formal run
  still fails". The old mitigation, "custom ECS behind the API", would not have helped on its own.
- **WP-1.1a** is a new sub-WP of WP-1.1. It depends only on WP-0.8's ECS part, which is committed, so it
  starts now rather than after all of Phase 0 (09 §8.2).
- **Unchanged:**
  - every other RT-01 clause;
  - the 90 `ecs_tests` and the cross-worker state hash;
  - the structural log, identity, dirty bits and apply order;
  - 04's replay and replication inputs.
  A WP-1.1a change that alters any of them is a defect, not an optimization.
- **The typical tick is unaffected** (53 ops, 35–70 µs per sync point). Only the peak case is at stake.
- **Asserts stay on** in the development configurations. The formal run is a release build with
  `HELIOS_ENABLE_ASSERTS=0`, and the 90 tests still run with asserts on every toolchain.
- **If B is decided,** the critical path through WP-1.1 moves, and the Phase 1 forecast is re-baselined at
  that gate (09 §0, principle 4).
- The flecs bugs SPIKES §2.4 records (optional DontFragment terms iterate incorrectly in 4.1.6) stay worked
  around in `QueryPlan::lookupMask`, and are reported upstream under K10.

## 6. Measurements that will close it

| # | Measurement | Where | Pass |
|---|---|---|---|
| M1 | **Indicator.** World burst divided by the raw-flecs burst for the same 9k ops in the same `ecs_bench` run. Release build, asserts off, warm median of 7 bursts, worst of 0/1/2/4 workers | Dev runner, per WP-1.1a PR, with a callgrind instruction count beside it | ≤ 1.6× (raw flecs uses about 0.9 ms of the 1.5 ms). It was 3.7–4.6× before WP-1.1a (3.98× in the corrected bench). WP-1.1a: 1.54× with GCC, 1.59× with Clang, medians of 9 runs; single runs up to 1.73× and 1.80× (§7) |
| M2 | **The formal RT-01 structural clause.** 9k ops (3k creates, 3k destroys, 3k toggles) applied at one sync point. `HELIOS_ENABLE_ASSERTS=0`, no other load, 0/1/2/4/8 workers, both toggle storages. Warm median of 7 bursts, with the maximum reported | SERVER reference box (the H1/H2 lab, 09 §4.3.1–4.3.2) | ≤ 1.5 ms in the worst configuration and storage |
| M3 | **The other RT-01 clauses, rerun.** Stages p99 including Jolt and the replication encode once they exist (02; SPIKES §3.2 notes), iteration, memory, tables, and determinism across worker counts | SERVER | As RT-01 |
| M4 | **The raw-flecs floor** for the same ops | SERVER | Reported. If it exceeds 1.5 ms, the B spike must show that a custom table move beats it before B is chosen |
| M5 | **The `InFrame` storage choice** (fragmenting or DontFragment pair), measured with item 4 | Dev runner, then SERVER | Recorded in this ADR with its tick, iteration, memory and create/destroy numbers. Dev runner: fragmenting kept (§7) |

When M2 and M3 pass, this ADR closes as accepted (A) and ADR-004 gains a one-line reference to it. When M2
fails at the Phase 1 gate with WP-1.1a merged, it closes with B decided, as §4 describes.

## 7. WP-1.1a results (2026-09-26)

WP-1.1a implemented option A's items 1–4 and decided item 5, in two rounds: the first missed M1
(2.05×), and the second cut the per-command overhead further. The evidence is in
[`engine/ecs/SPIKES.md`](../../engine/ecs/SPIKES.md) §5. The dev runner was the same shared 4-vCPU VM
as §1 (load 0.3–2.2 from other jobs), `Release` with asserts off (the `linux-bench` preset). The
structural log, identities, dirty bits, flecs ids, tables and row order are unchanged, as a digest of
the pre-WP-1.1a World pins in `ecs_tests`, and so is the zone's state hash.

Before measuring, the bench was corrected so that the World and raw-flecs halves do the same work: the
same apply/revert mix, the documented destroy victims, raw destroys that move rows as the World's do,
raw creates that write their values (they wrote none: `bd.ids` was missing), and raw toggles that
choose their tags before timing, as the World does while recording (SPIKES §5.3). Those corrections
alone move M1 about 10 % in the World's favour (the pre-WP-1.1a World: 4.40× on the old burst, 3.98×
on the corrected one); `--legacy-burst` keeps the old burst runnable. The burst's creates are one
`spawnN()` batch per frame, which is item 4's path for homogeneous spawns; the per-command form is
reported next to it.

| | Before WP-1.1a | spawnN creates, GCC | spawnN creates, Clang | Per-command creates, GCC |
|---|---|---|---|---|
| **M1 (worst of 0/1/2/4 workers and both toggle storages; median over runs)** | 3.98× (3.71–4.65) | **1.54×** (1.48–1.73) | **1.59×** (1.51–1.80) | 2.33× |
| M1 per configuration, tag / DontFragment (medians) | 3.1–3.3× / 3.6–3.8× | 1.28–1.32× / 1.40–1.47× | 1.24–1.32× / 1.38–1.49× | 1.9–2.0× / 2.1–2.2× |
| Callgrind beside it (tag / DontFragment toggles) | 2.52× / 2.58× | **1.24× / 1.29×** | 1.29× / 1.30× | 1.65× / 1.69× |
| 9k-op burst on the VM, per configuration (budget 1.5 ms) | 2.9–3.3 ms | 1.26–1.27 ms | 0.96–0.98 ms | 1.7–1.9 ms |
| Same ops on raw flecs | 0.94–0.98 ms | 0.96–0.99 ms | 0.75–0.79 ms | 0.94–0.99 ms |

(9 runs of each spawnN column and of "before", 5 of the per-command column, interleaved.)

**M1 is met on the median of runs, without much margin**: 1.54× and 1.59× against ≤ 1.6×. Single runs
reach 1.73× (GCC) and 1.80× (Clang), because each run's M1 is the worst of eight noisy ratios, and 2 of
9 GCC runs and 4 of 9 Clang runs exceed 1.6×. DontFragment storage decides it: its raw toggles are
cheap, so the World's create and destroy bookkeeping weighs more. What remains per op (SPIKES §5.5) is
the identity read for the structural log (the largest item: a DontFragment op otherwise touches no
table row), the registry release on destroy, and a liveness check per command. More margin would need
the identity in a cache line the op touches anyway, such as a user word in flecs' entity record (a
vendored patch) or option B's own entity record. With per-command creates, and on the legacy burst
(2.49×), M1 is not met. **The Phase 1 midpoint rule** (a scoping spike for B above 2.5×) is not
triggered. The formal decision stays with M2 on SERVER at the Phase 1 gate.

Item 4's second half, toggles sorted by (source table, id) and moved in batches, was evaluated and not
adopted. flecs 4.1.6 has no public bulk move for entity lists, and the public-API version (a cached edge
with `ecs_commit`) costs 5 % more instructions unsorted and 14 % more sorted, without a wall-clock gain.
Sorting would also change the destination tables' row order. A real batched move needs an upstream API
(K10).

**M5: `InFrame` stays a fragmenting exclusive pair.** On the dev VM, with spawnN creates (first-round
code):

| InFrame | Tables | Chunks | Tick p50 / p99 | Iterate 50k×3 | ECS peak | 3k creates / 3k destroys | 9k burst |
|---|---|---|---|---|---|---|---|
| Fragmenting pair (kept) | 2,384 | 1,757 | 1.67 / 3.06 ms | 0.206 ms | 45.0 MB | 0.22 / 0.47 ms | 1.70 ms (11.3 M instructions) |
| DontFragment pair | 440 | 163 | 0.63 / 1.12 ms | 0.110 ms | 28.0 MB | 0.93 / 1.87 ms | 4.09 ms (23.5 M instructions) |

The DontFragment pair improves the clauses that already pass with margins of 2× or more, and makes the
failing structural clause 2.4× more expensive. It should be measured again on SERVER (M5's second site)
and whenever flecs makes changes to non-fragmenting pairs cheaper.
