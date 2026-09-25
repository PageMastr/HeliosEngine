# engine/physics — Jolt 5.6 grids

L3, HEADLESS (02 §1.1). The physics runtime shared by the client, the cell and the editor: one Jolt
`PhysicsSystem` per grid (02 §5.4), stable body keys, deterministic queries, the `CharacterVirtual`
mover and the RT-03 determinism hash (02 §7.1, §8.2). Work package WP-0.9 (09 §2.1).

Jolt is vendored as `helios::tp::jolt` (`JPH_DOUBLE_PRECISION`, `JPH_CROSS_PLATFORM_DETERMINISTIC`,
no FMA) and linked **privately**: no Jolt type appears in `include/helios/physics/`.

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
- **Order by key, never by `BodyID`.** `createBodies()` adds a batch in `(layer, key)` order; `bodies()`,
  `stateHash()` and the query tie-breaks (equal fractions order by `(layer, key, subShape)`) never look
  at a `BodyID`. The closest-hit collectors keep the early-out one ulp above the best fraction, so a hit
  at exactly the same fraction still arrives and is tie-broken by key instead of by traversal order.
- **Worker count.** Jolt's deterministic mode plus the barrier-based adapter give the same state at any
  worker count (tested at 0, 1, 2, 3, 4, 16 and 32 workers, under unrelated load, and from inside a
  job).
- **Golden.** `physics_tests` pins the scripted scene (`tests/scene.cpp`: statics, a mesh, a box
  pyramid, spheres, capsules, convex hulls, a compound ship hull, debris, a projectile, a kinematic
  platform and a walking, jumping `CharacterVirtual`) after 600 steps: `0xff972a409e8145e1`. GCC 13.3
  and Clang 18 produce it bit for bit; MinGW builds the same test (not runnable in the Linux container);
  CI's `windows-msvc`, `windows-msvc-floor` and `windows-clang-cl` jobs are the cross-compiler check for
  MSVC and clang-cl. Never re-pin a golden to make one platform pass.

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
  BENCH-2's 1.5 km/s.
- **Catch-up bound.** `advance()` runs at most `GridDesc::maxStepsPerAdvance` (16) steps per call and
  drops the remaining whole steps with a warning, so a hitch cannot turn into a catch-up spiral (and a
  huge interval, where `accumulator -= dt` no longer changes the accumulator, cannot spin forever).
- **Characters stand along their up.** `createCharacter()` and `setCharacterUp()` turn the capsule so
  its local +Y (and the supporting volume under the feet) follows the up vector; before, a
  non-vertical up left the capsule lying on its side, a radius above the floor and unsupported.

## Not done yet (Phase 0 gaps and requests)

- **`third_party/jolt/patches/stable-order`** (02 §7.1) is not vendored: `third_party/` belongs to
  another owner. Stock Jolt 5.6 still orders contact constraints by a hash of `BodyID`s
  (`ContactConstraintManager` sort key and `SortContacts` tie-break), makes the lower `BodyID` "body 1"
  in `PhysicsSystem::ProcessBodyPair`, and orders `CharacterVirtual` contacts by `BodyID` /
  `CharacterID`. RT-03's permuted variant therefore passes only for islands with one contact
  constraint; `determinism: KNOWN DIVERGENCE ...` pins the multi-contact divergence and flips to an
  equality check once the patch lands. The patch reads `(object layer, mUserData)` in those three places
  and adds `Body::EFlags::NoCrossUpdateCache`; the keys it needs are already set by this module.
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
adapter (1–3 workers, under load, inside a job, 32 workers for 3,000 steps, two grids at once) and
the determinism goldens.

## Plan conformance

Plan-Rev: 6

Written to plan revision 6 (02 §5.4, §7.1; 09 §2.1 WP-0.9) on 2026-09-25,
ahead of its round (it needs WP-0.2r), under `docs/plan/09-roadmap-and-process.md` §5.10.2 D7. The
open deviations are listed in this README; the module still needs its row in 09 §8.1.
