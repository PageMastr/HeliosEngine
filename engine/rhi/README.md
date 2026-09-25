# engine/rhi — Helios render hardware interface

`helios::rhi` (target `helios_rhi`, L3, graphics-only) is the thin, handle-based GPU layer of
ADR-003 / `docs/plan/03-rendering.md` §1: a **Vulkan 1.3** backend (volk + VMA) and a **Null**
backend for tests and GPU-less automation, behind one backend-agnostic API. No Vulkan type appears
in a public header (`src/vulkan/` is the only place that includes Vulkan), so a D3D12 backend can
slot in behind the same seam (Phase 3 seam test / Phase 4 gate).

| Header (`helios/rhi/…`) | Contents |
|---|---|
| `handles.h` | `BufferH`, `TextureH`, `PipelineH`, `SwapchainH` (core generational handles), `BindlessIndex`, `Queue`, `TimelinePoint`, `Backend` |
| `format.h` | `Format` (color, depth, BC), `FormatInfo`, row/surface sizes, `mipExtent` |
| `types.h` | Buffer/texture/view/sampler/pipeline descriptions, `ResourceState` + `Barrier`, dynamic-rendering attachments, copies, indirect layouts, swapchain types, diagnostics structs |
| `caps.h` | `AdapterInfo`, optional `CapBit`s, `Limits`, `Caps` |
| `device.h` | `DeviceDesc`, `Device` (resources, bindless, pipelines, submission, timelines, frames, swapchains, diagnostics) |
| `command_list.h` | `CommandList` (barriers, dynamic rendering, draws incl. indirect-count, dispatch, copies, labels, breadcrumbs), `ScopedLabel` |
| `null_device.h` | `NullDevice`: command-stream trace, validation errors, tracked states |
| `utils.h` | Blocking `uploadBuffer`, `uploadTexture`, `readbackBuffer`, `readbackTexture` |
| `rhi.h` | Umbrella |

```cpp
auto device = rhi::Device::create({.backend = rhi::Backend::Vulkan}).value();
rhi::TextureH target = device->createTexture(rhi::TextureDesc::tex2D(
    rhi::Format::RGBA8Unorm, 256, 256, rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc)).value();
rhi::PipelineH pso = device->createGraphicsPipeline(desc).value();          // SPIR-V from helios_shaders()
rhi::CommandList* cmd = device->acquireCommandList(rhi::Queue::Graphics, "Frame");
cmd->barrier(rhi::Barrier::textureState(target, rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget));
const rhi::ColorAttachment color{.texture = target};
cmd->beginRendering({.colors = {&color, 1}});
cmd->bindPipeline(pso);
cmd->pushConstants(MyPush{device->srv(texture), device->sampler({}), device->deviceAddress(buffer)});
cmd->draw(3);
cmd->endRendering();
rhi::TimelinePoint done = device->submit(rhi::Queue::Graphics, {&cmd, 1}).value();
```

## Model

* **Bindless everything.** One descriptor set (set 0, update-after-bind, partially bound) holds
  arrays of sampled images (≤ 131,072), storage images (≤ 16,384), storage buffers (≤ 65,536) and
  deduplicated samplers (≤ 128), clamped to device limits. Sampled textures and storage buffers get
  a slot at creation (`srv()`), storage-image mips and extra views on first request (`uav()`,
  `srv(tex, view)`). Slot 0 of every array is a default resource (magenta texture, …). One pipeline
  layout (the heap + 128 bytes of push constants) serves every pipeline, so layouts never
  invalidate PSOs. Buffers are reached through **device addresses** (every buffer has one). The
  shader side is `shaders/core/bindless.slang`.
* **States, not access masks.** Barriers name logical `ResourceState`s (`RenderTarget`,
  `ShaderResource`, `CopySource`, …); the backend derives stages, access masks and image layouts
  (sync2). The render graph (engine/render) will emit them; the Null backend validates them.
* **Timelines everywhere.** Each logical queue (`Graphics`, `AsyncCompute`, `Transfer`) owns a
  timeline semaphore; `submit()` returns a `TimelinePoint` and takes points to wait for. Queues
  without dedicated hardware alias the graphics `VkQueue` (Caps says which) but keep their own
  timelines, so code is identical on every adapter. Swapchain binary semaphores are bridged into
  the graphics timeline internally (`SwapchainImage::ready`).
* **Frames.** `beginFrame()` bounds CPU run-ahead to `framesInFlight` (default 2), recycles that
  slot's per-thread command pools and runs deferred deletions whose frame has completed.
  `destroy()` is deferred until the GPU is done with the current frame; handles go stale at once.
* **Parallel recording.** Command lists come from per-thread, per-frame pools: acquire on the thread
  (job) that records, `end()` there, submit anywhere. Submission order defines execution order.
  A list must be submitted exactly once, before the next `beginFrame()`/`waitIdle()` recycles it;
  both backends reject stale, recycled, foreign, duplicated and twice-submitted lists at submit
  (and lists still open on another thread), and report commands recorded into a closed list.
* **Logical queue rules.** Graphics work only on `Graphics`; compute pipelines, dispatches and
  `clearTexture` on `Graphics`/`AsyncCompute`; copies anywhere. They are enforced per logical
  queue even where an adapter aliases the queues, so code validated on lavapipe cannot break on
  hardware with dedicated compute/transfer queues.
* **Pipelines** are built from SPIR-V with dynamic rendering (no render passes, no vertex input:
  vertex pulling). `PsoPriority` other than `Immediate` compiles on `DeviceDesc::pipelineCompilePool`;
  draws with a not-yet-ready pipeline are skipped and counted (`MemoryStats::psoMisses`) instead of
  stalling. A `VkPipelineCache` can be seeded/serialized (`pipelineCacheData()`).
* **Conventions shared with engine/math:** clip space +Y up (negative-height viewport), CCW front
  faces, reverse-Z friendly defaults (`GreaterOrEqual`, clear depth 0), column-major matrices
  uploaded unchanged (`mul(M, v)` in Slang == `M * v` in C++), scalar block layout.

## Diagnostics (03 §1.5)

* Object names and labels through `VK_EXT_debug_utils` (RenderDoc, Nsight, RGP); `ScopedLabel`.
* Khronos validation with `DeviceDesc::validation` or `HELIOS_RHI_VALIDATION=1` when the layer is
  installed; messages go to the log and `DeviceDesc::onMessage`, errors to `validationErrorCount()`.
  RHI misuse that would be undefined behavior in the driver is reported the same way and the
  command dropped: stale handles, recording into a closed list, draws without a pipeline or index
  buffer, rendering-scope and queue violations, and out-of-range copies/fills/updates/indirect
  arguments (checked with the same `resolveCopyRegion` rules as the Null backend — a software
  driver would otherwise memcpy past its allocations).
* Each device owns its `VkInstance` and calls instance-level Vulkan through its own
  `VolkInstanceTable`; volk's process-global instance pointers are never loaded (a second device or
  `enumerateAdapters()` used to rebind them, leaving NULL WSI/debug-utils entry points behind).
  Code that needs volk globals (e.g. an ImGui Vulkan backend) must load them itself.
* Released bindless slots are re-pointed at the defaults (magenta texture, default storage image,
  default buffer) before reuse, so a stale index reads a harmless default rather than a destroyed
  view. Storage-buffer descriptors are clamped to `maxStorageBufferRange`; larger buffers are
  reached through their device address.
* **Breadcrumbs:** `CommandList::breadcrumb(passId, Begin|End)` writes `(frame << 16) | passId` per
  queue into host-visible memory; `breadcrumbs()` reads them.
* **Device loss:** every call that can see `VK_ERROR_DEVICE_LOST` reports once through
  `DeviceDesc::onDeviceLost` with `VK_EXT_device_fault` data (when supported), breadcrumbs and
  submitted/completed timeline values, then fails cleanly. `DeviceDesc::debugDeviceLostAfterSubmits`
  / `HELIOS_RHI_INJECT_DEVICE_LOST=N` injects a loss to test the handling path (both backends).
* `memoryStats()`: per-heap budget/usage (`VK_EXT_memory_budget`), bytes and counts; allocations are
  also attributed to core memory tags (`GpuBuffers`, `GpuTextures` or `BufferDesc::tag`).

Environment overrides: `HELIOS_RHI_ADAPTER` (index or name substring), `HELIOS_RHI_VALIDATION`,
`HELIOS_RHI_CAPS_MASK` (CapBits to keep, so CI can exercise fallbacks), `HELIOS_RHI_INJECT_DEVICE_LOST`.

## Null backend (03 §1.4)

Implements the whole API without a GPU and completes work at submit. It validates usage flags,
push-constant sizes, rendering scopes, queue capabilities, label balance and handle lifetimes
(including resources destroyed between recording and submission) at record time, and **resource
states per texture subresource in submission order** at submit time. Buffer and texture copies are
emulated on host memory, so upload → copy → readback round trips return real data. Every submit
appends to a canonical text trace (resources by debug name) — the trace goldens of 03 §8.4, which
run on every toolchain including GPU-less Windows CI (`null_device.h` documents the format).

## Shaders

`helios_shaders(<target> FILES a.slang …)` (`cmake/HeliosShaders.cmake`) compiles Slang to SPIR-V 1.6
at build time with the pinned slangc (`tools/prebuilt/fetch_slang.cmake`, SHA-256 verified, override
with `HELIOS_SLANG_ROOT`), tracks imports through slangc depfiles and embeds the words into the
target (`<target>_shaders.h`: one `std::span<const uint32_t> stem()` per file plus `find()`).
Entry points are declared in the source with `[shader("…")]`; see `shaders/README.md`.

## Tests

`rhi_tests` (doctest) — 30 CPU cases run by default: formats, the Slang toolchain (embedded SPIR-V,
entry points, layout decorations, stem lookup), and the Null backend (API behavior, validation, a
trace golden re-rendered for determinism, state tracking, timelines, memory emulation, copy-region
rules, list lifetimes, parallel recording on the job system, swapchain flow, device-loss
injection). The `gpu*` suites (21 cases) need Vulkan and run as the CTest entry `rhi_tests_gpu`
(label `gpu`): clears, a triangle and a bindless textured quad against golden PNGs in
`tests/golden/` (per-pixel tolerance, each rendered twice and required to be bit-identical), compute
through device addresses, bindless buffers and storage images, the matrix and winding conventions,
cross-queue timelines, upload/readback, deferred deletion, async PSOs, parallel recording,
breadcrumbs, injected device loss, the CPU recording budget (bind + push + draw ≪ 1 µs on REF;
~0.2 µs measured on lavapipe, timed without validation layers), misuse rejection (out-of-range
transfers, closed/stale/foreign lists, queue rules), several devices side by side, released
bindless slots, and a **windowless swapchain** (SDL's `offscreen` video driver +
`VK_EXT_headless_surface`: acquire/present/resize with pixel readback; skipped where the driver has
no headless surfaces). `rhi_triangle_smoke` runs the sample in a real window under `xvfb-run`.

The GPU suites are clean under `VK_LAYER_KHRONOS_validation` (core + synchronization validation,
checked with layer 1.3.275 on lavapipe). GPU-assisted validation of that layer version cannot
instrument modules that hold several stages (one SPIR-V module per `.slang` file) and crashes
lavapipe's compiler on the textured-quad shader; use a newer layer for GPU-AV runs.

```
cmake -S . -B build/rhi -G Ninja -DHELIOS_BUILD_GRAPHICS=ON
ninja -C build/rhi rhi_tests rhi_triangle
ctest --test-dir build/rhi -R rhi --output-on-failure      # add -L gpu / -LE gpu to select
HELIOS_UPDATE_GOLDENS=1 build/rhi/bin/rhi_tests -ts=gpu*    # re-bless goldens (inspect them!)
```

Goldens are rendered with lavapipe (Mesa 25.2); the tests prefer a software adapter so a machine
with a discrete GPU still compares against the same reference (`HELIOS_RHI_ADAPTER` overrides).
Machines without Vulkan can skip the gpu suites with `HELIOS_SKIP_GPU_TESTS=1`.

## Known limitations (Phase 0)

* Resources shared by several queue families use `VK_SHARING_MODE_CONCURRENT`; queue-family
  ownership transfers for exclusive images come with the render graph. Copies on a dedicated
  transfer queue are not yet checked against `minImageTransferGranularity` (1x1x1 on current
  desktop GPUs).
* Per-thread command pools are keyed by `std::thread::id` and kept for the device's lifetime:
  record on a fixed set of threads (the job system's workers), not on short-lived threads.
* Barrier stage masks cover all shader stages for shader states (no per-stage refinement yet).
* Not yet exposed: timestamp queries/GPU profiling, mesh-shader draws, ray tracing, set 1 (per-frame
  view constants), MSAA depth resolve, a driver-workaround table, VRAM-budget-driven eviction,
  pipeline libraries. `CapBit`s for mesh shaders, RT, descriptor buffers, GPL, pipeline binaries,
  present-wait and VRS report adapter support; the features are enabled by the phase that uses them.
* Swapchain surfaces come from SDL3 (`SwapchainDesc::sdlWindow`); `HDR10`/colorspace selection is
  Phase 4. Device loss is reported, not recovered (Phase 4 per 03 §1.5).
* The Null backend does not execute shaders or clears; texture contents only change through copies.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
