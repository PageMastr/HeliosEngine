# cmake — Helios build helpers

| File | What it holds |
|---|---|
| `HeliosModule.cmake` | `helios_declare_module()`, `helios_module()`, `helios_executable()` (ROLE, ISA level, CPU gate), `helios_test()`, the warning flags; schedules `helios_finalize_build()` |
| `HeliosLayering.cmake` | the configure-time module-graph checks (02 §1.1 layers, peers, cycles, HEADLESS, EDITOR_ONLY, roles) and `helios_finalize_build()` |
| `HeliosIsa.cmake` | the ISA levels (below), the CPU gate's level and `_helios_cpu_gate()` (internal: `helios_executable()` calls it) |
| `HeliosModular.cmake` | the dev link model (`HELIOS_MODULAR`, ADR-016, 02 §1.4): the three link-group shared libraries, their export headers, self-contained images (`docs/adr/ADR-0.6c-link-model-spike.md`) |
| `isa_allowlist.cmake` | the ISA audit's lists: the gate's exports and imports, the self-dispatching symbols base images may contain |
| `pre_main_allowlist.cmake` | the attributed pre-`main` hooks of avx2 images (mimalloc, Tracy, Helios initializers) and the ELF gate entry |
| `HeliosSchema.cmake`, `HeliosShaders.cmake`, `HeliosWindows.cmake` | schema code generation, Slang shaders, the Windows manifest |
| `toolchains/` | the MinGW cross toolchain file (`cross-mingw` preset) |

## ISA levels (02 §1.1, ADR-011 amendment; WP-0.2r)

Helios builds **whole images at one ISA level**; no list grants a target or a file flags of its own.

| Level | Flags (GCC/Clang · MSVC · clang-cl) | Built at it |
|---|---|---|
| `avx2` | `-mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma -mfpmath=sse -ffp-contract=off` · `/arch:AVX2 /fp:precise` · the MSVC set plus `-mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma /clang:-ffp-contract=off` | every image of a role other than launcher and bootstrap (client, cell, gateway, voice, editor, bot, tool, sample, bench), the tests, and every library they link: every target not listed below |
| `base` (x86-64-v1) | `-march=x86-64 -mtune=generic` and every extension above it off by name · nothing (`/arch:SSE2` is the default) | the launcher and the bootstrap (ROLE `launcher`, `bootstrap`), and the `.base` copies of what they link |
| `gate` | the `base` set plus `-fno-stack-protector -fno-sanitize=all` · `/GS-` (plus `-fno-sanitize=all` with clang-cl); no `/RTC` and no MSVC `/fsanitize=address` (below) | the CPU gate's object libraries (`helios_cpu_gate_target()`): `helios_core_cpugate`, `helios_core_cpugate_hook` and the test hook `core_cpugate_hook_snb` |

- **How a level is applied.** `helios_isa_finalize()` runs at the end of configure, when every target exists. It
  gives every target its level with `helios_apply_isa_level()`, the one function that puts ISA flags on a
  target (CONF-11 exempts it by name), as PRIVATE options, so no consumer inherits them. The levels go to
  `<build>/helios_generated/isa_levels.txt`, which audit check 1 reads.
- **Base images and `.base` copies.** A base image's link closure is replaced by `<target>.base` copies (OBJECT
  libraries, the plan's `<module>@base`; CMake target names cannot contain `@`), compiled at `base`. Copies
  exist only once a base image is configured, and only for `HELIOS_ISA_BASE_MODULES` (`core`, `app`, `ui`,
  `text`, `loc`, `patch`, `crash`: `core` and `patch` exist today; `app` is named by WP-0.11's row but is not
  in the tree yet, and the launcher, WP-0.17, is the first base image to need it; `ui`, `text` and `loc` come
  with WP-1.6 (02 §7.5–7.6), and the launcher links `ui` from WP-1.21; `crash` comes in Phase 2),
  `HELIOS_ISA_BASE_THIRD_PARTY` (08 §2.1.1's SDL3, RmlUi, FreeType, HarfBuzz, SheenBidi, libunibreak, zstd,
  Monocypher, yyjson, sentry-native) and what those link. The audit's `lint_isa_fixture_base` image builds
  `helios_core.base`, `helios_patch.base` and the copies of mimalloc, Monocypher, zstd and the header-only
  libraries on every toolchain.
- **Configure errors** (fixtures `lint_layering_isa_*`): a base image whose closure reaches any other library
  (`physics`, `pcg`, `tp_jolt`, ...), with the path to it; `HELIOS_ISA_BASE_DENY` (`physics`, `pcg`,
  `tp_jolt`) is never eligible, even when a base module links it; a base image whose own sources name another
  library's objects (`$<TARGET_OBJECTS:…>`) unless they are a gate object library or a base copy; an ISA option
  on a library's `INTERFACE_COMPILE_OPTIONS` (consumers would inherit it: `tp_jolt` exports only its
  `JPH_USE_*` defines); `helios_executable(… ISA …)` in a listfile outside `tools/lint/`, under `apps/` or
  `engine/`, or with a level other than `avx2` or `base` (the explicit level is for the audit's fixtures);
  `CPU_GATE` on a base image; `NO_CPU_GATE` on an `avx2` image (WP-0.5r); a `HELIOS_ISA_LEVEL` set on anything
  but an image or a gate object library; `helios_cpu_gate_target()` on anything but an object library.
- **Modular dev builds** (`HELIOS_MODULAR=ON`, `HeliosModular.cmake`). The same levels: the link-group libraries,
  the module object libraries in them, `SDL3` and `tp_imgui` are `avx2`; the gate's object libraries keep `gate`
  inside `helios_runtime`. A base image links `.base` copies there too and never a group: `helios::<module>`
  names the module's consumer interface, which links its group, so the base image's links to it are replaced
  by the module's copy, and configure fails if the image still reaches a shared library built here. The
  `isa_*` layering fixtures run in both flavours (`lint_layering_modular_isa_*`), and audit check 3 rejects
  IFUNC relocations in the shared libraries gated executables load (ADR-0.6c §3.1).
- **Self-dispatch.** Code that checks CPUID itself stays allowed: in `base` images only in the symbols that
  `isa_allowlist.cmake` lists (audit check 4: zstd's BMI2 functions today), in `avx2` images anywhere (pcg's
  kernel selection). Check 4 reads TZCNT's encoding as BSF, because GCC and Clang emit `rep bsf` for
  count-trailing-zeros at x86-64-v1 and CPUs without BMI1 execute it as BSF. That leaves a false negative
  (code built for BMI1 that relies on TZCNT for a zero operand), which `lint_isa_base_sources` narrows for the
  Helios base modules: no BMI target attribute or pragma, TZCNT intrinsic or TZCNT assembly in their sources.
- **Which images carry the CPU gate** (02 §1.1; WP-0.5r). `helios_executable()` links the gate's hook into
  every `avx2` image of a gate role (client, cell, gateway, voice, editor, bot, tool), and `CPU_GATE` adds it to
  another `avx2` image (the gate's test children). There is no opt-out: `NO_CPU_GATE` on an `avx2` image stops
  configure. A shipping (monolithic) image carries the hook itself (Linux: its single `.preinit_array` entry;
  Windows: the first TLS callback, `.CRT$XLA0`). In a modular Windows build the hook lives in
  `helios_runtime.dll` and a gated executable gets `/INCLUDE:helios_cpu_gate_run`, so the DLL is always
  imported and initialized first; that gates every process that loads the DLL, tests, samples and a modular
  launcher included (ADR-0.6c §3 item 7). The DLL exports the gate's TLS slot as data
  (`/EXPORT:helios_cpu_gate_tls_entry,DATA`), so `core_tests` can check that it is the DLL's first TLS
  callback. On Linux only gated executables carry the hook, so there the role decides. A self-contained gated
  image links the hook itself (`HeliosModular.cmake`).
- **Gate-object rules CMake cannot express per target.** MSVC's `/RTC1` (CMake's Debug default) lives in
  `CMAKE_C_FLAGS_DEBUG`, which applies per directory: `engine/core/CMakeLists.txt` drops it from that
  directory's C flags, whose only C units are the gate's. cl has no switch that turns `/fsanitize=address` off
  again, so the top-level `CMakeLists.txt` adds it to every target whose level is not `gate`. Audit check 1
  rejects a gate unit that carries either, or coverage instrumentation.

The audit itself is `tools/lint/isa_audit.cmake` (see `tools/lint/README.md`).

## Plan conformance

Plan-Rev: 14

`HeliosIsa.cmake`, `isa_allowlist.cmake` and `pre_main_allowlist.cmake` follow 02 §1.1 at plan revision 13
(WP-0.2r part 1), and at 14 in modular builds (WP-0.6c part 1: 02 §1.1 *Which image*, 02 §1.4). The gate's
placement, exports, role rules and object rules follow 02 §1.1 at revision 14 (WP-0.5r part 1; 09 §5.10.4 (b));
the other helpers predate this README and are described by their own headers.
