# Helios

**Helios** is a sci-fi MMO engine being built as one integrated product: the engine runtime, an editor
with a full content tool suite, the game client, a launcher/patcher, dedicated simulation servers, and a
modern MMO backend. The target is a AAA-grade engine capable of building games in the vein of *Star Wars
Galaxies*, *EVE Online*, *Destiny*, *SWTOR* and *Star Citizen*: seamless space ↔ planet ↔ interior play,
player economies, crafting, large-scale combat and story content.

Windows is the primary platform for development and play. Linux is fully supported and is the production
OS for dedicated servers.

> **Status: Phase 0 (Foundations).** The master plan is complete and independently reviewed. The
> foundation modules below are implemented, adversarially reviewed and tested. The game client, editor and
> launcher applications do not exist yet: they are scheduled for Phase 0–1 in the
> [roadmap](docs/plan/09-roadmap-and-process.md). Helios is far from the AAA bar today; the plan says what
> reaching it takes.

---

## The plan

| Document | What it covers |
|---|---|
| [`docs/PLAN.md`](docs/PLAN.md) | **Start here.** Executive summary, architecture, decisions, roadmap, AAA scorecard, risks |
| [`docs/plan/00-decisions.md`](docs/plan/00-decisions.md) | Architecture Decision Record (binding) |
| [`docs/plan/01-vision-and-scope.md`](docs/plan/01-vision-and-scope.md) | Target game archetypes, the measurable "AAA bar", scope, glossary |
| [`docs/plan/02-engine-runtime.md`](docs/plan/02-engine-runtime.md) | Core, ECS, schema/reflection, large-world frames, streaming, assets, physics, animation, scripting, UI |
| [`docs/plan/03-rendering.md`](docs/plan/03-rendering.md) | Vulkan RHI, render graph, GPU-driven rendering, planets/atmosphere/space, VFX, post |
| [`docs/plan/04-networking-and-servers.md`](docs/plan/04-networking-and-servers.md) | Transport, gateway, cell servers, replication, authority handoff, server meshing |
| [`docs/plan/05-backend-services.md`](docs/plan/05-backend-services.md) | Go services, ledger/market, persistence, ops, patching/CDN |
| [`docs/plan/06-gameplay-framework.md`](docs/plan/06-gameplay-framework.md) | Data-driven gameplay kernel: abilities, items, crafting, economy, missions, AI, flight |
| [`docs/plan/07-editor-and-tools.md`](docs/plan/07-editor-and-tools.md) | The editor and its 30 tools (level, planet, data, scripting, quests, dialogue, animation, …) |
| [`docs/plan/08-client-and-launcher.md`](docs/plan/08-client-and-launcher.md) | Game client, game UI, launcher/patcher, installer, crash reporting |
| [`docs/plan/09-roadmap-and-process.md`](docs/plan/09-roadmap-and-process.md) | Phases 0–5, work packages, build process, testing, risk register, current status |
| [`docs/research/`](docs/research/) | Ten research reports: EVE Online, SWG/SWGEmu, SWTOR/HeroEngine, Star Citizen, Destiny, Unreal/Godot, MMO backends, editors, sci-fi gameplay & graphics, tech selection |

The plan was scored by three independent expert reviewers (server/backend, engine/rendering,
tools/production) against the goal above over five review/revise rounds, finishing at **9.2 / 9.0 / 9.0**.

## Architecture at a glance

```
 Launcher ──HTTPS──▶ Go services (identity, sessions, orchestrator, ledger, market, chat, …)
    │                     │  PostgreSQL · Valkey · NATS JetStream
    ▼                     │
 Client ══encrypted UDP══▶ Gateway (C++) ══trunks══▶ Cell servers (C++, headless engine)
                                                     zones on dilatable clocks, authority groups,
                                                     epoch-fenced handoff between cells
 Editor ── live edit instance / Play-in-Editor (local cell + gateway + backend + clients)
```

- **Engine:** C++20, Vulkan 1.3 (volk + VMA, bindless, render graph), SDL3, flecs ECS, Jolt Physics
  (double precision, cross-platform deterministic), Luau scripting, Slang shaders, one `.hschema` schema
  language that generates C++, Go and editor code.
- **Large worlds:** nested reference frames with double-precision positions, camera-relative reverse-Z
  rendering, one physics system per grid (planet surface, ship interior, station).
- **Backend:** Go 1.27 services; PostgreSQL is the system of record; local development runs everything in
  one executable with no Docker.

## Repository layout

```
engine/        engine modules (engine/<module>/include/helios/<module>, src/, tests/)
apps/          executables: cellserver, gateway, samples (client, editor, launcher to come)
services/      Go backend module (cmd/helios-backend, internal/, pkg/)
tools/         schemac (schema compiler), shaderc, rendertest, lint, CI and vendoring scripts
schemas/       *.hschema data definitions
shaders/       Slang shaders
third_party/   vendored dependencies (pinned, permissive licences; see MANIFEST.md)
docs/          master plan, research, ADRs
```

### Module status

| Module | What it is | Status |
|---|---|---|
| `engine/core` | Platform layer, memory, jobs, files/VFS, cvars, crash handling, process spawn, CPU gate | Done for Phase 0 |
| `engine/math` | f32/f64 math, reference frames, packing, deterministic transcendentals and noise, fixed point | Done for Phase 0 |
| `tools/schemac`, `engine/reflect` | `.hschema` compiler (C++ and Go emitters) and runtime reflection | Done for Phase 0 |
| `engine/ecs` | flecs integration, entity IDs, relationships, dirty tracking | Done; ECS benchmark fails the structural-ops budget ([ADR-004a](docs/adr/)) |
| `engine/rhi` | Vulkan 1.3 RHI + Null backend | Done for Phase 0 |
| `engine/render`, `tools/shaderc`, `tools/rendertest` | Render graph v0, shader compiler, golden-image testing | Done for Phase 0 |
| `engine/net` | Encrypted UDP transport (netcode + reliable), channels, NetSim | Done for Phase 0 |
| `engine/script` | Luau host: sandbox, fuel budgets, scheduler | Done; two budget items need a Luau patch |
| `engine/server`, `engine/authority`, `apps/cellserver`, `apps/gateway` | Cell and gateway skeletons | In progress |
| `engine/gameplay`, `engine/hxl` | Gameplay kernel and HXL formulas | In progress |
| `services/` | Go backend skeleton: identity, sessions, connect tokens, orchestrator | Done for Phase 0 |
| Client, editor, launcher, assets, physics, animation, audio, game UI | — | Not started (see roadmap) |

## Building on Windows

**Prerequisites**
- Visual Studio 2022 (17.14 or later) or Visual Studio 2026 with the *Desktop development with C++*
  workload (includes CMake and Ninja). Add *C++ Clang tools* for the clang-cl preset.
- Git.
- A GPU driver with Vulkan 1.3 support (current NVIDIA, AMD and Intel drivers). No Vulkan SDK is needed.
- Go 1.27.1 for the backend (`services/`).
- Internet access the first time you configure: the pinned Slang shader compiler is downloaded and
  SHA-256 verified.

**Build and test** (from an *x64 Native Tools Command Prompt for VS*):

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

Other presets: `windows-msvc-debug`, `windows-clang-cl`, and `windows-vs2022` / `windows-vs2026`, which
generate a Visual Studio solution under `build\<preset>\`.

**Run the sample:** `build\windows-msvc-release\bin\rhi_triangle.exe`.

**Run the backend** (no Docker needed; embedded PostgreSQL, NATS and a Redis-compatible store):

```bat
cd services
go run ./cmd/helios-backend --seed dev
```

See [`services/README.md`](services/README.md) for ports, flags and the Docker Compose setup.

## Building on Linux

Ubuntu 24.04 or similar, with GCC 13+ or Clang 17+, CMake 3.28+, Ninja, and the X11 development packages
listed in [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

```sh
cmake --preset linux-gcc          # or linux-clang, linux-debug-asan
cmake --build --preset linux-gcc
ctest --preset linux-gcc          # GPU tests run on any Vulkan driver, including Mesa lavapipe
```

- `linux-headless` builds only the dedicated servers and tools.
- `cross-mingw` cross-compiles the Windows code paths as a portability check.

## Continuous integration

Every push runs [CI](.github/workflows/ci.yml) on these jobs:
- Windows MSVC: VS 2026 primary and the VS 2022 (MSVC 14.44) floor;
- Windows clang-cl;
- Linux GCC and Linux Clang, with software-Vulkan GPU tests;
- a headless server build;
- a MinGW cross-build;
- Go tests on Windows and Linux, plus a PostgreSQL-backed integration suite.

A [nightly workflow](.github/workflows/nightly.yml) adds sanitizers and full Visual Studio solution builds.

## Contributing

[`CLAUDE.md`](CLAUDE.md) holds the conventions for every contributor, human or agent:
- platform rules and the build commands;
- module layering, which configure enforces;
- code style;
- the tests each change needs;
- licence and IP hygiene. No code from the leaked SWG source or from GPL/AGPL projects, and no
  third-party game IP in content.

## Licence

No licence has been chosen for Helios itself yet. Vendored third-party components keep their own licences,
recorded in [`third_party/MANIFEST.md`](third_party/MANIFEST.md).
