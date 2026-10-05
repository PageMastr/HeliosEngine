# ADR-0.6c: link-model spike (modular dev builds, link groups, symbol audit)

| | |
|---|---|
| **Status** | **Part 1 of 2 recorded (2026-10-04).** The modular dev build exists: `HELIOS_MODULAR`, the three link-group shared libraries with generated `HELIOS_*_API` headers, `/MD` for every image of a modular MSVC build, the `windows-msvc-dev` and `linux-dev` presets, the symbol audit and the `windows-msvc-dev` PR job. **RT-18 is unmeasured**: the reload loader, the `Probe` game module and the 100-reload soak are part 2. ADR-016 stands; nothing here reopens it. Two decisions departed from the earlier letter of 02 §1.4: what a group exports (§2) and where SDL3 and Dear ImGui live (§5, finding 7). This PR amends 02 §1.4 (the export policy until part 2, the Linux row, the third-party images) and 02 §1.1 ("Which image") to match, and gives the export tightening to part 2 |
| **Decides** | How [ADR-016](../plan/00-decisions.md#adr-016-dev-versus-shipping-link-model-game-module-hot-reload) and [02 §1.4](../plan/02-engine-runtime.md#14-link-model-and-game-module-hot-reload-adr-016) are built: what each group exports, how consumers link, which images carry their own copy of the engine, where the CPU gate lives in a modular build, which toolchains build the dev flavour |
| **Gates** | RT-18 (02 §8.2), and through it RT-14 and AAA-ITR-5. Part 1 delivers RT-18's prerequisites (the modular link model and the symbol audit) |
| **Owner** | Runtime lead. Work package WP-0.6c ([09 §2](../plan/09-roadmap-and-process.md), WP-0.6 (c)) |
| **Evidence** | `cmake/HeliosModular.cmake`; `tools/lint/symbol_audit.cmake` and its fixtures; `tests/link_model/` (`link_model_tests`); the layering fixtures in both flavours (`lint_layering_*`, `lint_layering_modular_*`); the `windows-msvc-dev` job of `.github/workflows/ci.yml` |

## 1. What part 1 built

**Option and presets.** `HELIOS_MODULAR` (default `OFF`). `OFF` is the shipping flavour and is unchanged:
every module is a static library, MSVC links `/MT` with `/OPT:REF /OPT:ICF`. The generated export headers
exist in both flavours, and their macros expand to nothing in shipping builds. `ON` is the dev flavour. The
presets are `windows-msvc-dev` (Ninja, the prompt's `cl`, RelWithDebInfo) and `linux-dev` (Clang, from
`linux-clang`, as 02 §1.4's table says). Both have build and test presets.

**Link groups.** A module's group follows from its row in `engine/CMakeLists.txt`, so 02 §1.4's table needs
no second list. `EDITOR_ONLY` is tested first, so an editor-only module can never land in `helios_runtime`,
which every server links.

| Group | Rule | Modules today |
|---|---|---|
| `helios_runtime` | HEADLESS and not EDITOR_ONLY | core, math, reflect, hxl, asset, records, ecs, physics, pcg, script, net, gameplay, authority, server |
| `helios_client` | neither HEADLESS nor EDITOR_ONLY | rhi, render |
| `helios_editor` | EDITOR_ONLY | assetpipe, toolsfw, editorui |

In a modular build `helios_module()` makes the module an `OBJECT` library linked into its group's shared
library. `helios::<module>`, which every consumer already links, then names an `INTERFACE` target that gives
the module's compile requirements (`$<COMPILE_ONLY:…>`, never its objects) and links the group library. A
module of the same group gets only the compile requirements, which keeps groups free of self-links and
cycles. Each third-party library a module links ends up in exactly one image.

**Layering in both flavours.** The configure-time checks map `helios::<module>` back to the module, so they
see the same graph in both flavours. A modular build adds one rule: only a group library or a
self-contained image (below) may link a module's object library directly, because anything else would carry
a second copy of the module's code and state. Every layering fixture runs in both flavours
(`lint_layering_<case>` and `lint_layering_modular_<case>`), and `lint_layering_modular_direct_objects`
seeds the new rule.

**Self-contained images.** `helios_self_contained(<target>)` gives an image its own copy of every module it
reaches (objects and third-party libraries, as in a shipping build) instead of the group libraries. The
module closure is computed at the end of configure. Two kinds of image need it:
- `helios-schemac`, which runs during the build and generates `gameplay`'s sources. Linked against
  `helios_runtime`, which contains `gameplay`, it would form a target cycle.
- White-box tests and benches that call a group's third-party library directly: `ecs_tests` and
  `ecs_bench` (flecs and mimalloc), `script_tests` (Luau's internals), `net_tests` and the `net_fuzz_*`
  targets (netcode), and `schemac_tests` (Luau.Analysis links its own Luau VM and compiler). A group
  hides its third-party code (§2), so these cannot link against it, and a second copy of flecs, Luau or
  netcode beside the group's would split its state.

Such an image shares nothing with the groups, so it tests its modules as a shipping image would, and the
symbol audit skips it. Every other test and tool links the groups.

**MSVC runtime and link.** `CMAKE_MSVC_RUNTIME_LIBRARY` is `MultiThreaded[Debug]DLL` before any target
exists, so every image of a modular build uses `/MD`, the vendored libraries included. No vendored library
forces its own runtime: Luau sets CMP0091 itself and its `LUAU_STATIC_CRT` defaults to `OFF`, SDL3 requires
CMake 3.16 (CMP0091 `NEW`), and the other vendored libraries are targets of `third_party/CMakeLists.txt`,
which inherits the top-level setting. The dev link flags are `/INCREMENTAL /OPT:NOREF /OPT:NOICF
/DEBUG:FULL`; the shipping `/OPT:REF /OPT:ICF` is not added in a modular build.

**Linux.** Everything that is not module code (tools, tests, apps, fixtures) compiles with
`-fvisibility=hidden -fvisibility-inlines-hidden`. Module code compiles with default visibility and
`-fvisibility-inlines-hidden` (§2). Group libraries link with `-z defs` (not under sanitizers, whose
runtimes live in the executables) and `--exclude-libs,ALL`. lld is used when the toolchain has it.
`-gsplit-dwarf` is used with Clang only: GCC splits with an `objcopy` pass after compiling, which failed
intermittently under ccache here.

**MinGW.** The cross build stays a check of the shipping flavour; `HELIOS_MODULAR=ON` with MinGW stops
configure. The dev flavour's Windows behaviour is MSVC's, which the `windows-msvc-dev` job builds and
tests. clang-cl takes the MSVC path (`WINDOWS_EXPORT_ALL_SYMBOLS`, `/MD`), but no job builds it modular yet.

## 2. Decision: what a group exports

**Every external symbol of the group's own objects, nothing from its third-party archives.** On MSVC this is
CMake's `WINDOWS_EXPORT_ALL_SYMBOLS`, which scans only the target's own objects (the modules' object files),
never the archives it links. On ELF it is default visibility for module code plus `--exclude-libs,ALL`.
`HELIOS_<GROUP>_API` (generated as `helios/<group>_api.h`) is required where a declaration needs it: data
that code outside the group reads (MSVC imports data only through `__declspec(dllimport)`) and C entry
points. It is `dllexport` while building the group, `dllimport` when consuming it, `visibility("default")`
on ELF, and empty in shipping builds. Exports are therefore explicit per namespace (namespace `helios` and
the `helios_` C prefix, which the audit enforces: R1, P1) and per declaration only for data.

**One third-party library is exported: Luau's VM.** `engine/script`'s public API is built on the Luau C API
(`helios/script/binding.h` includes `lua.h`), `toolsfw` in `helios_editor` binds its automation functions to
it directly, and the glue that `helios-schemac` generates calls it. The VM must stay one copy, so
`helios_runtime` links Luau.VM's objects as its own and exports them (`HELIOS_GROUP_EXPORTED_THIRD_PARTY`
in `cmake/HeliosModular.cmake`). The audit accepts exactly what the Luau.VM archive defines (R1). That
includes Luau's `FFlag`/`FInt` globals, which also have to be one copy. Every other vendored library stays
hidden in its group.

**Why not `-fvisibility=hidden` with an annotation on every exported declaration now.** The earlier text of
02 §1.4 and the brief asked for it. It means annotating every exported class and function in the 171 public headers of
every module, plus the internal headers that white-box tests include. On MSVC a `dllexport` class also
defines every special member, including implicitly declared copy constructors of classes with move-only
members (C2280), and raises C4251 for every STL member. Neither can be checked without MSVC, and the edit
would touch every module's headers while other work packages change them. The export-everything rule cannot
miss an export, and the audit keeps third-party code out of the export tables. Tightening to explicit
exports, module by module, follows once part 2 shows which symbols game modules import. 02 §1.4 (*Exports*
and the Linux row of the flavour table) now states this policy and names WP-0.6c part 2 as the owner of the
tightening.

**Data that crosses an image boundary.** On Windows, data needs `dllimport` on the consumer side, so every
variable that code outside its group reads carries the group macro. A scan of the GCC modular build (each
image's undefined and copy-relocated Helios data symbols, resolved against the image that exports them)
finds three today: `log::detail::g_globalLevel`, which every image's `log::isEnabled` reads, and
`edui::detail::kRobotoRegular` and `kRobotoRegularSize`, which `editorui_tests` reads. All three carry
their group's macro. A missing one is an MSVC link error (`LNK2019` on `__imp_…`), so the `windows-msvc-dev`
job catches the next one. The same applies to a function whose address is compared across images: without
`dllimport`, `&f` in a consumer is its own import thunk, not the DLL's function. The first MSVC test run
found one (`core_tests` compares `assertHandler()` with `&defaultAssertHandler`), which now carries
`HELIOS_RUNTIME_API`. Log channels that a header outside a module's sources names (the core channels,
`LogEcs`, `LogTools`, and `LogRhi`, whose private header `rhi_tests` includes) are declared with
`HELIOS_LOG_CHANNEL_EXTERN` and defined in one `.cpp`; `HELIOS_LOG_CHANNEL` stays for channels of a
module's own sources (an inline variable, so one copy per image).

**Export counts** (`HELIOS_MODULAR=ON`, RelWithDebInfo; `nm -D --defined-only`): GCC 13: `helios_runtime`
4,474 (271 of them Luau's VM), `helios_client` 349, `helios_editor` 485, SDL3 1,272, Dear ImGui 4,316;
Clang 18: 4,944 (272 Luau), 407 and 585.
MSVC 14.51 (`windows-msvc-dev` job): `helios_runtime` 10,084, `helios_client` 1,817, `helios_editor` 2,002;
`WINDOWS_EXPORT_ALL_SYMBOLS` also exports the inline and template instantiations of the group's objects.
The PE format allows 65,535 exports per image, so the largest group uses about 15 % of it.

## 3. Decision: the CPU gate in modular builds

- **Windows:** the gate hook object moves into `helios_runtime.dll` (02 §1.1, "Which image"). A gated
  executable carries no hook; it gets `/INCLUDE:helios_cpu_gate_run`, so it always imports the DLL even when
  its own code would not, and the loader initializes the DLL before any code of the executable. A
  self-contained gated image loads no group, so it links the hook itself, as a shipping image does.
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
   runtime DLLs for `helios_runtime.dll` (it is `/MD`), and check that every Helios DLL imports it. SDL3 and
   Dear ImGui are DLLs of their own in modular builds (§5), and neither imports `helios_runtime.dll`, so the
   loader may initialize them first. Once WP-0.2r builds them at `avx2`, check 3 must either attribute their
   initializers (as it does mimalloc's) or the build must make them import `helios_runtime.dll`.
4. On Linux the gate objects call `helios_cpu_gate_run` across an image boundary (PLT). WP-0.5r should link
   the probe object into gated executables too, or keep the cross-image call and add it to the gate
   allowlist; `helios_cpu_gate_verdict()` must then read the verdict the hook wrote, which is the
   executable's copy in the first case.
5. `helios_cpu_gate()` stays internal to `cmake/HeliosIsa.cmake` and keeps both branches; the
   self-contained branch is in `cmake/HeliosModular.cmake`.
6. **IFUNCs in the group libraries.** The ISA audit bans `R_X86_64_IRELATIVE` relocations in gated
   executables, because an IFUNC resolver runs at relocation time, before `.preinit_array`. In a modular ELF
   build the engine code sits in `libhelios_*.so` (and SDL3 and ImGui in their own libraries), which the
   dynamic linker also relocates before the gate. None has an IRELATIVE relocation today (`readelf -r` on
   the GCC and Clang modular builds); WP-0.5r adds the group and third-party libraries to the image check.
7. **Which processes are gated.** In a modular Windows build the hook runs in every process that loads
   `helios_runtime.dll`, including images whose role is `NO_CPU_GATE` or baseline: tests, samples and the
   future launcher, whose refusal screen 02 §1.1 wants reachable on an old CPU. Shipping launchers are
   monolithic, so that screen is unaffected; a modular launcher stops at the gate like any gated image.
   On Linux only gated executables carry the hook (`.preinit_array`), so the role still decides there.
   WP-0.5r records this in the gate's role rules.

## 4. The symbol audit

`tools/lint/symbol_audit.cmake` (CTest `lint_symbol_audit`, label `lint`) reads the images of a modular build.
ELF: `nm`; PE: `dumpbin /exports`, because a linked PE image has no symbol table.

| Rule | What fails |
|---|---|
| R1 | A group exports a strong definition, or mutable data of any binding (weak and GNU-unique inline and template statics included), outside namespace `helios` and the `helios_` C prefix |
| R2 | A third-party library with process state (mimalloc, flecs, Jolt, Luau, Tracy, SDL3, Dear ImGui, volk, netcode) is defined in two images |
| R3 | A consumer image, or a second group, has its own copy of mutable `helios` data that a group defines (02 §1.4, "No per-image caches of global state"). Two groups export such data with default visibility, so Linux binds both to one instance and only Windows splits it: the audit compares the groups' symbol tables. Data that an executable imports by copy relocation is in both images' dynamic symbol tables and is one instance, not a copy |
| R4–R6 | A game image defines anything from flecs, Jolt, Luau, mimalloc or Tracy; defines mutable `helios` data; or carries a global strong copy of an exported engine function |
| P1 | A group's export table has an undecorated name that is not `helios_*`, or no exports at all. The C names that MSVC's CRT and STL headers define inline or as `selectany` data (`fprintf`, `snprintf`, `__local_stdio_printf_options`, `__std_*`, `_Avx2WmemEnabledWeakValue`, …) are accepted: every object that uses them has a copy, and each image still calls its own over the one `/MD` CRT (policy `HELIOS_SYMBOL_PE_TOOLCHAIN`) |

02 §1.4 has no rule against STL types in signatures across the boundary: every image shares one CRT heap,
so STL objects may cross it (`link_model_tests` checks this), and the audit has no such rule either.

Fixtures: recorded `nm` and `dumpbin` listings (`tools/lint/tests/symbols/`, 12 cases, 9 of them seeded
failures, including `elf_group_duplicate_state` and `elf_group_weak_state`) run in every build; ELF modular
builds also build `link_model_bad_game`, which breaks R4–R6 on purpose. `link_model_probe` is the passing
game image.

**Where each rule runs.** R1–R6 need an ELF modular build. No CI job runs one yet: `windows-msvc-dev` runs P1
and the recorded fixtures, and `linux-dev` joins the nightly tier with part 2 (§6). Until then R1–R6 run
locally, in every `linux-dev` build (`ctest -L lint`).

**Limits, for part 2.** R6 sees only global definitions in a linked image: a hidden strong copy and a hidden
inline instantiation look the same there. Part 2 audits the game module's object files (`nm`, `dumpbin
/symbols`), where the two differ. The consumer and game rules run on ELF only; on Windows they need the game
module's objects too. P1 checks undecorated names only: `WINDOWS_EXPORT_ALL_SYMBOLS` also exports the
inline and template instantiations that the group's objects contain (STL ones included). Those are stateless
copies of header code, and R3's ELF run covers the stateful ones. Because P1 skips every decorated name,
third-party C++ compiled into a module's own objects (finding 4's VMA case) is invisible on PE; R1 on the
ELF build is the check for it.

## 5. Findings

What the first modular builds of the tree showed (GCC and Clang on Linux; MSVC through CI):

1. **White-box tests reach into third-party internals.** `ecs_tests` calls flecs and mimalloc, `script_tests`
   Luau's internals, `net_tests` netcode, and `schemac_tests` links Luau.Analysis, which brings its own VM.
   A group hides those libraries, so these images became self-contained (§1). The link model itself is
   exercised by `link_model_tests` and by every other test, which links the groups.
2. **Luau's C API crosses groups** (§2). This also bears on part 2: 02 §1.4 says Luau headers never reach
   game code, but `helios-schemac`'s Luau emitter generates binding glue that includes `lualib.h`. Part 2
   must either route generated game-module glue through `engine/script`'s binder or allow game modules to
   import (never define) Luau symbols, which R4 already permits.
3. **Consumers need the groups their groups need.** `render_tests` calls `core` directly but named only
   `helios::render`. A shared library's own dependencies are not on its consumers' link line (ELF does not
   resolve through `DT_NEEDED`; PE imports come from the import libraries on the link line), so each group
   passes on the groups its modules depend on.
4. **Third-party code compiled inside a module leaks into the exports.** VMA's implementation TU sits in
   `engine/rhi/src` and made `helios_client` export 373 VMA symbols (R1 on ELF; on MSVC its `vma*` C names
   would fail P1). In modular builds it is now an archive of its own inside the group.
5. **Header-defined state.** R3 found two kinds:
   - a log channel in `rhi`'s private header, which `rhi_tests` includes, so the test image registered a
     second "RHI" channel. It is now defined once and exported;
   - `engine/reflect`'s container TypeInfos (`TypeOf<std::vector<E>>`, `std::optional<E>`, …), built in
     function-local statics of header templates: 20 copies in 10 consumer images today. Two TypeInfo
     addresses for one type break identity checks across images, and a TypeInfo built inside a game module
     dies with it on unload. This is a known finding of the audit (policy `HELIOS_SYMBOL_KNOWN_FINDINGS`),
     owned by part 2: the TypeInfos must come from the registry in `helios_runtime` before the `Probe`
     module registers reflected types.
6. **Copy relocations and constants.** GCC's PIE executables import exported data by copy relocation, so
   the executable holds the one instance; R3 treats those as imports. Vtables, typeinfo and `constexpr`
   `string_view`s are per image but sit in `.data.rel.ro` and are not state; the audit reads sections
   (`nm -f sysv`) to tell them apart.
7. **SDL3 and Dear ImGui are shared libraries of their own** in modular builds, where 02 §1.4's table puts
   them inside `helios_client` and `helios_editor`. Their state must be one copy (SDL's video subsystem,
   windows and events; ImGui's `GImGui`), and both are called from more than one group and from apps and
   tests. Their own DLLs export them through the libraries' own macros (`SDL_DECLSPEC`, `IMGUI_API`), which
   a group's export-all scan of its own objects would not do for an archive. ImGui's Vulkan backend is left
   out of the modular build: the editor draws ImGui through the Helios RHI, and the backend would carry a
   second copy of volk. The goal of the table, one copy of each library, holds.
8. **Exceptions across images.** Luau raises errors as C++ exceptions. They are thrown and caught inside
   `helios_runtime` (`luaD_throw`, `luaD_rawrunprotected`) but unwind through binding frames in other images
   (`toolsfw`'s automation bindings). One CRT and `/EHsc` everywhere make that safe; the throw and catch
   sites stay in one image.
9. **The first MSVC modular build linked cleanly.** The data imports found by the ELF scan were all it
   needed, and `WINDOWS_EXPORT_ALL_SYMBOLS` works over linked object libraries (CMake's Ninja generator
   passes their objects to its export scan). Two things showed up only on Windows: the CRT's and STL's
   header inlines with C linkage in the export tables (P1 above), and one function-address comparison
   across images (§2). 307 of 308 tests passed on the first run, and all 308 (plus the 11 symbol-audit
   tests) after that fix.
10. **Not verified here.** MSVC is built and tested only by the `windows-msvc-dev` CI job, including the DLL
   search at test time (every image is written to `bin/`).
11. **ECS type ids were a per-image cache** (found in review). `World::id<T>()`, `replicationBit<T>()`,
   `registerComponent<T>()`, `bindType<T>()`, `CommandBuffer` and `SystemBuilder` indexed a per-World table by
   `typeSlot<T>()`, a number drawn into a function-local static of a header template. Each image outside
   `helios_runtime` had its own static, drew its own number, and found nothing that `helios_runtime` had
   bound: `world.id<NetIdentity>()` returned 0 in `link_model_tests`. No test crossed the boundary, because
   `ecs_tests` and `ecs_bench` are self-contained and no other image instantiated `typeSlot`. The table is now
   keyed by `ecs::kTypeKey<T>`, a compile-time FNV-1a hash of the type's name, which every image computes
   alike; nothing per image holds state, and a reloaded game module finds its components under the same
   key. Two distinct types with one qualified name (unnamed namespaces of different translation units)
   share a key, and a World binds only the first. `link_model_tests` checks the built-ins from the
   executable and lets `link_model_probe` read and write components of a World the executable created
   (typed `set`/`get` and a typed `CommandBuffer`); `ecs_tests` covers the table and the clash.
12. **State that two groups define, and third-party state in the exports.** The first audit compared only
   consumers with groups. A header-defined static instantiated in two groups is one instance on Linux and
   two on Windows, so R3 now compares groups with each other (none exists today). R1 also checked only
   strong definitions; weak and unique data passed. `helios_runtime` exported a global-namespace log channel
   (`LogPcg`, now in `helios::pcg`) and Jolt's inline static `CharacterID::sNextID`, which `engine/physics`'s
   objects instantiate with default visibility. Jolt lives only in `helios_runtime`, so that is still one
   instance; it is a known finding of R1, owned by part 2's explicit exports, which hide it.

## 6. Part 2

Part 2 adds the reload loader, the `Probe` game module (`game_probe`, replacing `link_model_probe`) and RT-18's
measurements: 100 reloads on 10k entities, the values checksum, memory tags back to baseline, the unmapped old
image, ASan on both toolchains, the 20-reload PR smoke test (the `windows-msvc-dev` job carries a TODO for
it) and the edit-to-reload time on DEV. It also adds `-fno-gnu-unique` for GCC game modules, switches the IDE
presets (`windows-vs2026`, `windows-vs2022`) to `HELIOS_MODULAR=ON` (ADR-001a rule 5) once the modular MSVC
build is proven in CI, and adds `linux-dev` to the nightly tier (02 §8.3), which runs the audit's ELF rules
in CI. Two more items belong to part 2:
- **Explicit exports** (02 §1.4, *Exports*): module code moves to `-fvisibility=hidden`, and every
  declaration another image uses carries its group's macro, starting with the API game modules import; the
  export-all scan goes away on MSVC.
- **Tracy across images** (02 §1.4, *Tracy*): `TRACY_EXPORTS` for the runtime group and `TRACY_IMPORTS` for
  game modules. No module links Tracy yet, so part 1 builds neither.
