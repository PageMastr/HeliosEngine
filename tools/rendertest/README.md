# helios-rendertest — golden-image tests (RC-1, AAA-REN-7)

`helios-rendertest` (`docs/plan/03-rendering.md` §8.4) renders deterministic scenes offscreen
through the render graph and compares them with goldens:

* **Vulkan** (lavapipe in CI; a software adapter is preferred so every machine compares against the
  same goldens, `HELIOS_RHI_ADAPTER` overrides): each scene is rendered **twice** — serial command
  recording, then parallel recording on the job system — and the two images must be bit-identical
  (catches missing barriers and recording-order dependence). The image is scored against
  `golden/vulkan-<driver>/<scene>.png` (falling back to `golden/vulkan-llvmpipe/`) with **ꟻLIP**
  (`engine/render/include/helios/render/flip.h`): mean ≤ 0.01 plus a per-scene maximum.
* **Null** (every toolchain, GPU-less CI): the plan dump and command-stream trace of the captured
  frame must equal `golden/null/<scene>.txt`, and two runs on fresh devices must be identical.

| Scene | What it covers |
|---|---|
| `triangle` | one raster pass (RC-1) |
| `compute` | async-compute storage image sampled by a fullscreen pass (RC-1) |
| `bindless` | eight quads, four bindless textures × four bindless samplers, non-uniform indices (RC-1) |
| `forward` | the forward pipeline v0 at 10⁷ m (camera-relative, reverse-Z infinite, async exposure) |
| `forward-srgb` | the same frame into an RGBA8Srgb output: the tonemap's hardware-encoding path (`encodeSrgb = 0`) |
| `normals` | the same frame with its DebugNormals view drawn over the right half |
| `postchain` | blur chain alternating graphics / async compute; transients aliased across queues |
| `mips` | mip chain by compute (one level on async compute), per-mip barriers, mip viewer |

**Coverage (RC-1: every shipped feature has a golden).** `--coverage` renders every scene once on the
Null backend (no GPU) and maps the pipelines bound in its captured frame back to engine/render's shipped
shaders (`engine/render/include/helios/render/shader_library.h`). It fails when a shipped shader entry
point or a pipeline engine/render recorded through `createShippedPipeline()` is bound by no scene whose
lavapipe and Null goldens are both committed, and lists which scenes cover what. A new render feature
therefore needs a scene and its goldens in the same commit; `--scene` limits the scenes that count.

* **No bypass.** Scenes create their own pipelines through `createLocalPipeline()` (`src/scenes.h`),
  and CTest `lint_shipped_pipelines` (`tests/shipped_pipelines_lint.cmake`, label `lint`, seeded
  fixtures in `tests/shipped_pipelines/`; registered through `tests/shipped_pipelines_tests.cmake` by
  `tools/lint/lint_tests.cmake`, so the headless configuration runs it too, and run by
  `tools/ci/run_lints.cmake`) fails on any identifier `create<X>Pipeline` other than
  `createShippedPipeline`/`createLocalPipeline` in `engine/`, `apps/` and `tools/` (not `engine/rhi/`,
  which implements the API, `tools/prebuilt/` or the lints' fixtures), unless a reasoned
  `// shipped-pipelines-lint: allow <reason>` waiver sits on its line or alone on the line above. It
  catches `.` and `->` calls, calls split across lines, a backslash-newline splice inside the
  identifier, member-function pointers and `std::invoke` that name the member (review mutants C7, L1,
  L3, L3b, L5, L8). It is a best-effort textual check against accidental direct pipeline creation,
  not against deliberately adversarial source; code review and the coverage check are the backstops.
  It does not see an identifier built by token pasting, a creation path (wrapper or member pointer)
  defined outside the scanned files, which includes `engine/rhi/`, or source in a file with an
  extension it does not scan (it scans `.cpp .cc .cxx .cppm .ixx .h .hh .hpp .hxx .inl .inc .ipp`).
* **One name, one pipeline.** Coverage maps pipelines by the debug name in the Null trace, so
  `createShippedPipeline()` rejects a recorded name reused with other shaders or other state (a
  fingerprint of raster, depth, blend, formats, topology and samples that names every desc member, so
  a new member fails to compile until it is fingerprinted; mutant C11), while the same desc on another
  device is accepted. A pipeline that depends on an input is named per value: the forward tonemap is
  `Forward.Tonemap.<output format>`, covered by `forward`/`normals` (RGBA8Unorm) and `forward-srgb`
  (RGBA8Srgb). A rendertest pipeline named like a shipped one fails the check
  (`rendertest pipeline '…' reuses the name of a shipped pipeline`; mutant C8).
* **Known limits** (a narrowed gap in `scorecard.jsonc`): "bound" is not "visible": a pipeline bound
  only into a marked debug output counts although its output never reaches the PNG. Passes without a
  pipeline (uploads, copies, clears) and shaders that are not embedded in `helios_render` (cooked
  SPIR-V, Phase 1) are not tracked, nor is a pipeline whose creation the lint cannot see. The record
  is filled by this process, so a pipeline or state variant created only for an input no scene uses
  (e.g. the tonemap for a BGRA8Srgb swapchain) is neither covered nor reported.

**Khronos validation (03 §1.5).** Where configure finds `VkLayer_khronos_validation.json` (the system
layer directories, `VK_ADD_LAYER_PATH`, `VK_LAYER_PATH` or the Vulkan SDK; not when cross-compiling),
the Vulkan scene tests pass `--require-validation`: the layer must load, or the test fails.
`HELIOS_SKIP_GPU_TESTS=1` skips only the Vulkan RHI's errors for a machine without Vulkan: no loader,
no driver (`VK_ERROR_INCOMPATIBLE_DRIVER`), no physical device, or none that meets Helios'
requirements; every other device error fails, so a layer that fails (not installed, a missing library,
a `vkCreateInstance` that fails) is never a skip. With a required layer, even a no-Vulkan error fails
when the loader forces layers on every instance (`VK_INSTANCE_LAYERS`, `VK_LOADER_LAYERS_ENABLE`) or
when a probe device created without validation, whatever `HELIOS_RHI_VALIDATION` says
(`DeviceDesc::validationFromEnvironment`), exists: the layer then failed as if there were no driver.
`rendertest.validation-layer` (`--validation-self-test`) prints the layer's name and version as the
device reports them (`Caps::validationLayer`) and checks that the layer reports a seeded error: a
barrier from the wrong state (`VUID-VkImageMemoryBarrier2-oldLayout-01197` on 1.3.275), which the
Vulkan RHI itself does not track, and that `validationErrorCount()`, which fails a golden, counted it.
`rendertest.validation-required` checks, with `HELIOS_SKIP_GPU_TESTS=1`, that `--require-validation`
and the self-test fail, and are not skipped, when the loader hides the layer
(`VK_LOADER_LAYERS_DISABLE`), when a broken manifest on `VK_LAYER_PATH` lists a layer whose library is
missing (also with `HELIOS_RHI_VALIDATION=1`), and, on Linux, with two fake
`VK_LAYER_KHRONOS_validation` layers built for the test only (`tests/fake_layer.c`, never installed):
one whose `vkCreateInstance` fails with `VK_ERROR_INITIALIZATION_FAILED` (also with
`HELIOS_RHI_VALIDATION=1` and forced by `VK_INSTANCE_LAYERS` or `VK_LOADER_LAYERS_ENABLE`), and one
that fails with `VK_ERROR_INCOMPATIBLE_DRIVER` (also with `HELIOS_RHI_VALIDATION=1` and forced by
`VK_INSTANCE_LAYERS`). It also checks that an adapter selection matching nothing (`HELIOS_RHI_ADAPTER`)
fails without any layer, and, where configure found the layer, through the `--print-probe` test hook,
that `HELIOS_RHI_VALIDATION=1` validates a default device but not the probe. Without the layer at
configure time, the scenes run with `--validation` (unvalidated), `rendertest.validation-layer`
reports Skipped, and every Vulkan result records `"validation": false`, which the report shows
("validated" column, and a count in the summary line). On Windows, `find_file` sees only
`$VULKAN_SDK` (`Bin`, `share/vulkan/explicit_layer.d`), not layers the loader finds through the
registry: on a machine without `VULKAN_SDK` the goldens then run unvalidated and
`rendertest.validation-layer` reports Skipped, which is visible, not a silent pass. The `win-gpu` runner
(WP-0.4) has the Vulkan SDK installed machine-wide, and `.github/workflows/win-gpu.yml` fails without `VULKAN_SDK`
or when `rendertest.validation-layer` does not report the layer. Reading the registry's layer list at configure
time stays a follow-up.

```
helios-rendertest --list
helios-rendertest --coverage
helios-rendertest --backend all --report --out build/rt      # report.html / report.md in build/rt
helios-rendertest --backend vulkan --scene forward --update-goldens   # re-bless (inspect the PNG!)
```

Per scene the tool writes `<out>/<backend>/<scene>.json` (result), the actual image or trace, a copy
of the golden and the ꟻLIP error map (magma). `--report-only` aggregates the JSON results into
`report.html` / `report.md` and fails if any scene failed or the suite exceeded `--budget`
(default 600 s, RC-1). The whole suite takes about 1 s on lavapipe on the 4-core dev container.

CTest: `rendertest.vulkan.<scene>` (labels `gpu;rendertest`; validated as described above, and any
validation error fails the scene), `rendertest.null.<scene>` (label `rendertest`), `rendertest.scenes`
(the CMake list matches the built-in scenes), `rendertest.coverage` (GPU-less, every toolchain),
`rendertest.report` (after the scene tests), `rendertest.validation-layer` and
`rendertest.validation-required` (labels `gpu;rendertest`), `lint_shipped_pipelines` and its fixtures
(label `lint`, registered by `tools/lint/lint_tests.cmake`), and `rendertest.cli` (GPU-less CLI checks:
a skip on a machine without a Vulkan driver, simulated with `VK_DRIVER_FILES`, and coverage failures for missing
scenes, missing goldens and a name collision, the last through the `--seed-name-collision` test hook;
`--print-probe` is the other test hook).
`HELIOS_SKIP_GPU_TESTS=1` turns a machine without Vulkan into a skip, which is recorded as a `skip`
result (so an older passing result in the same `--out` directory cannot stand in for the run);
the report ignores results of scenes that no longer exist. Golden updates need a justification in
review (09 §5.3 item 7).

Goldens are small PNGs committed with the tree for now; 03 §8.4 moves them to Git LFS per backend
and driver once the repository enables LFS.

## Plan conformance

Plan-Rev: 10

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 10 by WP-0.12 on
2026-09-27 (revisions 7–10 changed no anchor that maps here). No conformance delta is open; see
§5.10.4 (c) there. Two layout deviations from 03 §9.1 remain: the tool lives in `tools/rendertest`,
not `apps/tools/` (like `tools/schemac`; 09 §8.1 names `tools/`), and goldens are committed PNGs and
traces here rather than Git LFS under `tests/golden/`, because the repository has no LFS yet.
