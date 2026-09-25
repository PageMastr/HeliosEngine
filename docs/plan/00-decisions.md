# Helios — Architecture Decision Record (ADR index)

Status: **accepted** unless marked *open*. Every plan section and every implementation task must
conform to these decisions; changing one requires a new ADR entry with rationale.
Research backing lives in `docs/research/01..10-*.md` (cited as R01..R10).

## ADR-001 Platforms
- **Windows 10/11 x64 is the primary development and play platform** (user requirement).
  Toolchains: MSVC 2022 (primary), clang-cl. Visual Studio 2022 solution via CMake preset.
- **Linux x64 is fully supported**: GCC 13+ / Clang 17+. Linux is the production OS for dedicated
  servers; Windows can run every server locally for development.
- Every commit builds on Windows MSVC and Linux GCC/Clang in CI (GitHub Actions), plus a MinGW
  cross-compile portability check that can run inside Linux containers.
- macOS/consoles are non-goals for now; the RHI seam keeps them possible (R01, R06).

## ADR-002 Languages
- **C++20** for engine runtime, editor, client, launcher, zone/cell servers, gateway and tools.
- **Go 1.27** for control-plane / backend services (no CGO, so they build natively on Windows; see ADR-014).
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
- Terminology: "**ECS archetype**" means component storage only; designer-facing content types
  are "**record templates**" (never "archetype") across all plan sections.
- **One schema language** (`*.hschema`, compiled by `helios-schemac`, a C++ tool built first in
  the build) generates: C++ types + reflection + binary/text serializers + editor metadata + Luau
  bindings + **replication descriptors** (quantization, audience: all/owner/server-only, R02);
  Go structs + wire codecs; SQL migration stubs; typed-record (DataForge-like) tables (R03, R04).
- Stable 64-bit IDs everywhere (entity IDs Snowflake-style; content IDs = GUID / path hash).

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
  **v1** static multi-cell zones with handoff p99 < 100 ms → **v2** dynamic split/merge +
  gateway-side replication layer. No SpatialOS-style generality up front (R07).
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

## ADR-009 Editor
- ImGui docking + multi-viewport, on top of a UI-less **ToolsFramework** (documents, selection,
  commands, reflection-based property-diff transactions, asset DB client, automation) (R06).
- Editor runs the real renderer and simulation; **Play-in-Editor spawns a local cell server +
  N clients** (and bot clients).
- **Live collaborative editing** against a running dev server using a HeroEngine-style **edit
  instance** (only the edit instance persists changes; play instances are pinned to published
  content versions) (R03).
- Tool suite T01–T30 per R08 §3 (world, prefabs, terrain/planet, star-system/galaxy, data &
  archetypes, gameplay systems, script IDE + debugger, visual graphs, quest, dialogue, sequencer,
  animation, material graph, VFX, environment, UI designer, audio, modular ship/base assembly,
  character customization, AI/nav/spawn, asset browser, localization, profiling, GM/live-ops,
  validation, build/cook/deploy, collaboration).

## ADR-010 Client, game UI & launcher
- Client = same engine runtime + game UI + netcode; one continuous camera across cockpit/FPS.
- **Game UI** is a retained-mode, data-bound runtime UI system (renders to screen or to texture
  for diegetic screens). ImGui is editor/debug-only (R04).
- **Launcher** (C++, Windows-first): login, news, content-addressed chunk patching from CDN
  manifests, verification/repair, self-update; code-signing and installer per R10.

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
- **MSVC runtime: static `/MT`** everywhere (no VC++ redist for players); `/Z7` debug info so
  compiler caches work; CMake ≥ 3.28.
- Tagged memory with budgets; frame/tick linear allocators; generational handles across
  subsystem boundaries.

## ADR-013 Runtime libraries (R10 manifest)
| Area | Choice | Notes |
|---|---|---|
| Physics | Jolt **5.6.0**, `JPH_DOUBLE_PRECISION` + **`JPH_CROSS_PLATFORM_DETERMINISTIC`** on client and server, AVX2 baseline (launcher checks CPUID) | CI golden-hash test across MSVC/clang-cl/GCC/Clang |
| Animation | **ozz-animation 0.17** (runtime sampling/blending/IK + offline builders) | motion matching/IK extensions are Helios code |
| Navigation | **Recast/Detour 1.6** per physics grid, incl. moving ship interiors | |
| Audio | **miniaudio** device/mixing + Helios audio event system (banks, buses, 3D attenuation, occlusion); Wwise/FMOD only as optional proprietary plugins | Steam Audio (Apache-2.0) optional later for HRTF |
| Game UI | **RmlUi 6.x** (HTML/CSS-like, data binding) + FreeType + HarfBuzz (+ SheenBidi, libunibreak) | ImGui stays editor/debug-only |
| Text content | **JSONC** (canonical key order, one entity per file) parsed with **yyjson** | Go reads via hujson |
| Transport | **netcode v1.4.8 + reliable v1.4.5** (connect tokens, encrypted UDP, reliability/fragmentation) wrapped by Helios channels/replication | Monocypher 4.0.3 for Ed25519 manifest signing |
| Cell ↔ services | **nats.c v3.14** from C++; schema-codegen binary payloads | no protobuf/gRPC in C++ |
| Crash reporting | **sentry-native 0.17 (crashpad)**, self-hostable backend | Phase 2 |
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

## ADR-012 Quality bar & process
- CI: Windows MSVC + Linux GCC/Clang build, unit tests, headless server + bot smoke test,
  software-Vulkan render tests (lavapipe) with golden-image comparison, Go service tests.
- Performance budgets are explicit and tested (frame time, tick time, bandwidth, memory).
- Iteration-time budgets (R05): hot reload ≤ 2 s for scripts/data/shaders; editor open ≤ 10 s.
- IP hygiene: no code from leaked SWG source or AGPL/GPL projects; architecture learned only
  from public descriptions.
