# 02 — Engine Runtime

> **Status:** draft v1. **Conforms to:** ADR-001, -002, -004, -005, -006, -011, -012, -013.
> **Scope:** everything that runs inside a Helios process, except rendering (section 03) and networking
> (section 04).
> **Normative ownership:** this section defines the **`.hschema` grammar** (§3), the container and entity file
> formats, and the game-UI runtime (`engine/ui`). Section 08 builds the HUD and screens on top of `engine/ui`.
> **Citations:** `R06-ENG-03` and `R04-P0-1` follow 01 §7; `R10 §6` means research report 10, section 6.
> Capability IDs, BENCH scenes, hardware tiers and `AAA-*` IDs are defined in 01.

## 0. Principles and scope

1. **One runtime, many hosts.** The client, editor, cell, bots and tools all link the same L1–L4 libraries.
   The cell is that runtime without graphics, audio or UI (R06-ENG-02).
2. **One declaration per type.** Every data type is declared once, in `*.hschema` (ADR-004).
3. **Handles, not pointers.** Subsystems are handle-based "servers", with no tracing GC (R06 §6.1).
4. **Determinism.** Physics, PCG, HXL and replays are bit-identical across compilers (AAA-PLT-4).
5. **Budgets and seams.** Every budget has a test (§8.2), and the scaling seams exist from day one
   (R04 §9.1).

**Capabilities (01 §2.6).** This section owns W01–W03 and W08. It supplies the runtime for W04–W06, M03,
G02, G15/R04, R05 and R06.

---

## 1. Modules, plugins and build targets

### 1.1 Layering DAG (R06-ENG-01)

Key: **HL** = HEADLESS (the cell links it); **ED** = `EDITOR_ONLY`; `→` = privately linked third-party
library.

| L | Module (`engine/…`) | Responsibility | HL | Deps | Owner |
|---|---|---|---|---|---|
| 1 | `core` | Platform, memory, jobs, threads, VFS, dynlib, handles, log, CVars, CPU gate, crash | ✓ | → mimalloc, Tracy, xxHash | 02 |
| 1 | `math` | Vectors, transforms, frame math, cube-sphere, `det::` (06's `hmath`), noise, fixed point | ✓ | std only | 02 |
| 2 | `reflect` | Type registry, attributes, property paths, diff/patch, codecs | ✓ | core, math → yyjson | 02 |
| 2 | `hxl` | HXL bytecode VM (06 §1.2) | ✓ | core, math, reflect | 02/06 |
| 2 | `asset` | Handles, registry, `.hpak` mounts, IO, residency hook, hot reload | ✓ | core, reflect → zstd | 02 |
| 2 | `records` | Record DB, client/server split, tag registry | ✓ | reflect, hxl, asset | 02 |
| 2 | `ecs` | flecs wrapper: IDs, relationships, scheduler, command buffers, dirty bits, prefabs | ✓ | core, reflect → flecs | 02 |
| 2 | `loc` | String tables, MessageFormat subset | ✓ | core, asset | 02 |
| 2 | `telemetry`; `patch`, `crash` | Metrics (04); streaming installer and sentry wrapper (08 §4.1) | ✓ | core | 04 / 08 |
| 2 | `app`, `input` | SDL3 windows, events, devices, IME; actions, contexts, rebinding, haptics | – | core, reflect → SDL3 | 02 |
| 3 | `physics` | Jolt grids, characters, vehicles, buoyancy, queries | ✓ | core, math, asset → Jolt | 02 |
| 3 | `anim` | ozz sampling, anim graphs, IK, retargeting, LOD, hitbox poses | ✓ | core, math, asset → ozz | 02 |
| 3 | `nav` | Recast/Detour per grid | ✓ | core, math, asset → Recast | 02 |
| 3 | `pcg` | Fixed-point `hnoise`, terrain-graph VM, stamps, scatter, asteroids | ✓ | core, math, reflect, asset | 02 |
| 3 | `script` | Luau VMs, sandbox, budgets, tasks, bindings, DAP adapter | ✓ | core, reflect, asset → Luau | 02 |
| 3 | `net` | Transport | ✓ | core, reflect → netcode | 04 |
| 3 | `audio` | miniaudio mixer, events, banks, buses, 3D | – | core, math, asset → miniaudio | 02 |
| 3 | `text`, `ui` | Font engine; RmlUi, view-models, `UiSurface`, draw lists | – | reflect, asset, input, loc, script → RmlUi, FreeType, HarfBuzz, SheenBidi, libunibreak | 02 |
| 3 | `rhi`, `render` | RHI; render graph; `RenderScene` packet type | – | core, asset, ui | 03 |
| 4 | `world` | ZoneInstance, frames, `Reparent`, portals, grids, containers, streaming | ✓ | ecs, records, physics, anim, nav, pcg, script | 02 |
| 4 | `gameplay` | Kernel and sci-fi systems | ✓ | world, hxl | 06 |
| 4 | `assembly` | Modular part/port rules and budgets shared by T21, the client builder and the cell (07 T21) | ✓ | gameplay, records | 06 / 07 |
| 4 | `replication`, `netgame`, `authority`; `clientcore` | 04 §11.1; 08 §4.1 | ✓ | world, net, telemetry | 04 / 08 |
| 4 | `presentation` | Extractors filling `RenderScene`; cameras, listener, UI surfaces | – | world, render, audio, ui, app | 02 (03 co-owns) |
| 4 | `assetpipe` | Importers, bakers, DDC, cooker, pak writer | ED | asset, records, pcg, physics, anim, nav → cgltf, ufbx, meshopt, bc7enc_rdo, basisu | 02 |
| 4 | `toolsfw`, `editorui`, `edtools/*` | ToolsFramework, ImGui shell, tools | ED | world, assetpipe | 07 |

**Rules.**
- **Declaration.** Every module uses `helios_module(name [HEADLESS|EDITOR_ONLY] LAYER n …)`.
- **Configure fails** if any of these hold:
  - a module depends upward, or on an unlisted same-layer module, or the graph has a cycle;
  - a HEADLESS module reaches `app`, `input`, `rhi`, `render`, `audio`, `text`, `ui` or `presentation`;
  - the client, cell or gateway links an `EDITOR_ONLY` module.
- **Build order and presets.** `HELIOS_MODULE_ORDER` gains the new modules. The `linux-headless` preset
  (servers and tools, `HELIOS_BUILD_GRAPHICS=OFF`) builds on every commit.
- **No third-party types in public headers.** This keeps the custom-ECS fallback possible (ADR-004).
- **AVX2 containment (08 §2.2).**
  - Jolt's ISA flags stay on `tp_jolt`, which only `physics` links (privately). All other code is x86-64-v2.
  - `physics` has no dynamic static initializers.
  - `core::cpuGate()` sits in a non-AVX2 unit that runs first.
  - CI audits the flags.

### 1.2 Plugins ("gems")

A gem is a directory, `gems/<name>/`, containing:
- a `gem.jsonc` manifest listing the gem's name, version, dependencies and modules (with layer and
  headless/editor-only flags);
- its schemas, content and Luau.

This follows O3DE's gem model (R06 §6.2).

- **Enabling.** `helios.project.jsonc` enables gems per target. The Foundation layer (01 §4.1) is a set of
  gems.
- **Linking.** Shipping builds link gems statically.
- **Hot reload.** The editor and dev client build the game module as a DLL that `core::DynamicLibrary`
  reloads. State lives in the ECS and records, so the world survives a reload (AAA-ITR-5).
- **DLL boundary.** Under `/MT`, the DLL gets its allocator, log and jobs through an `EngineServices` table and
  never frees memory across the boundary.
- **Native mods.** A stable C ABI arrives in Phase 5 (R06-ENG-32).

### 1.3 Build targets

| Target | Binary | Links | Notes |
|---|---|---|---|
| client | `helios-client` | Runtime + graphics, audio, UI, `presentation`, `clientcore`, netcode | No Slang, `assetpipe` or ImGui in shipping |
| editor | `helios-editor` | Client + `toolsfw`, `editorui`, `edtools`, `assetpipe`, Luau.Analysis | Loads Slang at runtime; reloads the game DLL |
| cellserver | `helios-cell` | HEADLESS L1–L4, headless gem modules, nats.c | `HELIOS_BUILD_GRAPHICS=OFF` |
| gateway | `helios-gateway` | `core`, `reflect`, `net`, `telemetry` | No ECS |
| launcher | `helios-launcher` | `core`, `app`, `ui` on SDL_Renderer, `patch`, `crash` | Built without AVX2 (08 §2); ships as the `Helios.exe` bootstrap + `HeliosLauncher.exe` (08 §2.1) |
| tools | `helios-{schemac, shaderc, assetd, cook, pack, fitsim, bot}` | §3.5, §6 | `schemac` builds first (`core` + yyjson); `bot` is a headless client (R06-ENG-27) |

---

## 2. Core runtime

### 2.1 Phase 0 deliverables (landed or in flight)

- **`engine/core`** headers:
  - `platform`, `types`, `assert`, `result`, `log`;
  - `hash`, `name`, `guid`, `random`, `utf`, `containers`;
  - `handle`, `memory`, `jobs`, `thread`, `time`;
  - `fs`, `vfs`, `dynlib`, `cvar`, `cmdline`, `crash`, `version`.

  OS code lives only in `src/platform/{win32,posix}`.
- **`engine/math`** (a std-only leaf):
  - `vec`, `quat`, `mat`, `transform`;
  - `frame`: `FrameId`, `FramePos`, `FrameTransform`, `reparent()`;
  - `spherical`: `CubeMapping::EquiAngular`;
  - `noise`: bit-identical f32/f64 noise;
  - `pack`, `color`, `geometry`;
  - `det::` trig.

  It is built with `-ffp-contract=off` and `/fp:precise`.
- **This plan extends that code and never renames it.** Where they differ, the code wins. Phase 0 still adds
  to `math`:
  - `det::exp`, `ln`, `pow` and `asinh` (HXL);
  - `Q16` (Q16.16) and `Q32` (Q32.32);
  - `Fixed64` (2⁻¹⁰ m, 04 §5.4);
  - integer `rsqrt` (§5.8).
- **The platform layer provides:**
  - UTF-8 long paths;
  - async reads (IORing and io_uring in Phase 3), memory maps and virtual reserve;
  - pinned threads and high-resolution tick pacing;
  - process spawn, file watching and CPUID.
- **SDL3 3.4.16** is confined to `engine/app`, on the main thread.
- **Every executable embeds a manifest** with the UTF-8 code page, PerMonitorV2 and longPathAware (ADR-011).

### 2.2 Memory (R06-ENG-16)

- **Landed (`core/memory.h`):**
  - `MemoryTag` and `registerMemoryTag(name, budget)`;
  - mimalloc-backed `alignedAlloc(size, align, tag)`;
  - `memoryTagStats`;
  - `Linear`, `Frame` and `Pool` allocators, and `VirtualMemory`.
- **Phase 1 additions:**
  - a tag per module;
  - soft and hard budgets per tag and per ZoneInstance: soft triggers telemetry, eviction and paused spawners;
    hard refuses new authority groups (04 §8, AAA-CNT-4);
  - routing of Luau (`lua_Alloc` per VM, capped), Jolt, flecs, Recast, miniaudio, RmlUi and ImGui allocations
    through tags.
- **No global mimalloc override** (ADR-011). A Phase 0 spike checks heap thread affinity under job migration.
- **Frame arenas** use `FrameAllocator` (the Naughty Dog pattern, R06 §4.7).
  - Clients keep 3 generations (sim N, extract N, render N−1), each freed on its GPU fence.
  - Cells reset per zone tick.
  - Arena pointers never outlive their frame.

CPU budgets in GB (GPU budgets are 03's):

| Tag group | Client MIN (7, AAA-CNT-3) | Client REF (12) | Cell 500-player (6, 04 §3.4) | Cell 50k-entity (16) |
|---|---|---|---|---|
| ECS / physics | 0.3 / 0.3 | 0.5 / 0.5 | 0.3 / 0.8 | 1.5 / 3.0 |
| Anim / nav / PCG | 0.3 / 0.1 / 0.4 | 0.5 / 0.2 / 0.8 | 0.2 / 0.5 / 0.5 | 0.5 / 1.0 / 1.0 |
| Script / audio / UI | 0.2 / 0.4 / 0.2 | 0.3 / 0.6 / 0.3 | 0.25 / — / — | 1.0 / — / — |
| Records, loc, static containers | 0.6 | 0.8 | 2.7 | 5.0 |
| Streaming + CPU asset copies | 1.0 | 2.5 | 0.2 | 1.0 |
| Net / render CPU / arenas | 0.1 / 1.0 / 0.2 | 0.1 / 2.0 / 0.4 | 0.2 / — / 0.05 | 1.0 / — / 0.2 |
| Headroom | 1.9 | 2.5 | 0.3 | 0.8 |

### 2.3 Jobs (R06-ENG-03, R05-P0-1, R01-P0-5)

- **Landed (`core/jobs.h`):**
  - `JobSystem::run(fn, Counter*, Priority)` with a helping `wait(counter)`;
  - three priority levels, each with work stealing;
  - `parallelFor`;
  - `TaskGraph` (a validated DAG);
  - `BackgroundPool` for IO, compiles and pathfinding.

  Overhead is O(100 ns) per job, guarded by a 1M-job test.
- **Worker counts.** The client runs cores − 2 workers, and its main and render threads help while waiting. A
  cell runs cores − IO threads. Both are overridable with the `jobs.workers` CVar.
- **Fibers** come in Phase 2+, behind the same API (ADR-011). Their rules apply now: no TLS or held mutex across
  `wait`, and `/GT`.
- **Other schedulers on this pool:** Jolt's `JobSystemWithBarrier` (§7.1), the ECS scheduler (§4.3) and Luau
  lanes (§7.4). flecs threads are unused.

### 2.4 Frame and tick pipelines

**Client on REF at 60 Hz** (Destiny and Bevy pipelining, R05-P0-2):

```
main    | SDL pump, input | fixed ticks ×k (predict, local grids) | update (interp, anim, Luau, UI) | EXTRACT N |
workers |  physics, anim, activation, collision tiles, UI layout, occlusion rays (jobs from every stage)   |
render  |  prepare + record + submit N−1 (03)                                                            |
GPU     |  N−2                                                                                          |
```

- **Ticks.** Command frames run at the zone rate (20–60 Hz, 04 §5.1), with ≤ 4 catch-up ticks per frame.
- **Extract.** This is the only sim→render handoff. The ECS is read-only during it; extractors write dirty
  deltas into `RenderScene` (§5.2), and the render thread never reads the ECS.
- **Main-thread budget (08 §1).** Ticks ≤ 3 ms, update ≤ 4 ms (client Luau ≤ 1 ms) and extract ≤ 1 ms, for
  **≤ 8 ms** in total. The render thread needs ≤ 4 ms wall (03 §2.5).

**Cell tick.** 04 §3.3 owns the tick stages and their budgets. The engine provides:
- the per-zone stage graph (§4.3);
- `ZoneHost`, which runs zone ticks earliest-deadline-first and feeds per-zone CPU into TiDi;
- between-tick work: streaming, container activation (≤ 2 ms per zone tick), tile generation, and
  checkpoint encoding from copy-on-write snapshots.

**Threading rules (normative).**
1. SDL runs on the main thread only.
2. Workers never block on I/O and never hold a lock for more than 50 µs.
3. Parallel ECS stages touch only the components they declare. Structural changes go through command
   buffers.
4. Jolt bodies change only through per-grid op queues, applied in EntityId order.
5. At most one lane is inside a given Luau VM at any time.
6. Published assets are immutable, and swaps happen only at frame or tick boundaries.
7. Third-party callbacks feed per-worker queues, which are sorted at sync points.
8. Every public API documents its threading rules.

### 2.5 Profiling, logging, CVars, crash handling

- **Tracy 0.14.1** (`HELIOS_PROFILE=ON`):
  - `HELIOS_PROFILE_ZONE` / `HELIOS_PROFILE_FRAME`, with one named frame per zone tick;
  - `TracyAllocN` per tag;
  - loopback only on dev cells.
- **Always-on counters** feed `telemetry` and the editor budget panel.
- **Logging** as landed, plus a JSON-lines server sink (05).
- **CVars** (landed `core/cvar.h`) are declared with
  `HELIOS_CVAR(f32, cvarLookahead, "stream.lookaheadSec", 20.0f, "…")`.
  - Flags: `Cheat`, `Saved`, `Replicated`, `ReadOnly`.
  - Layering: code → `config/engine.jsonc` → project → user → command line → live config (05 §1.15).
  - Changes apply at frame or tick boundaries.
- **Crash handling.**
  - Phase 0: `installCrashHandler` (core).
  - Phase 2: 08's `engine/crash` (sentry-native + crashpad, R10 §9), initialized right after the CPU gate.
    Reports attach the last 2,048 log lines, CVars, versions and the zone.
  - CI uploads symbols, and cells notify the orchestrator (AAA-STB-1).

---

## 3. Schema and reflection (normative)

### 3.1 The `.hschema` language

**Where schemas live.**
- Engine schemas live in `engine/<module>/schema/`. Game and gem schemas live in `schemas/<pkg>/`.
- Tags live in `.htags` files (06 §1.1).
- A package maps to a C++ namespace. `///` comments become `@doc`.

**Declaration kinds:** `enum`, `flags`, `struct`, `component`, `relation`, `record`, `event`, `rpc`,
`message`, `service`, `viewmodel`, `formula`, `const`, `alias`.

```
package game.ship;
import "helios/world/frames.hschema";

enum ShipSize : u8 { Small; Medium; Large; Capital }
struct ThrusterMount {
  bone:     Name
  dir:      vec3f        @normalized
  maxForce: f32 = 50000  @unit(N) @range(0, 1e8) @editor(category="Thrust")
}
component ShipMotion replicate(all) lod(core) {        // header sugar for @replicate(all) @lod(core)
  pos:    WorldPos @quant(frame_cell, cell=4096m, res=1/256m) @predicted
  rot:    quatf    @quant(smallest3, bits=10)          @predicted @interp(slerp)
  vel:    vec3f    @quant(range=4096, bits=16)         @predicted
  server { lastInputTick: u32 }                        // → ShipMotion::Server, never replicated
}
relation InFrame @exclusive @acyclic @target(helios.world.ReferenceFrame)

/// A hull type; instances live in content/records/hull/*.hrec.
record ShipHullDef @table("hull") {                    // also declares ShipHullRef
  name:      LocString
  size:      ShipSize
  mass:      f32 @unit(kg) @range(100, 1e9)
  thrusters: list<ThrusterMount> @keyed @max(64)       // elements carry stable "$key" GUIDs
  handling:  { pitchRate: f32 @unit(deg/s); yawRate: f32; rollRate: f32 }
  client { prefab: AssetRef<Prefab>; icon: AssetRef<Texture> }
  server { lootTable: LootTableRef?; aiHints: map<Name, f32> }
}
struct  HullDamage @store(checkpoint) @version(2) { hp: f32; breaches: list<u8> @was("holes") }
struct  Ammo @store(ledger) @ledger_policy(batched_consume) @lifecycle(decay=30d) { rounds: u32 }
event   ShipDestroyed @audience(relevant) { ship: EntityId; killer: EntityId? }
rpc     RequestDock(target: NetHandle, bay: u8) client->server reliable @ratelimit(2/s) @intent(interact);
service Ledger @scope(shard) {
  rpc Execute(tx: LedgerTx) -> LedgerResult @idempotent @timeout(500ms) @reason_required;
}
formula ThrustToWeight(ship) = attr(ship, MaxThrust) / (attr(ship, Mass) * 9.81);
viewmodel ShipHud @client { speed: f32; throttle: f32; target: TargetVm? }
```

```
file   := 'package' qname ';' {'import' string ';'} {decl}
decl   := kind Name [':' Base] {hattr} ('{' members '}' | params ['->' type] ';' | '=' hxl ';')
members:= { field | rpc | ('client'|'server'|'editor') '{' members '}' }
field  := ident ':' type ['=' literal] {'@' attr} [';']          // ';' optional at end of line
type   := prim | Name | Name '<' type {',' type} '>' | type '?' | type '[' int ']'
        | 'enum' '{'…'}' | 'variant' '{' Alt ['{' members '}'] {';' …} '}' | '{' members '}'
hattr  := ['@'] ident ['(' args ')'] | 'client->server' | 'server->client' | 'server->server'
```

**Header sugar.** The `@` is optional in declaration headers, so the snippets in 04, 05 and 06 parse as
written.

**Built-in types:**
- scalars: `bool`, `i8`–`u64`, `f32`, `f64`;
- text: `string`, `Name`, `LocString`;
- math: `vec2/3/4f`, `vec3d`, `quatf/d`, `color`;
- world: `WorldPos` (§5.2), `EntityId`, `NetHandle`, `Duration`, `Tick`;
- references: `Guid`, `AssetRef<T>`, `Ref<T>`;
- gameplay: `TagSet`, `TagQuery`, `HxlExpr`;
- containers: `list`, `map`, `set`, `T?`, `T[N]`, `variant`.

### 3.2 Attributes

| Group | Attributes | Effect |
|---|---|---|
| Replication | `@replicate(all\|owner\|server\|none)`, `@lod`, `@quant`, `@rate`, `@priority`, `@predicted`, `@interp` | Generates `ComponentRepDesc` (04). `@predicted` fields join the rollback snapshot |
| Messages | direction, `@reliable`, `@ratelimit(n/s)`, `@intent`, `@audience`, `@idempotent`, `@timeout`, `@scope` | Lint (AAA-SEC-1): every client→server `rpc` needs `@ratelimit` + `@intent` |
| Persistence | `@persist`, `@store(checkpoint\|ledger\|character\|activity\|config)`, `@table`, `@key`, `@sql(…)`, `@lifecycle(despawn, decay, retain)`, `@ledger_policy(…)`, `@reason_required` | Checkpoint codec, SQL stubs, lifecycle jobs (05). Lint: `@store(ledger)` data is never `@persist` write-behind (ADR-008) |
| Split | `client {}`, `server {}`, `@server_only`, `@client_only`, `@authoring`, `@opaque` | §3.3 |
| ECS | `@tag`, `@shared`, `@sparse`, `@singleton`, `@exclusive`, `@acyclic`, `@target` | flecs traits (§4.1) |
| Editor | `@doc`, `@editor(category, widget, order)`, `@range`, `@step`, `@unit`, `@asset`, `@hidden`, `@readonly`, `@validate(hxl)`, `@keyed[(field)]` | Inspector, T08 grid, T28 validation. `@keyed` elements get stable `"$key"` GUIDs, so merges and overrides do not depend on list index (07) |
| Script / versioning | `@script(read\|write\|none)`; `@was`, `@version`, `@merge(append)` | §7.4; §3.4 |

### 3.3 Records, client/server split, tags, formulas

**Records (G02, R04-P0-9, R05-P0-7).**
- **Files and IDs.** Each record is one `.hrec` JSONC file. Its `$rid` is a 63-bit RecordId, **minted once**
  from secure random bits and stored in the file.
- **Stable references.** `$name` (for example `"hull/kestrel"`) can be renamed freely, because the ID never
  changes. Ledger items therefore never dangle, and T28 rejects reused IDs (07 §5.4).
- **Generated refs.** `record FooDef` also declares `FooRef`.
- **Inheritance.** `$parent` names a record template, resolved at cook time. Fields override, lists replace
  unless marked `@merge(append)`, and keyed lists merge by key.
- **Reason codes.** `ReasonCodeDef` (06 §4) is an engine record type with generated C++ and Go constants. RPCs
  marked `@reason_required` must carry a `ReasonCodeRef`.

**Client/server split (R02-P0-6).**
- **Records** cook to `records.client.hrdb` and `records.server.hrdb`.
- **Components** split into `X`, `X::Server` and `X::Client`. `::Server` exists only in cell and editor
  worlds and is never replicated.
- **Lint (AAA-SEC-4, R08-ED-P0-01).** No server-only field or type may reach a client cook. A shared field
  may reference a server-only record only when it is `@opaque`.

**Tags and formulas.**
- **Tags** compile to dense u16 `TagIndex` values (06 §1.1).
- **Formulas** (`formula`, `HxlExpr`) compile to type-checked HXL bytecode. The C++ and Go VMs share a golden
  corpus.

### 3.4 Versioning and field redirects

- **Stable IDs.** schemac assigns stable u32 type and field IDs, recorded in the committed, append-only
  `schemas/schema.lock.jsonc`. `--check-lock` fails CI on reuse. Deleted fields become tombstones.
- **Renames.** `@was("old")` keeps the ID. Readers accept the old key and the writer emits the new one.
- **Type changes.** Widenings keep the ID: i32→i64, f32→f64, T→T?, appended enum values. Anything else needs a
  new field. Structural migrations use `@version(n)` plus a C++ `upgrade<T>` hook.
- **Per format.**
  - **Text** and **tagged** readers tolerate unknown and missing fields, which enables N↔N+1 handoffs
    (04 §7).
  - **Cooked** data recooks, because the layout hash is part of the DDC key.
  - **Network** uses the protocol hash.

### 3.5 `helios-schemac`

| `--emit` | Output | Consumer |
|---|---|---|
| `cpp` | Structs, `TypeInfo`, JSONC/tagged/cooked codecs, protobuf-wire codecs and NATS request stubs for `message`/`service`, ECS registration, `Mut<T>`, `Cooked<T>` | C++ |
| `repl` | `ComponentRepDesc`, quantizers, RPC/event tables, protocol hash | 04 |
| `luau` | Tagged-userdata glue (128 tags) + `.d.luau` | Script host, luau-lsp |
| `proto` | `.proto` → `buf generate` → Go structs, Connect and `protoc-gen-helios-nats` bindings | 05 |
| `go` | Record structs and DB reader, JSONC readers, **validators** (ranges, units, `@validate` via Go HXL), reason-code constants | 05, 06, 07 |
| `sql` | goose migration stubs, diffed against the lock | 05 |
| `editor` | `TypeInfo` attributes + `schema.editor.json` (categories, widgets, units, docs, visibility badges) | 07 |
| `records` | JSONC → client and server `.hrdb`, HXL, tags, loc keys | Cook |
| `lint` | SEC-1/SEC-4, ledger/persist, keyed lists, naming, size budgets | CI |

- **Incremental builds.** `helios_schema(TARGET … PACKAGE … FILES …)` rebuilds incrementally and takes ≤ 1 s for
  2,000 types.
- **Committed Go.** Generated Go is committed and CI-checked (05 §2.1).
- **Tagged binary.** It is protobuf wire format with lock IDs as field numbers, so no libprotobuf is needed
  (ADR-013).

### 3.6 Runtime reflection API

```cpp
namespace helios::reflect {
struct FieldInfo { std::string_view name; u32 id, offset; const TypeInfo* type; FieldFlags flags;
                   Audience audience; AttrSpan attrs; };
struct TypeInfo  { std::string_view qualifiedName; TypeId id; u32 size, align; Kind kind; u64 layoutHash;
                   std::span<const FieldInfo> fields; TypeOps ops; AttrSpan attrs;
                   template<class A> const A* attr() const; };
const TypeInfo* find(TypeId id);  const TypeInfo* find(std::string_view qualifiedName);
// 07's path syntax: "Transform/position", "entries[#b21c]/weight", "baseAttrs[Ship.MaxLinearSpeed]"
Result<Ref> resolve(const TypeInfo&, void* object, const PropertyPath& path);
void diff(const TypeInfo&, const void* before, const void* after, PatchWriter& out);  // undo, prefabs, T30
Result<void> apply(const TypeInfo&, void* object, const Patch& patch);
Result<void> readJsonc(const TypeInfo&, void* object, JsonView in, ReadCtx& ctx);
void writeJsonc(const TypeInfo&, const void* object, JsonWriter& out);              // canonical (§3.7)
// + readTagged / writeTagged. Immutable after startup; reload swaps it at a safe point; lock-free lookups.
}
```

The registry feeds the inspector, undo, prefab overrides, bindings, replication and diffing (R06-ENG-05). It
is schema-first rather than built on a libclang header tool (R10 §6). `HELIOS_REFLECT` covers types internal
to the engine.

### 3.7 Serialization formats

| Format | Encoding | Used for |
|---|---|---|
| **Text** | Canonical JSONC (yyjson) | Records, entities, containers, prefabs, `.meta`, config |
| **Cooked** | Relocatable little-endian, memory-mapped, zero-copy | Record DB, containers, assets |
| **Tagged** | Protobuf wire, lock field IDs | Checkpoints, handoff blobs, journals, hot-reload messages, Go |
| **Network** | Bit-packed, quantized from `@quant` (04 §4) | Replication, RPCs |

**Canonical JSONC (R08-ED-P0-02, R01-P1-16).**
- **Order:** `$`-keys first, then schema order.
- **Layout:** one property per line; LF line endings; UTF-8.
- **Values:** shortest round-trip floats; defaults and inherited values omitted.
- **References:** `"guid:…"`, `"ent:…"`, `"loc:…"`, or a RecordId.
- **Comments:** notes that must survive editing go in `"$comment"`.

**Cooked layout.**
- Header: `{magic, formatVersion, rootTypeId, layoutHash, size}`.
- Self-relative `RelPtr<T>`/`RelSpan<T>`, 16-byte aligned.
- Loaders validate bounds and are fuzzed.

---

## 4. ECS on flecs (ADR-004, R06-ENG-04/06)

### 4.1 Registration and identity

**Registration.** schemac emits `registerComponents(ecs::World&)` (size, alignment, hooks), mapping attributes
to flecs traits:
- `@shared` → `(OnInstantiate, Inherit)`;
- `@sparse` → sparse storage;
- `@tag` → zero-size;
- `@singleton` → world entity.

Dev builds emit flecs meta for the explorer. Reflection always uses `TypeInfo`.

**Identity.**

| ID | Width | Scope | Source |
|---|---|---|---|
| `EntityId` | u64 | Global, persistent | `0…` runtime spawns: Snowflake 41/5/8/9 (05 §3). `10…` content-placed: `hash62(entityGuid)`; the GUID is minted once in the `.hent`, so moving an entity between containers keeps its ID; the cook rejects collisions. `11…` client-local |
| `NetHandle` | u32 | Zone instance | 24-bit index + 8-bit generation (04 §4.6); content-placed entities use their container's index table |
| `flecs::entity` | u64 | Process | Never serialized |

`EntityRegistry` maps `EntityId`↔flecs and keeps a dense `NetHandle` table. Each entity carries
`NetIdentity{EntityId, NetHandle, AgId}`. Scripts revalidate handles after every yield (06 §11).

### 4.2 Relationships

| Pair | Meaning | Traits |
|---|---|---|
| `(ChildOf, p)` | Transform hierarchy within one frame and authority group; the scene tree is a view of it | acyclic, cascade |
| `(InFrame, f)` | Frame of the entity's `WorldPos` (§5.1) | exclusive |
| `(DockedTo, h)` | Docking, landing | exclusive |
| `(IsA, prefab)` | Shares `@shared` components | built-in |

**Fragmentation guard.** Each distinct pair target creates a flecs table.
- If `ChildOf` or `DockedTo` breaches RT-01's cap of 5,000 tables, that relation moves to non-fragmenting
  storage (`DontFragment`; verify it in flecs 4.1.6).
- High-cardinality links, such as sockets and targets, are plain `EntityId` fields.

### 4.3 Scheduling, command buffers, update policies

```cpp
namespace helios::ecs {
struct SystemDesc {
  const char* name; Stage stage;                   // Input, PrePhysics, Physics, PostPhysics, …
  Access access;                                   // reads<…>(), writes<…>() → per-stage DAG (TaskGraph)
  QueryDesc query; u32 chunkGrain = 256;           // split into jobs by table range
  UpdatePolicy policy = UpdatePolicy::EveryTick;   // EveryNTicks(n, staggered) | ByUpdateLod
  f32 budgetUs = 0;                                // reported; gated in CI
  void (*run)(SystemContext&, QueryIter&);
};
class CommandBuffer {                              // one per job; applied in (stage, system, job, seq) order
public:
  TempEntity create(PrefabId prefab = {});  void destroy(Entity e);
  template<class T> void set(Entity e, const T& v);  template<class T> void remove(Entity e);
  void reparent(Entity e, FrameId frame);          // applied at the sync point (§5.3)
};
}
```

- **Scheduling.** Systems with non-conflicting access run in parallel (Bevy/Mass). flecs is read-only during
  parallel stages.
- **Deterministic structure.** Structural changes apply in a fixed order, so creation order never depends on
  threads. 04's bit-exact replay relies on this.
- **Update LOD (R04-P0-6).** `UpdateLod{Full|Reduced|Frozen}` is recomputed at 1 Hz (06 §7 tiers). Systems
  using `ByUpdateLod` run Reduced entities every 4th tick and skip Frozen ones.
- **Entity budgets (R04-P1-16).** `EntityBudgetDef` caps entities per zone, container, player and construct.

### 4.4 Change tracking for replication (Iris-style push)

- **Dirty bits in the component.** schemac adds a hidden `_dirty` field mask to each replicated component
  (ADR-004).
- **Precise writes.** Generated `Mut<C>` per-field setters (`m.set_pos(p)`) set the field bit and the entity's
  `RepDirty{u64 componentMask; Tick changed}` summary, which is finer than flecs change detection (04 §4.2).
- **Raw writes.** `Mut<C>::raw()` marks all fields dirty. 04's quantized comparison then filters out noise.
- **Structural log.** Creates, destroys, adds, removes and reparents go through observers into a per-tick log
  consumed by 04 and `presentation`.

### 4.5 Prefabs, authoring vs runtime, baking

- **Prefabs (R08-ED-P0-05).** A `.hprefab` holds entities with local IDs minted once.
  - An instance stores `$prefab`, `$overrides` as property-path patches
    (`"Thrusters/thrusters[#9a1e]/maxForce": 1.5e5`), and `$added`/`$removed`.
  - Nesting flattens at cook.
  - Instance `EntityId = hash(instanceGuid, prefabLocalId)`.
- **Runtime sharing.** Each loaded prefab is a flecs prefab entity holding `@shared` components, and instances
  inherit them via `IsA`. BENCH-5's 3,000 decor items therefore store only transforms and overrides.
- **Authoring vs runtime (R06-ENG-21).** `@authoring` components exist only in source and editor worlds.
  `Baker`s convert them at cook, and the editor re-runs them after every transaction (live baking).

---

## 5. World model (ADR-005, W01–W06, W08)

### 5.1 Reference frames and portal graphs

`world` owns the frame graph and builds on `math/frame.h` (`FrameId`, `FrameTransform` =
position/rotation/velocities relative to the parent, `KinematicState`, `composeFrames`, `relativeFrame`,
`reparent`, `rotatingBodyFrame`).

```cpp
namespace helios::world {
enum class FrameKind : u8 { Galaxy, System, Body, Grid, Interior };
struct FrameMotion { std::variant<Static, Orbit /*Kepler*/, Spin /*axis, ω, epoch*/, Kinematic> m; };
class FrameGraph {                                                        // one per ZoneInstance
public:
  FrameId create(const FrameDesc& d);  void destroy(FrameId f);
  FrameTransform inParent(FrameId f, ZoneTime t) const;                  // deterministic in t (det::)
  FrameTransform relative(FrameId from, FrameId to, ZoneTime t) const;   // via lowest common ancestor
  void setKinematic(FrameId f, const FrameTransform& x);                 // physics sync only
};
}
```

**Frame hierarchy (R04-P0-1).**
- The order is galaxy → system → body (rotating) → grid → interior.
- Frames are entities, and `(InFrame, f)` places everything else.
- Bodies spin, and may also orbit on Kepler rails. Harrow only spins (R04 §2.2).
- f64 covers a single system (R04 §2.1), so the galaxy frame places stars as
  `{i64×3 sector of 2⁴⁰ m, f64 offset}`.

**Portal-graph contract (W08, R02-P0-8).**
- Each interior container cooks a `PortalGraph`.
- It is used by 03 (culling, sky visibility, probe blend, exposure), audio, streaming, nav and 06's pressure
  gameplay.

| Element | Contents |
|---|---|
| Cell | `{id, local AABB + convex hull, flags (exterior-visible, pressurized), probeZone, exposureZone}` |
| Portal | `{id, cellA, cellB \| Exterior, convex polygon ≤ 8 verts, flags (window, airlock)}` |
| State | `PortalState{portalId, open, opacity}` components, replicated and driven by door entities |
| API | `cellAt(localPos)`, `visibleCells(viewCell, frustum, maxDepth = 8)`, `propagate(cell, fn)` |

### 5.2 `WorldPos`, precision, and the camera-relative contract

- **`WorldPos`.** The component `WorldPos{DVec3 local}` plus its `InFrame` target is math's `FramePos`.
  Rotations and local offsets are f32 (R06-ENG-07).
- **Precision.** Cross-frame math goes through the lowest common ancestor.
  - Grid-local content, such as a cockpit at 10¹³ m, is exact.
  - Free objects in a system frame get f64 steps of 15 µm at 10¹¹ m and 1.95 mm at 10¹³ m.
- **Two-level transforms (03 §2.6).** Extract never sends per-object camera-relative matrices. It writes two
  things:
  1. **Dirty** f32 instance transforms relative to a *render frame*: a grid, station, interior or planet tile.
     Entities placed directly in a System, Body or Galaxy frame use a **render cell** instead: the 4,096 m
     cell of that frame, the same cell as 04 §4.5's `frame_cell`.
  2. For each view, one f64 `frameToView[F]` per visible render frame or cell.

  03's prepare step converts each `frameToView[F]` to a `GpuFrameXform`, and the GPU composes the two. A ship
  at 1,500 m/s therefore updates one record, not every object on board.
- **Luau** gets `WorldPos` as f64 userdata, never as a float `vector` (06 §11).

### 5.3 `Reparent` preserving world position and velocity

The math is `helios::reparent()` (transport theorem). With A and B given relative to a common ancestor C as
`(R, o, V, Ω)`:

```
x_C = R_A·x + o_A        x_B = R_Bᵀ·(x_C − o_B)        q_B = q_Bframe⁻¹ · q_Aframe · q
v_B = R_Bᵀ·(R_A·v + V_A + Ω_A×(R_A·x) − V_B − Ω_B×(x_C − o_B))        w_B = R_Bᵀ·(R_A·w + Ω_A − Ω_B)
```

- **Execution.** It runs in f64 at the sync point, in EntityId order. It swaps `InFrame` and moves physics
  state (§5.4). `ChildOf` children keep their local transforms.
- **Event.** It emits `FrameChanged{entity, oldFrame, newFrame, oldToNew}`, which is consumed by:
  - 04, which replicates it atomically with the new transform;
  - `presentation`, which forwards it (and render-cell changes) in `RenderScene`, so 03 re-expresses previous
    transforms and TAA does not smear;
  - audio and streaming sources.
- **Example (BENCH-2).** Atmosphere entry reparents the ship into the rotating body frame, so
  `v_rot = v_inertial − ω×r`.

### 5.4 Physics grids (R04-P0-3, R06-ENG-23)

A **grid** is a frame that owns one Jolt `PhysicsSystem`:

| Kind | Examples | Origin | Gravity |
|---|---|---|---|
| **Host** | Ship or station interior, cargo bay | Hull pose, set as `Kinematic` after the parent grid steps | Uniform artificial gravity; host acceleration is ignored or scaled by `GridDef.inertialFactor` |
| **Bubble** | Open space; planet surface (body frame) | Fixed in the parent frame; re-centred when the body centroid drifts past R/2 | None in space; radial on a surface, applied per body before each step |

- **Bubbles.**
  - R = 20 km, so Jolt's float broadphase stays at ≤ 2 mm precision (R06 §6.7).
  - A body more than 1.25R from every bubble opens a new one. Bubbles closer than 0.75R merge.
  - Bubble `PhysicsSystem`s are pooled.
- **Ships.** The hull is one compound body in the parent grid. The interior is a child grid, so passengers
  never feel hull corrections (04 §5.3). Grids can nest, e.g. a ship in a hangar.
- **Transfers.**
  - Only authored `GridVolume`s trigger a transfer: enter 0.5 m inside, exit 1.0 m outside, 0.5 s minimum
    dwell. Servers flag any other transfer (04 §5.5).
  - At the sync point: read the state, remove the body, convert (§5.3), re-add it with the shared `Shape`.
    `CharacterVirtual`s are recreated.
  - Budget: ≤ 1 tick and < 1 mm error (BENCH-6).
- **Stepping.** Grids step in parallel. Kinematic frames update next, then transfers apply.
- **Multi-grid queries.** With `MultiGrid`, `castRay` and `castShape` continue into the parent grid when they
  leave the grid volume, excluding the host hull.

### 5.5 Zones and cells as the engine sees them

```cpp
class ZoneInstance {                    // 04 owns ZoneClock, the AG table and handoff; 02 owns the rest
public:
  ecs::World& ecs();  FrameGraph& frames();  PhysicsWorld& physics();  StreamingManager& streaming();
  script::Vm& vm();   const records::Db& records() const;  pcg::Universe& pcg();  nav::NavWorld& nav();
  const RegionSet& ownedRegions() const;   // v0: whole zone; v1: this cell's region (04 §6.5)
  void tick(JobSystem& jobs);
};
```

- **Pinning.** Each ZoneInstance pins one content version (04 §7).
- **Region filter.** `ownedRegions` filters streaming, spawners and authority.
- **Transitions.** During a zone transition the client keeps a second view that prefetches the destination.
- **Phase 1 layout.** Tallis space and the Harrow surface form one zone, and the station interior is a
  container. BENCH-2 is therefore seamless without a handoff.

### 5.6 Object containers (R04-P0-5, R06-ENG-20, W03)

A container is the unit of editing, streaming, persistence overlay and hot reload. Its source files follow
OFPA: `saltmarch.hcont` plus one `saltmarch.entities/<guid>.hent` per entity.

```jsonc
// saltmarch.hcont
{ "$container": "guid:5e1d…", "name": "saltmarch",
  "frame": { "parent": "body:harrow", "anchor": { "lat": 12.4, "lon": -33.1, "alt": 0 }, "heading": 90 },
  "bounds": { "center": [0, 0, 0], "radius": 1800 },
  "streaming": { "loadRadius": 6000, "hlodRadius": 40000, "group": "zone.tallis", "server": "bubble" },
  "children": ["guid:a41c…"], "layers": ["base", "event.drone_raid"], "budgets": { "entities": 4000 } }
// saltmarch.entities/7b2d….hent
{ "$entity": "7b2d4c1e-…", "$prefab": "guid:c0ffee…", "$overrides": { "Light/intensity": 1200 },
  "Transform": { "pos": [12.5, 0.0, -40.25], "rot": [0, 0.7071, 0, 0.7071] },
  "Spawner": { "def": 7134501129930413057, "maxAlive": 6 } }
```

**The cooked `.hcc`** is relocatable and has client and server variants. It holds:
- a header;
- an entity table (EntityId, NetHandle index, prefab);
- **SoA component blocks per ECS archetype**, ready for flecs bulk insert;
- dependencies;
- Jolt static shapes, nav tiles and the `PortalGraph`;
- an HLOD reference (client variant only).

**Runtime state.**
- Runtime changes to content-placed entities persist as a delta overlay keyed by EntityId. Dynamic entities
  belong to authority groups (05 §1.13).
- Data layers toggle event and phase sets.
- Two HLOD levels, a 1/8-triangle merged mesh and an impostor, cover the range from `loadRadius` to
  `hlodRadius`.

### 5.7 Streaming

```cpp
struct StreamingSource { FrameId frame; DVec3 pos; Vec3 vel; f32 lookaheadSec; f32 radiusScale; u8 priority; };
```

**Sources.**
- **Client:** the camera, the player's avatar or ship (with a 20 s velocity lookahead), and warp paths
  (R04-P1-15).
- **Server:** player interest bubbles (04 §4.3), AI sites and pinned containers.

**Priority.** `p = w·saturate(1 − d/r_load) + 1/(1 + t_reach)`. A container is requested when
`t_reach < 2·estimatedLoad + 5 s`.

**States:**

```
Unloaded → Requested → [Fetching] → Reading → Decoding → Resident → Activating (time-sliced) → Active
```

Unloading runs the chain in reverse, down to `Evicted`, with LRU eviction under budget.

**Residency hook (08 §2.6).** `asset` defines `IResidencyProvider`, and 08's `StreamingInstaller` implements
it with `resident(pak, range)`, `demand(pak, range, priority)` and `prioritizeGroup(group, priority)`.
- A miss puts the load into `Fetching`. The HLOD or placeholder shows meanwhile, or the spawn waits.
- Travel destinations call `prioritizeGroup`.
- Pak mounts implement `core::IMountProvider` plus async ranged reads.

**Budgets.**
- I/O ≤ 150 MB/s sustained. The game thread never blocks on I/O.
- Activation takes ≤ 1 ms of client main-thread time per frame and ≤ 2 ms of cell time per tick.
- Unload radius is 1.25× the load radius, with a 10 s minimum residency.
- A container activates only after its parent and its hard dependencies.
- Records are memory-mapped and load in ≤ 2 s for 100k records (AAA-CNT-5).

**BENCH-2 check.** At 1,500 m/s the 20 s lookahead is a 30 km corridor. The I/O budget can deliver 3 GB over
that window.

### 5.8 Deterministic PCG runtime (W04, R02-P0-5, R04-P1-12, R06-ENG-25)

`engine/pcg` is shared by the client, cell and editor. 03 generates **every visual terrain
LOD on the GPU**, and GPU float math is not bit-exact across vendors. Everything that feeds heights is
therefore **fixed-point `hnoise`** (03 §5.5):

| Element | Definition |
|---|---|
| Lattice hashes | PCG32 / xxHash32 |
| Domain | Q2.30 face coordinates; fixed-point polynomial `EquiAngular` warp (03 §5.4); integer `rsqrt` |
| Noise | Integer lattice cell + Q16.16 fraction, with an integer quintic fade |
| Heights | Q32.32 metres. The f64 conversion is exact for \|h\| < 2²⁰ m |
| Twin | Each node op exists as a C++ VM op and a Slang function (`hnoise.slang`, co-owned with 03); a per-node corpus hashes both |
| Floats | Only in `visualOnly` nodes (< 5 cm). Float `helios::noise` stays for CPU-only generation |

**Terrain graph.** A body's terrain is a typed node DAG (`.hpcg`). 07's layer stack is one view of it. This is
the generic node-graph formulation that ADR-006's patent note calls for.

| Family | Nodes |
|---|---|
| Regions | Spherical cap, geodesic polygon, polyline-with-width, tangent rectangle; feathered; union = max |
| Filters | Height, slope, aspect, curvature, latitude, noise, ecosystem map, mask; intersection = min |
| Generators | fBm, ridged, billow, domain warp, cellular, terrace, craters, erosion approximation |
| Affectors | Height, material/biome, colour, scatter, exclusion, passable, road/river carve, environment |
| Authored deltas | 07's sparse sculpt/paint delta tiles, keyed by `TileKey`, stored in fixed point |

**Evaluation.**
- **Bytecode.** Graphs compile to register bytecode that evaluates whole tiles SoA: ≤ 0.5 ms per 65×65 tile per
  core for a 40-node graph.
- **Tiles.** Tiles are `TileKey{face, level, x, y}`, and the same tiles serve rendering and collision
  (03 §5.4). Radius goes up to 6,400 km (AAA-CNT-1).
- **Collision.** The CPU builds collision `MeshShape`s only within 2 km of bodies in surface bubbles, ≤ 1 ms per
  tile on the Background pool. If a GPU vendor fails the conformance test, the client uploads these CPU tiles
  instead.
- **Contract.** Visual vs collision ≤ 5 cm. 03's CI holds it to ≤ 1 cm.

**Runtime inputs.**
- **Stamps.** `TerrainModification{id, Flatten|Crater|Road, geodesic footprint, params, version}`.
  - Stamps are persisted, replicated and applied last, in id order (06 §9).
  - A stamp invalidates the tiles under it, and their collision and nav.
- **Scatter.** Seeded per `hash(bodySeed, ruleId, tileKey@ruleLevel)`, with graph-mask density and Poisson
  rejection in hash order.
  - Visual-only scatter is generated on the GPU (03 §5.6).
  - Collidable and gameplay instances come from this library as entities.
- **Asteroid fields** (Scree belt).
  - A field is a volume with a density function, a size distribution and a composition.
  - Gameplay cells are 2 km, and each asteroid's ID is `hash(fieldId, cell, index)`.
  - Minable asteroids spawn as entities near ships. Mined asteroids are saved as a persisted delta set.
- **Systems (W06).** `StarSystemDef` defines stars, bodies (orbit, spin, radius, atmosphere, terrain graph,
  `WaterBodyDef`) and root containers.

---

## 6. Asset pipeline (R06-ENG-11/12/13, R08-ED-P0-08/16)

### 6.1 Identity and registry

- **Sidecars.** Each source file has a `.meta` sidecar holding `{guid, importer, importerVersion, settings,
  labels, provenance}`. Provenance is required (01 §5.2).
- **IDs and handles.**
  - `AssetId` is a u64 fold of the GUID; packaging rejects collisions.
  - `AssetHandle<T>` (core `Handle`) is a loaded instance, and swaps happen by handle.
- **Registry.** `helios-assetd` owns it: metadata, labels, dependencies and reverse dependencies, stored as an
  append-only tagged log. Cooks ship a `registry.hreg` that loads in ≤ 1 s for 200k assets (AAA-ITR-2).

### 6.2 Import → intermediate → cook

| Source | Importer | Intermediate (DDC only) | Cooked `pc-client` |
|---|---|---|---|
| glTF, FBX | cgltf, ufbx | `.imesh`, `.iskel`, `.ianim` | Meshlets and LODs (03), Jolt shapes, ozz clips |
| PNG, TGA, EXR | stb_image, tinyexr | `.itex` RGBA16F/8 + mips | BC7/BC5/BC4/BC6H (bc7enc_rdo, basisu) |
| WAV, FLAC | dr_wav, dr_flac | `.iaudio` f32 PCM | IMA-ADPCM (SFX); Opus for VO and music (libopus BSD-3, **manifest addition**) |
| `.slang` | `helios-shaderc` (03) | — | SPIR-V + reflection |
| Text sources | reflect, schemac, bakers | — | `.hcc`, `.hrdb`, prefabs, PCG bytecode |

- **Encoders.** Dev cooks use fast encoders, and release cooks use RDO.
- **Platforms.** `pc-client` (identical on Windows and Linux), `server` (§6.5) and `editor`.
- **DDC key.** `XXH3-128(builder, version, sourceHash, settingsHash, platform, layout hashes, dependency
  hashes)`.
- **DDC stores.**
  - **Local:** 100 GB, LRU.
  - **Shared:** an HTTP CAS (`GET/PUT /ddc/v1/<key>`), filled by CI.
  - Distribution uses BLAKE2b (05 §7).

### 6.3 Packaging (ADR-006, R01-P0-9)

```
HpakHeader { 'HPAK', version, platform, contentBuild u64, tags{tier, group, language}, tocOffset, tocSize }
Blobs      zstd per asset (large assets: 256 KiB blocks), 4 KiB aligned; pak ≤ 2 GiB
TOC        sorted { AssetId, cookedHash XXH3-128, offset, compSize, rawSize, codec, blocks }
           + XXH3-64 checksum per 64 KiB pak block
```

- **Build.** `assetpipe`'s writer, driven by 08's `helios-pack`.
- **Ordering.** Assets are ordered by tier → group (zone) → language → recorded first-use → GUID. This stable
  order keeps 05's FastCDC chunking efficient (AAA-CNT-7).
- **Patching.** Patch paks overlay by `AssetId` (the SWG TRE lesson, R02 §3.2), and duplicate assets are
  stored once.
- **Integrity.** The VFS verifies each block on its first read. A mismatch calls
  `StreamingInstaller::demand` to re-fetch it (08 §2.6).
- **PSO lists.** Each zone's group carries its per-zone PSO precache list in 03's format.

### 6.4 Asset processor and hot reload (R03-P0-5, R05-P0-10, AAA-ITR-1)

- **`helios-assetd`.**
  - Watches sources and runs a job DAG over source, job and product dependencies.
  - Priorities: editor request > hot reload > background.
  - Cooks on the fly over local HTTP for the editor, PIE clients and dev cells (AAA-ITR-3).
- **Protocol:** tagged messages over localhost TCP.
  - `AssetChanged{id, kind, cookedHash, ddcKey}`.
  - `RecordsDelta{contentVersion, changed[], removed[]}`.
  - `ScriptChanged`, `BuildFailed`.
  - Remote cells receive the same messages via `content.<scope>.published` (05).
- **Swap.** Handles swap at frame or tick boundaries. The old version is freed after its fence or once its
  references drain.
- **Latency budget (p95, DEV), save → visible ≤ 2 s:**

  | Step | Budget |
  |---|---|
  | Detect | ≤ 100 ms |
  | Build: records / Luau / textures / shaders / containers | ≤ 300 / 100 / 800 / 1,000 / 500 ms |
  | Notify | ≤ 10 ms |
  | Swap | ≤ 200 ms |

- **Schema layout changes** rebuild C++ and reload the game DLL (§1.2).

### 6.5 Server and client cooks

- **Server cooks keep:**
  - collision, hitbox capsules and sockets;
  - gameplay masks;
  - skeletons, plus the clips needed for hitboxes and root motion, at a reduced rate;
  - physical-material IDs;
  - shared and `server{}` records.
- **Server cooks drop** render data, audio, UI, HLOD, VFX and PSO lists.
- **Client cooks drop** `server{}` data.
- **CI** lints both directions (AAA-SEC-4).

---

## 7. Runtime subsystems

### 7.1 Physics (Jolt 5.6)

- **Build.** `JPH_DOUBLE_PRECISION` and `JPH_CROSS_PLATFORM_DETERMINISTIC` everywhere; AVX2 without FMA,
  contained as in §1.1; `Compute/` and `Shaders/` excluded (R10 §5).
- **Structure.** `PhysicsWorld` owns the grids. Each grid has a `PhysicsSystem`, a `TempAllocatorImpl` (8 MB
  on the client, 32 MB on a cell) and an op queue. `world` binds the body, collider, character, vehicle and
  `GridVolume` components. Shapes are shared through an `AssetId` cache.
- **Layers.** Object layers are Static, Terrain, Dynamic, Kinematic, Character, Vehicle, ShipHull, Debris,
  Projectile, Sensor and Interior. The broadphase has 5 layers. The collision matrix is a `PhysicsLayersDef`
  record.
- **Characters.** `CharacterVirtual` with a per-character up vector: radial on planets, grid gravity indoors.
  `ExtendedUpdate` handles stairs and floor-sticking. Client and cell share the mover (04 §5.2).
- **Vehicles.** Wheeled and tracked vehicles use `VehicleConstraint`, and hover vehicles use ray suspension.
  Ships apply 06's flight forces to the hull, with mass from records.
- **Buoyancy (Phase 3).** `WaterBodyDef` defines sea level plus **≤ 8 Gerstner waves**, evaluated with `det::`
  at zone time.
  - A body samples ≤ 16 hull points for submerged depth and drag.
  - Physics never reads 03's FFT ocean. 03 renders the Gerstner set as the ocean's large scale.
- **Determinism.**
  - Fixed dt, and bodies are added in EntityId order.
  - Contacts and query results are sorted (06 §11).
  - A CI hash runs across 4 compilers and 1, 4 and 16 workers.
  - `HeliosJoltJobSystem` (a `JobSystemWithBarrier`) runs Jolt work as High-priority jobs.
- **Netcode hooks.** `SingleBodyPredictor` supports hull prediction (04 §5.3). Frame-local hitbox capsules come
  from anim poses and feed 04's history.

### 7.2 Animation (ozz 0.17; R06-ENG-28, R08-ED-P1-08)

- **Evaluation.** SoA sampling, blending and local-to-model run in batch jobs. Palettes go to 03 at extract.
  ozz compression is used; ACL is deferred (R10 §7).
- **`AnimGraphDef`** compiles to a flat node array. Parameters come from the schema'd `AnimParams`.
  - Nodes: Clip, BlendSpace1D/2D, StateMachine (with boolean HXL-subset transitions), Layer (bone mask),
    Additive, MontageSlot.
  - IK nodes: TwoBone, Aim, FootPlacement, LookAt.
- **Notifies** are client cues. Gameplay timing uses ASM frames (06 §1.4). Root motion drives montages via the
  shared mover.
- **Retargeting.** `SpeciesDef.retargetMap` covers both baked and runtime retargeting, including digitigrade
  Keth legs.
- **Server.** Cells sample only the hitbox joints (≤ 24), at tick rate, from replicated movement plus montage
  `{id, startTick, rate}`. Budget: 200 entities at 60 Hz in ≤ 1 ms.
- **Crowd and animation-LOD contract (R05, BENCH-1; 03 §7.6):**

  | Tier | Range (scaled by screen size) | Evaluation | Output to 03 |
  |---|---|---|---|
  | A0 | < 15 m | Full graph + IK, every frame | Palette every frame |
  | A1 | 15–30 m | Graph without foot IK, 30 Hz | Palette on update |
  | A2 | 30–80 m | Locomotion subset, 15 Hz | Palette on update; 03 skins at reduced rate |
  | A3 | > 80 m or off-screen | Root motion only | `{impostorPose, heading, phase}` |

  Each entity carries `AnimLod{tier, poseSerial}` in the extract packet, so 03 re-skins only when
  `poseSerial` changes. A governor caps A0 at 32 entities and A1 at 96. Budget: 260 characters ≤ 2 ms on 8
  workers.
- **Later.** Motion matching and facial visemes in Phase 4 (R04); LMM in Phase 5.

### 7.3 Audio (miniaudio) and navigation (Recast/Detour 1.6)

**Audio.**
- **Mixing.** A Helios mixing graph runs on the miniaudio device, with buses Master → Music, SFX, Voice, UI
  and Ambience, and ducking snapshots.
- **Budget.** 128 real voices, ≤ 5 % of one core, ≤ 30 ms latency.
- **Events.** `AudioEventDef` records define containers, RTPCs, attenuation and Doppler. Banks stream with the
  containers that use them.
- **Spatial audio.** Occlusion uses ≤ 64 physics rays per frame. Sound propagates through the `PortalGraph`.
- **Later.** HRTF and voice chat come in Phase 3. Wwise and FMOD are allowed only as `IAudioBackend` plugins
  (ADR-013).

**Navigation (R08-ED-P1-12).**
- **Tiling.** One tiled `dtNavMesh` per grid, with 64 m tiles.
- **Tile sources.** Authored tiles stream with their containers. Terrain tiles are built near activity on the
  Pathfind pool, ≤ 5 ms each. Structures use `DetourTileCache`.
- **Ship interiors** are baked ship-local. `GridLink` off-mesh links trigger `Reparent`.
- **Queries** run asynchronously: 2,000/s at p99 ≤ 5 ms (06 §12.2). DetourCrowd runs per grid.

### 7.4 Scripting host (Luau 0.739; R01-P0-4, R08-ED-P0-10)

- **VMs.** Each cell zone instance has one VM on a serialized **script lane**, a PostPhysics job chain
  (04 §3.3). The client and the editor each have their own VM. Bytecode is shared per content version.
- **Sandbox.** Setup runs `luaL_openlibs`, then the Helios API, then `luaL_sandbox`, and gives each module its
  own `luaL_sandboxthread`. `io`, `os`, `debug` and `loadstring` are removed. `math.random` becomes named
  seeded streams (06 §11).
- **Budgets** (reconciling 04 §3.1 with 06 §11):
  - the lane gets ≤ 15 % of the tick;
  - the `interrupt` callback yields at 2 ms and kills at 5 ms per resume;
  - the heap is capped at 16 or 256 MB per VM, tracked per module with `lua_setmemcat`.
- **Coroutines are tasks.** `wait(zoneSecs)`, `awaitService` and `awaitEvent` yield, and
  `lua_pcallyieldable` makes yields inside `pcall` work.
- **Bindings** are generated from `@script`. Per-entity state lives in `ScriptState` components (04 §3.1).
- **Hot reload** calls `__reload(old)`. Coroutines already running finish on the old code.
- **Debugging.** The **DAP adapter lives in `engine/script`** (about 2 kLOC over `lua_breakpoint` and
  `lua_singlestep`). It serves over TCP from the editor, dev client and cells, including `--replay`.
  luau-lsp reads the generated `.d.luau`, and CI uses the old type solver (R10 §4).
- **Native codegen** is opt-in per module and off for UGC (Phase 5).

### 7.5 Game UI runtime (RmlUi 6.3; R04-P1-18, R03-P0-9, R08-ED-P1-10)

02 owns `engine/ui` and `engine/text`. 08 owns the HUD and screens, `WorldMarkerRenderer` and the addon host.

- **Rendering.** `RmlRenderInterfaceHelios` records `UiDrawList`s (geometry, bindless textures, scissor).
  03's UI pass replays them through the RHI (03 §7.5), and the launcher replays them on SDL_Renderer. UI code
  never calls a graphics API directly (the SWTOR lesson).
- **Text.** `text` is a `Rml::FontEngineInterface` built on FreeType, HarfBuzz, SheenBidi and libunibreak, with
  font fallback chains and MSDF glyphs for world-space text.
- **View-models.** A `viewmodel` generates `ViewModel<VM>` with `edit(FieldMask)` (08 §4.2), an RmlUi data
  model and Luau types.
  - Adapters push only dirty fields.
  - UI events go to Luau, which sends intents.
  - `<datagrid>` virtualizes rows.
- **Diegetic UI.** `UiSurface{document, resolution, maxHz, emissive, interactive}` renders into pooled 2048²
  atlas pages, and only when dirty.
  - Update rate: ≤ 30 Hz within 30 m, ≤ 5 Hz beyond, 0 off-screen.
  - Input: a ray → UV → RmlUi pointer event.
  - Budget: BENCH-1's 40 screens in ≤ 1.0 ms CPU (03 §7.5, 08 §1.8).
- **IME.** SDL3's `SDL_StartTextInputWithProperties`, `SDL_SetTextInputArea` and `SDL_EVENT_TEXT_EDITING` feed
  an inline composition span. It is tested with Windows CJK IMEs.

### 7.6 Input, localization, config (SDL3; R03-P1-2, AAA-CNT-6)

**Input.**
- **Actions and contexts.** `InputActionDef` defines buttons and 1D/2D/3D axes. `InputContextDef` contexts
  stack by priority: UI > seat channel (06 §8.4) > vehicle > on-foot.
- **Bindings** are per device class (keyboard/mouse, gamepad, HOTAS) and support dead zones, curves,
  hold/tap/chord triggers, rumble and gyro. The device class feeds 06's `AimAssistDef`.
- **Sampling.** Input is sampled per command frame.
- **Rebinds** are saved in `input.jsonc`.
- **Bots** inject 04 §5.2 input commands directly, without SDL.

**Localization.**
- **Format.** `LocString` keys point into `.hloc` tables, cooked to a relocatable `.hstr` per language.
- **Messages.** A MessageFormat subset with generated CLDR rules. ICU is tools-only (R10 §12).
- **Budget.** 500k strings: ≤ 100 ns per lookup and ≤ 48 MB per language.

**Config and saves.**
- **User files** live in `%APPDATA%\Helios\<project>\` and are written atomically.
- **World state** stays on the server. Tools snapshot worlds as tagged `.hsnap` files.

---

## 8. Ladder, acceptance, tests, risks, traceability

### 8.1 MVP → AAA feature ladder

| System | P0 | P1 | P2 | P3 | P4 | P5 |
|---|---|---|---|---|---|---|
| Core | Platform, jobs, memory tags, CVars, VFS, CPU gate, crash, Tracy | Budgets in CI | Fibers?; crashpad | io_uring/IORing | Live ceilings | — |
| Schema | schemac C++/Go/Luau/SQL, lock, lint | `$rid`, HXL, tags, split, repl, `@keyed` | proto, validators | Live deltas | — | UGC subset |
| ECS | Wrapper | **50k benchmark**, command buffers, dirty bits, prefabs | Update LOD, budgets | Non-fragmenting pairs? | — | — |
| World | Frame graph | Reparent, grids, BENCH-2, 10¹³ m | Multi-grid queries, portals | Nested grids, BENCH-6 | — | Seamless galaxy |
| Streaming | `.hpak` v0 | Containers, sources, budgets | HLOD, layers, residency | AAA-CNT-2 | MIN tuning | — |
| PCG | `hnoise` + Slang twin | Harrow, stamps, scatter, Scree | Biomes, roads, deltas | Earth-size | Vendor lab | Ecosystems |
| Assets | `.meta`, DDC, glTF, textures | assetd; records/Luau reload | All-asset reload, shared DDC, paks | Zone cook ≤ 60 s | Nightly ≤ 4 h | Distributed |
| Physics | Small hash | Characters, ships, grids | Vehicles | Hangars, buoyancy | Budgets | Destruction |
| Anim | Sampling | Graph, root motion, hitboxes | IK, retarget | Crowd LOD | Motion matching, facial | LMM |
| Audio / nav | Device | Events, 3D / tiles | Banks, occlusion / terrain tiles | Voice / moving interiors | Budgets | — |
| Script | VM, sandbox | Budgets, tasks, DAP, reload | Capabilities (06) | Codegen opt-in | — | UGC VMs |
| UI / input / loc | SDL3 input | RmlUi, view-models, `UiSurface` | IME, loc, rebinding | `<datagrid>` scale | Voiced languages | Addons |

### 8.2 Acceptance criteria (automated)

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RT-01 | **50k-entity zone**, SERVER, 20 Hz (20k replicated + 30k placed, ~150 ECS archetypes, 5k bodies in 12 grids). Engine stages ≤ 12 ms p99; 9k structural ops ≤ 1.5 ms; 50k×3 iteration ≤ 0.4 ms on one thread; ECS ≤ 400 MB; ≤ 5,000 tables. **Failure opens the custom-ECS ADR** | AAA-SRV-4, CNT-4 | 1 |
| RT-02 | **10¹³ m** (with 03): ship at 1 km/s, cockpit camera. Jitter < 0.05 px for a grid cube at 2 m and a system-frame object at 1 km; physics drift < 1 mm over 10 min; replication error ≤ 1/256 m | AAA-REN-5 | 1 |
| RT-03 | **Physics hash**: 1,000 bodies + 50 characters + 10 vehicles × 3,600 steps, identical on 4 compilers and at 1/4/16 workers | AAA-PLT-4 | 0–1 |
| RT-04 | **PCG hash**: 10k tiles on 3 bodies + scatter + asteroids, bit-identical across compilers and client/cell. `hnoise` C++/Slang corpus matches on lavapipe and vendor GPUs (≤ 1 cm) | AAA-PLT-4, CNT-1 | 1 |
| RT-05 | **Hot reload** in PIE client and cell ≤ 2 s p95: records and Luau (Ph1); all asset kinds (Ph2) | AAA-ITR-1 | 1–2 |
| RT-06 | **Streaming**: game-thread I/O waits ≤ 1 ms; zero misses in BENCH-2 at 1,500 m/s; ≤ 150 MB/s | AAA-CNT-2 | 1–3 |
| RT-07 | **Records**: 100k compile ≤ 60 s, load ≤ 2 s | AAA-CNT-5 | 3 |
| RT-08 | **Cook**: zone ≤ 60 s, nightly ≤ 4 h, warm editor open ≤ 10 s | AAA-ITR-2/4 | 2 |
| RT-09 | **Lint**: no server-only bytes in client paks; every client→server rpc has `@ratelimit` + `@intent`; no `EDITOR_ONLY` or AVX2 leak | AAA-SEC-1/4 | 1 |
| RT-10 | **Memory**: §2.2 budgets hold in every BENCH scene; 24 h soak RSS growth ≤ 2 % | AAA-CNT-3/4, STB-4 | 2–4 |
| RT-11 | **BENCH-6**: 1,000 boardings at 300 m/s under 1 g, zero fall-throughs; transfer ≤ 1 tick, < 1 mm | W02 | 3 |
| RT-12 | **Frame (BENCH-1)**: main thread ≤ 8 ms; 260 characters ≤ 2 ms; 40 UI surfaces ≤ 1.0 ms | AAA-REN-1 | 1–4 |
| RT-13 | **Script**: runaway killed ≤ 5 ms; heap caps; 10k coroutines per zone | 06 §12.2 | 1 |
| RT-14 | **Iteration**: `.cpp` edit → game DLL reloaded ≤ 30 s (MSVC); cell boot ≤ 5 s | AAA-ITR-3/5 | 1 |
| RT-15 | **Localization**: 500k strings, ≤ 100 ns lookup, ≤ 48 MB/language | AAA-CNT-6 | 4 |
| RT-16 | **Crashes**: 100 % symbolicated dumps | AAA-STB-1 | 2 |
| RT-17 | **Paks**: 1 % change → ≤ 1.5× changed bytes downloaded; bad blocks re-fetched | AAA-CNT-7 | 2 |

### 8.3 Test strategy

- **Unit and schema tests.** Each module has doctest suites. schemac emits round-trip and keyed-merge tests
  for every type.
- **Determinism** (4 compilers, nightly): physics, PCG plus the `hnoise` twin corpus, HXL, `det::`, `noise`,
  `Fixed64` and the 04 replay.
- **Fuzzing.** The JSONC, tagged, cooked and `.hpak` readers and the hot-reload protocol get ≥ 24 CPU-hours
  per release (AAA-SEC-7).
- **Benchmarks.** CTest `bench` runs nightly on REF and SERVER. A regression of more than 5 % fails, and RT-01
  and RT-12 gate releases.
- **Scenarios.** BENCH-2 and BENCH-6 flythroughs run with throttled I/O and injected misses. Bots cross every
  boundary type (R04 §9.5).
- **Sanitizers.** ASan and UBSan nightly; TSan on jobs, ECS and streaming; MSVC ASan. Windows path, DPI and
  IME cases; MinGW.

### 8.4 Risks and mitigations

| Risk | Mitigation |
|---|---|
| flecs performance, fragmentation, single maintainer (R10 §14.2) | RT-01 in Phase 1; flecs confined to `engine/ecs`; non-fragmenting traits; custom ECS behind the same API |
| Grid-transition bugs (SC's longest-running class, R04 §2.3) | One transfer path, hysteresis, invariant checks, boarding bots |
| GPU/CPU terrain mismatch | Fixed-point `hnoise` with a Slang twin, a conformance corpus, CPU-tile fallback |
| Determinism regressions | `fp_control.h`, `det::`, sorted callbacks, 4-compiler hashes |
| Jolt float broadphase; many `PhysicsSystem`s | ≤ 20 km bubbles, re-centring, pooling, body caps |
| Streaming stalls, including during streaming install | Lookahead, I/O budgets, residency priorities, HLOD fallback, flythrough CI |
| SWG terrain patents | Generic node graph; legal review before Phase 4 |

### 8.5 Traceability

| Requirement | § |
|---|---|
| R06-ENG-01, -02, -32; R05 §2.1; R10 §5 | 1 |
| R06-ENG-03, -16; R05-P0-1, P0-2; R01-P0-5 | 2 |
| R06-ENG-05; R03-P0-1; R02-P0-4, P0-6; R04-P0-4, P0-9; R05-P0-7; R08-ED-P0-01, P0-02; R01-P1-16 | 3 |
| R06-ENG-04, -06, -21; R04-P0-6, P1-16; R08-ED-P0-05 | 4 |
| R06-ENG-07, -20, -23, -25; R04-P0-1, P0-2, P0-3, P0-5, P1-12, P1-15, P2-24; R02-P0-5, P0-8; R08-ED-P0-07, P1-06, P1-07, P2-02 | 5 |
| R06-ENG-11, -12, -13; R01-P0-9; R03-P0-5, P1-6; R05-P0-10; R08-ED-P0-08, P0-16 | 6 |
| R06-ENG-28; R01-P0-4; R04-P1-18; R03-P0-9, P1-2; R08-ED-P0-10, P0-11, P1-08, P1-10, P1-12, P1-13, P2-03; R10 §4–8, §12 | 7 |

### 8.6 Cross-section alignment

| Section | What this section provides or adopts |
|---|---|
| 03 Rendering | Two-level transforms (§5.2); GPU terrain from `hnoise` (§5.8); `FrameChanged`; Gerstner buoyancy; portal and crowd-LOD contracts. `RenderScene` is defined in `render` and filled by `presentation` |
| 04 Networking | Field-level `Mut<C>` dirty bits; EntityId↔NetHandle; `ScriptState`; frame-local hitbox poses; §3.1 header sugar |
| 05 Backend | `service` blocks, `@ledger_policy`, `@lifecycle`, `ReasonCodeDef`, `@table`/`@sql`, proto/Go/C++/NATS emitters. Dev database is embedded-postgres (ADR-014, 05 §3.5); no service uses SQLite |
| 06 Gameplay | §3 is normative. `hmath` means `helios::det` |
| 07 Editor | `EDITOR_ONLY`, `@keyed`, minted `$rid`, editor metadata, Go validators, DAP in `engine/script`; the planet library is `engine/pcg` (07 uses this name) |
| 08 Client | AVX2 containment, tagged ≤ 2 GiB paks with block checksums, residency hook, per-zone PSO lists, `engine/ui` and `engine/text` |
| 09 Roadmap | RT-01 gates the ECS decision; approve libopus; implement the `LAYER` and `EDITOR_ONLY` checks |
