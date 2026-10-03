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
  Each compile rebuilds the plan in the storage of the previous one, with the compiler's scratch
  arrays kept alongside, so recompiling an unchanged graph allocates nothing once that storage has
  grown (counted for the 200-pass perf graph: 1,069 allocations in the first compile, 38 in the second,
  none after, where the previous compiler made 4,718 in every compile); the plan is exactly what a
  compile into fresh storage gives (tested field by field).
* **Reuse**: `reset(name)` empties the graph for the next frame's setup but keeps that storage, so a
  renderer that keeps one `RenderGraph` and rebuilds it every frame compiles without reallocating its
  plan. A new `RenderGraph` per frame works as before and is only somewhat slower (below).
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
them. The scorecard tracks the gated value as `render.graph_compile_200_passes_ms` (render budget 5 %,
per host class). The Phase 2 topology cache has its own budget: a hit ≤ 0.05 ms.

Until 2026-10-03 the compile had no margin on the nightly's most common hosted `ubuntu-24.04` class: best
0.302–0.313 ms, median batch 0.309–0.323 ms, red on every night there (faster classes passed). A
callgrind profile of the perf case put 32 % of the compile's 3.55 M instructions in `malloc`/`free` (a
vector per pass, subresource, reader, batch and barrier list, and a new plan each time) and 27 % in the
happens-before test that aliasing and placement repeat for pairs of resources. The compiler
now keeps its scratch and the previous plan's storage (above), uses flat arrays instead of those
vectors, and folds happens-before per resource, so that test is a few compares instead of a walk over
pass, batch and clock records: 1.20 M instructions per compile, 0.3 % of them in the allocator, with
placement's pair test (29 %) the largest remaining step. The plan is unchanged: besides the tests
below, the full plans (`fullPlanText`, every field) of the stress test's 300 random graphs × 2 frames
(compiled and executed, plus 2 more option sets each) and of the budget graph's shape at 10–400 passes
under 48 option and import combinations, 2,040 plans in all, were byte-identical to the previous
compiler's.

Interleaved runs of the old (`main` at ee0a8af) and new `render_tests` (linux-gcc RelWithDebInfo, the
nightly's configuration), alternating which goes first, on the shared 4-vCPU dev container while other
agents built (1-minute load average 0.6–4.1), 60 runs of each:

| `render_tests_perf` | Before: median (IQR; min–max) | After: median (IQR; min–max) | Change |
|---|---|---|---|
| Best of 10 batches (gated) | 0.302 ms (0.300–0.305; 0.291–0.594) | 0.104 ms (0.103–0.106; 0.102–0.181) | −66 % |
| Median batch | 0.328 ms (0.317–0.343; 0.307–0.617) | 0.114 ms (0.111–0.120; 0.108–0.189) | −65 % |

The new compile was faster in all 60 pairs. The old one now measures about 0.30 ms here (0.21–0.26 ms
when #18 landed), as slow as the failing hosted class. A new `RenderGraph` per compile, which allocates
its plan (1,069 allocations), measured a best of 0.126 ms against 0.284 ms (medians of 20 interleaved
runs each, −56 %), and `reset()` with the graph rebuilt 0.097 ms, so most of the gain does not depend
on reusing one graph. The margin on the hosted runner classes is verified only by the next nightly.

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
topology, samples; structured bindings name every desc member, so a new one fails to compile until the
fingerprint takes it), and rejects SPIR-V that is not embedded (by address) and a name reused with
other shaders or other state: a state variant needs its own name and golden, and a pipeline that
depends on an input is named per value (the tonemap per output format, `Forward.Tonemap.RGBA8Unorm`),
so renderers for several formats coexist in one process (the same desc on another device is fine too).
`helios-rendertest --coverage` (CTest `rendertest.coverage`) maps the pipelines bound in each golden
scene's Null trace back to these entry points and fails when a shipped entry point or a pipeline the
scenes create is bound by no scene with committed lavapipe and Null goldens (the scenes render to
RGBA8Unorm, and `forward-srgb` to RGBA8Srgb, the tonemap's hardware-encoding path). The CTest
`lint_shipped_pipelines` (label `lint`) keeps pipelines from bypassing the record: in `engine/`,
`apps/` and `tools/` (except `engine/rhi/`, which implements the API), any identifier
`create<X>Pipeline` other than `createShippedPipeline`/`createLocalPipeline` needs a reasoned
`// shipped-pipelines-lint: allow <reason>` waiver, and only `src/shader_library.cpp` is exempt (waived
today: rendertest's `createLocalPipeline()`, the raw-RHI sample and the pcg twin's test pipelines). The
lint is a best-effort textual check against accidental direct creation, not against deliberately
adversarial source; code review and the coverage check are the backstops. This is RC-1's "every
shipped feature has a golden" for the pipelines the golden scenes create. What it cannot see, and the
scorecard keeps as a gap: passes without a pipeline (uploads, copies, clears), whether a bound
pipeline's output reaches the golden image, shaders not embedded in `helios_render` (cooked SPIR-V from
content, Phase 1), pipelines created only for inputs no scene uses (the record is filled by the
rendertest process, so e.g. a tonemap for a BGRA8Srgb swapchain is neither covered nor reported), and a
pipeline the lint cannot see: a name built by token pasting, a creation path (wrapper or member
pointer) defined outside the scanned files, which includes `engine/rhi/`, or source in a file with an
extension it does not scan. Future costs: `createShippedPipeline()` reflects its module on every call
(cache per module before 03 §1.7's thousands of PSOs), and a hot-reloaded pipeline whose entry points
or state change must re-register its name (Phase 2).

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
  (incl. never across concurrent queues), placement, partition, entry resolution, dumps, compile
  budget, and storage reuse: a 200-pass graph recompiled after other options, recompiled again (in
  the same memory) and rebuilt after `reset()` into a smaller graph and back must equal a fresh
  compile in every field (`fullPlanText`, list names and placement offsets included).
* `test_graph_stress.cpp` — 300 random DAGs × 2 frames × random options, executed on the Null
  backend (serial and parallel recording) and checked by an independent reference simulator
  (culling, schedule, happens-before of every conflicting pair per physical subresource, states,
  contents seen by every read, placement overlap). Mutating the compiler (no waits, no WAR barrier,
  alias without happens-before, submission-order barrier simulation, …) makes it fail. A second case
  compiles 150 random graphs into reused storage (after other options, as a recompile, and as another
  graph after `reset()`) and compares each plan with a fresh graph's.
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
