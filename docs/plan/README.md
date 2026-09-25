# Helios master plan — section index

The master plan is `docs/PLAN.md`. Detailed sections live here:

| File | Section |
|---|---|
| 00-decisions.md | Architecture Decision Record (binding) |
| 01-vision-and-scope.md | Vision, target games, definition of the AAA bar, scope, non-goals |
| 02-engine-runtime.md | Core runtime, ECS, schema/reflection, world model, streaming, assets, physics, animation, audio, scripting, game-UI runtime (`engine/ui`, `engine/text`) |
| 03-rendering.md | RHI, render graph, GPU-driven pipeline, planets/space/atmosphere, VFX, post |
| 04-networking-and-servers.md | Transport, gateway, cell servers, replication, authority/handoff, instancing, anti-cheat |
| 05-backend-services.md | Go services, data stores, ledger/market, persistence, ops, patch/CDN |
| 06-gameplay-framework.md | Data-driven gameplay kernel and sci-fi MMO systems |
| 07-editor-and-tools.md | ToolsFramework and the full editor tool suite |
| 08-client-and-launcher.md | Game client, HUD and screens on `engine/ui`, launcher/patcher, installer, crash reporting |
| 09-roadmap-and-process.md | Phased roadmap, exit criteria, build-loop process, testing, risks |
| CONSISTENCY.md | Log of cross-section consistency fixes |
| _integration-notes.md | Cross-section issues raised by section authors (all resolved; see CONSISTENCY.md) |

## Phase vocabulary (used by every section)
- **Phase 0 — Foundations**: build/CI, core runtime, schema compiler, RHI bring-up, headless cell server skeleton, Go backend skeleton, launcher skeleton.
- **Phase 1 — First Light (vertical slice)**: one star system (star, planet, station); fly a ship, land, walk, shoot NPCs; ~50 players; editor places entities/edits records/PIE; login via launcher; persistence; core ledger for starter loadouts and currency (no player-to-player trading).
- **Phase 2 — Alpha Sandbox**: full inventory ledger and trading, crafting & resources, market, missions/quests/dialogue, AI, chat/guilds; P0 editor tools complete; multiple zones; 500 players per zone; content pipeline & chunk patching.
- **Phase 3 — Beta Scale**: static multi-cell zones & handoff, 5k CCU shard, instancing/phasing, housing/cities, territory control, animation suite, VFX editor, UI designer, live collaborative editing.
- **Phase 4 — Launch Quality**: AAA render features (visibility buffer, virtual shadows, HDR10; volumetric clouds and TAA/upscaling, which land earlier, become golden-gated per AAA-REN-6), perf/memory budgets met, security hardening, live-ops tooling, 20–50k CCU per shard.
- **Phase 5 — Ambition**: dynamic server meshing, gateway replication layer, 100k+ single shard, seamless galaxy travel, sandboxed player scripting, ray-traced effects.
