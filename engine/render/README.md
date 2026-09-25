# engine/render — Helios renderer (render graph v0, shader reflection, forward v0, ꟻLIP)

`helios::render` (target `helios_render`, L3, graphics-only; never linked by servers) builds on the
public `engine/rhi` API (ADR-003, `docs/plan/03-rendering.md` §1.7, §2, §8.4; WP-0.12).

| Header (`helios/render/…`) | Contents |
|---|---|
| `render_graph.h` | `RenderGraph`, `RgBuilder`, `RgContext`, `RgResourcePool`, the compiled `RgPlan`, `resolveEntryBarriers`, text/Graphviz dumps |
| `shader_reflection.h` | Helios shader reflection (`.hsr` v1 binary format, documented in the header), SPIR-V reflection, JSONC rendering |
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

**Budget:** compile of a 200-pass graph ≤ 2 ms on REF (measured: ~0.35 ms on the 4-core dev
container, `graph: compile budget`); the Phase 2 topology cache brings the unchanged case to ≤ 0.3 ms.

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
* `test_reflection.cpp`, `test_shaderc.cpp` — reflection of Slang output, hostile SPIR-V / `.hsr`
  input (huge member indices, cyclic types, string amplification); the real `helios-shaderc` end
  to end (byte-identical to the build's slangc, spirv-val, depfiles, errors, paths with shell
  metacharacters and non-ASCII names).
* `test_flip.cpp` — ꟻLIP stages against published CIELAB values, the analytic uniform case,
  properties, and agreement (~1e-6) with NVIDIA's reference implementation (flip-evaluator 1.7)
  on procedural images; PNG I/O.
* CTest `shaderc.spirv-val` runs `spirv-val` over every SPIR-V module the renderer, its tests and
  `helios-rendertest` embed (required when spirv-tools is installed, skipped otherwise).

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

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
