# engine/ecs — Helios ECS on flecs v4.1.6

`helios::ecs` (target `helios_ecs`, HEADLESS, L2) wraps **flecs v4.1.6** (ADR-004) behind a Helios
API: identity (EntityId / NetHandle / flecs entity), component registration driven by templates or
runtime descriptors, relationships and prefabs, a deterministic job-system scheduler, command
buffers applied at sync points, and Iris-style per-field dirty tracking for replication (02 §4).
flecs is private to `src/` — public headers only forward-declare `ecs_world_t` — so gameplay code
never compiles the 40k-line flecs header; `World::flecsWorld()` is the explicit escape hatch.

Deps: `helios::core` (log, assert, Result, jobs, memory tags, time, hashing), `helios::math`
(`FrameId`), `helios::tp::flecs`, `helios::tp::mimalloc`.

| Header (`helios/ecs/…`) | What it provides |
|---|---|
| `types.h` | `Entity` (flecs id), `EntityId` (block id / content hash62 / client-local), `NetHandle` (24-bit index + 8-bit generation), `NetIdentity`, `RepDirty`, `ComponentId`, `Tick`, `FieldMask` |
| `entity_id.h` | Block ids 41/5/17 (05 §1.4.5, same layout and golden vectors as Go `pkg/idgen`): `EntityIdMinter` (lock-free minting from held blocks, `allocateN` for a whole spawn batch, refill at half use, 1 h retirement, async delivery), `IdBlockSource` / `LocalIdBlockSource` (the AllocateIdBlocks rule in-process), `ClientLocalIdAllocator` |
| `registry.h` | `U64Map` (open addressing, backward-shift delete), `NetHandleTable` (content-reserved slots, FIFO reuse with delay, generations), `EntityRegistry` (EntityId ↔ Entity ↔ NetHandle; runtime ids in pages of 64 consecutive ids) |
| `component.h` | `ComponentDesc` (runtime path: name, size, align, hooks, flags, dirty layout), `componentDescOf<T>()` (template path), `ComponentFlags` (Shared, Sparse, DontFragment, Singleton, Replicated, DontInherit), `kFieldIndex<C, &C::field>` |
| `dirty.h` | `Mut<C>` / `MutColumn<C>` per-field dirty writers, `ChangeList` / `ComponentChange` |
| `command_buffer.h` | `CommandBuffer` (spawn → `TempEntity`, `spawnN` for homogeneous batches, destroy, add/remove/set, setParent, reparent (InFrame), dock/undock), `SpawnDesc`, `SpawnColumn`, `EntityRef` |
| `system.h` | `Stage` (04 §3.3 cell stages), `Term`/`TermAccess`, `SystemDesc`, `SystemBuilder`, `UpdatePolicy`, `ChunkView`, `SystemContext`, `SystemStats` |
| `world.h` | `World` (+ `WorldDesc`, `RelationConfig`, `DockStorage`), `Query`, `StructuralEvent`, `FrameRef`, `DockRef` |
| `heap.h` | `TaggedHeap` (mimalloc v3 heap + memory tag, pooled, sharded accounting), `ecsHeap()`, `ecsMemoryTag()` |
| `os_api.h` | `installFlecsOsApi()` (flecs → Helios log/assert/time/heap/threads/JobSystem tasks), `LogEcs`, stats |
| `ecs.h` | umbrella |

## Quick tour

```cpp
struct Health {                                    // hand-written stand-in for schemac output
    f32 hp = 100, maxHp = 100;
    ecs::FieldMask _dirty = 0;
    static constexpr auto kReplicatedFields = std::make_tuple(&Health::hp, &Health::maxHp);
};

jobs::JobSystem js;
ecs::World world({.jobs = &js, .shard = 3, .idBlocks = &orchestratorIdBlocks});  // or omit: local source
world.registerComponent<Health>();                  // template path (replicated: World adds RepDirty)
world.registerComponent<HullSpec>(ecs::ComponentFlags::Shared);   // @shared -> prefab-inherited
Result<ComponentId> blob = world.registerComponent(descFromReflection);  // runtime path

const Entity corvette = world.createPrefab("Corvette");
world.set(corvette, HullSpec{...});                 // shared by instances until overridden
const Entity ship = world.instantiate(corvette, {.frame = systemFrame, .ag = agId});

world.system("Regen").stage(ecs::Stage::PostPhysics).write<Health>().read<HullSpec>()
    .each([](ecs::SystemContext& ctx, ecs::ChunkView& c) {
        auto hp = c.mutColumn<Health>(0);           // dirty-tracking writer per row
        for (u32 r = 0; r < c.count(); ++r) {
            const f32 cap = c.at<HullSpec>(1, r).maxHp;   // shared (prefab) or owned
            if (hp[r]->hp < cap) hp[r].set<&Health::hp>(std::min(cap, hp[r]->hp + ctx.dt()));
            if (hp[r]->hp <= 0) ctx.commands().destroy(c.entity(r));   // applied at the sync point
        }
    });

world.tick(0.05f);                                  // all stages: gather -> jobs -> sync point
ecs::ChangeList changes;
world.gatherChanges(changes);                       // (EntityId, NetHandle, component, field mask)
```

## Design notes

**Identity (02 §4.1).** `spawn()` mints a block EntityId (or takes a content-placed id), issues a
NetHandle (dynamic, or a container slot via `contentHandleIndex`), and stores `NetIdentity` on the
entity. `World::destroy()` walks the (ChildOf/Parent) subtree, unregisters every identity and logs
`Destroy` before calling `ecs_delete`, so cascades and prefab children are covered. There is
deliberately no `on_remove` hook or observer on `NetIdentity`: any lifecycle hook makes every table
containing the component a flecs "complex" table and disables flecs' fast append/move/delete paths
(measured ~2x on structural ops). Entities must therefore be deleted through `World::destroy()`,
not raw `ecs_delete`. Children instantiated from prefab hierarchies get identities too. A spawn group or `spawnN` batch mints its EntityIds in one
`allocateN` (the same ids as one `allocate()` per spawn, in command order), registers them with one
lookup each, and writes NetIdentity in the same pass. The registry keeps runtime block ids in pages of
64 consecutive ids, so a burst of spawns or destroys touches a few cache lines per 64 entities
(content-placed ids stay hashed). The subtree walk (children, frame members, docking pairs) runs only
for entities flecs has flagged as a pair target (`EcsEntityIsTarget`, the flag `ecs_delete` uses for
its own cleanup).

**Components.** Both registration paths end in `registerComponent(const ComponentDesc&)`. "Plain"
components (no destruct/copy/move hooks — all trivially copyable C++ types) register with no flecs
hooks at all; their `construct` runs once to capture a default-value blob that the World writes
wherever it adds the component without a value. Non-trivial types get flecs type hooks that
trampoline to `ComponentHooks` (construct/destruct/copy/move over arrays — the shape a reflection
`TypeOps` will fill). Flags map schema attributes to flecs traits (`@shared` →
`(OnInstantiate, Inherit)`, `@sparse`, DontFragment, `@singleton`). Replicated components make the
World add `RepDirty` in the same structural step (spawn, add, set, override, command buffers), so
the per-entity summary appears without a flecs `With` trait (which would create intermediate
tables). Spawns and command-buffer groups look up their final table with `ecs_table_find` (no
intermediate tables in the table graph).
C++ types map to ids through a process-wide type slot → per-world vector (O(1), lock-free).
Component names must not resolve to an existing flecs entity (builtins, relations, named frames or
scopes); such registrations fail with `AlreadyExists` instead of silently re-typing that entity.

**Entity ids (05 §1.4.5).** Runtime EntityIds are block ids: `0 | 41-bit block prefix | 5-bit shard
| 17-bit offset`, shared with the Go services (`services/pkg/idgen`, golden vectors in
`services/testdata/vectors/block_ids.json`, checked by `ecs_tests`). There are no node ids: prefixes
come from `AllocateIdBlocks` (the orchestrator's `id_alloc` row per shard advances to
`max(last + n, now_ms)`), so ids minted by cells and by Go services never collide. `EntityIdMinter`
holds `idHoldBlocks` blocks (2, or 4 for battle-profile cells) and mints with one CAS per id; the
World calls `maintain()` in `beginTick()`, which refills once the current block is half used and
retires blocks an hour after allocation once a fresher one is held. A cell passes its orchestrator
client as `WorldDesc::idBlocks` (an async source answers `Busy` and delivers via `addBlocks()`);
without one the World uses a private `LocalIdBlockSource` on `idClock` (tests, tools, offline
worlds, deterministic replays), whose row value can be persisted (`idLastPrefix`). A spawn that
cannot get an id is refused (`WorldStats::commandsDiscarded`), as is a spawn whose prefab, parent or
frame is not alive.

**Relationships.** Hierarchy = flecs 4.1 non-fragmenting `Parent` (cycle-checked, cascade delete);
`InFrame` = exclusive pair (fragmenting by default: frames are few) with a reparent hook for the
world module (02 §5.3); `DockedTo` = `DockRef` field + reverse index by default (pairs selectable);
`IsA` prefabs with Shared (inherited) vs copied components and `override<T>()`; prefab children
(`addPrefabChild`) use `Parent` too, which flecs 4.1.6 instantiates without a table per instance.
Destroying a frame or docking host logs `SetFrame(0)` / `Undock` for the entities that were in it.
The spike behind these choices is SPIKES.md §2.

**Command buffers and sync points.** During a stage flecs is structurally read-only; every job
records into its own `CommandBuffer`, and the stage's sync point applies them in (system, job)
order. Job partitions are contiguous row ranges of the deterministic query order, so the applied
sequence — and therefore EntityIds, NetHandles, flecs ids and table row order — is identical for
any worker count (tested for 0/1/2/4 workers). At apply time set/add commands recorded for a
`TempEntity` are fused into its spawn; spawns with the same (frame, component list) are created
with one `ecs_bulk_init` into their final table and their values written in place (EntityIds are
still minted in command order). Fused initial values do not fire flecs OnSet observers. A buffer notes while recording whether every Set/Add
on a TempEntity directly follows its `spawn()`; then fusion takes those runs as they are, otherwise
it links the ops per temp. `spawnN(desc, count, columns)` records homogeneous spawns as column arrays
of trivially copyable values and applies them with one bulk insert, with the same result as `count`
`spawn()` + `set()` commands (identities, log, tables and row order, dirty bits); later commands on
batch entities apply in place. Add, remove and set of a table-stored component tell whether they
changed anything from the entity's table before and after (no `ecs_owns_id`).

**Dirty tracking (02 §4.4, 04 §4.2).** `Mut<C>::set<&C::f>(v)` writes and marks field bits in
`C::_dirty` (skipping unchanged values), and ORs the component's bit into the entity's `RepDirty`
with relaxed `std::atomic_ref` operations, so parallel systems writing different components of the
same entity are race-free. The summary bit is set on every write, even when the field bits were
already set, so stale bits can never hide a change. `World::set` / `setRaw` / `CommandBuffer::set` on
a component the entity already owns count as `raw()` writes (all fields marked, pending bits kept);
adding a component, spawning with it, instantiating or overriding starts it with a clean `_dirty`
mask (the Create/Add event already sends the whole value). `getMut` / `getMutRaw` are the untracked
paths. `gatherChanges()` scans the RepDirty tables (in parallel for large worlds) and returns a
deterministic `ChangeList`, clearing the bits. Structural changes go to the per-tick
`structuralLog()`.

**Scheduler (02 §4.3).** Systems declare stage, order, `after` edges, query terms (read / write /
with / without / optional) and random-access reads/writes. `buildSchedule()` computes a stable
linear order per stage (Kahn with (order, name) tie-break — independent of registration order) and
a DAG with edges between conflicting systems; non-conflicting systems run concurrently on the
JobSystem, each split into jobs of ~`chunkGrain` rows. Update policies: every tick, every N ticks,
staggered (a `once` function still runs every tick; the chunks follow the stagger). Per-system
stats (last/avg/max ns, rows, jobs, over-budget runs) and per-stage sync times are recorded.
Undeclared random access and structural calls during a stage assert. Query terms on Sparse or
DontFragment components work (flecs requires per-entity field access for them, so their results
become single-row chunks); optional terms on DontFragment components are resolved per entity by
the World because flecs 4.1.6 iterates them incorrectly (entities skipped or repeated).

**flecs OS API (ADR-011).** Installed once per process before any flecs call: memory → `ecsHeap()`
(mimalloc heap, tag "ECS"), log → `HELIOS_LOG_*` on channel `ECS`, abort → Helios assert handler,
time → monotonic clock, threads → `helios::Thread`, **tasks → JobSystem jobs**: flecs 4.1.6's
`ecs_set_task_threads` creates `task_new_` tasks at the start of each `ecs_progress` and joins them
at the end; Helios submits each as a High-priority job and joins with a helping `JobSystem::wait`.
Those tasks block on flecs' condition variables for the whole progress, so the count is clamped to
the worker count. This path exists for flecs-native systems/addons; Helios systems use the
job-native scheduler above and never block workers.

**Heaps (ADR-011, SPIKES.md §1).** `TaggedHeap` = a mimalloc v3 heap (usable from any thread) +
a MemoryTag, recycled through a process-wide pool because freeing a heap that live workers used is
unsafe in mimalloc 3.5.3; bulk `destroyAll()` only for thread-confined heaps; per-thread sharded
counters, and optional batched tag accounting.

## Threading rules

* A `World` is driven by one thread. All mutating calls (spawn, destroy, set, relationships,
  apply, registerComponent, addSystem) happen outside `tick()`/`runStage()` stages (asserted).
* System functions run on job workers. They may write only the components they declared as Write
  terms (chunk columns) — random-access writes (`SystemContext::mut`) need `singleJob()` — read
  declared components (`SystemContext::get`, checked in dev builds), and record structural
  changes in `ctx.commands()`.
* `Mut<C>` on one entity from two threads is only valid for *different* components `C`.
* `Query`, `ChunkView` data and component pointers are invalidated by structural changes; `Query`
  objects must be destroyed before their `World`.
* `EntityIdMinter::allocate`, `TaggedHeap` allocate/free, `installFlecsOsApi` and the OS API
  hooks are thread-safe.

## Tests and benchmark

`ecs_tests` (doctest, 98 cases) covers: block-id layout against the shared Go golden vectors,
the AllocateIdBlocks rule, minting (order, refill at half use, retirement, stalls, failed and async
sources, concurrency, determinism, restarts, never id 0); U64Map fuzz vs `std::unordered_map`; NetHandle FIFO/reuse delay/
generations/content slots; registry maps; TaggedHeap accounting, pooling, the mimalloc recycling
hazard regression, cross-worker frees and thread exit; OS API (heap, tags, logs, time, flecs task
threads on the JobSystem); template/runtime registration, hooks, traits, singletons, names;
hierarchy/reparenting/cycles/cascades (both storages), frames + reparent hook, docking (all
storages), prefabs (shared, copied, overrides, children with identities); command buffers (fusion,
bulk groups, ordering, payload lifetimes, stale targets, cycles in one batch); dirty tracking
(field masks, raw, parallel systems on shared entities, random-access writes); scheduler (stable
order, dependencies, conflicts, bit-identical results for 0/1/2/4 workers, update policies, stats,
access asserts); and `test_regressions.cpp` for defects found in review (sparse/DontFragment query
fields, dirty masks on whole-value writes, heap-pool ownership, dead relationship targets, prefab
children fragmentation, staggered `once`, frame/host destruction in the log, component-name
collisions, id-0 commands). WP-1.1a's structural fast paths are pinned by `test_structural_ops.cpp`
(a digest of a scripted workload over every structural command kind in four relation configurations,
recorded with the World before WP-1.1a) and `test_bulk_paths.cpp` (spawnN against the same spawn +
set commands, `allocateN` and batched NetHandle issue against one call per id, the paged registry
under churn and its handle releases, destroy runs against one destroy at a time, Sparse/DontFragment
ownership, fusion runs, the dock-host filter).

```
cmake -S . -B build/ecs -G Ninja -DHELIOS_BUILD_GRAPHICS=OFF
ninja -C build/ecs helios_ecs ecs_tests ecs_bench && ./build/ecs/bin/ecs_tests
./build/ecs/bin/ecs_bench            # RT-01 zone for 0/1/2/4 workers + spikes; exit 0 = PASS
./build/ecs/bin/ecs_bench --quick --spikes-only
```

`ecs_bench` is not a CTest by default; `-DHELIOS_ECS_BENCH_CTEST=ON` registers `ecs_bench --quick`
with label `bench`, and `ecs_bench_callgrind` (labels `bench;callgrind`) where valgrind exists. ADR-004a's
indicator M1 needs a release build with asserts off, the `linux-bench` preset:

```
cmake --preset linux-bench && cmake --build --preset linux-bench
./build/linux-bench/bin/ecs_bench --no-spikes                  # RT-01 verdict and "ADR-004a M1"
./build/linux-bench/bin/ecs_bench --no-spikes --per-command-creates   # burst creates as spawn() + set()
./build/linux-bench/bin/ecs_bench --no-spikes --legacy-burst   # the pre-WP-1.1a burst, bugs included
cmake -DECS_BENCH=build/linux-bench/bin/ecs_bench -P engine/ecs/bench/callgrind_burst.cmake
```

`--m1-gate` makes the exit code the M1 verdict and `--rounds` prints every measured burst. Results are in
SPIKES.md §3 (Phase 0) and §5 (WP-1.1a).

## Known limitations

* ADR-004a's indicator M1 (World burst / raw-flecs burst, ≤ 1.6×) is met on the dev VM on the median
  of runs, without much margin: 1.54× with GCC and 1.59× with Clang (worst configuration and toggle
  storage per run, medians of 9 runs; single runs up to 1.73× and 1.80×; 1.24–1.30× in instructions).
  With per-command creates instead of `spawnN` it is 2.3–2.8×. The 9k-op burst takes 1.26–1.27 ms per
  configuration there (0.96–0.98 ms with Clang), against the 1.5 ms of RT-01's structural clause, whose
  formal run is on SERVER (ADR-004a M2). SPIKES.md §5 has the numbers and where the rest goes.
* Sparse and DontFragment ownership checks call `flecs_component_sparse_has`, a flecs 4.1.6 internal
  declared in `src/flecs_internal.h`; a flecs update must re-check it.
* Observed with flecs 4.1.6 on the raw C API: once `ecs_remove_id` of a DontFragment *tag* has run on
  an entity that lacked it, later removes of that tag from entities of the same table leave it in
  place. DontFragment components with a value (the zone's Status) are not affected. Prefer those until
  it is reported and fixed upstream (K10).
* Toggles move one entity at a time: flecs 4.1.6 has no public bulk move (SPIKES.md §5.6).
* No schema compiler yet: replicated components are hand-written with `_dirty` +
  `kReplicatedFields`; the runtime descriptor path is ready for reflection `TypeInfo`.
* `UpdatePolicy::ByUpdateLod` (Phase 2) and `EntityBudgetDef` caps are not implemented.
* Queries touching Sparse or DontFragment components iterate one entity per chunk (flecs needs
  per-entity field access for them); keep them out of hot iteration.
* Type slots are per module image: a hot-reloaded game DLL must bind its types again (or look
  components up by name via the runtime path).
* Fused spawn values and `spawnN` values bypass flecs OnSet observers; command-buffer spawns of the same signature are
  materialized at the group's first spawn, i.e. before later non-spawn commands of that buffer.
* `dockedAt()` with `DockStorage::Field` prunes its reverse index lazily (on dock and on host
  destruction); it must not run concurrently with structural changes.
* The orchestrator's AllocateIdBlocks client (the production `IdBlockSource`) does not exist yet;
  cells use `LocalIdBlockSource`, whose ids are unique across processes only if each shard's row
  value is persisted (`idLastPrefix`) and a shard is served by one source.
* Deleting a prefab silently strips `IsA` and its inherited components from live instances (flecs
  cleanup); no structural event is logged for that.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
WP-1.1a (ADR-004a option A, 2026-09-26) keeps that state: it changes no plan requirement.
