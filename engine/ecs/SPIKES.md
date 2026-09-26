# engine/ecs Phase 0 spikes and RT-01 results

Everything here comes from `ecs_bench` (engine/ecs/bench). You can reproduce it with:

```
ninja -C build/<dir> ecs_bench
./build/<dir>/bin/ecs_bench                 # RT-01 zone for 0/1/2/4 workers, then spikes (a) and (b)
./build/<dir>/bin/ecs_bench --spikes-only   # spikes only
./build/<dir>/bin/ecs_bench --quick         # 60 ticks and smaller spikes (CI smoke, ~20 s)
./build/<dir>/bin/ecs_bench --inframe-dontfragment   # InFrame as a DontFragment pair (§2.3)
./build/<dir>/bin/ecs_bench --per-command-creates    # burst creates as spawn() + set() (§5)
```

§1–§4 are the Phase 0 results (WP-0.6, WP-0.8). §5 is WP-1.1a, the wrapper optimization of
ADR-004a option A, and has the current numbers and the release-build and callgrind commands.

**Machine.** The machine is a shared 4-vCPU cloud VM (Intel Xeon @ 2.8 GHz, 15 GB). It is not the
RT-01 reference box. Other build jobs ran on it throughout, with a load average between 2.4 and 5.3.
GCC 13.3 was used with `RelWithDebInfo` (`-O2 -g`) and **asserts enabled** (`HELIOS_ENABLE_ASSERTS=1`,
the default for that configuration), under flecs 4.1.6 and mimalloc 3.5.3.

Two full runs are quoted. **Run A** had a load of about 2.4. **Run B** used the final binary and had a
load of about 5, which means the 4 vCPUs were oversubscribed. Wall-clock numbers moved by up to 2x
between runs, and single bursts moved by up to 10x. For that reason the structural burst is reported
as the median of 7 bursts, next to its maximum. Where it matters, instruction counts from callgrind
are given as the machine-independent measure.

---

## 1. Spike (a): mimalloc heap thread affinity (ADR-011)

### 1.1 What mimalloc v3 changed

mimalloc 3.5.3 splits the old thread-bound `mi_heap_t` into two parts:

* **`mi_heap_t`**: a first-class heap. Any thread can allocate from it or free into it. Each thread
  lazily gets its own *theap* for the heap. The theaps live in versioned dynamic TLS slots, so the
  number of heaps is not limited by OS TLS keys.
* **`mi_theap_t`**: the thread-local view of a heap. Only the thread that obtained it may use it.

As a result, one heap per *tag group* can serve every job-system worker. It also serves jobs that are
stolen or that migrate between workers, and threads that exit. When a thread exits, its pages are
abandoned and later reclaimed, and its live blocks stay valid. The tests cover cross-thread frees,
thread exit with live blocks, and the recycling regression below.

### 1.2 Hazard found: heap recycling (mimalloc 3.5.3)

Every thread caches the theap it used last. It validates that cache only by comparing heap
**pointers**. Suppose a heap that another *live* thread has used is destroyed or deleted, and the next
`mi_heap_new()` returns the same address, which is the usual case. That thread then allocates from
the dead heap's zombie theap. In the raw reproduction, a crash was also seen in
`mi_arenas_page_try_find_abandoned`.

`ecs_bench` reproduces the hazard in the same process with the raw API and with `TaggedHeap`:

| | rounds | new heap at same address | allocation landed in wrong heap |
|---|---|---|---|
| raw `mi_heap_destroy` + `mi_heap_new` | 200 | 200 | **200** |
| `ecs::TaggedHeap` (pooled heaps) | 200 | n/a | **0** |

The same case is covered by the regression test "ecs heap spike: heaps used by several workers are recycled, never freed" in `tests/test_heap.cpp`.

### 1.3 Throughput (ns per alloc+free pair per thread, 16–528 B, batches of 256, best of 3)

| path | Run A 1 thr | Run A 4 thr | Run B 1 thr | Run B 4 thr |
|---|---|---|---|---|
| `mi_malloc` (process heap) | 8.3 | 10.0 | 10.8 | 17.6 |
| `mi_heap_malloc` (shared v3 heap) | 8.7 | 9.0 | 11.3 | 42.6 |
| `mi_theap_malloc` (per-thread theap) | 7.8 | 8.5 | 7.9 | 24.2 |
| `ecs::TaggedHeap`, exact tag stats | 76.9 | **885.6** | 78.4 | 461.6 |
| `ecs::TaggedHeap`, batched tag stats | 57.1 | 58.7 | 76.6 | 172.7 |
| `helios::alignedAlloc` (core: tag + header) | 41.4 | 464.5 | 77.8 | 295.1 |

Cross-worker frees, where a producer job allocates and a consumer job frees: `mi_malloc`/`mi_free`
cost 6–15 ns per pair, and `TaggedHeap` cost 140–210 ns per pair.

Where the time goes, from callgrind (`TaggedHeap`, batched, 1 thread): 364 instructions per pair.
`mi_usable_size` costs about 100 instructions per call and is called twice per pair: once to account
the allocation and once the free. Its cost includes an integer division in `_mi_page_ptr_unalign`.
`mi_heap_contains`, used by the dev-build `owns()` assert on free, costs about 47 instructions. Our
own shard counters cost about 60 instructions. With exact accounting, the core `MemoryTag` counters
add their shared atomics: `liveBytes`, `liveCount`, `totalCount` and a CAS loop on the peak. With 4
threads that shared cache line dominates: 460–890 ns per pair. Core `alignedAlloc` shows the same
collapse.

### 1.4 Also evaluated: mimalloc per-heap statistics as the tag source

mimalloc v3 keeps per-heap statistics at no extra cost (`MI_STATS=1` in release builds), which can be
read with `mi_heap_stats_get()` in about 1.3 µs. Both properties below were probed:

* **Accuracy.** Statistics are merged at page granularity, and a thread's theap only merges into the
  heap when *that thread* merges: on thread exit, on collection, or through an explicit
  `mi_theap_stats_merge_to_heap`. In one probe a live worker held 10.4 MB; a query from the main
  thread showed **0 bytes**. When the main thread then freed half of the worker's blocks, the query
  showed −5.4 MB.
* **Conclusion.** These statistics can only be the tag source if every worker merges at a sync point.
  That requires a "run on every worker" hook, which the JobSystem does not have. The option is
  parked; it should be revisited if the JobSystem adds such a hook.

### 1.5 Design and implementation (`include/helios/ecs/heap.h`)

`TaggedHeap` combines a v3 `mi_heap_t` with a `MemoryTag`. Its rules:

1. **Heaps are per tag group** (ECS, physics, script, and so on), not per object.
2. **A heap that other threads have used is never freed.** `~TaggedHeap` returns its heap to a
   process-wide pool, and pooled heaps are never freed, so no address is ever reused (§1.2). If live
   blocks remain, the heap is quarantined. Those blocks stay valid and the heap is not reused.
3. **Bulk free (`destroyAll`) works only for thread-confined heaps.** The heap tracks whether any
   thread other than its owner has used it. The thread id is cached per thread, because
   `currentThreadId()` is a `gettid` syscall on Linux.
4. **A `mi_theap_t*` is never cached across a job boundary or a `JobSystem::wait()`.** With Phase-2
   fibers, a job may resume on another thread.

Accounting uses `mi_usable_size`, which means no header and no size argument at free. The heap's own
counters are sharded per thread (32 cache-line shards). The tag can be fed in two ways:

* `Accounting::Exact` updates the tag on every allocation. Counts are exact, but the tag's shared
  atomics are contended.
* `Accounting::Batched` makes each thread shard forward its byte delta once it exceeds 256 KiB.
  `flushAccounting()` makes the tag exact at a quiescent point, such as the end of a tick. This mode
  is 5–15x faster under 4 threads.

### 1.6 Recommendations

* **ECS (flecs OS API):** use one process-wide `ecsHeap()` with tag "ECS". Exact accounting is
  enough, because flecs rarely goes to `malloc`: it has its own block allocators, and the heap never
  appeared in the RT-01 profiles.
* **High-rate allocators** (script VMs, network buffers): use `Accounting::Batched` with
  `flushAccounting()` once per tick, or a size-class pool above the heap.
* **Core change, requested but not made here:** the `MemoryTag` counters in `engine/core` should be
  sharded per thread, or get a `trackAllocationBatch(tag, bytesDelta, countDelta)` entry point. The
  peak CAS in particular should only be touched when the batch raises the peak. As it stands, core
  `alignedAlloc` is 30x slower than mimalloc under 4 threads, and the recycling hazard in §1.2 also
  applies to any core user of `mi_heap_*`.

---

## 2. Spike (b): flecs 4.1.6 DontFragment for high-churn data

In flecs 4.1, a `DontFragment` component or relationship lives in sparse storage, so adding or
removing it does not move the entity between tables. Two workloads were measured on 50k entities
with 3 components each.

### 2.1 Links: 20k entities linked to 2k targets, 9k re-targeted in one sync point

The table reports Run A / Run B. **+tables** is the number of new tables created. **assign** is the
initial 20k links. **churn** is 9k re-targets applied as one command buffer. **iterate** is a
Position+Velocity pass over all 50k entities. **lookup** returns everything linked to 100 targets.

| storage | +tables | assign ms | churn ms | iterate µs | lookup(100) µs | ECS MB |
|---|---|---|---|---|---|---|
| `DockedTo` fragmenting pair | 2000 | 27.9 / 35.8 | 17.4 / 21.4 | 158 / 158 | 81 / 63 | 21.1 |
| `DockedTo` DontFragment pair | **0** | 84.2 / 77.2 | 23.2 / 14.5 | 65 / 64 | 88 / 40 | 36.5 |
| `DockedTo` = `DockRef` field + reverse index | 1 | 36.5 / 20.3 | **2.8 / 2.0** | 68 / 64 | 174 / 106 | **14.1** |
| plain `Entity` field (no index) | 1 | 6.2 / 5.0 | 0.6 / 0.5 | 68 / 64 | 2784 / 2730 | 14.1 |
| `ChildOf` (fragmenting) | 2000 | 27.0 / 27.7 | 16.0 / 17.4 | 161 / 156 | 393 / 57 | 21.2 |
| `Parent` (flecs 4.1 non-fragmenting hierarchy) | 2 | 25.1 / 23.9 | **3.3 / 3.3** | 64 / 64 | 39 / 35 | 17.1 |

### 2.2 Status effects: 8 status components toggled on 9k of 50k entities per sync point (mean of 10 rounds)

| storage | +tables | toggle ms | iterate µs |
|---|---|---|---|
| regular components | 234 | 4.4 / 4.0 | 78 / 79 |
| DontFragment components | **0** | 6.2 / 5.2 | 65 / 63 |

The same toggles were also measured inside the RT-01 zone, on wide NPC entities with 8–12 components
(3k toggles, warm median, Run B). A fragmenting tag cost **1.9–2.3 ms** and a DontFragment `Status`
component cost **1.2 ms**. On the raw flecs C API the same toggles cost 0.7–1.35 ms and 0.72 ms. A
fragmenting toggle copies every column of the entity, so its cost grows with the width of the entity.
A DontFragment toggle does not grow with width.

### 2.3 InFrame as a DontFragment pair (the zone, 1 thread, same load)

| | tables | chunks | tick p50/p99 ms | iterate 50k×3 ms | ECS peak MB | 3k creates / 3k destroys ms |
|---|---|---|---|---|---|---|
| InFrame fragmenting (default) | 2384 | 1757 | 1.51 / 1.98 | 0.205 | 46.6 | 1.46 / 1.65 |
| InFrame DontFragment | **440** | **163** | **0.85 / 1.70** | **0.109** | **28.6** | 4.45 / 4.01 |

### 2.4 Findings

* DontFragment works in 4.1.6 for components and pairs, including exclusive pairs. It removes table
  fragmentation completely: 0 new tables in every case above.
* **DontFragment pairs are slow to change.** Every change of target emits events and updates the
  sparse relationship index. The initial assign of a DontFragment `DockedTo` pair costs 3x the
  fragmenting pair, and churn costs about the same as the fragmenting pair. It is 5–10x slower than a
  field with our own reverse index.
* **Queries touching DontFragment (and Sparse) data need per-entity access.** flecs marks such
  fields in `it.row_fields`; they must be read with `ecs_field_at`, and the World turns those results
  into single-row chunks. Keep DontFragment terms out of hot per-chunk iteration, or read the values
  through `ecs_get` in a second pass. (Review fix: the first version read them with `ecs_field` and
  crashed on any such query.)
* **flecs 4.1.6 iterates *optional* DontFragment terms incorrectly.** With `Counter` plus an optional
  non-fragmenting term, entities that lack the component were missing from one table's results and
  entities that have it were yielded twice in another. The World therefore leaves such optional
  terms out of the flecs query and resolves them per entity (`QueryPlan::lookupMask`). Non-optional,
  `With` and `Without` terms iterate correctly. Worth reporting upstream.
* Fragmentation costs table count, memory and iteration: 2000 extra tables made iteration 2.4x
  slower (158 µs vs 65 µs) and used 50% more memory in §2.1.

### 2.5 Recommendations (implemented as defaults: `RelationConfig`, `ComponentFlags`)

| data | storage | why |
|---|---|---|
| Hierarchy (`setParent`) | flecs 4.1 `Parent` (non-fragmenting), `nonFragmentingHierarchy = true` | 5x cheaper churn than `ChildOf` with no fragmentation, and the fastest children lookup |
| Docking (`DockedTo`) | `DockStorage::Field` (`DockRef` + reverse index) | cheapest churn and memory. Pairs are available as options (`Pair`, `PairDontFragment`) for systems that need to query by target |
| Frames (`InFrame`) | fragmenting exclusive pair by default, `inFrameDontFragment` option | the default keeps the RT-01 structural burst cheapest. The option halves tick, iteration and memory costs but makes creates and destroys 3x slower. **This should be decided in the RT-01 ADR** (§3.4) |
| High-churn flags and values on wide entities (status effects, combat state, …) | `ComponentFlags::DontFragment` | 0 tables and cheaper toggles on wide archetypes. The bench `Status` component is about 1.8x cheaper than a tag |
| High-churn data on narrow entities, or data read in hot loops | regular components | toggles on narrow entities are cheaper as table moves, and iteration stays per-chunk |

---

## 3. RT-01: 50k-entity zone (SERVER, 20 Hz)

> RT-01: 50k-entity zone, SERVER, 20 Hz (20k replicated + 30k placed, ~150 ECS archetypes, 5k bodies
> in 12 grids). Engine stages ≤ 12 ms p99; 9k structural ops ≤ 1.5 ms; 50k×3 iteration ≤ 0.4 ms on
> one thread; ECS ≤ 400 MB; ≤ 5,000 tables. Failure opens the custom-ECS ADR.

### 3.1 Zone and systems (bench/bench_zone.cpp)

The zone has 50,000 entities:

* 1,600 ships from 5 prefabs, with turrets on capitals and 400 of them docked
* 6,000 NPCs, 8,000 projectiles and 4,000 loot
* 30,000 placed props, with content-placed ids and reserved NetHandle slots
* 12 frames (grids), 5,000 physics bodies, 48 runtime variant tags and 7 replicated components

That gives **155 archetypes**, which are 6–12 components each and include `NetIdentity`/`RepDirty`.

Ten representative systems run on the deterministic scheduler with declared reads and writes:
AiScoring, ShipControl, WeaponFire, Integrate, IntegrateRotation, ProjectileUpdate, LootDecay,
ShieldRegen, ApplyDamage and SpatialHash. They use 2 sync points with structural commands (spawn and
despawn of projectiles). Each tick also runs `gatherChanges()`, which averages 17,460 field changes
on 8,249 entities per tick.

### 3.2 Results

| criterion | budget | Run A (load 2.4) | Run B (load 5, final binary) | verdict |
|---|---|---|---|---|
| engine stages p99, 4 workers | ≤ 12 ms | 4.54 ms (p50 3.14) | 10.20 ms (p50 1.79) | **PASS** (ECS stand-ins only, see below) |
| engine stages p99, 1 thread | – | 2.90 ms (p50 1.49) | 4.19 ms (p50 1.55) | – |
| 9k structural ops (3k create + 3k destroy + 3k tag toggles, warm) | ≤ 1.5 ms | 6.2–6.3 ms (first warm burst; one 25.5 ms outlier) | **6.07–6.71 ms** (median of 7, worst config; max 11.4) | **FAIL** |
| same 9k ops on the raw flecs C API | – | – | 0.96–1.71 ms (median) | (floor) |
| 50k×3 iteration, 1 thread | ≤ 0.4 ms | 0.200 ms | 0.203 ms | **PASS** |
| ECS memory (tag "ECS", peak) | ≤ 400 MB | 44.8 MB (24.7 live) | 46.6 MB (24.7 live) | **PASS** |
| tables | ≤ 5,000 | 2,384 (2,808 after the bursts) | 2,384 | **PASS** |
| ~150 archetypes | – | 155 | 155 | ok |
| determinism across 0/1/2/4 workers | – | identical state hash | identical (`2290440316d57edb`) | ok |

**RT-01 verdict: FAIL, on the structural-ops criterion only.** Per 02, this opens the custom-ECS ADR
(ADR-004).

Notes on the numbers:

* **Stage times cover the ECS side only.** There is no Jolt step and no replication encode. The
  full-engine p99 must be measured again when physics and networking land.
* **More workers made ticks slower on this machine.** 4 workers were slower than 1 thread because
  the per-system work is small (0.1–0.5 ms per system) and the VM was oversubscribed. On the
  reference box (≥ 8 cores) the 4-worker configuration should be measured again.
* **The Clang 18 build shows the same pattern.** A quick run gave a warm median of 3.2–4.2 ms
  for the 9k ops, against 0.81 ms on the raw flecs C API. Its tick p99 at 4 workers was 3.25 ms and
  iteration took 0.19 ms.
* **Typical tick load is far below the burst.** The representative tick has only 53 structural ops
  and 2 sync points costing 35–70 µs each. The 9k burst is the RT-01 peak case.

### 3.3 Where the 9k-op time goes

The tables below cover the warm burst: 3k creates in 12 frame groups (projectile archetype, 7
components each), 3k destroys, and 3k tag toggles on NPCs.

**Wall clock, Run B** (median of 7; raw flecs = same ops through `ecs_bulk_init`, `ecs_delete` and
`ecs_add_id`/`ecs_remove_id`, with no World bookkeeping):

| | 3k create | 3k destroy | 3k tag toggle | 3k DontFragment toggle |
|---|---|---|---|---|
| World (command buffers) | 1.75–1.85 ms | 1.9–2.5 ms | 2.1–2.3 ms | 1.17–1.24 ms |
| raw flecs C API | 0.02–0.05 ms | 0.21–0.27 ms | 0.73–1.35 ms | 0.72–0.75 ms |

**Instructions (callgrind, one warm burst):**

| | World apply | raw flecs |
|---|---|---|
| per create | ≈ 2.2k | ≈ 50 (bulk init with data) |
| per destroy | ≈ 1.5k | ≈ 1.0k |
| per tag or status toggle | ≈ 2.1–2.4k | ≈ 1.6–2.0k |
| all 12k commands | 27.4 M | ≈ 13.4 M |

How the World overhead breaks down:

* **Creates:** `spawnGroup` costs about 780 instructions per entity (NetHandle, registry inserts,
  NetIdentity and column writes). Allocating the EntityId costs about 70 more. `componentInfo` lookups cost about 600 per
  entity (8 per spawn at about 75 each). Command fusion, flattening and grouping cost about 700.
* **Destroys:** `unregisterSubtree` costs about 640 instructions (registry erase, handle release, the
  children lookup and the structural log).
* **Toggles:** `ecs_owns_id`, the liveness check, `componentInfo` and `logEvent` together cost about
  500 instructions per op.

### 3.4 Assessment for the ADR

* **flecs itself is at the budget, not the problem.** The same 9k operations on the raw flecs C API
  took 0.96–1.7 ms on this loaded VM, and 1.1–1.35 ms in quick runs. Tag toggles dominate that cost, because
  a toggle is a table move of a wide entity. A custom ECS would also pay for moves unless it stores
  such flags outside the archetype, and flecs already offers that as DontFragment: in the zone, it
  was about 1.8x cheaper than a tag.
* **Our wrapper costs 3–4x the flecs work.** That overhead is identity (EntityId and NetHandle
  maps), the structural log, dirty-tracking setup, and command-buffer fusion and grouping. A custom
  ECS needs the same identity and replication bookkeeping, so replacing flecs would not remove this
  part.
* **Recommended ADR-004 outcome: keep flecs and cut the wrapper overhead.** Then measure RT-01 again
  on the reference hardware. Concrete items, in expected-gain order:
  1. **Batch identity bookkeeping per spawn group.** Reserve EntityIds and NetHandles in blocks,
     bulk-insert them into the registry, and write `NetIdentity` columns in one pass.
  2. **Replace the `componentInfo` hash lookup** (U64Map with `mix64`) by a direct array for component
     ids below 64k. Cache the `ComponentInfo*` in the flattened op list.
  3. **Skip the children walk on destroy** when the entity has no `(ChildOf, e)`/Parent record
     (`ecs_id_in_use`), and skip `ecs_owns_id` on remove by comparing the table before and after.
  4. **Specialize command buffers.** For homogeneous spawns, add a `spawnN(prefab or signature,
     values)` path that bypasses per-command fusion. For tag toggles, sort commands by (source table,
     id) and move runs of entities in batches. flecs 4.1 has no public batched move for entity
     lists, so this needs an upstream API or a table-level bulk path.
  5. Decide `InFrame` storage together with (4). The DontFragment option halves tick and iteration
     costs, but it currently triples create and destroy cost.

  A 3x reduction of the wrapper overhead would bring the burst to about 2–2.5 ms on this VM. Reaching
  1.5 ms also needs faster hardware, or a relaxed budget of 9k ops per *two* sync points. (ADR-004a
  rejected the second reading. The outcome of these items is §5.)
* **The other RT-01 criteria pass with large margins:** iteration at 50% of budget, memory at 12%,
  tables at 48%, and deterministic parallel execution.

### 3.5 Other validation

* The **ThreadSanitizer** run (GCC 13, `-fsanitize=thread`) passed all 73 test cases with 18
  reports (see §4.3 for the run after the review). Every report is inside flecs' own worker pipeline: non-atomic statistics counters
  (`ecs_os_linc`, which flecs comments as "ok, only for stats") and `prev_match_count` in
  `flecs_query_cache_iter_init`. They come from the test that runs flecs-native multi-threaded
  systems on JobSystem tasks. The Helios scheduler, command buffers, dirty tracking and TaggedHeap
  produced **no** reports.
* **ASan+UBSan** passed the tests and `ecs_bench --quick` with no reports.

---

## 4. Review addendum (adversarial review of WP-ECS)

### 4.1 Changes that affect these results

* **EntityId layout.** Runtime ids are now the 05 §1.4.5 block ids (41-bit prefix / 5-bit shard /
  17-bit offset), which are the same as Go's `services/pkg/idgen` and are checked against its golden
  vectors. The 41/5/8/9 Snowflake layout used before is the one 05 rejected. It could also collide
  with block ids that Go services mint for the same shard, because its node and sequence bits
  overlap the 17-bit offset. Minting is one CAS per id from held blocks (`EntityIdMinter`); the
  per-id clock call is gone.
* **Dirty tracking.** A `set` of an owned replicated component now marks every field and keeps the
  bits already pending. Before, it overwrote them with the value's mask, which was 0. Adds, spawns,
  prefab copies and overrides now start with a clean mask. This adds one lookup per replicated
  `set`, which the RT-01 burst does not exercise.
* **Destroy.** Destroying a frame or docking host now logs the implicit `SetFrame(0)` / `Undock`
  events. This adds one `ecs_owns_id` and one hash lookup per destroy.

### 4.2 RT-01 re-run (GCC 13 RelWithDebInfo, same shared 4-vCPU VM, load about 4–5)

| criterion | budget | measured | verdict |
|---|---|---|---|
| engine stages p99, 4 workers | ≤ 12 ms | 3.35 ms (p50 1.75) | PASS |
| 9k structural ops, warm median (worst config) | ≤ 1.5 ms | 3.40–4.19 ms | **FAIL** |
| same ops on the raw flecs C API | – | 0.91–0.93 ms | (floor) |
| 50k×3 iteration, 1 thread | ≤ 0.4 ms | 0.204 ms | PASS |
| ECS memory peak | ≤ 400 MB | 46.6 MB | PASS |
| tables | ≤ 5,000 | 2,384 | PASS |
| determinism across 0/1/2/4 workers | – | identical (`e099e42eb09fc124`) | PASS |

Burst breakdown (warm median, 1 worker): create 1.08 ms, destroy 1.19 ms, tag toggle 1.14 ms; raw
flecs 0.02 / 0.18 / 0.71 ms. The verdict and the §3.4 assessment stand. The Clang 18 quick run was
also deterministic; its burst medians were 3.6–9.3 ms on the loaded VM.

**Prefab children.** `addPrefabChild` used `ChildOf`, so every instance of a prefab with children
created its own `(ChildOf, instance)` table. 100 two-level instances added 204 tables. That would
have broken the 5,000-table cap for a zone of ship prefabs with turrets, which the RT-01 zone did
not model (its turrets use `setParent`). Prefab children now use `Parent` when the hierarchy is
non-fragmenting. flecs 4.1.6 instantiates those children with `EcsTreeSpawner`, and 200 instances
add fewer than 20 tables (`test_regressions.cpp`).

### 4.3 Validation after the review

* 90 test cases pass on GCC 13 and Clang 18, several runs each and concurrently. The MinGW-w64
  cross build compiles and links warning-free; the tests were not run (no Wine).
* **ASan+UBSan** (GCC): the tests and `ecs_bench --quick` produce no reports.
* **TSan** (GCC; Clang's TSan runtime is not installed on this VM): 13 reports. All of them are in
  flecs' native worker pipeline, in the flecs task-thread test: `ecs_os_linc` statistics counters
  and `prev_match_count`, as in §3.5. None are in Helios code.

---

## 5. WP-1.1a: wrapper optimization (ADR-004a option A)

### 5.1 What changed

The items of §3.4, as ADR-004a numbers them. The structural log, identities, dirty bits, flecs ids,
tables and row order are unchanged: `tests/test_structural_ops.cpp` digests a scripted workload over
every structural command kind in four relation configurations and compares it with the digest of the
pre-WP-1.1a World (commit f08cf5b, GCC 13 and Clang 18 alike).

1. **Identity per spawn group.** EntityIds for a whole buffer come from `EntityIdMinter::allocateN`
   (one CAS per run within a block; the same ids as one `allocate()` per spawn). The registry reserves
   once and inserts with one lookup (`addNew`); NetHandles skip the lookup and the error object
   (`tryAssignHandle`); NetIdentity is written in the same pass. Runtime block ids live in pages of 64
   consecutive ids (§5.5), content-placed and client-local ids in the hash map.
2. **`componentInfo()`** is an inline array lookup for component ids below 64k, and the flattened spawn
   ops carry their `ComponentInfo` (which now points at its hooks).
3. **No needless work.** Destroy walks children, frame members and docking pairs only when flecs has
   flagged the entity as a pair target (`EcsEntityIsTarget`; `ecs_delete` relies on the same flag), and
   a filter over DockRef hosts skips the reverse-index lookup for other entities. Add, remove and set
   of a table-stored component compare the entity's table before and after instead of calling
   `ecs_owns_id`. The command-buffer path checks liveness once per command and logs from the record.
4. **Specialized command buffers.** `CommandBuffer::spawnN(desc, count, columns)` records homogeneous
   spawns as column arrays; the World creates them with one bulk insert, identical to `count` `spawn()`
   + `set()` commands (`tests/test_bulk_paths.cpp`). Buffers note at record time whether every Set/Add
   directly follows its spawn, and then fuse those runs without the per-temp linked lists; buffers
   without spawns skip fusion. Group values are copied column by column. Commands shrank from 48 to 32
   bytes. Batched toggles were evaluated and not adopted (§5.6).
5. **`InFrame` storage** stays fragmenting (§5.7, ADR-004a M5).

### 5.2 How to measure

M1 needs a release build with asserts off. The `linux-bench` preset is GCC `Release`, headless, with
the bench CTests on:

```
cmake --preset linux-bench && cmake --build --preset linux-bench
./build/linux-bench/bin/ecs_bench --no-spikes           # prints "ADR-004a M1 (World / raw flecs)"
./build/linux-bench/bin/ecs_bench --no-spikes --m1-gate # exit code = the M1 verdict
cmake -DECS_BENCH=build/linux-bench/bin/ecs_bench -P engine/ecs/bench/callgrind_burst.cmake
ctest --test-dir build/linux-bench -L callgrind          # the same, as a CTest (valgrind found)
```

`ecs_bench` prints M1 per configuration for tag and DontFragment toggles, and the worst of all of them
in the verdict. The callgrind job runs one single-threaded zone under
`valgrind --tool=callgrind --toggle-collect='bench::timed::*'`, which collects only the timed burst
phases: 6 warm World bursts and the 6 raw-flecs bursts with the same apply/revert mix. It prints the
instructions per burst and phase and the World / raw ratios, and passes `-DEXTRA_ARGS=...` through
(`--per-command-creates`, `--inframe-dontfragment`). A nightly hook belongs to WP-0.3's workflow.

### 5.3 Bench corrections

Every correction makes the World and the raw-flecs halves do the same work. None changes the zone,
the ticks or the state hash (`e099e42eb09fc124`, the same as §4.2).

* **Apply/revert parity.** The rounds alternate between applying and reverting the toggles, which cost
  differently (a DontFragment set is dearer than its remove). The World's warm rounds 1–7 and the raw
  rounds 8–14 had opposite mixes; one discarded raw burst now aligns them.
* **Destroy victims.** The warm bursts were documented to destroy the previous round's projectiles but
  took the first 3,000 in query order, the zone's older ones. The burst tables then grew every round,
  a cost the raw burst never paid. They now destroy the previous round's creates, in creation order.
* **Raw destroys.** The raw burst deleted its own creates, which sat at the ends of their tables, so
  half its deletes moved no row. It now deletes the previous raw burst's creates, like the World.
* **Raw creates** read 250-element arrays for all 12 frames; they now read 3,000 distinct values.
* The burst's command buffers are kept across rounds, as the scheduler keeps its per-job buffers.
* The burst creates are 12 `spawnN()` batches, one per frame: how a system records a volley.
  `--per-command-creates` keeps the spawn() + 7 set() form, and both are reported below.

### 5.4 Results

The same shared 4-vCPU VM as §3–4 (Xeon at 2.8 GHz, 33 MB L3 shared by the host), GCC 13.3 `Release`
(asserts off), flecs 4.1.6. "Before" is this branch's bench against the World of f08cf5b; that World
has no `spawnN()`, so its creates are per command.

**Instructions** (callgrind, per warm burst, mean of 6, 1 worker; per op):

| | before | after, spawnN creates | after, per-command creates | raw flecs |
|---|---|---|---|---|
| 3k creates | 2,771 | 352 | 1,317 | 55 |
| 3k destroys | 1,821 | 1,250 | 1,250 | 829 |
| 3k tag toggles | 2,449 | 2,159 | 2,159 | 1,937 |
| 3k DontFragment toggles | 2,578 | 2,347 | 2,347 | 1,880 |
| **9k ops, tag toggles** | 21.13 M (2.50×) | **11.29 M (1.33×)** | 14.18 M (1.68×) | 8.46 M |
| **9k ops, DontFragment toggles** | 21.51 M (2.59×) | **11.85 M (1.43×)** | 14.75 M (1.78×) | 8.30 M |

**Wall clock** (7 runs of each binary, interleaved, full `--no-spikes` run each, load 1.7–2.2 from
other jobs; median [min–max] over the runs):

| | before | after, spawnN creates | after, per-command creates |
|---|---|---|---|
| **M1 (per run: worst of 0/1/2/4 workers and both toggle storages)** | 5.45× [5.14–6.67] | **2.05× [1.74–3.84]** | 2.99× [2.76–5.50] |
| M1 per configuration, tag / DontFragment (medians, 0/1/2/4 workers) | 4.9–5.1 / 4.5–5.0 | 1.59–1.87 / 1.64–1.75 | 2.38–2.90 / 2.40–2.65 |
| 9k World burst, worst configuration per run | 5.86 ms | 2.04 ms [1.44–4.15] | 3.04 ms |
| 9k World burst, medians of the configurations | 4.83–5.31 ms | 1.44–1.77 ms | 2.24–2.74 ms |
| same ops on raw flecs | 0.96 ms | 0.95 ms | 0.94 ms |
| creates / destroys / tag toggles, World (medians over configurations) | 1.42 / 1.97 / 1.59 ms | 0.22 / 0.47 / 1.01 ms | 0.64 / 0.78 / 1.21 ms |
| the same on raw flecs | 0.02 / 0.24 / 0.69 ms | 0.02 / 0.24 / 0.67 ms | 0.02 / 0.24 / 0.67 ms |

Two earlier series of 5 runs with earlier builds of this branch (loads about 1.2 and 1.8) gave the
same picture: 4.56× and 5.16× before, 2.12× and 2.08× with spawnN creates, and per-configuration
medians of 1.56–1.94×.

The other RT-01 clauses are unchanged (medians over configurations): iteration 0.206 ms, ECS peak
45.0 MB (45.7 before: the paged registry is smaller), 2,384 tables, tick p50 1.67 ms and p99 3.1 ms at
4 workers (the ECS-side stages only, as in §3.2), deterministic across 0/1/2/4 workers with the same
state hash as before.

### 5.5 Why the wall clock lags the instruction count

With callgrind's cache model at an 8 MB last level, the World phases missed that level far more often
than the raw ones. First, the registry's hash map: one random line per spawn and per destroy. Runtime
ids are minted consecutively, so they now live in pages of 64 ids and a burst touches a few lines per
64 ids. After that change the World toggles still missed about 5× as often as the raw toggles
(16k against 3.3k read misses per 6 bursts), all of it in flecs' own row copies. The rows had been
evicted by the World creates just before them: skipping those creates in an experiment removed the
difference. With a 16 MB last level the World toggles miss 1.2k times (raw: under 10), and with 32 MB
neither misses. So the remaining gap is capacity: the World's command buffers, payload arrays and structural
log (12,000 events of 32 bytes per burst) are a larger footprint than the raw calls, and on this VM
the 33 MB L3 is shared with the host's other tenants and with the other jobs. A quiet machine keeps more of the zone
in its last-level cache, which is one reason M2 is measured on the SERVER box without other load.

### 5.6 Batched toggles (item 4, second half): evaluated, not adopted

flecs 4.1.6 has no public API that moves a list of entities between tables. Through the public API a
batch can only skip the per-entity edge lookup, with `ecs_commit` and a cached destination table.
On the raw burst's toggles (callgrind, 6 bursts): `ecs_add_id`/`ecs_remove_id` in command order took
34.86 M instructions, `ecs_commit` with a cached edge in command order 36.65 M (+5 %), and the same
sorted by (source table, id) 39.86 M (+14 %, the sort). The wall clock did not improve either (sorted
0.72–0.85 ms against 0.61–0.79 ms). Sorting also changes the row order of the destination tables,
which the digest pins. So the toggles keep per-entity moves. A real batched move would need an
upstream flecs API: append n rows to the destination table, move the columns in bulk, and delete n
rows from the source in command order. That is a request for upstream (K10); this WP did not file it.

### 5.7 `InFrame` storage (ADR-004a M5)

The zone and burst with `InFrame` as a DontFragment pair (`--inframe-dontfragment`), final code, spawnN
creates, medians over configurations (3 runs; fragmenting from the 7 runs of §5.4):

| InFrame | tables | chunks | tick p50 / p99 | iterate 50k×3 | ECS peak | 3k creates / 3k destroys | 9k burst (instructions) |
|---|---|---|---|---|---|---|---|
| fragmenting pair (default) | 2,384 | 1,757 | 1.67 / 3.06 ms | 0.206 ms | 45.0 MB | 0.22 / 0.47 ms | 1.70 ms (11.3 M) |
| DontFragment pair | **440** | **163** | **0.63 / 1.12 ms** | **0.110 ms** | **28.0 MB** | 0.93 / 1.87 ms | 4.09 ms (23.5 M) |

**Decision: `InFrame` stays a fragmenting exclusive pair.** The DontFragment pair halves tick,
iteration and memory, but those clauses already pass with margins of 2× or more. It makes the one
failing clause, the structural burst, 2.4× more expensive: every create adds the pair per entity and
every destroy removes it from flecs' sparse relationship index, 3,434 instructions per destroy
against 1,250. Revisit it if flecs makes non-fragmenting pair changes cheaper or frames become many.

### 5.8 Verdict

* **M1 is not met on the dev VM.** By the ADR's definition (worst of 0/1/2/4 workers and both toggle
  storages, same run, release build), M1 is **2.05×** (median of 7 runs, 1.74–3.84), against ≤ 1.6× and
  5.45× before. The per-configuration medians are 1.59–1.87×. In instructions the World burst is 1.33×
  (tag toggles) and 1.43× (DontFragment toggles) the raw-flecs burst. With per-command creates M1 is
  2.99× (1.68× and 1.78× in instructions).
* The 9k-op burst takes 1.44–1.77 ms per configuration (median) on this VM, 2.04 ms in the worst
  configuration of a run, against 4.8–5.9 ms before and the 1.5 ms budget. The formal clause is M2 on
  SERVER.
* ADR-004a's Phase 1 midpoint rule (a scoping spike for option B above 2.5×) is not triggered by
  these numbers.
* What is left: the World's toggles and destroys run at about 1.1× and 1.5× the raw instructions, and
  creates at 6× (0.2 ms). The remaining wall-clock gap is mostly the cache footprint of §5.5.
