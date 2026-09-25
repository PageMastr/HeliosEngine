# 08 — Game Client and Launcher

*Helios master plan, section 08. Draft v5 (round 4, from the other sections' fixes: the invisible Relocating
state for make-before-break gateway deploys §1.2 and §4.3 (05 §6.3.1); the CPU gate as the first TLS callback
§2.2 (02 §1.1); WP-2.16b for packaging §2.10.5 and §4.8; the Placement ghost's `ZoneStructures` and city
footprint pre-check and the City panel's binding to `CityGovernance` §1.7.2 (06 §9.2). Round 3: the
launcher's x86-64-v1 floor, its audit
and pre-v2 emulator runs §2.1.1; Store, Support and report, and Customization panels §1.7.2. Round 2:
Foundation panel breadth §1.7, product identity and branded builds §2.10). Conforms to
ADR-001/002/003/004/006/007/010/011/013/014/015/016; phases per `README.md`. Citations: `R10-§9` = report 10 §9; `R07-P0-11` = report 07 requirement P0-11. 04 owns wire and
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
5. **Every flow is automatable:** login, patching, crashes and every Foundation panel (§1.7.3) run headless in
   CI on Windows and Linux.
6. **Nothing on a player's PC is named by the engine.** Paths, URI schemes, registry keys, credentials,
   endpoints and trust roots come from the stamped product block (§2.10), so Helios games coexist and a
   studio brands its game without rebuilding the engine.

---

## 1. Client application

### 1.1 Startup sequence

| # | Step | Budget (REF) |
|---|---|---|
| 1 | **Launcher:** CPU/GPU gate → self-update → login → verify pointer and manifest → tier 0 → quick-verify executables (§2) | — |
| 2 | **Hand-off:** `CreateLaunchCode` (05 §1.1) returns a one-time 60 s code, passed to the client (`helios-client`, named per product, §2.10) on an **inherited pipe** (Windows handle list, Linux fd 3), never on the command line | 0.2 s |
| 3 | **Boot:** CPU gate, crash handler, mimalloc, VFS, settings, SDL3 window, Vulkan 1.3 device, `VkPipelineCache`, RmlUi + fonts, audio | ≤ 2.5 s |
| 4 | **Exchange:** `ExchangeLaunchCode` returns a 10-min JWT and a refresh token kept **in memory only** | ≤ 0.3 s |
| 5 | **Character select:** shard and character lists; a 3D scene streamed from one small container | ≤ 2 s |
| 6 | **Queue:** `Enqueue(shard, character)`, with position and ETA over SSE (05 §1.2). The player may idle in character select | 0 s unqueued |
| 7 | **Session:** `CreateSession` returns a connect token (≤ 45 s, 04 §2.3) pinned to `(compat_epoch, manifest_hash)`. Only the **compat epoch** gates (05 §1.14.1): a staged or older build of the live epoch is admitted. During a flip (`CONTENT_EPOCH_FLIP`) the client applies its pre-downloaded build and retries (§2.6); a build of any other epoch, or below `min_client`, hands off to the launcher (`--update`) | ≤ 0.2 s |
| 8 | **Connect:** HTP handshake (1.5 RTT), ACCEPT, placement, `ZoneChange` | ≤ 0.3 s |
| 9 | **Loading:** stream the **arrival set** (the player's authority-group containers plus a radius around them, and the zone's PSO precache list). `InWorld` starts once it is resident and the first owner snapshot has been applied | ≤ 8 s |

**No launch code** (dev builds; Steam later): the client shows the same RML login screen and fetches the channel
pointer itself. A build of another compat epoch, or below `min_client`, hands off to the launcher (`--update`).

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
| Relocating (Phase 4; invisible, no overlay) | InWorld | On `Relocate{token}` the client opens a second connection to the token's gateway while the first keeps playing (05 §6.3.1, 04 §2.4). Until `RelocateDone` it sends each input on both connections and applies STATE from both under the per-handle tick rule. It holds EVENT_R from the new connection until the old one delivers `PathClosed`, and de-duplicates pushes by message ID. It closes the old connection after `PathClosed`. If it cannot connect within 10 s it stays on the old connection, and the gateway retries or falls back to Reconnecting. If the old connection dies first, it sends `HeldEntities` on the new one |
| Reconnecting (overlay: 2 s without packets, 10 s linkdead per 04 §2.4, or OS resume) | InWorld / CharacterSelect | Reconnect token retried after a uniform 0–2 s jitter, then with backoff 0.5, 1, 2, 4 s… (each ±50 %) for ≤ 5 min, so a lost gateway box's 8k clients spread their redemptions (04 §2.4). If the Session service holds a redemption in its reconnect lane, the overlay shows the returned ETA and keeps the place (05 §6.7). On rejoin the client keeps its entity table and sends it once as `HeldEntities` (sorted handles, ≈ 4 KB), so every contributing cell resyncs with full-mask updates instead of creates (04 §6.6); a `ClockReset` after a server hitch re-bases the command clock (04 §5.1) |
| Disconnected | CharacterSelect / Login | Localized reason code |

### 1.3 Main loop and threading

| Thread | Work | Budget |
|---|---|---|
| **OS (main)** | Blocks in `SDL_WaitEventTimeout(1 ms)`, so every event is timestamped and enqueued within 1 ms: window, IME, cursor. Events go to the game thread through a lock-free ring; accumulated mouse deltas also go to an atomic that the render thread late-latches. Main-thread-only calls go through `SDL_RunOnMainThread` | ≤ 0.2 ms of work per frame |
| **Game** | **Phase A** (needs none of this frame's input): owner and remote snapshot apply and reconciliation replay, remote interpolation and animation jobs, client Luau timers, non-focused RmlUi layout, audio and streaming requests. **JIT sleep** (§1.3a). **Phase B** (latency-critical): drain the input ring → actions → speculative cue step → fixed-step prediction at the zone command rate (20–60 Hz, 04 §5.1) → local-player update, HUD view-models and focused-document layout → **extract** | A ≤ 3 ms; B ≤ 4 ms (ticks ≤ 3); extract ≤ 1 ms; total ≤ 8 ms at 60 fps |
| **Render** | Low-latency mode: prepare, record and submit **frame N as soon as extract N lands** (same frame, empty queue). Throughput mode: frame N−1 while the game thread simulates N (03 §2.5). Submits in 3 batches; **late-latches** camera orientation into batch 0 | ≤ 4 ms; extract → first submit ≤ 2.2 ms |
| **Present-wait** | Waits on `vkWaitForPresentKHR` per `present_id` and records when each frame reached the display (§1.3a) | ≈ 0 |
| **Workers / I/O** | Job graph (flecs, Jolt); streaming reads; `StreamingInstaller` at lowest priority | — |
| **Net** | Socket I/O, AEAD, keepalives; keeps running through hitches and window drags | ≤ 5 % of a core |

The game thread is separate from the OS thread because Win32 modal loops (window drag or resize) would
otherwise stall prediction. **Pacing:** ≤ 2 frames in flight as a resource cap. The just-in-time `FramePacer`
(§1.3a) is the default `ILatencyProvider`, combined with VRR and MMCSS "Games" for the game and render threads.
The vendor providers `VK_NV_low_latency2` and `VK_AMD_anti_lag` arrive in Phase 3.

### 1.3a Input latency: budget, just-in-time pacing and measurement (CL-6)

**The problem.** 03 §2.5's diagram is a throughput pipeline: the game thread simulates N, the render thread
records N−1 and the GPU executes N−2. With its queues full, about 3 frame periods (≈ 50 ms at 60 fps) separate an input from
the start of its image's scan-out. Late-latching the camera fixes mouse-look only. Firing, abilities and UI
need the whole chain to be short. So the client has two pacing modes:
- **Low-latency (default in play, every preset).** A just-in-time (JIT) frame start keeps the queues empty,
  and the render thread handles frame N in the same frame period.
- **Throughput.** 03 §2.5's full overlap, used only for the editor viewport, offline capture and
  `r.pacing=throughput`.

BENCH frame-rate gates (AAA-REN-1/2/3) run in low-latency mode. Low-latency may cost at most 3 % of the frame
rate.

**Measurement points.** All CPU times use one clock: QPC on Windows, `CLOCK_MONOTONIC` on Linux. GPU timestamps
map onto it through `VK_EXT_calibrated_timestamps`.

| Point | Event |
|---|---|
| M0 | Input time. The harness takes it just before `SendInput` or the `uinput` write; live telemetry uses the SDL3 event timestamp |
| M1 | The OS thread enqueues the event (ring, or mouse-delta atomic) |
| M2 | Phase B drains the ring; for the camera, the render thread latches the delta |
| M3 | Extract of the responding frame ends |
| M4 | Batch 0 of that frame is submitted |
| M5 | GPU start and end (calibrated timestamps of the first and last pass) |
| M6 | `vkQueuePresentKHR` returns |
| M7 | **Displayed.** `vkWaitForPresentKHR(present_id)` returns on the present-wait thread; Windows runs are cross-checked with PresentMon 2.x |
| M8 | Photon (weekly photodiode rig only) |

The **responding frame** differs per class:
- *action*: the first frame whose extract carries a cue or view-model change tagged with the input's
  prediction key;
- *camera*: the first frame whose latched view includes the mouse delta;
- *UI*: the first frame whose `UiDrawList` shows the pressed or hovered state of the targeted element ID.

**CL-6 measures M0 → M7.** Server round-trips are out of scope by design: confirmed hits and damage numbers
belong to 04's NS criteria.

**Latency budget.** The figures are ceilings on REF with low-latency mode, independent flip or exclusive
fullscreen, and a 144 Hz VRR lab display. The 60 fps columns are for 1440p High with a 60 fps limit; the
120 fps columns are for Performance mode (03 §8.1). Sample wait is uniform over one frame period T, so its
p95 is 0.95 T. The other rows are p95 ceilings, and their sum bounds the p95 conservatively.

| # | Stage | Points | Action / UI 60 fps | Camera 60 fps | Action 120 fps | Camera 120 fps |
|---|---|---|---|---|---|---|
| 0 | Device → OS (USB poll, 1 kHz mouse or keyboard; pads 4–8 ms). *Outside CL-6* | — | (1) | (1) | (1) | (1) |
| 1 | OS delivery → OS thread enqueue | M0 → M1 | 1.0 | 1.0 | 1.0 | 1.0 |
| 2 | **Input sample wait.** The event waits for the next Phase B sample (action) or render latch (camera); the JIT sleep ends just before this point | M1 → M2 | 15.8 | 15.8 | 7.9 | 7.9 |
| 3 | Phase B: actions, speculative cue step, prediction ticks, local update, HUD | M2 → extract start | 4.0 | — | 4.0 | — |
| 4 | Extract | → M3 | 1.0 | — | 1.0 | — |
| 5 | Render critical path: prepare 1.5 + graph compile 0.3 + record batch 0 0.4 (03 §8.1 CPU budgets). Camera: latch → submit | M3 → M4 | 2.2 | 0.3 | 2.2 | 0.3 |
| 6 | GPU: queue slack ≤ 0.3 + frame (03 §8.1.1: REF 60 fps walls ~13.0–14.5 ms against a ≤ 14.5 ms target; Performance mode wall ~7.2 ms against ≤ 7.2) | M4 → M5 end | 14.5 | 14.5 | 7.5 | 7.5 |
| 7 | GPU end → displayed (independent flip, VRR, no DWM composition) | M5 → M7 | 1.0 | 1.0 | 1.0 | 1.0 |
| | **Sum = CL-6 bound (M0 → M7, p95)** | | **39.5 → gate 40** | **32.6 → gate 33** | **24.6 → gate 28** | **17.7 → gate 25** |
| 8 | Scan-out to screen centre + pixel response. *Outside CL-6*; checked by the weekly rig | M7 → M8 | ≈ 3.5 + 2 at 144 Hz; ≈ 8.3 + 4 on a fixed 60 Hz panel | | | |

- **Fixed-refresh 60 Hz with V-Sync (FIFO):** the pacer targets the vblank, which adds its margin `m` and any
  vblank quantization left by a mispredicted frame. The gate for this configuration is p95 ≤ 45 ms.
- **Audio (informative):** cues leave Phase B in the same frame. With 02 §7's ≤ 30 ms device latency, input →
  sound is p95 ≤ 52 ms at 60 fps. It is tracked but not gated.
- **Linux:** the same stages apply, measured with X11 unredirected fullscreen or a Wayland compositor doing
  direct scan-out. The thresholds are ≤ 1.1 × Windows (the CL-21/AAA-PLT-5 parity rule).

**What makes non-camera input fast.**
1. **Late input sample.** Work that does not depend on this frame's input (Phase A) runs *before* the JIT
   sleep, so only Phase B (≤ 4 ms) plus extract sits between the sample and the render thread. Without the
   split, the full 8 ms game frame (instead of 4 + 1 ms) would push the 60 fps sum to 42.5 ms.
2. **Same-frame render.** In low-latency mode the render thread is idle when extract N lands, by construction.
   It starts frame N at once, and the GPU queue holds at most the executing frame plus the next batch 0. The
   "GPU executes N−2" row of 03 §2.5 occurs only in throughput mode.
3. **Chunked submission.** The render graph is submitted in 3 ordered batches: (0) GPU-scene scatter, culling
   and shadows, plus async-compute kicks; (1) prepass through forward opaque; (2) the rest, UI and present. The
   GPU starts ≤ 2.2 ms after extract. Batch 0 alone (≈ 2.4 ms of GPU work in 03 §8.1) outlasts the remaining
   ≈ 1.6 ms of recording, so the GPU never waits on later batches.
4. **Speculative cue step.** At 20–30 Hz zones, and at 120 fps even in 60 Hz zones, most frames run no command
   tick (04 §5.1), which alone would add up to one tick (50 ms at 20 Hz) to firing. So when Phase B samples a
   press on a frame with no tick due, the client runs that slot's ASM program (06 §1.4) on a **scratch copy** of
   its `AsmInstance` against the latest predicted state, up to its first `Wait`. It dispatches only the
   presentation outputs (`TriggerCue`, view-model `PlayMontage`, reticle and HUD state), stamped with the
   prediction key the next tick will allocate (`inputSeq<<8 | slot`). That tick then runs the real program.
   Cues deduplicate by key (06 §1.5). If the tick diverges, for example because ammo ran out in between, the
   speculative cues are cancelled through the normal reject path. No gameplay state changes outside ticks, so
   determinism, 04 §5.2's input format and server validation are unchanged. Server-only abilities show their
   pending state through the same step.
5. **Camera late latch.** When the render thread records batch 0, it reads the newest accumulated mouse delta,
   rebuilds the view and projection, and writes them into the frame's constants. The latched delta is reported
   back, so the next tick's look input does not apply it twice. To keep the latched frame correct:
   - CPU coarse culling at extract uses a frustum widened by max(3°, 2 × yaw rate × T);
   - GPU culling reads the latched view;
   - CSM cascades are sphere-fitted and texel-snapped, so they do not depend on rotation;
   - TAA and motion vectors use the latched previous view, stored per frame.

   Only orientation is latched, never position. Hit validation uses the tick orientation (04 §5.6); the two
   differ by at most one frame of mouse motion.
6. **UI and cursor.** RmlUi input events, and the layout of the focused and hovered documents, run in Phase B;
   other documents run in Phase A. In UI modes the pointer is the OS hardware cursor (`SDL_CreateColorCursor`),
   so pointer motion is composited by the OS within one refresh at any frame rate. The software cursor is used
   only on world-space `UiSurface`s (§1.8).

**JIT pacer (`FramePacer`, game thread).** Each frame records M1–M7. The pacer predicts the next frame's
Phase B + extract time (d̂B), render critical path (d̂R), GPU time (d̂G) and GPU-end → displayed time (d̂P).
Each prediction is the p90 over the last 32 frames, so one spike does not collapse the estimate. The sample
deadline is the later of two bounds, because sampling earlier than either only adds queueing:

```
t_sample = max( D_target − (d̂B + d̂R + d̂G + d̂P) − m ,     // display bound: frame-rate cap, VRR ceiling, or the next vblank under FIFO
                Ĝ_free   − (d̂B + d̂R)            − q )     // GPU bound: batch 0 lands q = 0.3 ms before the GPU goes idle
```

- `D_target` = the previous displayed time + the cap period. With VRR and no user cap, the cap is 3 % below
  the maximum refresh, so frames stay inside the VRR window. Under FIFO it is the next vblank, with the phase
  estimated from the M7 history.
- `Ĝ_free` = the predicted end of GPU work already submitted.
- The margin `m` starts at 1.0 ms. It grows by 0.5 ms whenever a frame is displayed > 1 ms after `D_target`,
  or the GPU idles > 1 ms while GPU-bound. It shrinks by 0.05 ms per on-time frame and is clamped to 0.3–4 ms.
- **Sleep.** Phase A runs first; the game thread then sleeps until `t_sample`. On Windows this uses
  `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` to `t_sample − 0.3 ms`, then a `_mm_pause`
  spin on QPC. On Linux it uses `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` with `PR_SET_TIMERSLACK` at
  50 µs, then a ≤ 0.2 ms spin. Wake-up error is ≤ 0.2 ms at p99. If Phase A overruns `t_sample`, there is no
  sleep and the margin grows.
- **No `present_wait`.** Some drivers lack it; lavapipe does (03 §1.2). There, M7 is estimated as GPU end plus
  a platform constant (0.5 ms for independent flip), calibrated nightly against PresentMon, and pacing runs on
  GPU completion alone (timeline-semaphore values plus calibrated timestamps). `DriverAdvisoryDef` records list
  the drivers where the lab found `present_wait` missing or unreliable. `VK_EXT_present_timing` replaces the
  estimate where drivers expose it **[verify availability]**.
- **Vendor providers (Phase 3).** `ILatencyProvider` implementations over the registered Khronos extensions
  `VK_NV_low_latency2` (`vkSetLatencyMarkerNV` fed from M2–M6, `vkLatencySleepNV`) and `VK_AMD_anti_lag`
  (`vkAntiLagUpdateAMD` at the input sample and at present). They replace only the sleep; markers, measurement
  and gates are unchanged. They are in-tree, with no SDK. Vendor names or logos in the UI need the vendor's
  programme sign-off (§4.6 licence row).
- **Settings:** Low latency *Off* (throughput) / *On* (default) / *On + Boost* (vendor provider only).

**Instrumentation.**
- **Markers.** `LatencyMarkers` in `engine/core` keeps a 256-frame ring of M1–M7 and GPU times per `FrameId`,
  about 20 timestamps per frame (≤ 5 µs). Each input gets an `InputId` that is carried through its prediction
  key or UI element to the responding frame.
- **Tracy** (`HELIOS_PROFILE` builds):
  - zones for Phase A, the JIT sleep, Phase B, extract, the render critical path, each submit batch and the
    present wait;
  - GPU zones through calibrated timestamps (03 §8.3);
  - plots `lat.action_ms`, `lat.camera_ms`, `lat.ui_ms`, `pacer.margin_ms` and `gpu.idle_ms`;
  - one `TracyMessage` per input, linking its `InputId` to its responding frame.
- **Gate runs** use the Release build with markers compiled in, plus one 60 s Tracy capture per scene for
  triage.
- **Live telemetry:** per-minute p50/p95 of camera, action and UI latency, from SDL timestamps, together with
  pacer misses, join §3's performance aggregates.
- **`helios-latbench` (nightly, H-class).**
  - Injects input at random phase (Poisson, mean 4 Hz) through Windows `SendInput` (the raw-input path) or a
    Linux `uinput` virtual mouse and keyboard.
  - Per scene and run: 3,000 fire presses, 1,000 ability presses, 3,000 mouse-look moves and 500 UI clicks.
  - Plays the harness rifle, the dash ability and HUD toggles under NetSim `good`, so reconciliation load is
    present.
  - A run fails if, on Windows, PresentMon reports composed rather than independent flip or its
    input-to-display time differs from M7 − M0 by > 2 ms. It also fails if, in an uncapped pass,
    low-latency fps is < 97 % of throughput-mode fps.
- **Weekly click-to-photon rig (W-class, a validation check rather than a gate).** An open-hardware USB HID
  clicker (1 kHz) and photodiode, one lab display per GPU vendor. The measured value must equal M7 − M0 +
  device + measured scan-out within ± 3 ms. Otherwise the software instrumentation is declared broken and
  CL-6 counts as *unmeasured*, which is failing (09 §5.6).

### 1.4 Settings and configuration

- **Machine scope** (display, graphics, audio device) lives in
  `%LOCALAPPDATA%\<Install>\<channel>\settings\machine.jsonc` (`$XDG_CONFIG_HOME/<productId>/<channel>/` on
  Linux). `<Install>` and `<productId>` come from the product descriptor (§2.10); every path, scheme, registry
  key and credential name in this section is scoped the same way.
- **Account scope** (keybinds, UI layout, chat tabs, addon settings) is a ≤ 256 KiB blob synced through the
  Character service, so it follows the player to another PC, as in WoW.
- **Cvars** are typed, with the `core/cvar.h` flags (02 §2.5): `Saved` (persisted to the machine file), `Cheat`
  (compiled out of shipping), `Restart`, `Replicated` (server-locked) and `Dev`; `+cvar=value` overrides them.
- **Scalability:** `ScalabilityGroupDef` records define groups (view distance, shadows, textures, effects,
  post, foliage, clouds, crowds, UI render-to-texture rate) at levels 0–4, and presets combine them. Auto-detect
  uses a GPU/VRAM table plus a 3 s GPU probe. Also: dynamic resolution, the upscaler choice (03), and a cost
  hint for each setting.
- **Latency and pacing (§1.3a):** Low latency Off / On (default) / On + Boost (vendor provider only); a frame
  cap (default: 3 % below the maximum refresh with VRR); V-Sync; a latency readout in the performance overlay.
- **Audio:** master, music, SFX, voice, UI and ambience buses. The output device follows the OS default.
  Dynamic-range presets, mono downmix, and a voice language separate from text.
- **Voice chat (Phase 3, 04 §2.7):** input device, push-to-talk binding (default) or voice activation,
  per-channel volume, proximity opt-in, "mute strangers", and per-player mute, block and report.

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

**Rules:**
- Tables use a virtualized `<datagrid>` element that only creates the rows on screen.
- **Nameplates, brackets and damage numbers are not RmlUi documents.** `WorldMarkerRenderer` draws them as
  instanced quads with SDF text, projected from f64, with screen-space declutter and CPU picking (R09 B15).
- 2D UI is composited **after TAA/upscaling, at native resolution**, then mapped to HDR at a user paper-white
  level (R09 B16).
- RML, RCSS and Luau hot-reload in dev and in the T19 UI designer (R08-T19).
- **Value moves are never optimistic** (§1.11): every panel that moves items or currency shows a pending state
  until the ledger acknowledges, and every trade, mail claim, market order, vendor or store purchase, restyle
  and collection reacquire is a protected action (§1.12).

#### 1.7.1 Panel contract and reskinning

Every Foundation panel is the same six-part unit, in the `foundation-ui` gem (02 §1.2: the Foundation layer
is a set of gems):

| Part | Path in the gem | Rule |
|---|---|---|
| Layout | `ui/<panel>/*.rml` | Elements that a flow or a player can act on carry a stable `id`; text comes only from `LocString` bindings |
| Style | `ui/<panel>/*.rcss` | Colours, sizes, radii, fonts and sprites come only from theme tokens (`$token`), never literals |
| View-models | `schemas/ui/<panel>.hschema` | `viewmodel` types. Fields bound with `@source(Component.field)` or `@source(Service.Stream)` get **generated** dirty-mask adapters, so no Foundation panel needs hand-written C++ |
| Controller | `ui/<panel>/<panel>.controller.luau` | Client Luau VM; reads view-models, sends intents through `clientcore` service clients or the net layer; uses the addon API (§1.12) |
| Input | the panel's `InputContextDef` | ModalUI, TextEntry, Map or Build (§1.5); gamepad focus via RmlUi's `nav-*` properties with a spatial fallback |
| Flow test | `tests/ui/flows/<panel>.flow.luau` | §1.7.3; a panel without a passing flow does not ship |

**Themes.** A `ThemeDef` record holds the tokens: semantic colours (hostile, friendly, neutral, loot tiers
0–6, surfaces, text, accent, warning) with a paired shape or icon for each (§1.9), a type scale, spacing and
radius scales, sprite-sheet references, UI sound cues and a `FontStackDef` per locale. `helios-cook` expands
`$token` references into one stylesheet set per theme; Settings switches sets without a restart, and dev
builds and T19 hot-reload them. The gem ships `foundation-dark`,
`foundation-high-contrast` and the colour-blind palettes. **A studio reskins without an engine edit** in two
ways, both content in its own gem:
1. a new `ThemeDef` plus sprite sheets and fonts, selected in `helios.project.jsonc` (`ui.theme`, and
   `product.branding.theme` for the launcher, §2.10);
2. overriding any Foundation `.rml`, `.rcss` or `.luau` file by path. Gems layer by priority, so the
   highest-priority file wins; T28 rule `ui.override-bindings` flags an override whose bindings no longer
   match the view-model schema.

#### 1.7.2 Foundation panels

The table covers every player-facing screen that 06's systems and the starter templates (09 §2.7.4) need.
Owning WPs are 09's: WP-1.20 (Ph1), WP-2.13 (Ph2), WP-3.8 (Ph3) and WP-4.9 (Ph4). Each panel lists its main
view-models (`…VM`, generated from `schemas/ui/`).

| Panel | Screens and key requirements | View-models | Ph (WP) |
|---|---|---|---|
| Frontend | Login; shard and character select over a 3D scene; queue position and ETA; handles `name#1234` (R03-P0-8) | `FrontendVM`, `ShardListVM`, `CharacterListVM`, `QueueVM` | 1 (1.20); queue UX 2 (2.13) |
| Character creator | Species and body type from `SpeciesDef`; parameter groups generated from `CustomizationParamDef` by kind (slider for Morph and BoneScale, swatches for PaletteColor, grids for MeshOption, TextureLayer and Decal). The preview quantizes exactly as `CharacterAppearance` does, so it equals what observers see (06 §10). Seeded randomize, 64-step undo, 8 preset slots in the account blob, face/body/full camera presets. Name entry checks `SpeciesDef` name rules locally as the player types; a rejection from Character `Create` (taken, reserved, filtered; 05 §1.5) returns to the name field with the failing rule. **Image-designer mode** opens the same panel on another player's vector after consent, and both players see one preview | `CreatorVM`, `AppearanceParamVM`, `NameRuleVM` | 1: species, body type, basic morphs (1.20) / 2: full parameter set (2.13) / 3: image designer (3.8) / 4: DNA blend (4.9) |
| HUD | Vitals; ability bar with predicted cooldowns; target; radar; ping/loss; **TiDi indicator when d < 0.95** (R01-P0-3); **interaction prompts** from 06 §9.6's `InteractPrompt` (verb, input glyph, disabled reason, hold progress); follower portraits and a command bar (06 §7.3); tracked objectives | `HudVM`, `AbilityBarVM`, `TargetVM`, `InteractPromptVM` | 1 (1.20); followers 2 (2.13) |
| Settings | Graphics presets with per-setting cost hints (§1.4), audio buses, latency options, accessibility (§1.9), privacy and legal links (§2.3); keybind rebinding with the conflict display (§1.5) | `SettingsVM`, `BindingVM` | 1 (1.20); rebinding 2 (2.13) |
| Maps | System orrery from f64 positions; galaxy map (jump graph, routes) as a 3D view with an RML overlay; **minimap** that switches between surface, interior and space scales; **surface maps** generated on the client from `engine/pcg` coarse tiles with `MapLayerDef` overlays and fog of war; **interior maps** per deck from cooked `PortalGraph` floor plans with live door and hazard state; markers from 06 §9.8, with hostiles shown only when detected | `MapVM`, `MarkerVM` | 1: orrery, minimap (1.20) / 2: galaxy, surface, interior (2.13) |
| Terminal host | Opens a `TerminalDef.ui` document with one tab per listed service (Market, Missions, Vendor, Bank, Mail, Crafting, Fitting, Customization, CrewMissions). Each tab hosts the panel from this table. The host's lifetime is the server's `TerminalSession`: at range + 2 m it closes with "Out of range" (06 §9.6). Renders on a `UiSurface` (§1.8) or in 2D, with a pop-out. The **Bank** tab moves items and currency between the character, its hangars and permitted org divisions | `TerminalVM`, `BankVM` | 1: host, mission board (1.20) / 2: all services (2.13) |
| Journal | Quests, generated missions and public events (06 §6.1–6.3): objectives with progress, tracking pins that feed the HUD and map markers, the mission board (accept, decline, standing requirements), quest sharing within the party | `JournalVM`, `ObjectiveVM`, `MissionOfferVM` | 2 (2.13) |
| Chat, social | Tabs, IME, item links, bidi-isolated names; friends, party, rosters of 10k members; emote list, emote wheel and paired-emote requests (06 §9.7), with emote text rendered locally from localized templates | `ChatVM`, `FriendsVM`, `PartyVM` | 2 (2.13) |
| Inventory, fitting | Drag and drop with a pending state; container trees (06 §2); socket and plug insertion; fitting preview through the shared `FittingValidator` (06 §2) with `.hfit` import and export | `InventoryVM`, `ContainerVM`, `FittingVM` | 2 (2.13) |
| Loot | Container window (06 §9.6): loot all or per item; need, greed or pass with the 30 s roll timer and the result list; "already taken" when another looter wins | `LootVM`, `LootRollVM` | 2 (2.13) |
| Market, contracts | Regional order-book grids (`<datagrid>`, CL-8); buy and sell order entry with the server's fee-context quote (broker fee, tax, 06 §4); my orders and history; contracts: exchange, courier (collateral, destination) and auction, browse and create. Player-vendor storefronts (06 §4) | `MarketVM`, `OrderBookVM`, `OrderEntryVM`, `ContractVM` | 2 (2.13); player vendors 3 (3.8) |
| NPC vendor | Buy, sell and buy-back tabs from `VendorDef`; server price quotes itemized into base, standing modifier and tax; stock and restock countdown; rows locked by an unmet requirement show it; each purchase is one idempotent ledger transaction with a spinner | `VendorVM`, `VendorQuoteVM` | 2 (2.13) |
| Trade window | Secure player trade (06 §4): two offer panes (items and currencies), a per-side **Accept**, then **Confirm**. Any change to either pane clears both accepts and highlights the changed rows, and Confirm stays disabled for 3 s after a change. It settles as one `AtomicSwap` with version checks, as a protected action | `TradeVM` | 2 (2.13) |
| Mail | Inbox (virtualized), read, compose with ≤ 50 recipients (05 §1.9); attachments and cash-on-delivery shown as escrowed; claim, return, delete; system notices; the 100 mails/h limit shown before sending | `MailboxVM`, `MailVM`, `ComposeVM` | 2 (2.13) |
| Crafting | **Schematic browser:** tree by category, search, filters (craftable now, station, skill). **Slots:** drag resource stacks or items per `SchematicDef.slots` entry, enforcing `identical` and quantity; each slot shows its escrow state. **Quality estimate:** per property, from 06 §3's weighted formula over the chosen stacks' public attributes, computed by the shared pure `CraftQuality` function that the cell also runs, and labelled "estimate". **Assembly** result, **experimentation** (allocate points across properties, ceiling bars, roll outcomes), **customization** (name, palette and `MeshOption` parameters with a 3D preview), **finalize** as a prototype or a manufacturing schematic. The panel mirrors the server state machine (`Open → … → Finalize`); after a reconnect it rebinds to the live session or shows the durable-timer refund. It hosts `SchematicDef.minigame` Luau documents. **Industry:** blueprint activities, inputs, server-quoted duration and fee, a job queue with durable-timer progress, and delivery | `SchematicBrowserVM`, `CraftSessionVM`, `ExperimentVM`, `IndustryJobsVM` | 2 (2.13); factories, invention 3 (3.8); minigames 4 (4.9) |
| Survey, harvesters | **Survey:** the resource-class tree; current spawns on this body with attribute ranges as bars; survey samples drawn as a heat-map overlay on the minimap and surface map, with a waypoint to the best sample; sampling progress on the HUD. **Harvester manager:** location, resource, concentration, rate, hopper fill, power, maintenance and time-to-full per harvester, projected on the client from `{lastEval, rate, hopper, power, maintenance}` for display only (the server stays authoritative, 06 §3); empty hopper, add power, pay maintenance, change resource, recall | `SurveyVM`, `SpawnListVM`, `HarvesterListVM` | 2 (2.13) |
| Placement, decoration | **Placement** (Build context): a ghost of the `StructureDef` footprint, coloured live by an advisory client pre-check that uses the cell's shared `PlacementCheck` sampler (slope, height delta, clearance against the zone's `ZoneStructures` manifest, city footprints and zoning, lot budget; 06 §9.2.1); 15° snap rotation, free with a modifier; a cost confirmation, then a spinner until the ledger acknowledges. **Decoration:** move and rotate gizmos clamped to interior bounds, an item-cap counter, undo of the last 20 moves (each an intent). **Structure panel:** ACL lists (admin, entry, ban, vendor), maintenance pool, decay stage, pack-up. **City panel:** rank, citizens, civic unlocks, taxes, treasury journal, elections (stand, vote) and the mayor's policy, bound to the `CityGovernance` world script through the cell (06 §9.2.4) | `PlacementVM`, `DecorVM`, `StructureVM`, `CityVM` | 2: placement, used by harvesters in M2 (2.13) / 3: housing, decoration, cities (3.8) |
| Organization | Roster (the 10k `<datagrid>`); a **role editor** over the schema's `Perm.*` names, grouped by domain (hangar and wallet divisions, structures, followers), with a diff preview before saving (06 §9.1); division names; **org wallets** with balances, a journal filtered by reason code and permission-gated transfers; applications and invites; titles; alliances; an audit-log view | `OrgVM`, `RoleEditorVM`, `OrgWalletVM`, `OrgAuditVM` | 2: roles, wallets (2.13) / 3: hangars, alliances (3.8) |
| Activity director, group finder | The **director** (06 §6.13): activities, tiers, modifiers, per-character lockouts and queue estimates. The queue panel: role selection, ETA and the 30 s ready check. The **group finder**: browse listings filtered by activity, tier, role and requirement; create a listing (140-character note); apply; the leader accepts. **Leaderboards** with shard, friends, guild and class partitions | `DirectorVM`, `QueueTicketVM`, `GroupFinderVM`, `LeaderboardVM` | 3 (3.8); ranked seasons and windows 4 (4.9) |
| Fleet | EVE-style fleet window (05 §1.12.1, 06 §8.3a). **Hierarchy tree** of fleet → wings → squads with commander slots, member rows (hull, zone, docked or in space from presence) and a filter; **drag and drop** of members between squads, wings and commander slots, gated by the viewer's rights, with a pending state and a refresh on `ROSTER_STALE`; invite by name, kick, promote, transfer boss, free-move and MOTD. **Broadcast bar** with one button per `BroadcastDef` and hotkeys, plus a broadcast history whose entries lock, align or warp in one click. **Fleet warp** to a selected object or bookmark at a chosen range, by fleet, wing or squad, showing each refused member's reason. **Adverts** (goal, join ACL, default squad) published to the group finder. Command bursts show on the HUD's effect bar and as range rings. Voice levels follow the slot (04 §2.7) | `FleetVM`, `FleetMemberVM`, `BroadcastVM`, `FleetAdvertVM` | 3 (3.8) |
| Killmail viewer | Personal, org and **fleet** lists (virtualized). Detail: victim, hull and fit (`.hfit` export), attackers with damage share and final blow, dropped and destroyed items with estimated value, location and time. Names resolve at display through `ResolveNames` (05 §1.5), so an erased character shows "Former pilot". A shareable `<uriScheme>://killmail/<id>` link (§2.10) | `KillmailListVM`, `KillmailVM` | 3 (3.8) |
| Dialogue | Letterbox, subtitles, choice wheel, group roll display, 30 s vote timer (06 §6.4) | `DialogueVM` | 2 (2.13); group 3 (3.8) |
| Progression | Skill and talent graphs; achievements with tiers and points; codex; legacy; companion influence and crew missions (06 §5, §7.3). **Collections** with **reacquire**: the server quotes the `Sink.Collection.Reacquire` price (06 §5.2), a confirmation shows price and balance after, then a spinner until the ledger acknowledges (protected action). **Season track** with free and premium lanes and claims (06 §5.3): premium ranks show locked with *Unlock premium*, which opens the season's Store offer; their claims enable when the entitlement lands, and the claim-window countdown warns 7 days and 1 day before unclaimed rewards are mailed | `ProgressionVM`, `AchievementVM`, `CollectionVM`, `SeasonTrackVM` | 2 (2.13); collections 3 (3.8); seasons 4 (4.9) |
| Overview | EVE-style sortable table of 1,000+ space objects | `OverviewVM` | 3 (3.8) |
| Customization | The terminal host's `Customization` tab (06 §9.6). **Appearance (restyle):** the character creator's parameter groups on the live character, with species and body type locked and the same quantized preview, so the preview equals what observers see. The cell quotes a price per changed group (itemized, with the balance after); *Apply* sends one `Restyle{vector, quoteVersion}` intent inside the terminal session and shows a spinner until the ledger sink is acknowledged (06 §10). An option gated by `unlock` shows its source (collection, legacy or Store) with a link. **Liveries:** for each owned hull in the terminal's hangar, the liveries unlocked through collections or entitlements (06 §5.2), previewed in 3D on the per-instance livery data (03 §4.2); *Apply* sends `SetLivery{ship, livery}`. Walking out of range closes the tab with "Out of range" and discards the unapplied edit | `CustomizeVM`, `RestyleQuoteVM`, `LiveryListVM` | 3 (3.8) |
| Support and report | **Report a player** from any name (chat line, nameplate, roster, fleet, group finder, killmail) or from the voice speaker list, which lists everyone heard in the last 60 s. The form has a category (harassment or hate, cheating or botting, spam or RMT, offensive name, threat of real-world harm, other), a ≤ 500-character note and **evidence**. Chat evidence is up to 20 lines the player received, picked in the chat view and sent with each line's server `msg_id` and evidence tag (05 §1.10), so it cannot be forged. Voice evidence comes from *Report voice*, which asks the forwarder to snapshot the speaker's 60 s ring as soon as it is clicked, before the form is filled (04 §2.7). Zone, position, tick and both character IDs are attached automatically. The subject is always an ID the server sent, never a typed name. *Also block this player* defaults to on for harassment, and the threat category shows the product's crisis-resources text. **Support tickets** cover account and billing (attaches a receipt from the Store history), item or currency loss (attaches a ledger transaction picked from the player's own wallet or item history), a stuck character (attaches zone and position for a GM teleport), a bug (attaches §3's log ring and breadcrumbs after explicit consent) and other; text is ≤ 2,000 characters. **My cases** lists reports and tickets with their state (received, under review, waiting for you, closed) and the expected first-response time from the case's SLA. Tickets open as a threaded view with staff shown by alias, where the player can reply or close with a 1–5 rating. Reporters see only "action taken" or "no action", never the sanction. Staff replies also arrive as a system notice. The limits (10 reports an hour, 3 per subject a day, 5 open tickets; 05 §1.17) are shown before sending. A player who cannot sign in uses the launcher's *Can't sign in?* link (§2.3) | `ReportVM`, `EvidencePickerVM`, `TicketVM`, `CaseListVM`, `CaseThreadVM` | 3 (3.8) |
| Store | **Catalogue** from `ListOffers` (05 §1.20): featured items, cosmetics, season passes and currency packs. Each offer shows the payment provider's localized price, what it grants, and a 3D preview of cosmetics in the Customization preview scene; owned offers are marked. **Owned:** entitlements with their source (purchase, collection mirror, season, grant), the premium-currency balance, and receipt history with refunds. **Real-money checkout:** `CreateCheckout(offer, priceVersion, idem)` returns a provider URL. It opens in the **system browser**, or in the storefront overlay through `IPlatformServices` on Steam and Epic SKUs. Payment data never touches the client, and there is no embedded web view. The panel shows *Waiting for payment* and follows `CheckoutStatus` (`pending → paid → granted`, or `failed`, `cancelled`, or `expired` after 30 min). Only the provider's webhook grants, so closing the client loses nothing, and the next login shows *Purchase delivered*. **Premium-currency purchases** are one idempotent ledger transaction behind a confirmation that shows the price and the balance after, then a spinner (protected action). **Ages 13–17:** `GetSpendStatus` puts the remaining monthly allowance beside every price. An offer above it is disabled with "Monthly limit reached — resets <date>", and randomized offers are hidden. The server enforces both anyway (`SPEND_CAP_EXCEEDED`). Randomized offers always show their odds | `StoreVM`, `OfferVM`, `EntitlementListVM`, `CheckoutVM`, `SpendCapVM` | 4 (4.9) |

**Coverage.** M2 "Osk Yard" (09) uses Survey, Placement, Crafting, Inventory, Market, Journal, Dialogue,
Chat and Organization, all Ph2 rows. The starter templates (09 §2.7.4) map as follows: `starter-sandbox` →
Survey, Crafting, Placement, NPC vendor, player vendors; `starter-fleet` → Fitting, Market, Organization,
Killmail viewer, Overview, Fleet; `starter-shooter` → Activity director, Loot, Progression; `starter-seamless` →
Maps, Terminal host, HUD; `starter-story` → Dialogue, Journal, Progression. Every template ships Support and
report. Customization appears wherever a terminal lists that service, and Store wherever the product sells
entitlements (`starter-shooter` and `starter-story` enable season passes). A template adds its own screens
through the same contract; `template-proof-<id>` runs the flows of the panels its class uses.

#### 1.7.3 Headless UI flows, lints and the reskin proof (CL-23)

- **Flows.** `helios-client --headless --rhi=null --script=tests/ui/flows/<panel>.flow.luau` runs the real
  RmlUi documents against a dev `helios-backend` and a headless cell loaded with a seeded fixture project.
  - The Luau `UiTest` module has the verbs of 07 §4.4 (`click`, `drag`, `type`, `key`, `scroll`,
    `waitFor(path | predicate, timeout)`, `capture`). It addresses elements by stable paths
    (`craft/slot[2]`, `market/orders/row[order:81]/cancel`) and injects events into the RmlUi context by the
    same route as SDL events.
  - Each flow runs its **happy path**, its **reject paths** and two **input-only passes**: keyboard only, and
    gamepad focus navigation only, with no pointer. Examples of reject paths: a trade pane changed after Accept
    clears both accepts; a 51st mail recipient is refused; a placement on too steep a slope comes back with the
    server's reason; a crafting session killed mid-experiment shows the refund; a restyle to a locked option
    or outside the species range is refused; a report whose chat line carries a forged or altered evidence tag
    is refused (`EVIDENCE_INVALID`), and an 11th report in an hour is rate-limited; a checkout by the 15-year-old
    fixture account above its remaining allowance is refused (`SPEND_CAP_EXCEEDED`); a replayed provider
    webhook grants once; a stale `priceVersion` re-quotes.
  - **Store and support fixtures.** The Store flow runs against the dev backend's fake payment provider
    (05 §1.20). The test plays the provider: it posts signed `paid`, `refunded` and replayed webhooks and
    asserts one grant per receipt. The Support flow files a chat report, a voice report (the fixture fleet
    channel has a bot speaker) and a ticket. It then asserts through the admin API that the case is in the
    queue with its category's priority and `first_response_due`/`resolve_due`, and that the chat evidence is
    marked verified. A staff reply and a state change made through the admin API must reach the player's
    thread and notice.
  - Assertions cover view-model state **and** the authoritative outcome: ledger history through the admin
    API's `dev` role (05 §1.17), and mail, market, structure and org state through the services' own `List`
    and `Get` calls made as the test character.
  - Determinism: fixed dt, animations off, seeded fixtures. Each flow takes ≤ 60 s; the full suite takes
    ≤ 20 min per OS nightly. The PR tier runs the flows of changed panels plus the lints at 100 %.
- **Layout lints** (07 §4.4's ED-15 rule set, adapted to RmlUi) at 75, 100, 150 and 200 % UI scale, with +40 %
  pseudo-localized text and the CJK and RTL test locales, on a 1280 × 720 viewport:
  - text clipped inside an element, overlapping interactive elements, or an element outside the viewport;
  - an interactive element with no localized label or tooltip, or a string literal in RML;
  - token contrast below 4.5:1, or below 7:1 in the high-contrast theme;
  - an interactive element that keyboard focus cycling or gamepad spatial navigation cannot reach;
  - a colour, size or font literal in panel RCSS instead of a token;
  - a binding to a field that is not in the panel's view-model schema.
- **Reskin proof.** The `ui-reskin-fixture` gem has no C++. It holds a `ThemeDef` that changes every token,
  replacement sprite sheets, a different font stack, and overrides of three RML files that re-lay out the
  HUD, inventory and market. Every flow and lint runs again under it. Its ꟻLIP goldens are kept separately,
  and each panel must differ from the default skin in ≥ 30 % of pixels, which proves the skin applied. The job
  fails if any engine or Foundation-gem binary differs from the SDK hash (the `template-proof` rule).
- **Goldens.** Lavapipe screenshots cover {en, pseudo, CJK, RTL} × {75, 100, 200 %} × {default, high contrast,
  colour-blind palettes} per panel (§4.5). T19 runs a panel's flow from its toolbar with the same harness.

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
| **Protected actions** | Movement, targeting, abilities, market orders, trades, mail, vendor and store purchases, restyles, collection reacquires, structure placement and player reports need a *secure context*: a hardware input event dispatched to a signed Foundation handler with no addon code (taint) on the call path. This blocks bots, automated rotations and addon-driven mass reporting |
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

A tiny stable bootstrap (`<Install>.exe`) starts `app-<build>/<Install>Launcher.exe`, which links `engine/core`,
`app`, `ui`/`text`, `patch` and `crash`. `<Install>` is the product's `installName` (§2.10). The SDK ships both
as prebuilt binaries named `Helios.exe` and `HeliosLauncher.exe`, which run unstamped only as the dev product;
stamping renames them (§2.10.4). RmlUi draws through **`SDL_Renderer`** (D3D11/D3D12 on Windows,
OpenGL/Vulkan on Linux, R10-§8).

**Why not ImGui:**
- The launcher is the first screen players see and needs shaping, RTL, accessibility, UI scale and rich news.
  ImGui has none of these (R08 F1), and ADR-010 keeps it editor/debug-only.
- The launcher **shares skins, fonts and localization** with the client frontend: the product's launcher
  `ThemeDef` and RML overrides (§1.7.1, §2.10.5).
- `SDL_Renderer` needs no Vulkan, so the launcher itself can report a missing Vulkan 1.3 driver.

This amends R10-§13's "ImGui … launcher UI" row (§4.8).

**Build.** The launcher links no Jolt and no AVX2 code. It and the bootstrap are 02 §1.1's **`base`** images:
they are compiled for **baseline x86-64 (x86-64-v1, SSE2)**, with their own `@base` builds of every module and
library they link (§2.1.1).

#### 2.1.1 The launcher's CPU floor — decision: **the whole launcher closure is x86-64-v1**

**The problem.** The launcher's one CPU job is to explain an unsupported CPU (R10-§5). Its link closure is
`core`, `app`, `ui`, `text`, `loc`, `patch` and `crash`, plus SDL3, RmlUi, FreeType, HarfBuzz, SheenBidi,
libunibreak, zstd, Monocypher, yyjson and sentry-native. If any of that were built for x86-64-v2 (SSE4.2,
POPCNT), a Core 2 or Phenom II class CPU would fault in the launcher instead of seeing the message. An
emulator run on Nehalem cannot catch this, because Nehalem implements SSE4.2 and POPCNT.

| Option | Cost | Verdict |
|---|---|---|
| **Build the launcher closure as an x86-64-v1 flavour** (02 §1.1's `base` level) | Nothing new: 02 already builds `<module>@base` object libraries for the launcher. The patcher's hot loops self-dispatch (below), so CL-10 and CL-11 are unaffected | **Chosen.** "Runs on any x86-64 CPU and explains an unsupported one" stays true |
| Declare x86-64-v2 the launcher's floor | None | Rejected. Windows 10 22H2, which the product supports (§2.2), still runs on pre-v2 CPUs, and they would get a raw fault |

**Flags.** `base` means the MSVC x64 default (no `/arch` flag, so SSE2), or `-march=x86-64 -mtune=generic`
with clang-cl, GCC and Clang, with no `-mcx16` and no `-msse3` or later. The same flags apply to the
launcher's third-party libraries, so their `__SSE4_1__` and `__AVX2__` paths compile out.

**Speed without raising the floor.** The launcher's hot loops choose wider code at run time by CPUID. They do
it only in functions listed by symbol on 02 §1.1's self-dispatch list:
- BLAKE2b's SSE4.1 and AVX2 compression functions (verify, repair and chunk checks);
- zstd's `DYNAMIC_BMI2` Huffman decoder;
- SDL3's CPUID-guarded blitters and audio converters.

Every CPU that can run the game (§2.2) takes the AVX2 paths, so CL-10 and CL-11 measure those. The SSE2
fallbacks only have to reach the refusal screen.

**Audit** (RT-09; 02 §1.1 checks 2 and 4). Every instruction in the linked launcher and bootstrap images,
and in the gate object of every `avx2` image, is mapped to its symbol. The build fails on any SSE3, SSSE3,
SSE4.1, SSE4.2, POPCNT, LZCNT/TZCNT, MOVBE, CMPXCHG16B, VEX, EVEX or BMI encoding outside a self-dispatch
symbol. The prebuilt CRT, the MSVC STL's vectorized algorithms and glibc dispatch themselves and are not
scanned; CL-17's emulator runs cover them.

**Execution** (CL-17). The launcher is run on emulated pre-v2 CPUs:
- on Windows, under Intel SDE `-mrm` (Merom: SSSE3, no SSE4.1) and `-pnr` (Penryn: SSE4.1, but no SSE4.2
  or POPCNT), with the chip check restricted to the main executable (`-chip-check-exe-only`). That checks
  our statically linked image and not the OS's own DLLs, which on Windows 11 24H2 may use SSE4.2;
- on Linux, under `qemu-x86_64 -cpu Opteron_G1` (SSE2 only), `core2duo` and `phenom`.

It must reach its refusal screen with 0 illegal instructions reported. The v2-without-AVX2 runs (SDE `-nhm`
and `-snb`, qemu `Nehalem` and `SandyBridge`) stay. `--gate-report=<file>` writes the verdict and the
detected features as JSON, so CI reads the result without driving the UI.

### 2.2 CPU and GPU gate

- **CPU** (R10-§5). The gate checks CPUID for AVX, AVX2, BMI1, BMI2, LZCNT, POPCNT, F16C and SSE4.2, plus
  OS YMM state (XCR0). The **launcher** runs on any x86-64 CPU (§2.1.1) and shows the refusal in RML:
  *"<displayName> requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: <model>."* It
  exits with code 78 when closed.
- **The client repeats the check** in 02 §1.1's gate TU. That TU is compiled at `base` (x86-64-v1) and runs
  before any other code in the image, third-party pre-`main` hooks included. On Windows it is the first TLS
  callback (`.CRT$XLA0`) of the first-initialized image, ahead of mimalloc's TLS callback and Tracy's
  `.CRT$XCB` statics, and it exits through `TerminateProcess`. On Linux it is the `.preinit_array` entry.
  So a client started without the launcher (storefront SKUs, §2.9) also explains a pre-v2 CPU. An
  illegal-instruction backstop shows the same message, and the client exits with code 78.
- **GPU:** Vulkan 1.3 plus required features, with a vendor driver link on failure. `DriverAdvisoryDef` records
  in live config flag known-bad drivers.
- **Also:** disk space and Windows 10 22H2+; a warning below MIN RAM.

### 2.3 Login and account flow

1. **Phases 1–3:** password over TLS 1.3 to Identity (argon2id), which returns a 10-min JWT and a 30-day
   rotating refresh token (05 §1.1).
2. **"Remember me"** stores only the refresh token, in Windows Credential Manager (`CredWriteW`, DPAPI; target
   `<publisherId>/<productId>/refresh/<accountId>`) or the Linux Secret Service (schema
   `<publisherId>.<productId>.Refresh`, attributes `account` and `channel`). Without a secret service it warns
   and does not persist.
3. **Phase 4:** OIDC auth-code + PKCE in the **system browser** with a loopback redirect (RFC 8252) and no
   embedded web view; TOTP MFA; Steam/Epic federation via `IPlatformServices`.
4. **Hand-off:** the launch code goes over the inherited pipe (§1.1); refresh tokens never leave the launcher.
   `traceparent` lets one trace span launcher → queue → gateway (05 §6.2).
5. **Legal and age (Phase 2, 05 §1.1, §6.6):** registration collects date of birth and country for the age gate.
   When Identity answers `LEGAL_ACCEPTANCE_REQUIRED{doc, version, url}`, the launcher fetches that ToS, EULA or
   privacy-policy version, checks its SHA-256, renders it as sanitized RML (§2.4), and sends
   `AcceptLegal(doc, version, sha256)`. No launch code is issued before that, and a new re-acceptance version
   prompts again. Settings → Privacy links to the `ExportMyData` and `EraseAccount` web flows (re-authentication
   required).
6. **Support before sign-in (Phase 3):** the login screen's *Can't sign in?* link opens the product's
   `endpoints.support` web form in the system browser. The form files an account-access ticket through 05
   §1.17's `OpenAccessTicket`, and the reply goes by email. Signed-in players use the in-game Support and
   report panel (§1.7.2).

### 2.4 News, status and channels

- **News:** a JSON feed per locale from the product's `endpoints.news`, signed by the `news` subkey of the
  product's keyset (§2.10.3). It is rendered as **sanitized RML** (no script or bindings; images only from
  allow-listed CDN hosts) and cached offline. Patch notes are per-build sidecars.
- **Status:** shard population band, queue ETA, and incident banners from live config.
- **Channels (05 §7):** `live` and `ptr` for players, `beta` by entitlement, `dev`/`qa` internal only. Each
  channel has its own install directory, but all channels of one product share its chunk cache. PTR installs
  are seeded from live (EVE's shared cache, R01 §8). Caches are never shared between products (§2.10.2).

### 2.5 Content-addressed chunk patching

**Fixed by 05 §7:** FastCDC (16/64/256 KiB), BLAKE2b-256 chunk IDs, zstd-19 on the CDN, packs for chunks under
32 KiB, `.hman` manifests with tiers 0/1/2, and a signed pointer with `sequence` (anti-rollback),
`rollout_pct` and `min_launcher`. `engine/patch` also implements FastCDC in C++ for local publishing and
tests, sharing golden vectors with Go.

```
%LOCALAPPDATA%\Programs\<Install>\     <Install>.exe (bootstrap), launcher.json, app-<build>\   (R10-§9)
<Library>\<Install>\<channel>\         default %LOCALAPPDATA%\<Install>\Games, or any NTFS/ext4 drive
   bin\ (tier 0)   content\*.hpak (≤ 2 GiB, sparse while streaming)   staging\   install.lock
   install.db      schema-generated snapshot + fsync'd append-only journal (no SQLite, ADR-014)
%LOCALAPPDATA%\<Install>\cache\chunks\ the product's chunk cache (default cap 10 GB)
```

Linux uses the same tree under `~/.local/share/<productId>/` (launcher, games) and
`$XDG_CACHE_HOME/<productId>/chunks` (§2.10.2).

`install.db` stores each file's manifest entry, chunk offsets and **per-chunk residency bitmap**, plus the last
accepted pointer `sequence`, keyset `version` and root epoch (the ratchets of §2.10.3).

1. **Trust chain.**
   - The product's root public keys (current and next) come from the binary's stamped product block
     (§2.10.4); a root signs the product's keyset at `/keys/<productId>/keyset.json` (05 §7). The keyset is
     fetched with every pointer check and cached in `install.db`. It needs a `version` ≥ the stored one and
     must name the same `productId`, so another product's keyset, pointer or manifest never verifies.
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

**Pre-download.** A pointer field `next{build_id, manifest_hash, compat_epoch, available_at}` makes the launcher
fetch the next build's missing chunks into the shared cache: resumable, rate-limited and disk-checked. Release
day then applies locally in minutes. Optional (Phase 4): spoiler files stay XChaCha20-encrypted until the key is
published. When `next` carries a new compat epoch (a coordinated flip, 05 §1.14.1), the gateway disconnects the
session at `available_at` with reason `CONTENT_EPOCH_FLIP` and a reconnect ticket. The client shows *"Update
ready — rejoining"*, applies the pre-downloaded build (step 7 of §2.5, renames only) and rejoins through the
reconnect lane without queueing. Same-epoch client builds never interrupt a session.

### 2.7 Self-update

1. The launcher has its own pointer and manifest; the game pointer's `min_launcher` forces an update.
2. The new build is downloaded to `app-<new>\` and verified. Its stamped product block must carry the same
   `productId`, and its root pair must equal the running one or be endorsed by it (the rotation record of
   §2.10.3); otherwise the update is refused and reported.
3. `launcher.json.tmp` is moved over `launcher.json` with `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` (an
   atomic same-volume rename), then the launcher relaunches through the bootstrap.
4. The new launcher writes `healthy` once its UI is up and a pointer fetch succeeds. After two failed starts the
   bootstrap reverts; the two previous builds are kept.
5. Rarely, the bootstrap updates itself: the running `<Install>.exe` is renamed to `<Install>.old.exe` (Windows
   allows this) and deleted on the next run (R10-§9).

### 2.8 Installer, signing, uninstall

**Installer — decision: a self-installing launcher** (`<Install>Setup.exe` = the stamped launcher with
`--install`, produced by T29's *Package & publish* profile, §2.10.5). Like the launcher, it is x86-64-v1, so
on an unsupported CPU it shows the refusal instead of installing (§2.1.1).
- **MSIX is rejected:** its read-only package directory breaks self-patching, and it conflicts with
  anti-cheat drivers.
- **WiX is rejected:** MS-RL licence plus a maintenance-fee EULA.
- **Inno Setup 7.1:** only an optional Phase 4 partner wrapper, pending sign-off on its About-box clause
  (R10-§9, §14).

**Install.** Per-user, with no UAC. The user picks a library folder (filesystem and free space are checked).
The launcher goes to `%LOCALAPPDATA%\Programs\<Install>`, with Start-menu and optional desktop shortcuts named
by `displayName`, an HKCU uninstall entry under `<publisherId>.<productId>` and an HKCU `<uriScheme>://`
handler (§2.10.2). Only a vendor anti-cheat bootstrapper, if adopted, ever elevates.

**Code signing** (AAA-PLT-6, SEC-6). Every PE (exe, dll, `crashpad_handler`) gets Authenticode SHA-256 with an
RFC 3161 timestamp, **after** stamping (§2.10.4), so the signature covers the product block. Keys are
HSM-held: Azure Trusted Signing or a cloud-HSM OV/EV certificate (eligibility **[unverified]**). Signing runs
only from the protected release branch, with two-person approval, and `signtool verify /pa /all` gates CI.
Each studio signs its own product with its own certificate; the Helios project signs only its own
products (the dev product, *Cinder Reach* and the SDK).

**Uninstall** (`--uninstall`, from Apps & Features). Optionally keeps game data. Removes directories,
shortcuts, registry entries, the handler and stored credentials, but keeps screenshots. It touches only names
derived from its own product block, so other Helios-based products are unaffected (CL-24). It deletes itself
last through a helper in `%LOCALAPPDATA%\<Install>\.uninst`, never `%TEMP%`.

### 2.9 Linux packaging and storefronts

- **Linux:** a tarball in Phase 1 (dev/QA). An **AppImage** in Phase 3 (AAA-PLT-6) that holds only the
  bootstrap and installs the launcher under `~/.local/share/<productId>/launcher/`, so self-update and
  patching match Windows. T29's *Package & publish* profile builds it (§2.10.5). Flatpak is optional in
  Phase 5; deb/rpm are non-goals.
- **Storefronts (Phase 4+):** Steam or Epic depots patch those SKUs, and our launcher is not used (R07 §7).
  The client's own gate still explains an unsupported CPU (§2.2). An isolated `IPlatformServices` plugin
  (Steamworks and EOS are proprietary) provides auth tickets, overlay, presence, achievements and the
  storefront's purchase overlay for the Store panel (§1.7.2). The client's own login and pointer check (§1.1)
  cover this path.

### 2.10 Product identity and branded builds

Every game built on Helios is its own **product**. The launcher, client, installer, crash handler and
telemetry take every OS-visible name, endpoint and trust root from one product descriptor. So two Helios games
install side by side on one PC without colliding, and a studio on the binary SDK ships a branded, signed
client and launcher **without rebuilding the engine** (01 §1.1 north star; AAA-TOOL-9; CL-24).

#### 2.10.1 The product descriptor

The `product` block of `helios.project.jsonc` (09 §2.7.2) is the only source. An example:

```jsonc
"product": {
  "productId":   "acme-starfall",              // ^[a-z][a-z0-9-]{2,31}$
  "publisherId": "com.acme-games",             // reverse DNS the studio controls
  "displayName": { "en": "Starfall", "de": "Starfall" },   // one entry per shipped language
  "installName": "Starfall",                   // ^[A-Za-z][A-Za-z0-9]{2,23}$: folders, executables
  "uriScheme":   "starfall",                   // RFC 3986 scheme, checked against the reserved list
  "version":     "1.4.0",                      // VERSIONINFO, About box, crash release tag
  "executables": { "bootstrap": "Starfall", "launcher": "StarfallLauncher",
                   "client": "Starfall-Client", "setup": "StarfallSetup" },
  "endpoints": {                               // https only; stamped into binaries (§2.10.4)
    "identity":  "https://id.starfall.example",
    "cdn":       ["https://cdn1.starfall.example", "https://cdn2.starfall.example"],
    "crash":     "https://crash.starfall.example/api/7/",   // the product's crashgw (§3)
    "telemetry": "https://tel.starfall.example/v1",          // HTTPS telemetry before the world (§3)
    "news":      "https://cdn1.starfall.example/news/",
    "legal":     "https://starfall.example/legal/",
    "support":   "https://starfall.example/support/"     // pre-sign-in ticket form (§2.3)
  },
  "channels": ["live", "ptr"],
  "branding": { "theme": "branding/launcher.theme.jsonc", "rmlOverrides": "branding/launcher/",
                "icon": "branding/icon.ico", "iconPng": "branding/icon-512.png",
                "splash": "branding/splash.png", "installerBanner": "branding/banner.png",
                "eula": "legal/eula.{lang}.md" },
  "signing": { "windows": "azure-trusted-signing", "appimage": "gpg:<key id>" },  // or pkcs11 | pfx-dev
  "keys": { "root": ["ed25519:<current fingerprint>", "ed25519:<next fingerprint>"] }  // written by product init
}
```

**`helios-tool product lint`** (also T28 rules `product.*`) fails on:
- a malformed ID, or a `productId` or `installName` that starts with `helios` (reserved for the engine's own
  products);
- a reserved or unsafe URI scheme: `helios` and `helios-*` (the editor's links, 07 §1.2–1.3), `http`, `https`,
  `file`, `ftp`, `mailto`, `data`, `javascript`, `ms-*`, `steam`, `com.epicgames.launcher`, `discord`; a scheme
  already registered on the build machine is a warning;
- an `installName` or executable name that is a Windows reserved device name (`CON`, `PRN`, `AUX`, `NUL`,
  `COM1–9`, `LPT1–9`);
- a non-HTTPS endpoint, a missing `displayName` for a shipped language, or missing EULA text;
- an ICO without the 16, 20, 24, 32, 40, 48, 64 and 256 px images, or a PNG icon under 512 px;
- `keys.root` fingerprints that do not match the keyset signer.

**Engine-owned products.** Unstamped SDK binaries run as the dev product `helios-dev` (installName
`HeliosDev`, `localhost` endpoints, a dev root committed in the repository, no URI scheme). The reference game
ships as `cinder-reach` (`CinderReach`, scheme `cinderreach`). Every CL criterion runs against one of these two.

#### 2.10.2 What is scoped

| Item | Windows | Linux |
|---|---|---|
| Launcher | `%LOCALAPPDATA%\Programs\<Install>\` | `~/.local/share/<productId>/launcher/` |
| Default library | `%LOCALAPPDATA%\<Install>\Games\<channel>\` | `~/.local/share/<productId>/games/<channel>/` |
| Chunk cache | `%LOCALAPPDATA%\<Install>\cache\chunks\` | `$XDG_CACHE_HOME/<productId>/chunks/` |
| Machine settings | `%LOCALAPPDATA%\<Install>\<channel>\settings\` | `$XDG_CONFIG_HOME/<productId>/<channel>/` |
| Crash database, logs | `%LOCALAPPDATA%\<Install>\{crash,logs}\` | `$XDG_STATE_HOME/<productId>/{crash,logs}/` |
| Uninstall entry | `HKCU\…\Uninstall\<publisherId>.<productId>` | `--uninstall`, also a `.desktop` action |
| URI handler | `HKCU\Software\Classes\<uriScheme>` | `<publisherId>.<productId>.desktop` with `MimeType=x-scheme-handler/<uriScheme>` |
| Shortcuts, app identity | `<displayName>.lnk`; AppUserModelID `<publisherId>.<productId>` | `.desktop` with `StartupWMClass=<productId>` |
| Refresh-token credential | Credential Manager target `<publisherId>/<productId>/refresh/<accountId>` | Secret Service schema `<publisherId>.<productId>.Refresh` |
| Single-instance lock | mutex `Local\<publisherId>.<productId>.launcher` | abstract socket `@<publisherId>.<productId>.launcher` |
| Executables | `product.executables.*` | the same names, lowercased |
| Endpoints (identity, CDN, crash, telemetry, news, legal, support) | stamped `endpoints.*` | the same |
| Trust | stamped root pair; keyset at `/keys/<productId>/keyset.json` (05 §7) | the same |

**Rules.**
- Code never builds these names. `helios::core::ProductIdentity` (`productId()`, `paths()`, `endpoints()`,
  `roots()`) is the only source, and a CI grep fails on `"Helios"`/`"helios"` literals in path, registry,
  credential, pipe or scheme construction in `apps/` and `engine/{patch,crash,clientcore,platform}`.
- Nothing user-visible says "Helios" except the About box and the licence notices; a "Powered by Helios"
  credit is optional.
- Products share nothing: no cache, credential, settings file, crash database, lock or handler. Each product
  runs its own backend deployment (05) with its own keys, so product A's launch codes, tokens, pointers and
  keysets mean nothing to product B.

#### 2.10.3 Product keys: `helios-tool product init`

The key ceremony creates the product's trust chain once. Its outputs:

| Output | Where it lives | Rotation |
|---|---|---|
| **Root pair** (Ed25519 `current` and a pre-committed `next`) | Offline only. Each private key is split into 3-of-5 Shamir shares over GF(2⁸) (in-tree, with published test vectors) on five age-encrypted USB sticks and printed QR sheets, held by named custodians in two locations. The whole key never touches a disk. A PIV smartcard with Ed25519 is an option **[verify firmware support]** | `next` is promoted after a compromise or every 3 years |
| **Subkeys:** `manifest` (game and launcher pointers and manifests), `news` (news and status banners, §2.4), `addons` (the Ph5 portal, §1.12) | Production: Vault/KMS; staging: SOPS/age; dev: a local file (05 §6.5). They leave the ceremony encrypted to the studio's age recipient | Quarterly, with a 30-day overlap (05 §6.5) |
| **`keyset.json`** `{productId, version, keys[{id, role, pub, notBefore, notAfter}], sig}` | CDN `/keys/<productId>/keyset.json` | Re-signed offline at each subkey rotation; `version` only grows |
| **Product block** `product.hprod`: the security-relevant descriptor fields (IDs, names, scheme, endpoints, root pair, minimum keyset version), signed by the current root | Stamped into binaries (§2.10.4); a copy is committed | Re-issued when endpoints, scheme or roots change |
| **Ceremony record** `keys/ceremony-<date>.json`: public keys, BLAKE2b fingerprints, participants, custodians, tool version | Committed to the project | One per ceremony |

**Procedure.**
1. `helios-tool product init --offline` runs on an air-gapped machine booted from a live USB, following the
   SDK's printed checklist. It refuses to start while any non-loopback interface is up (`GetAdaptersAddresses`
   or `getifaddrs`), and it records two named participants.
2. It generates the keys from the OS CSPRNG (`BCryptGenRandom`, `getrandom`), splits the roots, signs keyset
   v1 and the product block, and prints the fingerprints for the participants to compare.
3. Only public material, the signed artefacts and the encrypted subkeys go to the transfer medium. The
   project imports them with `helios-tool product import`.

**Rotation and recovery.**
- `product rotate-subkey` signs a new keyset offline. Four quarters can be pre-signed in one session.
- `product revoke-subkey` signs a keyset without the subkey; pointers signed by it then fail everywhere once
  the keyset's 60 s CDN cache expires (05 §7).
- `product rotate-root` promotes `next`: a keyset signed by `next` is accepted, the install stores the new
  root epoch and never accepts the old root again, and the next launcher self-update carries a product block
  with the new pair (`next`, a fresh `next₂`) signed by `next`, which §2.7 step 2 accepts because the running
  block names `next`.
- Losing up to two shares of a root is harmless. An annual drill reconstructs each root offline, signs a test
  message and verifies it.
- `product init --dev` makes throwaway keys in `<project>/.helios/devkeys/` (git-ignored) that only
  `localhost` CDNs serve. `--ceremony-sim` runs the full offline flow with simulated shares, so CI
  (`template-proof`, CL-24) exercises the real code path.

#### 2.10.4 Stamping prebuilt binaries

- **Prebuilt.** For each release the SDK ships shipping-flavour `helios-client` (all Foundation gems linked
  statically; enough for projects written only in Luau, records, graphs and UI, 09 §2.7.4, including project
  schema types, which ship as `.htypes` data rather than code, 02 §3.8), the launcher, the
  bootstrap and `crashpad_handler`, for win64 and linux64. They are **unsigned**, so that stamping does not
  break a signature; the SDK's own signed manifest (09 §2.7.2) proves their integrity.
- **Reserved space.** Each binary has a 16 KiB read-only `.hprod` section (`__declspec(allocate)` or
  `__attribute__((section))`) holding a magic header and zeros. The reader goes through a `volatile` pointer,
  so the compiler cannot fold the placeholder, and it references the section, so `/OPT:REF` and
  `--gc-sections` keep it. Windows binaries also reserve capacity for
  resources: an icon group (≤ 256 KiB), `VS_VERSIONINFO` (≤ 4 KiB) and the application manifest (≤ 4 KiB).
- **`helios-tool product stamp`** runs on Windows or Linux hosts through an in-tree PE and ELF writer:
  1. check the input's hash against the SDK manifest;
  2. write `product.hprod` into `.hprod`;
  3. on PE, rewrite the icon, version strings (CompanyName, ProductName, FileDescription, OriginalFilename,
     version) and manifest `assemblyIdentity` **in place**: data is written inside the reserved capacity and
     only the resource data-entry sizes change, so no section moves;
  4. recompute the PE checksum, rename to `product.executables.*`, and record the input and output hashes.
- **Signing comes after stamping** (§2.8), so Authenticode covers the product block.
- **The C++ route.** A project with a `game/` module or a C++ gem links its own shipping client (ADR-016)
  against the SDK's static libraries. The build generates the same `.hprod` object and `.rc` from the
  descriptor, so both routes produce the same layout and pass the same runtime check. The launcher and
  bootstrap contain no game code and are always stamped.
- **Run-time check.** Right after the CPU gate, `ProductIdentity::load()` reads `.hprod` and verifies its
  root self-signature (Monocypher), which catches a half-stamped or corrupted block, and checks that the tier-0
  manifest names the same `productId`. An unstamped binary runs only with `--dev`, as `helios-dev`, with a
  "DEV BUILD" watermark.
- **Limits.** The self-signature binds endpoints and scheme to the root; it cannot stop someone who can rewrite
  the binary. Authenticode (Windows), the AppImage signature (Linux) and the tier-0 hash check of every launch
  (§2.5 step 8) protect the stamped files.

#### 2.10.5 T29 *Package & publish* profile

One T29 profile (07 T29) turns a project into a shippable, branded product, headless as
`helios-tool package --product --channel <ch> --platform win64,linux64` or from the editor:

1. **Lint:** `product lint`, T28 and the project's tests.
2. **Cook** the client content per platform (T29 client profile).
3. **Binaries:** take the prebuilt client or link the project's own (§2.10.4), then stamp the client,
   launcher, bootstrap and `crashpad_handler`.
4. **Sign:** Authenticode through `signtool` on Windows hosts or jsign (Apache-2.0) on Linux hosts, against
   Azure Trusted Signing or a PKCS#11 cloud HSM; a test certificate in dev. A Windows runner checks
   `signtool verify /pa /all` before anything is published.
5. **Pack and publish:** `helios-pack` builds `.hpak`s with tier and group tags. `helios-patch publish
   --product <id> --channel <ch>` (05 §7) signs the `.hman` and pointer with the product's `manifest` subkey
   and uploads to the product's CDN (S3-compatible or a local directory).
6. **Installers:** Windows `<setup>.exe` (the stamped, signed launcher in `--install` mode; it downloads tier 0
   during install); Linux `<Install>-x86_64.AppImage` (AppImage type-2 runtime and `appimagetool`, both MIT,
   holding the stamped bootstrap, `AppRun`, the `.desktop` file and icons), signed with the studio's GPG key.
7. **Release data:** symbols to the product's symbol store (§3); an SPDX 2.3 SBOM and a third-party notices
   file (FreeType credit, OFL fonts and the rest) built from `third_party/MANIFEST.md` and the project's gems;
   `dist/<productId>/<version>/release.json` with hashes, sizes, signatures and the pointer `sequence`.
8. **Smoke:** on clean Windows and Linux runners, install from the outputs headless, patch to the same build
   (0 bytes), launch to character select against the product's staging backend, then uninstall.
9. **Promotion** from `ptr` to `live` keeps 05 §7's two-approver rule.

**Budget:** from a warm cook, `starter-blank` packages for both platforms in ≤ 15 min on DEV, upload excluded.
**Phasing:** Windows installer and Linux tarball layout in Ph2 (WP-2.7, WP-2.16b); AppImage in Ph3 (WP-3.8).

**Branding kit** (09 §2.7.2): the launcher `ThemeDef` and RML overrides for its documents (login, news, patch
progress, settings), splash, icons, installer banner, EULA and privacy text per language, and a news-feed
template. The launcher's login and patch documents have flows and lints like any Foundation panel (§1.7.3), so
a rebranded launcher is checked by CL-23 as well.

---

## 3. Crash and diagnostics pipeline

**Capture.**
- **Library:** sentry-native 0.17.1 with the crashpad backend (R10-§9), initialized right after the CPU gate in
  the client and launcher (later the editor and servers).
- **Handler:** a signed `crashpad_handler` ships in tier 0 and uploads via `winhttp` or curl to the product's
  stamped `endpoints.crash` (§2.10), with its database under the product's crash directory. Reports carry
  `productId`, `version` and the build ID as tags. The crashing process never writes its own dump.
- **Contents:** breadcrumbs (state transitions, zone, the last 200 log lines, the last 50 net events, preset,
  RAM/VRAM, GPU/driver), plus a 2 MB log ring and `gpu_crash.json`. Minidumps are ≤ 2 MB; full dumps only in
  opt-in QA builds.

**Special crash classes.**

| Class | Handling |
|---|---|
| GPU device lost / TDR | `VK_EXT_device_fault` data and the last render-graph pass from buffer markers (`VK_AMD_buffer_marker`, `VK_NV_device_diagnostic_checkpoints`) → non-fatal event → restart dialog. Vendor SDKs are optional plugins |
| Hang | Watchdog on game/render heartbeats: a 10 s stall triggers a crashpad dump-without-crash; at 60 s it offers to terminate |
| OOM, addon error | OOM reports include mimalloc stats. Addon errors are non-fatal and rate-limited, and the addon is disabled |

**Symbols.** Every tagged CI build (for a studio, T29's *Package & publish*, §2.10.5) publishes PDBs and
binaries to the product's symbol store in a `symstore` layout, served over HTTPS as a
Microsoft-compatible symbol server (VS/WinDbg via `_NT_SYMBOL_PATH`), with `/SOURCELINK`. Linux split debug
info is keyed by build-id in debuginfod layout. Release symbols are kept forever; dev symbols for 90 days.

**Ingest and triage.**
- **`crashgw`** (Go, 05's telemetry family) runs in each product's backend. It drops unknown builds and any
  other `productId`, rate-limits to 5 reports per install per hour, scrubs PII and keeps dumps for 90 days.
- It forwards to **self-hosted Sentry** for grouping and release health (FSL licence review; GlitchTip is the
  fallback). rust-minidump (MIT) re-symbolicates in batches.
- Server dumps go to a separate project. Groups are routed by CODEOWNERS.
- **Release gate:** a new PTR crash group with ≥ 20 users, or a crash-free-session drop of more than 0.3 pp,
  **blocks promotion to live**.

**Performance telemetry.**
- **Per-minute aggregates** from every client: frame-time p50/p95/p99; hitches > 50 ms by cause (PSO, I/O,
  Luau GC, net, UI); PSO and streaming misses (AAA-REN-4, CNT-2); RAM/VRAM (CNT-3); load phases; ping and loss;
  zone, preset and hardware. 1 % of sessions add detailed traces.
- **Latency aggregates** (§1.3a): per-minute camera, action and UI latency p50/p95 (from SDL timestamps to
  `present_wait`), pacer margin, missed targets, and presentation mode (independent flip or composed).
- **Transport:** `tel.<shard>.client.perf` via the gateway (05 §1.16), or HTTPS to the product's stamped
  `endpoints.telemetry` before reaching the world.
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
engine/core/product/ ProductIdentity: .hprod reader, root self-signature check, ProductPaths (§2.10)
apps/client/        helios-client: states, camera rig, settings, frontend
apps/launcher/      bootstrap + launcher (Helios.exe / HeliosLauncher.exe until stamped); --install / --uninstall;
                    `base` (x86-64-v1) images with `@base` module builds (§2.1.1)
tools/packager/     helios-pack: cook → CDC-friendly .hpak (≤ 2 GiB, stable order, per-asset zstd,
                    tier/group/language tags); local publish (prod publish = Go helios-patch, 05 §7)
tools/product/      helios-tool product {init, import, lint, stamp, rotate-subkey, revoke-subkey,
                    rotate-root} and package: ceremony, GF(2⁸) Shamir, PE/ELF stamper (§2.10.3–2.10.5)
gems/foundation-ui/ Foundation panels: RML/RCSS, Luau controllers, view-model schemas, ThemeDefs,
                    strings, tests/ui/flows/*.flow.luau (§1.7)
gems/ui-reskin-fixture/  CL-23 reskin proof (no C++)
tests/{ui,patch,client,product}/   lint harness, patch fixtures, client flows, side-by-side product fixtures
```

`engine/ui`, `engine/text` and `engine/input` are not HEADLESS, so cells never link them (R06-ENG-01). Their
runtimes (and `engine/loc`) are specified by 02 §7.5–7.6; this section owns the HUD and screens,
`WorldMarkerRenderer`, the addon host and the client-side uses listed above.

### 4.2 Key interfaces (sketch)

```cpp
namespace helios::core {
class ProductIdentity { public:                            // stamped .hprod block (§2.10.4)
  static Result<const ProductIdentity*> load();            // verifies the root self-signature once
  std::string_view productId() const; std::string_view publisherId() const; bool isDev() const;
  const ProductPaths& paths() const; const ProductEndpoints& endpoints() const;
  std::span<const Ed25519PublicKey, 2> roots() const; }; // current, next
}
namespace helios::patch {
struct VerifiedManifest;                                   // constructible only by TrustChain
struct Ratchets { uint64_t pointerSequence, keysetVersion; uint32_t rootEpoch; };  // stored in install.db
class TrustChain { public: explicit TrustChain(const core::ProductIdentity&);
  Result<VerifiedManifest> verify(std::span<const std::byte> keyset, std::span<const std::byte> pointer,
                                  std::span<const std::byte> manifest, Ratchets&) const; };
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
class FramePacer { public:                                 // game thread; §1.3a
  void endPhaseA(); void sleepUntilSample();               // JIT sleep, then Phase B samples input
  void onExtract(FrameId); void onSubmit(FrameId, uint32_t batch); void onDisplayed(FrameId, Timestamp m7);
  PacerStats stats() const; };                             // d̂B, d̂R, d̂G, d̂P, margin, misses
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
// helios::platform: ICredentialStore (CredMan | libsecret); ILatencyProvider (Builtin = FramePacer |
// NvLowLatency2 | AmdAntiLag, all in-tree); optional plugins IAntiCheatProvider, IPlatformServices
// helios::core: LatencyMarkers { mark(FrameId, Marker, Timestamp); bindInput(InputId, FrameId); }  // M1–M7 ring
```

### 4.3 MVP → AAA feature ladder

| Area | Ph 0 | Ph 1 | Ph 2 | Ph 3 | Ph 4 | Ph 5 |
|---|---|---|---|---|---|---|
| Launcher | Fetch chunked manifest from local CDN, verify Ed25519; x86-64-v1 (`base`) build and its audit | RmlUi UI, login, launch code, CPU/GPU gate with pre-v2 emulator runs (CL-17) | Live/PTR, self-update, news, bandwidth caps | Pre-download, shard status, *Can't sign in?* link | OIDC/PKCE, MFA, Steam/Epic | — |
| Patching | Chunker, trust chain, fixtures | Full-file download, quick verify | CDC diff, resume, repair (CNT-7) | Streaming tiers, in-game heal | Multi-CDN, patch-from | Galaxy-scale prefetch |
| Install / signing | — | Dev installer, tarball | **Signed installer**, uninstall (PLT-6) | AppImage (PLT-6) | Optional Inno | Flatpak |
| Client shell | SDL3 window, Null/Vulkan RHI | State machine, threads, settings, reconnect, camera late latch | Queue UX, synced settings; **Phase A/B split, JIT `FramePacer`, chunked submit, speculative cue step, `LatencyMarkers`, `helios-latbench` (CL-6 Ph2)** | Headless UI flows at scale; `VK_NV_low_latency2` / `VK_AMD_anti_lag` providers; BENCH-4 latency runs | HDR, exclusive FS; 120 fps latency gates; the Relocating state for gateway deploys (§1.2; 05 §6.3.1) | — |
| Input / camera | Keyboard/mouse actions | Contexts, gamepad, rumble, FP↔cockpit camera | Rebinding, HOTAS, IME | Aim-assist class, tactical camera | Gyro, photo mode | — |
| Game UI (§1.7.2) | — (RmlUi lands in Ph1; the Ph0 launcher is a plain SDL3 window) | RmlUi on RHI; frontend, basic creator, HUD, settings, terminal host, system map, MFDs (R06); panel contract, themes, first flows (CL-23) | Journal, chat, social, inventory, loot, market and contracts, NPC vendor, trade, mail, crafting and industry, survey and harvesters, placement, organization, dialogue, galaxy map, full creator | Overview, killmails, fleet window, director and group finder, housing, decoration, cities, player vendors, factories, group dialogue, image designer, collections reacquire, **support and report**, **customization** (restyle, liveries), internal addons | Public addons, crafting minigames, seasons, ranked, DNA blend, **store** | Addon portal |
| Product / packaging (§2.10) | — | `helios-dev` product, `ProductIdentity` paths | Product descriptor and lint, `product init` and `stamp`, T29 *Package & publish* (Windows installer, Linux tarball), side-by-side products (CL-24) | AppImage in the profile; first root-rotation drill | Per-product storefront SKUs | — |
| Access / i18n | — | UI scale, subtitles | Pseudo-loc CI, colourblind palettes | Remapping, comfort | 2 languages + CJK/RTL test locales, TTS | RTL mirroring |
| Crash / telemetry | Local minidumps | Breadcrumbs, perf aggregates | Sentry, crashgw, symbols (STB-1) | Release gates, GPU markers | Driver advisories, hang dumps | — |

### 4.4 Acceptance criteria

All figures are on REF; figures in brackets are MIN. **Class** is the 09 §5.6 evidence class:
- **N:** nightly on Windows and Linux CI;
- **H:** nightly on lab hardware; with no lab result the criterion is unmeasured, which counts as failing;
- **W:** long-running, scheduled;
- **M:** a manual or external record.

Each criterion is registered in `scorecard.jsonc` with its class.

| ID | Criterion | Class | Maps to | Ph |
|---|---|---|---|---|
| CL-1 | Launcher cold start → login interactive ≤ 2.0 s (≤ 3.5 s); RSS ≤ 150 MB idle, ≤ 400 MB while patching | H | — | 1 |
| CL-2 | "Play" → character select ≤ 6 s (≤ 12 s) with a warm PSO cache; ≤ 20 s with progress after a driver change | H | REN-4 | 2 |
| CL-3 | Character select → controllable in world, unqueued, p95 ≤ 15 s (≤ 25 s) | H | SRV-7 | 2 |
| CL-4 | Zone transition p99 ≤ 3 s; no frame > 50 ms in world | H | SRV-5, REN-5 | 1 |
| CL-5 | 5 s outage → back in world ≤ 3 s after recovery in ≥ 99 % of 1,000 runs (NetSim, headless); sleep/resume < 5 min reconnects (lab) | N + H | STB-5 | 2 |
| CL-6 | **Input latency** (§1.3a): input → displayed (M0 → M7), p95. Conditions: REF, low-latency mode, 144 Hz VRR lab display, independent flip, `helios-latbench` injection. **Ph2** (BENCH-1 with the harness rifle, dash ability and HUD; 1440p High; 60 fps limit): action and UI ≤ 40 ms, camera ≤ 33 ms, fixed 60 Hz V-Sync ≤ 45 ms. BENCH-4 joins when it enters the nightly lab (Ph3). **Ph4** adds Performance mode at 120 fps in BENCH-4: action ≤ 28 ms, camera ≤ 25 ms. Linux ≤ 1.1 × Windows. Low-latency fps ≥ 97 % of throughput mode. The weekly photodiode rig agrees within ± 3 ms | H | REN-1, REN-3 | 2 (4) |
| CL-7 | BENCH-1 HUD ≤ 1.0 ms CPU / 0.5 ms GPU at 1440p; 40 diegetic screens ≤ 1.0 ms CPU / 0.5 ms GPU (RT-12, 03 §7.5). BENCH-3: 2,000 brackets (the full NS-4.2 battle, 01 §3.2) ≤ 1.0 ms CPU | H | REN-1 | 3 |
| CL-8 | 10k-row market grid: first rows ≤ 150 ms after data arrives; no frame > 16.7 ms while scrolling | H | — | 2 |
| CL-9 | A 1 GB change in a 50 GB install downloads ≤ 1.5 GB (typical ≤ 1.2×). A 50 MB executable change ≤ 20 MB with patch-from | N | CNT-7 | 2 (4) |
| CL-10 | Full verify of 50 GB ≤ 3 min on NVMe (≤ 5 min on SATA SSD); quick verify < 1 s | H | CNT-7 | 2 |
| CL-11 | ≥ 90 % of a 1 Gbit/s link using ≤ 2 cores; 50 GB via a 500 Mbit/s CDN in ≤ 15 min | H | — | 2 |
| CL-12 | 1,000 kill or power-cut injections during install, patch and self-update → 0 corrupt installs; the launcher always starts; ≤ 16 MiB wasted per resume | N | — | 2 |
| CL-13 | Tier 0 ≤ 15 % of the install (manifest check); a 50 GB game playable within 15 min at 100 Mbit/s (shaped lab link) | N + H | — | 3 |
| CL-14 | 100 % rejection of tampered, expired, rolled-back or wrongly keyed pointers, manifests, keysets and chunks, including another product's and a revoked subkey's, and of a keyset signed by a root the install has ratcheted past; the `.hprod` reader rejects half-stamped and corrupted blocks; parsers fuzzed ≥ 24 CPU-h (release tier) | N | SEC-6/7 | 2 |
| CL-15 | Every injected crash kind (null deref, stack overflow, abort, pure virtual, OOM, hang, device lost) → symbolicated report ≤ 5 min, Windows and Linux | N | STB-1 | 2 |
| CL-16 | Crash-free sessions ≥ 98.0 / 99.4 / 99.8 % (Ph2/3/4; equivalent to STB-1 at a 2 h mean session), over 09 §5.6's minimum observed hours | M | STB-1 | 2–4 |
| CL-17 | **CPU floor, never a raw fault** (§2.1.1, §2.2). (a) **Pre-v2 CPUs:** Intel SDE `-mrm` (Merom) and `-pnr` (Penryn) with `-chip-check-exe-only` on Windows; `qemu-x86_64 -cpu Opteron_G1` (SSE2 only), `core2duo` and `phenom` on Linux. The launcher, the setup (`--install`) and `<Install>.exe` (through the launcher it starts) reach the RML refusal screen, and the client, cell, editor, bot and `helios-tool` show or print the CPU message; all exit with code 78. The emulator reports 0 illegal instructions. CI reads the verdict through `--gate-report`, and once per OS a UI-automation step finds the real screen. (b) **v2 without AVX2:** SDE `-nhm` and `-snb`, qemu `Nehalem` and `SandyBridge`: the same outcomes. (c) RT-09's `base`-image audit (02 §1.1 check 4) finds no SSE3-or-later, POPCNT, LZCNT, MOVBE, CMPXCHG16B, VEX, EVEX or BMI encoding outside a self-dispatch symbol in the launcher and bootstrap images, or in any gate object | N | — | 1 |
| CL-18 | Every shipped PE passes `signtool verify /pa /all` with a timestamp; installing needs no admin (test certificate nightly; production signature at the release tier) | N | PLT-6 | 2 |
| CL-19 | Pseudo-loc +40 % without clipping; 0 missing glyphs; golden screenshots at 75/100/200 % | N | — | 2 |
| CL-20 | Headless login → world → logout ≤ 90 s per commit; smoke tests on Win10 22H2, Win11 24H2 and Ubuntu 24.04 | N | PLT-3 | 1 |
| CL-21 | Linux client within 10 % of Windows in every BENCH; memory within CNT-3 | H | PLT-5, CNT-3 | 4 |
| CL-22 | Hostile-addon suite (loops, heap bombs, protected calls) stays ≤ 1.5 ms/frame and ≤ 64 MiB and never runs a protected action | N | SEC-1 | 4 |
| CL-23 | **Foundation UI flows** (§1.7.3): every §1.7.2 panel whose phase has been reached, plus the launcher's login and patch documents, completes its headless Luau flow on Windows and Linux (happy path, listed reject paths, keyboard-only and gamepad-only passes) with view-model and authoritative-state assertions (Store against the dev backend's fake payment provider; Support and report against the admin API's case queue); passes the RmlUi layout lints at 75–200 % with +40 % pseudo-loc and the CJK and RTL test locales; and the `ui-reskin-fixture` gem passes the same flows and lints, differs from the default skin in ≥ 30 % of each panel's pixels, and leaves every engine and Foundation binary byte-identical to the SDK | N | TOOL-9, R06 | 1–4 (per panel) |
| CL-24 | **Side-by-side branded products** (§2.10): two products packaged from `starter-blank` by T29's *Package & publish* profile, each with its own `product init --dev --ceremony-sim` keys, install per-user, patch a 1 % CDC change, open through their own URI schemes, store "remember me" credentials, report an injected crash to their own `crashgw`, self-update and uninstall **side by side** on Windows 10 22H2, Windows 11 24H2 and Ubuntu 24.04. Pass: no shared file, directory, registry key, credential, lock or scheme (a diff of filesystem, HKCU and Secret Service before and after); each rejects 100 % of the other's pointers, manifests and keysets; uninstalling one leaves the other passing full verify; Windows outputs pass `signtool verify /pa /all` and the AppImage signature verifies; no engine or backend source differs from the SDK | N | PLT-6, TOOL-9, SEC-6 | 2 (Windows, Linux tarball layout) / 3 (AppImage) |

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
- **Latency:**
  - `FramePacer` unit tests on a simulated clock cover GPU-bound, CPU-bound, cap-bound, FIFO and no-`present_wait`
    cases with injected spikes. They assert the sample deadline, bounded margin growth and recovery, and ≤ 3 %
    GPU idle when GPU-bound.
  - A speculative-cue test drives an ASM through accept, divergence and out-of-ammo, and asserts that cues are
    deduplicated or cancelled by key.
  - Nightly `helios-latbench` on the lab (CL-6); the weekly photodiode rig validates the instrumentation.
- **UI automation (CL-23, §1.7.3):** one headless Luau flow per Foundation panel, with reject paths and
  keyboard-only and gamepad-only passes, asserting view-model and authoritative state; the RmlUi layout lints;
  the reskin fixture. Lavapipe golden screenshots cover {en, pseudo, CJK, RTL} × {75, 100, 200 %} × {default,
  high contrast, colour-blind palettes}, and T19 reuses the harness.
- **CPU floor (CL-17, §2.1.1):**
  - Gate unit tests feed recorded CPUID and XGETBV dumps (Merom, Penryn, Nehalem, Sandy Bridge, Haswell,
    Zen 1, and a hypervisor that masks AVX in XCR0) and assert the verdict and the brand string.
  - Self-dispatch tests force each BLAKE2b and zstd path (SSE2, SSE4.1, AVX2, BMI2) and compare them
    against the golden vectors shared with Go.
  - Per commit: the `base`-image disassembly audit (RT-09). Nightly: the launcher, setup, bootstrap and
    client under SDE and qemu across the pre-v2 and v2-without-AVX2 models.
- **Store and support (CL-23):** the Store and Support and report flows of §1.7.3 against the dev backend's
  fake payment provider and the admin API, including the reject paths (spending cap, webhook replay, forged
  evidence tag, rate limit).
- **Product identity (CL-24, §2.10):**
  - `product lint` vectors (reserved schemes, device names, ID patterns, icon sets).
  - Stamper round trips on real SDK binaries: stamp, then Authenticode-sign, then verify; resource sizes
    beyond the reserved capacity are refused; a stamped PE still loads its icon and version strings.
  - Ceremony simulation (`--ceremony-sim`): 3-of-5 reconstruction from every 3-share subset, and failure with
    2 shares; subkey rotation, revocation and root rotation, each followed by the CL-14 rejection suite.
  - A side-by-side job installs two fixture products on clean Windows and Linux runners and diffs the
    filesystem, HKCU and the Secret Service before and after each step.
- **Nightly lab:**
  - MIN/REF hardware on Win10, Win11 and Ubuntu, across NVIDIA, AMD and Intel.
  - Defender real-time scanning stays on during install/verify budget runs.
  - DPI/monitor hotplug, sleep/resume, and an 8 h soak (RSS growth ≤ 2 %).
  - 04 NetSim profiles drive reconnect tests.

### 4.6 Risks and mitigations

| Risk | Mitigation |
|---|---|
| RmlUi too slow for EVE-density UI | `<datagrid>`, `WorldMarkerRenderer` outside RmlUi, CL-7/8 gates; plan B: native grid element |
| JIT pacing mispredicts: late frames or lost fps | p90 predictions with an adaptive margin; REN-1/2 p99 frame-time gates are measured in low-latency mode; the ≤ 3 % fps-cost gate; users can choose Off |
| `present_wait` missing or unreliable on a driver | GPU-completion fallback calibrated against PresentMon; `DriverAdvisoryDef` entries; the lab covers NVIDIA, AMD and Intel on both OSes |
| RmlUi or SDL3 upstream stalls | Pinned, vendored, confined behind `engine/ui` and the platform layer |
| AVX2 floor or a failing gate; a pre-v2 CPU faulting before any gate | An x86-64-v1 launcher closure and gate TU (§2.1.1), with wide code only in listed self-dispatch functions; the gate repeated in the client; the `base`-image audit (RT-09); emulator CI across pre-v2 and v2-without-AVX2 models (CL-17) |
| False or mass reports, forged chat evidence | Evidence only by server IDs and keyed evidence tags (05 §1.10); voice evidence only from the forwarder's ring; rate limits and duplicate merging (05 §1.17); reporting is a protected action, so addons cannot automate it |
| Minors overspending; loot-box regulation | Server-enforced 13–17 monthly caps shown on every price; randomized offers hidden and refused for minors and in countries listed in live config, with odds always shown; payment only on the provider's page (05 §1.20) |
| SmartScreen and Defender false positives | Stable names, HSM signing, Microsoft submissions, no packers |
| Signing-key or CDN compromise | Offline root, rotating subkeys, pinned keyset, anti-rollback, expiry, Authenticode |
| Self-update bricks installs | Health handshake, 2 previous builds kept, CL-12 |
| Patch egress cost (05 §6.4) | CDC + patch-from, shared cache, CL-9 gate |
| Addons enable botting | Protected actions + taint; server-side detection |
| Licence sign-offs | FreeType FTL, Inno (if used), SIL OFL fonts, EOS/Steamworks, Sentry FSL; vendor low-latency *branding* (the extensions themselves need none) |
| Foundation UI breadth slips behind 06's systems, so M2 or a template has no screen | Every panel has a phase and an owning WP (§1.7.2); CL-23 fails a phase exit for a panel without a passing flow; panels use generated adapters, so most need no C++ |
| Two Helios games collide on one PC, or a studio needs an engine rebuild to brand its game | Every OS name comes from `ProductIdentity` (§2.10.2) with a CI grep against literals; prebuilt stampable binaries; CL-24 on both OSes |
| A studio loses its root key or leaks a subkey | 3-of-5 shares in two locations plus a pre-committed `next` root; quarterly subkeys with revocation; annual reconstruction drill (§2.10.3) |
| Stamping breaks Authenticode or SmartScreen reputation | Stamp before signing; in-place writes only; per-product executable names signed with the studio's own certificate accrue their own reputation (§1.15) |

### 4.7 Traceability

| Requirement | Where |
|---|---|
| R10-§8 (SDL3: IME, rumble, raw input, DPI, `SDL_Renderer`) | §1.3, §1.5, §1.15, §2.1 |
| R05-P0-2 (extract pipeline) with a Destiny-class feel: low-latency pacing, input-latency budget | §1.3, §1.3a |
| R10-§9 (self-installing launcher, atomic swap, WinHTTP/libcurl, Inno/WiX/MSIX, Authenticode, crashpad, symbols, EAC) | §1.14, §2.5–2.8, §3 |
| R10-§2 (manifest, `/MT`), §5 (AVX2 gate), §7 (Ed25519), §12 (RmlUi and text stack) | §1.7, §1.10, §1.15, §2.2, §2.5 |
| R07 §7, P0-11, P1-20 (CDC patching); R01-P0-9, R01 §8 (shared cache) | §2.4–2.6 |
| R07-P0-2/3, R07 §4 (OIDC, queue, reconnect); R07 §6 (dumps, symbol server) | §1.1, §1.2, §2.3, §3 |
| R03-P0-9 and lesson 2 (UI decoupled from the GPU API), R03-P0-8, P1-2, P1-6, P2-5 | §0, §1.7, §1.10, §2.6 |
| R04-P1-14 (continuous camera), R04-P1-18 / §7 (in-world UI) | §1.6, §1.8 |
| R05-P0-3 (prediction), P0-9 (per-device aim assist), P2-25 (streaming install) | §1.5, §1.11, §2.6 |
| R06-ENG-01/27; R08 F1, T19, T25; R09 B15/B16 | §1.7–1.13, §4.1 |
| 06's player-facing systems (crafting, survey and harvesters, structures, trade, mail, vendors, terminals, orgs, group finder, killmails, character creation) and the starter templates (09 §2.7.4) | §1.7.2, §1.7.3 (CL-23) |
| 01 §1.1 north star (studios ship without engine source edits); R08-T29 (package and deploy); AAA-PLT-6 | §2.10 (CL-24) |
| R10-§5 (AVX2 gate) on any x86-64 CPU; 02 §1.1's ISA levels (`base` = x86-64-v1) | §2.1.1, §2.2 (CL-17) |
| 05 §1.20 entitlements and the 13–17 caps (§6.6); 06 §5.2 collection reacquire; 06 §5.3 season premium lane | §1.7.2 Store and Progression (CL-23) |
| 05 §1.10 chat reports; 04 §2.7 voice reports; 05 §1.17 cases; 06 §9.6 `Customization` terminal service; 06 §10 appearance | §1.7.2 Support and report, Customization (CL-23) |
| AAA-REN-4/5, SRV-5/7, CNT-2/3/7, STB-1/5, SEC-1/6/7, PLT-3/5/6, TOOL-9 | §4.4 |

### 4.8 Cross-section dependencies

- **00 ADR:** amend R10-§13 so ImGui is not the launcher UI; allow-list SIL OFL-1.1 for fonts (both done in ADR-010).
- **02 Engine:**
  - Resolved (round 3; 02 §1.1, ADR-011 amendment): whole images are built at the `avx2` level, and only the
    CPU-gate translation unit is exempt, at `base`; it runs first.
  - Paks ≤ 2 GiB with per-asset compression, stable order and `group`/`tier`/`language` tags.
  - A VFS residency hook, per-block checksums and per-zone PSO lists.
  - Resolved: 02 owns the `engine/ui`/`engine/text` runtime (02 §7.5); this section owns HUD, screens and addons
    (README updated).
  - Done in 02 §2.4: the game-thread order Phase A → JIT sleep → Phase B → extract, and `LatencyMarkers`
    (§1.3a).
  - Done (round 2): the settings paths in 02 §2.5 and §7.6 and the launcher row of 02 §1.3 are product-scoped
    (§2.10); 02 §7.5 adds `@source(Component.field | Service.Stream)` view-model annotations, from which
    schemac generates §1.7.1's dirty-mask adapters.
  - Done (round 3): 02 §1.1's `base` level is x86-64-v1 on every compiler (`-march=x86-64`, not
    `-march=x86-64-v2`); audit checks 2 and 4 reject SSE3-or-later, POPCNT, LZCNT, MOVBE and CMPXCHG16B
    encodings as well; check 5 adds the pre-v2 SDE and qemu models; the self-dispatch list names BLAKE2b's
    and SDL3's CPUID-guarded functions (§2.1.1).
- **03 Rendering:** UI composite after upscaling, HDR paper white, `UiSurface` and world-marker passes, cockpit
  depth range, GPU crash markers. Also low-latency mode for 03 §2.5 (done): same-frame render, 3-batch
  submission, latched view constants read by GPU culling and TAA, rotation-invariant CSM, and the
  `VK_NV_low_latency2` and `VK_AMD_anti_lag` rows in §1.2.
- **04 Networking:**
  - `device_class` in the input header, a client TiDi field, the shared `clientcore`, and the attestation
    user-data format.
  - Resolved: 04 §1 uses embedded-postgres (ADR-014); `device_class` is in 04 §5.2.
- **05 Backend:**
  - Resolved: A12 now restates AAA-CNT-7 (CL-9/CL-10 govern).
  - Needed (now in 05 §1.1, §1.5, §6.2, §7): pointer fields `next{}`, `min_client` and the CDN list; a settings
    blob of ≤ 256 KiB; `crashgw` plus a symbol bucket; bot credentials; shared FastCDC vectors.
  - Done (round 2): the keyset path is per product, `/keys/<product>/keyset.json` with max-age 60 s (05 §7).
    Each product runs its own backend deployment and `crashgw`. §1.7.3's flows read ledger history through the
    existing admin API (05 §1.17) and need no new endpoint.
  - Done (round 3): chat evidence tags (05 §1.10); report evidence and cases from Phase 3 (§1.16); the case
    and support-ticket model, player and staff APIs, SLA targets and `OpenAccessTicket` (§1.17); the Store
    API (`ListOffers`, `CreateCheckout`, `CheckoutStatus`, `SpendWithPremium`, `GetSpendStatus`), default
    13–17 caps and the dev fake provider (§1.20); the admin console's case queue (§5).
- **06 Gameplay:** the speculative cue step uses only 06 §1.4's ASM and 06 §1.5's key-deduplicated cues.
  Done (round 2): 06 §3 states that `CraftQuality` and 06 §9.2 that `PlacementCheck` are pure functions shared
  with §1.7.2's crafting estimate and placement ghost. 06 §6.13's "activity director (08 §1.7)" is the
  Activity director panel. Done (round 3): 06 §10 defines the `Restyle` and `SetLivery` intents behind the
  Customization panel.
- **07 Editor:** T19 on `engine/ui` (it runs §1.7.3's panel flows), T25 string tables, PIE on `apps/client`,
  addon API docs. Done (round 2): T29 lists the *Package & publish* profile (§2.10.5). Done (round 3): T27
  has the case queue with SLA fields (Ph3).
- **09 Roadmap:** CL-6 (Ph2) in WP-2.13 and the Phase 2 exit (done); the Phase 0 launcher skeleton; buying the
  HSM signing certificate in Phase 1; legal sign-offs. Done (round 2): the §1.7.2 panels in WP-1.20, 2.13, 3.8
  and 4.9 with CL-23 per phase; product scoping in WP-2.7; `product init`, `stamp` and *Package & publish* in
  WP-2.16b with CL-24 (Ph2) and the AppImage clause in WP-3.8; the §2.7.2 branding kit points at §2.10;
  `template-proof` packages through the profile; two test-matrix rows; CL-23 and CL-24 in the Ph1–4 exits.
  Done (round 3): WP-0.17 builds the launcher at x86-64-v1 with the pre-v2 emulator runs; WP-3.3 the cases and support queue;
  WP-3.8 the Support and report, Customization and collections panels; WP-4.4 the Store API and caps; WP-4.9
  the Store panel.
- **PLAN.md:** §4.2 names the `base` level as x86-64-v1 and the pre-v2 emulator check behind "runs on any
  x86-64 CPU" (done, round 3).
