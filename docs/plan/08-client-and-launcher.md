# 08 — Game Client and Launcher

*Helios master plan, section 08. Draft v1. Conforms to ADR-001/002/004/006/007/010/011/013/014; phases per
`README.md`. Citations: `R10-§9` = report 10 §9; `R07-P0-11` = report 07 requirement P0-11. 04 owns wire and
token formats and 05 the patch service; this section owns the client and launcher sides.*

**Capabilities (01 §2.6):** owns **R06** (data-bound game UI, diegetic MFDs, data grids; Ph1). Supplies the
client side of W03/W05 (streaming residency, seamless transitions), M05 (prediction hooks, per-device aim
assist), G06/G09/G11 (market, social, dialogue UI) and S08 (global handles).

---

## 0. Design rules

1. **The client is a view plus intents.** It predicts only what 04 §5 allows and never decides value
   (AAA-SEC-1).
2. **One runtime:** the engine plus `engine/clientcore`, `engine/ui` and netcode (ADR-010). Bots link the same
   core.
3. **UI never touches the ECS or GPU API.** It reads schema-generated view-models, writes intents, and draws
   only through `engine/ui` (R03 lesson 2, R03-P0-9).
4. **Nothing runs unverified:** an Ed25519 manifest chain covers every byte, Authenticode every PE (AAA-SEC-6).
5. **Every flow is automatable:** login, patching and crashes run headless in CI on Windows and Linux.

---

## 1. Client application

### 1.1 Startup sequence

| # | Step | Budget (REF) |
|---|---|---|
| 1 | **Launcher:** CPU/GPU gate → self-update → login → verify pointer and manifest → tier 0 → quick-verify executables (§2) | — |
| 2 | **Hand-off:** `CreateLaunchCode` (05 §1.1) returns a one-time 60 s code, passed to `helios-client` on an **inherited pipe** (Windows handle list, Linux fd 3), never on the command line | 0.2 s |
| 3 | **Boot:** CPU gate, crash handler, mimalloc, VFS, settings, SDL3 window, Vulkan 1.3 device, `VkPipelineCache`, RmlUi + fonts, audio | ≤ 2.5 s |
| 4 | **Exchange:** `ExchangeLaunchCode` returns a 10-min JWT and a refresh token kept **in memory only** | ≤ 0.3 s |
| 5 | **Character select:** shard and character lists; a 3D scene streamed from one small container | ≤ 2 s |
| 6 | **Queue:** `Enqueue(shard, character)`, with position and ETA over SSE (05 §1.2). The player may idle in character select | 0 s unqueued |
| 7 | **Session:** `CreateSession` returns a connect token (≤ 45 s, 04 §2.3). A `content_build` mismatch hands off to the launcher | ≤ 0.2 s |
| 8 | **Connect:** HTP handshake (1.5 RTT), ACCEPT, placement, `ZoneChange` | ≤ 0.3 s |
| 9 | **Loading:** stream the **arrival set** (the player's authority-group containers plus a radius around them, and the zone's PSO precache list). `InWorld` starts once it is resident and the first owner snapshot has been applied | ≤ 8 s |

**No launch code** (dev builds; Steam later): the client shows the same RML login screen and fetches the channel
pointer itself. A stale build hands off to the launcher (`--update`).

### 1.2 State machine

`ClientStateMachine` is a stack on the game thread, so overlays (menu, reconnect, cinematic) suspend input
without tearing down the world. Transitions are table-driven, unit-tested, and each one is logged as a
breadcrumb and a telemetry event.

| State | Next | Timeout / failure |
|---|---|---|
| Boot | Login / CharacterSelect | Failed gate → message box, exit |
| Login (no code) | CharacterSelect | Backoff after 3 failures |
| CharacterSelect / Create | Queue | JWT refreshed every 8 min |
| Queue | Connecting | SSE drop → re-present the signed ticket |
| Connecting | Loading | 10 s per gateway; try all (≤ 8), then Disconnected |
| Loading | InWorld | 90 s → Disconnected (`stream_timeout`) |
| InWorld | ZoneTransition / Reconnecting / CharacterSelect | — |
| ZoneTransition | InWorld | World keeps rendering behind transit VFX; ≤ 3 s (AAA-SRV-5); missing downloads show progress (§2.6) |
| Reconnecting (overlay: 2 s without packets, 10 s linkdead per 04 §2.4, or OS resume) | InWorld / CharacterSelect | Reconnect token retried with backoff 0.5, 1, 2, 4 s… for ≤ 5 min |
| Disconnected | CharacterSelect / Login | Localized reason code |

### 1.3 Main loop and threading

| Thread | Work | Budget |
|---|---|---|
| **OS (main)** | `SDL_PumpEvents`, window, IME, cursor. Events go to the game thread through a lock-free queue. Main-thread-only calls go through `SDL_RunOnMainThread` | ≤ 0.2 ms |
| **Game** | Input actions; fixed-step prediction at the zone command rate (20–60 Hz, 04 §5.1); ECS jobs; client Luau (≤ 1 ms); view-models; RmlUi layout; **extract** | ≤ 8 ms at 60 fps |
| **Render** | Prepare, record and submit frame N−1 (ADR-003); **late-latches** camera orientation from the newest mouse delta | ≤ 4 ms |
| **Workers / I/O** | Job graph (flecs, Jolt); streaming reads; `StreamingInstaller` at lowest priority | — |
| **Net** | Socket I/O, AEAD, keepalives; keeps running through hitches and window drags | ≤ 5 % of a core |

The game thread is separate from the OS thread because Win32 modal loops (window drag or resize) would
otherwise stall prediction. Pacing: ≤ 2 frames in flight, a high-resolution waitable-timer limiter, VRR, and
MMCSS "Games" for the game and render threads. Reflex and Anti-Lag are optional plugins behind
`ILatencyProvider` (Phase 4).

### 1.4 Settings and configuration

- **Machine scope** (display, graphics, audio device) lives in
  `%LOCALAPPDATA%\Helios\<channel>\settings\machine.jsonc` (`$XDG_CONFIG_HOME/helios` on Linux).
- **Account scope** (keybinds, UI layout, chat tabs, addon settings) is a ≤ 256 KiB blob synced through the
  Character service, so it follows the player to another PC, as in WoW.
- **Cvars** are typed, with flags `archive`, `cheat` (compiled out of shipping), `restart`, `server_locked` and
  `dev`; `+cvar=value` overrides them.
- **Scalability:** `ScalabilityGroupDef` records define groups (view distance, shadows, textures, effects,
  post, foliage, clouds, crowds, UI render-to-texture rate) at levels 0–4, and presets combine them. Auto-detect
  uses a GPU/VRAM table plus a 3 s GPU probe. Also: dynamic resolution, the upscaler choice (03), and a cost
  hint for each setting.
- **Audio:** master, music, SFX, voice, UI and ambience buses. The output device follows the OS default.
  Dynamic-range presets, mono downmix, and a voice language separate from text.

### 1.5 Input

- **Records.** `InputActionDef` defines the value type (button or 1–3D axis), triggers (press, release, hold,
  tap, double-tap, chord) and modifiers (deadzone, curve, invert, sensitivity, smoothing). `InputContextDef`
  adds priority, `blocksLower`, `cursorMode` and `allowInTextEntry`.
- **Context stack:** Frontend, ModalUI, TextEntry, OnFoot, Vehicle, Pilot, Turret, Engineering, Map, Build. A
  seat pushes one context per granted **input channel** (06 §8.4).
- **Bindings** layer as record defaults → device profile → account overrides. Rebinding captures the next
  input and flags conflicts among contexts that can be active together.
- **Mouse:** mouse-look uses SDL3 relative raw input. The context sets cursor mode, with a hold (Alt) or toggle
  key to free the cursor, which is confined when windowed. Sensitivity is in cm/360°. Flight offers a
  virtual-joystick reticle or rate mode.
- **Gamepad:** SDL3 HIDAPI; glyphs from `SDL_GetGamepadType`; rumble and trigger rumble from cue records
  (`HapticDef`, 06 §1.5); gyro aim in Phase 4; spatial UI focus navigation.
- **HOTAS:** per-device curves, with axes keyed by joystick GUID.
- **Aim-assist class:** inputs carry `device_class` (mouse, pad or HOTAS), so the server applies the matching
  `AimAssistDef` (06 §8.6, R05-P0-9). The class can change only after 1 s without aim input, and the server
  flags implausible mixes.
- **IME:** a focused text field calls `SDL_StartTextInputWithProperties` and `SDL_SetTextInputArea` with the
  caret rectangle, so the candidate window sits beside the text. Preedit text renders inline. Text input stops
  on blur, so the IME never swallows WASD (R10-§8).

### 1.6 One continuous camera

A `CameraRig` blends `CameraMode`s inside its anchor reference frame:

| Mode | Behaviour |
|---|---|
| FirstPerson | Head-bone anchored; seat entry is animation-driven with **no cut** (R04-P1-14) |
| Cockpit | Seat-anchored; free-look; clamped G-force head offset; separate view-model depth range and FOV (03, R09 B16) |
| OverShoulder / Chase | Spring arm with a sphere cast against the current grid's Jolt system |
| Orbit / Tactical | EVE-style orbit, 10 m–100 km, for command flight |
| Cinematic | Driven by `Cinematic.Play` or dialogue auto-staging (06 §6); 0.3–0.5 s blends |

The pose is a frame-local f64 `WorldPos` that follows `Reparent()` across grid transfers (ship → planet →
interior) and keeps its world pose; view matrices are camera-relative (ADR-003/005). The galaxy map is a separate
scene view. Comfort settings: shake and G-offset 0–100 %, head-bob, horizontal FOV 70–110° (Hor+), and a
separate cockpit FOV.

### 1.7 Game UI on RmlUi

**Stack** (ADR-013, R10-§12):
- **RmlUi 6.3** for documents, RCSS, data binding and animation.
- Our own `Rml::FontEngineInterface`, because the stock engine does not shape: FreeType rasterizes, HarfBuzz
  shapes, SheenBidi reorders, libunibreak breaks lines.
- An RHI render interface: one premultiplied sRGB pipeline, bindless textures, cached geometry, scissor/stencil
  clipping.
- Luau bindings generated by schemac, because RmlUi's Lua plugin targets stock Lua.

```
*.hschema viewmodel ──schemac──► C++ struct + RmlUi data-model registration + Luau types
replicated ECS ─► ViewModel adapters (C++, dirty masks) ─► RmlUi data models ─► RML/RCSS documents
                                  Luau UI controllers (client VM) ──intents──► Net
```

**Foundation-layer panels** (01 §4.1; forkable RML + Luau):

| Panel | Key requirements | Ph |
|---|---|---|
| Frontend | Login; shard/character select over a 3D scene; queue ETA; handles `name#1234` (R03-P0-8) | 1 (creation 4) |
| HUD | Vitals; ability bar with predicted cooldowns; target; radar; ping/loss; **TiDi indicator when d < 0.95** (R01-P0-3) | 1 |
| Maps | System orrery from f64 positions; galaxy map (jump graph, routes) as a 3D view with an RML overlay | 1 / 2 |
| Chat, social | Tabs, IME, item links, bidi-isolated names; friends, party, rosters of 10k members | 2 |
| Inventory, market | Drag and drop with a pending state; fitting preview via the shared validator (06 §2); order-book grids | 2 |
| Dialogue | Letterbox, subtitles, choice wheel, group roll display, 30 s vote timer (06 §6) | 2 (group 3) |
| Overview | EVE-style sortable table of 1,000+ space objects | 3 |

**Rules:**
- Tables use a virtualized `<datagrid>` element that only creates the rows on screen.
- **Nameplates, brackets and damage numbers are not RmlUi documents.** `WorldMarkerRenderer` draws them as
  instanced quads with SDF text, projected from f64, with screen-space declutter and CPU picking (R09 B15).
- 2D UI is composited **after TAA/upscaling, at native resolution**, then mapped to HDR at a user paper-white
  level (R09 B16).
- RML, RCSS and Luau hot-reload in dev and in the T19 UI designer (R08-T19).

### 1.8 Diegetic and world-space UI

- **Rendering.** `UiSurface{document, resolution, maxHz, emissive, interactive}` renders into a pooled 2048²
  atlas page (LRU) in a render-graph pass, only when dirty. Update rate: ≤ 30 Hz when visible within 30 m,
  ≤ 5 Hz when farther, 0 when off-screen.
- **Interaction.** A crosshair or cursor ray becomes a surface UV and an RmlUi pointer event; gamepads get focus
  navigation. Any surface can pop out to a 2D overlay (accessibility).
- **Other uses.** MFDs bind to ship view-models. Holograms reuse this path with 03's material. The helmet or
  canopy HUD is collimated at infinity (R04-P1-18).

### 1.9 Accessibility

- **Colour:** semantic tokens (hostile, friendly, loot tier) paired with shapes or icons; protan, deutan and
  tritan palettes; an optional daltonization filter.
- **Subtitles:** 4 sizes, background opacity, speaker names, directional sound captions.
- **UI:** scale 75–200 % (independent of DPI), a minimum text size, a high-contrast theme.
- **Controls:** hold-or-toggle everywhere, full remapping, one-handed presets. Comfort toggles for shake, blur,
  head-bob and flashes.
- **Speech (Phase 4):** text-to-speech for menus and chat (SAPI, Speech Dispatcher).

### 1.10 Localization and text

- **Strings:** stable `LocString` IDs and a MessageFormat subset with CLDR plural, gender and select rules,
  generated into HEADLESS `engine/loc`. ICU is tools-only (R10-§12, R03-P1-2). CI runs pseudo-localization at
  +40 % length.
- **Fonts:** `FontStackDef` per locale with per-glyph fallback. CJK uses locale-specific fonts (Han
  unification).
- **Glyphs:** UI text up to 32 px uses FreeType hinted bitmaps in an LRU atlas. World-space text uses **offline
  MSDF atlases** (msdfgen is tools-only) with a bitmap fallback.
- **RTL:** RTL text in Phase 4; `[dir=rtl]` layout mirroring in Phase 5, per game.
- **Names:** wrapped in FSI/PDI isolates; the server rejects bidi controls in them.
- **Languages:** text for every language is always installed; VO only for the selected language (05 §7 tags).

### 1.11 Prediction hooks and network UX

- **Predicted intents:** `PredictedIntent<T>` gives view-models `pending | confirmed | rejected`. Inventory moves
  show a pending ghost that animates back on rejection. Ability cooldowns are predicted and corrected on reject
  (04 §5.7).
- **Value moves** are never optimistic: they show a spinner until the ledger acknowledges.
- **Connection loss:** "Connection interrupted" after 2 s without packets. Remote entities extrapolate for
  ≤ 250 ms, then freeze with a lag glyph (04 §5.3).

### 1.12 UI addons — decision: **yes, sandboxed Luau addons, WoW-style, phased**

MMO players expect UI mods (R08-T19). The Foundation UI itself uses the addon API (dogfooding).

| Rule | Detail |
|---|---|
| Surface | View-models, RML/RCSS, localization, rate-limited chat send, SavedVariables ≤ 1 MiB |
| Sandbox | Separate Luau VM (R10-§4) with no `io`/`os`/`debug`/`loadstring`, network or files. **≤ 1.5 ms/frame** across all addons and a **64 MiB** heap. An addon that errors is disabled; it never crashes the client |
| Information | Only what the default UI sees, i.e. what the server sent (04 §9) |
| **Protected actions** | Movement, targeting, abilities, market orders, trades and mail need a *secure context*: a hardware input event dispatched to a signed Foundation handler with no addon code (taint) on the call path. This blocks bots and automated rotations |
| Phasing | Ph3: internal only. Ph4: public `AddOns/` folder. Ph5: curated, signed portal |

### 1.13 Headless and bot modes

`engine/clientcore` is HEADLESS-safe (session state machine, service clients, launch code, prediction glue,
world decode). `apps/client` and 04's thin `helios-bot` (500–2,000 per process) both link it (R06-ENG-27).
`helios-client --headless --rhi=null --script=<flow>.luau` runs the real UI without a GPU. Bots use
bot-scoped credentials that only test shards accept.

### 1.14 Anti-cheat stance

| Layer | Measure | Ph |
|---|---|---|
| Server authority | Intents only, validation, movement budgets, capped lag compensation, information hiding (04 §5, §9) | 1 |
| Integrity | Authenticode, plus a launcher `WinVerifyTrust` check before launch; verified install; pak block checksums; no shipped symbols or cheat cvars | 2 |
| Client hygiene | Sandboxed Luau and addons with protected actions; Luau codegen is W^X, never RWX (R10-§4) | 3 |
| Optional vendor | `IAntiCheatProvider`: init, attestation in the token's 256 B user data, tick, vendor channel via the gateway. Candidate: **EAC via EOS** (free, Linux/Proton support, proprietary, flagged; R10-§9). Decided in Phase 4; the design never depends on it | 4 |

### 1.15 Windows specifics

- **Manifest** (R10-§2): UTF-8 code page, PerMonitorV2, `longPathAware`, `supportedOS`;
  `/SUBSYSTEM:WINDOWS`; static `/MT`. Exports `NvOptimusEnablement`/`AmdPowerXpressRequestHighPerformance`.
- **DPI and multi-monitor:** UI scale = display scale × user scale, following DPI changes across monitors. The
  display is persisted by name + bounds (SDL IDs are unstable), and positions are clamped after a monitor is
  unplugged.
- **Fullscreen:** borderless by default (flip model); exclusive via `VK_EXT_full_screen_exclusive`,
  Alt-Tab-safe; windowed.
- **HDR (Phase 4):** HDR10 PQ and scRGB via `VK_EXT_swapchain_colorspace` + `VK_EXT_hdr_metadata`. Display
  capability comes from SDL3 window HDR properties and DXGI. A calibration screen.
- **Power:** game threads opt out of EcoQoS while background downloads opt in. `ES_DISPLAY_REQUIRED` is held in
  world, because gamepads don't reset the idle timer. Backgrounded: 30 fps cap, optional mute. The client
  reconnects immediately on resume. Game Mode needs no API.
- **Defender and SmartScreen** (R10-§9): no packers, nothing run from `%TEMP%`, stable publisher and names, and
  every release submitted to Microsoft.
- **Data location:** all data in `%LOCALAPPDATA%`, because Controlled Folder Access blocks Documents and Pictures.
- **Firewall:** no listening sockets, so no firewall prompt.

### 1.16 Linux specifics

- **Build:** in the Steam Runtime 3 "sniper" SDK container (glibc ≥ 2.31 floor), with
  `-static-libstdc++ -static-libgcc`.
- **Runtime loading:** `dlopen` for `libvulkan.so.1`, X11/Wayland (via SDL), `libcurl.so.4`, `libsecret`, and
  optionally Feral `libgamemode` (BSD-3).
- **Display:** SDL picks Wayland or X11. Fullscreen is borderless only; HDR is best effort (Phase 5).
- **Input:** IME via IBus or Fcitx. The launcher detects missing udev/hidraw gamepad permissions.
- **Other:** cooked paths are lowercase; crashpad registers as ptracer (`PR_SET_PTRACER`); Proton is best effort
  only.

---

## 2. Launcher and patcher

### 2.1 Architecture — decision: **C++ SDL3 + RmlUi on `SDL_Renderer`**

A tiny stable bootstrap (`Helios.exe`) starts `app-<build>/HeliosLauncher.exe`, which links `engine/core`,
`ui`/`text`, `patch` and `crash`. RmlUi draws through **`SDL_Renderer`** (D3D11/D3D12 on Windows,
OpenGL/Vulkan on Linux, R10-§8).

**Why not ImGui:**
- The launcher is the first screen players see and needs shaping, RTL, accessibility, UI scale and rich news.
  ImGui has none of these (R08 F1), and ADR-010 keeps it editor/debug-only.
- The launcher **shares skins, fonts and localization** with the client frontend.
- `SDL_Renderer` needs no Vulkan, so the launcher itself can report a missing Vulkan 1.3 driver.

This amends R10-§13's "ImGui … launcher UI" row (§4.8).

**Build.** The launcher links no Jolt and targets **baseline x86-64 (SSE2)**. CI runs it under Intel SDE `-nhm`
on Windows and `qemu-x86_64 -cpu Nehalem` on Linux.

### 2.2 CPU and GPU gate

- **CPU:** CPUID for AVX, AVX2, BMI1, LZCNT, POPCNT, F16C and SSE4.2, plus OS YMM state (XCR0). Failure
  shows *"Helios requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: <model>."*
  (R10-§5).
- **The client repeats the check** in a non-AVX2 translation unit that runs before other static initializers
  (`init_seg(compiler)` on MSVC, `constructor(101)` on GCC/Clang). A `STATUS_ILLEGAL_INSTRUCTION`/SIGILL
  handler shows the same message.
- **GPU:** Vulkan 1.3 plus required features, with a vendor driver link on failure. `DriverAdvisoryDef` records
  in live config flag known-bad drivers.
- **Also:** disk space and Windows 10 22H2+; a warning below MIN RAM.

### 2.3 Login and account flow

1. **Phases 1–3:** password over TLS 1.3 to Identity (argon2id), which returns a 10-min JWT and a 30-day
   rotating refresh token (05 §1.1).
2. **"Remember me"** stores only the refresh token, in Windows Credential Manager (`CredWriteW`, DPAPI) or the
   Linux Secret Service. Without a secret service it warns and does not persist.
3. **Phase 4:** OIDC auth-code + PKCE in the **system browser** with a loopback redirect (RFC 8252) and no
   embedded web view; TOTP MFA; Steam/Epic federation via `IPlatformServices`.
4. **Hand-off:** the launch code goes over the inherited pipe (§1.1); refresh tokens never leave the launcher.
   `traceparent` lets one trace span launcher → queue → gateway (05 §6.2).

### 2.4 News, status and channels

- **News:** a JSON feed per locale, signed by a news subkey in the manifest keyset. It is rendered as
  **sanitized RML** (no script or bindings; images only from allow-listed CDN hosts) and cached offline. Patch
  notes are per-build sidecars.
- **Status:** shard population band, queue ETA, and incident banners from live config.
- **Channels (05 §7):** `live` and `ptr` for players, `beta` by entitlement, `dev`/`qa` internal only. Each
  channel has its own install directory, but all share one chunk cache. PTR installs are seeded from live (EVE's
  shared cache, R01 §8).

### 2.5 Content-addressed chunk patching

**Fixed by 05 §7:** FastCDC (16/64/256 KiB), BLAKE2b-256 chunk IDs, zstd-19 on the CDN, packs for chunks under
32 KiB, `.hman` manifests with tiers 0/1/2, and a signed pointer with `sequence` (anti-rollback),
`rollout_pct` and `min_launcher`. `engine/patch` also implements FastCDC in C++ for local publishing and
tests, sharing golden vectors with Go.

```
%LOCALAPPDATA%\Programs\Helios\     Helios.exe (bootstrap), launcher.json, app-<build>\   (R10-§9)
<Library>\Helios\<channel>\         default %LOCALAPPDATA%\Helios\Games, or any NTFS/ext4 drive
   bin\ (tier 0)   content\*.hpak (≤ 2 GiB, sparse while streaming)   staging\   install.lock
   install.db      schema-generated snapshot + fsync'd append-only journal (no SQLite, ADR-014)
%LOCALAPPDATA%\Helios\cache\chunks\ shared chunk cache (default cap 10 GB)
```

`install.db` stores each file's manifest entry, chunk offsets and **per-chunk residency bitmap**, plus the last
accepted `sequence`.

1. **Trust chain.**
   - The root key is pinned in the binary and signs the keyset.
   - The pointer needs a valid signature, an unexpired `expires` (7 days, against freeze attacks) and a
     `sequence` ≥ the stored one. A staged build is taken only if `hash(install_id) mod 100 < rollout_pct`.
   - The manifest hash must match the pointer, and its signature must verify with **Monocypher 4.0.3 +
     `monocypher-ed25519`**.
   - Chunk hashes are checked before each write, file hashes before each rename.
2. **Plan** (idempotent). Each changed chunk comes from the first available of: a local file (via `install.db`,
   no re-hash), the shared cache, `patch-from` if smaller (05 §7), or a CDN range. Adjacent ranges coalesce into
   multi-range requests of ≤ 8 MiB.
3. **Disk check.** Required space = staged files + cache writes + 1 GiB. If short, patch file by file: paks are
   ≤ 2 GiB, so peak overhead is ≤ 2 GiB. Show exact numbers and offer another drive. Paks are fully preallocated
   on exFAT, which has no sparse files.
4. **Download.**
   - WinHTTP (system proxy, SChannel, HTTP/2) on Windows; libcurl on Linux.
   - 8 concurrent requests (adaptive 4–16) across weighted multi-CDN hosts; a host is dropped above 5 % errors.
   - Each chunk is verified, then written at its staging offset.
5. **Bandwidth.** A token-bucket cap (25 % of measured throughput while the game runs), a pause on metered
   Windows networks (`INetworkCostManager`), and an optional schedule.
6. **Resume.** The journal records verified ranges every 64 MiB or 2 s. After a kill or power cut, re-planning
   wastes ≤ 16 MiB.
7. **Apply.** Renames run under a journal transaction, and tier-0 executables **flip last, together**. An
   interrupted flip replays forward on recovery, and the client refuses to start while `flip_in_progress` is
   set.
8. **Verify and repair** (R07-P0-11).
   - **Quick** (every launch, < 1 s): size, mtime and tier-0 executable hashes.
   - **Full:** parallel BLAKE2b with a per-chunk mismatch map; **repair** fetches only the bad chunks.
   - **In game:** a pak block that fails its checksum is re-fetched by the `StreamingInstaller`.

### 2.6 Streaming install and pre-download

**Tiers and groups** (R03-P1-6, R05-P2-25): tier 0 = launcher, client and login area; tier 1 = common content;
tier 2 is grouped per zone (`group` tag = zone record hash), per VO language and per optional HD pack.

**Play while downloading.** The game is playable once tier 0 is done. The launcher then releases
`install.lock`, and the client's in-process `StreamingInstaller` (the same `engine/patch`) becomes the single
writer. Every VFS read checks the lock-free residency bitmap. A miss raises a demand request, and streaming
shows a placeholder LOD or delays the spawn.

**Priority:** (1) demand misses; (2) the destination (the `ZoneChange` target, the quantum-travel target at
spool, party members' zones); (3) jump-graph neighbours; (4) tier 1; (5) tier 2 by zone popularity;
(6) optional packs.

**The one allowed loading screen.** If the arrival set's ETA exceeds the transit time, the transition shows
*"Downloading region — 1.2 GB, 2:10"*.

**Pre-download.** A pointer field `next{build_id, manifest_hash, available_at}` makes the launcher fetch the next
build's missing chunks into the shared cache: resumable, rate-limited and disk-checked. Release day then applies
locally in minutes. Optional (Phase 4): spoiler files stay XChaCha20-encrypted until the key is published.

### 2.7 Self-update

1. The launcher has its own pointer and manifest; the game pointer's `min_launcher` forces an update.
2. The new build is downloaded to `app-<new>\` and verified.
3. `launcher.json.tmp` is moved over `launcher.json` with `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` (an
   atomic same-volume rename), then the launcher relaunches through the bootstrap.
4. The new launcher writes `healthy` once its UI is up and a pointer fetch succeeds. After two failed starts the
   bootstrap reverts; the two previous builds are kept.
5. Rarely, the bootstrap updates itself: the running `Helios.exe` is renamed to `Helios.old.exe` (Windows allows
   this) and deleted on the next run (R10-§9).

### 2.8 Installer, signing, uninstall

**Installer — decision: a self-installing launcher** (`HeliosSetup.exe` = launcher `--install`).
- **MSIX is rejected:** its read-only package directory breaks self-patching, and it conflicts with
  anti-cheat drivers.
- **WiX is rejected:** MS-RL licence plus a maintenance-fee EULA.
- **Inno Setup 7.1:** only an optional Phase 4 partner wrapper, pending sign-off on its About-box clause
  (R10-§9, §14).

**Install.** Per-user, with no UAC. The user picks a library folder (filesystem and free space are checked).
The launcher goes to `%LOCALAPPDATA%\Programs\Helios`, with Start-menu and optional desktop shortcuts, an HKCU
uninstall entry and an HKCU `helios://` handler. Only a vendor anti-cheat bootstrapper, if adopted, ever
elevates.

**Code signing** (AAA-PLT-6, SEC-6). Every PE (exe, dll, `crashpad_handler`) gets Authenticode SHA-256 with an
RFC 3161 timestamp. Keys are HSM-held: Azure Trusted Signing or a cloud-HSM OV/EV certificate (eligibility
**[unverified]**). Signing runs only from the protected release branch, with two-person approval, and
`signtool verify /pa /all` gates CI.

**Uninstall** (`--uninstall`, from Apps & Features). Optionally keeps game data. Removes directories,
shortcuts, registry entries, the handler and stored credentials, but keeps screenshots. It deletes itself last
through a helper in `%LOCALAPPDATA%\Helios\.uninst`, never `%TEMP%`.

### 2.9 Linux packaging and storefronts

- **Linux:** a tarball in Phase 1 (dev/QA). An **AppImage** in Phase 3 (AAA-PLT-6) that holds only the
  bootstrap and installs the launcher under `~/.local/share/helios/`, so self-update and patching match
  Windows. Flatpak is optional in Phase 5; deb/rpm are non-goals.
- **Storefronts (Phase 4+):** Steam or Epic depots patch those SKUs, and our launcher is not used (R07 §7). An
  isolated `IPlatformServices` plugin (Steamworks and EOS are proprietary) provides auth tickets, overlay,
  presence and achievements. The client's own login and pointer check (§1.1) cover this path.

---

## 3. Crash and diagnostics pipeline

**Capture.**
- **Library:** sentry-native 0.17.1 with the crashpad backend (R10-§9), initialized right after the CPU gate in
  the client and launcher (later the editor and servers).
- **Handler:** a signed `crashpad_handler` ships in tier 0 and uploads via `winhttp` or curl. The crashing
  process never writes its own dump.
- **Contents:** breadcrumbs (state transitions, zone, the last 200 log lines, the last 50 net events, preset,
  RAM/VRAM, GPU/driver), plus a 2 MB log ring and `gpu_crash.json`. Minidumps are ≤ 2 MB; full dumps only in
  opt-in QA builds.

**Special crash classes.**

| Class | Handling |
|---|---|
| GPU device lost / TDR | `VK_EXT_device_fault` data and the last render-graph pass from buffer markers (`VK_AMD_buffer_marker`, `VK_NV_device_diagnostic_checkpoints`) → non-fatal event → restart dialog. Vendor SDKs are optional plugins |
| Hang | Watchdog on game/render heartbeats: a 10 s stall triggers a crashpad dump-without-crash; at 60 s it offers to terminate |
| OOM, addon error | OOM reports include mimalloc stats. Addon errors are non-fatal and rate-limited, and the addon is disabled |

**Symbols.** Every tagged CI build publishes PDBs and binaries in a `symstore` layout, served over HTTPS as a
Microsoft-compatible symbol server (VS/WinDbg via `_NT_SYMBOL_PATH`), with `/SOURCELINK`. Linux split debug
info is keyed by build-id in debuginfod layout. Release symbols are kept forever; dev symbols for 90 days.

**Ingest and triage.**
- **`crashgw`** (Go, 05's telemetry family) drops unknown builds, rate-limits to 5 reports per install per
  hour, scrubs PII and keeps dumps for 90 days.
- It forwards to **self-hosted Sentry** for grouping and release health (FSL licence review; GlitchTip is the
  fallback). rust-minidump (MIT) re-symbolicates in batches.
- Server dumps go to a separate project. Groups are routed by CODEOWNERS.
- **Release gate:** a new PTR crash group with ≥ 20 users, or a crash-free-session drop of more than 0.3 pp,
  **blocks promotion to live**.

**Performance telemetry.**
- **Per-minute aggregates** from every client: frame-time p50/p95/p99; hitches > 50 ms by cause (PSO, I/O,
  Luau GC, net, UI); PSO and streaming misses (AAA-REN-4, CNT-2); RAM/VRAM (CNT-3); load phases; ping and loss;
  zone, preset and hardware. 1 % of sessions add detailed traces.
- **Transport:** `tel.<shard>.client.perf` via the gateway (05 §1.16), or HTTPS before reaching the world.
- **Patch telemetry:** throughput and bytes saved per CDN/ISP.
- **Privacy:** consent on first run; crash reporting is opt-out; 90-day retention.

---

## 4. Layout, interfaces, ladder, acceptance, tests, risks, traceability

### 4.1 Module and directory layout

```
engine/clientcore/  HEADLESS: session FSM, HTTPS/SSE clients, launch code, predicted intents
engine/input/       actions, contexts, rebinding, devices, haptics (SDL3 via platform layer)
engine/ui/          RmlUi: RHI + SDL_Renderer backends, view-model binding, focus/nav, UiSurface,
                    WorldMarkerRenderer, addon host (sandbox, taint)
engine/text/        FreeType/HarfBuzz/SheenBidi/libunibreak font engine, glyph atlases
engine/loc/         HEADLESS: LocString, MessageFormat subset, CLDR rules
engine/patch/       HEADLESS: .hman, trust chain, install.db, planner, FastCDC, IHttpClient
                    (WinHTTP | libcurl | Faulty), verify/repair, StreamingInstaller
engine/crash/       sentry-native wrapper, breadcrumbs, watchdog, GPU markers
apps/client/        helios-client: states, camera rig, settings, frontend
apps/launcher/      Helios bootstrap + HeliosLauncher; --install / --uninstall
tools/packager/     helios-pack: cook → CDC-friendly .hpak (≤ 2 GiB, stable order, per-asset zstd,
                    tier/group/language tags); local publish (prod publish = Go helios-patch, 05 §7)
content/ui/, content/loc/, tests/{ui,patch,client}/   Foundation UI, strings, flows, fixtures
```

`engine/ui`, `engine/text` and `engine/input` are not HEADLESS, so cells never link them (R06-ENG-01). Their
runtimes (and `engine/loc`) are specified by 02 §7.5–7.6; this section owns the HUD and screens,
`WorldMarkerRenderer`, the addon host and the client-side uses listed above.

### 4.2 Key interfaces (sketch)

```cpp
namespace helios::patch {
struct VerifiedManifest;                                   // constructible only by TrustChain
class TrustChain { public: Result<VerifiedManifest> verify(std::span<const std::byte> pointer,
                     std::span<const std::byte> manifest, uint64_t minSequence) const; };
class Installer { public:                                  // single writer; holds install.lock
  Result<Plan> plan(const VerifiedManifest&, const InstallDb&, const ChunkCache&, PlanOptions) const;
  Result<void> run(const Plan&, IProgress&, RateLimit, CancelToken);    // resumable, idempotent
  Result<VerifyReport> verify(VerifyMode, IProgress&); };              // Quick | Full
class StreamingInstaller { public:                         // in client; any thread
  bool resident(PakId, uint64_t offset, uint32_t len) const;          // lock-free
  RequestId demand(PakId, ByteRange, Priority); void prioritizeGroup(GroupId, Priority); };
}
namespace helios::client {
class ClientStateMachine { public: void post(const ClientEvent&); ClientState top() const; }; // game thread
template<class T> struct PredictedIntent { IntentId id; T payload; IntentStatus status; };
}
namespace helios::input {
class InputSystem { public:                                // game thread
  void pushContext(ContextId); void popContext(ContextId);
  ActionValue read(ActionId) const;                        // value + DeviceClass {Mouse, Gamepad, Hotas}
  Result<void> rebind(ActionId, BindingSlot, PhysicalInput); };
}
namespace helios::ui {
template<class VM> class ViewModel { public: VM& edit(FieldMask dirty); };  // schemac-generated
class UiSurface { public: void setPolicy(float maxHz, float nearMeters); void injectPointer(Vec2 uv, Buttons); };
struct IAddonHost { virtual bool secureContext() const = 0; };            // protected-action gate
}
// helios::platform: ICredentialStore (CredMan | libsecret); optional plugins IAntiCheatProvider,
// IPlatformServices, ILatencyProvider
```

### 4.3 MVP → AAA feature ladder

| Area | Ph 0 | Ph 1 | Ph 2 | Ph 3 | Ph 4 | Ph 5 |
|---|---|---|---|---|---|---|
| Launcher | Fetch chunked manifest from local CDN, verify Ed25519 | RmlUi UI, login, launch code, CPU/GPU gate | Live/PTR, self-update, news, bandwidth caps | Pre-download, shard status | OIDC/PKCE, MFA, Steam/Epic | — |
| Patching | Chunker, trust chain, fixtures | Full-file download, quick verify | CDC diff, resume, repair (CNT-7) | Streaming tiers, in-game heal | Multi-CDN, patch-from | Galaxy-scale prefetch |
| Install / signing | — | Dev installer, tarball | **Signed installer**, uninstall (PLT-6) | AppImage (PLT-6) | Optional Inno | Flatpak |
| Client shell | SDL3 window, Null/Vulkan RHI | State machine, threads, settings, reconnect | Queue UX, synced settings | Headless UI flows at scale | HDR, exclusive FS | — |
| Input / camera | Keyboard/mouse actions | Contexts, gamepad, rumble, FP↔cockpit camera | Rebinding, HOTAS, IME | Aim-assist class, tactical camera | Gyro, photo mode | — |
| Game UI | — (RmlUi lands in Ph1; the Ph0 launcher is a plain SDL3 window) | RmlUi on RHI; HUD, frontend, system map, MFDs (R06) | Chat, inventory, market, social, dialogue, galaxy map | Overview, group dialogue, internal addons | Public addons | Addon portal |
| Access / i18n | — | UI scale, subtitles | Pseudo-loc CI, colourblind palettes | Remapping, comfort | 2 languages + CJK/RTL test locales, TTS | RTL mirroring |
| Crash / telemetry | Local minidumps | Breadcrumbs, perf aggregates | Sentry, crashgw, symbols (STB-1) | Release gates, GPU markers | Driver advisories, hang dumps | — |

### 4.4 Acceptance criteria

All figures are on REF; figures in brackets are MIN.

| ID | Criterion | Maps to | Ph |
|---|---|---|---|
| CL-1 | Launcher cold start → login interactive ≤ 2.0 s (≤ 3.5 s); RSS ≤ 150 MB idle, ≤ 400 MB while patching | — | 1 |
| CL-2 | "Play" → character select ≤ 6 s (≤ 12 s) with a warm PSO cache; ≤ 20 s with progress after a driver change | REN-4 | 2 |
| CL-3 | Character select → controllable in world, unqueued, p95 ≤ 15 s (≤ 25 s) | SRV-7 | 2 |
| CL-4 | Zone transition p99 ≤ 3 s; no frame > 50 ms in world | SRV-5, REN-5 | 1 |
| CL-5 | 5 s outage → back in world ≤ 3 s after recovery in ≥ 99 % of 1,000 runs; sleep/resume < 5 min reconnects | STB-5 | 2 |
| CL-6 | Input → present p95 ≤ 40 ms at 60 fps; camera ≤ 25 ms at 120 fps with late latching | REN-3 | 4 |
| CL-7 | BENCH-1 HUD ≤ 1.0 ms CPU / 0.5 ms GPU at 1440p; 40 diegetic screens ≤ 1.0 ms CPU / 0.5 ms GPU (RT-12, 03 §7.5). BENCH-3: 1,000 brackets ≤ 0.6 ms CPU | REN-1 | 3 |
| CL-8 | 10k-row market grid: first rows ≤ 150 ms after data arrives; no frame > 16.7 ms while scrolling | — | 2 |
| CL-9 | A 1 GB change in a 50 GB install downloads ≤ 1.5 GB (typical ≤ 1.2×). A 50 MB executable change ≤ 20 MB with patch-from | CNT-7 | 2 (4) |
| CL-10 | Full verify of 50 GB ≤ 3 min on NVMe (≤ 5 min on SATA SSD); quick verify < 1 s | CNT-7 | 2 |
| CL-11 | ≥ 90 % of a 1 Gbit/s link using ≤ 2 cores; 50 GB via a 500 Mbit/s CDN in ≤ 15 min | — | 2 |
| CL-12 | 1,000 kill or power-cut injections during install, patch and self-update → 0 corrupt installs; the launcher always starts; ≤ 16 MiB wasted per resume | — | 2 |
| CL-13 | Tier 0 ≤ 15 % of the install; a 50 GB game playable within 15 min at 100 Mbit/s | — | 3 |
| CL-14 | 100 % rejection of tampered, expired, rolled-back or wrongly keyed pointers, manifests and chunks; parsers fuzzed ≥ 24 CPU-h | SEC-6/7 | 2 |
| CL-15 | Every injected crash kind (null deref, stack overflow, abort, pure virtual, OOM, hang, device lost) → symbolicated report ≤ 5 min, Windows and Linux | STB-1 | 2 |
| CL-16 | Crash-free sessions ≥ 98.0 / 99.4 / 99.8 % (Ph2/3/4; equivalent to STB-1 at a 2 h mean session) | STB-1 | 2–4 |
| CL-17 | On an emulated CPU without AVX2: the CPU message, never a raw SIGILL | — | 1 |
| CL-18 | Every shipped PE passes `signtool verify /pa /all` with a timestamp; installing needs no admin | PLT-6 | 2 |
| CL-19 | Pseudo-loc +40 % without clipping; 0 missing glyphs; golden screenshots at 75/100/200 % | — | 2 |
| CL-20 | Headless login → world → logout ≤ 90 s per commit; smoke tests on Win10 22H2, Win11 24H2 and Ubuntu 24.04 | PLT-3 | 1 |
| CL-21 | Linux client within 10 % of Windows in every BENCH; memory within CNT-3 | PLT-5, CNT-3 | 4 |
| CL-22 | Hostile-addon suite (loops, heap bombs, protected calls) stays ≤ 1.5 ms/frame and ≤ 64 MiB and never runs a protected action | SEC-1 | 4 |

### 4.5 Test strategy

- **Unit (doctest):** state-transition tables, input triggers and binding conflicts, camera blends across
  `Reparent()`, MessageFormat vectors, journal replay, and FastCDC/manifest golden vectors shared with Go.
- **Patch-diff fixtures** (built by `helios-pack`): an insert at the pak head, reordered assets, a compression
  change, renames and deletes, an added language, an executable-only change, and 1 GB changed in 50 GB. Each
  asserts **exact download bytes** and final hashes.
- **Fault injection:**
  - `FaultyHttp`: resets, truncation, slow-loris, 5xx, corrupt bytes, stale objects, TLS failures.
  - `FaultyFs`: ENOSPC, AV sharing violations, torn writes, and a kill at every journal step.
  - Clock skew, and crashes via `--crash=<kind>`.
- **UI automation:** headless Luau flows click elements by ID and assert view-model state. Lavapipe golden
  screenshots cover {en, pseudo, CJK, RTL} × {75, 100, 200 %} × palettes, and T19 reuses the harness.
- **Nightly lab:**
  - MIN/REF hardware on Win10, Win11 and Ubuntu, across NVIDIA, AMD and Intel.
  - Defender real-time scanning stays on during install/verify budget runs.
  - DPI/monitor hotplug, sleep/resume, and an 8 h soak (RSS growth ≤ 2 %).
  - 04 NetSim profiles drive reconnect tests.

### 4.6 Risks and mitigations

| Risk | Mitigation |
|---|---|
| RmlUi too slow for EVE-density UI | `<datagrid>`, `WorldMarkerRenderer` outside RmlUi, CL-7/8 gates; plan B: native grid element |
| RmlUi or SDL3 upstream stalls | Pinned, vendored, confined behind `engine/ui` and the platform layer |
| AVX2 floor or a failing gate | Two gates plus emulator CI (CL-17) |
| SmartScreen and Defender false positives | Stable names, HSM signing, Microsoft submissions, no packers |
| Signing-key or CDN compromise | Offline root, rotating subkeys, pinned keyset, anti-rollback, expiry, Authenticode |
| Self-update bricks installs | Health handshake, 2 previous builds kept, CL-12 |
| Patch egress cost (05 §6.4) | CDC + patch-from, shared cache, CL-9 gate |
| Addons enable botting | Protected actions + taint; server-side detection |
| Licence sign-offs | FreeType FTL, Inno (if used), SIL OFL fonts, EOS/Steamworks, Sentry FSL |

### 4.7 Traceability

| Requirement | Where |
|---|---|
| R10-§8 (SDL3: IME, rumble, raw input, DPI, `SDL_Renderer`) | §1.3, §1.5, §1.15, §2.1 |
| R10-§9 (self-installing launcher, atomic swap, WinHTTP/libcurl, Inno/WiX/MSIX, Authenticode, crashpad, symbols, EAC) | §1.14, §2.5–2.8, §3 |
| R10-§2 (manifest, `/MT`), §5 (AVX2 gate), §7 (Ed25519), §12 (RmlUi and text stack) | §1.7, §1.10, §1.15, §2.2, §2.5 |
| R07 §7, P0-11, P1-20 (CDC patching); R01-P0-9, R01 §8 (shared cache) | §2.4–2.6 |
| R07-P0-2/3, R07 §4 (OIDC, queue, reconnect); R07 §6 (dumps, symbol server) | §1.1, §1.2, §2.3, §3 |
| R03-P0-9 and lesson 2 (UI decoupled from the GPU API), R03-P0-8, P1-2, P1-6, P2-5 | §0, §1.7, §1.10, §2.6 |
| R04-P1-14 (continuous camera), R04-P1-18 / §7 (in-world UI) | §1.6, §1.8 |
| R05-P0-3 (prediction), P0-9 (per-device aim assist), P2-25 (streaming install) | §1.5, §1.11, §2.6 |
| R06-ENG-01/27; R08 F1, T19, T25; R09 B15/B16 | §1.7–1.13, §4.1 |
| AAA-REN-4/5, SRV-5/7, CNT-2/3/7, STB-1/5, SEC-1/6/7, PLT-3/5/6 | §4.4 |

### 4.8 Cross-section dependencies

- **00 ADR:** amend R10-§13 so ImGui is not the launcher UI; allow-list SIL OFL-1.1 for fonts (both done in ADR-010).
- **02 Engine:**
  - Keep Jolt's AVX2 flags PRIVATE to physics, or exempt the CPU-gate translation unit.
  - Paks ≤ 2 GiB with per-asset compression, stable order and `group`/`tier`/`language` tags.
  - A VFS residency hook, per-block checksums and per-zone PSO lists.
  - Resolved: 02 owns the `engine/ui`/`engine/text` runtime (02 §7.5); this section owns HUD, screens and addons
    (README updated).
- **03 Rendering:** UI composite after upscaling, HDR paper white, `UiSurface` and world-marker passes, cockpit
  depth range, GPU crash markers.
- **04 Networking:**
  - `device_class` in the input header, a client TiDi field, the shared `clientcore`, and the attestation
    user-data format.
  - Resolved: 04 §1 uses embedded-postgres (ADR-014); `device_class` is in 04 §5.2.
- **05 Backend:**
  - Resolved: A12 now restates AAA-CNT-7 (CL-9/CL-10 govern).
  - Needed (now in 05 §1.1, §1.5, §6.2, §7): pointer fields `next{}`, `min_client` and the CDN list; a settings
    blob of ≤ 256 KiB; `crashgw` plus a symbol bucket; bot credentials; shared FastCDC vectors.
- **07 Editor:** T19 on `engine/ui`, T25 string tables, PIE on `apps/client`, addon API docs.
- **09 Roadmap:** the Phase 0 launcher skeleton, buying the HSM signing certificate in Phase 1, and legal
  sign-offs.
