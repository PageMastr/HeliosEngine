# Helios — Architecture Decision Record (ADR index)

Status: **accepted** unless marked *open*. Every plan section and every implementation task must
conform to these decisions; changing one requires a new ADR entry with rationale.
Research backing lives in `docs/research/01..10-*.md` (cited as R01..R10).

| ADR | Topic | ADR | Topic |
|---|---|---|---|
| [001](#adr-001-platforms) | Platforms; [001a](#adr-001a-windows-toolset-policy-review-round-2) Windows toolset policy | [009](#adr-009-editor) | Editor |
| [002](#adr-002-languages) | Languages | [010](#adr-010-client-game-ui--launcher) | Client, game UI, launcher |
| [003](#adr-003-rendering) | Rendering | [011](#adr-011-core-runtime) | Core runtime |
| [004](#adr-004-entity-model--reflection) | Entity model, reflection; [004a](../adr/ADR-004a-ecs-rt01-structural-ops.md) ECS structural ops vs RT-01 (*open*) | [012](#adr-012-quality-bar--process) | Quality bar, process |
| [005](#adr-005-world-model-large-worlds) | Large worlds | [013](#adr-013-runtime-libraries-r10-manifest) | Runtime libraries |
| [006](#adr-006-content-model) | Content model | [014](#adr-014-backend-toolchain-r10-11) | Backend toolchain |
| [007](#adr-007-server--network-architecture) | Server and network | [015](#adr-015-voice-chat) | Voice chat |
| [008](#adr-008-backend-services) | Backend services | [016](#adr-016-dev-versus-shipping-link-model-game-module-hot-reload) | Dev vs shipping link model |

## ADR-001 Platforms
- **Windows 10/11 x64 is the primary development and play platform** (user requirement).
  Toolchains: **MSVC**, with a primary toolset and a supported floor (the toolset policy below), and clang-cl.
  Visual Studio solutions come from CMake presets for both supported Visual Studio versions.
- **Linux x64 is fully supported**: GCC 13+ / Clang 17+. Linux is the production OS for dedicated
  servers; Windows can run every server locally for development.
- Every commit builds and tests on both MSVC toolsets, clang-cl and Linux GCC/Clang in CI (GitHub
  Actions), plus a MinGW cross-compile portability check that can run inside Linux containers.
- macOS/consoles are non-goals for now; the RHI seam keeps them possible (R01, R06).

### ADR-001a Windows toolset policy (review round 2)

**Context.** GitHub's `windows-latest` image now ships Visual Studio 2026 (toolset v145, MSVC 14.51), so the
Windows CI job has been building with it. The `DOCTEST_CONFIG_USE_STD_HEADERS` fix in
`third_party/CMakeLists.txt` and the `windows-vs2026` preset come from that move. Before this ADR, no job
tested VS 2022, which the plan called primary. MSVC binary compatibility runs in one direction only. The
v140–v145 toolsets are ABI-compatible, with three exceptions, cited below as C1–C3:
1. **C1.** The final link must use a linker at least as new as the newest toolset that compiled any object or
   static library in it.
2. **C2.** A process must load a VC++ runtime at least as new as the newest toolset that built any of its images.
3. **C3.** `/GL` objects and `/LTCG` libraries link only with the exact compiler version that produced them.

The binary SDK (09 §2.7.2) ships static libraries and import libraries. Whatever toolset builds it
therefore sets the minimum toolset for every studio that uses it.

**Decision: two toolset roles.**

| Role | Toolset | Pin | Tested by | Builds |
|---|---|---|---|---|
| **Primary** (daily development; the recommended install) | Visual Studio 2026, v145 | Not pinned: the newest MSVC 14.5x update on `windows-latest` (`_MSC_VER` ≥ 1950; 14.51 today) | PR: `windows-msvc` (monolithic `windows-msvc-release`, all tests) and `windows-msvc-dev` (modular, symbol audit, 20-reload smoke; ADR-016). Nightly: `windows-vs2026` MSBuild build plus `ctest` | Developer builds, most of the PR signal, and the user's milestone validation by default (09 §5.9) |
| **Floor**, which is also the **release toolset** | Visual Studio 2022 17.14, v143 | Exactly **MSVC 14.44** (`_MSC_VER` 1944, compiler 19.44), the last v143 release | PR: `windows-msvc-floor` on the pinned **`windows-2022`** image through `ilammy/msvc-dev-cmd` with `toolset: 14.44`: `windows-msvc-release` build, all unit tests and warnings-as-errors. Nightly: `windows-vs2022` MSBuild build plus `ctest` | **Every Windows binary Helios releases:** the SDK's static and import libraries, the SDK's editor, tools and servers, and the launcher, client, cells, gateway and voice forwarder. Also the binaries that nightly BENCH and H-class runs execute, so the measured binaries are the shipped ones |
| clang-cl | The clang-cl that ships with the same Visual Studio, over the primary toolset's STL | Follows the primary | PR: `windows-clang-cl` | Portability and warning coverage only; never a release toolset |

**Why build releases with the floor.** Given C1, this is the only choice that lets studios on VS 2022 17.14
and studios on any VS 2026 update consume the same SDK. The v145 linker reads v143 libraries; the v143 linker
does not read v145 ones. ADR-002 fixes the language at C++20, which 14.44 implements fully, so the floor costs
no feature the code base uses. The main v145 gains are C++23 library work and optimizer improvements. Budgets are
measured on floor-built binaries, so every perf criterion is met by the binaries that ship.

**Rejected alternatives.**
- **(a) Declare v145 the minimum now.** VS 2022 17.14 users could no longer use the SDK, and the gain is
  nothing ADR-002 needs. This becomes the plan at 1.0, under rule 6.
- **(b) Build the SDK once per toolset.** It doubles SDK size, signing, validation and the `upgrade-project`
  matrix, and a single floor build satisfies C1–C3 for both toolsets anyway.
- **(c) Keep VS 2022 primary and ignore the runner image.** No CI job would test what developers actually
  install, and each image update would break the build without notice, as doctest did.

**Rules.**
1. **Consumers.** A project that uses the SDK (game modules, the shipping link, tools) may build with any MSVC
   from 14.44 to the newest v145 update, or with clang-cl over such an STL. The SDK's `HeliosConfig.cmake`
   stops configure when `CMAKE_CXX_COMPILER_VERSION` is below 19.44, and `helios/sdk_config.h` raises a
   matching `#error` when `_MSC_VER` is below 1944. The message names the installed version and the fix
   ("install VS 2026, or update VS 2022 to 17.14"), so the user never sees a bare LNK2001 on an STL internal.
   The same two files hold consumers to the `avx2` ISA level (02 §1.1). `HeliosConfig.cmake` exports the
   `avx2` flag set as `INTERFACE` compile options on every imported SDK target. `sdk_config.h` raises an
   `#error` when `__AVX2__` (and, off MSVC, `__BMI__`, `__BMI2__`, `__F16C__`, `__LZCNT__` and `__POPCNT__`) is
   missing, or when `__FMA__`, `_M_FP_FAST` or `_M_FP_CONTRACT` is defined.
2. **No `/GL` in distributed libraries (C3).** SDK `.lib` files are compiled without `/GL` and archived without
   LTCG. Binaries that Helios links itself (client, servers, launcher) may use LTCG and PGO (02 §1.4), because
   one toolset compiles and links them. The release job fails if any SDK archive member is an LTCG
   (anonymous-object) member, per `dumpbin /headers`, or if its Rich header shows a compiler newer than the
   pin.
3. **Runtime DLLs for dev `/MD` images (C2).** The SDK is a per-user install without UAC, so it does not ship an
   app-local VC++ runtime. App-local DLLs take precedence over `System32`, and an app-local copy would pin an
   old runtime under a game DLL built by a newer v145 update. It relies on the system-wide v14 redistributable
   that Visual Studio installs, which is always at least as new as the installed toolset. On first run the editor
   checks the registry value `HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64\Version` against the
   SDK's minimum and links to the redistributable if it is older. The **packaged artist editor** (ADR-016;
   AAA-ITR-6) does carry app-local runtime DLLs. `helios-tool package-editor` copies them from the newer of
   two sources: the SDK's validated redistributable, or the project toolset's `VCToolsRedistDir`. Shipping
   (`/MT`) images link the CRT statically through the consumer's linker, so this rule does not apply to them.
4. **CRT and debug variants.** SDK libraries are built with `_ITERATOR_DEBUG_LEVEL=0`. The static libraries for
   the shipping link are `/MT`; the dev shared libraries and their import libraries are `/MD`. Project presets
   (09 §2.7.2) add a **`DebugGame`** configuration for debugging game code against the SDK: `/Od /Ob0`, `/MD`,
   IDL 0 and full PDBs. Building with `/MDd` or IDL 2 against the SDK is unsupported. The STL's
   `detect_mismatch` pragmas turn that into LNK2038, and the template's CMake stops it earlier, at configure.
   Engine-source builds keep full Debug (IDL 2), because every image is built together.
5. **Presets.** The IDE presets are modular because IDE users are the ones who edit game code and expect hot
   reload (AAA-ITR-5). In the table, "the prompt's `cl`" means the one in the developer prompt, primary or floor.

   | Preset | Generator | Toolset | Link model | State |
   |---|---|---|---|---|
   | `windows-msvc-release` | Ninja | The prompt's `cl` | Monolithic, `/MT` | Exists |
   | `windows-msvc-debug` | Ninja | The prompt's `cl` | Monolithic, Debug | Exists |
   | `windows-msvc-dev` | Ninja | The prompt's `cl` | `HELIOS_MODULAR=ON`, `/MD` (ADR-016) | WP-0.6c |
   | `windows-vs2026` | Visual Studio 18 2026 (CMake ≥ 4.2, the first release with this generator) | v145. `-T v143,version=14.44` gives a floor build inside VS 2026 when the v143 build-tools component is installed | `HELIOS_MODULAR=ON` from WP-0.6c; monolithic until then | Exists, monolithic |
   | `windows-vs2022` | Visual Studio 17 2022 | v143 (14.44) | `HELIOS_MODULAR=ON` from WP-0.6c | Exists, monolithic |
   | `windows-clang-cl` | Ninja | clang-cl | Monolithic | Exists |

   Ninja presets need only CMake ≥ 3.28 (ADR-011). Release and CI builds use the Ninja presets, and the
   MSBuild presets are built nightly.
6. **Moving the floor.** The floor changes only through an ADR-001 amendment. A floor bump breaks the SDK under
   09 §2.7.2's rules, so it is announced at least two minor releases ahead. Before 1.0 it lands in a minor
   release; after 1.0, only in a major release.
   - **Planned:** at the 1.0 release (Phase 4 exit), the floor and release toolset move to the v145 update that
     is current when the 1.0 branch is cut. VS 2022's mainstream support ends in January 2027.
   - **Earlier triggers:** (a) the code needs a feature or codegen fix that 14.44 lacks, in which case the
     amendment cites the failing test; or (b) GitHub announces the retirement of `windows-2022`. For (b), the
     floor job first moves to `windows-latest` with the v143 (14.44) build-tools component and
     `-vcvars_ver=14.44`. The floor itself moves only if that component cannot be installed.
   - **The primary is never pinned.** Every `windows-latest` update is tested in the PR tier on the day the
     image changes, so a break like doctest's shows up there and not at a release.
7. **Counting compilers.** Wherever the plan says "five compilers" (DoD item 3 in 09 §5.3, the determinism
   hashes, K4), it counts compiler families, and the MSVC family means both MSVC jobs. Both run the same unit
   tests, warnings-as-errors and golden determinism hashes (AAA-PLT-4), so any difference in codegen or warnings
   between 14.44 and 14.5x fails the PR.
8. **Linux.** libstdc++ has the same one-way compatibility. Linux release binaries and the Linux SDK are built
   in 08 §1.16's Steam Runtime 3 "sniper" container (glibc ≥ 2.31) with the GCC 13 floor. WP-0.1 installs
   GCC 13 there if the image's default compiler is older. SDK consumers need GCC ≥ 13, or Clang ≥ 17 over a
   GCC 13 or newer libstdc++.

**PR-tier compiler matrix (8 configurations; 09 §5.6).**

| Job | Runner | Toolchain |
|---|---|---|
| `windows-msvc` | `windows-latest` | Primary MSVC |
| `windows-msvc-dev` | `windows-latest` | Primary MSVC, modular (from WP-0.6c) |
| `windows-msvc-floor` | Pinned `windows-2022` | Floor MSVC 14.44 |
| `windows-clang-cl` | `windows-latest` | clang-cl |
| `linux-gcc` | Ubuntu 24.04 | GCC |
| `linux-clang` | Ubuntu 24.04 | Clang |
| `linux-headless` | Ubuntu 24.04 | GCC, no graphics |
| `cross-mingw` | Ubuntu 24.04 | MinGW cross-compile |

The Nightly tier adds `windows-vs2026`, `windows-vs2022`, `linux-dev` and the sanitizers.

**Acceptance.**
- **AAA-PLT-1** (01 §3.10) passes only with all eight PR jobs green. WP-0.1 adds the floor job and the two
  nightly MSBuild jobs. The current `ci.yml` has only the primary Windows job and the two Linux jobs (09 §8.1).
- A CMake script test drives the rule-1 version gate with injected versions: 19.43 fails with the message, and
  19.44, 19.50 and 19.51 pass (WP-2.16a2).
- From WP-2.16a2 (Phase 2), a nightly `sdk-consumer` job installs the SDK built by the floor job on both images.
  It configures `starter-blank` with a C++ game module, builds its `DebugGame`, dev and shipping link variants,
  and runs its PIE smoke test: with MSVC 14.44 on `windows-2022`, and with the primary on `windows-latest`.
  It also asserts that every game-module object carries the `avx2` flag set, runs 02 §1.1's pre-gate check on
  the linked images, and runs the shipping client under SDE `-nhm`, which must exit with code 78.
  Either leg failing blocks the release.
- `tools\milestone\validate.ps1` accepts either toolset (09 §5.9) and records the compiler version in the
  milestone report.

## ADR-002 Languages
- **C++20** for engine runtime, editor, client, launcher, zone/cell servers, gateway and tools.
- **Go 1.27.1** for control-plane / backend services (no CGO, so they build natively on Windows; see ADR-014).
- **Luau** (typed, sandboxed Lua dialect, MIT) for gameplay scripting on client, server and editor —
  chosen over LuaJIT by R01/R03/R05/R06 (sandboxing for UGC & server, gradual typing, active
  maintenance, native codegen on x64). One VM per zone/worker; the hot path never goes through
  scripts.
- **Visual graphs** (logic, quests, dialogue, AI, materials, VFX, abilities) are domain-level node
  graphs (Godot VisualScript lesson). Logic graphs compile to Luau source. **Predicted abilities**
  compile to a native, rollback-safe state-machine format executed by C++ (Overwatch Statescript
  lesson, R05) — never to script.

## ADR-003 Rendering
- **Vulkan 1.3** RHI (dynamic rendering, synchronization2, timeline semaphores, descriptor
  indexing/bindless, buffer device address) through volk + VMA. RHI is a thin handle-based layer
  (Godot RenderingDevice / O3DE RHI level) with a **Null backend** for headless servers/tests and a
  seam for a future **D3D12** backend (Windows-first product; decision deferred to Phase 4).
- **Render graph** (Frostbite FrameGraph shape) with automatic barriers, transient aliasing,
  async compute, parallel command recording.
- **Extract → prepare → submit** pipeline (Destiny, R05): simulate frame N while rendering N-1.
- **GPU-driven**: persistent GPU scene, compute culling (frustum + HZB), indirect draws; meshlets
  and a visibility buffer later. **Clustered forward+** baseline shading, PBR.
- **Camera-relative rendering, reverse-Z, infinite far plane**; CPU culling in f64.
- Sci-fi feature set (R09): physically based atmosphere (Hillaire 2020 LUTs), cube-sphere
  quadtree planets, volumetric clouds, volumetric nebulae + starfields, HDR/bloom/auto-exposure,
  GPU particles, shields/holograms/engine plumes, TAA + upscaling.
- **Shader toolchain: Slang** (Apache-2.0 w/ LLVM exception; modules, generics/interfaces for
  material graphs, built-in reflection). Consumed as a **pinned prebuilt release (v2026.18.2)**
  fetched by a SHA-256-verified bootstrap into `tools/prebuilt/slang/` (override with
  `HELIOS_SLANG_ROOT`). `helios-shaderc` (C++, links slang) runs in the asset processor and emits
  SPIR-V + a Helios reflection blob. The editor loads Slang at runtime for hot reload; the shipped
  client contains **no** shader compiler (cooked SPIR-V + recorded PSO lists → `VkPipelineCache`).
  Plan B: vendored glslang + SPIRV-Reflect (R10 §3).

## ADR-004 Entity model & reflection
- **Archetype ECS = flecs v4.1.6** (MIT) is the single runtime model on client, server and editor;
  relationships (`ChildOf`, `IsA` prefabs, custom pairs such as `InFrame`, `DockedTo`) for
  parent/child, attachment, ownership, docking. The scene tree is a *view*. Components come from
  schema codegen; replication dirty bits live in our components (Iris-style push); network IDs are
  our own 64-bit IDs mapped to `flecs::entity`; flecs systems run on the Helios job system.
  A custom ECS is the fallback only if the Phase 1 50k-entity zone benchmark fails (R10 §6).
- ***Open:* [ADR-004a](../adr/ADR-004a-ecs-rt01-structural-ops.md) (2026-09-25).** The Phase 0 pre-bench
  (`engine/ecs/SPIKES.md` §3) passed every RT-01 clause except structural ops: 3.4–6.7 ms for 9k ops against
  1.5 ms, while raw flecs does the same ops in ≈ 0.9 ms. The overhead is the Helios wrapper's bookkeeping, so
  option A (optimize the wrapper, keep flecs) runs first in WP-1.1a. A custom ECS is decided only if the
  optimized wrapper still fails RT-01 on SERVER at the Phase 1 gate. The budget stays 9k ops per sync point.
- Terminology: "**ECS archetype**" means component storage only; designer-facing content types
  are "**record templates**" (never "archetype") across all plan sections.
- **One schema language** (`*.hschema`, compiled by `helios-schemac`, a C++ tool built first in
  the build) generates: C++ types + reflection + binary/text serializers + editor metadata + Luau
  bindings + **replication descriptors** (quantization, audience: all/owner/server-only, R02);
  Go structs + wire codecs; SQL migration stubs; typed-record (DataForge-like) tables (R03, R04).
- Stable 64-bit IDs everywhere. Runtime entity IDs are time-prefixed (Snowflake-style) 63-bit IDs minted
  locally from PG-allocated blocks, laid out 41-bit ms / 5-bit shard / 17-bit offset with no node IDs
  (05 §1.4.5, 02 §4.1); content IDs = GUID / path hash.

## ADR-005 World model: large worlds
- **Nested reference frames**: galaxy/sector → star system → body (rotating) → grid (ship/station)
  → interior. Positions are **frame-local f64** (`WorldPos`); local offsets/rotations f32.
  First-class `Reparent()` preserving world position and velocity (R04).
- **Jolt Physics** with `JPH_DOUBLE_PRECISION`; **one PhysicsSystem per grid** (planet surface,
  ship interior, station, open space), with grid transfer at boundaries (R04, R06).
- Deterministic unit test: render + simulate at 10^13 m from origin without jitter.

## ADR-006 Content model
- **Object containers**: authored, nested, streamable units — same unit is edited, streamed by
  client and server, persisted and hot-reloaded (R04). Source is diffable text, one entity per
  file where practical (OFPA-style); cooked to relocatable binary.
- **Typed record database** (items, ships, abilities, NPCs, loot, recipes…) from the schema;
  text source in git; compiled binary with **hash IDs** shared by client, servers and Go
  services (Destiny/DataForge/EVE FSD, R01/R04/R05).
- **Asset pipeline**: import → intermediate → cook(platform) with a derived-data cache keyed by
  content hash; packaging into **content-addressed, content-defined-chunked** containers (TOC →
  chunk hashes, zstd) so the launcher downloads only missing chunks (R01, R07).
- **Deterministic procedural generation** (planets, terrain layer stacks à la SWG, scatter,
  resources): same seed → bit-identical results on client and server (R02, R04).
  *Legal note (R02): the SWG terrain approach is covered by US patents 8,115,765 / 8,207,966 /
  8,368,686 — obtain legal review before shipping a direct re-implementation; our design uses a
  generic node-graph formulation.*

## ADR-007 Server & network architecture
- Topology (R07 §8): **Client ↔ Gateway (C++, encrypted UDP, netcode.io-style connect tokens)
  ↔ Cell servers (C++ headless engine)**. Clients never connect directly to a cell; the gateway
  hides topology and re-routes on handoff (R02, R04, R07).
- **Shard → Zone → Cell**. Zone = gameplay space with its own frame (system, planet surface,
  interior, instance). Cell = spatial region owned by exactly one process. Per-zone tick rate
  (1–60 Hz) on a **dilatable zone clock** (EVE TiDi built in, R01).
- **Authority**: single writer per *authority group* (ship + contents) with **epoch fencing**;
  ghosts within an interest margin; cross-boundary effects as ordered messages to the owner.
- Roadmap: **v0** one cell per zone (EVE/Albion model) with handoff/ghost *interfaces* present →
  **v1** (Phase 3) static multi-cell zones with handoff p99 < 100 ms, and planned region migration for
  rolling restarts, drains and pre-provisioning (≤ 1 s hitch, no state loss; 04 §6.7) → **v1.5** (Phase 4) a **replicant
  state tier** for every persistent world zone → **v2** (Phase 5) dynamic split/merge + gateway-side replication
  layer. No SpatialOS-style generality up front (R07).
- **Replicant tier (Phase 4; review round 1, option (a) chosen).** A replicant is `helios-cell --replicant`:
  the headless binary with no simulation. The orchestrator places one 2-vCPU replicant per ≤ 4 world-zone
  cells (from one zone or several), on a different host. Cells stream it the field chunks of every audience that the serialize-once gather
  already produces for ghosts (04 §4.2, §6.2), for every AG they own, fenced by `(region, lease_gen)` and AG
  epoch. The replicant keeps the latest state in RAM, holds no authority, never serves clients and never
  writes persistence. After a cell crash the warm standby resumes from that RAM instead of from 30 s-old
  checkpoints, so a world zone loses ≤ 1 s of non-value state (AAA-SRV-9 Ph4, NS-4.6) with no
  disconnect and a ≤ 10 s hitch (04 §6.4–6.5). It is the default for every persistent world-zone profile
  (hub, station, open space, fleet battle), single-cell or multi-cell, and mandatory for multi-cell zones;
  activity, phase and housing instances default to checkpoints and may opt in by zone record (round 4: SC's
  shipped recovery covers every server of a shard, so multi-cell-only coverage left most players at 30 s).
  - *Why.* Star Citizen shipped its replication layer (Replicant, Atlas, Scribe and Gateway) with
    Persistent Entity Streaming in Alpha 3.18 and static server meshing on it in Alpha 4.0 (December 2024), so
    a game-server crash no longer disconnects players (R04 §4.1–4.2). The build loop stops at Phase 4, so a
    Phase 4 bar with checkpoint-only recovery would sit below the shipped reference. The tier reuses chunks
    cells already encode, so it costs a trunk stream (≤ 20 Mbit/s per 500-player cell) and one small process
    per ≤ 4 cells.
  - *Scope versus SC.* SC's Hybrid service also streams client replication from its replicants. Helios
    keeps client replication in cells until v2; the player-visible outcome of a crash (no disconnect, state
    resumed from RAM) is the same from Phase 4.
  - *Rejected:* (b) keeping all stateful replication in Phase 5 with a mandatory hot standby per multi-cell
    zone, which meets the RPO but costs a second simulating-class process per zone, leaves the stateful tier
    to an opt-in phase and gives v2 nothing to build on; state held in gateways, which would make the
    stateless public edge stateful and put server-audience data on it.
  - *Legal gate.* Counsel reviews Improbable's US 11,792,306 before WP-4.3 starts, and again before the
    Phase 5 gateway layer (WP-5.2; 09 K18). If the review requires it, the same stream feeds a per-zone hot
    standby cell instead of a replicant, with the same ≤ 1 s RPO.
  - *v2 builds on it:* the Phase 5 gateway replication layer moves client interest and priority onto the
    replicant tier, which makes a cell crash invisible (≤ 1 s hitch).
- **Replication** (Tribes/Quake3/Iris): interest grid, priority × staleness under a per-client
  bandwidth budget, serialize-once per entity per tick, delta vs acked baseline, quantization.
  Owner prediction + reconciliation; others interpolated; lag-compensated hit registration with
  capped rewind; server-authoritative movement with speedhack checks (R05, R07).
- Overload policy: offload → degrade replication → split → time dilation → admission control.

## ADR-008 Backend services
- **Go services**: auth/identity (argon2id, OIDC-ready, JWT), login queue, session/connect-token,
  orchestrator/world-directory (leases, epochs, placement), character, **item & currency ledger**
  (append-only, idempotency keys, atomic trades, epoch-fenced writes), market (regional order
  books), crafting/industry jobs, mail, chat, social (friends, guilds/corps, presence), durable
  timers, persistence gateway (write-behind batching from cells), content/publish service,
  telemetry ingest, GM/admin API, patch manifest service, public read API.
- **Messaging**: NATS + JetStream for control plane and durable events; cell↔cell handoff and
  replication use a direct low-latency transport, never the bus (R02, R07).
- **Data**: PostgreSQL = system of record; Valkey (Redis-compatible, BSD) = cache/presence/queues.
  No gameplay logic in SQL.
- **Local dev on Windows without Docker**: one `helios-backend` binary runs every service
  in-process with embedded NATS, embedded-postgres and miniredis (ADR-014). Docker Compose (Postgres/Valkey/NATS) for
  production-like runs; Kubernetes + Agones for fleets later.
- Every value movement logged with a reason code (faucets/sinks telemetry, R01).
- **Studio backend extension (amendment, round 3; 05 §1.23).** Studios extend the backend with **world
  scripts**: shard-scope sandboxed Luau in world-script hosts (`helios-cell --role world-script`), with tables,
  RPCs, events, timers and reason codes declared in a `worldscript` schema block, one generic PG store, and
  escrow-only value (cells escrow, scripts release, sink or grant within faucet caps). A public Go
  service-module API is rejected for now: Go's `plugin` package does not work on Windows, and studio Go would
  bypass the Luau sandbox's fuel and heap limits. The Go service interfaces stay internal.

## ADR-009 Editor
- ImGui docking + multi-viewport, on top of a UI-less **ToolsFramework** (documents, selection,
  commands, reflection-based property-diff transactions, asset DB client, automation) (R06).
- Editor runs the real renderer and simulation; **Play-in-Editor spawns a local cell server +
  N clients** (and bot clients).
- **Live collaborative editing** against a running dev server using a HeroEngine-style **edit
  instance** (only the edit instance persists changes; play instances are pinned to published
  content versions) (R03).
- Tool suite T01–T30 per R08 §3 (world, prefabs, terrain/planet, star-system/galaxy, data &
  record templates, gameplay systems, script IDE + debugger, visual graphs, quest, dialogue, sequencer,
  animation, material graph, VFX, environment, UI designer, audio, modular ship/base assembly,
  character customization, AI/nav/spawn, asset browser, localization, profiling, GM/live-ops,
  validation, build/cook/deploy, collaboration).

## ADR-010 Client, game UI & launcher
- Client = same engine runtime + game UI + netcode; one continuous camera across cockpit/FPS.
- **Game UI** is a retained-mode, data-bound runtime UI system (renders to screen or to texture
  for diegetic screens). ImGui is editor/debug-only (R04).
- **Launcher** (C++, Windows-first): login, news, content-addressed chunk patching from CDN
  manifests, verification/repair, self-update; code-signing and installer per R10. UI is **RmlUi on
  SDL_Renderer** (shares the game-UI toolkit; no Vulkan/AVX2 dependency so it can run a CPU gate
  and explain unsupported hardware). Self-installing per-user installer; MSIX/WiX rejected.
- Font assets: SIL OFL-1.1 is allow-listed for fonts (not code).

## ADR-011 Core runtime
- Platform layer with Win32 and POSIX implementations (sockets, file mapping, dynamic libraries,
  threads, timers, crash handling/minidumps). **Windowing & input: SDL3 3.4.16** (zlib) — chosen
  over GLFW for IME text input (MMO chat), gamepad rumble/gyro, raw mouse and multi-window editor
  (R10 §8). Windows executables embed a manifest (UTF-8 code page, PerMonitorV2 DPI, long paths).
- Job system: task graph with worker pool, priorities, **counter-based waits that help execute
  other jobs**, separate long-running pools (IO, shader compile, pathfinding); Jolt's
  `JobSystemWithBarrier` and flecs worker threads run on it. Fibers (Win32 fibers / asm context
  switch on Linux, R10 §6) are a Phase 2+ optimization behind the same API if profiling demands.
- Memory: **mimalloc v3.5.3** heaps behind the tagged-allocator interface (no global override);
  Luau, Jolt, flecs and ImGui allocators routed through it.
- Formatting: `std::format` (all three toolchains support it) behind `HELIOS_LOG_*`.
- **MSVC runtime: static `/MT`** for every shipped or deployed binary (no VC++ redist for players); dev
  modular builds use `/MD` (ADR-016); `/Z7` debug info so
  compiler caches work; CMake ≥ 3.28 (≥ 4.2 only for the `windows-vs2026` generator). Which MSVC toolset
  builds what is ADR-001a.
- Tagged memory with budgets; frame/tick linear allocators; generational handles across
  subsystem boundaries.
- **ISA levels (amendment, round 3; 02 §1.1).** Every x86-64 image that links runtime modules (client,
  cell, editor, bot, gateway, voice, tools) is built whole at the **`avx2`** level: AVX2, BMI1/2, LZCNT,
  POPCNT and F16C, with no FMA contraction and no AVX-512. The launcher and bootstrap are **`base`**
  (x86-64-v1: SSE2 only, MSVC's default; 08 §2.1.1). Inside each `avx2` image only the CPU-gate TU is `base`. It runs before
  any other code in the image, third-party pre-`main` hooks included. On Windows it is the first TLS callback
  (`.CRT$XLA0`) of the first-initialized image, so it precedes mimalloc's `.CRT$XLB` callback and `.CRT$XIB`
  initializer and Tracy's `.CRT$XCB` statics and `thread_local`s. The round-3 `init_seg(compiler)` placement
  did not. On Linux it is the `.preinit_array` entry. It uses no STL, defines no COMDAT or weak symbols and
  calls only allowlisted OS entry points. The pre-gate audit enumerates every TLS callback, `.CRT$XI*`,
  `.CRT$XC*` and `.CRT$XD*` entry and `_pRawDllMain`. SDK consumers are held to `avx2` by ADR-001a rule 1.
  This replaces the per-file AVX2 allowlist, which Jolt's inline AVX headers made unworkable.

## ADR-012 Quality bar & process
- CI: the ADR-001a PR matrix (primary and floor MSVC, clang-cl, Linux GCC/Clang, headless, MinGW) builds and
  runs unit tests, a headless server + bot smoke test, software-Vulkan render tests (lavapipe) with
  golden-image comparison, and Go service tests.
- Performance budgets are explicit and tested (frame time, tick time, bandwidth, memory).
- Iteration-time budgets (R05): hot reload ≤ 2 s for scripts/data/shaders; editor open ≤ 10 s.
- IP hygiene: no code from leaked SWG source or AGPL/GPL projects; architecture learned only
  from public descriptions.

## ADR-013 Runtime libraries (R10 manifest)
| Area | Choice | Notes |
|---|---|---|
| Physics | Jolt **5.6.0**, `JPH_DOUBLE_PRECISION` + **`JPH_CROSS_PLATFORM_DETERMINISTIC`** on client and server, AVX2 baseline (the launcher and each image's CPU gate check CPUID; ADR-011) | CI golden-hash test on every determinism toolchain: both MSVC toolsets, clang-cl, GCC, Clang and MinGW (02 RT-03). Ragdolls and cloth are client-only, in a cosmetic `PhysicsSystem` outside prediction; cloth = Jolt soft bodies with skinned constraints in Phase 4, bone-chain secondary motion before that; no GPU cloth (02 §7.1) |
| Animation | **ozz-animation 0.17** (runtime sampling/blending/IK + offline builders) | motion matching/IK extensions are Helios code |
| Navigation | **Recast/Detour 1.6** per physics grid, incl. moving ship interiors | |
| Audio | **miniaudio** device/mixing + Helios audio event system (banks, buses, 3D attenuation, occlusion); Wwise/FMOD only as optional proprietary plugins | Steam Audio (Apache-2.0) optional later for HRTF; **libopus** (BSD-3) for VO/music cooking and voice chat, approved in 09 §3.2 #6 |
| Game UI | **RmlUi 6.3** (HTML/CSS-like, data binding) + FreeType + HarfBuzz (+ SheenBidi, libunibreak) | ImGui stays editor/debug-only |
| Text content | **JSONC** (canonical key order, one entity per file) parsed with **yyjson** | Go reads via hujson |
| Transport | **netcode v1.4.8 + reliable v1.4.5** (connect tokens, encrypted UDP, reliability/fragmentation) wrapped by Helios channels/replication | Monocypher 4.0.3 for Ed25519 manifest signing |
| Cell ↔ services | **nats.c v3.14** from C++ (cells, gateways, and the editor for T30 collaboration, 07 §1.8); schema-codegen binary payloads | no protobuf/gRPC in C++ |
| Crash reporting | **sentry-native 0.17.1 (crashpad)**, self-hostable backend | Phase 2 |
| Textures | basis_universal (KTX2/UASTC), bc7enc_rdo (tools), tinyexr | |
| Import | cgltf, **ufbx** (FBX), msdfgen (UI/SDF fonts) | tools only |
| Profiling | **Tracy 0.14.1** (client+viewer versions must match) | |

## ADR-014 Backend toolchain (R10 §11)
- **Go 1.27.1** (1.24 is out of support). Libraries: pgx, go-redis (Valkey), nats.go + embedded
  nats-server, **connect-go** (Go↔Go RPC, protobuf), goose migrations, golang-jwt, argon2id,
  OpenTelemetry. No CGO.
- **Local dev on Windows without Docker: embedded-postgres** (real PostgreSQL, one SQL dialect —
  supersedes the SQLite idea in ADR-008), embedded NATS, **miniredis** for Valkey. SQLite only for
  tools and offline caches.

## ADR-015 Voice chat
- **Opus over HTP through a Helios forwarder** (Phase 3). Clients send 20 kbit/s Opus (20 ms frames, two per
  datagram, FEC, DTX) on a VOICE channel to their gateway on UDP 7777. Gateways relay to `helios-voice`, a C++
  SFU-style forwarder (no decoding, mixing or transcoding) that owns party, fleet, org, ship-intercom and
  proximity channels. Proximity and intercom audiences come from the owning cell's interest and AG trees. There
  is one transport, one public port and one crypto stack. Budgets, routing, moderation and acceptance (NS-3.9)
  are in 04 §2.5, §2.7 and §11.4.
- **Rejected:** a self-hosted WebRTC SFU such as LiveKit (Apache-2.0). The native client would need libwebrtc
  (large, hard to build `/MT`) and a second ICE/TURN/crypto stack, and proximity membership would still come
  from cells. It stays an option for web companion apps (Phase 5). Vivox- or Discord-class SDKs are allowed only
  as optional proprietary plugins.
- **Moderation:** mute and block are enforced server-side; a report snapshots a 60 s in-memory ring; there is no
  continuous recording. Automated moderation is a Phase 4 decision after legal review.

## ADR-016 Dev versus shipping link model (game-module hot reload)
- **Shipping** (`HELIOS_MODULAR=OFF`): every engine module and gem is linked statically into one image, with
  `/MT` (ADR-011), `/OPT:REF /OPT:ICF` and `-fvisibility=hidden`. Gem entry points are called from a generated
  `static_modules.cpp`, never from static initializers. The client, launcher, cells, gateway, voice forwarder
  and server-side tools ship this way. The SDK's editor, `helios-tool`, `helios-assetd` and `helios-cook` are
  dev-flavour builds, because they load a project's editor modules (07 §1.10).
- **Dev** (`HELIOS_MODULAR=ON`, presets **`windows-msvc-dev`** and **`linux-dev`**, plus the IDE presets
  `windows-vs2026` and `windows-vs2022`, ADR-001a rule 5): modules are combined into
  three shared libraries, one per link group (`helios_runtime`, `helios_client`, `helios_editor`). They export
  through `HELIOS_*_API` macros (dllexport/dllimport or visibility "default"). Each third-party library and each
  process singleton (type registry, component ids, memory tags, CVars, jobs, mimalloc heaps, Jolt `Factory`,
  Tracy) lives in exactly one image. Reloadable gem modules build as `game_<gem>[_client|_edcore|_edui]` DLLs/`.so`s
  against those libraries (the editor kinds: 07 §1.10).
- **CRT:** `/MD` for every image in a dev build, third-party libraries included. This gives one CRT heap, so
  STL objects may cross the boundary without a global `operator new` override (ADR-011 still forbids one).
  The packaged artist editor ships the VC++ runtime DLLs app-local, taken from the newer of the SDK's validated
  redistributable and the project toolset's (ADR-001a rule 3); the developer SDK uses the system-wide runtime.
- **Dev link:** `/INCREMENTAL /OPT:NOREF /OPT:NOICF /DEBUG:FULL` on MSVC. On Linux, lld or mold, `-Wl,-z,defs`
  and `-gsplit-dwarf`; GCC game modules add `-fno-gnu-unique`.
- **Reload protocol and rules** (02 §1.4): stage a uniquely named copy, load it beside the old image, swap at a
  frame or tick boundary, migrate changed component layouts through tagged serialization, then roll back the
  old image's registration scope and unload it. Reloadable code keeps no mutable statics, has no vtables or
  function pointers in components and no `thread_local` destructors, and passes a CI symbol audit.
- **Gate:** the Phase 0 spike WP-0.6c (02 RT-18) must pass before RT-14 and AAA-ITR-5 are claimed. If it
  fails, iteration falls back to snapshot-and-restart with the same ≤ 30 s budget, and this ADR is reopened.
- **Rejected:** (a) a game DLL that links its own static copy of the engine, which splits singletons and heaps;
  (b) `/MT` with a replaceable global `operator new/delete` that forwards to the engine heap, which ADR-011
  forbids and which still leaves `malloc`/`free` and CRT state split; (c) a C-ABI `EngineServices` table for
  in-house gameplay code, which is too narrow for the ECS and reflection APIs. It is kept for Phase 5 native
  mods.
