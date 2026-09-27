# engine/render — Helios renderer (render graph v0, shader reflection, forward v0, ꟻLIP)

`helios::render` (target `helios_render`, L3, graphics-only; never linked by servers) builds on the
public `engine/rhi` API (ADR-003, `docs/plan/03-rendering.md` §1.7, §2, §8.4; WP-0.12).

| Header (`helios/render/…`) | Contents |
|---|---|
| `render_graph.h` | `RenderGraph`, `RgBuilder`, `RgContext`, `RgResourcePool`, the compiled `RgPlan`, `resolveEntryBarriers`, text/Graphviz dumps |
| `shader_reflection.h` | Helios shader reflection (`.hsr` v1 binary format, documented in the header), SPIR-V reflection, JSONC rendering |
| `shader_library.h` | The embedded (shipped) shader modules, `createShippedPipeline()` and the record of shipped pipelines behind RC-1's coverage check |
| `forward.h` | Minimal forward pipeline (Upload → Clear → Geometry → async Exposure → Tonemap), meshes, deterministic infinite reverse-Z projection, camera-relative per-frame data |
| `image.h` | RGBA8 images, PNG I/O (stb) |
| `flip.h` | ꟻLIP (LDR) image difference, implemented from the paper |

## Render graph (03 §2)

```cpp
RenderGraph graph("Frame");
RgTexture back = graph.importTexture("Backbuffer", image.texture, device.textureDesc(image.texture),
                                     {.finalState = rhi::ResourceState::Present, .waitFor = image.ready});
struct Geo { RgTexture color; };
const Geo& geo = graph.addPass<Geo>("Geometry", PassFlags::Raster,
    [&](RgBuilder& b, Geo& d) {
        d.color = b.colorAttachment(b.create("SceneColor", RgTextureDesc::tex2D(Format::RGBA16Float, w, h)), 0);
        b.depthAttachment(b.create("Depth", RgTextureDesc::tex2D(Format::D32Float, w, h)));
    },
    [=](const Geo&, RgContext& ctx) { ctx.cmd().bindPipeline(pso); ctx.cmd().draw(3); });
graph.addPass("Exposure", PassFlags::AsyncCompute, [&](RgBuilder& b) { b.read(geo.color); /* ... */ }, exec);
graph.compile().value();
RgExecuteResult r = graph.execute(device, pool, {.jobs = &jobSystem}).value();
device.present(swapchain, r.graphics());
```

* **Setup** declares resources and accesses. Every write creates a new *version*; reads name the
  version they consume, so stale reads are setup errors and culling is exact. Usage flags of
  transients come from their accesses; imported resources are checked against their usage.
* **Compile**: culling (flood from `NeverCull`, imported writes and `markOutput`), submission
  batches per queue split only where another queue waits, minimal timeline waits (vector clocks),
  aliasing of transients whose lifetimes are ordered by happens-before (never across concurrently
  running queues), a placement plan (64 KiB-aligned greedy interval packing per D3D12 heap tier-1
  class), per-subresource barriers (layout transitions, RAW/WAW/WAR on one queue, releases of
  graphics-only states before async compute, final states of imported resources in the pass or a
  graphics epilogue), `DontCare` stores for attachments nobody reads, command-list partition.
* **Execute** resolves *entry* barriers (first touch of a physical resource) against the pool's
  states from the previous frame or the import state — a graphics prologue takes transitions the
  consuming queue cannot perform — waits for the previous frame's use on other queues (and, on
  every queue that touches an import, for its `waitFor` point), records the lists in parallel on
  the job system and submits the batches in order. New transient textures get their per-mip
  bindless views (`uav(mip)`, one-mip SRVs) when created, and the sub-views of imported textures
  that passes declare are created in plan order before recording, so slot numbers — and the
  command streams — do not depend on how the recording jobs interleave.
* **Pool**: several graphs per frame may share an `RgResourcePool`; resources no graph used during
  the last `keepFrames` completed frames (`Device::frameIndex()`) are destroyed.
* `dumpText()` / `rgDumpPlan()` give deterministic plan dumps (used in trace goldens),
  `dumpGraphviz()` the pass/version DAG with queues, culled passes and waits (visualizer input).

**Budget:** a full compile of 200 passes, which is the topology-change case, ≤ 0.3 ms CPU on REF at
60 fps (03 §2.2 item 7, §8.1.5). `perf: render graph compile of 200 passes <= 0.3 ms (no topology
cache)` in `render_tests_perf` asserts it on the best of 10 batches of 5 compiles, in optimized builds
without sanitizers, and reports the median batch and §8.1.5's 120 fps column (≤ 0.2 ms) without gating
them. §8.1.5's rows are p50; on the 4-core dev container, which is not REF, the best of 10 is
≈ 0.21–0.25 ms and the median batch ≈ 0.21–0.22 ms (a review's 200 single compiles: p50 0.212–0.215 ms
against a best of 0.211–0.213 ms), so the two agree here. The Phase 2 topology cache has its own
budget: a hit ≤ 0.05 ms.

## Shader reflection (03 §1.7)

`reflectSpirv()` extracts entry points (stage, workgroup size, push-constant use), the push-constant
block (members with offsets, sizes, kinds; Slang's `_Array_` wrappers unwrapped), specialization
constants (id, kind, default) and descriptor bindings (set, binding, kind, count/runtime array,
access from `NonWritable`/`NonReadable`, entry-point mask). `serializeReflection()` /
`parseReflection()` implement the `.hsr` v1 binary format; `reflectionToJsonc()` a readable
rendering. Both parsers treat their input as untrusted: sparse member tables, a work budget on
type-graph walks (cyclic or exponentially shared types fail as `Corrupt`), linear-time
entry-point interface lookup, and a bound on how far `.hsr` string references may expand. `helios-shaderc` (tools/shaderc) writes `.hsr` next to its SPIR-V. The forward pipeline
checks its C++ push-constant structs against the reflected sizes at creation.

**Shipped shaders.** Every `shaders/*.slang` file is embedded in `helios_render`, and features build
their pipelines with `createShippedPipeline()` (`shader_library.h`), which records each pipeline's
name with its module entry points and a fingerprint of its other state (raster, depth, blend, formats,
topology, samples), and rejects SPIR-V that is not embedded (by address) and a name reused with other
shaders or other state: a state variant needs its own name and golden, also when a feature builds the
same pipeline for several output formats in one process (the same desc on another device is fine). `helios-rendertest --coverage` (CTest `rendertest.coverage`) maps the
pipelines bound in each golden scene's Null trace back to these entry points and fails when a shipped
entry point or recorded pipeline is bound by no scene with committed lavapipe and Null goldens. The
CTest `lint_shipped_pipelines` (label `lint`, a textual check) keeps pipelines from bypassing the
record: in `engine/`, `apps/` and `tools/` (except `engine/rhi/`, which implements the API), any
identifier `create<X>Pipeline` other than `createShippedPipeline`/`createLocalPipeline` needs a
reasoned `// shipped-pipelines-lint: allow <reason>` waiver, and only `src/shader_library.cpp` is exempt
(waived today: rendertest's `createLocalPipeline()`, the raw-RHI sample and the pcg twin's test
pipelines). This is RC-1's "every shipped feature has a golden" for pipelines. What it cannot see, and
the scorecard keeps as a gap: passes without a pipeline (uploads, copies, clears), whether a bound
pipeline's output reaches the golden image, shaders not embedded in `helios_render` (cooked SPIR-V from
content, Phase 1), and a pipeline created through token pasting or a member pointer obtained outside the
scanned files. Future costs: `createShippedPipeline()` reflects its module on every call (cache per
module before 03 §1.7's thousands of PSOs), and a hot-reloaded pipeline whose entry points or state
change must re-register its name (Phase 2).

## Forward pipeline v0

Clear (copy) → Geometry (raster, vertex pulling through buffer device addresses, reverse-Z
`GreaterOrEqual`, depth `DontCare` store) → Exposure (async compute: log-average luminance of a
16×16 grid) → Tonemap (fullscreen, ACES fit, sRGB). Positions are frame-local f64; the CPU uploads
only camera-relative f32 matrices (`toMatrixCameraRelative`), so a scene at 10⁷ m produces
byte-identical GPU data to the same scene at the origin (tested). `infiniteReverseZ()` uses `det::`
trigonometry, making the uploaded bytes (and the Null trace goldens) identical on every CRT.

## Tests (`render_tests`)

* `test_graph.cpp` — culling, versions, setup errors, barrier derivation, same-state hazards,
  mip-chain subresources, async batches and waits, releases, final states/epilogue, aliasing
  (incl. never across concurrent queues), placement, partition, entry resolution, dumps, compile budget.
* `test_graph_stress.cpp` — 300 random DAGs × 2 frames × random options, executed on the Null
  backend (serial and parallel recording) and checked by an independent reference simulator
  (culling, schedule, happens-before of every conflicting pair per physical subresource, states,
  contents seen by every read, placement overlap). Mutating the compiler (no waits, no WAR barrier,
  alias without happens-before, submission-order barrier simulation, …) makes it fail.
* `test_graph_null.cpp` — trace golden of a small frame, parallel == serial recording, pool state
  carry-over and trimming, cross-frame waits, swapchain import, execute-time prologue, context validation.
* `test_forward.cpp` — meshes, projection, camera-relative precision, Null trace golden.
* `test_shader_library.cpp` — the embedded entry points, the forward renderer's pipeline record, and
  rejection of unnamed, foreign (including copied), unknown-entry and conflicting pipelines.
* `test_graph_perf.cpp` — `perf:` compile budget of a 200-pass graph, ≤ 0.3 ms (runs in `render_tests_perf`).
* `test_reflection.cpp`, `test_shaderc.cpp` — reflection of Slang output, hostile SPIR-V / `.hsr`
  input (huge member indices, cyclic types, string amplification); the real `helios-shaderc` end
  to end (byte-identical to the build's slangc, spirv-val, depfiles, errors, paths with shell
  metacharacters and non-ASCII names).
* `test_flip.cpp` — ꟻLIP stages against published CIELAB values, the analytic uniform case,
  properties, and agreement (~1e-6) with NVIDIA's reference implementation (flip-evaluator 1.7)
  on procedural images; PNG I/O.
* CTest `shaderc.spirv-val` runs `spirv-val` over exactly the SPIR-V modules the build cooks (every
  module declared by a `helios_shaders()` call: render, rhi, pcg, samples, rendertest), fails when a
  declared module is missing or the list is empty (`shaderc.spirv-val.fixture_*`), and ignores stale
  `.spv` files. It is required when configure finds spirv-tools (Linux CI installs it) and reports
  Skipped, not Passed, otherwise.

Trace goldens live in `tests/golden/`; re-bless with `HELIOS_UPDATE_GOLDENS=1` (review the diff).
Golden images and GPU runs are in `tools/rendertest`.

```
cmake -S . -B build/render -G Ninja -DHELIOS_BUILD_GRAPHICS=ON
ninja -C build/render render_tests helios-shaderc helios-rendertest
ctest --test-dir build/render -R "render|rendertest" --output-on-failure
```

## Known limitations (v0)

* **Aliasing** is realized by pooling physical resources with identical descriptions; the
  placement plan (different descriptions sharing heap memory) is computed and validated but needs
  RHI placed resources (`createTexture` in a heap at an offset, aliasing barriers) to be realized.
* The RHI has no "discard with source sync" barrier: the first use of a reused physical resource
  transitions from its previous state (contents are logically discarded; no extra cost on the
  tested drivers).
* Cross-queue ownership transfers are unnecessary because the RHI creates resources `CONCURRENT`;
  the transfer queue is not used by the graph yet (copies run on graphics).
* No compile cache (Phase 2), no per-pass GPU timings/visualizer UI yet (ImGui T26), command lists
  are balanced by pass count rather than last frame's costs.
* Declared resource accesses are checked at the resource level (`RgContext`); per-binding checks
  against `.hsr` accesses arrive with materials.

## Plan conformance

Plan-Rev: 10

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked at revision 10 by WP-0.12 on
2026-09-27: revisions 7–10 changed 06 §1.2, 02 §7.4, 04 §10.2, 09 §5.2a and ADR-004a, none of which
maps here. No conformance delta is open; see §5.10.4 (c) there.
