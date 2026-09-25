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
| 09-roadmap-and-process.md | Phased roadmap, exit criteria, developer-experience track (docs, SDK, upgrades, starter templates), sponsor budget and funding gates, build-loop process, testing, risks |
| CONSISTENCY.md | Log of cross-section consistency fixes |
| _integration-notes.md | Cross-section issues raised by section authors (all resolved; see CONSISTENCY.md) |

## Phase vocabulary (used by every section)
- **Phase 0 — Foundations**: build/CI, core runtime, schema compiler, RHI bring-up, headless cell server skeleton, Go backend skeleton, launcher skeleton.
- **Phase 1 — First Light (vertical slice)**: one star system (star, planet, station); fly a ship, land, walk, shoot NPCs; ~50 players; editor places entities/edits records/PIE; login via launcher; persistence; core ledger for starter loadouts and currency (no player-to-player trading).
- **Phase 2 — Alpha Sandbox**: full inventory ledger and trading, crafting & resources, market, missions/quests/dialogue, AI, chat/guilds; P0 editor tools complete; multiple zones; 500 players per zone; content pipeline & chunk patching; binary SDK, New Project and project upgrades (09 §2.7); project schema types with no compiler (02 §3.8); ground vehicles and mounts (06 §8.1a); editor extension API preview (07 §1.10); product-branded packaging (08 §2.10).
- **Phase 3 — Beta Scale**: static multi-cell zones & handoff, co-location of coupled entities across cell seams (04 §6.2a), planned region migration for rolling restarts and drains (04 §6.7), 5k CCU shard, instancing/phasing, activities with matchmaking, group finder and PvP match rules (06 §6.6–6.13), player fleets (05 §1.12.1, 06 §8.3a), EVA, housing and player cities (the Foundation `CityGovernance` world script; 06 §9.2), territory control with reinforcement timers (06 §9.3), voice chat (ADR-015), player reports and support cases (05 §1.17, 08 §1.7.2), animation suite, VFX editor, UI designer, live collaborative editing, multi-cell PIE, stable editor extension API and world-script API (05 §1.23) with world scripts in PIE and the editor (07 §1.6.2), three starter templates.
- **Phase 4 — Launch Quality**: AAA render features (visibility buffer, virtual shadows, HDR10; volumetric clouds and TAA/upscaling, which land earlier with per-commit goldens, reach their AAA-REN-6 completeness gate), perf/memory budgets met, security hardening, live-ops tooling and the in-game store (05 §1.20), facial animation and lip-sync (02 §7.2), 50k CCU per shard, sovereignty (06 §9.3), the replicant state tier for every persistent world zone (a cell crash loses ≤ 1 s of state; ADR-007), gateway deploys by make-before-break session relocation (05 §6.3.1), all five starter templates, and the human-judged gates: an external look-and-feel panel (AAA-REN-8) and a content team building a zone with the shipped tools (AAA-TOOL-10).
- **Phase 5 — Ambition**: dynamic server meshing, gateway replication layer on the replicant tier (≤ 1 s crash hitch), 100k+ single shard, seamless galaxy travel, sandboxed player scripting, ray-traced effects.
