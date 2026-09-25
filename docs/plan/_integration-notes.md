# Cross-section issues reported by section authors (for the integrator)

> **Status (2026-09-25):** every item below is resolved in the owning section or in 09 §3.2. The fixes are
> logged in [CONSISTENCY.md](CONSISTENCY.md). This file is kept as the historical record.

## From 01 (vision)
- "archetype" overloaded: use "ECS archetype" for storage only; "record template" for designer content (ADR-004 now says this). 05/06 must follow.
- Sections 02–08 should cite 01's capability IDs (§2.6); 03/08/09 use benchmark scenes BENCH-1..6 and hardware tiers; 09 uses the AAA scorecard as the build-loop termination condition.
- Aligned targets with 04: 2,000-ship battle at TiDi 10% in Phase 4; cell-crash hitch ≤ 1 s in Phase 5; rewind cap 200 ms.
- ADR gaps (now filled by ADR-013): audio, runtime UI, Recast, ozz. 07 should decide on web-based narrative/localization tools (R03).
- Patent reviews (SWG terrain patents before Phase 4; Improbable US 11,792,306 before Phase 5 replication layer) → 09 risk register.

## From 06 (gameplay)
- 02: .hschema needs client{}/server{} blocks, @store, tag + HXL cook steps; kernel needs strict FP math, WorldPos Luau userdata, Luau CPU budgets.
- 04: owner channel for prediction accept/reject, hitbox history ≥ 500 ms (rewind cap 200 ms), phase filtering, idempotent ApplyDamage/ApplyEffect. (04 says done.)
- 05: Go HXL package, world-state flags service, Economy Sim service, crafting escrow refund timers.
- README phase vocabulary puts inventory ledger in Phase 2, but 05 has a core ledger in Phase 1 → 09 must reconcile (Phase 1 = core ledger for starter loadouts/currency; Phase 2 = full trading/market).
- 07: graph compilers with rollback-safety checks, loot/drop simulators, "view as state" debugger.
- 06 still uses "archetype" for designer content types → change to "record template".

## From 04 (networking)
- 05: reconnect via Session service ticket exchange (not gateway-minted); per-instance netcode keys → rotation by draining; orchestrator mints trunk tokens. (05 says aligned.)
- 02: generated Mut<C> mutators push per-field dirty bits; 64-bit EntityId ↔ 32-bit wire handle map; ScriptState components; frame-local hitbox history.
- 06: cross-cell writes only via send_effect(); predicted abilities use prediction keys; damage/loot never predicted.
- Open decision: netcode lacks forward secrecy → X25519 re-key proposal for Phase 4 security review (→ 09 risks).

## From 05 (backend)
- 04: gateway SealReconnectTickets (batched), client HTTPS Reconnect(ticket), orchestrator MintTrunkToken, shard key rotation by draining.
- 02: .hschema service blocks, ledger_policy, lifecycle rules, ReasonCodeDef, table annotations; schemac emits .proto + Go/C++ binary codecs + Go NATS binding; Snowflake 41/5/8/9 bits.
- 08: launch codes (not refresh tokens), SSE queue status, BLAKE2b-256 chunk verification, install tiers, rollout_pct, patch-from, anti-rollback, install journal.
- R10 suggested an offline SQLite fallback; ADR-014 single dialect wins; offline machines use `pg-install --from <zip>`.

## From 08 (client & launcher)
- 02: Jolt AVX2 flags must not leak into the client's CPU-gate TU (launcher/CPU check compiled without AVX2); paks ≤ 2 GiB with group/tier/language tags; VFS residency hook; README lists "game UI" in both 02 and 08 → 02 owns the UI runtime, 08 owns client HUD/screens.
- 05: verify criterion A12 (60 GB < 10 min) weaker than AAA-CNT-7 → align to 01; patch pointer needs next{}, min_client, CDN list; settings blob; crashgw; bot credentials; shared FastCDC test vectors.
- 04: §1 still mentions SQLite for dev → must say embedded-postgres (ADR-014); add device_class input field.
- ADR: ADR-013/010 say ImGui for launcher UI? 08 decides RmlUi on SDL_Renderer for the launcher → update ADR-010. Fonts: allow-list SIL OFL-1.1 for font assets.
- 07: T19 UI designer targets engine/ui; PIE uses apps/client.
- 09: start HSM code-signing cert procurement in Phase 1; legal sign-offs (FreeType, Inno Setup, EOS, Sentry FSL) in risk register.

## From 03 (rendering)
- 02: grid-local instance transforms + one camera-relative transform per visible grid (NOT per-object camera-relative matrices); fixed-point deterministic terrain noise with a Slang twin (GPU generates all terrain LODs); Reparent() frame-change event; Gerstner-only buoyancy; portal-graph + crowd LOD contracts. (Sent to 02 author.)
- 07: T16 material graph enforces PSO budgets (closed set of ~12 shading models); T17 VFX compiles to bounded kernels; 03 provides editor passes, thumbnails, ImGui backend.
- 08: RmlUi render interface, HDR settings, PSO warm-up + benchmark UI, bracket content.
- D3D12: Phase 3 test backend, Phase 4 gate with 4 triggers.

## From 07 (editor & tools)
- 02: @keyed list elements (stable per-element GUIDs); record hash IDs minted once & stored; EDITOR_ONLY module flag; schemac editor metadata + Go validators; DAP adapter in script host. (Sent to 02 author.)
- 03: editor passes (ID buffer, outlines, grid), ImGui backend on RHI, offscreen thumbnails, Slang material-interface contract; VFX editor is in-house (Effekseer rejected). (03 says provided.)
- 05: add a collab service + overlay content versions. 04 vs 05 gateway port mismatch (7777 vs 27015) → pick one (use 7777/udp for gateway game traffic). 05's "SQLite dev DB" mentions conflict ADR-014 → remove.
- 09/R10: AAA-TOOL-3 marks T15 complete in Ph3 but facial animation (R04 capability) is Ph4 → reconcile. imgui-node-editor not vendored yet (vendor when T11 starts, or use ImGuizmo GraphEditor). nats.c allowed in editor. libgit2 rejected (license) → git CLI for source control.

## From 02 (engine runtime)
- 03: co-own hnoise.slang + test corpus; RenderScene defined in `render`, filled by `presentation`.
- 04: schema snippets parse because `@` is optional in declaration headers (fine).
- 05: claims 05 still says SQLite for dev — lead checked: 05 says "No SQLite fallback for services" (OK). Verify no stale SQLite mentions anywhere.
- 07: 07's "planetgen" == `engine/pcg` → use `engine/pcg` everywhere.
- 09: approve libopus (voice chat, BSD-3); extend HELIOS_MODULE_ORDER + LAYER/EDITOR_ONLY CMake checks; Phase 0 spikes for mimalloc heap thread-affinity and flecs DontFragment.
