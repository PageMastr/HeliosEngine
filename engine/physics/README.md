# engine/physics — Jolt 5.6 grids

L3, HEADLESS (02 §1.1). The physics runtime shared by the client, the cell and the editor: one Jolt
`PhysicsSystem` per grid (02 §5.4), stable body keys, deterministic queries, the `CharacterVirtual`
mover and the RT-03 determinism hash (02 §7.1, §8.2). Work package WP-0.9 (09 §2.1).

Jolt is vendored as `helios::tp::jolt` (`JPH_DOUBLE_PRECISION`, `JPH_CROSS_PLATFORM_DETERMINISTIC`,
no FMA, plus the Helios `stable-order` patch, `third_party/jolt/patches/0001-stable-order.patch`) and
linked **privately**: no Jolt type appears in `include/helios/physics/`.

## API

| Header | Contents |
|---|---|
| `runtime.h` | `PhysicsRuntime`: reference-counted process setup. Jolt's allocation hooks go to `helios::alignedAlloc` under the `physics.jolt` memory tag (mimalloc heaps, no global `operator new`), its trace hook to the `Physics` log channel, its assert hook (builds with `JPH_ENABLE_ASSERTS`) to the Helios assert handler, then the type factory is registered |
| `types.h` | `ObjectLayer` (Static, Terrain, Dynamic, Kinematic, Character, Vehicle, ShipHull, Debris, Projectile, Sensor, Interior), the five `BroadPhaseLayer`s, `CollisionMatrix` (defaults per 02 §7.1; later filled from `PhysicsLayersDef`), `BodyKey`, `BodyHandle`/`CharacterHandle` (generational), `BodyState` |
| `shape.h` | `ShapeRef` (ref-counted, shareable across grids) and box, sphere, capsule, cylinder, convex hull, triangle mesh and static compound constructors |
| `grid.h` | `PhysicsGrid`: `GridDesc` (host or bubble, capacities, gravity, fixed step, temp allocator, optional job system, collision matrix); bodies keyed by `(layer, key)`; kinematic moves; characters; `step()` / `advance()`; `castRay`, `castRayAll`, `castShape`; `stateHash()` |

Threading: a grid is driven by one thread at a time; `step()` fans out onto the Helios job system
through `jolt::HeliosJobSystem` (Jolt's `JobSystemWithBarrier`, High-priority jobs) and returns once
every job it queued has finished (it helps while waiting, so it may run other queued Helios jobs).
That drain keeps the Jolt job pool at one step's jobs: without it, wrappers of jobs a barrier had
already run piled up across steps on an oversubscribed pool and exhausted it. `PhysicsGrid::stats()`
reports the jobs in flight (0 between steps), the pool's peak and the temp allocator's heap
fallbacks; the job test asserts the first two over 3,000 steps at 32 workers. Different grids may
step concurrently. `PhysicsRuntime` construction and shape creation are thread-safe.

## Determinism rules (02 §7.1, RT-03)

- **Fixed step.** `step()` advances exactly `GridDesc::fixedDt` with `collisionSteps` collision steps;
  `advance(seconds)` accumulates real time and runs whole steps (at most `maxStepsPerAdvance`).
- **Stable body keys.** Every body carries its key (EntityId, packed TileKey or PCG instance key) in
  Jolt's `mUserData`. Key 0 is refused and `(layer, key)` is unique per grid (checked in every build).
- **Order by key, never by `BodyID`.** A `BodyID` is a slot index that follows the grid's add and remove
  history, so a predictor, a cell and a replay hold the same bodies under different `BodyID`s; no result
  may depend on one. `createBodies()` adds a batch in `(layer, key)` order; `bodies()`, `stateHash()` and
  the query tie-breaks (equal fractions order by `(layer, key, subShape)`) never look at a `BodyID`. The
  closest-hit collectors keep the early-out one ulp above the best fraction, so a hit at exactly the
  same fraction still arrives and is tie-broken by key instead of by traversal order.
- **Jolt's solver order is by key too** (the vendored `stable-order` patch, listed in
  `third_party/MANIFEST.md`). Stock Jolt 5.6 sorted contact constraints by a hash of `BodyID`s, made the
  lower `BodyID` "body 1" of a pair (in `ProcessBodyPair` and in each contact constraint) and ordered
  `CharacterVirtual`'s contacts by `BodyID`, so a body touching several others solved its contacts in a
  different order on each host. The patch orders all of these by `(object layer, user data)`, i.e. by
  `(layer, key)`; Jolt's caches stay keyed by `BodyID` (they are lookups, not orders).
- **No contact cache across steps for hulls and vehicles.** `createBody()` sets the patch's
  `Body::EFlags::NoCrossUpdateCache` on every ShipHull- and Vehicle-layer body: on the first collision
  step of each `step()`, their contacts recompute the manifold and start the impulses at zero (later
  collision steps of the same `step()` use the cache as usual). A client predicts these bodies and a
  rollback restores bodies, never the cell's contact cache, so a predicted step must be a function of the
  bodies' states. Resting hulls and vehicles lose only warm-start convergence (RT-03's slope-creep bound
  is Phase 1, WP-1.5).
- **Worker count.** Jolt's deterministic mode plus the barrier-based adapter give the same state at any
  worker count (tested at 0, 1, 2, 3, 4, 16 and 32 workers, under unrelated load, and from inside a
  job).
- **Goldens.** `physics_tests` pins the scripted scene (`tests/scene.cpp`: statics, a mesh, a box
  pyramid, spheres, capsules, convex hulls, a compound ship hull, debris, a projectile, a kinematic
  platform and a walking, jumping `CharacterVirtual`) after 600 steps at `0x1000fc8781dc8b58`, and the
  tile scene (36 Terrain tiles with a ship hull, a Vehicle-layer chassis, a box stack and the character
  each over the shared corner of four tiles) at `0x51afce82ae11d217`. The full scene was re-pinned once
  when the `stable-order` patch landed (stock Jolt gave `0xff972a409e8145e1`): the patch changes the
  contact sort order and the hull's warm start. GCC 13.3 and Clang 18 produce both bit for bit; MinGW
  builds the same test (not runnable in the Linux container); CI's `windows-msvc`, `windows-msvc-floor`
  and `windows-clang-cl` jobs are the cross-compiler check for MSVC and clang-cl. Never re-pin a golden
  to make one platform pass.
- **Permuted variant (RT-03).** Both scenes are rebuilt after 1,000 dummy add/remove cycles, one body at
  a time, so that every `BodyID` differs and every pair of bodies has the opposite `BodyID` order (or a
  shuffled one), and must give the same hashes every second and the character the same supporting tile
  every step. Reverting the patch locally fails both cases from the first second; reverting any one of
  its orders alone (the sort key, the body-1 choice, the pair order or the character's contact order)
  fails at least one of them, and disabling only `NoCrossUpdateCache` fails
  `stable-order: ShipHull and Vehicle bodies …`.

## Validation and limits

- **Input validation.** Everything that would reach Jolt as NaN, as an out-of-range limit or as a
  combination Jolt only asserts on in debug builds is refused with `InvalidArgument`: zero or
  non-finite rotations (a zero quaternion used to normalize to NaN and poison the whole grid),
  non-finite or negative mass, friction, restitution, damping and speed caps, mesh shapes (and
  compounds holding one) on moving bodies or as the swept shape of `castShape()` (Jolt has no mass
  properties or cast function for them), `maxBodies` above Jolt's 23-bit `BodyID` index, and
  non-finite `fixedDt` or more than 64 collision steps.
- **Speed cap.** `BodyDesc::maxLinearVelocity` defaults to 10 km/s. Jolt's own default, 500 m/s,
  silently clamped every body below 06's NAV mode (1 km/s), RT-19's 1 km/s closing ships and
  BENCH-2's 1.5 km/s. `createBody` clamps the initial `linearVelocity` to the cap and
  `angularVelocity` to Jolt's (0.25·π·60 rad/s) for dynamic and kinematic bodies, as `setVelocity`
  does. Jolt asserts that a new body starts within its caps. Before, release builds clamped dynamic
  bodies at the first step, before integrating, but never clamped kinematic bodies, which kept an
  over-cap initial velocity. While stepping, Jolt clamps only dynamic bodies; its one other clamp,
  on a kinematic body hit by a `LinearCast` (CCD) body, is unreachable while every Helios body uses
  discrete motion quality. `moveKinematic` sets velocities unclamped.
- **Catch-up bound.** `advance()` runs at most `GridDesc::maxStepsPerAdvance` (16) steps per call and
  drops the remaining whole steps with a warning, so a hitch cannot turn into a catch-up spiral (and a
  huge interval, where `accumulator -= dt` no longer changes the accumulator, cannot spin forever).
- **Characters stand along their up.** `createCharacter()` and `setCharacterUp()` turn the capsule so
  its local +Y (and the supporting volume under the feet) follows the up vector; before, a
  non-vertical up left the capsule lying on its side, a radius above the floor and unsupported.

## Not done yet (Phase 0 gaps and requests)

- **`CharacterVirtual` sweep ties.** Its stair-walk and floor-stick sweeps keep the first of several hits
  at exactly the same fraction, in broadphase traversal order, which depends on the grid's add/remove
  history; the patch does not change that. The permuted tile scene does not diverge on it, but a
  character stepping onto two coplanar tiles at once could; WP-1.5 (the shared mover, RT-03 in full)
  should add a case and, if needed, a key tie-break like the query collectors'.
- **ISA allowlist.** Jolt's headers select AVX2 paths inline, so every `helios_physics` TU is compiled
  with `tp_jolt`'s AVX2 flags. Until WP-0.2r moves to whole-image ISA levels, `lint_isa_audit` reports
  them unless `helios_physics` joins `HELIOS_ISA_AVX2_TARGETS` in `cmake/isa_allowlist.cmake`.
- Phase 1 and later: the multi-grid `PhysicsWorld`, bubble merge/split and grid transfer (with `world`),
  body tables and `CreateBodyWithID` restores for replay keyframes, the `SingleBodyPredictor`,
  `CharacterVirtual` inner bodies, vehicles (Phase 2), and RT-03's full scope (1,000 bodies,
  50 characters, 10 vehicles, 3,600 steps, AMD and Intel).

## Tests

`physics_tests` (doctest): runtime and memory accounting, shapes, grid and body lifecycle, validation,
capacity, fixed stepping, the collision matrix, queries and tie-breaks, the character mover, the job
adapter (1–3 workers, under load, inside a job, 32 workers for 3,000 steps, two grids at once), the
determinism goldens and permuted variants, and the `stable-order` patch's `NoCrossUpdateCache` rule
(`tests/test_stable_order.cpp`, with a control case that shows the check can fail).

## Plan conformance

Plan-Rev: 12

Written to plan revision 6 (02 §5.4, §7.1; 09 §2.1 WP-0.9) on 2026-09-25,
ahead of its round (it needs WP-0.2r), under `docs/plan/09-roadmap-and-process.md` §5.10.2 D7. Brought to
revision 12 on 2026-10-03 for the `stable-order` patch (02 §7.1, RT-03's permuted variant). One finding
against 02 §7.1: stock Jolt orders by `BodyID` in a fourth place the plan does not list, the body order
of each contact constraint (`ContactConstraintManager` stores and solves a pair lower `BodyID` first,
whatever `ProcessBodyPair` chose); the patch covers it. The open deviations are listed in this README.
