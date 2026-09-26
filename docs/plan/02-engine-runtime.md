# 02 — Engine Runtime

> **Status:** draft v5 (round-5 minor revisions: the illegal-instruction backstop passes deliberate traps
> through, handles only VEX/EVEX and POPCNT faults and hands #UD to the crash handler once it installs, and the
> mimalloc init mode is stated as the default that check 3 guards (§1.1); heap lifetime and sharded tag
> accounting from the ECS spike (§2.2); Jolt ordering independence: stable body keys, the `stable-order`
> patch, no cross-`Update` contact cache on predicted bodies and body tables for restores, with RT-03's
> permuted variant and RT-19's multi-tile case (§5.4, §7.1); the Luau codegen-fuel and inline-counter patches
> and the replay-keyframe script rebase (§7.4; 04 §10.2); round-3 review fixes: whole-image ISA levels with a pre-gate audit (§1.1, ADR-011
> amendment); terrain collision tiles with a workload model, a no-substitution rule and RT-20 (§5.8a); project
> schema packages that need no C++ (§3.8, RT-21); from 03's round-3 fixes: CPU tables at 45, 30 and 120 fps
> with the Performance-mode interleave (§2.4, RT-12 clauses), the appearance skeleton and facial runtime (§7.2,
> RT-22); round-4 review fixes: the Windows CPU gate becomes the first TLS callback, ahead of mimalloc's and
> Tracy's pre-`main` hooks, with an enumerating pre-gate audit and SDK consumers held to `avx2` (§1.1); the
> terrain fence cap becomes a core-ms budget that fits its wall bound, with client fence lines in §2.4 and
> RT-12's landing fence case (§5.8a); from the other sections' round-4 fixes: `det::HitboxSampler` for server and
> client hitbox poses, the Luau `det-math` patch and AMD/Intel runs in RT-03 and RT-04 (§7.2, §7.4; 04 §10.2),
> rendertest-only animation-LOD cvars (§7.2; 03 §8.4a), and WP-0.2r, WP-0.5r and the WP-2.16 sub-WPs in §1.1,
> RT-09 and §8.6 (09 §5.10.4, §2.3a)). **Conforms to:** ADR-001, -001a, -002, -004, -005, -006, -007, -008,
> -011, -012, -013, -014, -015, -016.
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
| 2 | `replay` | `SimInbox`, replay recorder, log format and replayer driver (04 §10.2); state hashes are supplied by `replication` through a callback | ✓ | core, reflect | 04 |
| 2 | `app`, `input` | SDL3 windows, events, devices, IME; actions, contexts, rebinding, haptics | – | core, reflect → SDL3 | 02 |
| 3 | `physics` | Jolt grids, characters, vehicles, buoyancy, queries | ✓ | core, math, asset → Jolt | 02 |
| 3 | `anim` | ozz sampling, anim graphs, IK, retargeting, LOD, hitbox poses | ✓ | core, math, asset → ozz | 02 |
| 3 | `nav` | Recast/Detour per grid | ✓ | core, math, asset → Recast | 02 |
| 3 | `pcg` | Fixed-point `hnoise`, terrain-graph VM, stamps, scatter, asteroids | ✓ | core, math, reflect, asset | 02 |
| 3 | `script` | Luau VMs, sandbox, budgets, tasks, bindings, DAP adapter | ✓ | core, reflect, asset → Luau | 02 |
| 3 | `net` | Transport | ✓ | core, reflect → netcode | 04 |
| 3 | `audio` | miniaudio mixer, events, banks, buses, 3D | – | core, math, asset → miniaudio | 02 |
| 3 | `voice` | Voice capture, Opus codec, jitter buffers, voice spatialization (§7.3, ADR-015; Phase 3) | – | core, audio, net → libopus | 02 |
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
  - a HEADLESS module reaches `app`, `input`, `rhi`, `render`, `audio`, `voice`, `text`, `ui` or `presentation`;
  - the client, cell or gateway links an `EDITOR_ONLY` module.
- **Build order and presets.** `HELIOS_MODULE_ORDER` gains the new modules. The `linux-headless` preset
  (servers and tools, `HELIOS_BUILD_GRAPHICS=OFF`) builds on every commit.
- **Link model (ADR-016, §1.4).** Shipping builds link every module statically into one image. Dev builds
  (`HELIOS_MODULAR=ON`) combine the modules into three shared libraries, one per link group, so a game DLL can
  be reloaded without duplicating process singletons.
- **No third-party types in public headers.** This keeps the custom-ECS fallback possible (ADR-004).
- **ISA levels (ADR-011 amendment; 08 §2.2).** Whole images are built at `avx2` or `base`, and only the CPU
  gate runs before AVX2 code can. See *ISA levels and the pre-gate audit* below.

**ISA levels and the pre-gate audit.** A baseline ISA is needed in only two places: code that runs before
the CPU gate, and images that must run on a CPU without AVX2 so they can explain the refusal. After the
gate has passed, the CPU is known to support AVX2, so AVX2 code anywhere in the image is safe. That includes
an AVX2 copy of an inline function that wins the linker's COMDAT pick. The ODR leak is a hazard only on the
pre-gate path and in baseline images.

A per-file allowlist cannot work anyway. Jolt's headers pick AVX paths inline (`DVec3` stores `__m256d`
under `JPH_USE_AVX`, and the `Vec3`/`Vec4`/`Mat44`/`DMat44` `.inl` files use `_mm256` intrinsics). Every
`physics` TU therefore needs AVX2, and its STL and engine instantiations would win COMDAT picks against
baseline callers. So Helios builds **whole images at one level** and audits the pre-gate path:

| Level | Flags: MSVC · clang-cl · GCC and Clang | Built at this level |
|---|---|---|
| **`avx2`** | `/arch:AVX2 /fp:precise`, never `/fp:contract` or `/fp:fast` · the same plus `-mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma /clang:-ffp-contract=off` · `-mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma -mfpmath=sse -ffp-contract=off` | Every image that links a runtime module: client, cell (every role), editor, bot, gateway, voice and the `helios-*` tools. Every engine module, gem, game module and third-party library those images link is built at this level too. `tp_jolt`'s `JPH_USE_AVX2 … JPH_USE_F16C` defines stay PUBLIC because Jolt's headers need them. Every consumer is now at the same level, so the defines agree by construction |
| **`base`** (x86-64-v1) | Default `/arch:SSE2` · the same · `-march=x86-64 -mtune=generic`, no `-mcx16` (08 §2.1.1) | (a) The launcher and the `Helios` bootstrap (08 §2.1–2.2), with their own `base` builds of `core`, `app`, `ui`, `text`, `loc`, `patch`, `crash` and their third-party libraries. (b) **The CPU gate inside every `avx2` image**: two C objects, the probe `engine/core/src/cpugate/cpu_gate.c` and the OS hook `engine/core/src/platform/{win32,posix}/cpu_gate_hook.c`. 08 and ADR-011 call the pair the "gate TU" |

- **Build rules.**
  - `cmake/HeliosIsa.cmake` defines `HELIOS_ISA_AVX2` and `HELIOS_ISA_BASE`.
  - `helios_executable(… ISA avx2|base)` applies the level to the target and to the modules it links. The
    `<module>@base` object libraries exist only for the modules the launcher links.
  - Configure fails if a `base` image links `physics`, `pcg`, `tp_jolt` or any other `avx2` library.
  - `tp_jolt` keeps its defines PUBLIC but no longer carries ISA compile options of its own. Its
    `/arch:AVX2` and `-mavx2 …` options are PUBLIC today (09 §8.1 lists them; WP-0.2r removes them).
  - No FMA is used anywhere, and `JPH_USE_FMADD` stays off. VEX-encoded scalar and 128-bit ops round exactly
    like their SSE forms, so determinism (§7.1, RT-03, RT-04) is unchanged.
- **SDK consumers are held to `avx2` too.** A game module built against the binary SDK (07 §1.10, ADR-001a)
  is compiled by the user's CMake, not ours, so the level travels with the SDK:
  - *Flags.* `HeliosConfig.cmake` puts the `avx2` flag set (the column above for the consumer's compiler) on
    every imported SDK target as `INTERFACE` compile options. The SDK's `helios_game_module()` and
    `helios_executable()` also apply it to every static library in the module's link closure, as in-tree
    builds do, so a consumer's own third-party library cannot stay at the compiler default.
  - *Header.* `helios/sdk_config.h`, which every public SDK header includes, adds `#error`s next to its
    compiler-version check. GCC, Clang and clang-cl must define `__AVX2__`, `__BMI__`, `__BMI2__`, `__LZCNT__`,
    `__POPCNT__` and `__F16C__`, and must not define `__FMA__`. MSVC must define `__AVX2__` and neither
    `_M_FP_FAST` nor `_M_FP_CONTRACT`. The message names the missing flag and the fix ("build the module with
    `helios_game_module()`").
  - *Load time.* `helios_module_info()` reports `isa` (§1.4), and the module loader refuses a module that does
    not report `avx2`.
  - *Gate and audit.* `Helios::runtime`'s `INTERFACE` link options pull the gate objects into consumer
    executables (`/INCLUDE:helios_cpu_gate_tls_entry` on Windows), and the SDK's `helios_executable()` runs
    `helios-tool isa-audit --pre-gate` (check 3 below) after each consumer link.
  - *Proof.* The nightly `sdk-consumer` job (ADR-001a) runs `helios-tool isa-audit --objects` over every object
    of `starter-blank`'s game module and its static libraries. Each object's recorded command line must carry
    the `avx2` set and no FMA or contraction flag. On MSVC and clang-cl the record is CodeView `LF_BUILDINFO`
    (`/Z7`). On Linux it is `.GCC.command.line`, from `-frecord-gcc-switches` (GCC) or
    `-frecord-command-line` (Clang), which the `INTERFACE` options add. An object with no record fails. The
    job also runs check 3 on the consumer's shipping and dev images, and check 5's SDE `-nhm` pass on its
    shipping client, which must exit with code 78.
- **The CPU gate runs before any other code in the image, third-party code included.** 08 §2.2's
  storefront SKUs start the client directly, so the gate, not the launcher, is what stands between a pre-AVX2
  CPU and a raw fault.
  - *Windows: the first TLS callback.* The hook is a `PIMAGE_TLS_CALLBACK` in section `.CRT$XLA0`
    (`#pragma const_seg` on MSVC and clang-cl, `__attribute__((section(".CRT$XLA0"), used))` on MinGW). The
    object pulls in `/INCLUDE:_tls_used` and `/INCLUDE:helios_cpu_gate_tls_entry`, so neither `/OPT:REF` nor
    LTCG can drop it. The linker sorts grouped sections by the text after `$`, and GNU ld's PE script uses
    `SORT(.CRT$XL*)`. The hook therefore lands directly after the CRT's `__xl_a` sentinel (`.CRT$XLA`) and
    becomes `IMAGE_TLS_DIRECTORY.AddressOfCallBacks[0]`. The loader calls an image's TLS callbacks before its
    entry point, so the gate precedes every other pre-`main` mechanism in that image:

    | Mechanism | Sections | Called from | Users in the tree today |
    |---|---|---|---|
    | Other TLS callbacks | `.CRT$XLB`–`.CRT$XLY` | The loader, before the entry point | mimalloc's `mi_tls_attach` (`.CRT$XLB`) and `mi_tls_detach` (`.CRT$XLY`) in its default `MI_WIN_INIT_USE_CRT_TLS` mode (`third_party/mimalloc/src/prim/windows/prim.c`); the CRT's `__dyn_tls_init` (`.CRT$XLC`) and `__dyn_tls_dtor` (`.CRT$XLD`); MinGW's winpthreads (`.CRT$XLF`) |
    | Dynamic `thread_local` initializers | `.CRT$XDA`–`.CRT$XDZ` | `__dyn_tls_init`, a TLS callback | Tracy's `s_token`, `s_token_detail`, `s_threadHandle` and `s_gpuCtx` (`TracyProfiler.cpp`) |
    | Raw DLL entry | `_pRawDllMain` | `_DllMainCRTStartup`, before the CRT initializes (DLLs only) | None. mimalloc's `MI_WIN_INIT_USE_RAW_DLLMAIN` mode would set it. `tp_mimalloc` defines only `MI_STATIC_LIB` and relies on mimalloc's default, which `prim.c` sets to `MI_WIN_INIT_USE_CRT_TLS` for every Helios toolchain (only Intel's compilers get `MI_WIN_INIT_USE_TLS_DLLMAIN`, whose hooks are the same `.CRT$XLB`/`.CRT$XLY` symbols). The raw mode must be requested explicitly, and check 3 fails an image whose `_pRawDllMain` is not CRT-owned, so no define is needed |
    | C initializers | `.CRT$XIA`–`.CRT$XIZ` | The entry point (`_initterm_e`) | The CRT; mimalloc's `mi_crt_init` (`.CRT$XIB`) |
    | C++ initializers | `.CRT$XCA`–`.CRT$XCZ` | The entry point (`_initterm`) | Tracy's `init_seg(".CRT$XCB")` statics; `init_seg(compiler)` (`.CRT$XCC`), `lib` (`.CRT$XCL`) and every ordinary dynamic initializer (`.CRT$XCU`) |

    - *Why the old placement failed.* Round 3 placed the gate at `init_seg(compiler)`, which is `.CRT$XCC`.
      Every row above except the last ran before it, and so did Tracy's `.CRT$XCB`. All of that code is
      compiled at `avx2` under the whole-image rule, so a directly started client faulted inside mimalloc on
      a pre-AVX CPU before the gate or its backstop existed. The landed hook (`.CRT$XIB`) has the same flaw.
      It follows mimalloc's `.CRT$XLB` callback, and it ties with mimalloc's own `.CRT$XIB` entry, so link
      order decides which runs first. WP-0.5r moves it to `.CRT$XLA0` (09 §5.10.4).
    - *Which image.* Shipping (monolithic) images carry the hook in the executable. Modular dev builds
      (§1.4) carry it in `helios_runtime.dll`, and the executable carries none. The loader initializes a
      DLL's imports before the DLL, and it calls an executable's TLS callbacks only after every statically
      imported DLL has initialized. `helios_runtime.dll` imports only OS and Microsoft runtime DLLs. Every
      other Helios image imports it directly or through another group. Its first TLS callback is therefore
      the first Helios-compiled code in the process. Check 3 verifies both import facts.
    - *Rejected alternative: build `tp_mimalloc` and Tracy's client TU at `base`.* It fixes today's two cases
      but not the next library that adds a hook. It also turns mimalloc's LZCNT, TZCNT and POPCNT bit scans on
      the allocation fast path into slower baseline sequences. Placement fixes the class of problem, and the
      audit proves it.
  - *The Windows failure path runs under the loader lock, before the CRT exists.* The next callback is AVX2
    code, so on failure the hook never returns:
    1. It writes the message to stderr (`GetStdHandle`, `WriteFile`).
    2. If the executable's PE header (`GetModuleHandleW(nullptr)`) names the `WINDOWS_GUI` subsystem, the
       hook shows the message with `MessageBoxW`, taken from `user32.dll` through
       `LoadLibraryExW(…, LOAD_LIBRARY_SEARCH_SYSTEM32)` and `GetProcAddress`.
       - In the shipping client, the executable imports `user32.dll` (SDL3). The loader has already
         initialized it, so the call only adds a reference.
       - In modular dev builds the load may be nested under the loader lock. The UCRT's own fatal-error
         dialog does the same inside `_DllMainCRTStartup`. No application thread exists yet, so nothing can
         hold a lock the dialog needs.
       - `HELIOS_CPU_GATE_SILENT=1` (read with `GetEnvironmentVariableW`) skips the dialog for CI.
    3. It ends with `TerminateProcess(GetCurrentProcess(), 78)`, never `ExitProcess`. `ExitProcess` would send
       `DLL_PROCESS_DETACH` through the image's TLS callbacks, and mimalloc's `.CRT$XLY` hook would run AVX2
       code on the way out. The backstop's handler exits the same way.

    On success the hook records the verdict, installs the illegal-instruction backstop
    (`AddVectoredExceptionHandler`; rules below) and returns. For `DLL_THREAD_ATTACH` and the detach reasons it
    returns at once.
  - *Linux: `.preinit_array`.* The gate is the executable's single `.preinit_array` entry. glibc runs that
    array before any `.init_array` and before the initializers of shared objects, so the gate also precedes
    `libhelios_runtime.so` in dev builds. mimalloc's `constructor(101)` and Tracy's `init_priority(101…108)`
    objects therefore run after it in both flavours, and Tracy's `thread_local`s initialize lazily on first
    use. Only relocation-time code could run earlier, so IFUNC resolvers (`R_X86_64_IRELATIVE`) are banned.
    `init_array` priorities below 101, which are reserved for the toolchain, are banned too.
  - *Proof that it ran.* `core::platformInit()` checks `helios_cpu_gate_verdict()` in every build and stops
    with "CPU gate did not run" unless it reads *pass*. A dropped or misplaced hook therefore fails the first
    smoke test on an AVX2 CI machine instead of shipping unnoticed.
  - *Gate object rules:*
    - C only, with no libc or CRT calls, no exceptions and no RTTI. It uses the `__cpuid` and `_xgetbv`
      intrinsics.
    - The objects are compiled at `base` with `/GS-` and `-fno-stack-protector`, because the `/GS` cookie is
      not initialized before the entry point. They get no ASan, UBSan, coverage or `/RTC` instrumentation.
    - They define only `static` functions and data plus three external symbols: `helios_cpu_gate_run` (the
      probe), `helios_cpu_gate_verdict` and the Windows table slot `helios_cpu_gate_tls_entry`. The Linux
      `.preinit_array` entry is `static` and `used`.
    - Calls go only to an allowlist of OS entry points and to the CFG dispatch pointer, which the loader sets
      when it maps the image, before any TLS callback. The Windows entries are `GetStdHandle`, `WriteFile`,
      `GetModuleHandleW`, `GetEnvironmentVariableW`, `LoadLibraryExW`, `GetProcAddress`, `GetCurrentProcess`,
      `TerminateProcess` and `AddVectoredExceptionHandler`. The Linux entries are `write`, `_exit` and
      `sigaction`.
  - *What the gate checks:* 08 §2.2's feature list (which includes BMI2) and XCR0. On failure it shows or
    prints the CPU message and exits with code 78.
  - *The illegal-instruction backstop* catches the one fault a missed feature check would cause, #UD, on the
    way to the crash handler. On Windows it is the vectored handler above (`STATUS_ILLEGAL_INSTRUCTION`); on
    Linux the gate installs it with `sigaction(SIGILL)` and `SA_SIGINFO | SA_RESETHAND`.
    - *It decodes the faulting opcode* at `ExceptionAddress` or `si_addr`, and it handles only a genuine ISA
      fault: a VEX or EVEX lead byte (`C4`, `C5`, `62`, after optional `66`, `F2`, `F3` or REX prefixes),
      which covers every AVX, AVX2, FMA, F16C and BMI1/2 instruction, or POPCNT (`F3 0F B8`). Only those get
      the CPU message and exit 78. LZCNT and TZCNT decode as BSR and BSF on older CPUs and never fault, which
      is why the gate checks them explicitly.
    - *Deliberate traps pass straight through:* `ud2` (`0F 0B`), `ud1` (`0F B9`) and `ud0` (`0F FF`), which
      `__builtin_trap`, `-fsanitize-trap` and trap-on-unreachable emit, and every other #UD. Windows returns
      `EXCEPTION_CONTINUE_SEARCH`. Linux returns, and `SA_RESETHAND` has restored `SIG_DFL`, so the
      re-executed instruction takes the normal crash path. A real crash therefore never ends as
      "CPU unsupported" without a dump. The in-tree hooks already pass the three traps through
      (`hcg_is_deliberate_trap`); the VEX/EVEX test is WP-0.5r's.
    - *It stops at the crash handler.* `core`'s crash handler (Phase 0–1) and, from Phase 2, `engine/crash`
      take over when they install. On Linux their `sigaction(SIGILL)` replaces the backstop, and their
      re-raise path restores `SIG_DFL`, never the saved backstop (today's `posix_crash.cpp` restores the
      previous handler, which is the backstop). On Windows the crash handler adds its own vectored handler
      first in the chain (`AddVectoredExceptionHandler(1, …)`). It sends every `STATUS_ILLEGAL_INSTRUCTION`
      except the three traps to the dump path, which terminates the process, so the backstop behind it never
      runs again; the traps continue to SEH and the unhandled-exception filter. This keeps the gate objects at
      three exports. The crash report carries a `cpu_gate` annotation (the verdict, the CPU brand, the feature
      bits and the fault's lead bytes, classified by the same decoder from a shared `static inline` header),
      so an unsupported encoding that slips past the gate is still labelled in triage.
    - *Proof (audit check 5 and `core_tests`).* A gate test child that passes the gate executes, before the
      crash handler installs: `ud2`, which must die by SIGILL or an unhandled exception, never exit 78; and,
      under SDE `-hsw`, an EVEX (AVX-512) instruction, which must exit 78 with the message. After the crash
      handler installs, the same two must each produce a dump with no exit 78, the EVEX one annotated as an
      ISA fault.
  - *Nothing can sort ahead of it.* No object other than the gate and the CRT may contribute to `.CRT$XLA`
    through `.CRT$XLA0`. Dynamic `thread_local` initializers are no longer banned, because `__dyn_tls_init`
    is a later TLS callback. §1.4's rule against `thread_local` destructors in reloadable code still applies.
- **Self-dispatching third-party code** in `base` images keeps selecting its own ISA at run time. The known
  cases are zstd's `DYNAMIC_BMI2` Huffman decoder and BLAKE2b's SSE4.1 and AVX2 compression functions in the
  launcher's `patch` path, and SDL3's CPUID-guarded blitters and audio converters. `cmake/isa_allowlist.cmake`
  lists such functions by symbol. The static CRT's `memcpy`/`memset` and the MSVC STL's vectorized
  algorithms also dispatch themselves. Check 4 accepts wide instructions in symbols whose PDB contribution
  comes from Microsoft's prebuilt libraries (`libvcruntime`, `libucrt`, `libcpmt`). glibc is a shared library,
  outside the image.
- **The audit (WP-0.2r, RT-09) runs on every build.**
  1. *Flags.* `compile_commands.json` shows every TU carrying exactly its target's level set.
     - `avx2` TUs have no FMA or AVX-512 flag and no FP contraction.
     - `base` TUs and the gate TU have no flag above x86-64-v1: no `/arch:AVX*`, `-msse3` through
       `-msse4.2`, `-mpopcnt`, `-mlzcnt`, `-mcx16`, `-mavx*`, `-mbmi*`, `-mfma` or `-march` other than
       `x86-64`.
  2. *The gate object.*
     - `objdump -d` or `dumpbin /disasm` shows no VEX, EVEX or BMI encoding, and none above x86-64-v1:
       no SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, LZCNT/TZCNT, MOVBE or CMPXCHG16B.
     - It defines no external COMDAT, `selectany` or weak symbol. `nm` shows no `W`, `V` or `u` symbols, and
       `dumpbin /symbols` shows only `static` symbols and the three external symbols listed above.
     - Every undefined symbol is on the allowlist, so nothing the gate calls can resolve to an `avx2` copy.
       No `__security_cookie` or `__asan_*` reference appears.
  3. *The pre-gate path of each final `avx2` image* (client, cell, editor, bot, gateway, voice and tools, in
     both link flavours, each with and without `HELIOS_PROFILE`). `helios-tool isa-audit --pre-gate` reads the
     linked image and its PDB (`llvm-readobj --coff-tls-directory`, `llvm-pdbutil` section contributions) or
     its ELF tables.
     - *Windows (a): enumerate.* It lists every pre-C++ entry with its symbol and contributing object or
       library:
       - each pointer in `IMAGE_TLS_DIRECTORY.AddressOfCallBacks`, up to the terminating null;
       - each non-null pointer in `.CRT$XIA`–`.CRT$XIZ`, `.CRT$XCA`–`.CRT$XCZ` and `.CRT$XDA`–`.CRT$XDZ`;
       - `_pRawDllMain`, if the image defines it.
     - *Windows (b): the gate is first.* In the image that carries the gate, `AddressOfCallBacks[0]` is the
       gate hook. No other object contributes a non-null entry to `.CRT$XLA`–`.CRT$XLA0`. Other images
       carry no gate hook.
     - *Windows (c): every other entry is attributed.* An entry passes if it is CRT-owned or listed. CRT-owned
       means a contribution from `libvcruntime`, `libucrt` or `libcpmt`, or from the `/MD` startup objects of
       `msvcrt.lib`, `vcruntime.lib` and `ucrt.lib`. For MinGW it means the mingw-w64 CRT, libgcc, libstdc++
       and winpthreads. Listed means named, with its section, in `cmake/pre_main_allowlist.cmake`, which today
       holds:
       - mimalloc's `_mi_tls_callback_pre` (`.CRT$XLB`), `_mi_tls_callback_post` (`.CRT$XLY`) and
         `_mi_crt_callback_init` (`.CRT$XIB`);
       - Tracy's `.CRT$XCB` statics and its `.CRT$XD*` `thread_local`s, in `HELIOS_PROFILE` builds;
       - Helios modules' own `.CRT$XCU` initializers.

       All of these run after the gate, so their ISA level is free. The list makes a dependency update that
       adds a pre-`main` hook, or moves one ahead of the gate, fail review instead of passing silently.
     - *Windows (d): load order.* The gate image's static imports are only OS and Microsoft runtime DLLs on
       an allowlist: system DLLs such as `kernel32`, `advapi32`, `bcrypt`, `psapi`, `ws2_32` and `dbghelp`,
       the VC++ runtime DLLs in `/MD` builds and the ASan runtime in sanitizer builds. The check then walks
       `dumpbin /dependents` from each executable: every Helios-built DLL in its static import closure must
       import the gate image, directly or through another group, because a DLL that does not could be
       initialized first.
     - *Linux.* `.preinit_array` holds exactly the gate. `readelf -r` shows no `R_X86_64_IRELATIVE` in any
       Helios image. There are no `.init_array.0xxxx` sections below priority 101.
     - *Linux.* `.dynsym` defines nothing outside a short allowlist. An `avx2` copy therefore cannot interpose
       a shared library's vague-linkage symbol.
     - *Seeded fixtures (RT-09).* The check must fail on each of these canary objects:
       - a TLS callback that sorts ahead of the gate: a second `.CRT$XLA0` contribution linked before the
         gate object;
       - an AVX2 `.CRT$XIB` initializer that is not on the list;
       - an avx2-built DLL outside the gate image's import closure;
       - a `_pRawDllMain` defined by a canary library.

       It must pass on the real mimalloc and Tracy hooks.
  4. *`base` images, after linking* (launcher and bootstrap).
     - Every VEX, EVEX or BMI instruction, and every instruction above x86-64-v1 (SSE3 to SSE4.2, POPCNT,
       LZCNT/TZCNT, MOVBE, CMPXCHG16B), in the linked image is mapped to its symbol through the PDB
       (DIA, via `llvm-pdbutil`) or DWARF.
     - The check fails unless that symbol is on the self-dispatch list. This catches intrinsics, `target`
       attributes and COMDAT picks at link time, which a per-object check cannot see.
  5. *Emulation (CL-17).* Intel SDE runs with `-snb` (AVX without AVX2), with `-nhm` (no AVX), and with
     `-mrm` and `-pnr` (pre-v2: no SSE4.2 or POPCNT) under `-chip-check-exe-only`; on Linux, qemu-user runs
     the matching models plus `Opteron_G1` (SSE2 only).
     - The client, cell, editor, bot and `helios-tool` each show or print the CPU message and exit with
       code 78, never with SIGILL.
     - The launcher reaches its refusal screen.
     - The backstop fixtures pass (*illegal-instruction backstop* above): `ud2` never exits 78, and under SDE
       `-hsw` an EVEX instruction exits 78 before the crash handler installs and produces an annotated dump
       after it.
     - *Which builds.* The runs cover the shipping client with and without `HELIOS_PROFILE`. They also cover
       the modular `HELIOS_PROFILE=ON` dev flavour of the editor, PIE client, bot and `helios-tool`, whose
       `helios_runtime.dll` carries the gate together with mimalloc's and Tracy's pre-`main` hooks.
     - *Checking DLLs.* `-chip-check-exe-only` would not check that DLL. The modular `-nhm` and `-snb` runs
       therefore use the full-process chip check in report mode (`-chip-check-die 0`, with a report file).
       CI fails on any reported instruction whose address lies in a Helios-built image, and it ignores OS
       DLLs, which may dispatch on kernel-reported CPU state that SDE does not emulate.
     - *Canaries.* The canary fixtures of check 3 also run under `-nhm`. The ordering canary must be reported,
       and a canary that follows the gate must not be.

### 1.2 Plugins ("gems")

A gem is a directory, `gems/<name>/`, containing:
- a `gem.jsonc` manifest listing the gem's name, version, dependencies and modules (with layer and
  headless/editor-only flags, and `kind: editor-core | editor-ui` for editor extension modules, 07 §1.10);
- its schemas, content and Luau.

This follows O3DE's gem model (R06 §6.2).

- **Enabling.** `helios.project.jsonc` enables gems per target. The Foundation layer (01 §4.1) is a set of
  gems.
- **Linking.** Shipping builds link gems statically. A generated `static_modules.cpp` calls each enabled
  module's entry point explicitly, so no registration depends on static initializers that archive linking or
  `/OPT:REF` could drop.
- **Hot reload.** In dev builds, gem modules marked `"reloadable": true` build as DLLs/`.so`s that link against
  the engine's shared libraries. The editor, PIE cells and PIE clients reload them live. State lives in the ECS
  and records, so the world survives a reload (AAA-ITR-5). §1.4 gives the link model, the reload protocol and
  the rules for reloadable code.
- **Native mods.** A stable C ABI (`EngineServices` table, C types only) arrives in Phase 5 (R06-ENG-32). It is
  not used for in-house game modules, which share the C++ ABI of the dev build (§1.4).

### 1.3 Build targets

| Target | Binary | Links | Notes |
|---|---|---|---|
| client | `helios-client` | Runtime + graphics, audio, UI, `presentation`, `clientcore`, netcode | No Slang, `assetpipe` or ImGui in shipping |
| editor | `helios-editor` | Client + `toolsfw`, `editorui`, `edtools`, `assetpipe`, Luau.Analysis | Loads Slang at runtime; reloads the game DLL |
| cellserver | `helios-cell` | HEADLESS L1–L4, headless gem modules, nats.c | `HELIOS_BUILD_GRAPHICS=OFF`; `--replicant` runs it as a non-simulating replicant (04 §6.4, ADR-007; Phase 4); `--role world-script` runs it as a world-script host with no zone simulation (05 §1.23; Phase 3) |
| gateway | `helios-gateway` | `core`, `reflect`, `net`, `telemetry` | No ECS |
| voice | `helios-voice` | `core`, `net`, `telemetry` | SFU-style Opus forwarder (04 §2.7, ADR-015; Phase 3); no ECS, no decoding |
| launcher | `helios-launcher` | `core`, `app`, `ui` on SDL_Renderer, `patch`, `crash` | ISA level `base` (§1.1, 08 §2); the SDK's `Helios.exe` bootstrap + `HeliosLauncher.exe`, which T29 stamps and renames per product (08 §2.1, §2.10). Every other target is `avx2` |
| tools | `helios-{schemac, shaderc, assetd, cook, pack, fitsim, bot}` | §3.5, §6 | `schemac` builds first (`core` + yyjson); `bot` is a headless client (R06-ENG-27) |

### 1.4 Link model and game-module hot reload (ADR-016)

**The problem.** Engine modules are static libraries. A game DLL that uses the `ecs`, `reflect` or
`gameplay` APIs would otherwise either link its own copies, which splits process singletons, or need symbols
imported from the executable. The singletons at risk are flecs per-type ids, the `TypeInfo` registry, Jolt's
`Factory`, the memory-tag and CVar registries, and Tracy. Under `/MT` each image also gets its own CRT heap,
so an STL object allocated on one side of the boundary and freed on the other corrupts memory. ADR-011 rules
out a global `operator new` override as the fix. Finally, `/OPT:REF /OPT:ICF` in RelWithDebInfo disables
incremental linking.

**Two link flavours.**

| | **Shipping** (`HELIOS_MODULAR=OFF`) | **Dev** (`HELIOS_MODULAR=ON`) |
|---|---|---|
| Used by | Client, launcher, cells, gateway, voice and tools as shipped or deployed; CI release builds | Editor, PIE cells and clients, bots and the artists' prebuilt editor (AAA-ITR-6); the SDK's `helios-tool`, `helios-assetd` and `helios-cook`, which load project editor modules (07 §1.10) |
| Engine | Every module and gem linked statically into one image | Modules are `OBJECT` libraries combined into three shared libraries, one per link group (below) |
| Game code | Gems linked statically; explicit `static_modules.cpp` | Reloadable gem modules build as `game_<gem>[_client\|_edcore\|_edui].dll/.so` (the editor kinds: 07 §1.10); non-reloadable gems link into their group, except a project's editor modules on the binary SDK, which are always their own DLLs |
| MSVC CRT | `/MT` (ADR-011; no redist for players) | **`/MD` for every image**, third-party included, so there is one CRT heap. The prebuilt editor ships the VC++ runtime DLLs app-local |
| MSVC link | `/OPT:REF /OPT:ICF`, optional LTCG (never in SDK static libraries, ADR-001a rule 2) | `/INCREMENTAL /OPT:NOREF /OPT:NOICF /DEBUG:FULL`; PCH per game module |
| Linux | `-fvisibility=hidden`, static | `-fvisibility=hidden -fvisibility-inlines-hidden`, `-Wl,-z,defs`, lld or mold, `-gsplit-dwarf`; GCC game modules add `-fno-gnu-unique` |
| Presets | `windows-msvc-release`, `linux-gcc`, `linux-headless`, … | **`windows-msvc-dev`** (RelWithDebInfo, `/MD`, the dev link flags), the IDE presets `windows-vs2026` and `windows-vs2022` (ADR-001a rule 5) and **`linux-dev`** (Clang) |

The Linux dev flags are there for two reasons. `-Wl,-z,defs` makes a missing export a link error, as it
already is on Windows. `-fno-gnu-unique` stops GCC's `STB_GNU_UNIQUE` symbols from making the old `.so`
impossible to unload.

**Link groups** (dev only). Each group is one shared library, so each third-party library has exactly one copy.

| Group | Modules | Third-party inside |
|---|---|---|
| `helios_runtime` | Every HEADLESS L1–L4 runtime module: `core` … `authority`, `clientcore`, `telemetry`, `patch`, `crash` | mimalloc, Tracy client, xxHash, yyjson, flecs, Jolt, ozz, Recast, Luau, zstd, netcode, sentry-native |
| `helios_client` | `app`, `input`, `audio`, `voice`, `text`, `ui`, `rhi`, `render`, `presentation` | SDL3, miniaudio, libopus, RmlUi, FreeType, HarfBuzz, volk, VMA |
| `helios_editor` | `assetpipe`, `toolsfw`, `editorui`, `edtools/*` | ImGui, importers and encoders |

- **Exports.** Each group exports through `HELIOS_RUNTIME_API`, `HELIOS_CLIENT_API` or `HELIOS_EDITOR_API`.
  These expand to `__declspec(dllexport/dllimport)` or `visibility("default")` in dev builds and to nothing in
  shipping builds.
- **Singletons.** Every process singleton lives in exactly one image and is reached only through exported
  functions: the type registry, the component-id table, the memory-tag, CVar and log registries, the job
  system, the mimalloc heaps, Jolt's `Factory` and the Tracy client.
- **No per-image caches of global state.** Header-defined `inline` and template statics must not hold such
  state. Generated component code gets its id from `ecs::componentId(TypeId)` using the schema-lock
  `TypeId`, never from flecs' C++ per-type cache. flecs, Jolt and Luau headers never reach game code, which
  §1.1's "no third-party types in public headers" rule already guarantees.
- **Tracy.** The runtime group compiles the Tracy client with `TRACY_EXPORTS`, and game modules build with
  `TRACY_IMPORTS`. `HELIOS_PROFILE_ZONE` in a reloadable module expands to Tracy's transient zones, which copy
  their source-location strings, so the profiler never holds a pointer into an unloaded image.

**Module entry points.** A game module exports three C functions:
- `helios_module_info()` returns `{abi, engineBuildId, compiler, crt, iteratorDebugLevel, isa}`.
  `engineBuildId` hashes the engine's public headers and compile flags. On a mismatch the loader refuses the
  DLL and reports that the engine changed and a restart is needed. `#pragma detect_mismatch` catches CRT and
  iterator mismatches at link time. `isa` is set from `__AVX2__` when the module compiles, and the loader
  refuses any value but `avx2` (§1.1, *SDK consumers*).
- `helios_module_load(ModuleContext&, const ReloadState*)`.
- `helios_module_unload(ModuleContext&, ReloadStateWriter&)`.

Every registration a module makes goes through its `ModuleContext` and is recorded in a
`ModuleRegistrationScope`: types, components, systems, observers, hooks, CVars, console commands, Luau
bindings, RPC and event handlers, asset factories, and editor registrations (07 §1.10). Unload rolls the scope
back.

**Reload protocol** (editor, PIE cell and PIE client; budgets on DEV with the sample project):

| Step | What happens | Budget |
|---|---|---|
| 1 Build | A save, or Ctrl+Alt+F11, builds only `game_<gem>` with the dev preset. The compiler is fed through ccache and `/Z7`, and the link is incremental against the group import libraries | Compile of one `.cpp` ≤ 15 s; link ≤ 8 s |
| 2 Stage | The loader copies the DLL and its PDB (or `.so` and `.dwo`) to `.helios/hot/<name>-<n>.*`. On Windows it rewrites the CodeView PDB path in the copy (the cr.h technique), so the linker and debugger never contend for a locked file. On Linux `dlopen(RTLD_NOW \| RTLD_LOCAL)` of a unique path avoids the loader's path cache | ≤ 0.3 s |
| 3 Load new | The new image is loaded next to the old one. `helios_module_info` is checked, and `helios_module_load` registers into a staging scope | ≤ 0.3 s |
| 4 Quiesce | At the next frame or tick boundary: jobs drained, script lanes idle, the render thread holds only extracted data | ≤ 1 frame |
| 5 Swap | Components whose layout hash is unchanged stay in place, and their `TypeOps` and hooks are repointed to the new image. Changed layouts are written with `writeTagged`, re-registered and read back with `readTagged`, which tolerates added and removed fields (§3.4). Registry `TypeInfo` slots are stable, so pointers held by engine systems stay valid. `HELIOS_RELOAD_STATE` blobs move across | ≤ 1 s for 50k entities |
| 6 Unload old | The old scope is rolled back, then `FreeLibrary` or `dlclose`. The loader verifies the image is gone (`GetModuleHandleW` or `dl_iterate_phdr`) and warns if it is still mapped | ≤ 0.1 s |
| **Total** | **Save → reloaded ≤ 30 s p95 (RT-14, AAA-ITR-5)** | |

If step 3 or 5 fails (a load error, an init error, or a migration that cannot convert), the new image is
unloaded and the old one stays active. PIE processes reload through the same protocol, triggered by assetd's
`ModuleBuilt` message. If `engineBuildId` changed, the protocol falls back to 07 §1.6's "Rebuild & restart
PIE", a warm restart of ≤ 5 s.

**Rules for reloadable code.** A clang-tidy check and a symbol audit enforce these:
- Components are vtable-free and hold no function pointers. Their heap members use engine containers or the
  STL, which is safe because dev builds share one CRT heap.
- There is no mutable namespace-scope or static state except `HELIOS_RELOAD_STATE(T, name)`, which is kept
  in a runtime-owned tagged blob.
- There is no `thread_local` with a non-trivial destructor. glibc pins a `.so` that has one, and Windows runs
  the destructors only on the thread that unloads. Code uses `core::ThreadSlot` instead.
- Custom Jolt shapes or constraints, render passes and new RHI objects are not allowed. Those belong in
  non-reloadable gems.
- **Symbol audit (CI).** `dumpbin /symbols` or `nm` runs over each game image. It must show no definitions
  at all from flecs, Jolt, Luau, mimalloc or Tracy. From engine namespaces it must show no data symbols
  (statics, registries, template static members) and no strong definitions of exported engine functions.
  Inline functions and template instantiations from engine headers are allowed. A game image imports engine
  state; it never owns any.

**Phase 0 spike (09 WP-0.6c, gates RT-14).** A `Probe` component with a `list<u32>` field is declared in a
game-DLL schema, and 10k live entities carry it. The world survives 100 reloads that alternate code-only
edits, an added field and a removed field. Pass conditions:
- the values checksum is identical after every reload;
- memory-tag totals return to baseline;
- the old image is unmapped each time;
- a CVar, a system, an observer, a Luau binding and a Tracy zone registered from the DLL all work after every
  reload;
- ASan is clean with MSVC `/fsanitize=address` on `windows-msvc-dev` and with Clang on `linux-dev`;
- the symbol audit passes;
- a one-`.cpp` edit reaches a reloaded world in ≤ 30 s p95 on DEV (RT-18).

If the spike fails, the fallback is restart-based iteration: `.hsnap` save, rebuild, relaunch and restore.
It has the same ≤ 30 s budget, and ADR-016 is then reopened.

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
- **SDL3 3.4.16** is confined to `engine/app`, on the OS (process main) thread (§2.4).
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
  In dev builds the heaps live in the `helios_runtime` shared library and every image shares one `/MD` CRT
  heap, so no override is needed at the DLL boundary either (§1.4, ADR-016).
- **Heap lifetime (normative; spike (a), `engine/ecs/SPIKES.md` §1).** mimalloc 3.5 lets any thread use a
  first-class `mi_heap_t`, but each thread caches its last theap and validates the cache only by heap address.
  After `mi_heap_destroy` + `mi_heap_new` the new heap usually has the old address, and in the spike 200 of 200
  allocations then landed in the dead heap. So:
  - a heap that any thread other than its creator has used is **never freed**. It returns to a process-wide
    pool (`TaggedHeap`, one heap per tag group), and while it still has live blocks it is quarantined and not
    reused, so no heap address is ever recycled;
  - bulk free (`destroyAll`) is allowed only on heaps confined to one thread;
  - a `mi_theap_t*` is never cached across a job boundary or `JobSystem::wait()`, because a job may resume on
    another worker (fibers, Phase 2).

  The rule binds every `mi_heap_*` user, `core` included. A `core_tests` regression repeats the spike's 200
  destroy-and-new rounds with a live foreign theap and requires 0 misrouted allocations.
- **Tag accounting is sharded and batched (normative).** Exact accounting on a tag's shared atomics
  (`liveBytes`, `liveCount`, `totalCount` and a CAS loop on the peak) collapsed to 460–890 ns per alloc+free
  pair at 4 threads in the spike, and core `alignedAlloc` ran ≈ 30× slower than `mi_malloc` there. So:
  - each `MemoryTag` counts in 32 cache-line shards chosen by thread; a shard forwards its byte and count
    deltas to the tag once they pass 256 KiB (`trackAllocationBatch`), and the peak is touched only when a batch
    raises it;
  - `flushAccounting()` at each cell tick end and client frame end makes every tag exact. Soft and hard
    budgets and telemetry read the tag there, so a hard budget is enforced to within 256 KiB per thread;
  - exact per-allocation accounting remains an option only for low-rate heaps, such as flecs' ECS heap, which
    rarely calls `malloc`;
  - `mi_usable_size` supplies sizes, so blocks carry no header and frees take no size.

  Target, checked by a `core_tests` benchmark on SERVER: a tagged alloc+free pair costs ≤ 3× `mi_malloc`'s at
  4 and 16 threads. The spike's batched `TaggedHeap` measured 57–173 ns against 8–18 ns on a loaded 4-vCPU VM,
  and the two `mi_usable_size` calls, ≈ 200 of its 364 instructions per pair, are the next cut.
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

The cells' PCG budgets include the `pcg.collision` tile cache: ≤ 0.4 GB of 0.5 and ≤ 0.7 GB of 1.0 (§5.8a).

### 2.3 Jobs (R06-ENG-03, R05-P0-1, R01-P0-5)

- **Landed (`core/jobs.h`):**
  - `JobSystem::run(fn, Counter*, Priority)` with a helping `wait(counter)`;
  - three priority levels, each with work stealing;
  - `parallelFor`;
  - `TaskGraph` (a validated DAG);
  - `BackgroundPool` for IO, compiles and pathfinding.

  Overhead is O(100 ns) per job, guarded by a 1M-job test.
- **Worker counts.** The client runs `min(8, logical processors − 2)` workers, and its game and render threads
  help while waiting. The game and render threads are hinted onto separate physical cores.
  - REF (16 or 20 logical processors) gets 8 workers.
  - On MIN, the Ryzen 5 3600 (6 cores, 12 threads) gets 8 workers on SMT siblings, and the SMT-less i5-9400F
    gets **4**. §2.4's MIN budgets are written for the i5-9400F's 4 workers on 6 threads. That is the tighter
    case, and the Ryzen holds them with margin. The lab measures that case on a Ryzen 5 3600 with SMT
    disabled (RT-12).
  - The `BackgroundPool` runs 2 threads on MIN and 3 on REF, at below-normal priority. It shares the same
    cores, so its load counts in §2.4's per-frame CPU budget.
  - A cell runs cores − IO threads.
  - All of these can be overridden with the `jobs.workers` and `jobs.background` CVars.
- **Fibers** come in Phase 2+, behind the same API (ADR-011). Their rules apply now: no TLS or held mutex across
  `wait`, and `/GT`.
- **Other schedulers on this pool:** Jolt's `JobSystemWithBarrier` (§7.1), the ECS scheduler (§4.3) and Luau
  lanes (§7.4). flecs threads are unused.

### 2.4 Frame and tick pipelines

**Client at 60 Hz, on REF and MIN** (Destiny and Bevy pipelining, R05-P0-2). This is the three-thread model of 08 §1.3
(OS, game and render threads, plus workers and a net thread):

```
OS      | SDL_WaitEventTimeout(1 ms) pumps; window, cursor, IME ─► input ring + mouse-delta atomic
        |   (≤ 0.2 ms of work; may sit in a Win32 modal loop)
game    | A: snapshots, reconcile, remote interp/anim, Luau timers, other UI | JIT sleep |
        | B: drain ring → actions → speculative cues → fixed ticks ×k (predict, local grids)
        |    → local update, HUD | EXTRACT N |
workers | physics, anim, activation, collision tiles, UI layout, occlusion rays (jobs from every stage)
render  | low-latency: prepare + record + submit N right after EXTRACT N, in 3 batches; late-latches camera
        |   orientation into batch 0. Throughput mode: N−1 (03 §2.5)
net     | socket I/O, AEAD, keepalives, VOICE datagrams (08 §1.3)
GPU     | low-latency: N (queue = executing frame + next batch 0); throughput mode: N−2
```

- **Ticks.** Command frames run at the zone rate (20–60 Hz, 04 §5.1), with ≤ 4 catch-up ticks per frame.
- **Extract.** This is the only sim→render handoff. It runs on the game thread. The ECS is read-only during
  it, extractors write dirty deltas into `RenderScene` (§5.2), and the render thread never reads the ECS.
- **Budgets.** The two CPU budget tables after this list cover REF and MIN. 08 §1.3 uses the REF column, and
  03 §8.1 takes its render-thread rows from here.
  - **Input latency (08 §1.3a, CL-6).** The `FramePacer` sleeps between Phase A and Phase B so that input is
    sampled as late as possible, and `core` hosts `LatencyMarkers` (a per-frame M1–M7 timestamp ring).
  - The update is split around the input sample. Phase A (input-independent) runs before the JIT sleep, and
    Phase B (ticks, local update and HUD) runs after it.
- **Why the game thread is separate.** A window drag or resize makes Win32 run a modal loop inside
  `SDL_PumpEvents`. Only the OS thread stalls, so prediction, ticks, rendering and the net thread keep
  running. The OS thread fills the input ring from an `SDL_AddEventWatch` callback, so resize and focus events
  still arrive during the modal loop.
- **Editor variant.** The editor's OS thread is also its game thread, because ImGui's SDL platform backend and
  multi-viewport windows call SDL directly. An editor stall during a window drag is acceptable. PIE client 1
  in the viewport shares that stall; PIE clients in their own processes use the client model. Headless cells,
  bots and tools have no OS thread: their main thread runs `ZoneHost` or the tool.

**Client CPU budgets per thread** (ms wall per frame, p50 unless marked; 60 fps = 16.7 ms; RT-12). REF covers
BENCH-1, 2, 4 and 5 at 1440p High. MIN covers the scenes AAA-REN-2 holds to 60 fps, at 1080p Low with 4 job
workers (§2.3).

| Thread or stage | REF | MIN BENCH-1 | MIN BENCH-2 | MIN BENCH-4 |
|---|---|---|---|---|
| OS thread (work, not waiting) | ≤ 0.2 | ≤ 0.3 | ≤ 0.3 | ≤ 0.3 |
| Game, Phase A (snapshots, reconcile, remote interp/anim, timers, unfocused UI) | ≤ 3 | ≤ 4.5 | ≤ 3.0 | ≤ 4.0 |
| Game, Phase B (of which fixed ticks) | ≤ 4 (≤ 3) | ≤ 4.5 (≤ 3.0) | ≤ 5.0 (≤ 4.0) | ≤ 5.5 (≤ 4.5) |
| Game, Phase B: terrain fence inside the fixed ticks (§5.8a; below) | ≈ 0; ≤ 1.0 p99 on a frame that builds (BENCH-2, 5) | — | ≈ 0; ≤ 1.4 p99 on a frame that builds | — |
| Game, extract | ≤ 1 | ≤ 1.5 | ≤ 1.5 | ≤ 1.5 |
| **Game thread total** (p99) | **≤ 8** (≤ 12) | **≤ 10.5** (≤ 16) | **≤ 9.5** (≤ 16) | **≤ 11** (≤ 16) |
| Client Luau (inside A and B, wall budget §7.4) | ≤ 1 | ≤ 1.3 | ≤ 1.3 | ≤ 1.5 |
| Render thread (03 §2.5, §8.1) | ≤ 4 | ≤ 6 | ≤ 6 | ≤ 6 |
| JIT-sleep slack left at 60 fps | ≥ 8.7 | ≥ 6.2 | ≥ 7.2 | ≥ 5.7 |

**Client CPU budgets per system** (core-ms per frame, summed over every thread the system runs on, from
Tracy zones; the wall ms on 4 workers is in brackets where the system is on the game thread's critical path):

| System | REF (worst of BENCH-1/2/4) | MIN BENCH-1 | MIN BENCH-2 | MIN BENCH-4 |
|---|---|---|---|---|
| Animation (§7.2: sampling, blending, IK, palettes) | 14 (≤ 2 wall on 8 workers, 260 characters) | 10 (≤ 3.0), 260 characters under the MIN governor | 1 (≤ 0.5) | 6 (≤ 2.0), 86 characters |
| Physics (§7.1: prediction ticks, local grids, movers, queries) | 5 | 3 (≤ 1.0) | 5 (≤ 1.5) | 7 (≤ 2.0) |
| Cosmetic physics (ragdolls, secondary chains; cloth from Ph4) | ≤ 12 (§7.1's 0.5 ms + 1 ms wall caps on 8 workers) | 1.5 | — | 2.5 |
| UI (§7.5: HUD view-models and layout, diegetic surfaces) | 2.0 (HUD 1.0 + 40 surfaces 1.0) | 2.8 (HUD 1.3 + 40 surfaces 1.5) | 1.3 | 1.5 (combat HUD, 80 world markers) |
| Render jobs (prepare, recording, residency; 03 §8.1) | 4.0 | 5.5 | 5.5 | 5.5 |
| Streaming decode and activation (Background pool: zstd, meshopt, container reads; §5.7) | 4 (at the 150 MB/s ceiling) | 2 | 6 (150 MB/s at ≈ 1 GB/s of zstd output per Zen 2 core) | 2 |
| Terrain CPU (§5.8: collision-tile prefetch; visual tiles only under 03 §5.5a F3) | 1.5 (+ 2 under F3) | — | 1.5 (+ 2.7 under F3: 200 tiles/s × 0.8 ms) | — |
| Terrain fence builds (§5.8a; p50 ≈ 0, the cap on a frame that builds) | ≤ 2 (≤ 1.0 wall) | — | ≤ 2.8 (≤ 1.4 wall) | — |
| Audio mixer and voice (§7.3) | 0.8 | 0.8 | 0.8 | 1.0 |
| Net thread | 0.3 | 0.4 | 0.4 | 0.5 |
| Other jobs (occlusion and audio rays, nav, culling helpers, telemetry) | 2 | 2 | 2 | 2.5 |
| **Whole process** (includes the game and render threads' serial work) | **≤ 60 of 133** (8 cores) | **≤ 45 of 100** (6 threads) | **≤ 45 of 100** | **≤ 50 of 100** |

- **Where the MIN numbers come from.** Zen 2 and Coffee Lake cores do about 0.7× the game-code work per
  millisecond of REF's Zen 4 and Alder Lake cores, so the same work costs about 1.4×. MIN's CPU-side Low
  settings (below) remove 10–25 % of that work. The MIN budgets are therefore 1.3–1.4× REF, with the process
  total held to ≤ 50 % of the i5-9400F's six threads. The other half covers the GPU driver's threads (about
  2–5 core-ms at MIN draw counts), the OS and bursts. It also covers 03 §5.5's cold-set terrain bursts when
  the F3 fallback is active: at most 2 Background threads for at most 10 frames, masked by the warp and
  undock sequences.
- **MIN CPU settings.** These `cpu.*` CVars are picked by the same first-launch benchmark as the graphics
  preset (03 §8.2):
  - crowd governor A0 ≤ 16 and A1 ≤ 48 (REF 32 and 96), with animation tier ranges × 0.75;
  - ≤ 8 ragdolls, secondary chains in A0 only (≤ 4 per character), no cloth;
  - diegetic UI at ≤ 15 Hz within 30 m and ≤ 2 Hz beyond (REF 30 and 5);
  - 64 real audio voices and 32 occlusion rays (REF 128 and 64).

  Streaming lookahead, tick rates and prediction are never scaled down, because they are correctness settings.
- **Terrain fence on the client (§5.8a).** The fence runs inside Phase B's fixed ticks for the player's own
  body. It is ≈ 0 ms at p50, because the prefetch keeps synchronous builds rare (RT-06 requires 0 holds). On
  a frame that builds, it is capped at 2 SERVER-core ms of builds. That is ≤ 2 core-ms and ≤ 1.0 ms of wall
  on REF, and ≤ 2.8 core-ms and ≤ 1.4 ms of wall on MIN, where the game thread helps two parallel builds.
  - *Where it fits.* The time comes out of the game thread's p99 (≤ 12 ms on REF, ≤ 16 ms on MIN) and the
    whole-process headroom, not its p50.
  - *Why it is not CPU-bound.* MIN BENCH-2 keeps ≥ 7.2 − 1.4 = 5.8 ms of JIT-sleep slack on such a frame, and
    MIN BENCH-5 at 30 fps keeps ≥ 20.4 ms, so the frame is not CPU-bound.
  - *Past the cap.* The client suspends prediction instead of building more, so a burst of misses costs a
    few frames of server-driven movement, never a long frame. RT-12's landing case checks this.
- **CPU-bound frames.** Each nightly run classifies each frame from `LatencyMarkers` and GPU timestamps. A
  frame is CPU-bound when the game or render thread's time exceeds both the GPU frame time and the scene's
  frame period (16.7 ms, or 22.2, 33.3 or 8.33 ms for the columns below). Each RT-12 clause allows ≤ 1 % of
  them per run.

**Client CPU budgets at the other gated frame rates** (RT-12's 30/45 fps and 120 fps clauses). AAA-REN-1 holds
REF BENCH-3 to 45 fps, AAA-REN-2 holds MIN BENCH-3 and 5 to 30 fps, and AAA-REN-3 holds REF BENCH-4 to 120 fps
in Performance mode (03 §8.1.6). Per-frame work keeps its per-frame cost. Work that runs at a fixed rate
(ticks, snapshots, streaming, audio) is converted to cost per frame at that frame rate. Zone rates come from
04 §3.4: fleet battle 2 Hz × `d` with `CommandKinematic` ships and no 6-DoF prediction, ground hub 20 Hz, and
activity 60 Hz for BENCH-4.

| Thread or stage (ms wall, p50) | REF BENCH-3 (45 fps, 22.2 ms) | MIN BENCH-3 (30 fps, 33.3 ms) | MIN BENCH-5 (30 fps, 33.3 ms) | REF BENCH-4 Performance (120 fps, 8.33 ms) |
|---|---|---|---|---|
| OS thread | ≤ 0.2 | ≤ 0.3 | ≤ 0.3 | ≤ 0.2 |
| Game, Phase A | ≤ 4.0 (2 Hz snapshots applied in slices over the frames of a tick, volley cues, 2,000-row overview model) | ≤ 5.5 | ≤ 5.0 (150 avatars) | ≤ 1.5; ≤ 1.0 on tick frames, because the interleave moves the deferrable part to the next frame |
| Game, Phase B (of which fixed ticks) | ≤ 3.0 (≤ 0.5), including the 2,000-bracket HUD (08 CL-7: ≤ 1.0) | ≤ 4.0 (≤ 0.7) | ≤ 4.5 (≤ 3.0; a 20 Hz tick on 2 frames in 3) | ≤ 3.5 (≤ 2.5) on tick frames; ≤ 1.2 on the others (speculative cue step, local update, HUD) |
| Game, Phase B: terrain fence (§5.8a) | — | — | ≈ 0; ≤ 1.4 p99 on a frame that builds | — |
| Game, extract | ≤ 1.5 (2,000 grid transforms, brackets, emitter tables) | ≤ 2.0 | ≤ 2.0 | ≤ 0.8 |
| **Game thread total** (p99) | **≤ 8.5** (≤ 16) | **≤ 11.5** (≤ 25) | **≤ 11.5** (≤ 25) | **≤ 6.0 on tick frames, ≤ 4.0 on others** (≤ 8.0 over all frames) |
| Client Luau (inside A and B) | ≤ 1.0 | ≤ 1.3 | ≤ 1.3 | ≤ 0.6 |
| Render thread (03 §8.1.5) | ≤ 5 (03 RC-5) | ≤ 7 | ≤ 7 | ≤ 3.5 |
| JIT-sleep slack left | ≥ 13.7 | ≥ 21.8 | ≥ 21.8 | ≥ 2.3 on tick frames |

| System (core-ms per frame) | REF BENCH-3 (45 fps) | MIN BENCH-3 (30 fps) | MIN BENCH-5 (30 fps) | REF BENCH-4 Performance (120 fps) |
|---|---|---|---|---|
| Animation (§7.2; turrets and capital articulations in BENCH-3) | 2 | 2.5 | 8 (≤ 3.0 wall), 150 characters under the MIN governor; A1's 30 Hz is every frame at 30 fps | 3 (≤ 1.0 wall), 86 characters under the performance governor, 4 F0 faces |
| Physics (§7.1; BENCH-3 evaluates 2,000 `CommandKinematic` ships per frame for interpolation) | 5 (≤ 1.2 wall) | 6 (≤ 2.0) | 4 (≤ 1.5) | 5 on tick frames (≤ 1.5 wall), ≤ 0.5 on others |
| Cosmetic physics (wreck debris; ragdolls, secondary chains) | 3 | 1.5 | 1.5 | 3 (stepped at 60 Hz, render-interpolated; ≤ 8 ragdolls) |
| UI (§7.5) | 3.0 (HUD 1.0 + 2,000 brackets 1.0 + overview 1.0) | 4.0 | 2.8 (HUD 1.3 + surfaces 1.5) | 0.8 (bindings every frame, layout at 60 Hz) |
| Render jobs (03 §8.1.5) | 7.0 | 9.5 | 8.0 | 3.0 |
| Streaming decode and activation (Background pool) | 2.5 (≈ 70 MB/s: arriving hulls and capital LODs) | 4.8 (≈ 60 MB/s) | 4.8 (≈ 60 MB/s) | 1.0 (≈ 75 MB/s) |
| Terrain CPU (§5.8; collision-tile prefetch) | — | — | 1.5 (+ 2 under 03 §5.5a F3) | — |
| Terrain fence builds (§5.8a; p50 ≈ 0, the cap on a frame that builds) | — | — | ≤ 2.8 (≤ 1.4 wall) | — |
| Audio mixer and voice | 1.2 | 1.2 | 1.0 | 0.5 |
| Net thread | 0.6 (the 2,000-ship downstream at `d` = 1) | 0.8 | 0.6 | 0.2 |
| Other jobs | 2.5 | 3 | 3 | 1.2 |
| **Whole process** | **≤ 45 of 178** (8 cores × 22.2 ms) | **≤ 50 of 200** (6 threads × 33.3 ms) | **≤ 55 of 200** | **≤ 30 of 67** (8 cores × 8.33 ms) |

- **Tick/Phase A interleave (Performance mode).** A 60 Hz zone ticks on every other frame at 120 fps. On a
  frame with a tick due, Phase A runs only its input-independent essentials: snapshot apply, reconcile and
  remote interpolation. Unfocused UI documents, Luau timers not bound to this frame, A1–A2 animation job
  dispatch and audio occlusion rays move to the next, tickless frame. Both frame kinds then stay under the
  8.33 ms period. Phase B (≤ 3.5 ms) plus extract (≤ 0.8 ms) sits inside 08 §1.3a's 120 fps rows 3 and 4
  (4.0 + 1.0 ms p95), and the speculative cue step keeps firing latency independent of the tick parity.
- **Performance `cpu.*` settings** (applied with 03's Performance mode, REF only):
  - crowd governor A0 ≤ 16 and A1 ≤ 48, tier ranges × 0.75, ≤ 4 F0 faces;
  - cosmetic physics stepped at 60 Hz and render-interpolated, ≤ 8 ragdolls, secondary chains in A0 only;
  - HUD layout at 60 Hz (bindings every frame), diegetic UI at ≤ 15 Hz within 30 m and ≤ 2 Hz beyond;
  - streaming residency and CPU mip prediction on alternate frames (03 §1.3a);
  - 32 audio occlusion rays at 60 Hz.

  As at MIN, tick rates, prediction and streaming lookahead are never scaled.

**Cell tick.** 04 §3.3 owns the tick stages and their budgets. The engine provides:
- the per-zone stage graph (§4.3);
- `ZoneHost`, which runs zone ticks earliest-deadline-first and feeds per-zone CPU into TiDi;
- between-tick work: streaming, container activation (≤ 2,000 entity instantiations ≈ 2 ms per zone tick,
  metered in work units, not wall time), collision-tile prefetch (§5.8a: ≤ 1,000 core-ms/s on the ground-hub
  profile), and checkpoint encoding from copy-on-write snapshots. The `TerrainFence` runs inside stage 3
  (§5.8a: ≤ 8 core-ms of builds per tick, ≤ 1.2 ms p99 wall on 8 workers at that cap, ≤ 0.2 ms on a tick
  that builds nothing). Async completions reach the simulation only through the zone's `SimInbox` and are recorded as
  replay events (04 §10.2).

**Threading rules (normative).**
1. **SDL runs on the OS thread only.**
   - SDL event pumping stays on the OS thread (the process main thread, which SDL requires).
   - Input and window events reach the game thread through a lock-free single-producer/single-consumer ring of
     4,096 entries. Each entry carries SDL's nanosecond timestamp, so command frames sample input by time
     (§7.6). On overflow, mouse motion is coalesced first. Key, button and text events are never dropped: if the ring is
     still full, they wait in an overflow list on the OS thread until the game thread drains the ring.
   - Game-thread calls that SDL requires on the main thread go through `SDL_RunOnMainThread(fn, data,
     wait=false)`. These are window mode, cursor capture and relative mouse, `SDL_StartTextInputWithProperties`,
     `SDL_SetTextInputArea` and clipboard. `engine/app` wraps them in a thread-checked API that marshals
     automatically and asserts in dev builds.
   - The game thread never waits on the OS thread inside a frame. `wait=true` is allowed only at startup,
     shutdown and display-mode changes.
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
  - Flags as landed: `Cheat`, `Saved`, `Replicated`, `ReadOnly`. Phase 1 adds `Restart` (applies on next
    launch) and `Dev` (compiled out of shipping builds, like `Cheat`). 08 §1.4 uses these names: `Saved` is the
    persisted flag, and `Replicated` is server-locked.
  - Layering: code → `config/engine.jsonc` → project → user → command line → live config (05 §1.15). On the
    client, the "user" layer is 08 §1.4's machine file. That file is
    `%LOCALAPPDATA%\<Install>\<channel>\settings\machine.jsonc` on Windows and
    `$XDG_CONFIG_HOME/<productId>/<channel>/` on Linux, named by the stamped product (08 §2.10). Account-scoped values (keybinds, UI layout) come from the account blob, not from CVars.
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
- A package is **native** (compiled to C++ and Go) or **dynamic** (compiled to a runtime type bundle, with no
  C++ or Go). Project packages are dynamic by default (§3.8).
- Tags live in `.htags` files (06 §1.1).
- A package maps to a C++ namespace. `///` comments become `@doc`.

**Declaration kinds:** `enum`, `flags`, `struct`, `component`, `relation`, `record`, `event`, `rpc`,
`message`, `service`, `viewmodel`, `formula`, `const`, `alias`, `scriptlib`, `worldscript`. A `scriptlib` declares the
signatures of hand-written C++ functions that Luau can call. schemac generates their glue, so every
Luau→C++ call passes through generated code that charges fuel (§7.4). A `worldscript` declares a studio world
script's escrow wallets, tables, indexes, RPCs, event subscriptions, timers and reason codes. 05 §1.23 owns its
members and semantics; schemac validates it, emits the typed accessors of the `world` Luau realm and puts it in
the server part.

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
scriptlib Physics @realm(server, client) {                // C++ in engine/physics/script/, glue generated
  fn raycast(from: WorldPos, dir: vec3f, maxDist: f32 @unit(m), channel: QueryChannel) -> RayHit?
     @script(cost=24) @pure;
  fn overlapSphere(at: WorldPos, radius: f32 @unit(m), filter: TagQuery) -> list<EntityId> @max(256)
     @script(cost=30, each=2, of=result) @pure;
}
scriptlib World @realm(server) {
  fn spawn(prefab: PrefabRef, at: WorldPos) -> EntityId @script(cost=60, each=8, of=prefab);
}
```

```
file   := 'package' qname ';' {'import' string ';'} {decl}
decl   := kind Name [':' Base] {hattr} ('{' members '}' | params ['->' type] ';' | '=' hxl ';')
members:= { field | rpc | fn | ('client'|'server'|'editor') '{' members '}' }
fn     := 'fn' ident '(' [param {',' param}] ')' ['->' type] {'@' attr} ';'   // scriptlib only
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
| Script / versioning | `@script(read\|write\|none)` on fields; `@script(cost=n[, each=m, of=result\|<arg>])`, `@pure` and `@realm(server\|client\|editor\|world)` on `scriptlib` functions (`world` = world-script hosts, 05 §1.23); `@was`, `@version`, `@merge(append)` | §7.4 (fuel charges; lint: every `fn` needs `cost`, and `of=result` needs `@pure`); §3.4 |

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
| `luau` | Tagged-userdata glue (128 tags), `scriptlib` call glue with the fuel charge and binding IDs from the lock, `fuel_costs.defaults.json`, and `.d.luau` (with each function's cost in its doc comment) | Script host, luau-lsp, `--calibrate-fuel` |
| `proto` | `.proto` → `buf generate` → Go structs, Connect and `protoc-gen-helios-nats` bindings | 05 |
| `go` | Record structs and DB reader, JSONC readers, **validators** (ranges, units, `@validate` via Go HXL), reason-code constants | 05, 06, 07 |
| `sql` | goose migration stubs, diffed against the lock | 05 |
| `editor` | `TypeInfo` attributes + `schema.editor.json` (categories, widgets, units, docs, visibility badges) | 07 |
| `records` | JSONC → client and server `.hrdb`, HXL, tags, loc keys | Cook |
| `lint` | SEC-1/SEC-4, ledger/persist, keyed lists, naming, size budgets | CI |
| `docs` | JSON doc model per type, field, enum, component, message and service (`///` comments, units, ranges, defaults, audience, `@deprecated`); fails on a public symbol with no doc string | `helios-docs` (09 §2.7.1, AAA-TOOL-7) |
| `types` | The `.htypes` runtime type bundle of a dynamic package (§3.8), split into client and server parts | `reflect`, Go `pkg/htypes`, editor, cook |

Native packages use every row except `types`. Dynamic packages use only `types`, `luau` (the `.d.luau`
declarations), `editor`, `records`, `lint` and `docs`.

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

### 3.8 Project schema packages: the data-only path (01 §1.1; 08 §2.10.4; R03 §2.4)

**The problem.** If every package compiled to C++ and Go (§3.5), a studio that adds one record type, one
`ScriptState` component (04 §3.1) or one view-model would need all of this:
- a C++ toolchain on every designer and artist machine;
- a game-DLL reload in ≤ 30 s instead of ≤ 2 s;
- its own linked client and cell;
- a rebuilt `helios-backend` for the Go validators.

That would contradict 01 §1.1 ("native C++ game modules remain optional"), 08 §2.10.4's prebuilt stamped
client, the no-C++ starter templates (09 §2.7.4) and the artists' prebuilt editor (AAA-ITR-6).
HeroEngine's DOM Editor created classes and fields live, without a compile, as a governed, tool-edited
artifact (R03 §2.4). Helios keeps that governance (T08's schema editor, 07) and adds a compiled path that
produces data, not code.

**Two modes per package.**

| Mode | Packages | schemac emits (§3.5) | Runtime types |
|---|---|---|---|
| **native** | Engine and Foundation packages, plus any project package listed in `helios.project.jsonc` `schemas.native` because a `game/` C++ module or a hot-path system needs it | Everything except `types` | Generated `TypeInfo`, `TypeOps`, codecs, `Mut<T>`, Go structs |
| **dynamic** (the default for project packages) | Every other package in the project's `schemas/` and in non-Foundation gems | `types`, `luau` (`.d.luau` only), `editor`, `records`, `lint`, `docs` | `reflect` registers the `.htypes` bundle at load |

Moving a package between modes is one line in `helios.project.jsonc`. The text, tagged and cooked formats
key every field by its lock ID and do not depend on the in-memory layout, so saved data is identical in both
modes. Live components migrate through the tagged path, as for any layout change (§1.4 step 5).

**The `.htypes` bundle.** Each dynamic package compiles to one relocatable cooked blob (§3.7 layout). It is
split into client and server parts, like records (§3.3).
- **Contents:**
  - types and fields: lock IDs, name atoms, types, offsets and attributes;
  - enums and defaults;
  - `@validate` and `formula` HXL bytecode;
  - replication descriptors from `@replicate`, `@quant`, `@rate`, `@priority` and `@audience`;
  - a layout hash per type.
- **It is content.** It ships in paks and travels in the client part or the server part (05 §1.14.1). Its
  client-visible part enters the compat fingerprint like any replication descriptor.
- **IDs.** Type and field IDs come from the project's `schema.lock.jsonc`, as for native types (§3.4).
- **Budgets.** schemac builds 2,000 types in ≤ 1 s (the §3.5 budget), and the bundle loads in ≤ 50 ms.

**What a dynamic package may declare.**
- **Allowed:** `enum`, `flags`, `struct`, `alias`, `const`, `record`, `component` (including `ScriptState`
  components), `event`, `viewmodel`, `formula` and `worldscript`. `worldscript` is already data-driven
  (05 §1.23).
- **Client→server `rpc`s are allowed** when a Luau handler on the cell serves them. The SEC-1 lint still
  requires `@ratelimit` and `@intent`.
- **Rejected by schemac**, because native code consumes them:
  - `scriptlib`, `service`, `message` and `relation`;
  - `@predicted` fields;
  - `@store(ledger)` and `@ledger_policy`;
  - `@sql`, and custom `@table` SQL (05 owns those tables).

  A studio that needs one of these marks the package native.

**Runtime.**
- **Layout.** `reflect` computes the layout at load: declaration order with natural alignment.
  - A replicated component starts with the hidden `_dirty` mask.
  - Generated C++ uses `std::string`, `std::vector`, `std::map` and `std::optional`, which cannot hold an
    element type known only at run time. Dynamic types therefore use type-erased engine containers:
    `DynString`, and `DynList` and `DynMap` (16 B each: pointer, u32 count, u32 capacity; a map is a sorted
    list of pairs), plus `DynOptional` (a flag and an inline value). Each takes its element `TypeInfo` from
    the field.
  - `TypeInfo.kind` carries a `dynamic` flag.
  - RT-21 checks that every type's JSONC, tagged and cooked bytes are identical in both modes.
- **Generic `TypeOps`.** One table-driven implementation walks the `FieldInfo` list for every operation:
  default construction, destroy, copy, move, equality, hash, the JSONC, tagged and cooked codecs,
  `diff`/`apply`, and property paths. Targets on REF, relative to generated code: copy and compare ≤ 2×,
  tagged codec ≤ 2×, JSONC ≤ 1.5×.
- **ECS.** `ecs::registerDynamicComponent(const TypeInfo&)` creates a flecs component from a runtime size,
  alignment and hooks, which flecs supports. Its id comes from `ecs::componentId(TypeId)`, as for game-DLL
  types (§1.4). Storage is an ordinary flecs column. Prefabs, `@shared`, `@sparse`, checkpoints, undo and the
  inspector all work through `TypeInfo` already.
- **Writes and replication.** C++ cannot name a dynamic type.
  - Every write goes through `reflect::DynMut`, whose per-field setters set the `_dirty` bits exactly as
    `Mut<C>` does (§4.4).
  - `repl::describeDynamic` builds each `ComponentRepDesc` at load from the bundle, using 04's table-driven
    quantizers. The bytes on the wire equal those of the same type compiled native.
  - Stage 6 (04 §3.3) reports the dynamic share as `repl.dynamic_us`.
- **Luau.** Dynamic components, records, structs and view-models are userdata, with one tag per kind.
  - `__index` and `__newindex` resolve a field through Luau's `useratom` string atoms to its `FieldInfo`.
    An access costs ≈ 40 ns, against ≈ 20 ns for generated glue.
  - The `.d.luau` declarations are generated, so luau-lsp type-checks scripts statically.
  - Field access is charged fuel like any safepoint (§7.4).
- **Records and HXL.** Record DBs include dynamic record types, and scripts and tools read them through
  `DynRecordView`. HXL compiles a dynamic field read to a `(TypeId, fieldId, offset)` triple from the bundle.
- **View-models.** `ui::DynViewModel` binds a dynamic view-model to an RmlUi data model through a custom
  `Rml::VariableDefinition` that reads the layout. It marks only dirty fields, with `DirtyVariable`.
  `@source(Component.field)` and `@source(Service.Stream)` bind through a table-driven adapter that reads
  the source through `TypeInfo`. Native view-models get generated adapters instead (§7.5), and both push only
  dirty fields. Luau controllers drive everything else (08 §1.7.1).
- **Go.** `services/pkg/htypes` interprets the same bundle. It provides:
  - a generic record reader for record DBs;
  - a JSONC reader;
  - **validators** for ranges, units and `@validate` (through Go HXL);
  - a tagged-binary decoder.

  The SDK's prebuilt `helios-backend`, the collab service (05 §1.14) and T28's Go rules therefore accept
  project types. Checkpoint and character blobs stay opaque to Go, and GM and web tools render them through
  the interpreter.

**Hot-path lint (`schema.dynamic-hot`).**
- Engine code and native game modules reach dynamic types only through the generic paths above. A native
  `SystemDesc` cannot declare them, because `reads<T>()` needs a C++ type.
- Every BENCH and bot profile reports the generic-path cost per type. CI fails a content build in two cases,
  and T28 then suggests `schemas.native` for the package:
  - a zone's dynamic replicated components take more than 1 ms of stage 6's 3 ms;
  - its Luau systems touch more than 5,000 dynamic-component instances per tick.

**Hot reload of a data-only schema change** (p95 on DEV, no DLL, no compiler):

| Step | Budget |
|---|---|
| A save (a T08 schema transaction or a text edit) is detected by assetd | ≤ 100 ms |
| schemac rebuilds `.htypes`, `.d.luau`, editor metadata and lint output, incrementally | ≤ 500 ms |
| The records, Luau and RML that reference changed types are recooked | ≤ 1.5 s |
| `TypesChanged{package, bundleHash, changed[]}` is sent | ≤ 10 ms |
| Swap at a tick or frame boundary. The registry keeps each type's `TypeInfo` slot. Types whose layout hash is unchanged only swap attributes. Changed layouts migrate through `writeTagged` → re-register → `readTagged`, the step 5 path of §1.4, which tolerates added and removed fields (§3.4) | ≤ 1 s for 50k entities |
| Luau `__reload`, view-model rebinding and the inspector refresh | ≤ 0.5 s |
| **Total** | **≤ 5 s (RT-21, 07 ED-22)** |

- **Destructive edits.** A change that removes or retypes a field that live data holds shows T08's migration
  preview before it saves.
- **Live builds.** Changing a client-visible type in a live build is a compat-epoch change (05 §1.14.1), like
  any other.

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
| `EntityId` | u64 | Global, persistent | `0…` runtime spawns: time-prefixed block IDs 41/5/17 (ms / shard / offset), minted locally from PG-allocated blocks of 2^17 by the `EntityRegistry` minter (05 §1.4.5). `10…` content-placed: `hash62(entityGuid)`; the GUID is minted once in the `.hent`, so moving an entity between containers keeps its ID; the cook rejects collisions. `11…` client-local |
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
| Cell | `{id, local AABB + convex hull, flags (exterior-visible, pressurized, roofed), probeZone, exposureZone}` |
| Portal | `{id, cellA, cellB \| Exterior, convex polygon ≤ 8 verts, flags (window, airlock)}` |
| State | `PortalState{portalId, open, opacity}` components, replicated and driven by door entities |
| API | `cellAt(localPos)`, `visibleCells(viewCell, frustum, maxDepth = 8)`, `propagate(cell, fn)` |
| Source | `PortalCell` and `Portal` authoring entities in the container, generated and edited in 07 T01's Volumes mode (07 §2.6.3); the cook converts them and drops them from runtime worlds |

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
| **Host** | Ship or station interior, cargo bay; a ship's `Exterior` EVA shell (06 §8.2a) | Hull pose, set as `Kinematic` after the parent grid steps | Uniform artificial gravity (0 g for an `Exterior` shell); host acceleration is ignored or scaled by `GridDef.inertialFactor` |
| **Bubble** | Open space; planet surface (body frame) | Centre of the bubble's box, snapped to the 4,096 m lattice and fixed in the parent frame; re-centred when the box centre drifts past R/2 | None in space; radial on a surface, applied per body before each step |

**Bubbles are disjoint clusters.** This follows the Space Engineers cluster-tree pattern, learned from public
descriptions. A bubble is an axis-aligned **box** in its parent frame, with f64 bounds, that owns one pooled
`PhysicsSystem`. R = 20 km is the design half-extent. With it, Jolt's float broadphase bounds keep a resolution
of ≤ 2 mm (R06 §6.7). Those bounds are rounded outward, and narrowphase and integration are f64-relative
(`JPH_DOUBLE_PRECISION`), so a larger box costs extra broadphase pairs, never missed contacts.

| Rule | Definition |
|---|---|
| **Interaction box** | For each dynamic, kinematic or character body: the AABB of its bounding sphere, which does not change with rotation, grown by `m = 50 m + 2·\|v\|·Δt`. At 1 km/s and 20 Hz, m = 150 m |
| **I1: disjoint** | The bubble boxes in one parent frame never overlap |
| **I2: contained** | Every body's interaction box lies inside its own bubble's box |
| **Consequence** | Two bodies in different bubbles start a step at least m_a + m_b apart. They cannot close that gap in one step, so **every contact is inside one `PhysicsSystem`**, and seams never change contact results |
| **Statics** | Terrain tiles, placed statics and asteroids are instanced in every bubble their bounds overlap, with a shared `Shape` and no owner. Statics therefore need no seam rule |
| **Membership** | A body belongs to the bubble whose box contains its interaction box. The margin gives hysteresis: a body changes bubble only through a merge or a split, never by drifting across a seam. These moves are not grid transfers, so 04 §5.5 does not flag them |
| **Growth** | At each sync point, if an interaction box would leave its bubble's box, the box grows to contain it |
| **Merge** | If growth would make two boxes overlap, the bubbles merge before the next step. The bubble with fewer bodies moves into the other, with positions re-expressed relative to the surviving origin (no frame change) |
| **New bubble** | A body spawned or placed outside every box gets a new box: its interaction box grown to a 2 km minimum side, or a merge if that box would overlap another |
| **Split** | At 1 Hz, a bubble whose box side exceeds 2R = 40 km, or whose members fall into separate groups, splits. The cut is the axis-aligned plane with the widest gap (≥ 0) between member interaction boxes, found with members sorted by EntityId and ties broken on x, then y, then z. The smaller side moves to a new bubble |
| **Shrink and re-centre** | At 1 Hz each box shrinks to its members' union plus 1 km, and the origin re-centres when it is more than R/2 from the box centre |
| **Origin lattice** | Bubble origins snap to the parent frame's 4,096 m `frame_cell` lattice (04 §4.5), so every origin change is an exact multiple of 4,096 m. Each origin change (re-centre, merge, split) is a replicated `BubbleOrigin{bubble, origin, epoch, tick}` event, applied at the same tick on the cell and on the predicting client |
| **Oversize** | A dense formation that cannot split may grow to 4R (80 km side; the float ULP at 40 km is 3.9 mm). Beyond 4R, `physics.bubble_oversize` alarms. BENCH-3 and every other BENCH scene must show zero |
| **Caps and cost** | ≤ 10,000 bodies per bubble. A merge or split moving 2,000 bodies takes ≤ 2 ms at the sync point (RT-19). Bubble `PhysicsSystem`s are pooled |

- **Fleet-battle profile.** The 2 Hz fleet-battle profile runs ships as `CommandKinematic` (06 §8.1), so
  kinematic ship–ship pairs never collide. The seam rules matter for the 20 Hz open-space profile, for dynamic
  debris and for surface bubbles.
- **Client side.**
  - The client keeps one local bubble. Its origin is the `BubbleOrigin` of the cell bubble that owns the
    player's hull, so the client predicts in the cell's exact coordinates, and 04 §5.3's bit-identical hull
    integration holds across re-centres and merges.
  - The client predicts only the owned hull against static colliders, and remote ships are interpolated
    kinematic proxies. Statics are instanced identically in both bubbles, so a hull-versus-static contact
    predicted by the client is bit-identical to the cell's, even when the static straddles a seam. That also
    needs Jolt's solver order to ignore `BodyID`s, which differ between the two systems, and the hull to carry
    no contact cache across ticks: §7.1's ordering-independence rule.
  - Ship–ship contacts are cell-authoritative. I1 and I2 guarantee that no seam contact is ever missed or
    doubled (RT-19).
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

- **Pinning.** Each ZoneInstance pins one content compat epoch for life (04 §7). Its server part (server-only
  records, server Luau, tuning) can be hot-swapped at a tick boundary by a live hotfix (05 §1.14.1).
- **Region filter.** `ownedRegions` filters streaming, spawners and authority.
- **Partitions.** `ownedRegions` is built from the zone's `ZonePartitionDef` record (07 §2.6.1, §2.6.5). v0
  zones use `kind = Whole` (one region). v1 static multi-cell zones use a plane BSP or a grid of ≤ 64 regions
  in the zone's frame. The orchestrator creates one region lease per region (05 §1.4.2), and `regionAt(FramePos)`
  is the one geometric test shared by cells, the handoff trigger and multi-cell PIE.
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
- Activation takes ≤ 1 ms of client game-thread time per frame and ≤ 2 ms of cell time per tick. On cells the
  2 ms is metered as ≤ 2,000 entity instantiations, and each activation tick is a replay event (04 §10.2).
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
| Twin | Each node op exists as a C++ VM op and a Slang function (`hnoise.slang`, co-owned with 03); a per-node corpus hashes both. The Slang twin uses 32-bit integer ops only (explicit hi/lo products, no `shaderInt64`; 03 §5.5a) |
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
  core for a 40-node graph on the **AVX2 kernels**, validated by the WP-0.9c spike before Ph1 (03 §5.5a, risk
  K5b). Generators take the tile level: coarser than the collision levels, octaves shorter than 2× the sample
  spacing are skipped under 03 §5.5a's amplitude rule, identically on CPU and GPU, and the corpus runs per
  level. Collision levels evaluate every octave.
- **SIMD kernels.** Each op's body is written once, in `kernels/vm_ops.inl`, against a lane type `Lanes<W>`
  (i32 and i64 vectors, with add-with-carry, 32×32→64 products and shifts). It is compiled three times, all
  at the image's `avx2` level (§1.1), so no kernel needs per-file flags:

  | TU | Width | Role |
  |---|---|---|
  | `vm_avx2.cpp` | 8 × i32 (Q32.32 products via `_mm256_mul_epi32`/`_mm256_mul_epu32` on even and odd lanes) | **The budgeted path.** It is the default kernel |
  | `vm_sse42.cpp` | 4 × i32 in 128-bit intrinsics, which encode as VEX-128 at the `avx2` level without changing any result | A width-4 conformance twin: it proves the result does not depend on the lane width. Used by runs forced with `pcg.kernel=sse42`. Never budgeted |
  | `vm_scalar.cpp` | 1 | The reference implementation that the Slang twin and the corpus are checked against, and the template for a later NEON port. ARM64 is out of scope |

  - **Only integer ops go in the kernel TUs.** A lint rejects `float` and `double` there. `visualOnly` float
    nodes run in shared code whatever kernel is selected. Integer results are exact, so all three widths
    hash identically, and RT-04 runs its corpus through each of them.
  - **Selection.** `pcg`'s module entry point (called explicitly from `static_modules.cpp`, or at load in
    modular builds; §1.2) reads the `pcg.kernel` CVar (default `avx2`) and stores one `const VmKernels*`.
    No CPUID dispatch is needed, because every image that links `pcg` has passed the gate. The tile
    evaluator calls through the table once per op per tile, not per sample, so a 40-node tile costs about 40
    indirect calls.
  - **Measurement guard.** `pcg` logs `pcg.kernel` at start. WP-0.9c, RC-13, RT-20 and the RT-04 bench
    record it, and **a MIN, REF or SERVER run that did not select `avx2` fails**, so a forced 4-lane run
    cannot trip 03 §5.5a's F3 threshold. The SSE4.2 path's ms per tile (expected ≈ 1.8× AVX2) is reported alongside
    for information only.
- **Tiles.** Tiles are `TileKey{face, level, x, y}`, and the same tiles serve rendering and collision
  (03 §5.4). Radius goes up to 6,400 km (AAA-CNT-1).
- **Collision.** The CPU builds collision tiles at each body's one collision level, only for the tile sets of
  §5.8a, at ≤ 1 ms per tile. If a GPU vendor fails the conformance test, the client uploads these CPU tiles
  instead.
- **Contract.** Visual vs collision ≤ 5 cm over the whole surface.
  - At shared sample points both are the same fixed-point value, and 03's CI holds the GPU twin to ≤ 1 cm.
  - Between collision vertices the collision mesh interpolates linearly. T04's budget analyzer therefore
    requires Σ over octaves of A·min(2, π²·s_c²/(2λ²)) ≤ 5 cm, where A is the octave's amplitude, λ its
    wavelength and s_c the collision spacing (§5.8a). That sum is the octave's linear-interpolation error
    bound. An octave that would break the bound must be `visualOnly`, as in 03 §5.5a's F3 split.

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

### 5.8a Terrain collision tiles: selection, consistency and budget (W04, R04-P1-12)

**Collision level.** For each body, the collision level `L_c` is the one level whose sample spacing `s_c`
lies in (0.75 m, 1.5 m]. On Harrow (1,500 km) that is level 15: s_c ≈ 1.12 m and tiles ≈ 72 m on a side. On
a 6,400 km body it is level 17: s_c ≈ 1.2 m and tiles ≈ 77 m.
- A collision tile is the 65² fixed-point heights of §5.8. `det::` converts them into f32 vertices
  relative to the tile's f64 anchor, and they are built into a Jolt `MeshShape` of 8,192 triangles
  (≈ 130 KB).
- The shape is instanced into every bubble it overlaps (§5.4 Statics).
- Contacts, character movers, vehicle suspension rays and terrain queries use **only** `L_c` tiles.

**Tile sets.** Each set is a pure function of body state (pose, velocity, bounds, the step Δt and the
body's `a_max`) and of deterministic tile data. The cell and the predicting client therefore compute the
same sets for the same state.

| Set | Definition | Used by |
|---|---|---|
| Step box `S(b)` | The AABB of the body's bounding sphere, grown by \|v\|·Δt + 1 m | The fence |
| Contact set `C(b)` | The `L_c` tiles that overlap `S(b)` and whose height slab intersects it | The fence: all of them must be present before the step |
| Lookahead set `A(b)` | The `L_c` tiles that pass the slab test and overlap this box: the segment `[p, p + v·T]` with T = 2 s, its length capped at 1,024 m, grown on every side by ½·a_max·T² + r + 2 m. Shapes are built for the first 0.5 s of travel; farther tiles hold heights only (17 KB) until the body approaches | Background prefetch |
| Height slab | A tile's slab is the `[min, max]` of its level-(`L_c` − 3) ancestor's samples over the tile's footprint, widened by the graph's static `slabMargin`. The margin covers three things: each octave's interpolation bound at the ancestor's spacing, the full amplitude of octaves the ancestor skips (03 §5.5a), and the bounds of stamps and authored deltas. The graph compiler computes it, and T04 checks it against the corpus | Keeps bodies above the terrain, such as a ship at altitude, from requesting tiles |

- `a_max` comes from the body's movement record: `MovementDef`, or the vehicle or ship definition.
- These have no terrain sets: bodies in host grids (interiors, EVA shells), and remote proxies on the
  client.
- Sleeping and static bodies have \|v\| = 0, so they pin only their `C(b)` (≈ 1–4 tiles each).

**No substitution (normative).**
- **No other terrain source.** Physics never uses a render tile, a GPU tile or a coarser ancestor for
  contacts, movers, suspension or queries. The renderer's parent fallback (03 §5.4) is visual only.
- **Fence.** At the start of stage 3 (04 §3.3), `TerrainFence` computes every `C(b)` in EntityId order.
  - Any tile that is not resident is built synchronously on the job system, in `TileKey` order, and counted
    in `physics.collision_tile_sync`. That counts a prefetch miss, which costs time but never changes a
    result.
  - A tile's content is a pure function of the body seed, the graph, the stamps and the `TileKey`. A
    synchronous build therefore changes no result and needs no replay event.
- **Fence cap, in core-ms.** Tiles cost different amounts, so the cap is a cost budget,
  `pcg.collision.fenceCoreMs`, counted in SERVER-core milliseconds:

  | Build kind | When | Cost |
  |---|---|---|
  | **Full** | Neither heights nor shape is cached: a turn off the lookahead, a spawn, a stamp rebuild | ≤ 1.0 (VM ≤ 0.5 + `MeshShape` ≤ 0.5) |
  | **Shape-only** | Heights are cached, because the tile is in `A(b)` beyond its first 0.5 s | ≤ 0.5 (`MeshShape` only) |

  | Host | `fenceCoreMs` | Most builds (per tick on cells, per frame on clients) | Wall at the cap, p99 |
  |---|---|---|---|
  | Cell (ground-hub profile, 8 workers) | 8 per tick | 8 full, 16 shape-only, or a mix | ≤ 1.2 ms: scan ≤ 0.2 + builds ≤ 1.0 |
  | Client, REF | 2 per frame, shared by that frame's catch-up ticks | 2 full, 4 shape-only, or a mix | ≤ 1.0 ms |
  | Client, MIN (≈ 1.4× per unit, 4 workers) | 2 per frame | The same | ≤ 1.4 ms |

  - *Why the cell's wall bound holds.* The scan computes each `C(b)` in EntityId chunks across the workers.
    Asleep and static bodies reuse the set from their last tick, so the scan is ≤ 0.2 ms for 2,500 bodies.
    Builds are then dispatched one tile per job, full builds first and then shape-only builds, each kind in
    `TileKey` order. Every build costs 0.5 or 1.0 ms, and the total is ≤ 8 core-ms on 8 workers. Each full
    build therefore gets a worker to itself, the shape-only builds pair up on the rest, and the builds finish
    within 1.0 ms. The round-3 cap of 16 tiles could be 16 full builds, which is 16 core-ms or ≈ 2 ms of wall,
    against a stated ≤ 1 ms.
  - *Usual ticks.* A tick that builds nothing costs only the scan (≤ 0.2 ms). RT-20 allows
    `physics.collision_tile_sync` ≤ 1 per 1,000 body-ticks, so most ticks build nothing.
  - *Budget.* Fence builds count against `pcg.collision.coreMsPerSec` and RT-20's generation limit. A cell at
    the cap on every tick would spend 160 core-ms/s on them.
- **Hold (cell).** Bodies are admitted in EntityId order while their missing tiles still fit the cap. A tile
  that several bodies share is charged once. Every later body with a missing tile is **held**.
  - A held body keeps its pose and velocity, sits out that one step as a Jolt `Kinematic` body at rest, and
    gets its velocity back afterwards.
  - Holds are counted in `physics.collision_tile_miss`.
  - A hold depends on generation timing, so it is recorded as a `CollisionHold{tick, entity}` replay event
    (04 §10.2), like a wall-backstop kill.
- **Warm starts.** Cold sets are the main source of full builds. When a spawn, respawn or warp exit picks its
  destination, it queues that destination's `C(b)` and the first 0.5 s of its `A(b)` at the head of the
  prefetch queue. The respawn timer or the ≥ 1 s warp spool-up covers the build, so a cold set reaches the
  fence only if that lead time is shorter than the build.
- **Client.** The predicting client runs the same fence for its own hull or character, with the same sets
  and the client cap above.
  - *Where it runs.* The fence runs inside Phase B's fixed ticks. Its builds are high-priority jobs, and the
    game thread helps execute them while it waits. §2.4 carries the fence as a Phase B line.
  - *Past the cap.* If `C(b)` cannot be completed within the frame's cap, the client **suspends prediction**
    for that body instead of holding it. It follows server snapshots until the tile is resident, which is
    logged as `clientcore.prediction_suspended{reason=terrain}`, and never predicts against a surface the
    cell may not have. The unfinished builds continue as high-priority jobs outside the tick.
  - *Cosmetic physics.* Cosmetic physics (§7.1) uses only resident `L_c` tiles, and it freezes a ragdoll
    whose tiles are missing.
- **Result.** Both sides always contact exactly the same `L_c` triangles, so 04 §5.3's bit-identical
  hull-versus-static contacts hold on planet surfaces as well. No body can fall through a missing tile.
- **Queries.** Terrain ray and shape casts test the level-(`L_c` − 3) slabs first. They enter `L_c` tiles
  only where the cast enters a slab, which is usually near the shooter and the target, whose tiles are
  already resident.
  - A cast visits at most 16 `L_c` tiles. Past that point it reports no terrain hit. The limit depends only
    on the cast and the slabs, so every host truncates at the same point.
  - Visited tiles that are not resident are built synchronously inside the query.
  - The Luau glue charges fuel per *visited* tile (`each`, calibrated at the resident ratio seen in NS-2.4
    traces), so fuel stays a function of the arguments (§7.4).
  - T28 flags a surface weapon or sensor whose range exceeds 16 tiles (≈ 1.1 km of ground track on
    Harrow).

**Budget and cache (cell).**
- **Prefetch.** Prefetch runs between ticks on the Background pool, nearest time-to-reach first.
  - It has a per-cell budget, `pcg.collision.coreMsPerSec`. The budget is 1,000 on the ground-hub profile,
    one of its 8 cores (04 §3.4).
  - Past the budget, the lookahead horizon T shrinks from 2 s to 1 s and then to 0.5 s, and
    `pcg.collision.degraded` is raised. The fence absorbs the rest.
- **Cost per tile** on the SERVER core: the VM takes ≤ 0.5 ms (§5.8, 03 §5.5a) and the `MeshShape` build
  ≤ 0.5 ms, so ≤ 1 ms in total. A slab ancestor costs ≤ 0.5 ms (heights only) and covers 64 collision tiles.
- **Cache.** The cache is an LRU keyed by `TileKey`, under the `pcg.collision` memory tag.
  - It is capped at 0.4 GB of the 500-player cell's 0.5 GB PCG budget, and at 0.7 GB of the 50k-entity
    cell's 1.0 GB (§2.2).
  - Tiles in any current `A(b)` are pinned.
  - Nav terrain tiles (§7.3) and collidable scatter placement read heights from the cache.
  - A stamp invalidates its tiles at a tick boundary, and the rebuilt tiles are admitted at the next fence.
- **Client.** The client caches ≤ 96 tiles (≈ 14 MB) for its own body under its PCG tag. Their prefetch cost
  is in §2.4's Terrain row, and the fence's synchronous builds are in its Phase B fence line.

**Workload model.** Harrow at 20 Hz on the SERVER core, with 500 players dispersed ≥ 1 km apart so that no
tile is shared. This is the worst case.

| Body class | Count | `A(b)` box → tiles | New tiles/s each | Tiles/s | Tiles pinned |
|---|---|---|---|---|---|
| On foot, sprinting at 7 m/s with turns (a_max 10 m/s²) | 440 | 59 × 45 m → ≈ 3 | ≈ 0.16 | ≈ 70 | ≈ 1,320 |
| Ground vehicle at 100 m/s (a_max 8 m/s²) | 50 | 242 × 42 m → ≈ 7 | ≈ 2.2 | ≈ 110 | ≈ 350 |
| Ship in low flight at 300 m/s, inside the slab (a_max 30 m/s²) | 10 | 784 × 184 m → ≈ 42 | ≈ 15 | ≈ 150 | ≈ 420 |
| Spawns, respawns and warp exits (cold sets) | 1 per s | 3–42 | — | ≤ 20 | — |
| Slab ancestors | — | — | — | ≈ 22 (heights only) | ≈ 100 |
| NPCs in 50 lairs (2,000, mostly asleep) | 2,000 | `C(b)` only | ≈ 0 | ≈ 0 | ≈ 450 |
| **Total** | | | | **≈ 370 tiles/s ≈ 0.37 core** | **≈ 2,650 tiles ≈ 0.25 GB** |

- **Memory.** About 1,600 tiles hold shapes and the rest hold heights only: 1,600 × 147 KB + 1,050 × 17 KB
  ≈ 0.25 GB.
- **Headroom.** The 1,000 core-ms/s budget is ≈ 2.7× this design demand.
- **Cheaper cases.** Shared tiles (a town, a convoy) cost less, and ships above their slab cost nothing. A
  fixed 2 km radius would instead hold ≈ 2,400 tiles per player.
- **Gate.** RT-20 measures this load.

---

## 6. Asset pipeline (R06-ENG-11/12/13, R08-ED-P0-08/16)

### 6.1 Identity and registry

- **Sidecars.** Each source file has a `.meta` sidecar holding `{guid, importer, importerVersion, settings,
  labels, provenance}`. Provenance is required (01 §5.2).
- **IDs and handles.**
  - `AssetId` is a u64 fold of the GUID; packaging rejects collisions.
  - `AssetHandle<T>` (core `Handle`) is a loaded instance, and swaps happen by handle.
- **Registry.** 07 §3.4 owns the authoring registry. It is `.helios/registry.db` (SQLite, which ADR-014
  allows for tools), holding GUID ↔ path, type, labels, dependencies and reverse dependencies.
  `helios-assetd` is its only writer, and it can be rebuilt from the `.meta` files. Cooks ship a relocatable
  `registry.hreg`, which 02 owns and which loads in ≤ 1 s for 200k assets (AAA-ITR-2). Runtime code never
  opens SQLite.

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
- **DDC stores.** 07 §3.2 owns the tiers and caps, and 02 owns the key.
  - **Local:** `%LOCALAPPDATA%\Helios\DDC` (`$XDG_CACHE_HOME/helios/ddc` on Linux), LRU, **200 GB** default
    cap.
  - **Shared:** `helios-ddc`, an HTTP CAS (`GET/PUT /ddc/v1/<key>`) on MinIO or S3, filled by CI.
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
  - Cooks on the fly for the editor, PIE clients and PIE cells on the same workstation (AAA-ITR-3).
- **Transport (07 §3.1 owns it).** Tagged messages over the named pipe `\\.\pipe\helios-assetd-<project>`,
  or the Unix socket `$XDG_RUNTIME_DIR/helios-assetd-<project>.sock` on Linux. Every local process uses this
  one channel for requests, cooked-product fetches and notifications. There is no TCP listener, so there is no
  Windows firewall prompt and no remote exposure.
  - `AssetChanged{id, kind, cookedHash, ddcKey}`.
  - `RecordsDelta{contentVersion, changed[], removed[]}`.
  - `ScriptChanged`, `ModuleBuilt{module, path, engineBuildId}` (§1.4), `BuildFailed`.
  - Remote dev cells receive the same messages via `content.<scope>.published` (05), and fetch products from
    the shared DDC by key.
- **Swap.** Handles swap at frame or tick boundaries. The old version is freed after its fence or once its
  references drain.
- **Latency budget (p95, DEV), save → visible ≤ 2 s:**

  | Step | Budget |
  |---|---|
  | Detect | ≤ 100 ms |
  | Build: records / Luau / textures / shaders / containers | ≤ 300 / 100 / 800 / 1,000 / 500 ms |
  | Notify | ≤ 10 ms |
  | Swap | ≤ 200 ms |

- **Schema changes.** Native packages rebuild C++ and reload the game DLL through §1.4's protocol, which
  migrates live components whose layout changed (≤ 30 s). Dynamic project packages rebuild only their
  `.htypes` bundle and swap it with no DLL and no compiler, in ≤ 5 s (§3.8).

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

- **Build.** `JPH_DOUBLE_PRECISION` and `JPH_CROSS_PLATFORM_DETERMINISTIC` everywhere; AVX2 without FMA
  at the image's `avx2` level (§1.1); `Compute/` and `Shaders/` excluded (R10 §5).
- **Small bubbles.** A dispersed surface population creates many small bubbles (§5.4). A bubble with ≤ 64
  bodies steps as one job, calling `PhysicsSystem::Update` with a single-job Jolt job system and the
  worker's own 4 MB `TempAllocatorImpl`. Only larger bubbles own a pooled allocator (8 MB on the client,
  32 MB on a cell) and fan out through `HeliosJoltJobSystem`. 500 single-player bubbles therefore cost
  ≈ 32 MB of temp memory on 8 workers, not 16 GB.
- **Structure.** `PhysicsWorld` owns the grids. Each grid has a `PhysicsSystem` and an op queue, and its
  temp allocator follows the small-bubble rule above. `world` binds the body, collider, character, vehicle and
  `GridVolume` components. Shapes are shared through an `AssetId` cache.
- **Layers.** Object layers are Static, Terrain, Dynamic, Kinematic, Character, Vehicle, ShipHull, Debris,
  Projectile, Sensor and Interior. The broadphase has 5 layers. The collision matrix is a `PhysicsLayersDef`
  record.
- **Characters.** `CharacterVirtual` with a per-character up vector: radial on planets, grid gravity indoors.
  `ExtendedUpdate` handles stairs and floor-sticking. Client and cell share the mover (04 §5.2).
- **Vehicles (Phase 2).** Wheeled and tracked vehicles use `VehicleConstraint`, and hover vehicles use ray
  suspension; both are `PhysicsStepListener`s on a single chassis body (06 §8.1a). A bubble that holds a
  Vehicle-layer body steps physics at 60 Hz (3 collision steps at a 20 Hz tick), and vehicle casts break ties by
  `TileKey` or `EntityId`, never `BodyID`. Ships apply 06's flight forces to the hull, with mass from records.
- **Buoyancy (Phase 3).** `WaterBodyDef` defines sea level plus **≤ 8 Gerstner waves**, evaluated with `det::`
  at zone time.
  - A body samples ≤ 16 hull points for submerged depth and drag.
  - Physics never reads 03's FFT ocean. 03 renders the Gerstner set as the ocean's large scale.
- **Determinism.**
  - Fixed dt, and bodies are added in EntityId order.
  - Contacts and query results are sorted (06 §11).
  - **Ordering independence (normative).** No result may depend on a `BodyID`. A `BodyID` is a slot index
    that follows each `PhysicsSystem`'s add and remove history: the predictor's differ from the cell's
    (06 §8.1a), the cell's depend on which tile bodies came and went before, and a replay from a keyframe
    allocates new bodies into different free slots. Stock Jolt 5.6.0 orders solver work by `BodyID` in three
    places:
    1. `ContactConstraintManager` builds each contact constraint's `mSortKey` from
       `SubShapeIDPair{body 1 ID, sub-shape 1, body 2 ID, sub-shape 2}.GetHash()`, and `SortContacts` orders
       by that key, then by the body IDs;
    2. `PhysicsSystem::ProcessBodyPair` makes the lower-ID body "body 1" when both have the same motion type;
    3. `CharacterVirtual`'s `ContactOrderingPredicate` orders by `mBodyB`, then by `mCharacterIDB`, which a
       global counter assigns.

    A hull, chassis or character touching several tile bodies would therefore run Gauss–Seidel in a different
    order on each host and diverge in the low bits, whatever the cast tie-breaks above do. The rules:
    - *Stable body keys.* Each body's `mUserData` holds its key: the `EntityId` for entity bodies (placed
      statics included), the packed `TileKey` for Terrain-layer tile bodies, and the deterministic PCG instance
      key for generated asteroids and collidable scatter (§5.8). A `CharacterVirtual` carries its entity's
      `EntityId`. `(object layer, key)` is unique within a `PhysicsSystem`; debug builds assert it
      in `AddBody`, which refuses key 0.
    - *Vendored patch `third_party/jolt/patches/stable-order`* (≈ 80 lines, listed in
      `third_party/MANIFEST.md` beside Luau's patches, rebased on each Jolt update and covered by
      `sim_abi.physics`, 04 §6.7). It uses `(layer, key)` in all three places: the sort-key hash and its
      tie-break, the body-1 choice and `ContactOrderingPredicate`. Jolt's caches stay keyed by `BodyID`,
      because they are lookups, not orders.
    - *No cross-`Update` contact cache on predicted bodies.* Warm-start impulses, and manifolds that Jolt
      reuses when a pair moved < 1 mm (`mBodyPairCacheMaxDeltaPositionSq`), carry state from one step to the
      next in caches keyed by `BodyID`. A client rollback restores the chassis and constraint (06 §8.1a
      rule 3) or the server's full state, never the cell's cache. The same patch therefore adds
      `Body::EFlags::NoCrossUpdateCache` (the last free flag bit), set on every ShipHull- and Vehicle-layer
      body on every host. On the first collision step of each `PhysicsSystem::Update`, a pair involving such a
      body recomputes its manifold and starts its impulses at zero. Later collision steps of the same `Update`
      (a vehicle bubble's 3 substeps) warm-start as usual, and the old manifold is still found, so
      `OnContactPersisted` and 06's impact events are unchanged. A predicted step is thus a pure function of
      06 §8.2's inputs and the collider set. Resting ships and parked vehicles lose only warm-start
      convergence, and RT-03's permuted variant bounds the effect.
    - *Wheel contact IDs.* A `VehicleConstraint`'s `SaveState`, which 06 §8.1a's correction carries from the
      cell to the client, holds each wheel's contact `BodyID`. Vehicles keep Jolt's default of a full wheel
      test on every step (`SetNumStepsBetweenCollisionTestActive` and `…Inactive` at 1), which replaces that
      ID before anything but its validity is read, so the cell's bytes restore correctly into the predictor.
    - *Where exact IDs are still needed.* Jolt's `RestoreState` writes into existing bodies by ID. Replay
      keyframes and migration residuals therefore carry a per-grid body table `(BodyID, stable key)`, and the
      restoring host creates every body with `BodyInterface::CreateBodyWithID` and every constraint in
      `mConstraintIndex` order before restoring (04 §6.7, §10.2).
    - *Rejected: deterministic `BodyID`s everywhere* (`CreateBodyWithID` from a shared allocator driven by
      stable keys). The predictor holds only a subset of the cell's bodies, so a shared key → slot map needs a
      global slot space per system and collision probing whose result depends on which other keys exist. A
      keyed order does not care which bodies exist.
  - A CI hash runs on every determinism toolchain (both MSVC toolsets, clang-cl, GCC, Clang and MinGW; 09 §6,
    ADR-001a rule 7) and at 1, 4 and 16 workers.
  - `HeliosJoltJobSystem` (a `JobSystemWithBarrier`) runs Jolt work as High-priority jobs.
- **Netcode hooks.** `SingleBodyPredictor` supports hull prediction and, from Phase 2, ground-vehicle prediction,
  stepping the chassis with its `VehicleConstraint` or hover listener and snapshotting the constraint through
  Jolt's `SaveState`/`RestoreState` (04 §5.3, 06 §8.1a). Frame-local hitbox capsules come
  from each skeleton's `HitboxSetDef` (≤ 24 capsules, each tagged with one `HitZone.*`), posed by anim, and feed
  04's history; the resolved zone becomes `DamagePacket.hitZone` (06 §8.7).
- **Authoring records** (sketched in 07 §2.6.1). `CollisionProfileDef` (object layer, narrowing ignores, query
  channels, default material); `PhysicalMaterialDef` (friction, restitution, density, surface tags, penetration;
  impact cues and footsteps in its `client{}` block). Cooked shapes carry a physical-material ID per sub-shape
  (§6.5). `HitboxSetDef` as above. `RagdollDef`, `SecondaryChainDef` and `ClothDef` are `@client_only`.
- **Cosmetic physics (client only).** Ragdolls (Phase 2) and cloth (Phase 4) run in a separate client-side
  **cosmetic `PhysicsSystem`** per local bubble. It holds shared static shapes and kinematic proxies of nearby
  characters' hitbox capsules, and never contains a predicted body, so 04 §5.3's bit-identical hull prediction is
  untouched. Cells never simulate either. Death replicates only `{point, dir, impulse}` from the `DamagePacket`;
  each client's ragdoll is plausible, not identical, and loot position comes from the server capsule root.
  - *Ragdolls:* Jolt `Ragdoll` with swing-twist constraints from `RagdollDef`; ≤ 16 active per client, ≤ 0.5 ms
    on 8 workers; asleep after 5 s, then frozen into a static pose. Powered hit reactions in Phase 3.
  - *Cloth decision:* Phase 2 ships **secondary-motion chains** only (`SecondaryChainDef`: ≤ 8 bones, a Verlet
    anim-graph node in the A0–A1 tiers, reading 03's analytic wind; ≤ 8 chains per character, ≤ 0.05 ms each).
    Phase 4 adds **cloth as Jolt soft bodies with skinned constraints** (`ClothDef`: capes, banners, flags) in
    the A0 tier within 30 m, ≤ 16 simulated cloths and ≤ 1 ms on 8 workers, skinned-only beyond. There is no
    GPU cloth.

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
  `{id, startTick, rate}`. Budget: 200 entities at 60 Hz in ≤ 1 ms. Cells and clients pose hitboxes with
  `det::HitboxSampler` (exact normalize, no `rsqrtps`/`rcpps` estimates, scalar local-to-model without
  contraction), never ozz's `SamplingJob`/`BlendingJob`, whose estimate intrinsics differ between Intel and
  AMD (04 §10.2). ozz's SIMD jobs serve cosmetic animation only.
- **Crowd and animation-LOD contract (R05, BENCH-1; 03 §7.6):**

  | Tier | Range (scaled by screen size) | Evaluation | Output to 03 |
  |---|---|---|---|
  | A0 | < 15 m | Full graph + IK, every frame | Palette every frame |
  | A1 | 15–30 m | Graph without foot IK, 30 Hz | Palette on update |
  | A2 | 30–80 m | Locomotion subset, 15 Hz | Palette on update; 03 skins at reduced rate |
  | A3 | > 80 m or off-screen | Root motion only | `{impostorPose, heading, phase}` |

  Each entity carries `AnimLod{tier, poseSerial}` in the extract packet, so 03 re-skins only when
  `poseSerial` changes. A1 and A2 updates are phase-staggered by EntityId, so each frame carries half of A1
  and a quarter of A2 (03 §7.6a's skinning budget assumes it). A governor caps A0 at 32 entities and A1 at 96.
  Budget: 260 characters ≤ 2 ms on 8 workers. Rendertest and capture builds add three debug cvars for 03's
  temporal-stability check (03 §8.4a): `anim.forceTier` pins a character's tier, `anim.forcePhase` pins
  its stagger phase, and `anim.lodRate=full` evaluates A1–A2 every frame (the full-rate control). Shipping
  builds compile them out.
- **Appearance skeleton.** `BoneScale` parameters (06 §10) are applied **once per appearance**. They scale the
  bind skeleton's bone lengths and set per-joint scale, with child compensation so a scaled forearm does not
  scale the hand. Local-to-model therefore emits scaled palettes with no per-frame cost beyond the existing
  per-joint multiply. Attachments, IK targets and client hitbox joints read the same skeleton, while cells
  use the validated ranges. `Morph` and `DnaBlend` geometry is baked by 03 (§7.6a), never evaluated per
  frame.
- **Facial runtime (Ph4; R04, WP-4.6, AAA-REN-8 L5).**
  - *Rig.* Each head basis has a `FaceRigDef`: 52 FACS-aligned channels, named as the capture solver emits them
    (09 §4.3.3), plus jaw, tongue, eye and lid joints and eyelid-follow correctives. A DNA-blended head uses
    the β-weighted blend of its bases' shapes, which 03 §7.6b bakes.
  - *Layers,* summed per channel and clamped to [0, 1]:
    1. **Performance curves.** Captured or keyed `FaceClip`s (52 f16 curves at 30 Hz, stored as compressed ozz
       float tracks) played by dialogue and montage slots (T13/T15).
    2. **Viseme layer,** for lines without a performance clip. At cook time `helios-assetd` phoneme-aligns each
       VO line into a `VisemeTrack` per language (15 visemes, with onset and offset). At runtime each viseme
       maps to a per-species FACS pose, coarticulated by 80 ms raised-cosine cross-fades, and jaw-open is
       scaled by the line's RMS envelope. Proximity voice chat has no alignment, so it drives jaw-open alone
       from each decoded Opus frame's envelope.
    3. **Emotion layer.** T13's auto-staged additive curves (brow, cheek, lid tension).
    4. **Eyes and lids.** Look-at comes from the LookAt IK node: the eyes lead and the head follows after
       150–250 ms, within ±35° of eye yaw and ±25° of pitch. Micro-saccades (0.5–2°, 2–4 per second) and
       macro-saccades go to conversation targets. Blinks come every 2–6 s and last 150 ms, with lid-follow on
       gaze pitch. The saccade and blink generators are seeded per EntityId, so replays are deterministic.
  - *Clock.* Curves and visemes are evaluated at the voice's playback position at the frame's predicted
    display time (08 §1.3a M7), corrected for the device's output latency. Lips therefore track the audio
    heard, not the audio submitted.
  - *Face LOD:*
    - **F0:** hero faces, plus A0 faces within 8 m that are speaking or in dialogue, ranked by screen size and
      capped at 8 (4 on MIN and in Performance mode). All layers run, and 52 unorm8 weights plus joint
      overrides go into the extract packet (64 B per face).
    - **F1:** other A0 and A1 faces. Only jaw, lid and eye joints move, and visemes map to jaw-open.
    - **F2:** A2 and beyond. The face is neutral.
  - *Budget.* ≤ 0.05 ms per F0 face and ≤ 0.01 ms per F1 face on one REF worker, so ≤ 0.8 core-ms for 8 F0
    and 32 F1 faces, inside the Animation row of §2.4. 03 §7.6b's GPU side is ≤ 0.15 ms.
  - *Acceptance:* RT-22.
- **Later.** Motion matching in Phase 4 (R04); LMM in Phase 5.

### 7.3 Audio (miniaudio) and navigation (Recast/Detour 1.6)

**Audio.**
- **Mixing.** A Helios mixing graph runs on the miniaudio device, with buses Master → Music, SFX, Voice, UI
  and Ambience, and ducking snapshots.
- **Budget.** 128 real voices, ≤ 5 % of one core, ≤ 30 ms latency.
- **Events.** `AudioEventDef` records define containers, RTPCs, attenuation and Doppler. Banks stream with the
  containers that use them.
- **Spatial audio.** Occlusion uses ≤ 64 physics rays per frame. Sound propagates through the `PortalGraph`.
- **Later.** HRTF comes in Phase 3. Wwise and FMOD are allowed only as `IAudioBackend` plugins (ADR-013).

**Voice chat (`engine/voice`, Phase 3; ADR-015).** The server side is 04 §2.7: the gateway relays VOICE to
`helios-voice`, which owns the channel model (party, fleet, org, ship intercom, proximity), talk rights,
speaker selection, blocks, moderation and the bandwidth budget of ≤ 40 kbit/s up and ≤ 104 kbit/s down. The
engine owns the client pipeline and one server-side helper:

| Stage | Design | Budget |
|---|---|---|
| Capture | miniaudio capture device, 48 kHz mono, 20 ms frames. The callback writes to a lock-free SPSC ring. Push-to-talk by default; voice activation uses an energy gate with a 300 ms hangover (08 §1.4 settings). An `IVoiceDsp` seam hosts echo cancellation and noise suppression (SpeexDSP or RNNoise, pending 09 §3.2 approval) | ≤ 10 ms capture latency |
| Encode | libopus, 20 kbit/s VBR, complexity 5, in-band FEC tuned for 5 % loss, DTX, two frames per datagram (04 §2.7) | ≤ 0.1 ms per 20 ms frame |
| Send and receive | Datagrams go straight to the **net thread's** VOICE queue, never through the game thread, so voice survives game-thread hitches. Received frames go to a per-stream adaptive jitter buffer (40–100 ms) with Opus PLC and FEC | ≤ 3 decoded streams (04 speaker selection) |
| Mix and spatialize | Streams play on the **Voice** bus. `party`, `fleet` and `org` are 2D. `ship` intercom is 2D with an optional radio-filter DSP. `prox` streams are placed at the speaker entity's interpolated pose, which is resolved from its `NetHandle` and delivered to the audio thread in the extract packet. They are positioned relative to `presentation`'s listener, with SFX attenuation and `PortalGraph` occlusion. Any active stream ducks Music by 6 dB | Encode, 3 decodes and mixing ≤ 0.3 ms per 20 ms on the audio thread (≈ 1.5 % of a core); ≤ 2 MB |
| Server helper | The owning cell computes `prox` hearers within ≤ 40 m. On the same grid in open space or on a surface this is straight-line distance. Inside interiors it is path distance through open portals only, found with `PortalGraph::propagate`. It computes them on the 4 Hz interest refresh and streams `VoiceAudience` deltas (04 §2.7) | ≤ 0.2 ms per refresh for 500 players |

The client never decides who hears whom. Per-player mute is local, and block and sanction are server-side
(04 §2.7). Acceptance is NS-3.9.

**Navigation (R08-ED-P1-12).**
- **Tiling.** One tiled `dtNavMesh` per grid, with 64 m tiles.
- **Tile sources.** Authored tiles stream with their containers. Terrain tiles are built near activity on the
  Pathfind pool, ≤ 5 ms each. Structures use `DetourTileCache`.
- **Ship interiors** are baked ship-local. `GridLink` off-mesh links trigger `Reparent`.
- **Queries** run asynchronously: 2,000/s at p99 ≤ 5 ms (06 §12.2). DetourCrowd runs per grid.

### 7.4 Scripting host (Luau 0.739; R01-P0-4, R08-ED-P0-10)

- **VMs.** Each cell zone instance has one VM on a serialized **script lane**, a PostPhysics job chain
  (04 §3.3). The client and the editor each have their own VM. A world-script host (`helios-cell --role
  world-script`) runs one VM per world-script partition under the cell rules below (sandbox, deterministic
  fuel, heap caps), with the `world` realm's API in place of the zone API (05 §1.23). Bytecode is shared per
  content version.
- **Sandbox.** Setup runs `luaL_openlibs`, then the Helios API, then `luaL_sandbox`, and gives each module its
  own `luaL_sandboxthread`. `io`, `os`, `debug` and `loadstring` are removed. `math.random` becomes named
  seeded streams (06 §11). On every host, the vendored `det-math` patch routes Luau's transcendentals, `^`,
  the `luauF_*` math fastcalls, the `vector` library, native codegen's libm pointers and the compiler's
  constant folding to `det::`, so script math is bit-identical across MSVC/UCRT and glibc (04 §10.2). On cells `collectgarbage` is also removed and a wrapped `setmetatable` rejects
  `__mode`, so GC timing stays unobservable for replay (04 §10.2).
- **No involuntary yields (normative; 04 §3.1, §10.2 and 06 §11 follow).**
  - A coroutine yields only at calls its author can see: `wait`, `awaitService`, `awaitEvent`, `task.yield()`
    and `task.checkpoint()`. The `interrupt` callback never calls `lua_yield`.
  - Preemptive yield is only partly possible anyway. Luau raises "attempt to yield across metamethod/C-call
    boundary" whenever `nCcalls > baseCcalls` (`VM/src/ldo.cpp`, `lua_yield`), so an interrupt inside a
    metamethod, a `table.sort` comparator or a callback invoked from C++ would error instead of yielding.
  - It is also unsafe. An invisible yield breaks 06 §11's "revalidate handles after every yield" rule and
    allows lost updates when a script reads a value, gets suspended, and then writes the value back.
- **Budgets** (reconciling 04 §3.1 with 06 §11). On cells, **1 fuel = one `gc < 0` safepoint**. Binding and
  builtin calls are charged extra fuel (next bullet), so fuel tracks CPU time and not only Luau instructions.
  The constants are fuel counts, calibrated on the reference SERVER core, with `ns_per_fuel` measured by
  `--calibrate-fuel`.

  | Budget | Cells (deterministic fuel) | Client and editor (wall time) | Action |
  |---|---|---|---|
  | Soft, per resume | `fuel_per_resume` ≈ 2 ms (calibrated, 04 §10.2) | 2 ms | Telemetry `ScriptOverBudget{module, entity, fuel}` and a DAP/editor warning. From then on `task.checkpoint()` yields; below the soft budget it returns at once. When `lua_isyieldable` is false, `task.checkpoint()` returns `false` instead of yielding |
  | Hard, per resume | `fuel_kill` = min(≈ 5 ms, 25 % of the tick), calibrated; 4.2 ms at 60 Hz | 5 ms | Kill (below) |
  | Lane, per tick | `fuel_per_tick` ≈ 15 % of the tick | 1 ms on the client (§2.4) | Coroutines are resumed in `(wake tick, EntityId, seq)` order while lane fuel remains; the rest wait for the next tick. An engine→Luau callback on the lane counts as a resume. **Bound:** lane fuel per tick ≤ `fuel_per_tick` + `fuel_kill` (one resume that started with budget left) |
  | Wall backstop | 20 ms per resume. It is a fault path, not a budget: it catches a binding whose real cost far exceeds its calibrated charge, such as a pathological mesh | — | Kill, logged as a `ScriptKilled` replay event (04 §10.2); the only wall-time script decision on cells. Outside NS-2.4's injected kills, every backstop kill is filed as a calibration defect |
  | Heap | 16 or 256 MB per VM, tracked per module with `lua_setmemcat` | same | Allocation failure → Luau memory error |

- **Fuel charges for bindings and builtins (cells; deterministic).**
  - **Declared per function.** Every Luau-callable C++ function is a `scriptlib` `fn` with
    `@script(cost=n[, each=m, of=…])` (§3.1). The lint fails a function without `cost`, and a clang-tidy check
    rejects `lua_pushcfunction` outside generated glue and the sandbox's builtin wrappers. So no C++ path
    reachable from Luau is uncharged.
  - **Charged in the glue.** The generated glue calls `script::charge(L, bindingId, items)`. The charge is
    `cost + each × items`.
    - When `of=` names an argument, `items` is that argument's list length or, for a `PrefabRef`, the
      prefab's cooked entity count. The whole charge is taken **before** the call.
    - `of=result` is allowed only on `@pure` functions. `cost` is charged before the call and
      `each × len(result)` right after it.
    - If a charge takes the resume to `fuel_kill`, the glue sets the sticky kill flag and raises it there.
      That is before any side effect, or just after a call with no side effects. The outcome is the same as
      a safepoint kill and just as deterministic, because the charge depends only on arguments and
      results.
    - Charges count toward `fuel_per_resume` and lane accounting exactly as safepoints do.
  - **Builtins.** The sandbox replaces the builtins whose C work grows with their input with charging
    wrappers, before `luaL_sandbox` freezes the libraries. These are `string.rep`, `format`, `gsub`,
    `find`, `match`, `gmatch` and `split`; `table.concat`, `sort` (charged n·⌈log₂ n⌉ before any comparator
    runs), `move`, `create`, `clone` and `find`; and `buffer.fill` and `copy`. Scripts are compiled with
    `Luau::CompileOptions::disabledBuiltins` listing those functions, so no `FASTCALL` bypasses a wrapper.
    The fastcall builtins that remain are O(1) or copy one existing string (`string.sub`). On cells, pattern
    functions also cap their subject at 64 KiB.
  - **Calibration (`helios-cell --calibrate-fuel`, reference SERVER core, pinned, fixed turbo policy).**
    1. `ns_per_fuel` is the median ns per safepoint over the interpreter corpus in `engine/script/calib/`.
    2. Each `scriptlib` function and wrapped builtin runs a generated microbenchmark. Its argument sets
       come from fixtures sampled from NS-2.4 and BENCH-4 zone traces. The run gives the p95 fixed cost and
       the p95 per-item slope. Then `cost = ⌈p95_fixed / ns_per_fuel⌉` and `each = ⌈slope / ns_per_fuel⌉`.
    3. The output is `content/profiles/fuel_costs.jsonc`, committed as canonical JSONC and keyed by the
       function's schema-lock ID. It holds the three lane constants and every `(cost, each)` pair. The
       schema's `cost` is only the default until a function's first calibration.
    4. The cook embeds the table in the zone profile. **The replay header stores the full table** (≤ 2,000
       rows × 8 B ≈ 16 KB) and its xxh3, and replays charge from the header. Recalibration therefore never
       changes an old recording, and cross-compiler replays (NS-3.8) use one table.
  - **Staying calibrated.**
    - The nightly `--calibrate-fuel --check` on the SERVER lab node fails when any function's measured p95
      exceeds its charge by more than 1.5×, or when a merged function has no calibrated row.
    - In production, each lane exports `script_ns_per_fuel{module}`. A module above 2× the calibrated
      `ns_per_fuel` for 60 s raises an alert, which means a binding is under-charged in live content.
  - **Client and editor.** The glue counts the same fuel there as well. Budgets on those hosts are wall
    time, so the count does not decide anything. It feeds the editor's script profiler and the DAP
    variables view, so a designer sees a script's cell cost while testing in PIE.
- **Tick allowance (04 §3.3).** The lane's share of stage 4 is `fuel_per_tick`. With every binding charged,
  the lane's wall time is ≈ fuel × `ns_per_fuel`, and RT-13 holds it within 1.25×. The only possible
  overrun is the bound above: one resume of at most `fuel_kill`.
  - At 20 Hz that is ≤ 5 ms, inside the 15 ms between 04's 35 ms p99 target and the 50 ms tick.
  - At 60 Hz it is ≤ 4.2 ms, and 11.7 + 4.2 ms < 16.7 ms. That is why `fuel_kill` is capped at 25 % of
    the tick.
  - A resume that reaches `fuel_kill` is killed, and three kills in 60 s disable its module. A module can
    therefore cause at most three worst-case ticks per minute.
  - Heavy resumes that stay under the kill appear as `ScriptOverBudget` and raise TiDi's `L` like any other
    load. They never exceed the tick silently.

- **Kill semantics.**
  - The interrupt raises a Lua error at a `gc < 0` safepoint, or the binding glue raises it at a fuel charge.
    Luau is built with C++ exceptions (`LUA_USE_LONGJMP=0`, the default), so binding frames unwind through
    RAII. That includes built-ins such as `table.sort` that call back into Lua. Bindings that call back into
    Luau hold no locks.
  - When engine C++ invokes a Luau callback, such as an event handler or a UI binding, it uses a `lua_pcall`
    wrapper. The wrapper checks the killed flag, finishes its own cleanup, and then raises the kill again in
    the calling coroutine, if there is one.
  - The kill is **sticky**: the thread is marked killed and every later safepoint raises again, so `pcall`
    cannot swallow it.
  - When the resume returns, the scheduler closes the thread (`lua_resetthread`) and emits
    `ScriptKilled{tick, module, entity, fuel}` telemetry. A fuel kill is deterministic, so replay repeats it
    at the same fuel count without a log entry. Only wall-backstop kills are logged replay events.
  - Three kills of one module within 60 s disable it in that zone and raise a T27 alert.
- **Handles are checked on every dereference.** Each `Entity` userdata access checks liveness and generation
  through `EntityRegistry` (≤ 20 ns) and raises a typed `StaleHandle` error instead of touching a recycled
  entity. 06 §11's revalidation rule remains a gameplay rule, because a target can die or change AG across an
  explicit `wait`.
- **Coroutines are tasks.** `wait(zoneSecs)`, `awaitService` and `awaitEvent` yield, and
  `lua_pcallyieldable` makes yields inside `pcall` work.
- **Bindings** are generated from `@script`. Per-entity state lives in `ScriptState` components (04 §3.1).
  A project declares them in a dynamic package, with no C++ (§3.8).
- **Hot reload** calls `__reload(old)`. Coroutines already running finish on the old code.
- **Debugging.** The **DAP adapter lives in `engine/script`** (about 2 kLOC over `lua_breakpoint` and
  `lua_singlestep`). It serves over TCP from the editor, dev client and cells, including `--replay`.
  luau-lsp reads the generated `.d.luau`, and CI uses the old type solver (R10 §4).
- **Native codegen** is opt-in per module and off for UGC (Phase 5). On cells and world-script hosts it stays
  off: `VmConfig` refuses codegen on those hosts. The vendored `third_party/luau/patches/codegen-fornloop-fuel`
  is the precondition for lifting that, because it makes native fuel equal to the interpreter's: Luau 0.739's
  code generator puts the numeric-`for` interrupt at the top of the loop body instead of in `FORNLOOP`, so a
  loop left by `break` or `return` costs one extra fuel in native code (04 §10.2). Lifting the refusal is
  §8.1's P3 "codegen opt-in on cells" item, behind 04 §10.2's interpreter-versus-native corpus run.
- **Interrupt cost.** WP-0.10 measured the `interrupt` callback at 12–17 % of script time against RT-13's
  ≤ 10 %, so the vendored `third_party/luau/patches/fuel-counter` (an inline counter decremented at each
  `gc < 0` safepoint, calling the host only when it reaches zero) is required on every host that meters fuel.
  It also lets the VM charge operations the API cannot see, such as `..` copies. Both patches join `det-math`
  in `sim_abi.script` (04 §6.7).
- **Replay-keyframe rebase (cells; 04 §10.2).** Every 30 min of zone time a cell rebases its VM so that a
  replay keyframe can be cut. It waits for a tick at which no coroutine of a module without an
  `Authority.Adopted` handler is suspended (≤ 60 s, else the keyframe is skipped once), ends every
  coroutine, swaps in a fresh VM already loaded for the same content pin, runs the module chunks in manifest
  order and raises `Adopted{cause = rebase}` for every entity with a `ScriptState`, exactly as after a
  planned migration. The lane's per-module kill history and disabled set are C++ state and carry across.

### 7.5 Game UI runtime (RmlUi 6.3; R04-P1-18, R03-P0-9, R08-ED-P1-10)

02 owns `engine/ui` and `engine/text`. 08 owns the HUD and screens, `WorldMarkerRenderer` and the addon host.

- **Rendering.** `RmlRenderInterfaceHelios` records `UiDrawList`s (geometry, bindless textures, scissor).
  03's UI pass replays them through the RHI (03 §7.5), and the launcher replays them on SDL_Renderer. UI code
  never calls a graphics API directly (the SWTOR lesson).
- **Text.** `text` is a `Rml::FontEngineInterface` built on FreeType, HarfBuzz, SheenBidi and libunibreak, with
  font fallback chains and MSDF glyphs for world-space text.
- **View-models.** A native `viewmodel` generates `ViewModel<VM>` with `edit(FieldMask)` (08 §4.2), an RmlUi
  data model and Luau types. A dynamic one, from a project package, binds through `ui::DynViewModel` and
  table-driven adapters with the same dirty-field behaviour (§3.8).
  - Adapters push only dirty fields. Fields annotated `@source(Component.field)` or `@source(Service.Stream)`
    get schemac-generated adapters, so Foundation panels need no hand-written C++ (08 §1.7.1).
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
- **Sampling.** Input is sampled per command frame. The game thread drains the OS thread's event ring (§2.4)
  and assigns each event to a command frame by its SDL timestamp.
- **Rebinds** layer as record defaults → device profile → account overrides (08 §1.5). Account overrides live
  in the account-scope settings blob (08 §1.4), which is synced through the Character service (05) and cached
  locally for offline and editor use. There is no separate `input.jsonc`.
- **Bots** inject 04 §5.2 input commands directly, without SDL.

**Localization.**
- **Format.** `LocString` keys point into `.hloc` tables, cooked to a relocatable `.hstr` per language.
- **Messages.** A MessageFormat subset with generated CLDR rules. ICU is tools-only (R10 §12).
- **Budget.** 500k strings: ≤ 100 ns per lookup and ≤ 48 MB per language.

**Config and saves.**
- **User settings (08 §1.4 owns them).** Machine scope (display, graphics, audio device, `Saved` CVars) is in
  `%LOCALAPPDATA%\<Install>\<channel>\settings\machine.jsonc`, or the same layout under
  `$XDG_CONFIG_HOME/<productId>` on Linux (`<Install>` and `<productId>` come from the product, 08 §2.10).
  Account scope (keybinds, UI layout, chat tabs, addon settings) is a ≤ 256 KiB blob synced through the
  Character service. `engine/core` provides the atomic writer (write to a temp file, flush, rename) and the
  canonical JSONC format. Editor per-user files follow 07 (layouts in §1.3, journal in §1.2).
- **World state** stays on the server. Tools snapshot worlds as tagged `.hsnap` files.

---

## 8. Ladder, acceptance, tests, risks, traceability

### 8.1 MVP → AAA feature ladder

| System | P0 | P1 | P2 | P3 | P4 | P5 |
|---|---|---|---|---|---|---|
| Core | Platform, jobs, memory tags, CVars, VFS, CPU gate, ISA levels and pre-gate audit, crash, Tracy; **link-model spike (RT-18)** | Budgets in CI; game-DLL live reload; OS/game/render threads; MIN BENCH-2 CPU tracking | Fibers?; crashpad | io_uring/IORing; **MIN CPU budgets gate (RT-12 MIN)**, `cpu.*` settings | Live ceilings | — |
| Schema | schemac C++/Go/Luau/SQL, lock, lint | `$rid`, HXL, tags, split, repl, `@keyed` | proto, validators; **dynamic project packages** (`.htypes`, generic `TypeOps`, `DynViewModel`, Go `pkg/htypes`; RT-21) | Live deltas | — | UGC subset |
| ECS | Wrapper | **50k benchmark**, command buffers, dirty bits, prefabs | Update LOD, budgets | Non-fragmenting pairs? | — | — |
| World | Frame graph | Reparent, grids, BENCH-2, 10¹³ m | Multi-grid queries, portals | Nested grids, BENCH-6 | — | Seamless galaxy |
| Streaming | `.hpak` v0 | Containers, sources, budgets | HLOD, layers, residency | AAA-CNT-2 | MIN tuning | — |
| PCG | `hnoise` + Slang twin; AVX2/SSE4.2-width/scalar kernels | Harrow, stamps, scatter, Scree; collision tile sets, fence and cache (§5.8a; RT-20 slice) | Biomes, roads, deltas; RT-20 at 500 players with vehicles | Earth-size | Vendor lab | Ecosystems |
| Assets | `.meta`, DDC, glTF, textures | assetd; records/Luau reload | All-asset reload, shared DDC, paks | Zone cook ≤ 60 s | Nightly ≤ 4 h | Distributed |
| Physics | Small hash; `stable-order` patch and stable body keys (RT-03 permuted variant) | Characters, ships, grids, disjoint bubbles (RT-19); physical materials | Vehicles (06 §8.1a); bubble merge/split at fleet scale; ragdolls (cosmetic system) | Hangars, buoyancy; powered ragdolls | Budgets; cloth (Jolt soft bodies) | Destruction |
| Anim | Sampling | Graph, root motion, `HitboxSetDef` hitboxes | IK, retarget, secondary chains; appearance skeleton (`BoneScale`) | Crowd LOD, staggered A1/A2 | Motion matching; facial runtime (FACS layers, visemes, eyes, face LOD; RT-22) | LMM |
| Audio / voice / nav | Device | Events, 3D / — / tiles | Banks, occlusion / — / terrain tiles | HRTF / `engine/voice` + prox audience (NS-3.9) / moving interiors | Budgets | — |
| Script | VM, sandbox, fuel meter, sticky kill | Budgets, tasks, `task.checkpoint`, DAP, reload; `scriptlib` binding fuel, builtin wrappers, calibrated cost table; `fuel-counter` and `codegen-fornloop-fuel` patches | Capabilities (06); replay-keyframe rebase (04 §10.2) | Codegen opt-in on cells | — | UGC VMs |
| UI / input / loc | SDL3 input | RmlUi, view-models, `UiSurface` | IME, loc, rebinding | `<datagrid>` scale | Voiced languages | Addons |

### 8.2 Acceptance criteria (automated)

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RT-01 | **50k-entity zone**, SERVER, 20 Hz (20k replicated + 30k placed, ~150 ECS archetypes, 5k bodies in 12 grids). Engine stages ≤ 12 ms p99; 9k structural ops ≤ 1.5 ms; 50k×3 iteration ≤ 0.4 ms on one thread; ECS ≤ 400 MB; ≤ 5,000 tables. **Failure opens the custom-ECS ADR** | AAA-SRV-4, CNT-4 | 1 |
| RT-02 | **10¹³ m** (with 03): ship at 1 km/s, cockpit camera. Jitter < 0.05 px for a grid cube at 2 m and a system-frame object at 1 km; physics drift < 1 mm over 10 min; replication error ≤ 1/256 m | AAA-REN-5 | 1 |
| RT-03 | **Physics hash**: 1,000 bodies + 50 characters + 10 vehicles × 3,600 steps, identical on every determinism toolchain (both MSVC toolsets, clang-cl, GCC, Clang and MinGW; 09 §6, ADR-001a rule 7), at 1/4/16 workers, and on AMD and Intel CPUs (the H1 lab's SERVER and Intel boxes, 09 §4.3.1; 04 NS-3.8). The same job runs 06 GP-4a's FBW hash (flight controller and thruster allocation, including saturation and damaged-thruster cases, and EVA suit thrusters), on which 04 §5.3's bit-exact hull prediction depends, and from Ph2 06 GP-4d's ground-vehicle and mount hash. **Permuted variant (§7.1):** the same scene, with hulls, chassis and characters each touching ≥ 3 terrain tile bodies, is rebuilt so that every `BodyID` differs (1,000 dummy add/remove cycles first, tiles admitted in reverse, bodies re-added in shuffled order) and must give the identical hash; a CI build without the `stable-order` patch must fail it. ShipHull and Vehicle bodies resting on a 20° tile slope creep < 1 mm in 60 s and sleep within 2 s (the no-cross-`Update`-cache rule) | AAA-PLT-4 | 0–1 |
| RT-04 | **PCG hash**: 10k tiles on 3 bodies + scatter + asteroids, bit-identical across compilers, client/cell, AMD and Intel CPUs, and the AVX2, SSE4.2 and scalar kernel widths (§5.8). `hnoise` C++/Slang corpus matches on lavapipe and vendor GPUs (≤ 1 cm). The CPU bench runs on the `avx2` kernel (asserted from the `pcg.kernel` log) | AAA-PLT-4, CNT-1 | 1 |
| RT-05 | **Hot reload** in PIE client and cell ≤ 2 s p95: records and Luau (Ph1); all asset kinds (Ph2) | AAA-ITR-1 | 1–2 |
| RT-06 | **Streaming**: game-thread I/O waits ≤ 1 ms; zero misses in BENCH-2 at 1,500 m/s; ≤ 150 MB/s; `physics.collision_tile_miss` = 0 on the client and the cell through the descent and landing (§5.8a) | AAA-CNT-2 | 1–3 |
| RT-07 | **Records**: 100k compile ≤ 60 s, load ≤ 2 s | AAA-CNT-5 | 3 |
| RT-08 | **Cook**: zone ≤ 60 s, nightly ≤ 4 h, warm editor open ≤ 10 s | AAA-ITR-2/4 | 2 |
| RT-09 | **Lint**: no server-only bytes in client paks; every client→server rpc has `@ratelimit` + `@intent`; no `EDITOR_ONLY` leak; §1.1's ISA levels hold on every image in both link flavours, with and without `HELIOS_PROFILE` (the five audit checks: flags per level; an x86-64-v1, COMDAT-free, import-allowlisted gate object; the gate as the first TLS callback of the first-initialized image, with every TLS callback, `.CRT$XI*`, `.CRT$XC*` and `.CRT$XD*` entry and `_pRawDllMain` enumerated and attributed, which covers mimalloc's `.CRT$XLB`, `.CRT$XLY` and `.CRT$XIB` hooks and Tracy's `.CRT$XCB` statics and `.CRT$XD*` `thread_local`s, the import-closure load order, and the four seeded canaries failing; in `base` images, instructions above x86-64-v1 only in listed self-dispatching symbols; the SDE and qemu emulator runs, including SDE `-nhm` and `-snb` on the modular `HELIOS_PROFILE=ON` editor with the Helios DLLs chip-checked); SDK game-module objects carry the `avx2` set (`sdk-consumer`, from WP-2.16a2; §1.1); every `scriptlib` function has `@script(cost)` (§7.4); no dynamic package declares a native-only construct (§3.8) | AAA-SEC-1/4 | 1 |
| RT-10 | **Memory**: §2.2 budgets hold in every BENCH scene; 24 h soak RSS growth ≤ 2 % | AAA-CNT-3/4, STB-4 | 2–4 |
| RT-11 | **BENCH-6**: 1,000 boardings at 300 m/s under 1 g, zero fall-throughs; transfer ≤ 1 tick, < 1 mm; `physics.collision_tile_miss` = 0 wherever the run touches a surface | W02 | 3 |
| RT-12 | **Frame (REF, BENCH-1)**: game thread ≤ 8 ms (p99 ≤ 12), OS thread ≤ 0.2 ms; 260 characters ≤ 2 ms; 40 UI surfaces ≤ 1.0 ms. A scripted 10 s window drag (Windows) keeps ticks and prediction running, with no tick gap > 1 frame. **MIN clause (H-class):** on the lab's two MIN boxes (09 §4.3.1): MIN-NV as built (Ryzen 5 3600, 12 threads) and MIN-AMD with SMT disabled in firmware (6 threads, 4 workers), which stands in for the i5-9400F. Both run Windows 10 22H2, and MIN-NV also runs Ubuntu 24.04. At 1080p Low with the MIN `cpu.*` settings, BENCH-1, 2 and 4 meet §2.4's MIN columns. The game thread holds its p50 total and p99 ≤ 16 ms, and the render thread ≤ 6 ms. Every per-system row is within 110 % of its core-ms. The whole process is within its total. ≤ 1 % of frames are CPU-bound (§2.4). **Landing fence case (MIN BENCH-2, both boxes; REF too):** the descent and landing run again with the client's collision prefetch throttled to 10 %, so the fence builds on many frames and reaches its cap. The fence stays ≤ 1.4 ms p99 wall on MIN and ≤ 1.0 ms on REF, and never exceeds its cap of 2 SERVER-core ms of builds per frame (≤ 2.8 core-ms as measured on MIN). When `C(b)` does not fit the cap, prediction suspends (`clientcore.prediction_suspended{reason=terrain}`) and resumes once the tiles are resident, with 0 terrain-caused corrections and 0 bodies below the collision surface. No frame that builds or suspends is CPU-bound, and the game thread holds p99 ≤ 16 ms on MIN and ≤ 12 ms on REF. MIN BENCH-2 is tracked nightly from Ph1, once the H1 lab lands (K7); a > 20 % breach for 5 nights opens a WP-3.4 item early. The clause **gates at the Ph3 exit** for BENCH-1, 2 and 4, a full phase before AAA-REN-2's Ph4 gate, and is re-run at Ph4 on final content. **30/45 fps clause (H-class):** REF BENCH-3 at 1440p High, and MIN BENCH-3 and BENCH-5 at 1080p Low on both MIN boxes, meet §2.4's other-rate columns. The game thread holds its p50 total with p99 ≤ 16 ms (REF BENCH-3) or ≤ 25 ms (MIN), and the render thread holds ≤ 5 ms or ≤ 7 ms. Every per-system row is within 110 % of its core-ms, and ≤ 1 % of frames are CPU-bound at 22.2 or 33.3 ms. REF BENCH-3 is tracked nightly from Ph2 (the WP-2.15 replay), and MIN BENCH-3 and 5 from Ph3. The clause gates at the Ph3 exit on the WP-2.15 capture and is re-run at Ph4 on WP-4.10's final capture. **120 fps clause (H-class):** REF BENCH-4 in Performance mode with the performance `cpu.*` settings. The game thread is ≤ 6.0 ms p50 on tick frames and ≤ 4.0 ms on others, with p99 ≤ 8.0 ms. The render thread is ≤ 3.5 ms, and ≤ 1 % of frames are CPU-bound at 8.33 ms. It is tracked nightly from Ph3, when BENCH-4 joins the lab, and **gates at the Ph4 midpoint**, ahead of AAA-REN-3 (03 RC-9) and CL-6's 120 fps gate at the Ph4 exit | AAA-REN-1, REN-2, REN-3 | 1–4 (MIN and 30/45 fps: 3; 120 fps: Ph4 midpoint) |
| RT-13 | **Script**: runaway killed at `fuel_kill` on cells (replay kills at the identical fuel count) and ≤ 5 ms wall on the client; `pcall` cannot swallow a kill. A kill inside a metamethod, inside a `table.sort` comparator and inside a Luau callback invoked from C++ unwinds cleanly: ASan-clean, no engine lock held, VM usable afterwards. An instrumented `lua_yield` shows no yield except at explicit calls, including when budgets run out inside those three places. A handle whose entity is destroyed across a `wait` raises `StaleHandle`. Heap caps; 10k coroutines per zone. **Binding-heavy case (Ph1):** a script that loops over `Physics.raycast` against a 200k-triangle soup, `overlapSphere` returning 256 entities, `World.spawn` of a 20-entity prefab, `string.rep` and `table.sort` of 1M elements (no comparator), with no Luau work between calls, runs for 1,000 ticks on the SERVER core at 20 and 60 Hz, with the three-kills rule suspended for the run. A second variant calls `task.checkpoint()` in every iteration and is deferred, never killed. In both, the lane's fuel never exceeds `fuel_per_tick` + one resume. Its wall time stays ≤ 1.25 × (`fuel_per_tick` + `fuel_kill`) × `ns_per_fuel` every tick. In the first variant the resume is flagged at `fuel_per_resume` and killed by a glue charge at `fuel_kill`, with **zero wall-backstop kills** and no side effect from the call that tripped the kill. A replay repeats the kill at the identical fuel count using the header's cost table, and still does so after `fuel_costs.jsonc` is recalibrated. Fuel counts are identical between interpreter and codegen | 06 §12.2 | 0–1 |
| RT-14 | **Iteration**: one gameplay `.cpp` edit → game DLL live-reloaded in the editor and PIE cell ≤ 30 s p95 on DEV (`windows-msvc-dev`; Linux `linux-dev` tracked), world state preserved; cell boot ≤ 5 s. **Depends on RT-18** | AAA-ITR-3/5 | 1 |
| RT-15 | **Localization**: 500k strings, ≤ 100 ns lookup, ≤ 48 MB/language | AAA-CNT-6 | 4 |
| RT-16 | **Crashes**: 100 % symbolicated dumps | AAA-STB-1 | 2 |
| RT-17 | **Paks**: 1 % change → ≤ 1.5× changed bytes downloaded; bad blocks re-fetched | AAA-CNT-7 | 2 |
| RT-18 | **Link-model spike (WP-0.6c)**: a game-DLL component on 10k live entities survives 100 reloads (code-only, add-field and remove-field edits) with an identical values checksum, memory tags back to baseline, and the old image unmapped. A CVar, a system, an observer, a Luau binding and a Tracy zone from the DLL all work after every reload. ASan is clean with MSVC and Clang. The DLL passes §1.4's symbol audit. The edit-to-reload time is ≤ 30 s p95 on DEV | AAA-ITR-5, PLT-1 | 0 |
| RT-19 | **Bubble seams**: 1,000 randomized trials of two ships closing at 1 km/s with the contact on a bubble seam. The contact count, pair and tick match a single-bubble control run exactly. The contact point is within 1 mm, the normal within 0.1° and the impulse and post-contact velocities within 0.5 %. The residue is f64 re-expression at the merge, not a missed contact. There are zero pass-throughs, and replays of the seam run are bit-exact. A ship at 1 km/s hitting a static asteroid that straddles a seam produces bit-identical contacts on the predicting client and the cell. So do a hull (and, from Ph2, a chassis) in contact with ≥ 3 terrain tiles at once for 600 steps, with the predictor's `BodyID`s allocated in a different order from the cell's and a client rollback re-simulating 10 ticks mid-run (§7.1). A merge or split of 2,000 bodies takes ≤ 2 ms. BENCH-3 shows zero `bubble_oversize` | W02 | 1 (fleet scale: 2) |
| RT-20 | **Surface collision load (§5.8a)**: the ground-hub profile (8 SERVER cores, 20 Hz) on Harrow's mountain and plains presets for 30 min, with 500 bots dispersed ≥ 1 km apart: 440 on foot, sprinting with random turns and jumps; 50 ground vehicles at 100 m/s; 10 ships in low flight at 300 m/s through valleys; 1 respawn or warp exit per second; and 2,000 NPCs in 50 lairs. Pass: tick p99 ≤ 35 ms (04 §3.3); collision-tile generation, fence builds included, ≤ 1,000 core-ms per second in every 10 s window; `TerrainFence` ≤ 8 core-ms of builds in every tick and ≤ 1.2 ms p99 wall (≤ 0.2 ms p99 on ticks that build nothing); `physics.collision_tile_miss` = 0, and `physics.collision_tile_sync` ≤ 1 per 1,000 body-ticks; `pcg.collision` ≤ 0.4 GB; `pcg.kernel=avx2` logged. Twenty of the bots run full client prediction: every terrain contact matches the cell bit for bit (0 corrections caused by terrain), and a probe finds 0 bodies below the collision surface. A 10 min replay of the run is bit-exact. **Fence-cap variant:** 5 min with prefetch throttled to 10 % and 20 respawns and warp exits in one second: the fence reaches its cap, ticks that hit it stay ≤ 1.2 ms p99 in the fence and ≤ 35 ms p99 overall, every body over the cap is held and logged as `CollisionHold`, no held body falls through, and the replay repeats each hold. **Ph1 slice:** 50 bots (44 on foot, 6 ships) on the same terms, with generation ≤ 250 core-ms/s | AAA-SRV-1, CNT-1; W04 | 1 (full: 2) |
| RT-21 | **Project types without C++ (§3.8)**: a dynamic package of 200 types (records; `ScriptState` components of 10–40 fields with lists, maps and optionals; replicated components; events; view-models) passes schemac's round-trip and keyed-merge tests through generic `TypeOps`. Its JSONC, tagged, cooked and network bytes equal those of the same package compiled native, on every determinism toolchain. Generic ops meet §3.8's targets on REF. In PIE, 10k entities carrying 3 dynamic components survive 100 data-only schema edits (added, removed, `@was`-renamed and widened fields) with an identical values checksum, and each save is live in ≤ 5 s p95 with no compiler present. The SDK's prebuilt `helios-backend` and collab service validate a 10k-record corpus of the package with 0 differences from the native Go validators | AAA-ITR-1, TOOL-9; 01 §1.1 | 2 |
| RT-22 | **Facial runtime (§7.2; 03 §7.6b)**, in the BENCH-1 dialogue bookmark: a T13-staged conversation with 8 F0 faces (2 captured hero heads and 6 DNA-blended, viseme-driven heads) in 2 languages, plus 32 F1 faces in view. **Sync:** the lip-to-audio offset (each viseme's onset in the displayed frame's weights, timed at M7, against its phoneme onset in the mixer's output-sample timestamps) is within ±40 ms p95 and never beyond +45/−125 ms (ITU-R BT.1359's detectability limits). **Eyes:** gaze converges on a new look-at target in ≤ 250 ms; blink rate is 10–20 per minute; saccade amplitude and rate stay within §7.2's ranges; with a camera still for 60 s, no face holds a fixed gaze for > 4 s. **Cost:** ≤ 0.05 ms per F0 face p95 and ≤ 0.8 core-ms in total on REF; the MIN box keeps 4 F0 faces within the same per-face bound; 03's GPU side is ≤ 0.15 ms (REF) and ≤ 0.1 ms (MIN). **Data paths:** the same thresholds hold with the audio-viseme fallback of 09 §4.3.3 (no captured clips), and a replayed dialogue produces bit-identical weights | R04; AAA-REN-8 (L5) | 4 |

### 8.3 Test strategy

- **Unit and schema tests.** Each module has doctest suites. schemac emits round-trip and keyed-merge tests
  for every type.
- **Determinism** (the five compiler families of ADR-001a rule 7, both MSVC toolsets included; nightly): physics, PCG plus the `hnoise` twin corpus, HXL, `det::`, `noise`,
  `Fixed64` and the 04 replay.
- **Fuzzing.** The JSONC, tagged, cooked and `.hpak` readers and the hot-reload protocol get ≥ 24 CPU-hours
  per release (AAA-SEC-7).
- **Benchmarks.** CTest `bench` runs nightly on REF and SERVER. A regression of more than 5 % fails, and RT-01
  and RT-12 gate releases.
- **Scenarios.** BENCH-2 and BENCH-6 flythroughs run with throttled I/O and injected misses. Bots cross every
  boundary type (R04 §9.5). Every BENCH and bot run asserts `physics.collision_tile_miss` = 0. A fault
  test throttles tile generation to force fence builds and holds, and checks that a held body never falls
  through, that the predicting client suspends instead of diverging, and that the replay repeats each
  `CollisionHold` (§5.8a). RT-20's fence-cap variant and RT-12's landing fence case time that path against
  the cell's and the client's caps.
- **Sanitizers.** ASan and UBSan nightly; TSan on jobs, ECS and streaming; MSVC ASan. Windows path, DPI and
  IME cases; MinGW.
- **Link model (ADR-016).** CI builds `windows-msvc-dev` (modular) and a monolithic release preset on every
  commit, so a missing `HELIOS_*_API` export fails the PR. `linux-dev` builds nightly. The symbol audit and a
  20-reload smoke test of the sample game module run on every PR, and the RT-18 soak of 100 reloads under ASan
  runs nightly.
- **Threads.** A client test drives a scripted window drag and resize through SDL's test video driver on
  Windows and asserts the RT-12 no-gap clause. TSan covers the OS→game input ring.
- **ISA levels.** The five §1.1 audit checks run on every PR for every image in both link flavours, with and
  without `HELIOS_PROFILE`, including check 3's four canary fixtures. The SDE and qemu emulator runs (CL-17)
  run nightly on Windows and Linux. They include the modular `HELIOS_PROFILE=ON` editor, PIE client, bot and
  `helios-tool` under SDE `-nhm` and `-snb` with the Helios DLLs chip-checked. Every smoke test asserts
  `helios_cpu_gate_verdict()` = pass. The nightly `sdk-consumer` job adds the object-level `avx2` check and
  check 3 on consumer-linked images.
- **Project types.** RT-21's dual-mode corpus (the same package compiled native and dynamic) runs on every PR,
  and the 100-edit PIE soak runs nightly. The generic `TypeOps` and the Go `htypes` interpreter share
  schemac's round-trip fixtures and are fuzzed with the other readers.
- **Seams.** RT-19's randomized seam trials run in the physics suite. A debug invariant checker asserts I1
  and I2 after every sync point in bots and BENCH runs.
- **Physics order.** RT-03's permuted variant and RT-19's multi-tile case run on every PR with the determinism
  hashes; debug builds assert unique `(layer, key)` pairs per `PhysicsSystem` (§7.1).
- **Script.** WP-0.10's corpus covers the RT-13 kill and yield cases, plus fuel identity between the
  interpreter and native codegen (04 §10.2), which passes once `codegen-fornloop-fuel` lands (the case is
  pinned today), and RT-13's ≤ 10 % overhead with `fuel-counter` (§7.4). The binding-heavy RT-13 case runs in WP-1.6 once `scriptlib`
  glue exists. `--calibrate-fuel --check` runs nightly on the SERVER lab node (§7.4), and a unit test asserts
  that every wrapped builtin appears in `disabledBuiltins`, so no call reaches it through `FASTCALL`.

### 8.4 Risks and mitigations

| Risk | Mitigation |
|---|---|
| flecs performance, fragmentation, single maintainer (R10 §14.2) | RT-01 in Phase 1; flecs confined to `engine/ecs`; non-fragmenting traits; custom ECS behind the same API. The Phase 0 pre-bench (`engine/ecs/SPIKES.md` §3) failed only the structural-ops clause, 3.4–6.7 ms for 9k ops against 1.5 ms, while raw flecs does the same ops in ≈ 0.9–1.7 ms: the cost is the wrapper's identity maps, structural log and command fusion, which a custom ECS would need too, so wrapper optimization comes first and is re-measured on SERVER (09 records the decision) |
| Grid-transition bugs (SC's longest-running class, R04 §2.3) | One transfer path, hysteresis, invariant checks, boarding bots |
| GPU/CPU terrain mismatch | Fixed-point `hnoise` with a Slang twin, a conformance corpus, CPU-tile fallback |
| Determinism regressions | `fp_control.h`, `det::`, sorted callbacks, hashes on every determinism toolchain (09 §6) |
| Jolt orders solver work by `BodyID`, so predictor, cell and replay diverge on multi-body contacts | Stable body keys and the `stable-order` patch; no cross-`Update` contact cache on ShipHull and Vehicle bodies; body tables with `CreateBodyWithID` for keyframes and residuals (§7.1); RT-03's permuted variant and RT-19's multi-tile case |
| Jolt float broadphase; many `PhysicsSystem`s | ≤ 20 km bubbles, re-centring, pooling, body caps |
| Contacts lost or doubled at bubble seams | Disjoint-cluster invariants I1 and I2 (§5.4), statics instanced per bubble, RT-19 seam trials, invariant checker in bots |
| Game-DLL reload corrupts memory or splits singletons | ADR-016 modular dev build with one CRT heap, single-image singletons, registration scopes, symbol audit, RT-18 spike in Phase 0; fallback is snapshot-and-restart iteration |
| Dev/ship divergence (`/MD` modular vs `/MT` monolithic) | Both flavours build on every commit. Shipping-flavour nightly runs every BENCH scene and RT criterion. No dev-only code paths except the loader |
| Script authors surprised by budgets | No involuntary yields; `ScriptOverBudget` warnings in the editor; explicit `task.checkpoint()`; sticky, deterministic kills; PIE shows cell fuel per binding |
| Bindings under-charged, so the lane overruns its tick share | Every Luau→C++ path is generated glue with a calibrated charge; nightly `--calibrate-fuel --check` (> 1.5× fails); live `script_ns_per_fuel` alert at 2×; `fuel_kill` capped at 25 % of the tick; RT-13 binding-heavy case |
| AVX2 code runs before the CPU gate (including third-party pre-`main` hooks such as mimalloc's TLS callback and Tracy's `.CRT$XCB` statics), reaches a `base` image through a COMDAT pick, or enters through an SDK consumer's module | Whole-image ISA levels (ADR-011 amendment): only the gate objects and the launcher are baseline. The gate runs first: the first TLS callback (`.CRT$XLA0`) of the first-initialized image on Windows, and `.preinit_array` on Linux. It uses no STL, defines no COMDAT and exits with `TerminateProcess`. The pre-gate audit enumerates TLS callbacks, `.CRT$XI*/XC*/XD*`, `_pRawDllMain`, the import-closure load order, IFUNCs, low-priority constructors and `.dynsym`, and runs seeded canaries. Post-link symbol mapping covers wide instructions in `base` images (RT-09). The CL-17 emulator runs include the modular profiling editor, and a runtime assert proves the gate ran. SDK targets export the `avx2` set, `sdk_config.h` enforces it, and `sdk-consumer` checks every object |
| Terrain collision tiles overload a cell or a client frame, or client and cell collide against different terrain | Velocity-scaled tile sets from body state, slab gate, prefetch budget with a degradation ladder and warm starts for spawns and warp exits, a synchronous fence with a cost cap (8 core-ms per cell tick, ≤ 1.2 ms wall; 2 per client frame, ≤ 1.4 ms on MIN) budgeted in §2.4 and 04 §3.3, no coarser substitution, logged holds and client prediction suspension (§5.8a); RT-20 at 500 dispersed players with a fence-cap variant; RT-12's landing fence case |
| Project types need C++ after all, or the generic path is too slow | Dynamic packages with generic `TypeOps`, `DynViewModel` and a Go interpreter (§3.8); byte-identical formats in both modes, so a package can switch to native at any time; `schema.dynamic-hot` CI budgets; RT-21 and 07 ED-22 |
| MIN CPU-bound in BENCH-1/2/4 found only at the Ph4 exit | §2.4 MIN tables; nightly MIN BENCH-2 from Ph1; RT-12 MIN clause gates at Ph3; `cpu.*` settings as the first lever, then per-system budgets |
| CPU cannot hold BENCH-3 at 45/30 fps or BENCH-4 at 120 fps | §2.4 tables for the 45, 30 and 120 fps columns; the tick/Phase A interleave and performance `cpu.*` settings; RT-12's 30/45 fps clause gates at Ph3 and its 120 fps clause at the Ph4 midpoint, before AAA-REN-2/3 and CL-6 at the Ph4 exit |
| Streaming stalls, including during streaming install | Lookahead, I/O budgets, residency priorities, HLOD fallback, flythrough CI |
| SWG terrain patents | Generic node graph; legal review before Phase 4 |

### 8.5 Traceability

| Requirement | § |
|---|---|
| R06-ENG-01, -02, -32; R05 §2.1; R10 §5 | 1 |
| R06-ENG-03, -16; R05-P0-1, P0-2; R01-P0-5 | 2 |
| R06-ENG-05; R03-P0-1, §2.4 (governed schema without a compile); R02-P0-4, P0-6; R04-P0-4, P0-9; R05-P0-7; R08-ED-P0-01, P0-02; R01-P1-16; 01 §1.1 (no C++ required) | 3 |
| R06-ENG-04, -06, -21; R04-P0-6, P1-16; R08-ED-P0-05 | 4 |
| R06-ENG-07, -20, -23, -25; R04-P0-1, P0-2, P0-3, P0-5, P1-12, P1-15, P2-24; R02-P0-5, P0-8; R08-ED-P0-07, P1-06, P1-07, P2-02 | 5 |
| R06-ENG-11, -12, -13; R01-P0-9; R03-P0-5, P1-6; R05-P0-10; R08-ED-P0-08, P0-16 | 6 |
| R06-ENG-28; R01-P0-4; R04-P1-18; R03-P0-9, P1-2; R08-ED-P0-10, P0-11, P1-08, P1-10, P1-12, P1-13, P2-03; R10 §4–8, §12 | 7 |

### 8.6 Cross-section alignment

| Section | What this section provides or adopts |
|---|---|
| 03 Rendering | Two-level transforms (§5.2); GPU terrain from `hnoise` (§5.8); `FrameChanged`; Gerstner buoyancy; portal and crowd-LOD contracts. `RenderScene` is defined in `render` and filled by `presentation`. The 8-lane CPU VM of 03 §5.5a is `pcg`'s default `vm_avx2.cpp` kernel, built at the image's `avx2` level (§1.1, §5.8). 03 §8.1.5's render CPU lines come from §2.4's tables, including the 45, 30 and 120 fps columns. 03 §7.6a–b consume §7.2's staggered A1/A2 palettes, the appearance skeleton (`BoneScale`) and the F0 face weights. Physics uses only collision-level tiles and never 03 §5.4's parent fallback (§5.8a) |
| 04 Networking | Field-level `Mut<C>` dirty bits; EntityId↔NetHandle; `ScriptState`; frame-local hitbox poses; §3.1 header sugar. Luau budgets: fuel metering with **no involuntary yields** and a sticky kill at `fuel_kill` (§7.4; 04 §3.1 and §10.2 follow). Voice: `engine/voice` client pipeline and the `PortalGraph` proximity helper for 04 §2.7; `helios-voice` build target. Per-call binding fuel with a calibrated table in the zone profile and replay header, `fuel_kill` ≤ 25 % of the tick, and the stage 4 Luau allowance (§7.4; 04 §3.3, §10.2). `TerrainFence` at the start of stage 3 (≤ 8 core-ms of builds per tick, ≤ 1.2 ms p99 wall) and the `CollisionHold` replay event (§5.8a; 04 §3.3, §10.2). Dynamic components replicate through `repl::describeDynamic` (§3.8). Jolt's solver order by stable body keys, the no-cross-`Update` cache rule and body tables for `RestoreState` (§7.1; 04 §5.3, §6.7, §10.2); the replay-keyframe script rebase and the `codegen-fornloop-fuel` and `fuel-counter` Luau patches (§7.4; 04 §10.2) |
| 05 Backend | `service` blocks, `@ledger_policy`, `@lifecycle`, `ReasonCodeDef`, `@table`/`@sql`, proto/Go/C++/NATS emitters. Dev database is embedded-postgres (ADR-014, 05 §3.5); no service uses SQLite. `services/pkg/htypes` interprets dynamic project packages, so the prebuilt backend and the collab service validate project types (§3.8; 05 §1.14) |
| 06 Gameplay | §3 is normative. `hmath` means `helios::det` |
| 07 Editor | `EDITOR_ONLY`, `@keyed`, minted `$rid`, editor metadata, Go validators, DAP in `engine/script`; the planet library is `engine/pcg` (07 uses this name). **Adopted from 07:** assetd transport over a named pipe or Unix socket (07 §3.1), local DDC at 200 GB (07 §3.2), and the SQLite authoring registry (07 §3.4). 02 keeps the DDC key, the cooked formats and `registry.hreg`. Game-DLL reload (§1.4) backs 07 §1.6's hot swap. Dynamic project packages (§3.8) back T08's governed schema editor and ED-22; T04's budget analyzer checks the collision interpolation bound and `slabMargin` (§5.8, §5.8a) |
| 08 Client | Whole-image ISA levels with a baseline gate TU that runs first: the first TLS callback (`.CRT$XLA0`) of the first-initialized image on Windows, ahead of mimalloc's and Tracy's hooks, and `.preinit_array` on Linux (§1.1; 08 §2.2 adds BMI2 to the gate and now cites this placement); the prebuilt stamped client runs project types from `.htypes` data (§3.8; 08 §2.10.4); tagged ≤ 2 GiB paks with block checksums, residency hook, per-zone PSO lists, `engine/ui` and `engine/text`. **Adopted from 08:** the OS/game/render thread model (08 §1.3) and the settings scopes and CVar flag names (08 §1.4) |
| 09 Roadmap | RT-01 gates the ECS decision; approve libopus; implement the `LAYER` and `EDITOR_ONLY` checks; the WP-0.6c link-model spike (RT-18), which also delivers `HELIOS_MODULAR` and the `windows-msvc-dev` and `linux-dev` presets, gates RT-14 in WP-1.4; WP-0.10 carries RT-13's yield and kill tests; WP-1.2 carries RT-19; WP-0.2r reworks the in-tree ISA code to §1.1's image levels and five-check audit, and WP-0.5r moves the in-tree gate to `.CRT$XLA0` (09 §5.10.4 lists the deltas: the per-file allowlist, `tp_jolt`'s PUBLIC ISA options, the `.CRT$XIB` hook); WP-0.9c measures the `avx2` kernel; WP-1.3 carries RT-20's Ph1 slice and WP-2.2 its full run; WP-2.16c1–c3 and WP-2.16d carry the dynamic packages (RT-21) and WP-2.16e 07's ED-22 (09 §2.3a); WP-1.6 builds binding fuel (RT-13 binding case); WP-3.4 carries RT-12's MIN clause (Ph3) |
