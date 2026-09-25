# 10 — Technology Selection & Windows-first Build Plan

> **Method note.** Checked on 2026-09-25. Every version, date and license below was confirmed with `git ls-remote`, shallow or sparse clones of the upstream repositories (commit dates from `git log`), and `proxy.golang.org` for Go modules. I also configured and built the current `third_party/` tree with CMake 3.28.3, Ninja and GCC 13.3 on Ubuntu 24.04: all `tp_*` targets built, and so did Luau.VM, Luau.Compiler and Luau.CodeGen. Web search and the GitHub REST API were blocked. Claims I could not check are marked **[unverified]**.

---

## 1. Overview

The stack the lead chose is broadly right for a Windows-first, Linux-capable MMO engine. It needs one platform-library swap, several version bumps (one is a security fix), and fixes to real build bugs in the current tree.

### Verdicts on the lead's decisions

| Decision | Verdict |
|---|---|
| C++20, Vulkan 1.3 via volk + VMA, Dear ImGui docking/ImGuizmo/ImPlot, meshoptimizer, cgltf, stb, miniaudio, zstd, xxHash, doctest, Recast/Detour, Luau 0.739 | **Agree.** Luau 0.739 is the newest tag (2026-09-18). |
| Jolt 5.3 with `JPH_DOUBLE_PRECISION` | **Agree on Jolt, disagree on the pin.** Upgrade to **v5.6.0** and fix the ISA and determinism flags (§5). |
| GLFW 3.4 | **Disagree: replace it with SDL3 3.4.16.** GLFW 3.5.1 (2026-07-31) still has no IME, rumble or gyro API (§8). |
| Monocypher 4.0.2 | **Must upgrade to 4.0.3.** Upstream fixed a *timing-leak vulnerability in EdDSA/Ed25519* (2026-06-15). |
| Tracy 0.11.1 | **Upgrade to v0.14.1.** The client and the viewer must be the same version. |
| Go 1.24 | **Strong disagree.** Go 1.24 is out of support: its last patch was go1.24.13 (2026-02-03), and Go 1.26.0 shipped 2026-02-10. Use **Go 1.27.1** (2026-08-28). |
| Pure-Go SQLite for Windows local development | **Disagree for the system of record.** Use `embedded-postgres` (a real PostgreSQL 18.3, no Docker, no admin rights). Keep SQLite for tools and offline caches (§11). |
| Go control plane, C++ gateway, PostgreSQL + Valkey + NATS JetStream | **Agree.** |

### Build bugs in the current tree (fix before engine code lands)

1. **`third_party/CMakeLists.txt` compiles Jolt SSE2-only on Windows.** The ISA flags sit behind `if(NOT MSVC)`, and `MSVC` is true for both `cl` and `clang-cl`. The result:
   - Windows client builds get no `/arch:AVX2` and no `-mavx2`.
   - Linux builds get AVX2 **plus `-mfma`**, which turns on `JPH_USE_FMADD`.
   - Client and server physics therefore differ numerically, and the Windows build is also slower.
   - With `clang-cl` it would also fail to compile once Jolt's LZCNT and F16C intrinsics are reached without `-mlzcnt`/`-mf16c`.
2. **`ZSTD_MULTITHREAD=0` turns multithreading *on*.** zstd tests `#ifdef ZSTD_MULTITHREAD`, and the built `libtp_zstd.a` imports `pthread_create` (checked with `nm`). Remove the define, or turn MT on deliberately and link `Threads::Threads`.
3. **The C runtime is never chosen.** Upstream defaults disagree: Jolt, ozz and netcode default to static `/MT`, while Luau and SDL default to `/MD`. Set `CMAKE_MSVC_RUNTIME_LIBRARY` once at the top level (§2).
4. **`find_program(ccache)` is used with MSVC's default `/Zi`**, which compiler caches cannot cache. Use `/Z7` (§10).
5. **`HELIOS_SANITIZE` skips MSVC.** MSVC supports `/fsanitize=address`.
6. **The Monocypher vendoring is incomplete.** It copies only `monocypher.c/h`. Standard Ed25519, needed to interoperate with Go's `crypto/ed25519` for signed patch manifests, lives in `src/optional/monocypher-ed25519.c`.

---

## 2. Windows-first Build Notes

**Toolchains.**
- Baseline: **VS 2022 17.14 (v143)**.
- Also run **VS 2026 (v18)** in CI. It exists: Jolt 5.5/5.6 release notes mention VS 2026 and "MSVC 18.6.0" fixes.
- **clang-cl** from the VS "C++ Clang tools" component.
- On Linux: **GCC ≥ 13** and **Clang ≥ 18**. Ubuntu 24.04 ships both.
- Jolt 5.6 deprecates compilers older than VS 2022, Clang 16 and GCC 12.

**CMake.**
- Raise `cmake_minimum_required` from 3.24 to **3.28**:
  - Ubuntu 24.04's packaged CMake is 3.28.3 (verified).
  - 3.25 or newer is needed for `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` (CMP0141).
  - Presets v8 need 3.28.
- Test with CMake **4.4.x** too. CMake 4 rejects projects whose policy version is below 3.5. The vendored GLFW (`3.4...3.28`) and Luau (3.10) pass.

**Global MSVC settings.** Set these in the root `CMakeLists.txt` *before* `add_subdirectory(third_party)`:

```cmake
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT everywhere: no VC++ redist
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")  # /Z7 → cacheable
if(MSVC)  # true for cl and clang-cl
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:inline /EHsc /bigobj
                      $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor> $<$<CXX_COMPILER_ID:MSVC>:/GT>)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  add_link_options($<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF> $<$<CONFIG:Release,RelWithDebInfo>:/OPT:ICF> /DEBUG:FULL)
endif()
```

- **Why static `/MT`:** the client, launcher and crash handler then run on a clean Windows install without a VC++ redistributable. Jolt, ozz and netcode already default to it. Every vendored CMake project must inherit the variable rather than set its own:
  - Luau: `LUAU_STATIC_CRT` stays OFF, and the global setting wins.
  - ozz: `ozz_build_msvc_rt_dll` stays OFF.
  - netcode: honors the variable when it is already defined.
- **`/GT`** makes TLS fiber-safe, which the job system needs (§6).
- **`/utf-8`** is mandatory. The source and execution character sets must be UTF-8 (fmt, Luau and HarfBuzz strings).
- **Distinguish clang-cl from cl** with `CMAKE_CXX_COMPILER_ID STREQUAL "Clang"` plus `CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"`, never with `MSVC` alone.

**Application manifest** (`engine/platform/win/helios.manifest`, linked into every executable):
- `activeCodePage=UTF-8` (Windows 10 1903+);
- `dpiAwareness=PerMonitorV2`;
- `longPathAware=true`;
- the Windows 10/11 `supportedOS` GUIDs.

The client links with `/SUBSYSTEM:WINDOWS`. Tools and servers keep a console.

**Repository hygiene.** `.gitattributes` (`* text=auto eol=lf`, `*.bat eol=crlf`, assets `binary`); no symlinks; `core.longpaths true`; short build dirs (e.g. `C:\h\out`) because Ninja/MSVC object paths can pass 260 characters; a Windows 11 **Dev Drive** or Defender exclusions for the repo and `out/`.

**Vulkan without the SDK.** volk loads the driver's `vulkan-1.dll` (`libvulkan.so.1` on Linux), so the build needs only the vendored headers. The only SDK piece worth an *optional* developer install is the validation layers (SDK, or a pinned `VkLayer_khronos_validation` drop in `tools/prebuilt/`). Linux CI uses Mesa **lavapipe** for headless tests.

---

## 3. Shader Toolchain

| | glslang 16.6.0 (2026-09-11) | **Slang v2026.18.2 (2026-09-22)** | DXC v1.10.2605.37 (2026-08-06) |
|---|---|---|---|
| License | BSD-3 / MIT / Apache-2.0 / "AML-glslang", plus **`glslang_tab.cpp` under GPL-3.0-or-later WITH Bison-exception-2.2** (per `REUSE.toml`) | **Apache-2.0 WITH LLVM-exception** | NCSA (LLVM) — permissive but not on our list |
| Language | GLSL (and HLSL) | HLSL-like, with **modules, generics, interfaces**, autodiff, GLSL compatibility module | HLSL |
| Reflection | Needs SPIRV-Reflect (Apache-2.0, 2 files) | **Built-in reflection API**: parameter blocks, bindless layouts | Needs SPIRV-Reflect |
| Source build | Light (a couple of minutes). No Python when `ENABLE_OPT=OFF`. SPIRV-Tools needs **Python 3** (`find_host_package(Python3 REQUIRED)`, `ggt.py` codegen). | Heavy: about 20 submodules and Python 3. It **downloads DXC and slang-llvm binaries at configure time** unless `SLANG_ENABLE_DXIL=OFF` and `SLANG_SLANG_LLVM_FLAVOR=DISABLE`. | Very heavy (an LLVM fork) |
| Prebuilt | Vulkan SDK | GitHub releases for win/linux x64 and arm64, and in the Vulkan SDK since 1.3.296 (per README) | GitHub releases |
| Tooling | — | `slangd` language server for VS and VS Code | — |

**Recommendation: Slang**, as a build-time and editor-time tool only.
- **Why:** bindless material parameter blocks, material-graph codegen into Slang `interface` implementations (avoiding `#define` permutation explosion, the report-06 warning), built-in reflection, and a future path to D3D12/Metal if the RHI seam is ever used.
- **Consume the pinned prebuilt release:**
  - Unpack it to `tools/prebuilt/slang/2026.18.2/{windows-x64,linux-x64}` with Git LFS, or fetch it once with a SHA-256-verified bootstrap script.
  - `HELIOS_SLANG_ROOT` overrides the location.
  - The upstream building guide itself says to redistribute the exact library version you linked (no ABI promise across versions).
  - A weekly CI job does a source build of the same tag with DXIL and LLVM off, to prove reproducibility.
- **`helios-shaderc`** (C++, links `slang-compiler`) runs in the asset processor. It emits SPIR-V plus a Helios reflection blob: bindings, push constants, specialization constants, and material parameter layout.
- **The shipped client contains no shader compiler.** It ships cooked SPIR-V and builds a `VkPipelineCache` from recorded PSO lists (report 04, P0-10).
- **Editor hot-reload:** the editor and dev client load the Slang shared library at runtime. The asset processor recompiles on file change and pushes new SPIR-V over the hot-reload socket.
- **SPIRV-Cross:** not needed (Slang emits other targets itself).
- **SPIRV-Reflect:** optional, in tests only, to cross-check Slang reflection.
- **`spirv-val`:** runs in Linux CI, where SPIRV-Tools builds with Python, over all cooked shaders.
- **Fallback (plan B):** vendor glslang 16.6.0 with `ENABLE_OPT=OFF` plus SPIRV-Reflect. Its GPL-with-Bison-exception parser file is distributable, but log it for legal review if the editor is ever shipped to modders.
- **DXC:** rejected (size, and HLSL→SPIR-V lags Slang for Vulkan features).
- Slang release asset names and Linux glibc floor: **[unverified]**.

---

## 4. Scripting (Luau) Integration

**Version.** 0.739, the newest tag. It builds cleanly with GCC 13; Luau's CMake adds `/MP` and `/we4018` on MSVC.

**Bindings: generated, not hand-written.**
- The schema IDL (§6) generates:
  - C++ glue using **tagged userdata** (`lua_newuserdatataggedwithmetatable`, `lua_touserdatatagged`, `lua_setuserdatadtor`; there are 128 userdata tags);
  - a `.d.luau` definitions file for both `Luau.Analysis` and luau-lsp.
- Hand-write only the core runtime: scheduler, timers, logging, math.
- **Positions:** Luau's `vector` is float (`LUA_VECTOR_SIZE` 3 or 4), so world positions must be a double-precision `WorldPos` userdata, never a vector.
- Define `LUA_VECTOR_SIZE` and other `luaconf.h` overrides as **PUBLIC** compile definitions on `tp_luau`, so the VM and our code agree.

**Sandboxing.**
- Call `luaL_openlibs`, then register the Helios API, then `luaL_sandbox(L)`. Each script environment gets `luaL_sandboxthread` (`lua_setsafeenv`).
- **Memory cap:** a per-VM `lua_Alloc` enforces the limit, and `lua_setmemcat`/`lua_totalbytes` attribute memory to modules (256 categories).
- **CPU budget:** `lua_callbacks(L)->interrupt` runs at loop back-edges and calls, and throws when the budget is exceeded.
- No `loadstring`, no `debug` library, no `io`/`os`.
- Player-authored code (report 03 §6.1) runs in **separate VMs** with stricter quotas and codegen off.

**Coroutines for server scripts.** Each script entry point runs in a `lua_newthread`. Service and DB calls are C functions that register a continuation and `lua_yield`; the scheduler `lua_resume`s when the NATS reply or DB result arrives, and `lua_pcallyieldable` allows yielding through protected calls. This is EVE's tasklet model (report 01) without a second VM.

**Native codegen.** `Luau.CodeGen` supports **x64 Windows**: it has an explicit `ABIX64::Windows`, `VirtualAlloc`/`VirtualProtect` W^X transitions (never RWX), and `RtlAddFunctionTable` unwind registration, so crash dumps unwind through JIT frames. It also supports SysV x64 and A64.
- Enable it per module (`--!native` or `luau_codegen_compile`) after profiling. Keep the server on the interpreter first; it is fast and deterministic to debug.
- Always call `luau_codegen_supported()`.

**Type analysis in the editor.**
- Link `Luau.Analysis` (`helios::tp::luau_analysis`) into the editor and the compile/validate service (report 03 §8.1).
- In 0.739 the new solver (`LuauSolverV2`) is still on `ExperimentalFlags`' opt-in list. **Gate CI on the old solver** and run the new one as a non-blocking job.

**Debugger.**
- There is no upstream DAP adapter. Build one (≈1.5–2 kLOC) in the script host from `lua_breakpoint`, `lua_singlestep`, `lua_getinfo`/`lua_getlocal` and the `debugbreak`/`debuginterrupt` callbacks. Serve DAP over TCP from the editor, dev client and zone server; VS Code attaches with a generic DAP config.
- IntelliSense: **luau-lsp 1.70.0** (MIT, 2026-09-20) with our generated definition files (`luau-lsp.types.definitionFiles`, platform `standard`).
- Formatting: **StyLua v2.5.2 is MPL-2.0**. That is fine for a developer tool, but never vendor it into shipped code.

**Per-zone VM isolation.**
- One `lua_State` VM per zone or cell, owned by exactly one job at a time. VMs are not thread-safe.
- Script instances are threads inside that VM.
- Zone migration serializes *script state as data* (components and quest state), never the Lua heap.
- Per report 05, predicted and rollback abilities are **not** Luau. They run in a C++ state-machine VM.

---

## 5. Physics (Jolt)

**Upgrade to v5.6.0 (`e77f175`, 2026-07-11).**

What it brings:
- up to 40% faster and up to 70% less memory (scene-dependent);
- a new, cheaper friction model — a *behavior change*, so retune vehicles and characters;
- a fix for "cross platform determinism between ARM64 and x64 builds when compiling with double precision";
- VS 2026 support (5.5.0);
- several `CharacterVirtual` fixes.

5.6 adds `Jolt/Compute/` (DX12, VK, MTL, CPU backends) and `Jolt/Shaders/` for GPU hair. Our `GLOB_RECURSE Jolt/*.cpp` **must exclude `Compute/` and `Shaders/`**, and must not define `JPH_USE_DX12`, `JPH_USE_VK`, `JPH_USE_MTL` or `JPH_USE_CPU_COMPUTE`.

**ISA flags.** These mirror Jolt's own `Jolt.cmake`. Put them on `tp_jolt` as PUBLIC:

| Compiler | Options | Definitions (emit explicitly — Jolt notes a mismatch "causes link errors") |
|---|---|---|
| MSVC (`cl`) | `/arch:AVX2` | `JPH_USE_AVX2 JPH_USE_AVX JPH_USE_SSE4_1 JPH_USE_SSE4_2 JPH_USE_LZCNT JPH_USE_TZCNT JPH_USE_F16C` |
| clang-cl, GCC, Clang | `-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfpmath=sse` (no `-mfma` in deterministic mode) | same |

**AVX2 becomes the client's minimum CPU.** Build the **launcher without AVX2**, and have it check CPUID and show a clear error message.

**Determinism: turn `JPH_CROSS_PLATFORM_DETERMINISTIC` on for both client and server**, from day one.

Jolt's cross-platform-deterministic mode needs:
- `/fp:precise` on MSVC (the default; since VS 2022 it no longer implies contraction);
- `-ffp-contract=off` on Clang and clang-cl (`/clang:-ffp-contract=off`);
- no `-ffast-math` anywhere in engine code that feeds physics;
- no FMADD;
- the same Jolt version on both sides.

The flag is a `JPH_VERSION_ID` feature bit, so inside one binary every translation unit must agree. Across the client and server *processes* there is no ABI coupling. Matching flags are what make:
- client prediction of owned vehicles converge with fewer corrections;
- server replays reproducible on Windows developer machines;
- a **CI golden-hash test** (simulate N steps, hash body states, compare MSVC, clang-cl, GCC and Clang) possible.

Cost is about 8% (report 06). Broadphase query order and callback order stay nondeterministic, so gameplay must not depend on them. Keep `JPH_ENABLE_ASSERTS` in Debug only, and `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` in Debug on MSVC. Implement `JobSystemWithBarrier` on our scheduler, and add `Jolt/Jolt.natvis` to targets.

---

## 6. Core Libraries

**ECS: flecs v4.1.6 (MIT, `fb55f3c`, 2026-06-28).** Archetype storage with built-in **relationships** (`ChildOf`, `IsA` prefabs, custom pairs such as `DockedTo`/`InFrame` — what reports 04 and 06 ask for), cached queries, observers, pipelines, `set_task_threads` to run systems on *our* job system, and `FLECS_CUSTOM_BUILD` to strip add-ons (REST explorer in dev builds only). Two files (`distr/flecs.c` ≈ 100k lines of C99). Rules: components come from our schema codegen; replication dirty bits live in our components (Iris-style push), because table-level change detection is not enough; network IDs are our own 64-bit IDs mapped to `flecs::entity`. Benchmark a 50k-entity zone at the vertical slice. **EnTT v4.0.0 is rejected** (sparse sets suit chunked replication and GPU upload poorly, per report 06; heavy templates). A **custom ECS** is the fallback only if the benchmark fails.

**Job system: custom fibers (≈2–3 kLOC), not enkiTS.** Counters, three priorities, separate I/O and compile pools (report 06 ENG-03). Windows: **Win32 fibers** (`CreateFiberEx`/`SwitchToFiber`, debugger-friendly) plus `/GT`. Linux x64: port marl's `osfiber_asm_x64.S` (Apache-2.0, HEAD `b8406ab`), **not** `ucontext` (`swapcontext` makes a `sigprocmask` syscall per switch). Rules: no TLS or OS mutex held across a wait; Tracy `TRACY_FIBERS`; `__sanitizer_start_switch_fiber` for Clang ASan; a `--jobs=threads` no-fiber mode for debugging. enkiTS v1.12 (zlib) is plan B behind the same counter-based API.

**Allocator: mimalloc v3.5.3 (MIT, `d4881d3`)** — upstream marks v3 recommended and 3.5.3 a *critical* fix. Link statically and use `mi_heap_*` behind our tagged-allocator interface; route Luau's `lua_Alloc`, Jolt's `Allocate` hooks, the flecs OS API and ImGui through it. **No global override on Windows** (it needs `mimalloc-redirect.dll` and the dynamic CRT, which conflicts with `/MT`). High cadence (8 releases in 2 months), so re-pin quarterly.

**Formatting: {fmt} 12.2.0 (MIT) over `std::format`.** All three standard libraries have `<format>` (MSVC, libstdc++ 13+, libc++ 17+), but fmt gives identical behavior and compile-time checks everywhere, correct UTF-8 console output on Windows, and faster compiles. Alias it as `helios::format`.

**JSON: yyjson 0.13.0 (MIT).** Fast read/write, mutable DOM, one C file; `YYJSON_READ_ALLOW_COMMENTS | ALLOW_TRAILING_COMMAS` gives JSONC. simdjson v4.6.11 rejected (parse-focused, larger).

**Text content format: canonical JSONC everywhere.** Schema-ordered keys, one property per line, GUID references, one entity per file: diffable and mergeable (report 08 F3), readable from Go (`tailscale/hujson` → `encoding/json`), one parser. **TOML rejected** (toml++'s last release was v3.4.0, 2023-10; two formats split tooling).

**Reflection: schema-first codegen.** One IDL generates C++ structs, type info and serializers; network descriptors; Luau bindings and `.d.luau`; Go structs; editor inspector metadata; Postgres migration stubs (as reports 02, 03, 04 and 08 ask). `helios-schemac` is a **C++ host tool built in the same CMake build**, so the client needs neither Python nor Go. A libclang header tool is rejected (needs an LLVM install on Windows). A small `HELIOS_REFLECT` macro set covers engine-internal types; C++26 reflection can come later.

**C++ ↔ Go payloads** use schema codegen (binary) over NATS. Do **not** vendor protobuf C++ or gRPC C++ (the abseil and OpenSSL chain is painful on Windows). Protobuf stays Go↔Go and external.
- Cells use **nats.c v3.14.0** (Apache-2.0, `6cb096a`):
  - `NATS_BUILD_STREAMING=OFF`;
  - TLS OFF on Windows dev builds;
  - TLS against system OpenSSL on Linux server builds.

---

## 7. Animation, AI, Audio, Networking, Crypto

**Animation: ozz-animation 0.17.0 (MIT, 2026-08-01).**
- Runtime sampling, blending, two-bone and aim IK, SoA math.
- Build the runtime libraries plus `gltf2ozz` in the cooker. Set `ozz_build_fbx=OFF` (the FBX SDK is proprietary), and turn samples and tests OFF.
- ACL (MIT) is **deferred**. Its last release was v2.1.0 (2023-12), though HEAD moved in 2025-09. Adopt it only if clip memory becomes a problem.

**Navigation: Recast/Detour v1.6.0** (zlib; still the latest tag, 2023-05).
- Bake tiles offline per cell. Use DetourTileCache for structure placement. Run it on the server only, plus the editor.

**Audio.**
- **miniaudio 0.11.25** (MIT-0/PD, 2026-03-04): keep it for devices, mixing and basic 3D.
- **SoLoud is rejected:** no commits since 2024-08.
- **Steam Audio v4.8.1** (Apache-2.0, 2026-02-11): P2, for HRTF, occlusion and reverb, consumed as the prebuilt `phonon` library. A source build pulls in heavy optional dependencies **[unverified]**.
- **Wwise and FMOD** (report 08 F5) are **commercial**. If chosen, keep them behind an `IAudioBackend` interface; they are outside the permissive policy.

**Networking: netcode v1.4.8 + reliable v1.4.5** (BSD-3, Más Bandwidth, both updated 2026-09-13).
- **netcode** handles connect tokens (minted by the Go auth service; matches report 07), encryption and replay protection, with a **byte-identical vendored libsodium subset** (`sodium.c/h`, ISC).
- **reliable** handles acks, fragmentation and RTT.
- Our replication layer sits on top. yojimbo v1.13.5 is *not* adopted, because its message and serialize layer overlaps our codegen.
- Compile `netcode.c` and `reliable.c` from our own CMake. Their CMake forces `/MT` unless the variable is already defined.
- **Rejected:**
  - **GameNetworkingSockets v1.6.0** (BSD-3): it needs protobuf plus OpenSSL/libsodium/BCrypt on Windows, and its relay features are Steam-centric **[unverified]**;
  - **ENet v1.3.18**: no encryption, last release 2024-04.

**Winsock notes.**
- **Set `SIO_UDP_CONNRESET` to FALSE** on every UDP socket; otherwise an ICMP port-unreachable makes the next `recvfrom` fail with `WSAECONNRESET`.
- `WSAStartup` + `ws2_32`; MB-sized `SO_RCVBUF`/`SO_SNDBUF` on servers; dual-stack IPv6 (`IPV6_V6ONLY=0`); bind dev servers to `127.0.0.1` to avoid the Firewall prompt. RIO/IOCP only if Windows ever hosts production cells. Linux: `recvmmsg`/`sendmmsg`, `SO_REUSEPORT`, UDP GSO.
- Windows tick timers: `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` rather than `timeBeginPeriod`, which Windows 11 honours less for hidden windows **[unverified]**.

**Crypto: Monocypher 4.0.3** (BSD-2/CC0, `ab2b16d`) **plus `optional/monocypher-ed25519.c`.**
- Uses: X25519, XChaCha20-Poly1305, BLAKE2b, Argon2, Ed25519 (for patch manifests and signed content).
- Randomness comes from the OS (`BCryptGenRandom`, `getrandom`).
- Full libsodium 1.0.22 is **not** vendored: it has no upstream CMake (MSVC builds use its `.sln`) and would duplicate netcode's subset.
- Go uses `golang.org/x/crypto` v0.57.0 (chacha20poly1305, argon2) and stdlib `crypto/ed25519`.

---

## 8. Platform Layer: SDL3, not GLFW

**Recommendation: SDL 3.4.16** (zlib, `fa2c02b`, 2026-09-02).

GLFW 3.5.1's only new features are unlimited mouse buttons and EGL/GLX config queries. It dropped XP/Vista and old MinGW. It still lacks what an MMO client needs:

| Need | GLFW 3.5.1 | SDL 3.4 |
|---|---|---|
| **IME for chat** (CJK composition, candidate window) | No preedit API | `SDL_StartTextInputWithProperties`, `SDL_SetTextInputArea`, `SDL_EVENT_TEXT_EDITING`; ImGui's `imgui_impl_sdl3` wires `PlatformSetImeDataFn` |
| Gamepads (Destiny-style) | XInput/DirectInput plus mapping database; no rumble, gyro or DualSense features | HIDAPI drivers for PS4/PS5/Switch/8BitDo, rumble and trigger rumble, gyro, `SDL_HINT_WINDOWS_GAMEINPUT` |
| Raw input on Windows | Raw mouse motion only | Raw mouse and raw keyboard (`SDL_HINT_WINDOWS_RAW_KEYBOARD`) |
| High-DPI and multiple windows (editor) | Yes | Yes (per-monitor v2, content scale), plus native file dialogs, taskbar progress (3.4) |
| Launcher UI without Vulkan | — | `SDL_Renderer` (D3D11/D3D12 on Windows) plus `imgui_impl_sdlrenderer3` |
| Steam Deck / Linux | X11 (Wayland off in our build) | X11 **and** Wayland, loaded dynamically (`SDL_X11_SHARED`, `SDL_WAYLAND_SHARED`) |

The ImGui v1.92.9-docking tag contains `imgui_impl_sdl3`, `imgui_impl_sdlrenderer3` and `imgui_impl_sdlgpu3` (verified).

**Build.** Add SDL3 through its own CMake with `SDL_STATIC=ON` and `SDL_SHARED=OFF`, using our CRT and `SDL_TEST_LIBRARY=OFF`. Linux needs X11, Wayland and xkbcommon *headers* at build time only.

**Migration cost.** The platform module plus switching the ImGui backend: roughly a week, and less now than after launcher and editor code exist.

---

## 9. Packaging, Launcher, Crash Reporting

**Install model.** Per-user, no admin rights, into `%LOCALAPPDATA%\Programs\Helios`. The **launcher is self-installing** (one signed `HeliosSetup.exe` = the launcher run with `--install`): a tiny stable bootstrap, versioned `app-<build>/` directories, and an atomic switch of `current`. Self-update downloads the new launcher, renames the running exe (Windows allows renaming a running image) and relaunches. Content ships as content-addressed chunks (reports 01, 06) under an **Ed25519-signed manifest** that Monocypher verifies before anything executes. HTTPS uses **WinHTTP** on Windows (system proxy and certificate store, no OpenSSL) and system libcurl on Linux; the patch service controls staged-rollout percentages.

**Installers.**

| Option | Status |
|---|---|
| **Inno Setup 7.1.0** (custom permissive license with an attribution clause, "must retain copyright notice … in About boxes") | Optional wrapper for a classic Setup.exe. **Flag:** not on the allow-list verbatim; needs sign-off. |
| **WiX v7.0.0** | **Reject.** It is **MS-RL** (reciprocal), and the **Open Source Maintenance Fee EULA** applies to users with revenue ≥ US$10k. Its Burn engine and custom actions end up inside the shipped installer. |
| **MSIX** | **Reject for the game.** The package directory is read-only (breaks self-patching), kernel anti-cheat drivers are incompatible, and the install location is constrained. |

**Code signing.**
- Sign **every** PE file (exe, dll, `crashpad_handler`) with Authenticode, using an RFC 3161 timestamp.
- Since June 2023 the CA/B Forum rules require keys on HSMs, so use:
  - **Azure Trusted Signing** (managed HSM, GitHub Action; eligibility and current name **[unverified]**), or
  - an OV/EV certificate held in a cloud HSM.
- SmartScreen reputation builds up per certificate and file, so keep a stable publisher and do not rename binaries each release. Whether EV still grants instant reputation is **[unverified]**.
- To avoid Defender false positives:
  - never use packers or UPX;
  - do not run executables from `%TEMP%`;
  - submit each release to the Microsoft Security Intelligence portal.

**Crash reporting: sentry-native 0.17.1** (MIT, 2026-09-24) with the **crashpad backend** (Apache-2.0; Sentry's crashpad fork builds with CMake, so no depot_tools).
- Upstream `chromium/crashpad` is rejected: it needs gn and depot_tools.
- Set `SENTRY_TRANSPORT=winhttp` on Windows and `custom` or `curl` on Linux.
- Do **not** call `MiniDumpWriteDump` from inside the crashing process. The out-of-process handler exists for exactly that reason.
- Ingest options:
  - self-hosted Sentry (FSL-licensed service; internal use is fine **[unverified]**);
  - GlitchTip;
  - our own Go endpoint plus `rust-minidump`.
- Upload symbols from CI: PDBs to a `symstore` layout (plus `/SOURCELINK`), and Linux split debug info keyed by build-id.

**Anti-cheat.**
- The server-authoritative design is the primary defence.
- **Easy Anti-Cheat via the EOS SDK** (free, supports Linux and Proton when opted in) is P1/P2 behind an interface. Its SDK is **proprietary**; flag it.
- Luau native codegen uses RW→RX pages with registered unwind info, which anti-cheat tolerates better than RWX.

---

## 10. Build & CI

**CMakePresets.json (v8).** Use Ninja Multi-Config everywhere; VS "Open Folder", VS Code CMake Tools and CLion all consume it. The VS `.sln` generator stays optional.

```json
{ "version": 8,
  "configurePresets": [
    { "name": "base", "hidden": true, "generator": "Ninja Multi-Config",
      "binaryDir": "${sourceDir}/out/${presetName}",
      "cacheVariables": { "CMAKE_CONFIGURATION_TYPES": "Debug;RelWithDebInfo;Release" } },
    { "name": "win", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
      "architecture": { "value": "x64", "strategy": "external" },
      "toolset": { "value": "host=x64", "strategy": "external" } },
    { "name": "windows-msvc",     "inherits": "win", "cacheVariables": { "CMAKE_CXX_COMPILER": "cl", "CMAKE_C_COMPILER": "cl" } },
    { "name": "windows-clang-cl", "inherits": "win", "cacheVariables": { "CMAKE_CXX_COMPILER": "clang-cl", "CMAKE_C_COMPILER": "clang-cl" } },
    { "name": "linux-gcc",   "inherits": "base", "cacheVariables": { "CMAKE_CXX_COMPILER": "g++", "CMAKE_C_COMPILER": "gcc" } },
    { "name": "linux-clang", "inherits": "base", "cacheVariables": { "CMAKE_CXX_COMPILER": "clang++", "CMAKE_C_COMPILER": "clang" } },
    { "name": "linux-server", "inherits": "linux-clang", "cacheVariables": { "HELIOS_BUILD_GRAPHICS": "OFF" } } ],
  "buildPresets": [
    { "name": "windows-msvc-debug",   "configurePreset": "windows-msvc", "configuration": "Debug" },
    { "name": "windows-msvc-release", "configurePreset": "windows-msvc", "configuration": "RelWithDebInfo" } ] }
```

Add matching `testPresets` and `workflowPresets` (configure → build → test).

**Compiler cache.**
- Use **sccache 0.18.0** (Apache-2.0) on Windows and **ccache 4.14** on Linux, through a `HELIOS_COMPILER_LAUNCHER` option.
- Both need **`/Z7` (Embedded)**, which is why §2 sets it. Precompiled headers are not cached by sccache on MSVC, so keep PCH off in CI or accept the misses.
- In GitHub Actions, use `mozilla-actions/sccache-action` with the GHA cache backend.

**PDBs and debugging.**
- Link with `/DEBUG:FULL`; `/DEBUG:FASTLINK` only for local iteration, never for shipped builds.
- Use `/SOURCELINK` so PDBs map back to commits, and publish symbols to the symbol store on every tagged build.
- **natvis:** add `Jolt.natvis`, `imgui.natvis` and Helios's own (handles, containers, entity IDs) as target sources. CMake passes `/NATVIS:` for MSVC-linked targets **[unverified for Ninja]**.

**GitHub Actions matrix.** Pin the images rather than using `-latest`; `windows-2025`/`ubuntu-24.04` contents are **[unverified]**.

| Job | When |
|---|---|
| windows-msvc Debug + RelWithDebInfo (VS 2022) | per commit |
| windows-clang-cl RelWithDebInfo | per commit |
| linux-gcc and linux-clang RelWithDebInfo, plus `linux-server` (`HELIOS_BUILD_GRAPHICS=OFF`, report 06 ENG-02) | per commit |
| Go: `CGO_ENABLED=0` build and test for linux/amd64 **and** windows/amd64 (cross-compiled) | per commit |
| Format and tidy: clang-format and clang-tidy **pinned** (e.g. the PyPI `clang-format==<pin>` wheel), because VS's bundled LLVM differs from LLVM 23.1.x | per commit |
| Linux ASan+UBSan and TSan; MSVC `/fsanitize=address` (not with `/RTC` or `/ZI`; put the ASan runtime DLL on `PATH` for tests) | nightly |
| Determinism golden-hash (Jolt) across all four compilers; Vulkan smoke test on lavapipe | nightly |
| Slang source build check; VS 2026 build | weekly |

**Tests.** doctest **2.5.3** (upgrade from 2.4.12) through CTest.

---

## 11. Go Backend Libraries

**Toolchain.** Everything builds with `CGO_ENABLED=0` for Windows and Linux. Use **Go 1.27.1**: set `go 1.27.0` and `toolchain go1.27.1` in `go.mod`.

| Module | Version (date) | License | Use |
|---|---|---|---|
| `github.com/jackc/pgx/v5` | v5.11.0 (2026-09-07) | MIT | Postgres driver and pool. Use `pgx` natively, not `database/sql`, in services. |
| `github.com/exaring/otelpgx` | v0.12.0 | Apache-2.0 | pgx tracing |
| `github.com/redis/go-redis/v9` | v9.22.0 (2026-08-03) | BSD-2 | **Chosen** Valkey/Redis client: mature, works with Valkey and miniredis, has OTel hooks |
| `github.com/valkey-io/valkey-go` | v1.0.78 | Apache-2.0 | Alternative if RESP3 client-side caching is needed |
| `github.com/nats-io/nats.go` | v1.54.0 (2026-09-18) | Apache-2.0 | NATS and JetStream client |
| `github.com/nats-io/nats-server/v2` | v2.15.0 (2026-09-17) | Apache-2.0 | **Embedded** in the dev binary (`server.NewServer`, JetStream on, `DontListen` plus in-process connection) |
| `connectrpc.com/connect` | v1.21.0 (2026-09-08) | Apache-2.0 | **Chosen** over grpc-go v1.84.0. It speaks gRPC, gRPC-Web and Connect over plain `net/http`, which the browser-based writer and localization tools need (report 03). |
| `google.golang.org/protobuf` / `buf` | v1.36.12 / v1.73.0 | BSD-3 / Apache-2.0 | Service schemas, lint, breaking-change checks |
| `github.com/go-chi/chi/v5` | v5.3.2 | MIT | HTTP routing (admin, ESI-style public API) |
| `github.com/golang-jwt/jwt/v5` | v5.3.1 | MIT | JWT with EdDSA. Keep netcode connect tokens separate. |
| `golang.org/x/crypto` | v0.57.0 | BSD-3 | `argon2.IDKey` (argon2id), chacha20poly1305 for connect tokens |
| `go.opentelemetry.io/otel` (+sdk, otelhttp v0.71.0) | v1.46.0 | Apache-2.0 | Traces and metrics |
| `github.com/prometheus/client_golang` | v1.24.1 | Apache-2.0 | `/metrics` |
| `github.com/pressly/goose/v3` | v3.28.0 (2026-09-02) | MIT | Migrations (SQL files, embedded with `embed.FS`) |
| `github.com/sqlc-dev/sqlc` (tool) | v1.31.1 | MIT | Type-safe query codegen against Postgres |
| `github.com/fergusstrange/embedded-postgres` | v1.34.0 (2026-03-17) | MIT | **Dev: a real PostgreSQL 18.3 on Windows and Linux with no Docker.** It downloads zonky binaries once from Maven Central and caches them. |
| `github.com/alicebob/miniredis/v2` | v2.39.0 | MIT | Dev and test Valkey stand-in. Valkey has no native Windows build. |
| `modernc.org/sqlite` | v1.59.0 (2026-09-15) | BSD-3 | Offline tools, editor caches, tiny single-file stores |
| `github.com/klauspost/compress` | v1.20.0 | BSD-3/Apache-2.0 | zstd for the patch and CDN tooling (interoperates with C++ zstd) |
| `github.com/tailscale/hujson` | pseudo-version 2026-07-27 | BSD-3 | Read JSONC content in Go |

**Why embedded Postgres instead of SQLite for local development.** The ledger, market and persistence services will use `SELECT … FOR UPDATE SKIP LOCKED`, SERIALIZABLE retries, JSONB, partitioning and `LISTEN/NOTIFY`, none of which SQLite has. Maintaining two SQL dialects doubles the migrations and hides bugs until staging. `helios-dev.exe` should embed nats-server, embedded-postgres and miniredis, with **SQLite as the fallback only when Postgres binaries are unavailable** (offline or first run).

---

## 12. Content Formats

- **glTF: cgltf v1.15** (keep). Add **ufbx v0.23.0** (MIT/PD, 2026-06-22) for FBX import in the editor and cooker, following report 08 F6.
- **Textures:** cook straight to BCn (BC7 color, BC5 normals, BC4 masks, BC6H HDR, BC1 low tier) with zstd per chunk. **bc7enc_rdo** (MIT/Unlicense; no tags, pin HEAD `b943862`, 2026-07-30) encodes BC1–5 and BC7 with **RDO** for smaller LZ output; leave its `bc7e.ispc` out (needs the ISPC compiler). **basis_universal v2_50** (Apache-2.0, 2026-07-22) covers HDR (UASTC HDR → BC6H) and optional **KTX2/XUBC7** for download-size-critical streamed textures. KTX-Software is unnecessary (our container is our own; basisu writes `.ktx2`). **ISPCTextureCompressor rejected** (needs ISPC; last commit 2024-09).
- **EXR: tinyexr v3.2.0** (BSD-3) for import. Its miniz is MIT. OpenEXR 3.5 is rejected (Imath and libdeflate dependencies, heavy).
- **Fonts: FreeType 2.14.3** (2026-03-22), **under the FTL**. **Flag:** FreeType is dual FTL/GPLv2; FTL is BSD-like but **requires crediting FreeType in product documentation**, so it is permissive but not on our allow-list verbatim. **msdfgen v1.13** (MIT) bakes SDF/MSDF atlases offline for 3D and diegetic UI. stb_truetype stays for editor/debug only (weak at small sizes).
- **Shaping and text:** **HarfBuzz 14.5.0** ("Old MIT", 2026-09-21; amalgamated `harfbuzz.cc` build), **SheenBidi v3.0.0** (Apache-2.0) for bidi, **libunibreak 8.0** (zlib, 2026-09-15) for line and grapheme breaks.
- **Localization: ICU 78.3** (Unicode-3.0, permissive) is **tools and backend only, for now.** Its data file is tens of MB. The client gets CLDR plural and gender rules through codegen and a small MessageFormat subset (report 03 P1-2). Go uses `golang.org/x/text`.
- **Runtime UI: RmlUi 6.3** (MIT, 2026-08-22), following report 08 F1.
  - It uses FreeType by default.
  - Its Lua plugin targets the stock Lua C API, **not Luau**, so we write our own bindings through schema codegen.

---

## 13. Dependency Manifest

Status values:
- **vendored**: keep at the current pin;
- **upgrade**: bump the pin;
- **add**: vendor as source into `third_party/`;
- **tool**: build-time or developer only, never shipped;
- **reject**.

| Name | Pinned tag (commit) | License | Purpose | Status |
|---|---|---|---|---|
| Vulkan-Headers | v1.4.364 (b0c3dd6) | Apache-2.0/MIT | Vulkan API | vendored |
| volk | 1.4.350 (3ca312a) | MIT | Vulkan loader | vendored (vulkan-sdk-1.4.357.0 available) |
| VulkanMemoryAllocator | v3.4.0 | MIT | GPU memory | vendored (latest) |
| Dear ImGui docking | v1.92.9-docking | MIT | Editor, debug and launcher UI | vendored. **Add backends `imgui_impl_sdl3` and `imgui_impl_sdlrenderer3`.** |
| ImGuizmo / ImPlot | master 18cef5e / v1.0 | MIT | Gizmos, plots | vendored |
| **Jolt Physics** | **v5.6.0 (e77f175)** | MIT | Physics | **upgrade** (from v5.3.0). Fix ISA flags, add determinism flags, exclude `Compute/` and `Shaders/`. |
| meshoptimizer | v1.2 | MIT | Mesh optimization | vendored (latest) |
| cgltf | v1.15 | MIT | glTF | vendored (latest) |
| stb | master (2c980bb) | MIT/PD | Images, debug fonts | vendored |
| miniaudio | 0.11.25 | MIT-0/PD | Audio | vendored (latest) |
| zstd | v1.5.7 | BSD-3 | Compression | vendored. **Remove `ZSTD_MULTITHREAD=0`.** |
| xxHash | **v0.8.4 (c87183a)** | BSD-2 | Hashing | upgrade (from v0.8.3) |
| doctest | **v2.5.3 (2d0a935)** | MIT | Tests | upgrade |
| Recast/Detour | v1.6.0 | zlib | Navigation | vendored (latest) |
| **Monocypher** | **4.0.3 (ab2b16d)** + `optional/monocypher-ed25519.c` | BSD-2/CC0 | Crypto, signatures | **upgrade (security fix)** |
| **Tracy** | **v0.14.1 (30997d5)** | BSD-3 | Profiler | **upgrade**. Pin the viewer to the same version. |
| Luau | 0.739 (a62362a) | MIT | Scripting | vendored (latest) |
| GLFW | 3.4 | zlib | Windowing | **reject/remove** (replaced by SDL3) |
| **SDL3** | **release-3.4.16 (fa2c02b)** | zlib | Platform, input, IME, gamepads | **add** |
| **flecs** | **v4.1.6 (fb55f3c)** | MIT | ECS | **add** |
| **mimalloc** | **v3.5.3 (d4881d3)** | MIT | Allocator | **add** |
| **{fmt}** | **12.2.0 (1be298e)** | MIT | Formatting | **add** |
| **yyjson** | **0.13.0 (6447536)** | MIT | JSON/JSONC | **add** |
| **ozz-animation** | **0.17.0 (744eb9d)** | MIT | Animation runtime and gltf2ozz | **add** |
| **netcode** | **v1.4.8 (47a156b)** | BSD-3 (+ ISC sodium subset) | Secure UDP connections | **add** |
| **reliable** | **v1.4.5 (e4e7092)** | BSD-3 | Acks, fragmentation | **add** |
| **nats.c** | **v3.14.0 (6cb096a)** | Apache-2.0 | Cell ↔ NATS | **add** (server targets only) |
| **sentry-native** | **0.17.1 (620f6af)** + its crashpad submodule | MIT + Apache-2.0 | Crash reporting | **add** |
| **FreeType** | **VER-2-14-3 (0a0221a)** | FTL (choose FTL) | Font rasterization | **add (flag: attribution)** |
| **HarfBuzz** | **14.5.0 (863d3f7)** | Old MIT | Text shaping | **add** |
| **SheenBidi** | **v3.0.0 (cfe430e)** | Apache-2.0 | Bidirectional text | **add** |
| **libunibreak** | **libunibreak_8_0 (28a2756)** | zlib | Line breaking | **add** |
| **RmlUi** | **6.3 (ba95ffe)** | MIT | Player UI | **add** (P1) |
| basis_universal | v2_50 (9bebe16) | Apache-2.0 | HDR/BC6H, KTX2/XUBC7 encode; transcoder | **add** (encoder: tool; transcoder: runtime if KTX2 is used) |
| bc7enc_rdo | HEAD b943862 | MIT/Unlicense | BC1–7 RDO encoding | **tool** |
| tinyexr | v3.2.0 (6f470c9) | BSD-3 | EXR import | **tool** |
| ufbx | v0.23.0 (fcc5d6b) | MIT/PD | FBX import | **tool** |
| msdfgen | v1.13 (1874bcf) | MIT | SDF font atlases | **tool** |
| Slang (prebuilt) | v2026.18.2 (afeaf51) | Apache-2.0 WITH LLVM-exception | Shader compiler, reflection | **tool** (editor loads it; never shipped) |
| luau-lsp | 1.70.0 | MIT | Luau IDE support | **tool** |
| StyLua | v2.5.2 | **MPL-2.0** | Luau formatter | **tool only** |
| sccache / ccache | v0.18.0 / v4.14 | Apache-2.0 / GPL-3.0 | Compiler cache | **tool** (ccache is GPL; tool-only use is fine) |
| SPIRV-Reflect | vulkan-sdk-1.4.357.0 | Apache-2.0 | Reflection cross-check | tool (tests, optional) |
| glslang | 16.6.0 (e1b562a) | BSD/MIT/Apache + GPL-w/Bison-exc. file | Plan-B compiler | reject (plan B) |
| DXC | v1.10.2605.37 | NCSA | HLSL compiler | reject |
| SPIRV-Cross | vulkan-sdk-1.4.357.0 | Apache-2.0 | Cross-compilation | reject (Slang covers it) |
| EnTT | v4.0.0 | MIT | ECS | reject |
| enkiTS | v1.12 | zlib | Tasks | reject (plan B) |
| marl | HEAD b8406ab | Apache-2.0 | Fibers | reject as a library; port the asm context switch only |
| simdjson / toml++ | v4.6.11 / v3.4.0 | Apache-2.0 / MIT | Parsers | reject |
| ACL | v2.1.0 | MIT | Animation compression | defer |
| SoLoud | (no release since 2020; HEAD 2024-08) | zlib | Audio | reject (stale) |
| Steam Audio | v4.8.1 | Apache-2.0 | HRTF, occlusion | defer (P2, prebuilt) |
| GameNetworkingSockets | v1.6.0 | BSD-3 | Transport | reject |
| ENet | v1.3.18 | MIT | Transport | reject |
| libsodium (full) | 1.0.22 | ISC | Crypto | reject (netcode's subset suffices) |
| protobuf C++ / gRPC C++ | — | BSD-3 / Apache-2.0 | RPC | reject in the engine |
| ISPCTextureCompressor / KTX-Software / OpenEXR | v1.6.0 / v4.4.2 / v3.5.0 | MIT / Apache-2.0 / BSD-3 | Textures, EXR | reject |
| ICU4C | release-78.3 | Unicode-3.0 | i18n | tools/backend only (defer for client) |
| chromium/crashpad upstream | HEAD | Apache-2.0 | Crash handler | reject (needs depot_tools) |
| WiX / MSIX | v7.0.0 / — | MS-RL + OSMF / — | Installer | reject |
| Inno Setup | is-7_1_0 | Inno Setup License | Setup wrapper | optional (legal sign-off) |
| EOS SDK (Easy Anti-Cheat) | — | Proprietary | Anti-cheat | defer (flag) |

---

## 14. Risks

1. **Slang is a fast-moving prebuilt binary** (≈2 releases/month, no cross-version ABI promise). *Mitigation:* pin and hash-verify, weekly source-build check, and keep core passes inside the GLSL-compatible subset so glslang plan B stays viable.
2. **flecs has one maintainer and is ~100k lines of C.** *Mitigation:* confine flecs types to `engine/world`, keep IDs and schema our own, benchmark before content depends on it.
3. **Fibers:** TLS caching (`/GT` exists only on MSVC), ASan annotations, harder debugging. *Mitigation:* threads-only mode; enkiTS plan B.
4. **AVX2 minimum CPU.** *Mitigation:* launcher CPUID check. An SSE4.2 variant is possible but doubles the determinism matrix.
5. **Determinism has gaps:** Jolt query/callback order, and any `-ffast-math` leaking into physics-facing code. *Mitigation:* the four-compiler golden-hash job.
6. **Licenses needing legal sign-off:** FreeType FTL (documentation credit); Inno Setup (About-box notice); ISC (netcode's sodium subset — permissive, add to the allow-list); glslang's Bison-exception file (plan B only); proprietary EAC/EOS and Wwise/FMOD if chosen. ccache (GPL) and StyLua (MPL) are tool-only, never linked.
7. **Release churn** (mimalloc 8 releases in 2 months; Luau weekly). *Mitigation:* a quarterly bump window, with `MANIFEST.md` recording the SHA and reason.
8. **Code-signing and SmartScreen rules shift** (managed-signing eligibility, EV treatment). Re-verify before the first public build.
9. **embedded-postgres downloads binaries on first run**; offline developers fall back to SQLite, so keep smoke tests on that path.

---

## 15. Sources

All repositories were checked on 2026-09-25 with `git ls-remote` and shallow clones (tag dates are commit dates). Files read are named where they back a specific claim.

**Physics**
- https://github.com/jrouwe/JoltPhysics — `Docs/ReleaseNotes.md` (v5.4–5.6), `Build/CMakeLists.txt`, `Jolt/Jolt.cmake`, `Jolt/Core/Core.h`

**Scripting**
- https://github.com/luau-lang/luau — vendored `VM/include/lua.h`, `luaconf.h`, `CodeGen/src/CodeAllocator.cpp`, `CodeBlockUnwind.cpp`, `Common/include/Luau/ExperimentalFlags.h`
- https://github.com/JohnnyMorganz/luau-lsp (README, LICENSE)
- https://github.com/JohnnyMorganz/StyLua (LICENSE)

**Shaders**
- https://github.com/shader-slang/slang — `LICENSE`, `.gitmodules`, `docs/building.md`, `README.md`
- https://github.com/KhronosGroup/glslang — `LICENSE.txt`, `REUSE.toml`, `CMakeLists.txt`, `known_good.json`
- https://github.com/KhronosGroup/SPIRV-Tools — `CMakeLists.txt`, `source/CMakeLists.txt` (Python requirement)
- https://github.com/KhronosGroup/SPIRV-Reflect
- https://github.com/KhronosGroup/SPIRV-Cross
- https://github.com/microsoft/DirectXShaderCompiler

**Platform and core libraries**
- https://github.com/glfw/glfw — `docs/news.md` (3.5.1)
- https://github.com/libsdl-org/SDL — `WhatsNew.txt`, `include/SDL3/SDL_hints.h`, `SDL_keyboard.h`, `CMakeLists.txt`, `LICENSE.txt`
- https://github.com/ocornut/imgui (v1.92.9-docking backends tree)
- https://github.com/SanderMertens/flecs — `docs/BuildingFlecs.md`, `docs/Systems.md`
- https://github.com/skypjack/entt
- https://github.com/dougbinks/enkiTS
- https://github.com/google/marl
- https://github.com/edubart/minicoro
- https://github.com/microsoft/mimalloc (readme: v3 recommended, Windows override notes)
- https://github.com/fmtlib/fmt
- https://github.com/ibireme/yyjson
- https://github.com/simdjson/simdjson
- https://github.com/marzer/tomlplusplus

**Animation and audio**
- https://github.com/guillaumeblanc/ozz-animation (`CMakeLists.txt` options)
- https://github.com/nfrechette/acl
- https://github.com/mackron/miniaudio
- https://github.com/jarikomppa/soloud
- https://github.com/ValveSoftware/steam-audio

**Networking and crypto**
- https://github.com/mas-bandwidth/netcode — `BUILDING.md`, `CMakeLists.txt`, `sodium/`
- https://github.com/mas-bandwidth/reliable
- https://github.com/mas-bandwidth/yojimbo
- https://github.com/ValveSoftware/GameNetworkingSockets
- https://github.com/lsalzman/enet
- https://github.com/jedisct1/libsodium
- https://github.com/LoupVaillant/Monocypher — `CHANGELOG.md` 4.0.3
- https://github.com/nats-io/nats.c

**Profiling, crash reporting and installers**
- https://github.com/wolfpld/tracy — `NEWS` (v0.12–v0.14.1)
- https://github.com/getsentry/sentry-native — `LICENSE`, `.gitmodules`, `CMakeLists.txt`
- https://github.com/chromium/crashpad
- https://github.com/google/breakpad
- https://github.com/jrsoftware/issrc — `license.txt`
- https://github.com/wixtoolset/wix — `LICENSE.TXT`, `OSMFEULA.txt`, `README.md`

**Textures, fonts, text and UI**
- https://github.com/BinomialLLC/basis_universal (README v2.5)
- https://github.com/richgel999/bc7enc_rdo (LICENSE, README)
- https://github.com/GameTechDev/ISPCTextureCompressor
- https://github.com/KhronosGroup/KTX-Software
- https://github.com/syoyo/tinyexr
- https://github.com/AcademySoftwareFoundation/openexr
- https://github.com/ufbx/ufbx
- https://github.com/freetype/freetype (`LICENSE.TXT`)
- https://github.com/harfbuzz/harfbuzz (`COPYING`)
- https://github.com/Tehreer/SheenBidi
- https://github.com/adah1972/libunibreak
- https://github.com/unicode-org/icu
- https://github.com/Chlumsky/msdfgen
- https://github.com/mikke89/RmlUi

**Build tooling**
- https://github.com/Kitware/CMake
- https://github.com/mozilla/sccache
- https://github.com/ccache/ccache
- https://github.com/llvm/llvm-project (tags)

**Go**
- Go toolchain versions and dates: https://proxy.golang.org/golang.org/toolchain/@v/list and the `.info` endpoints
- Go module versions: `https://proxy.golang.org/<module>/@latest` for every module in §11
- https://github.com/fergusstrange/embedded-postgres (README, `config.go`: default V18 = 18.3.0)

**Internal**
- Research reports 01–09 in `docs/research/`
- `third_party/MANIFEST.md`, `third_party/CMakeLists.txt`, root `CMakeLists.txt`
- A local build of the current tree (GCC 13.3, CMake 3.28.3, Ninja), including the `nm` check of `libtp_zstd.a`
