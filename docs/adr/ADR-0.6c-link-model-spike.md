# ADR-0.6c: link-model spike (modular dev builds, link groups, symbol audit)

| | |
|---|---|
| **Status** | **Part 1 of 2 recorded (2026-10-04).** The modular dev build exists: `HELIOS_MODULAR`, the three link-group shared libraries with generated `HELIOS_*_API` headers, `/MD` for every image of a modular MSVC build, the `windows-msvc-dev` and `linux-dev` presets, the symbol audit and the `windows-msvc-dev` PR job. **RT-18 is unmeasured**: the reload loader, the `Probe` game module and the 100-reload soak are part 2. ADR-016 stands; nothing here reopens it |
| **Decides** | How [ADR-016](../plan/00-decisions.md#adr-016-dev-versus-shipping-link-model-game-module-hot-reload) and [02 §1.4](../plan/02-engine-runtime.md#14-link-model-and-game-module-hot-reload-adr-016) are built: what each group exports, how consumers link, where the CPU gate lives in a modular build, which toolchains build the dev flavour |
| **Gates** | RT-18 (02 §8.2), and through it RT-14 and AAA-ITR-5. Part 1 delivers RT-18's prerequisites (the modular link model and the symbol audit) |
| **Owner** | Runtime lead. Work package WP-0.6c ([09 §2](../plan/09-roadmap-and-process.md), WP-0.6 (c)) |
| **Evidence** | `cmake/HeliosModular.cmake`; `tools/lint/symbol_audit.cmake` and its fixtures; `tests/link_model/` (`link_model_tests`); the `windows-msvc-dev` job of `.github/workflows/ci.yml` |

## 1. What part 1 built

**Option and presets.** `HELIOS_MODULAR` (default `OFF`). `OFF` is the shipping flavour and is unchanged:
every module is a static library, MSVC links `/MT` with `/OPT:REF /OPT:ICF`. `ON` is the dev flavour. The
presets are `windows-msvc-dev` (Ninja, the prompt's `cl`, RelWithDebInfo) and `linux-dev` (Clang, from
`linux-clang`). Both have build and test presets.

**Link groups.** A module's group follows from its row in `engine/CMakeLists.txt`, so the table in 02 §1.4
needs no second list:

| Group | Rule | Modules today |
|---|---|---|
| `helios_runtime` | every HEADLESS module | core, math, reflect, hxl, asset, records, ecs, physics, pcg, script, net, gameplay, authority, server |
| `helios_client` | neither HEADLESS nor EDITOR_ONLY | rhi, render |
| `helios_editor` | every EDITOR_ONLY module | assetpipe, toolsfw, editorui |

In a modular build `helios_module()` makes the module an `OBJECT` library linked into its group's shared
library. `helios::<module>`, which every consumer already links, then names an `INTERFACE` target that gives
the module's compile requirements (`$<COMPILE_ONLY:…>`, never its objects) and links the group library. A
module of the same group gets only the compile requirements, which keeps groups free of self-links and
cycles. Each third-party library a module links ends up in exactly one group.

**Layering in both flavours.** The configure-time checks map `helios::<module>` back to the module, so they
see the same graph in both flavours. A modular build adds one rule: only a group library (or a declared
build-time tool, below) may link a module's object library directly, because anything else would carry a
second copy of the module's code and state. The layering fixtures run in both flavours.

**Build-time tools.** `helios-schemac` runs during the build and generates `gameplay`'s sources. Linked
against `helios_runtime`, which contains `gameplay`, it would form a target cycle. `helios_standalone_tool()`
links the modules such a tool needs (here only `core`) into the tool itself.

**MSVC runtime and link.** `CMAKE_MSVC_RUNTIME_LIBRARY` is `MultiThreaded[Debug]DLL` before any target
exists, so every image of a modular build uses `/MD`, the vendored libraries included. No vendored library
forces its own runtime: Luau's `LUAU_STATIC_CRT` defaults to `OFF`, SDL3 keeps `SDL_LIBC=ON`, and the others
are plain targets. The dev link flags are `/INCREMENTAL /OPT:NOREF /OPT:NOICF /DEBUG:FULL`; the shipping
`/OPT:REF /OPT:ICF` is no longer added in a modular build.

**Linux.** Everything that is not module code (tools, tests, apps, fixtures) compiles with
`-fvisibility=hidden -fvisibility-inlines-hidden`. Module code compiles with default visibility and
`-fvisibility-inlines-hidden`. Group libraries link with `-z defs` (not under sanitizers, whose runtimes live
in the executables) and `--exclude-libs,ALL`. lld is used when the toolchain has it.

**MinGW.** The cross build stays a check of the shipping flavour; `HELIOS_MODULAR=ON` with MinGW stops
configure. The dev flavour's Windows behaviour is MSVC's, which the `windows-msvc-dev` job builds and tests.

## 2. Decision: what a group exports

**Every external symbol of the group's own objects, nothing from its third-party archives.** On MSVC this is
CMake's `WINDOWS_EXPORT_ALL_SYMBOLS`, which scans only the target's own objects (the modules' object files),
never the archives it links. On ELF it is default visibility for module code plus `--exclude-libs,ALL`.
`HELIOS_<GROUP>_API` (generated as `helios/<group>_api.h`) is required where a declaration needs it: data
that code outside the group reads (MSVC imports data only through `__declspec(dllimport)`) and C entry
points. It is `dllexport` while building the group, `dllimport` when consuming it, `visibility("default")`
on ELF, and empty in shipping builds.

**Why not explicit exports on every declaration now.** 02 §1.4 and the brief ask for `-fvisibility=hidden`
with explicit exports. That means annotating every exported class and function in about 170 public headers
across every module. On MSVC a `dllexport` class also forces the definition of every special member,
including implicitly declared copy constructors of classes with move-only members, and raises C4251 for
every STL member. Neither can be checked without MSVC, and the edit would touch every module's headers while
other work packages change them. The export-everything rule is the conservative first step: it cannot miss
an export, and the audit (section 4) keeps third-party code out of the export tables. Tightening to explicit
exports, module by module, is follow-up work once part 2 shows which symbols game modules actually import.

**Measured export counts** (GCC 13, `linux-dev` settings; MSVC counts come from the CI job's audit step):
EXPORT_COUNTS. The PE format allows 65,535 exports per image.

## 3. Decision: the CPU gate in modular builds

- **Windows:** the gate hook object moves into `helios_runtime.dll` (02 §1.1, "Which image"). A gated
  executable carries no hook; it gets `/INCLUDE:helios_cpu_gate_run`, so it always imports the DLL even when
  its own code would not, and the loader initializes the DLL before any code of the executable.
- **Linux:** the hook stays in each gated executable. `.preinit_array` exists only in executables, and glibc
  runs it before the initializers of every shared object, `libhelios_runtime.so` included. The hook calls
  the probe, `helios_cpu_gate_run`, which now lives in `libhelios_runtime.so`; the call happens after
  relocation and before any initializer.

**What WP-0.5r must change on top** (09 §5.10.4, row "Where the gate is linked"):
1. The Windows hook in `helios_runtime.dll` still sits in `.CRT$XIB`, so inside the DLL it ties with
   mimalloc's `.CRT$XIB` initializer and runs after mimalloc's `.CRT$XLB` TLS callback, as in a monolithic
   image. WP-0.5r's move to `.CRT$XLA0` applies unchanged to the DLL.
2. In the DLL the failure path runs inside `DllMain` under the loader lock, so `ExitProcess` is even more
   hazardous there than in an executable: WP-0.5r's `TerminateProcess(…, 78)` is required for modular
   builds.
3. Audit check 3 (d), "the gate image imports only OS and Microsoft runtime DLLs", must accept the VC++
   runtime DLLs for `helios_runtime.dll` (it is `/MD`), and check that every Helios DLL imports it.
4. On Linux the gate objects call `helios_cpu_gate_run` across an image boundary (PLT). WP-0.5r should link
   the probe object into gated executables too, or keep the cross-image call and add it to the gate
   allowlist; `helios_cpu_gate_verdict()` must then read the verdict the hook wrote, which is the
   executable's copy in the first case.
5. `helios_cpu_gate()` stays internal to `cmake/HeliosIsa.cmake` and keeps both branches.

## 4. The symbol audit

`tools/lint/symbol_audit.cmake` (CTest `lint_symbol_audit`, label `lint`) reads the images of a modular build.
ELF: `nm`; PE: `dumpbin /exports`, because a linked PE image has no symbol table.

| Rule | What fails |
|---|---|
| R1 | A group exports a strong definition outside namespace `helios` and the `helios_` C prefix |
| R2 | A third-party library with process state (mimalloc, flecs, Jolt, Luau, Tracy, SDL3, Dear ImGui, volk) is defined in two images |
| R3 | A consumer image has its own copy of mutable `helios` data that a group defines (02 §1.4, "No per-image caches of global state") |
| R4–R6 | A game image defines anything from flecs, Jolt, Luau, mimalloc or Tracy; defines mutable `helios` data; or carries a global strong copy of an exported engine function |
| P1 | A group's export table has an undecorated name that is not `helios_*` |

Fixtures: recorded `nm` and `dumpbin` listings (`tools/lint/tests/symbols/`) run in every build; ELF modular
builds also build `link_model_bad_game`, which breaks R4–R6 on purpose. `link_model_probe` is the passing game
image.

**Limits, for part 2.** R6 sees only global definitions in a linked image: a hidden strong copy and a hidden
inline instantiation look the same there. Part 2 audits the game module's object files (`nm`, `dumpbin
/symbols`), where the two differ. The consumer and game rules run on ELF only; on Windows they need the game
module's objects too.

## 5. Findings

FINDINGS

## 6. Part 2

Part 2 adds the reload loader, the `Probe` game module (`game_probe`, replacing `link_model_probe`) and RT-18's
measurements: 100 reloads on 10k entities, the values checksum, memory tags back to baseline, the unmapped old
image, ASan on both toolchains, the 20-reload PR smoke test (the `windows-msvc-dev` job carries a TODO for
it) and the edit-to-reload time on DEV. It also switches the IDE presets (`windows-vs2026`,
`windows-vs2022`) to `HELIOS_MODULAR=ON` (ADR-001a rule 5) once the modular MSVC build is proven in CI, and
adds `linux-dev` to the nightly tier (02 §8.3).
