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
| `normals` | the same frame with its DebugNormals view drawn over the right half |
| `postchain` | blur chain alternating graphics / async compute; transients aliased across queues |
| `mips` | mip chain by compute (one level on async compute), per-mip barriers, mip viewer |

**Coverage (RC-1: every shipped feature has a golden).** `--coverage` renders every scene once on the
Null backend (no GPU) and maps the pipelines bound in its captured frame back to engine/render's shipped
shaders (`engine/render/include/helios/render/shader_library.h`). It fails when a shipped shader entry
point or a pipeline engine/render built from one is bound by no scene whose lavapipe and Null goldens are
both committed, and lists which scenes cover what. A new render feature therefore needs a scene and its
goldens in the same commit; `--scene` limits the scenes that count.

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

CTest: `rendertest.vulkan.<scene>` (labels `gpu;rendertest`; with `--validation`, so Khronos validation
runs wherever the layer is installed, as on the Linux CI images, and any error fails the scene), `rendertest.null.<scene>` (label
`rendertest`), `rendertest.scenes` (the CMake list matches the built-in scenes), `rendertest.coverage`
(GPU-less, every toolchain), `rendertest.report` (after the scene tests), and `rendertest.cli`
(GPU-less CLI checks, including coverage failures for missing scenes and goldens).
`HELIOS_SKIP_GPU_TESTS=1` turns a missing Vulkan device into a skip, which is recorded as a `skip`
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
