# 03 — Rendering

> **Status:** draft v5 (round-1 review fixes: §1.3a streaming residency, §4.6a acceleration structures, §5.5a
> terrain-generation throughput, §5.6a vegetation, §5.8a weather effects, RC-12/13; round-2 fixes: §1.3a
> `StreamTexId`-keyed u32 feedback, §5.4a horizon-map far terrain shadows, §5.8 cloud fast mode, §7.6a
> character composites and crowd impostors at scale, RC-8/10/11; round-3 fixes: §8.1 GPU columns for every
> gated scene, tier and frame rate, MIN BENCH-3 plan and VRAM check, render CPU at 45/30/120 fps, §4.5 capital
> shadow sizing, §7.6a character geometry (rest-mesh bakes, skinned-output ring), §7.6b faces, RC-8…11;
> round-4 fixes: §7.3 motion-vector contract, §7.6a previous positions on every skinned tier (ring caps
> 56/112 MB), §8.4a temporal-stability check through FSR 2 at 1.5× and 1.7×, RC-9/10).
> **Conforms to:** ADR-001,
> -003, -005, -006, -011, -012, -013.
> **Capabilities owned** (01 §2.6): R01, R02, R03, R07; the render half of W04, W05, R04, R05; the
> render-to-texture host for R06 (08 owns the UI).
> **Citations:** `R09-B5` = research 09 Part B §B5; `R09-RD-P0-3` = R09 renderer requirement P0 #3;
> `R06-ENG-09` = R06 requirement table; `R04-P0-10`, `R05-P0-2`, `R01-P1-15` = numbered requirements
> (01 §7). **Budgets are GPU ms on REF at 1440p High** unless marked MIN (1080p Low, upscaled).

## 0. Principles

1. **Budgets are features.** Every pass has a GPU budget in every scene, tier and frame rate that
   AAA-REN-1/2/3 gate: six REF columns and five MIN columns (§8.1), with render CPU budgets at 60, 45, 30 and
   120 fps. The nightly lab fails any regression > 5 %.
2. **One renderer from cockpit cup to star system.** Inputs are `(frameId, f64 local)`; frame chains
   compose in f64; the GPU sees camera-relative f32 (ADR-005, R09-B1).
3. **GPU-driven from Phase 1**, never retrofitted. The CPU issues ≤ 1,000 commands per frame.
4. **No stutter by construction:** vertex pulling, a closed set of shading models, material
   instances as data, recorded PSO lists, no PSO creation on the render thread (AAA-REN-4).
5. **Headless first.** Servers never link `rhi` or `render`; the Null backend tests the graph and
   pipeline without a GPU.
6. **Temporal amortization with explicit reactive masks**, because stars, beams and HUD glyphs break
   naive TAA (R09-B13).
7. **Seams before depth:** `IUpscaler`, the D3D12 seam, the visibility buffer and cluster LOD plug
   into the same GPU scene and render graph. Every feature is golden-tested from Phase 0 (AAA-REN-7).

---

## 1. RHI (`engine/rhi`)

A thin handle-based layer at Godot RenderingDevice / O3DE RHI level (ADR-003, R06-ENG-08). No
Vulkan type appears in a public header.

### 1.1 API sketch

```cpp
namespace helios::rhi {
template <class Tag> struct Handle { uint32_t index : 20, gen : 12; };  // generational, 0 = null
using BufferH = Handle<struct BufTag>;  using TextureH = Handle<struct TexTag>;
using PipelineH = Handle<struct PsoTag>;
using BindlessIndex = uint32_t;                      // slot in the global descriptor heap
enum class Queue : uint8_t { Graphics, AsyncCompute, Transfer };
struct TimelinePoint { Queue queue; uint64_t value; };

class Device {                                        // thread-safe unless noted
public:
    static Result<std::unique_ptr<Device>> create(const DeviceDesc&);  // Vulkan | Null | (D3D12)
    const Caps& caps() const;           // features, limits, vendor workarounds, cap mask
    BufferH  createBuffer(const BufferDesc&);          // desc: size, usage, MemUsage, name, tag
    TextureH createTexture(const TextureDesc&);
    BindlessIndex srv(TextureH, const ViewDesc& = {});  BindlessIndex uav(TextureH, uint32_t mip);
    BindlessIndex srv(BufferH);                         uint64_t deviceAddress(BufferH);
    BindlessIndex sampler(const SamplerDesc&);          // deduplicated immutable samplers
    PipelineH createGraphicsPipeline(const GraphicsPsoDesc&, PsoPriority);  // async
    bool isReady(PipelineH) const;
    void destroy(BufferH);              // deferred until all queues pass the current value
    CommandList* acquireCommandList(Queue);            // per-thread, per-frame pools
    TimelinePoint submit(Queue, std::span<CommandList* const>, std::span<const TimelinePoint> waits);
    Result<void> present(SwapchainH, TimelinePoint after);
    MemoryStats memoryStats() const;    // VK_EXT_memory_budget, per heap and memory tag
};
class CommandList {                     // owned by one job while recording
public:
    void barrier(std::span<const Barrier>);   // emitted by the render graph; manual use is linted
    void beginRendering(const RenderingDesc&);  void endRendering();
    void bindPipeline(PipelineH);  void pushConstants(const void*, uint32_t bytes);  // ≤ 128 B
    void drawIndexedIndirectCount(BufferH args, uint64_t off, BufferH count, uint64_t cOff, uint32_t max);
    void drawMeshTasksIndirectCount(/*…*/);   // Caps::meshShader only
    void dispatch(uint32_t x, uint32_t y, uint32_t z);  void dispatchIndirect(BufferH, uint64_t);
    void writeTimestamp(QueryPoolH, uint32_t);  void breadcrumb(uint16_t passId, Stage);
    void beginLabel(const char*, uint32_t rgba);  void endLabel();
};
}
```

**Bindless.** One global set, update-after-bind and partially bound: 131,072 sampled images, 16,384
storage images, 65,536 storage buffers, 128 immutable deduplicated samplers (clamped to device
limits). Set 1 is a per-frame ring of view constants. Everything else is **push constants (≤ 128 B)
carrying indices**. One pipeline layout serves every pipeline, so layouts never invalidate PSOs
(R06-ENG-09).

**BDA** is baseline (VMA, RT, descriptor buffers). Shaders reach buffers through a Slang
`BufferRef<T>` that lowers to a device address on Vulkan and a heap index on D3D12. Pointer
arithmetic outside it is a lint error.

### 1.2 Vulkan baseline and optional features

| Class | Features / extensions | Use |
|---|---|---|
| **Required** | Vulkan 1.3: dynamic rendering, synchronization2, timeline semaphores, maintenance4, BDA, descriptor indexing (runtime arrays, partially bound, update-after-bind, non-uniform), draw-indirect-count, MDI, subgroup ballot/arithmetic, `samplerFilterMinmax`, `shaderResourceMinLod` (streaming mip clamp, §1.3a), BC formats | All tiers, lavapipe |
| Optional | `VK_EXT_mesh_shader` | Meshlet path (MIN/RDNA1 lacks it) |
| | `VK_KHR_acceleration_structure` + `ray_query` | DDGI (Ph3, §4.6a), RT effects (Ph5) |
| | `sparseResidencyImage2D` + `sparseResidencyAliased` | Sparse texture streaming, a Ph4 option behind the §1.3a slot table |
| | `shaderInt64` | **Not required and not used by `hnoise`.** The terrain twin is 32-bit-only by lint (§5.5a); int64 appears only in the optional atomics below |
| | `VK_EXT_descriptor_buffer`, `VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_binary` | Heap updates, fast PSO link, binary cache |
| | int64 buffer/image atomics | Visibility-buffer variants |
| | `VK_EXT_memory_budget`, `_memory_priority`, `_pageable_device_local_memory` | Budgets, residency |
| | `VK_EXT_swapchain_colorspace`, `_hdr_metadata`, `_full_screen_exclusive` | HDR10/scRGB |
| | `VK_KHR_present_id/present_wait`, `VK_EXT_calibrated_timestamps` | Pacing, Tracy GPU zones, latency markers (08 §1.3a) |
| | `VK_NV_low_latency2`, `VK_AMD_anti_lag` | Vendor `ILatencyProvider`s (Ph3; 08 §1.3a) |
| | `VK_EXT_device_fault`, NV checkpoints, `VK_AMD_buffer_marker` | Crash diagnostics |
| | `VK_KHR_fragment_shading_rate` | VRS for clouds/particles (Ph4) |

Vulkan 1.4 is used opportunistically through the same caps bits (R09 overview). `Caps` carries a
**driver-workaround table** (vendor × driver version) and a cap mask (`HELIOS_RHI_CAPS_MASK`) that
disables optional paths, so CI exercises every fallback. *Verified in this container:* lavapipe
(Mesa 25.2.8) reports Vulkan 1.4.318 with mesh shaders, ray query, acceleration structures,
descriptor buffer, GPL, int64 image atomics, `shaderInt64`, `shaderResourceMinLod`, sparse binding and
2D sparse residency, and 1M update-after-bind images, but no `present_wait`, `device_fault` or
`pipeline_binary`. The GTX 1660 S and RX 5600 XT (MIN) expose `shaderResourceMinLod` too.

### 1.3 Memory (VMA 3.4) and VRAM budgets

Pools: **persistent** (GPU scene, geometry, textures; VMA custom pools, defragmented off-frame),
**transient** (graph aliasing heap), **upload ring** (256 MB on REF, 64 MB on MIN = two frames of MIN's 32 MB
upload cap; in the ReBAR heap when present, else host-visible staging that is not VRAM), **readback** 16 MB.
Geometry is one suballocated pool read by vertex pulling. Allocations carry ADR-011 tags; streaming pools
shrink on `memory_budget` pressure before the OS evicts (§1.3a).

| VRAM category (AAA-CNT-3) | REF | MIN |
|---|---|---|
| Render targets + transient heap | 1.2 GB | 0.5 GB |
| Texture streaming pool | 4.0 | 1.8 |
| Geometry pool | 1.5 | 0.8 |
| ↳ of which characters: skinned-output ring + per-appearance rest-mesh cache (caps, §7.6a) | 0.21 (112 + 96 MB) | 0.1 (56 + 48 MB) |
| GPU scene, materials, lights | 0.3 | 0.15 |
| Terrain tiles + virtual texture | 0.6 | 0.3 |
| Shadows (composition per preset in §4.5) | 0.5 | 0.25 |
| Volumetrics, particles | 0.65 | 0.25 |
| Upload/readback, UI/RTT atlases | 0.5 | 0.25 |
| RT acceleration structures | 0.5 | — |
| Reserve | 0.25 | 0.7 |
| **Total** | **10.0 GB** | **5.0 GB** |

The lines are checked per scene: §8.1's **VRAM check** fills every MIN line for BENCH-1 (260 characters) and
BENCH-3 (2,000 ships), the two scenes most likely to break the 5 GB ceiling, and RC-9/RC-10 gate each line.

### 1.3a Texture and geometry streaming residency (`render/streaming`)

**Split of responsibility.** 02 §5.7 decides which **containers** are resident: Requested → Resident →
Active, with `StreamingSource` lookahead. 08 §2.6's `StreamingInstaller` decides whether a pak byte range
is **on disk**. This section decides **how much of each resident asset is in VRAM**: texture mips and mesh
LODs. It never loads a container itself. A container's unload evicts everything it owns once 02's 10 s
minimum residency expires. Terrain tiles are generated, not streamed (§5.4–5.5a). The PVT (Ph3) shares the
feedback pass below but keeps its own page cache.

**Cooked layout** (T24/`helios-shaderc` cook, stored inside 02 §6.3's 256 KiB blocks):

| Asset | Always resident while its container is Resident (the "tail") | Streamed, one pak byte range each |
|---|---|---|
| Texture `.htex` (BC1/4/5/6H/7) | Header + **mip tail = every mip ≤ 64²** (≤ 5.4 KiB for BC7/BC5/BC6H, ≤ 2.7 KiB for BC1/BC4), packed in the asset's first block | Each mip > 64², largest first, 4 KiB aligned, so `demand(pak, range)` fetches exactly one mip |
| Mesh LOD chain | Bounds, `GpuMesh` header, meshlet table of the **coarsest LOD** (≤ 1/16 of LOD0's triangles) and the impostor, if any | Each finer LOD (vertices, indices, meshlets), one range per LOD |

Tails for a 16k-texture working set cost ≤ 90 MB, charged to the texture pool. Tails and coarsest LODs
mean a visible object is **never missing**, only blurry or coarse.

**Mip requests: GPU feedback, then CPU prediction.**
1. **GPU feedback (visible objects).** Forward shading (and the Ph4 visibility-buffer material pass)
   evaluates one pixel in each 4 × 4 tile per frame, rotating with the 16-phase TAA jitter so every pixel is
   sampled every 16 frames. For each *streamed* texture its material samples (the reflection blob marks
   them; ≤ 8 per material, layers included) it computes
   `λ = floor(log2(max(|∂uv/∂x|, |∂uv/∂y|)·size) + mipBias)`, where `mipBias` includes the upscaler bias
   (§7.3).
   - **Keyed by `StreamTexId`, not by bindless slot.** A stream-in moves a texture to a *fresh* bindless
     slot (below), so a slot-keyed buffer would lose the texture's feedback at every mip change. The shader
     takes the texture's stable `StreamTexId` from the material-instance data, which is the same ID it
     uses to fetch `GpuTexSlot`. It never uses the bindless index returned by that fetch.
   - **32-bit atomics.** GPU atomics are at least 32-bit, so there are two **u32 arrays indexed by
     `StreamTexId`**: `fbMip[]` (`InterlockedMin` of λ) and `fbCover[]` (`InterlockedAdd` of
     feedback-pixel count, which gives the `coverage` term below). Each is 512 KB for 131,072 IDs, so
     1 MB in total.
   - **Wave aggregation.** A wave-level scalarization loop runs over unique IDs in the wave
     (`WaveReadLaneFirst`, then `WaveActiveMin(λ)` and `WaveActiveCountBits` over the matching lanes).
     It issues one `InterlockedMin` and one `InterlockedAdd` per unique texture per wave.
   - **Rejected alternative:** a packed 4 × u8 word updated by a CAS loop. Thousands of waves per frame
     hit hot textures (terrain layers, hero skin), and CAS retries serialize under that contention.

   The prepass does the same for alpha-test textures, and the terrain shader for biome layers. Mesh decals
   write feedback. Particles, clustered-decal atlases and UI atlases are resident with their owner and are
   exempt. Cost ≤ 0.1 ms, inside forward opaque.

   **Compaction.** After forward opaque, a compaction pass runs one thread per ID (≤ 0.02 ms). For each
   touched entry it appends an 8 B record `{u32 StreamTexId, u8 mip, u24 coverage}` to a readback list
   capped at 16k entries (128 KB), and resets that entry to the arrays' initial values (`fbMip` = 0xFF,
   `fbCover` = 0). The list is read
   back two frames late through the readback ring. Neither array is ever cleared or read back whole. When
   the list is full, the remaining entries are simply not reset, so they accumulate into the next frame's
   list; no feedback is lost.
2. **CPU prediction (not yet visible).** In prepare, a job walks mesh–material pairs of instances in
   Resident or Active containers whose `t_reach` from any `StreamingSource` is < 5 s (02 §5.7, including
   the ship's 20 s velocity lookahead), and computes
   `mipCpu = log2(size / (uvDensity · metresPerPixel(dMin)))`. `uvDensity` is the cooked texels-per-metre
   (T28's texel-density metric), and `dMin` is the closest approach along the source's velocity over the
   next 5 s. It processes ≤ 20k pairs per frame, round-robin, and runs on the camera's new position right
   away after a camera cut, a zone `Reparent` or a warp exit (the `cut` hint in `RenderScene`).
3. **Target:** `target = min(gpuMip over the last 8 frames, mipCpu + 1)`, clamped to the texture's
   installed maximum. An optional HD pack (08 §2.6) that is absent caps it at 2048². Hysteresis: a target
   may only get *coarser* after 2 s without a finer request.

**Residency manager** (one render job, ≤ 0.5 ms CPU per frame on REF and MIN):
- **Priority** of a missing mip:
  `p = 4·min(deficit, 4)·√coverage + 2·p_src + pin`, where `deficit = residentMip − target`, `coverage`
  is the feedback pixel share, `p_src` is 02 §5.7's `w·saturate(1 − d/r_load) + 1/(1 + t_reach)` for the
  owning container, and `pin` = 8 for hero assets (the player's ship, cockpit, avatar and current target
  within 50 m, always full resolution).
- **Requests** go to the VFS as async ranged reads (IO pool, zstd decode). A range that is not on disk goes
  to `StreamingInstaller::demand`: visible deficits of ≥ 2 mips as **priority 1 (demand miss)**, and
  smaller visible deficits and lookahead as **priority 2 (destination)**, in 08 §2.6's order. Hero
  assets and the current planet's biome layers are prioritized via `prioritizeGroup` on approach.
- **Upload** runs on the transfer queue from the 256 MB upload ring, at most **64 MB per frame on REF
  and 32 MB on MIN**, highest priority first. Sustained throughput is bounded by 02's 150 MB/s I/O budget,
  not by this cap. Graphics waits on the transfer timeline point with queue-ownership acquire barriers
  emitted by the graph (§2.2). Without a dedicated transfer queue, copies go to async compute at ≤ 0.3 ms.
- **Eviction:** the pool target is `min(preset cap (§8.2), device-local budget − other categories)`,
  re-read every second from `VK_EXT_memory_budget`. When usage exceeds 95 % of the heap budget, the target
  shrinks 10 % per second until usage is < 90 %, never below 60 % of the preset cap. Victims are the top
  mips of the lowest-priority textures not requested for 5 s, least recently used first. Mips requested in
  the last 1 s are never evicted (thrash guard), and tails of referenced textures are never evicted. Without
  the extension, the pool target is the preset cap.
- **Defragmentation:** textures > 256² allocate from power-of-two size-class VMA pools; an off-frame pass
  moves ≤ 16 MB per frame on the transfer queue.

**Descriptor-stable updates.** Materials never hold a raw bindless index for a streamed texture. They hold
a stable **`StreamTexId`** into a persistent `GpuTexSlot{u32 bindless; f16 minLod; u16 flags}` table (8 B per
texture, 1 MB for 131,072), whose changed entries are written by the GPU-scene scatter (§2.6) at the start of
the frame's command stream. A VkImage cannot grow its mip count, so:
- **Stream in:** allocate image B with the new top mip, upload that mip, copy the resident mips A → B on
  the transfer queue, write B into a **fresh** bindless slot, and publish the new `GpuTexSlot` in the next
  frame's table. The old slot and image A are freed through deferred destroy (§1.1) once every queue has
  passed the frame's timeline value. No descriptor that an in-flight frame might read is ever rewritten.
- **Fade in:** `minLod` starts at 1 (the previous top mip, in B's numbering) and eases to 0 over
  0.25 s. `sampleStreamed()` in `helios.core` passes it as SPIR-V's `MinLod` operand
  (`shaderResourceMinLod`), so a new mip blends in rather than popping.
- **Stream out:** first raise `minLod` to clamp sampling to the retained mips (instant, descriptor-stable).
  The smaller image replaces the larger one lazily, at the next defrag slice or under budget pressure.
- **Ph4 option:** with sparse residency, one full-size sparse image per texture replaces the realloc and
  copy, behind the same `StreamTexId`. It ships only if realloc copies exceed 10 % of the transfer budget
  in the BENCH matrix.

**Geometry streaming (Ph1 to Ph4; Ph5 replaces it with 128 KB cluster pages, §3.3).**
- The culling pass (§3.2) writes `desiredLod[meshIndex]` with `InterlockedMin`. The array holds one u32
  per `GpuMesh` index, 256 KB for 65,536 meshes; the index is stable for the mesh's lifetime, and atomics
  are 32-bit. Touched entries go through the same compaction into the readback list. Culling draws
  `max(desiredLod, finestResidentLod)`, so an unstreamed LOD falls back to a coarser resident one and never
  to a hole. The same CPU prediction covers meshes that are not yet visible.
- Finer LODs suballocate in the geometry pool (a VMA virtual block, defragmented off-frame). A `GpuMesh`
  LOD's resident bit flips through the scatter only after its upload's timeline point, so a draw never
  reads partial data. §3.3's 8-frame dithered cross-fade hides the switch.
- Mesh uploads share the per-frame transfer cap and the priority queue with textures. Containers beyond
  `loadRadius` render as 02 §5.6 HLOD proxies.

**Telemetry and debug** (T26): a mip-deficit heatmap view, per-pool residency, request latency
histograms, the upload MB per frame and `stream_stall` events. The feedback pass also writes a 16-bin
histogram of `residentMip − target` weighted by coverage, which RC-12 reads.

| Streaming budget | REF | MIN |
|---|---|---|
| Residency CPU (feedback decode, prediction, requests, upload recording) | ≤ 0.5 ms/frame | ≤ 0.5 ms/frame |
| GPU feedback write + compaction | ≤ 0.1 + 0.02 ms (in forward) | ≤ 0.1 + 0.02 ms |
| Feedback memory / readback | 1 MB texture + 256 KB mesh arrays; ≤ 128 KB list per frame | same |
| Upload cap on the transfer queue | 64 MB/frame | 32 MB/frame |
| Feedback latency | 2 frames | 2 frames |
| Convergence after a camera cut (all visible texels within 1 mip) | ≤ 3 s | ≤ 3 s |

The acceptance criterion is **RC-12** (§9.3): BENCH-2 at 1,500 m/s and BENCH-6 at 300 m/s.

### 1.4 Null backend

Implements the whole API without GPU objects, validates usage (access vs usage flags, push-constant
sizes, handle lifetimes) and records a canonical **command-stream trace** (pass order, barriers,
aliasing, draws). It serves graph tests, GPU-less Windows CI and headless automation; 01 §5.4's
"golden images on the Null backend" means **trace goldens**. Cells and bots link no RHI.

### 1.5 Validation, naming, GPU crash diagnostics

- Every object gets a `VK_EXT_debug_utils` name and every pass a label, so RenderDoc, Nsight and RGP
  show the graph. CI runs Khronos validation; nightly adds sync and GPU-assisted validation.
- **Breadcrumbs:** each pass writes `(frame, passId, begin|end)` into a host-visible buffer
  (`fillBuffer`, or `buffer_marker`/checkpoints). On `DEVICE_LOST` the client puts the last completed
  and in-flight pass per queue, `device_fault` data, the graph trace and PSO names into the
  sentry-native minidump (ADR-013). Radeon GPU Detective uses the same labels; Nsight Aftermath is an
  optional proprietary plugin.
- Phase 1 reports and exits on device loss; Phase 4 attempts one device re-creation.

### 1.6 D3D12 seam and Phase 4 gate

- **Seam rules:** no Vulkan types above `rhi/src/vulkan`. The binding model maps to SM 6.6
  `ResourceDescriptorHeap` + root constants. Timelines map to `ID3D12Fence`. Barriers are graph states
  (`ShaderRead`, `ColorWrite`…), not Vulkan masks. Slang emits DXIL.
- **Phase 3 seam test:** a Windows-only `d3d12` backend passes the triangle, compute and bindless
  goldens. It keeps the seam honest and is not a product.
- **Phase 4 gate:** build the full backend (≈ 2 engineers × 6 months) only if REF Windows telemetry
  shows at least one of these:
  - Vulkan driver defects cost > 1 engineer-month per quarter;
  - a required presentation feature (HDR, VRR, overlays, anti-cheat, capture) is unreliable;
  - a D3D12 prototype of BENCH-1 is > 10 % faster;
  - a platform partner requires it.
  Otherwise Vulkan stays the only shipping backend.

### 1.7 Shader pipeline: Slang → SPIR-V

- **Toolchain.** Pinned Slang v2026.18.2. `helios-shaderc` (asset processor) emits SPIR-V 1.6 plus a
  **reflection blob** (`.hsr`) with:
  - entry points and workgroup sizes;
  - push- and specialization-constant layouts;
  - the material parameter layout (T16 edits instances from it);
  - declared resource accesses, checked against graph declarations;
  - a content hash for the DDC.
  `spirv-val` runs over every cooked shader in Linux CI; SPIRV-Reflect cross-checks in tests (R10 §3).
- **Modules:** `helios.core` (bindless, `BufferRef`, GPU-scene structs shared with C++ via generated
  headers), `helios.brdf`, `helios.lighting`, `helios.atmosphere`, `helios.materials` (`IMaterial`)
  and the pass entry points.
- **Permutation budget.** Permutations come only from shading model × vertex factory × pass, plus
  ≤ 8 static switches per material graph. Quality tiers use specialization constants. The cook
  enforces ≤ 64 PSOs per graph and ≤ 3,000 graphics + 500 compute PSOs per build (id Tech 7 ships
  hundreds, R06 §4.5).
- **PSO caching and precompilation** (R04-P0-10):
  1. The cook enumerates PSOs statically and merges **recorded PSO lists** (`.psol`) that
     instrumented nightly bot runs and QA playtests log.
  2. The client ships SPIR-V + `.psol`. On first launch, a driver change or a cache-UUID mismatch it
     compiles the list on the shader-compile pool (with launcher progress) into a `VkPipelineCache`
     (or `pipeline_binary` blobs) keyed by device UUID + driver version.
  3. The render thread never blocks. A draw whose PSO isn't ready uses its shading model's resident
     **fallback PSO**, and a `pso_miss` event feeds the next list. Zone entry preloads cue records to
     warm VFX PSOs.
  4. With pipeline libraries, prebuilt parts link in < 1 ms.
- **Hot reload:** the editor loads Slang; the asset processor recompiles dependents and pushes SPIR-V
  over the hot-reload socket; PSOs swap at a frame boundary. ≤ 2 s p95 (AAA-ITR-1, Ph2).
- **Plan B** (glslang): core passes stay in the GLSL-compatible Slang subset; graphs would need a
  GLSL emitter (~1 engineer-month).

---

## 2. Render graph and frame pipeline (`engine/render`)

### 2.1 API

FrameGraph shape (R06 §4.4): setup declares, compile plans, execute records.

```cpp
struct PrepassData { RgTexture depth, normals, motion; };
const auto& pre = graph.addPass<PrepassData>("DepthNormalPrepass", PassFlags::Raster,
    [&](RgBuilder& b, PrepassData& d) {
        d.depth   = b.depthAttachment(b.create("SceneDepth", TexDesc::d32f(rs)), Clear::depth(0.f));
        d.normals = b.colorAttachment(b.create("NormalRough", TexDesc::rgba16f(rs)), 0);
        d.motion  = b.colorAttachment(b.create("Motion", TexDesc::rg16f(rs)), 1);
        b.readIndirect(cull.args);  b.readIndirect(cull.count);
    },
    [=](const PrepassData&, RgContext& ctx) {
        ctx.cmd().drawIndexedIndirectCount(ctx.buffer(cull.args), 0, ctx.buffer(cull.count), 0, kMax);
    });
blackboard.add<SceneTextures>({pre.depth, pre.normals, pre.motion});
```

Transients belong to the graph; histories, GPU scene and swapchain are **imported** with their state.

### 2.2 Compile

1. **Cull** by reference-count flood from side-effect passes; hidden debug views cost nothing.
2. **Order:** declaration order within dependencies; async passes go into overlap windows.
3. **Aliasing:** first/last use per transient, greedy interval packing by memory type and alignment
   (`vmaCreateAliasingImage`), discard barrier on first use. Target: ≥ 30 % transient memory saved
   on the BENCH-2 graph.
4. **Barriers:** per-subresource state tracking; one `vkCmdPipelineBarrier2` per pass boundary;
   ownership transfers for exclusive images; `CONCURRENT` buffers across queues.
5. **Sync:** one timeline semaphore per queue, minimal waits.
6. **Partition** into 4–12 command lists balanced by last frame's costs, one job each.
7. **Cache:** an unchanged topology hash skips steps 1–5. Budget ≤ 0.3 ms CPU for 200 passes.

### 2.3 Async compute

| Graphics queue busy with | Overlapped on async compute |
|---|---|
| Shadow maps | Light/decal binning, GTAO, sky-view + AP LUTs, particle and precipitation sim/sort, wind field |
| Prepass → forward opaque | Terrain tile producer, TLAS/BLAS builds, DDGI trace + update, cloud raymarch, exposure histogram, character composites and rest-mesh bakes (§7.6a) |
| Any (transfer queue) | Streaming uploads and defrag copies (§1.3a) |
| Post | Next frame's GPU-scene scatter |

Async is a per-vendor toggle; the graph is correct serialized.

### 2.4 Recording and visualizer

Primary command buffers come from per-thread, per-frame pools; dynamic rendering removes secondaries.
The **visualizer** (ImGui, T26) shows the pass DAG, per-pass ms vs budget, lifetimes, aliasing and
barriers, inspects any resource, freezes culling and takes one-click RenderDoc captures (in-app API, MIT).

### 2.5 Frame pipeline: simulate N / extract / prepare / submit N−1 (R05-P0-2)

```
main jobs    | Sim N      |Extract N| Sim N+1                |Extract N+1|
render jobs  | Prepare N-1 → Record + Submit N-1 | Prepare N → Record + Submit N |
GPU          |       executes N-2                |       executes N-1            |
```

- **Extract** (sync point, ≤ 1.0 ms): **feature extractors** (Tatarchuk, R05 §2.3) copy dirty
  frame-local transforms, per-view `frameToView` (f64), camera, lights, cue events, skinning palettes
  and `UiDrawList`s into 02's double-buffered **`RenderScene` packet**. Coarse f64 culling of grids
  and planet tiles happens here.
- **Prepare** (jobs overlapping Sim N+1): f64 → f32 camera-relative frame transforms, scatter lists,
  planet quadtree selection, emitter tables, graph build.
- **Record + submit:** parallel recording, ordered submit, `present_wait` pacing, ≤ 2 frames in flight.
- **Low-latency mode** (the default in play; 08 §1.3a, CL-6). The diagram above is **throughput mode**. In
  low-latency mode:
  - 08's `FramePacer` delays the game thread's input sample just in time, so the render thread is idle when
    Extract N lands and prepares, records and submits N in the same frame period.
  - The graph is submitted in 3 ordered batches: (0) scatter, culling and shadows, plus async kicks;
    (1) prepass through forward opaque; (2) the rest, UI and present. The GPU starts ≤ 2.2 ms after extract.
  - The camera orientation is late-latched into batch 0's view constants. GPU culling, TAA and motion vectors
    read the latched views, and extract's CPU coarse culling uses a guard-banded frustum. CSM cascades are
    sphere-fitted, so they do not depend on rotation.
  - The GPU queue holds the executing frame plus the next batch 0, never N−2. Throughput mode remains for
    the editor viewport and offline capture.
- **Access rules:** feature renderers read only the packet and immutable render data. Packet memory
  comes from tagged frame heaps freed on the frame's timeline value (R06 §6.4); there are no locks.
  The render thread needs ≤ 4 ms wall on REF at 60 fps, ≤ 5 ms in BENCH-3 at 45 fps and ≤ 3.5 ms in
  Performance mode at 120 fps; MIN needs ≤ 6 ms at 60 fps and ≤ 7 ms at 30 fps (§8.1 CPU table, 02 §2.4).
- **Layering** (02 §1): `render` (L3) exposes a Godot-server-style handle API; 02's `presentation`
  (L4) implements the extractors over flecs, so `render` has no ECS types.

### 2.6 GPU scene

```cpp
struct GpuInstance {              // 64 B, persistent slot, scatter-updated
    float    localToFrame[3][4];  // grid/frame-local transform (f32 is ample inside a grid)
    uint32_t frameIndex;          // → GpuFrameXform for this view
    uint32_t meshIndex;           // → GpuMesh: LOD chain, meshlet ranges, bounds
    uint32_t materialIndex;       // → material instance parameters
    uint32_t flagsCustom;         // 8-bit flags | 24-bit per-instance data offset (livery, damage)
};
struct GpuFrameXform {            // 96 B per visible frame per view, uploaded every frame
    float frameToCamRel[3][4];    // rotation + (frameOrigin − cameraPos), computed in f64
    float prevFrameToCamRel[3][4];
};
```

- **Two-level transforms.** Instances are stored grid-local; prepare uploads one `GpuFrameXform` per
  visible frame (ship grid, station, planet tile, sector cell). A ship at 1,500 m/s updates one
  96-byte record instead of every instance aboard; the 2,000-ship BENCH-3 battle uploads ~200 KB.
- **Scatter:** `(slot, record)` pairs go through the upload ring into a compute scatter: 100k
  updates ≤ 0.2 ms.
- **Capacity:** 1M slots (REF) / 512k (MIN), with generational `RenderInstanceId`s. Procedural
  instances (scatter, asteroids, ring debris) are generated per frame into transient streams.
- Previous transforms for motion vectors are copied only on change.
- Frames within TLAS range also get a 48 B `GpuFrameAnchor` (frame → render anchor) for ray tracing
  (§4.6a).

### 2.7 Precision: camera-relative, reverse-Z, infinite far plane

- **Projection:** infinite reverse-Z,
  `P = [[f/a,0,0,0],[0,−f,0,0],[0,0,0,n],[0,0,−1,0]]` (`z_ndc = n/−z_view`), `D32_SFLOAT`, clear 0,
  `GREATER_OR_EQUAL`. Relative depth resolution ≈ 2⁻²³ everywhere, about 0.12 m at 1,000 km (R09-B1).
- **Near planes:** the world uses 0.1 m in depth range [0, 0.95]. The cockpit and first-person layer
  has its own FOV and a 1 cm near plane in [0.95, 1] (R09-B16).
- **Camera-relative:** frame chains (interior → grid → body → system → sector) compose in f64; only
  `frameOrigin − cameraPos` becomes f32. Error is one f32 ulp relative to distance, always sub-pixel.
- **Anchors:** GPU-persistent world data (particle space, froxel/cloud history, VSM pages, tile
  caches, the wind field, DDGI exterior cascades, the TLAS) lives in its own frame, or relative to a
  per-zone **render anchor** that snaps after 4 km of camera travel. Histories rebase by the exact f64
  delta.
- **Reparent:** extract emits a frame-change event and prepare re-expresses the previous transform
  in the new frame, so TAA doesn't smear at airlocks (BENCH-6).
- **10¹³ m test** (ADR-005, AAA-REN-5): a ship grid at 10¹³ m moving 1 km/s, camera in the cockpit,
  a 1 m cube in the grid 2 m away and a free object in the system frame 1 km away; 600 frames;
  centroid jitter < 0.05 px for both. Grid-local content is exact (02 §5.2). Free system-frame
  objects share the ~2 mm f64 step at 10¹³ m, which is sub-pixel beyond ~50 m.

---

## 3. GPU-driven geometry

### 3.1 Mesh data

Import (T24) uses meshoptimizer 1.2:
- vertex cache/fetch optimization;
- meshlets of ≤ 64 vertices / 124 triangles with sphere and cone bounds (`meshopt_buildMeshlets`,
  `meshopt_computeMeshletBounds`);
- up to 6 LODs via `meshopt_simplify`, with object-space error recorded.

Vertex streams are quantized: 3 × 16-bit positions (per meshlet for hulls > 500 m), octahedral
2 × 16-bit normal and tangent, half2 UVs.

### 3.2 Culling (per view, two-phase HZB)

1. **Phase 1, instances visible last frame:** frustum, layer mask, screen size and LOD select, then
   meshlet frustum, normal-cone and occlusion tests against last frame's HZB.
2. **Draw phase 1** → depth.
3. **HZB build:** `samplerFilterMinmax` single-pass downsampler, ≤ 0.1 ms.
4. **Phase 2:** everything rejected or untested is tested against the new HZB. Draw the newly
   visible and update visibility bits (Aaltonen & Haar 2015; niagara).

- **Mesh-shader path:** task shaders cull 32 meshlets each; one `vkCmdDrawMeshTasksIndirectCountEXT`
  per PSO bucket.
- **Fallback (MIN):** compute writes surviving triangles into a transient index buffer, with optional
  per-triangle culling (Wihlidal 2016), and issues one `drawIndexedIndirectCount` per bucket. With
  13 shading models a view has < 60 buckets.
- Shadow views skip HZB until VSM (Ph4) culls against page masks.
- Target: 1M instances, 200k visible, 30M pre-cull triangles → ≤ 0.5 ms.

### 3.3 LOD roadmap

- **Ph1:** discrete LOD by projected error (1 px on High) with an 8-frame dithered cross-fade that
  TAA resolves.
- **Ph3:** HLOD proxies for modular stations and settlements (T21, BENCH-5).
- **Ph4 "Nanite-lite" v1** for capitals and stations > 200 m:
  - Offline DAG: group 8–32 meshlets, simplify to half with locked borders, re-split, repeat
    (meshoptimizer's cluster-LOD reference).
  - Runtime: draw a cluster when `error(self) ≤ τ < error(parent)` (Karis 2021). Start with a flat
    parallel test; add BVH traversal above ~1M clusters. Hardware raster only.
- **Ph5:** streamed 128 KB geometry pages for all static meshes. A software rasterizer only if
  micro-triangles dominate profiles.

### 3.4 Fleets: impostors and brackets (R03, BENCH-3, R09-B15)

- **Octahedral impostors** (Brucks): 12 × 12 full-sphere views per hull/livery group in a 2048² atlas
  (albedo, normal, depth, emissive). Three views blend with depth parallax and are **lit at runtime**.
- **Bands:** mesh ≥ 48 px → impostor 6–48 px → **bracket** < 6 px (EVE-style sprites; 08 owns
  content, 03 the instanced sprite pass). Sizes are display pixels; the mesh band's start is a preset setting
  (§8.2: 64 px on Low, 56 on Medium, 48 on High, 40 on Ultra).
- Hulls are instanced; liveries and damage are per-instance data (R01-P1-15).
- BENCH-3 (2,000 ships, 20 capitals; the client view of the NS-4.2 battle, 01 §3.2):
  - **REF High:** fleet geometry ≤ 4.5 ms across prepass, shadows and forward; the band thresholds keep
    ≤ 250 hulls in the mesh band at the BENCH-3 camera.
  - **MIN Low:** the mesh band starts at 64 px, so ≤ 120 hulls are in it. Fleet geometry is ≤ 6.5 ms on the
    compute-culling path (no mesh shaders). Only 2 capitals get dedicated shadow maps (§4.5), and the
    particle significance pass keeps 256k (§6.1). §8.1 has the full MIN BENCH-3 column and VRAM check.

### 3.5 Visibility buffer (Phase 4, R06-ENG-29)

- Raster writes `R32_UINT = cluster << 7 | triangle` plus depth, with one PSO per vertex factory.
- A compute pass classifies 8 × 8 tiles by material and dispatches indirect shading per material with
  analytic barycentric derivatives (Burns & Hunt 2013; Karis 2021), reusing the clustered lighting code.
- Transparents, particles, hair and view models stay forward.
- It is enabled per scene when forward opaque exceeds budget in BENCH-3/5; goldens prove parity.

---

## 4. Materials and lighting

### 4.1 BRDF and shading models (R09-B7)

- **Standard:** GGX with height-correlated Smith and Lambert diffuse, with multiple-scattering
  compensation (Fdez-Agüera 2019). Metal/roughness workflow (Karis 2013; Lagarde & de Rousiers 2014).
- **Extensions:**
  - clear coat (second lobe, F0 0.04) for painted hulls;
  - anisotropic GGX (Kulla & Conty 2017) for brushed metal;
  - Charlie sheen (Estevez & Kulla 2017);
  - **emissive in nits** with animated masks driven by attributes such as `Ship.Power.Offline`.
- **Closed set of 13 shading models:** Standard, ClearCoat, Cloth, Skin, Hair, Eye, Unlit, Hologram,
  Shield, Glass, Terrain, Water, **Foliage** (two-sided, thin-surface transmission, §5.6a). A new one needs
  render-lead review, because the set bounds PSOs.
- **Weather response** (wetness, puddles, snow cover; §5.8a) is a surface modifier in `helios.materials`
  applied to Standard, ClearCoat, Terrain and Foliage. It is not a shading model and adds no permutation;
  a specialization constant removes it in space.
- **Textures:** KTX2-derived `.htex` (BC7, BC5, BC6H, BC4), mip-streamed from GPU feedback and CPU
  prediction (§1.3a).

### 4.2 Hard-surface content

- **Layered materials:** base + ≤ 4 layers (paint, dirt, wear, burn) blended by curvature, AO and
  vertex-color masks. Livery, wear and damage are per-instance data: hull + faction + livery are data,
  not assets (EVE SOF, R01-P1-15).
- **Trim sheets** on UV1, masks on UV0; T28 validates texel density.
- **Decals:** mesh decals for greebles. **Clustered decals** (Sousa & Geffroy 2016) are box-projected
  from bindless atlas pages and binned with lights: ≤ 4,096 (REF) / 1,024 (MIN). Scorches and faction
  marks arrive as cues (06 §1.5).

### 4.3 Material graph → Slang (T16)

```slang
interface IMaterial {
    associatedtype Params;                                // layout reflected for instance editing
    SurfaceOutput evalSurface(SurfaceInput s, Params p);
    float  evalOpacity(SurfaceInput s, Params p);          // masked domains only
    float3 evalVertexOffset(VertexInput v, Params p);      // bounded WPO (declared max)
}
struct ForwardPass<M : IMaterial> { /* shared lighting code */ }
```

Nodes lower to SSA and then to a Slang type implementing `IMaterial`, deterministically (stable GUID
order) and content-hashed into the DDC. **Instances** are pure data, edited without recompiling
(R08-ED-P0-12). Only masked or WPO materials get their own depth and shadow PSOs. T16 shows
instructions, samplers, permutations and PSOs against the §1.7 caps; the cook fails when a graph
exceeds them.

### 4.4 Clustered forward+ (R09-B11, R09-RD-P0-5)

- **Structure:** 2D tile bitmasks + 1D z-bins (Drobot 2017), refining froxel clustering (Olsson et al.
  2012; DOOM 2016). Lights are sorted by view depth; each 64 px tile holds a bitmask over that list,
  and each of 64 exponential z-bins holds an index range. Shading walks `tileMask ∩ zbin`.
- **Capacity:** 4,096 visible lights on REF (≈ 470 KB at 1440p), 1,024 on MIN. Decals and probes use
  parallel lists.
- **Light types:** point, spot, rect/tube area (LTC, Heitz et al. 2016), and ≤ 4 directional (sun,
  companion star, planetshine, art key). Plumes, muzzle flashes, running lights and holograms
  register lights.
- Binning ≤ 0.2 ms, async.

### 4.5 Shadows

- **Sun (Ph1):**
  - 4 cascades (2048² REF / 1024² MIN), PSSM λ 0.8, sphere-fitted and texel-snapped (rotation-invariant, which
    the camera late latch relies on, 08 §1.3a), 5 × 5 PCF with receiver-plane bias.
  - **In space**, cascades fit the player's grid plus nearby grids rather than the frustum.
  - **Capital ships** within 20 km get dedicated maps, ranked by projected size: **4 × 4096² on High and
    Ultra, 4 × 2048² on Medium, 2 × 2048² on Low (MIN)**. Other capitals use the cascades. The maps are
    **D16**, because each map's orthographic depth range is fitted to its capital's bounding sphere
    (≤ 2.5 km). A D16 step is then ≤ 4 cm, well under the 5 × 5 PCF footprint.
  - **Range:** cascades end at `csmFar`, which §8.2 sets per preset (1 km on Low, 2 km on Medium and High,
    4 km on Ultra before VSM). Cascade 3 fades out over its last 10 %.
- **Far terrain shadows (Ph1, §5.4a):** per-tile **horizon maps** from the tile producer. They cover
  terrain self-shadowing beyond `csmFar`, from 1 km to orbit. Inside the CSM range they also cover distant
  ridges outside the cascades' caster volume, combined as `min(csm, horizon)`. They are the only terrain
  shadowing in the orbit shading path.
- **Eclipses:** analytic sphere occlusion (moons on planets, planets on rings).
- **Local lights:** an 8192² (REF) / 4096² (MIN) **D16** atlas (linear depth ranges ≤ 64 m) with ≤ 32
  tile updates per frame (≤ 16 on Low and in Performance mode). **Static casters are cached in grid
  space**, so interior shadows stay valid while the ship flies.
- **Contact shadows:** a 16-step screen-space march for the sun.
- **Ph4 virtual shadow maps** (R06-ENG-26): 16k² virtual clipmap levels in 128² pages; physical pool
  8192² (REF) / 4096² (MIN). Pages are marked from depth and cached with GPU-scene invalidation;
  moving ships use dynamic pages, while terrain and stations stay cached. VSM replaces the cascades
  and the capital maps where it is on (Ultra).
- **Ph5:** ray-query shadows on SHOWCASE.
- **Shadow memory against §1.3's line:**

  | Resource | Low (MIN) | High (REF) | Ultra with VSM (Ph4) |
  |---|---|---|---|
  | Sun cascades (D32) | 3 × 1024² = 12 MB | 4 × 2048² = 64 MB | — (VSM pool 8192² D32 = 256 MB, page tables 8 MB) |
  | Local-light atlas (D16) | 4096² = 32 MB | 8192² = 128 MB | 8192² = 128 MB |
  | Capital maps (D16) | 2 × 2048² = 16 MB | 4 × 4096² = 128 MB | — (dynamic VSM pages) |
  | **Total / §1.3 line** | **60 MB / 0.25 GB** | **320 MB / 0.5 GB** | **392 MB / 0.5 GB** |

  Horizon maps (§5.4a) belong to the terrain line, and filter scratch belongs to the transient heap.

### 4.6 IBL, probes, GI (R09-B14)

- **Space IBL:** the region's nebula cubemap (§5.2), GGX-prefiltered plus L2 SH: nebula-tinted rim
  light against a hard sun, the EVE look (R09-B5). In atmosphere, a 128² sky cube from the sky-view
  LUT, one face per frame (≤ 0.15 ms).
- **Reflection probes** are box-projected and **parented to grids**; from Ph3 they are
  **relightable** (stored albedo/normal/depth cubes relit each frame).
- **GI:** Ph1 IBL + GTAO + probes → Ph3 grid-parented DDGI (Majercik et al. 2019) via ray queries on
  REF, raster-captured SH probes at assembly/cook time on MIN → Ph5 ReSTIR on SHOWCASE. No
  lightmaps: modular ships invalidate them.
  - **DDGI volumes (REF, Ph3):**
    - *Grid volumes:* authored per ship or station interior, parented to the grid, with 1.5 m probe
      spacing (a Mule interior ≈ 1,800 probes). A quarter of the probes update per frame at 144 rays each.
    - *Exterior:* camera-centred scrolling cascades on planet surfaces, 3 × (24 × 12 × 24) probes at 2, 8
      and 32 m spacing. They scroll in whole-probe steps in render-anchor space; an eighth of the probes
      update per frame at 128 rays each.
    - Irradiance uses 8 × 8 octahedral texels and visibility 16 × 16, with probe classification and
      relocation. Hysteresis is 0.97, so a rotating ship's sunlit windows converge in ≈ 0.5 s.
    - Trace and update ≤ 0.5 ms on async compute; forward shading samples the probes.
  - **Assembly-time probes (MIN and non-RT GPUs):** T21 assembly (ships, modular bases) and the cook
    (authored settlements) raster-capture L2 SH probes on the same 1.5 m / 2 m grids. They store sky
    visibility and bounce albedo, so they are **relit** every frame from the current sky SH and sun
    (time of day and weather still apply).
  - Acceleration structures: §4.6a.

### 4.6a Acceleration-structure management (REF, Ph3; RT effects Ph5)

- **Space.** One **TLAS is rebuilt every frame in render-anchor space** (§2.7): the anchor snaps after 4 km
  of camera travel and the TLAS range is ≤ 1 km, so coordinates stay below ~5 km and the f32 ulp stays
  ≤ 0.5 mm in a 10¹¹ m system. A compute pass writes each `VkAccelerationStructureInstanceKHR` from
  `GpuInstance.localToFrame × frameToAnchor`. `frameToAnchor` is a 48 B `GpuFrameAnchor` record, uploaded
  only for frames within TLAS range and computed in f64 in prepare like `frameToCamRel`, so a moving ship
  still updates one record. Probe ray origins (grid-local) go through the same matrix. Ray `tMin` is
  1 cm.
- **Range limit against the 1M-slot GPU scene.** A compute pass (≤ 0.1 ms) selects instances whose bounds
  intersect the union of active DDGI volumes, each expanded by its maximum ray length (64 m interior, 512 m
  for the outer exterior cascade). The cap is **16,384 TLAS instances**. On overflow, a two-pass histogram
  keeps the highest `projectedSize × 1/(1 + distance)`; overflow counters go to T26.
- **Membership:**

  | Content | In the TLAS? | BLAS |
  |---|---|---|
  | Static meshes, hulls, station and ship modules | Yes | Per mesh at its **RT LOD** (the first LOD with ≤ 25 % of LOD0's triangles); each T21 module is its own instance, so assembly builds no BLAS |
  | Terrain tiles | Only tiles within 1 km of an active exterior volume | Built from the tile's height atlas at 33² (2,048 triangles) when the tile is produced; ≤ 8 builds per frame; ≤ 256 resident; freed with the tile |
  | Ocean | Sea-level plane proxy | Shared |
  | Scatter rocks and tree trunks | Within 128 m | Shared per-mesh BLAS; leaf cards are excluded, and trunk proxies carry `OPAQUE` |
  | WPO materials | Yes, undeformed | Per mesh |
  | Skinned meshes | **Excluded in Ph3**; Ph5 refits ≤ 16 nearest at 15 Hz | Refit-only pool (Ph5) |
  | Particles, grass, impostor-band ships, brackets | Never | — |

- **BLAS pool.** BLASes build lazily on first TLAS inclusion, not on load, with
  `PREFER_FAST_TRACE | ALLOW_COMPACTION`, and are compacted the following frame (size query, copy, free:
  ≈ 50 % saving). LRU eviction keeps the pool within its share of §1.3's 0.5 GB line: BLAS 0.35 GB, terrain
  BLAS 0.1 GB, TLAS + scratch 0.05 GB. An instance whose BLAS is not built yet is skipped for that frame;
  probe hysteresis hides it.
- **Budget (async compute):** TLAS build ≤ 0.15 ms for 16k instances; BLAS builds and compaction copies
  ≤ 0.3 ms (≈ 200k triangles per frame, queued nearest first); **≤ 0.5 ms total**. It appears as
  "DDGI + AS" in §8.1.
- **Tests.** Lavapipe has ray query, so DDGI goldens (Kestrel interior, Saltmarch exterior at dusk) run in
  the per-commit suite at 320 × 180 with reduced probe counts. The cap-masked MIN path (assembly-time
  probes) has its own goldens. Hardware goldens run nightly (RC-11 Ph3).

### 4.7 Fog, GTAO, SSR

- **Froxel fog** (Wronski 2014; Hillaire 2015):
  - 160 × 90 × 64 at 1440p, exponential to 2 km.
  - Injects height fog, volumes, combat dust and nebula edges.
  - Lit by CSM, the far terrain shadow term (§5.4a, so ridges cut shafts through haze at sunset) and
    clustered lights, with blue-noise jitter and 0.9 temporal history.
  - ≤ 0.7 ms.
- **GTAO** (Jimenez et al. 2016): half resolution, denoised, bent normals. ≤ 0.5 ms.
- **SSR:** Hi-Z trace (Stachowiak 2015) on reprojected last-frame HDR at half resolution, with probe
  fallback. ≤ 0.6 ms.
- The prepass writes a **thin G-buffer** (normal/roughness, motion) for GTAO, SSR, contact shadows and
  upscaling.

### 4.8 Pass order (forward path)

Streaming uploads (transfer queue) → scatter → FACS shapes (F0 faces) + skinning into the ring (§7.6a–b) →
cull 1 + prepass → HZB → cull 2 + prepass → [async: binning, GTAO, sky/AP LUTs, tiles + horizon maps, wind
field, particle and precipitation sim, character composites and rest-mesh bakes, TLAS/BLAS builds, DDGI
trace + update] → shadows → froxel fog → forward opaque (+ streaming
feedback, then feedback compaction) → sky + far layer →
clouds/nebula → SSR → transparents (sorted, WBOIT, particles, precipitation, shields, distortion) →
TAA/upscale → star layer → exposure, bloom, flares, motion blur, DoF, tonemap + grade → UI/HUD →
present.

---

## 5. Space and planet rendering

### 5.1 Starfield (R09-B5, R09-RD-P0-9)

- **Data:** the galaxy model (02/T07) supplies catalogue stars as `{octDir 32b, magnitude f16,
  temperature u16}`: 8 B each, 8 MB per million. Background stars are hashed per direction cell.
  Directions are recomputed per system.
- **Rendering:** PSF sprites whose integrated energy follows magnitude (`E = E₀·10^(−0.4m)` lux), at
  least 1.5 px wide. Stars go through exposure, so they vanish in daylight and toward the sun. On
  planets the transmittance LUT attenuates them and adds scintillation. A galactic-band layer is baked
  from catalogue density.
- **Temporal:** the star layer composites at **display resolution after upscaling**, before bloom,
  masked by the max-filtered sky mask. Stars never enter TAA history (R09-B13).

### 5.2 Nebulae

- **Tier A (Ph1), the EVE look:** a cook-time raymarch of procedural emission/absorption density lit
  by embedded stars bakes a per-region BC6H cubemap (2048²/face) with GGX mips and SH. It is both
  backdrop and IBL.
- **Tier B (Ph3 v1, Ph5 full), fly-through:**
  - A sparse brick map (8³ bricks: density + RGB emission) authored in T07/T18.
  - Raymarch at quarter resolution, 32–64 steps, blue-noise jitter, occupancy-mip empty-space skipping.
  - 1/16 temporal update with bilateral upsample; the froxel volume covers the near field.
  - 1.5 ms.
  - Local volumes are excluded from the region bake, so nothing double-counts.

### 5.3 Suns and lens flares

- **Sun:** a limb-darkened disc from temperature and luminosity; illuminance `E = L/(4πd²)` ×
  luminous efficacy (≈ 120–130 klux at 1 AU); blackbody color × atmospheric transmittance, so red
  dwarfs and blue giants light correctly (R09-B2/B6). The corona is an art-directed HDR billboard.
- **Flares:** a compute pass takes 64 blue-noise depth samples around the sun and writes a
  time-smoothed visibility value. Ghosts, halo, anamorphic streak and starburst draw with an indirect
  instance count from it: no CPU readback. Cheap screen-space ghosts handle other bright sources.

### 5.4 Planet terrain: cube-sphere CDLOD (R09-B3, R04-P1-12, W04)

- **Topology:** 02's cube-sphere: 6 quadtree faces with tangent warp `u' = tan(u·π/4)` and
  `TileKey{face, level, x, y}`. Render and collision tiles are the same tiles.
- **Nodes:** one 64 × 64-quad tile (65² samples) with geomorphing (Strugar 2009): no cracks, no
  skirts, no pops.
  - Depth for ≤ 0.5 m spacing: 17 levels on Harrow (1,500 km radius), 19 at 6,400 km (AAA-CNT-1).
  - Morphing covers the last 30 % of each range, with range ratio 2.0.
- **Selection:** f64 CPU traversal with frustum and **horizon culling** (Cozzi & Ring). It yields
  `(TileKey, morph)` records drawn as one instanced indirect draw. Each tile has an f64 centre with
  tile-local f32 vertices.
- **Tile producer** (async compute):
  - Writes height (R32F), octahedral normal and top-4 biome weights into an atlas of 65² tiles plus
    borders: 4,096 tiles (~180 MB) on REF, 2,048 on MIN, LRU eviction.
  - Throughput ≤ 64 tiles per frame, prioritized by screen error, in 0.8 ms. §5.5a sizes this against
    demand, validates it on MIN, and pre-decides the fallbacks.
  - Levels 0–3 of every body in view (6 × 85 = 510 tiles, ~22 MB) stay resident. Missing tiles fall back
    to the upsampled parent, so there are never holes or waits.
  - **Trajectory prefetch:** each frame, selection also runs for the camera's predicted positions at
    +1, +2 and +4 s along 02's `StreamingSource` velocity. Tiles found only there are queued at lower
    priority than visible tiles.
- **Runtime stamps** (housing flattening, craters; 06 §9) are replicated lists applied as the last
  affector; overlapping tiles regenerate.
- **Horizon maps** for far terrain shadows come from the same producer (§5.4a).

### 5.4a Far terrain shadows: per-tile horizon maps (Ph1)

**Gap it closes.** CSM ends at `csmFar`, 2 km on High (§4.5). During BENCH-2's descent, from 1 km to
400 km altitude, everything else is terrain that must self-shadow: at low sun, a 3 km ridge casts a
57 km shadow at 3° elevation. The orbit shading path (§5.6's 64² baked albedo) must show the same shadows,
so that terminators have relief.

**Why horizon maps rather than a per-pixel heightfield raymarch.** A horizon map (Max 1988) stores,
per terrain sample and azimuth, the elevation angle of the highest terrain along that direction.
- It is **independent of the sun**: time of day, a second sun and eclipses cost nothing extra.
- It is **cached with the tile**, so it is computed once rather than every frame.
- It serves terrain, far receivers, froxels and the orbit path from **one lookup**.

A screen-space raymarch beyond the last cascade would have to be redone every frame at full resolution
whenever the camera or sun moved, and would not serve the orbit path.

**Generation** (`visualOnly`, float math allowed, §5.5; async compute, inside the "Terrain tiles" row):
- **Resolution.** For each produced tile, a horizon pass writes a **33² × 8-azimuth** map (17² on Low):
  8 u8 horizon angles per sample, stored as two RGBA8 texels in a parallel atlas layer.
  - Size: 8.5 KB per tile (2.3 KB on Low). That is 35 MB for REF's 4,096 tiles and 4.6 MB for MIN's
    2,048 on Low, charged to the §1.3 terrain category.
  - Encoding is piecewise linear, finest where sunsets need it: −16°…0° in 32 codes (0.5°), 0°…20° in
    160 codes (0.125°), 20°…80° in 63 codes (≈ 0.95°).
- **Marching.** Rays from each sample follow the 8 great-circle directions of the tile's tangent frame,
  with 24 geometric steps: the first at 2 sample spacings, then a ratio of 1.3, reaching ≈ 3,600 spacings.
  - The first step skips local relief, which CSM, contact shadows and the tile normal handle.
  - Rays stop at `min(3,600·s, √(2·R·h_max))`, where `h_max` is the body's maximum relief, known
    statically from the T04 graph. On Harrow (R = 1,500 km; the reference graph's h_max ≈ 7.5 km) that is
    150 km, the geometric horizon of the highest peak, so no caster is missed. On a 6,400 km body with the
    same relief it is ≈ 310 km.
  - Examples: at 5 km altitude the finest active tile edge is ≈ 1.25 km (§5.5a), so s ≈ 39 m and rays
    reach 141 km. At 50 km altitude s ≈ 390 m and rays hit the cap.
- **Height lookup.** Each step reads `heightAt(p, s_step/2)`. A GPU tile hash (`TileKey → atlas slot`,
  8,192 open-addressed entries, maintained by the producer) finds the finest resident tile whose spacing
  is at least that fine, or else the finest resident ancestor. Levels 0–3 are always resident, so every
  lookup resolves.
- **Elevation.** The elevation angle is computed from true 3D positions against the sample's local tangent
  plane, so planet curvature (horizon dip, ≈ 4.5° from a 5 km peak on Harrow) is exact.
- **Scheduling** (it shares the producer's 0.8 ms async slice, §5.5a):
  - Height tiles always go first. The horizon pass gets **≤ 0.15 ms per frame, taken only from the
    remainder the height tiles leave**. §5.5a's 64-tile burst capacity and RC-13 are therefore unchanged,
    and the "Terrain tiles" row stays 0.8 ms.
  - Estimated rate: ≈ 8 tiles per 0.15 ms on REF (33²) and ≈ 12 on MIN (17²). Against BENCH-2's steady
    3.3 tiles per frame that is 2.4× and 3.6× headroom. WP-1.8 measures it next to RC-13.
  - During a cold-set burst (≈ 300 tiles), horizons lag by ≤ 40 frames. Meanwhile a tile uses its parent's
    horizon map, upsampled, exactly as heights fall back, so shadows are soft for up to 0.7 s but never
    missing.
- **Invalidation:**
  - A horizon evaluated before its eight same-level neighbours were resident is flagged. It is re-run once,
    at the lowest priority, when they arrive.
  - A runtime stamp (§5.4) re-queues horizons for resident tiles within 4 × its radius.
  - The map is evicted with its tile.

**Application** (≤ 0.05 ms, inside forward opaque; §8.1 sub-budget):
- **Per-pixel visibility.** Terrain shading bilinearly samples the tile's map for the two azimuths that
  bracket the sun's and interpolates the horizon angle `h`. It then computes
  `vis = smoothstep(h − r, h + r, e_sun)`, where `e_sun` is the sun's elevation at the point and
  `r = max(sun angular radius, 0.5°)`. That gives soft, alias-free penumbrae.
  - Two suns take one lookup each.
  - The result multiplies the analytic eclipse term and the cloud shadow map (§5.8).
- **CSM blend.** Inside the CSM range, `sunVis = min(csm, vis)`: CSM owns local casters, and the horizon
  adds ridges outside the cascades' caster volume. Using `min` rather than a product avoids darkening twice.
  Over cascade 3's last 10 %, CSM fades out and `vis` remains. Ph4 VSM replaces CSM inside its clipmap;
  horizon maps still serve beyond it and in orbit.
- **Non-terrain receivers beyond `csmFar`** (tree impostors, HLOD proxies, visual scatter, settlement
  proxies) read `vis` for the terrain point beneath them through the tile hash. Because they stand above
  that point, their shadow edge can be up to one horizon sample early, which is invisible at > 1 km.
- **Froxels.** Froxel fog lights froxels beyond `csmFar` with the same `vis`, so ridges cast visible shafts
  through haze (inside the froxel row).
- **Orbit path** (> 100 km, §5.11). Per-pixel tile shading lights the baked 64² albedo with the tile normal
  × `vis`, so mountain shadows lengthen across the terminator. Far-layer bodies (> 2,000 km, §5.7) skip it,
  since their shadows are sub-pixel.

**Acceptance** (RC-11 Ph1; lavapipe goldens at 320 × 180 per RC-1):
- **Harrow sunset goldens** at **5 km** and **50 km** altitude, with sun elevation 3° and 8°, looking
  across the sun azimuth over the reference ridge set. Mountain shadows must be present and must match a
  `helios-rendertest` reference with ꟻLIP mean ≤ 0.02. The reference is a single 16k² terrain-only shadow
  map over 20 km (≈ 1.2 m texels) at 5 km, and a CPU ray-traced heightfield at 50 km.
- An **orbit terminator golden** at 400 km.
- A 10 s fly-through across `csmFar` with no visible seam: per-frame ꟻLIP against the same reference
  ≤ 0.02 inside the fade band.

### 5.5 Deterministic heights shared with the server

Collision on client and server uses 02's deterministic `pcg` bytecode VM for the T04 graph
(R08-ED-P0-07), bit-identical everywhere (AAA-CNT-1, AAA-PLT-4). The client also builds near-field
tiles on the CPU for collision. 03 generates **every visual LOD on the GPU** from the same graph
compiled to Slang. The reason is CPU headroom and burst latency, not steady-state demand (§5.5a). BENCH-2's
steady demand (≤ 200 tiles/s) would cost ≈ 10 % of one core, but a cold set after a camera cut or warp
exit (~300 tiles) plus prefetch is ≈ 150 core-ms. MIN's six cores are already committed to simulation,
physics, animation and decode (02 §2.4). The CPU VM therefore stays the conformance reference and the
fallback. Vulkan float math is not bit-exact across vendors (2.5 ULP division, implementation-defined
transcendentals and denormals). Therefore:

- **Noise and domain math are integer/fixed-point**, built on 02's `det` primitives (`hnoise`):
  lattice hashes (PCG/xxHash32), Q16.16/Q32.32 interpolation, our own polynomial curves. Each node
  op exists twice, as a C++ VM op and as a Slang function, and a per-node conformance corpus hashes both.
- `visualOnly` nodes (micro-displacement < 5 cm, detail normals) may use floats and never feed
  collision.
- **CI:** 10⁶ hashed samples per test planet must match across the CPU, lavapipe, NVIDIA, AMD and
  Intel. The target is bit-identical; the hard limit is ≤ 1 cm, inside 02's 5 cm visual/collision
  contract.
- **Fallback:** on a failing vendor, near-field tiles use the CPU tiles that already exist for
  collision, uploaded on the transfer queue.

### 5.5a Terrain-generation throughput: 32-bit twin, cost model, spike, fallbacks

**32-bit-only GPU twin.** Consumer GPUs emulate 64-bit integer arithmetic, and RDNA runs 32-bit integer
multiply at quarter rate. `hnoise.slang` therefore uses **only 32-bit integer operations**, and the
shader lint rejects `int64_t`/`uint64_t` in it:
- Q32.32 heights are `int2 {hi, lo}` with explicit carry adds.
- 32 × 32 → 64-bit products use SPIR-V `OpUMulExtended`/`OpSMulExtended` (Slang's GLSL-compatible
  `umulExtended`/`imulExtended`), which need no `shaderInt64`.
- Q16.16 noise internals stay in single 32-bit words.
- The CPU VM may use native int64. Integer results are exact, so both twins hash identically, and the
  per-node corpus proves it.

**Level-adaptive octaves.** Generator ops take the tile's level. At levels coarser than the collision
levels (sample spacing > 2 m), an octave whose wavelength is < 2 × the sample spacing is skipped. It
could only alias at that spacing. The skip is taken only if the skipped octaves' summed amplitude, known
statically from the graph, is < 0.5 px at the level's minimum view distance: ≤ 12 cm at the ~500 m where
the first such level starts. Collision levels always evaluate every octave, so
02 §5.8's 5 cm visual/collision contract is unaffected. The rule depends only on `(graph, level)`, so CPU
and GPU skip identically, and the conformance corpus runs per level.

**Demand model** (Harrow, High, 1440p). CDLOD keeps ~24 visible tiles per active level. The finest
active level's tile edge is ≈ h/4 at altitude h.

| Case | Tile demand |
|---|---|
| Horizontal flight at speed v | ≈ 64·v/h tiles/s: 1,500 m/s at 1 km → 96/s; 300 m/s at 100 m → 192/s |
| Vertical descent at rate w | ≈ 96·w/h tiles/s: 1,500 m/s at 10 km → 14/s; 300 m/s at 1 km → 29/s |
| Cold set (camera cut, warp exit, spawn) | ~300 visible tiles at once, above the resident levels 0–3 |
| Steady-state BENCH-2 design demand | ≤ 200 tiles/s ≈ 3.3 tiles per frame |

64 tiles per frame is therefore **burst capacity**: a cold set in 5 frames, with ≈ 19× steady-state
headroom. It is not the steady-state need.

**Cost model.** The reference graph is 40 nodes, 10 of them generators averaging 8 octaves of 3D gradient
noise, ≈ 120 32-bit integer operations per noise evaluation (8 PCG hashes, fade, lerps). That is ≈ 10k
integer ops per sample.
- **GPU:** 64 full-detail tiles (270k samples) ≈ 2.7 G ops in 0.8 ms ≈ 3.4 T int-ops/s. The GTX 1660 S
  peaks at ≈ 2.5 T. Level-adaptive octaves roughly halve the average, which puts the target at the edge
  of MIN.
- **CPU:** one 65² tile ≈ 42 M ops. AVX2 SoA at 8 lanes lands at ≈ 0.5–0.8 ms per core on a Ryzen 5 3600,
  also at the edge. The 8 lanes are `pcg`'s default `vm_avx2.cpp` kernel. Every image that runs it is built
  at the `avx2` ISA level and has passed the CPU gate (02 §1.1). The 4-lane twin hashes identically but is
  never budgeted (02 §5.8).

**The plan does not assume either fits. It measures them first.**

**Spike (WP-0.9c, Phase 0; 09 §2.1).** Two-week timebox, with the decision recorded in `docs/adr/`:
- *Workload:* the reference 40-node graph (the Harrow T04 preset shape: continents, ridged mountains,
  domain warp, craters, terraces) as bytecode plus `hnoise.slang`.
- *GPU:* tiles per 0.8 ms, full detail and level-adaptive, on the `win-gpu` runner (WP-0.4) and on
  lavapipe (throughput for the CI budget only, no performance claim), with a per-architecture
  int-throughput scaling model. **MIN confirmation** on a GTX 1660 S and an RX 5600 XT is due as soon as
  the lab lands, and before WP-1.8 starts. K7's trigger (unmeasured at the Ph1 midpoint) counts as red.
- *CPU:* `pcg` VM with AVX2 SoA, ms per 65² tile per core on a MIN-class CPU (Ryzen 5 3600 /
  i5-9400F) and on SERVER hardware, full detail. A run that did not log `pcg.kernel=avx2` is invalid, not red.

| Outcome | Threshold on MIN | Action (pre-decided) |
|---|---|---|
| **Green** | GPU ≥ 64 tiles in ≤ 0.8 ms **and** CPU ≤ 0.5 ms per tile per core | Plan as written |
| **Amber (F2)** | GPU ≥ 16 tiles in ≤ 0.8 ms | Tile budget = measured capacity (≥ 16 per frame = 960/s, still ≥ 4.8× steady demand). Trajectory prefetch deepens from +4 s to +8 s. Cold sets take ≤ 19 frames behind parent fallback, and the warp tunnel and undock sequences mask them |
| **Red (F3)** | GPU < 16 tiles, **or** CPU > 0.5 ms per tile per core | Split the graph. Octaves with wavelength < 4 m become `visualOnly`, and T04's validator bounds their summed amplitude to ≤ 5 cm, so collision evaluates only the fixed-point **base octaves**. Levels whose minimum view distance is ≥ 2 km (all coarser than the collision levels) use a **float twin** (`hnoise_f32.slang`) held to ≤ 10 cm across vendors, which is < 0.1 px at 2 km. Near-field visual tiles stay fixed-point on the GPU, or use the CPU collision tiles (§5.5 fallback) if the GPU is also red. If only the GPU is red, 2 Background workers also generate visual tiles at steady state (≤ 200 tiles/s ≈ 0.1–0.2 core) |

- *Risk:* the outcome arms **K5b** (09 §7). Its trigger is any spike result below green, or RC-13 red later.
- *Ongoing gate:* **RC-13** (Ph1) re-measures the adopted variant nightly on MIN and REF inside
  BENCH-2.

### 5.6 Texturing, biomes, scatter

- **Biomes:** climate maps (temperature, humidity, geology; Genesis model, R04 §5) come from the
  same graph. Each planet has ≤ 16 tileable BC7/BC5 layers, ≤ 4 per tile, height-blended, with
  triplanar mapping only above 35° slope.
- **Orbit views** use a 64² baked albedo per tile, so they match the ground.
- **Ph3 procedural virtual texture** (Chen 2015, Far Cry 4): layers, decals and roads composited into
  128² pages on feedback demand.
- **Scatter** (T05 rules, deterministic seeds): visual-only instances are generated per tile on the
  GPU (≤ 500k visible on REF). Gameplay scatter (collidable rocks, resource nodes, tree trunks near
  paths) comes from the CPU library as entities (02/06). A rule is either visual or collidable, never
  both, so nothing renders twice. Foliage rendering is §5.6a.

### 5.6a Vegetation (Ph2 scatter → Ph3 full)

**Assets.** A T05 rule references a `FoliageDef` record:
- a mesh LOD chain with a tree LOD0 of ≤ 20k triangles and leaf cards;
- an octahedral impostor from §3.4's baker: 8 × 8 hemi-octahedral views in a 1024² atlas per species,
  with albedo, normal, depth and transmission;
- wind parameters: trunk stiffness, branch and leaf-flutter frequencies, and a bounded WPO maximum
  (§4.3), which culling adds to the bounds;
- an interaction radius.

T28 rejects leaf cards whose alpha coverage is < 50 % of the card area (overdraw) and requires
coverage-preserving alpha mips (Castaño 2010), so canopies do not thin with distance.

**Bands (High; §8.2 scales them):**

| Class | Full | Reduced | Impostor | Beyond |
|---|---|---|---|---|
| Grass / ground cover | Procedural blades to r_g = 30 m | Density thins and blades widen to keep coverage, 30–60 m | — | Biome layer colour on the terrain |
| Shrubs | LOD0–1 to 40 m | LOD2 to 120 m | 120 m – 1 km | Tile albedo |
| Trees | LOD0 to 30 m, LOD1 to 80 m | LOD2 to 200 m | 200 m – 3 km, lit at runtime | Tile albedo plus canopy tint in the 64² orbit bake (§5.6), so orbit matches ground |

**Rendering.**
- **Instances:** tree and shrub instances are generated per terrain tile when the tile is produced. They
  are 8 B each (16-bit tile-local position, packed rotation and scale) and cached with the tile. The
  §3.2 two-phase HZB cull and LOD select run on them each frame, with MDI per LOD bucket.
- **Grass blades:** generated from per-tile density in a mesh shader on REF (Ghost of Tsushima style,
  Wohllaib 2021), or expanded by compute into the transient index buffer on MIN. Budget: ≤ 2M blades on
  REF and ≤ 600k on MIN, within r_g.
- **Alpha test:** alpha-tested leaves draw in the prepass with their masked depth PSO (§4.3). Forward
  shading uses `EQUAL` depth, so it never alpha-tests and keeps early-Z.
- **Shading:** the Foliage model (§4.1) is two-sided with thin transmission and specular occlusion from a
  per-instance canopy-density term.
- **Shadows:** full foliage in CSM cascades 0–1, impostor depth casters in cascades 2–3, none beyond.
  Grass casts no CSM; contact shadows and GTAO ground it.
- **GI:** trunks enter the TLAS; leaf cards do not (§4.6a).

**Global wind field** (async compute, ≤ 0.03 ms):
- A 32 × 16 × 32 RG16F volume covers 512 × 128 × 512 m around the camera in render-anchor space.
- Sources: the weather's base wind (§5.8a); scrolling gust noise; and ≤ 64 **wind emitters** per frame
  from cues (ship thrusters, landing wash, rotor wash, explosions), each a radial or directional impulse
  with decay.
- Foliage WPO, GPU particles (a force module, §6.1), precipitation (§5.8a) and cloth and secondary motion
  all read the same field. The base wind plus gusts is an analytic hash function of `(position, zone
  time)`, so 02's CPU cloth and secondary-motion jobs evaluate the identical function without a GPU
  readback. Emitters are CPU-known cues too.

**Player interaction.**
- A camera-centred 128² interaction map (0.5 m texels, 64 m) receives capsule splats each frame from
  ≤ 256 interactors: characters, vehicles and landing ships.
- Grass and shrubs bend away from the splats, recovering over ≈ 2 s. Thruster wash adds both a splat and a
  wind emitter.
- Cost ≤ 0.02 ms. Trails are cosmetic and not persisted.

**Budgets:** included in the §8.1 rows, broken down under §8.1. **Goldens:** a Harrow meadow and forest
edge with a fixed wind seed at noon and dusk, and a landing-wash interaction sequence.

### 5.7 Atmosphere: Hillaire 2020 (R09-B2, R09-RD-P0-10)

- **Parameters:** per-planet `AtmosphereDef` records (T18): radii, Rayleigh/Mie/ozone profiles,
  ground albedo, sun spectrum from the star, up to two suns.
- **LUTs:** transmittance 256 × 64 and multi-scattering 32 × 32 are rebuilt when parameters change,
  so weather and terraforming can animate them. Sky-view 192 × 108 R11G11B10F and aerial perspective
  32³ RGBA16F are rebuilt per view per frame. Total ≤ 0.3 ms.
- **Space view:** outside the atmosphere, the shell is shaded per pixel at half resolution from the
  LUTs (Hillaire's space path), with ray-sphere planet shadow.
- **Aerial perspective** applies to opaque, transparents, particles and clouds.
- **Far layer:** bodies > 2,000 km away render as analytic ray-sphere impostors with tile-baked albedo
  and the same atmosphere. It exists for cost and culling; infinite reverse-Z already covers precision.

### 5.8 Volumetric clouds (Nubis, R09-B4)

- **Ph1:** a lit 2D cloud shell from the planet weather map, enough for BENCH-2 at Medium.
- **Ph3 volumetric** (Schneider & Vos 2015):
  - *Density:* a spherical shell. Density = a rotating weather cubemap (coverage, type, precipitation) ×
    a per-type height gradient × 128³ Perlin-Worley, eroded by 32³ Worley and curl noise. The weather
    cubemap is generated from the replicated `PlanetWeather` (§5.8a); before Ph3 (including the Ph1 2D
    shell) it is a static T18-authored map.
  - *Lighting:* Beer × powder, dual-lobe HG, multi-scatter octaves (Wrenninge 2013), a 6-step sun cone,
    sky-view ambient.
  - *Temporal (normal mode):* the cloud buffer is **quarter resolution**, half width × half height
    (1280 × 720 at 1440p). It uses 4 × 4 checkerboard updates, so 1/16 of the buffer (57.6k rays) is traced
    per frame, with reprojection over a 16-frame history. High uses 48 primary steps, a 6-step sun cone and
    3 multi-scatter octaves.
  - *Consistency:* orbit shows the same weather map as a 2D shell, blended by altitude.
  - *Cloud shadows:* a top-down density map.
  - Budget **1.5 ms** (normal mode).
  - **Fast mode.** In normal mode each pixel is refreshed only every 16 frames. At 1,500 m/s its history
    is 400 m old, and inside the layer that much parallax cannot be reprojected: it leaves ghost trails.
    Tracing every quarter-res pixel instead would take 16× the rays. Fast mode therefore changes *what*
    is traced, not how often:
    - **Trigger.** It engages when camera speed relative to the planet's surface frame exceeds 500 m/s
      **and** the camera is below cloud top + 2 km. It releases below 400 m/s or above cloud top + 2.5 km
      (hysteresis). It also engages for 16 frames after a camera cut or warp exit, when history is invalid
      anyway.
    - **Above the layer.** Above cloud top + 2 km at > 500 m/s, no volumetric rays are traced. The 2D
      shell (same weather map, parallax-offset tops) renders instead at ≤ 0.4 ms. Between cloud top + 2 km
      and cloud top it cross-fades into the fast-mode volume.
    - **In and below the layer.** The buffer is **1/8 resolution per axis** (320 × 180 at 1440p, 57.6k
      pixels), so it traces the same ray count as normal mode, but traces **every pixel every frame**:
      - **20 primary steps** on High (12 on Medium), with a per-frame blue-noise start offset;
      - a 3-step sun cone and **one multi-scatter octave**, plus a constant multi-scatter boost calibrated
        per cloud type against the 3-octave result;
      - detail-Worley erosion only on the 8 nearest steps;
      - temporal accumulation with α = 0.5, and depth and velocity rejection instead of the checkerboard;
      - a joint-bilateral 8× upsample against full-resolution depth.

      Motion blur (§7.4) and speed streaks mask the lower resolution at these speeds.
    - **Cost model.** The ray count equals normal mode, and each ray costs ≈ 0.45× (fewer steps, cone
      taps and octaves), ≈ 0.7 ms. Adding 0.15 ms for the upsample and 0.05 ms for the temporal pass gives
      **≤ 1.0 ms on REF High** (≤ 0.7 ms on Medium), a §8.1 line of its own that replaces the normal-mode
      line and is never added to it. MIN at Low draws the 2D shell (0.4 ms) and has no fast mode.
    - **Transitions** cross-fade over 0.25 s. The outgoing mode's history is reprojected without new rays,
      so a fade costs only the incoming mode plus 0.05 ms.
    - **Measured** in BENCH-2's **in-layer segment**: the scripted descent from cloud top + 2 km to the
      cloud base, flown at 1,500 m/s through the reference cloud deck with a fixed weather seed (RC-8,
      RC-11 Ph4).
- **Ph4:** golden (AAA-REN-6). **Ph5:** voxel/SDF Nubis³ fly-through.

### 5.8a Weather effects (Ph3, with volumetric clouds)

**One weather state drives everything.** Clouds, precipitation, wetness, fog, wind and lightning read one
state, so a storm looks the same from orbit, from the cockpit and from the ground.
- **Input** (06 owns the simulation and gameplay effects, 03 renders them): a replicated per-body
  `PlanetWeather{seed, epoch, fronts[≤ 32]{centre, radius, velocity, type, intensity, lightningRate},
  baseWind, temperatureBias}`, ≤ 1 KB and sent reliably on change. T18's per-biome weather tables feed
  06's front spawner.
- **Weather map:** the client rasterizes fronts plus seeded noise into the §5.8 weather cubemap
  (256²/face, one face per frame). Channels are coverage, cloud type, precipitation intensity, and
  precipitation kind from biome temperature (rain, snow, sand, ash).
- **Local sample:** each frame the camera's local weather is sampled from the map, together with a
  wetness/snow accumulator. Weather is **visual, not deterministic-critical**. Gameplay-relevant facts
  (visibility modifiers, "is raining here" for status effects) are computed on the cell and replicated;
  the client only visualizes them.

**Precipitation** (GPU particles, §6.1 pools):
- A camera-local cylinder, 40 m radius for rain and 25 m for snow, sand and ash. Up to 60k rain streaks
  or 40k flakes on REF, 20k / 15k on MIN (§8.2).
- Streak direction and length come from `wind − cameraGridVelocity` × shutter. Above 150 m/s relative
  speed the particles fade out and froxel density streaks plus canopy droplets carry the effect, so a
  1,500 m/s descent through a storm stays cheap.
- **Occlusion:** a top-down 512² depth map over 128 m × 128 m is rendered from static and grid geometry.
  It is re-rendered only when the camera moves 16 m or a grid moves (≤ 0.05 ms amortized). It kills rain
  under overhangs and inside hangars and spawns ≤ 4k splashes per frame at occluder hits. Portal-graph
  interiors (02 §5.1) switch precipitation off, and windows show exterior rain through exterior-visible
  portals.
- **Far rain:** rain shafts under cloud bases are a precipitation term in the cloud and froxel density,
  so distant storms read from kilometres away and from orbit.
- **Canopy and visor droplets:** a screen-space droplet normal and refraction layer on the cockpit glass
  and helmet visor, driven by exposure to precipitation and airspeed.

**Surfaces** (the §4.1 weather modifier):
- **Wetness** accumulates to 1 in ≈ 2 min of heavy rain and dries in ≈ 10 min, scaled by temperature and
  wind. It is masked by the occlusion map and by 02's `pressurized` interior cells.
- It darkens albedo by porosity, drives roughness toward 0.08 (Lagarde 2013), forms puddles in terrain
  cavities on slopes < 5° once wetness > 0.5, and adds animated rain-ripple normals.
- **Snow** accumulates on upward-facing surfaces (`N·up`) as a global post-layer, outside the ≤ 4 material
  layers, and on terrain as a biome-layer override. Sand dust accumulates the same way.
- Cost ≤ 0.15 ms, inside forward opaque.

**Lightning:**
- Bolts are branching ribbons (midpoint displacement, seeded from `hash(frontId, strikeIndex)`), both
  cloud-to-ground and intra-cloud. Strike times and positions derive from `PlanetWeather` and zone time,
  so every client sees the same strike.
- Each strike adds a ≤ 0.3 s clustered light (≤ 4 at once) and an in-cloud point-light term in the cloud
  lighting (visible as flashes from orbit), and emits a thunder cue delayed by distance/343 m/s (02
  audio). Flash pixels are excluded from exposure metering, so the histogram does not pump.
- Gameplay strikes, if any, are server cues that reuse the same visual.

**Fog and haze:** weather drives froxel height-fog density and the atmosphere's Mie term; §5.7's LUT
rebuild on parameter change already supports this. **Sand and dust storms** use the same pipeline:
horizontal streaks plus heavy froxel density.

**Budget:** the §8.1 "Weather" row (REF 0.3 ms, MIN 0.2 ms): sim 0.05 (async), occlusion 0.05, render and
splashes 0.15, lightning 0.05. **Goldens:** night rain with a seeded lightning strike at Saltmarch, snow
cover on a Harrow highland biome, a wet street with puddles and ripples, a dust storm, and orbit views of
the same storm.

### 5.9 Oceans (Ph3)

FFT waves (Tessendorf 2001; JONSWAP; 3 cascades of 256² at 250/37/5 m; Jacobian foam) on async
compute, displacing a sea-level cube-sphere quadtree; far field uses an analytic glint BRDF (Bruneton,
Neyret & Holzschuch 2010). Budget 1.0 ms.

### 5.10 Rings, gas giants, asteroid fields

- **Rings:** an annulus with a radial density/color profile, forward-scattering phase, ray-sphere
  planet shadow, ray-plane ring shadow on the planet, hashed GPU debris near the camera.
- **Gas giants:** curl-advected band palette on a slowly updated cubemap, storm decals,
  deep-absorption Hillaire atmosphere; near-approach volumetrics in Ph5.
- **Asteroid fields (Scree belt):** deterministic cells (200 m near, 2 km mid) generate GPU instances
  from ~20 base meshes, with spin as a function of time (no CPU updates). Minable asteroids are
  CPU-library entities.

### 5.11 Orbit to ground to interior (BENCH-2, W05, AAA-REN-5)

| Altitude | Rendering |
|---|---|
| > 100 km | CDLOD root levels + tile albedo (canopy tint baked in) lit with **horizon-map terrain shadows** (§5.4a), per-pixel atmosphere, 2D cloud shell with lightning flashes |
| 100–10 km | Volumetric clouds (or the 2D shell above cloud top + 2 km at > 500 m/s; fast mode inside the layer, §5.8), rain shafts and AP froxels fade in; horizon-map terrain shadows |
| < 10 km | CSM on terrain inside `csmFar`, horizon maps beyond it (`min` blend, §5.4a), scatter, virtual texture, froxel fog; tree impostors inside 3 km |
| < 200 m AGL | Foliage meshes and grass, wind and landing-wash interaction, precipitation particles (below 150 m/s relative), surface wetness |
| Interior | 02's portal graph (W08) culls interiors and drives sky visibility, probe blend, exposure zones and precipitation masking |

The same tiles, atmosphere and weather map are used throughout, so there is no representation swap.
Tiles prefetch along the trajectory (§5.4) and textures and meshes along 02's `StreamingSource` lookahead
(§1.3a), with zero residency misses at 1,500 m/s (AAA-CNT-2) and no frame > 50 ms (RC-2, RC-12).

### 5.12 Ship interiors

Clustered lights with grid-space shadow caching, relit grid probes, DDGI (REF) or assembly-time
probes (MIN); `Ship.Power.*` drives lights and emissives; windows and airlocks are portals.

---

## 6. Sci-fi VFX

### 6.1 GPU particles (R09-B10, R06-ENG-34)

- **Pools:** SoA, 1M particles on REF (BENCH-3: 400k active), 256k on MIN (BENCH-3's significance pass keeps the top 256k). Positions live in the
  **emitter's frame**, so thruster trails stay correct on moving ships.
- **Per frame, mostly async compute:**
  1. Emit from emitter tables and cue events, with deterministic seeds.
  2. Simulate: gravity, drag, curl noise, attractors, the global wind field (§5.6a), and
     **depth-buffer collision** against reprojected depth.
  3. Compact dead and alive lists.
  4. Sort sorted emitters only (FidelityFX Parallel Sort, MIT).
  5. Build indirect args.
  6. Render billboards, stretched billboards, **ribbons**, **mesh particles** and **beams**.
- **Lighting:** clustered lists plus froxel fog, or per-vertex on Low. Particles write motion vectors
  where near-opaque, otherwise the **reactive mask**.
- **Budgets:** T17 per-effect limits, a GPU top-N significance pass, half-res smoke on Low/Med.
- **VFX graph (T17):** module stacks compile to a bounded set of Slang uber-kernels. We build in-house
  rather than on Effekseer because GPU simulation, clustered lighting and grid frames are required
  (closes R08-ED-P1-09).

### 6.2 Energy shields (R09-B8)

- **Geometry:** a cooked ellipsoid or inflated hull (≤ 2k triangles).
- **Shading:** fresnel rim, triplanar hex/noise pattern, depth-fade intersection glow. The strength
  attribute drives opacity; sector masks map directional faces.
- **Impacts:** a GPU ring of 4,096 `ShieldImpact{float3 localPos; float t; float strength; uint
  shield; uint damageType;}` written from cues (06 §1.5). The shader sums
  `Σ sᵢ·e^(−k(t−tᵢ))·ring(chord(p,pᵢ) − v(t−tᵢ))` over ≤ 16 recent impacts.

### 6.3 Holograms and transparency

- **Holograms:** fresnel, world-space scanlines, flicker/glitch displacement, chromatic offset, depth
  fade; volumetric ones are particle splats.
- **Transparency:** per-object sorting for large surfaces (shields back then front faces, canopies);
  **weighted-blended OIT** (McGuire & Bavoil 2013) for dense hologram clusters (BENCH-1); per-emitter
  particle sorting. Every transparent writes the reactive mask.

### 6.4 Plumes, trails, warp

- **Plumes:** nested cones with an analytic shock-diamond core. Mach-disk spacing follows throttle and
  ambient pressure. Each plume adds heat distortion and a clustered light.
- **Contrails** appear only where atmosphere density > 0.
- **Warp/quantum** (R09-B9): stars stretched along velocity in the star pass, a camera-space noise
  tunnel, radial blur + chromatic aberration, FOV kick and entry/exit flash. The warp state machine
  (06 §8.3) drives every parameter, and the tunnel also hides streaming (R04 §6.3).

### 6.5 Weapons, explosions, damage

- **Beams** are HDR-core strips ending at the replicated impact; **bolts and tracers** are stretched
  billboards (length = v × shutter); **impacts** add sparks, a clustered decal and a ≤ 0.2 s light.
- **Explosions:** 8 × 8 flipbooks with **motion-vector frame blending** and six-way lighting,
  shockwave rings, a flash light; sparse volumetric explosions in Ph5.
- **Damage:** per-instance hull masks plus scorch decals; re-entry plasma is a shell effect driven by
  dynamic pressure.

---

## 7. Post and presentation

### 7.1 HDR and exposure (R09-B6, R09-RD-P0-3)

- **Units:** RGBA16F scene-referred; sun in lux, emissives in nits, exposure in EV100.
- **Pre-exposure:** lighting is scaled by last frame's exposure (Lagarde & de Rousiers 2014), or
  120 klux sun beside black shadow overflows FP16.
- **Histogram exposure:**
  - 256 bins spanning −10 to +20 EV;
  - 50–95 % trimmed mean;
  - adaptation at +3 / −1 EV/s;
  - T18 volumes set bias and clamps (cockpits, interiors).
- Local tone mapping (Mertens et al. 2007) arrives in Ph5.

### 7.2 Bloom, tonemap, grading, HDR10

- **Bloom:** Jimenez 2014 13-tap downsample with a Karis average on the first mip, tent upsample over
  7 mips, 0.04 blend. ≤ 0.3 ms.
- **Tone mapping:** AgX by default, ACES-fit as an option.
- **Grading:** 32³ log-space LUTs; T18 volumes blend up to 4.
- **HDR10 (Ph4):** a PQ/Rec.2020 swapchain with paper white (default 200 nits). Peak luminance comes
  from `hdr_metadata` plus a DXGI output query via the Windows platform layer. scRGB is the alternate
  path, SDR the fallback.

### 7.3 TAA and upscaling (R07, R09-RD-P0-8)

TAA follows Karis 2014: 16-phase Halton jitter, closest-depth motion-vector dilation, YCoCg variance
clipping, Catmull-Rom history, and reactive/composition masks.

```cpp
struct UpscaleInputs { RgTexture color, depth, motion, reactive, composition, exposure;
                       float2 jitter; Extent render, display; CameraParams cam; bool reset; };
class IUpscaler { public:
    virtual UpscalerCaps caps() const = 0;
    virtual RgTexture addPasses(RenderGraph&, const UpscaleInputs&) = 0; };
```

- **Implementations:**
  - Helios TAA/TAAU, always present;
  - **FSR 2** (MIT, Vulkan), the default upscaler;
  - FSR 3.1 once FidelityFX supports Vulkan again (R09-B13);
  - **DLSS** (Streamline) and **XeSS** as optional proprietary plugins.
- **Modes:** 1.5× / 1.7× / 2× / 3×; mip bias `log2(render/display)`.
- **Motion-vector contract.** Every opaque surface writes the exact screen motion between the geometry
  displayed this frame and the geometry displayed last frame. This includes skinned characters at
  reduced reskin rates: every skinned tier keeps a previous-position slot (§7.6a). The reactive mask is
  reserved for transparents, non-opaque particles, beams and holograms (§6.1, §6.3). The composition mask
  is reserved for surfaces whose shading moves without their geometry: diegetic screens (§7.5), scrolling
  emissives and the star layer. Opaque skinned characters write neither mask. §8.4a's temporal-stability
  check enforces the contract through FSR 2 at MIN's and Performance mode's render scales.
- Frame generation in Ph5.

### 7.4 Other post

Motion blur (McGuire et al. 2012, ≤ 0.4 ms, stars excluded); half-resolution scatter-as-gather DoF
for cinematics and ADS; grain, chromatic aberration, vignette; g-force greyout from flight g-load.

### 7.5 UI, render-to-texture, cockpit

- **Order:** tonemap → **RmlUi** (08) through `RmlRenderInterfaceHelios` → ImGui (editor/debug). In
  HDR10, UI renders at paper white.
- **Diegetic screens and MFDs** (R09-B16, R04-P1-18):
  - UI documents render into 2048² atlas pages at ≤ 30 Hz, round-robin.
  - Screens that culling reported invisible are skipped.
  - A `Screen` material adds emissive nits, a pixel grid and glare.
  - BENCH-1's 40 screens: ≤ 1.0 ms CPU + 0.5 ms GPU.
- **Collimated HUD:** projected at infinity after upscaling. Target brackets are projected from f64
  positions.
- The cockpit layer (§2.7) gets world lighting and glass dirt and scratches.
- **Editor passes** (07 §5.5): object-ID buffer (R32_UINT, 1-pixel readback picking), jump-flood
  selection outlines, grid, depth-tested debug draw, gizmos, an `imgui_impl_helios` multi-viewport
  backend on the RHI, and offscreen views for assetd thumbnails and previews (lavapipe-capable).

### 7.6 Characters, at a practical level (R04, R05, G15)

- **Geometry:** `Morph` and `DnaBlend` shapes are baked once per appearance and LOD into a cached rest
  mesh. Compute skinning from ozz palettes (02 §7.2) runs at each tier's palette rate into a tiered
  skinned-output ring (§7.6a). Per-frame FACS blend shapes are evaluated only for ≤ 8 F0 faces (§7.6b).
- **Skin:** pre-integrated (Penner & Borshukov 2011) on Low/Med; Burley screen-space SSS (Golubev
  2018) on High+.
- **Hair:** cards with dithered alpha, shaded with Karis 2016.
- **Eyes:** refractive cornea/iris parallax.
- **Customization:** layers are composited into a per-character atlas whose resolution follows the
  character's tier, from 2048² for heroes down to shared crowd atlases, and which is block-compressed on
  the GPU and cached (§7.6a, R09-A18).
- **Crowds** (BENCH-1): skinning at 30 Hz for A1 (15–30 m) and 15 Hz for A2 (30–80 m), staggered across
  frames, and tinted crowd-class impostors beyond 80 m (§7.6a). Animation LOD is owned by 02 (§7.2 tiers
  A0–A3).

### 7.6a Character composites and crowd impostors at scale (Ph2 → Ph4)

**The problem.** BENCH-1 has 200 avatars plus 60 NPCs, and BENCH-5 has 150 avatars. One 2048² RGBA8
composite with mips is ≈ 21 MB, so 200 of them would be ≈ 4.2 GB. Even as BC7 they would be ≈ 1 GB. Either
figure breaks MIN's 1.8 GB texture pool and 5 GB ceiling (§1.3, AAA-CNT-3, RC-10). Resolution therefore
follows screen size and 02's animation-LOD tier, composites are block-compressed on the GPU, and a capped
cache charged to the texture pool holds them.

**What is composited.** Of `CharacterAppearance`'s parameter kinds (06 §10), only `PaletteColor`,
`TextureLayer`, `Decal` and the texture part of `DnaBlend` change the composite. `Morph`, `BoneScale` and
`MeshOption` change geometry only; *Geometry at scale* below handles them.
- **Sources** are ordinary streamed `.htex` files with `StreamTexId`s: the species and body-type base skin,
  ≤ 4 DNA head-basis textures, and shared layer atlases (tattoos, makeup, scars, ageing, grime). The
  composite requests the source mip that matches its tier as a priority-2 request (§1.3a).
- **Outputs:** albedo (BC7, sRGB), masks (BC1: roughness, cavity/specular occlusion, SSS thickness) and, at
  hero and A0 tiers only, a normal map (BC5: the DNA-blended head plus relief decals). A1–A2 characters use
  the shared normal of their dominant head basis, which is indistinguishable at 15–80 m.
- **Clothing and armour are not composited.** Outfit meshes use shared textures with per-instance palette
  tint and ≤ 4 decal slots (§4.2's layered path), so they cost no memory per character.

**Tiers** (the §8.2 preset sets the resolutions; bytes include mips):

| Tier | Who | REF High | MIN Low | Channels | Size per character (REF / MIN) |
|---|---|---|---|---|---|
| Hero (≤ 4) | The local player, the dialogue or cinematic subject, the current target within 50 m (the §1.3a `pin` set), and the character creator and T22 previews | 2048² | 1024² | Albedo + masks + normal (3.3 B/texel) | 14.0 MB / 3.5 MB |
| A0 (≤ 32, 02's governor) | < 15 m, scaled by screen size | 1024² | 512² | Albedo + masks + normal | 3.5 MB / 0.87 MB |
| A1–A2 | 15–80 m (A1 ≤ 96) | 512² | 256² | Albedo + masks (2.0 B/texel) | 0.52 MB / 0.13 MB |
| A3 and impostor range | > 80 m | No per-character composite: a **crowd-class impostor atlas tinted per character** (below) | same | — | 0 |

**Worst case, BENCH-1** (all 260 characters within 80 m, 200 unique avatar appearances):
- **REF:** 4 × 14.0 + 32 × 3.5 + 224 × 0.52 ≈ **285 MB**, against a **320 MB** cache cap (8 % of the 4 GB
  pool).
- **MIN:** 4 × 3.5 + 32 × 0.87 + 224 × 0.13 ≈ **71 MB**, against a **96 MB** cap (5 % of the 1.8 GB pool).

Naive 2048² composites would need 4.2 GB for the same crowd.

**Runtime GPU block compression.** A composite job runs on async compute:
1. It blends ≤ 12 layers per texel in linear space (tint-multiply, overlay, and RNM for relief-decal
   normals) into a transient RGBA8/RG8 tile.
2. It encodes blocks with Helios's own Slang encoders:
   - **BC7 mode 6 only** (PCA endpoints plus one refinement pass) for albedo;
   - **BC1 range fit** for masks;
   - **BC5** as two BC4 range fits for normals.
3. It writes the blocks to a buffer and copies them into the BC image with `copyBufferToImage` on the same
   queue. This is portable to the D3D12 seam, because it needs no storage view of a BC format.

Encode quality must be ≥ 38 dB PSNR against an offline maximum-quality BC7 encode of T22's stress grid.

**Streaming and cache.**
- **A streamed texture with a procedural source.** Each composite is registered as a streamed texture with
  its own `StreamTexId`. It gets GPU feedback, priority and eviction like any texture (§1.3a), but its
  "stream in" is a re-composite at the higher tier, not a pak read.
  - **Promotion** (for example A1 → A0 at 15 m) composites the new tier and fades it in over 0.25 s via
    `minLod`.
  - **Demotion** raises `minLod` at once. The smaller image replaces the larger one lazily, as with stream
    out.
  - **Appearance edits** (image designer, 06 §10) re-composite at the current tier and cross-fade.
- **Cache key:** a 64-bit hash of (species, body type, texture-affecting parameter subset, tier). NPCs that
  share a preset, and identical avatars, share one entry.
- **Eviction and cap.** The cache is LRU over entries not referenced for 10 s, and demoted entries keep
  their larger image for 10 s of hysteresis. It is a sub-pool of the §1.3 texture pool with the §8.2 cap.
  Under `memory_budget` pressure, dropping a composite's top mip *is* a tier demotion, so pool shrink
  (§1.3a) needs no special case.

**Composite budget.**
- **Per frame:** ≤ 0.3 ms of async compute on REF and MIN, and at most 4 jobs. A job covers ≤ 1024², so a
  2048² hero composite is four quadrant jobs.
- **Admission:** a cost model (texels × layers, calibrated from timestamps) admits jobs in priority order:
  hero, then A0, A1 and A2, each by feedback coverage. A job that does not fit waits for the next frame.
- **Throughput targets** (measured in WP-2.3): REF ≥ 8 Mtexel/ms, ≈ 2.4 Mtexel per frame; MIN ≥ 3 Mtexel/ms,
  ≈ 0.9 Mtexel per frame.
- **Cold arrival in BENCH-1** (all 260 characters new): REF 4 × 4.2 + 32 × 1.05 + 224 × 0.26 ≈ 109 Mtexel,
  ≈ 45 frames ≈ 0.8 s. MIN ≈ 27 Mtexel, ≈ 30 frames ≈ 0.5 s.
- **Tint fallback.** Until its composite arrives, a character renders with the species base skin × its
  skin-tone palette colour and the shared head-basis normal. It is never missing, only plainer: no tattoos
  or makeup.
- **Steady state** (tier promotions, appearance edits): p50 ≤ 0.1 ms in BENCH-1 and BENCH-5.

**A3 crowd impostors for customized characters.**
- **Crowd class** = (species, body type, outfit silhouette class). T22 assigns each outfit `MeshOption` to
  one of ≤ 8 silhouette classes per body type and runs the **crowd bake** (07 T22, Ph4). Ph3 ships one
  generic class per species and body type.
- **Atlas per class:** 4 poses (idle, walk, run, sit: 02 §7.2's `impostorPose`) × 8 phases × 8 headings =
  256 frames.
  - Frames are 32 × 64 px (16 × 32 on MIN Low). At 80 m on 1440p a 1.8 m avatar is ≈ 28 px tall, so the
    frames are ≈ 2× supersampled.
  - Channels: albedo baked with neutral palette regions (BC7), normal (BC5), and a **palette-slot mask**
    (BC4: skin, hair and 4 outfit slots).
  - Size ≈ 1.75 MB per class (0.44 MB on MIN). These are ordinary streamed textures: ≤ 32 classes are
    present in BENCH-1, so ≤ 56 MB on REF and ≤ 14 MB on MIN.
- **Runtime:** instanced, one draw per class.
  - The shader picks the frame from 02's `{impostorPose, heading, phase}`, blending the two nearest of 8
    headings.
  - It tints masked texels with the character's ≤ 6 palette colours, a 24 B per-instance record unpacked
    from `CharacterAppearance` at extract.
  - It scales the quad by the appearance's height and build bone scales, and lights it at runtime like
    §3.4 impostors.

  Skin tone, hair and outfit colours, height and build therefore read at crowd distance with **no
  per-character bake**. Tattoos and faces are < 3 px at that distance and are not represented. A2 ↔ A3
  changes use §3.3's 8-frame dithered cross-fade. Cost ≤ 0.1 ms for 200 impostors, inside forward opaque.

**Geometry at scale: rest-mesh bakes and the skinned-output ring (Ph2).** The composites solve texture
memory; geometry needs the same treatment for 260 characters. Per character, the GPU chain is:
1. a **rest mesh baked once per appearance and LOD**;
2. plus FACS deltas, for F0 faces only (§7.6b);
3. linear-blend skinning from the palette;
4. a write into the **skinned-output ring**, which the prepass, shadows and forward read through the
   `Skinned` vertex factory (vertex pulling, §3.1).

*When each parameter kind is evaluated* (06 §10):

| Kind | Evaluated | How | Per-frame GPU cost |
|---|---|---|---|
| `Morph` (body and face sliders) | **Once per appearance and LOD** | The rest-mesh bake: base LOD + Σ wᵢ·Δᵢ over sparse deltas. Outfit meshes carry the body-shape deltas, transferred at cook time (T22), so clothing follows the body | 0 |
| `DnaBlend` (≤ 4 head bases) | **Once per appearance and LOD** | The same bake: the head region is Σ βⱼ·baseⱼ. For F0-capable heads it also blends the bases' 52 FACS delta sets (§7.6b) | 0 |
| `BoneScale` | **Once per appearance** | 02 §7.2 folds the scales into the appearance's bind skeleton, so every palette already carries them. That keeps attachments, IK and hitbox joints consistent, and 03 bakes no vertices for it | 0 (the palette build's per-joint multiply exists anyway) |
| `MeshOption` | On change | Selects outfit and part meshes. Each is its own skinned draw that shares the character's palette | — |
| FACS shapes (Ph4) | Every frame, F0 faces only | §7.6b | ≤ 0.15 ms REF, ≤ 0.1 ms MIN |
| Skinning | At the tier's palette rate (below) | Compute | ≤ 0.6 ms in BENCH-1 |

*Per-tier vertex maxima* (all parts, outfits and hair cards; T22 lints the LODs; LOD selection still follows
§3.3's projected error): hero ≤ 80k vertices (MIN 50k), A0 ≤ 40k (25k), A1 ≤ 12k (8k), A2 ≤ 5k (3k).

*Rest-mesh bake.*
- **Format:** 16 B per vertex: position as 3 × unorm16 over the LOD's bounds (≤ 0.03 mm steps), plus octahedral
  normal and signed tangent as 2 × snorm16 each. UVs, bone indices and weights stay in the shared base-mesh
  stream.
- **Scheduling:** bakes run on async compute in the composite job's priority order (hero, A0, A1, A2),
  capped at ≤ 0.1 ms per frame (p50 ≤ 0.02 ms in steady state). One bake costs about 0.04 ms for a hero LOD
  and 0.02 ms for A0, dominated by the sparse morph deltas (≈ 60 active sliders) and the head's 4-basis
  blend.
- **Cold arrival in BENCH-1** (260 new characters): ≈ 2.6 ms of bake work on REF, so ≈ 26 frames (≈ 0.45 s)
  at the cap. Until an appearance is baked, the character draws its species and body-type base mesh with
  `BoneScale` applied. That is the geometry twin of the tint fallback: never missing, only generic in shape
  for < 1 s.
- **Cache:** keyed by (base mesh, LOD, hash of the geometry-affecting parameters), so identical appearances
  share one entry. It is an LRU sub-pool of the §1.3 geometry pool, like the composite cache: entries
  unreferenced for 10 s go first, and a coarsened LOD keeps its finer bake for 10 s of hysteresis. Caps
  (§8.2): REF High 96 MB, MIN Low 48 MB.
- **Worst case, BENCH-1:**
  - REF: (4 × 80k + 32 × 40k + 96 × 12k + 128 × 5k) × 16 B = 54 MB, plus 25 % for LOD cross-fades and
    hysteresis, plus 8 F0 FACS delta sets × 1.4 MB, ≈ **79 MB** against the 96 MB cap.
  - MIN, under 02's MIN governor (A0 ≤ 16, A1 ≤ 48): (4 × 50k + 16 × 25k + 48 × 8k + 192 × 3k) × 16 B = 25 MB,
    ≈ **37 MB** with the same overheads (4 F0 sets), against the 48 MB cap.

*Skinned-output ring* (tier slots suballocated in the geometry pool; a slot is taken when a character enters
a tier and freed when it leaves). Every skinned tier uses the same **32 B** vertex layout: two 12 B position
slots (A and B, ping-ponged) plus 8 B of packed normal and tangent. Tiers differ only in reskin rate and
vertex maxima:

| Tier | Reskin rate (02 §7.2's palette rate) | Layout per vertex | Motion vectors |
|---|---|---|---|
| Hero, A0 | Every frame | 32 B | Per vertex, exact on every frame |
| A1 | 30 Hz, staggered by entity: half reskin each frame at 60 fps, a quarter at 120 fps | 32 B | Per vertex, exact on every frame, including hold frames (below) |
| A2 | 15 Hz: a quarter each frame at 60 fps, an eighth at 120 fps | 32 B | The same |
| A3 | Not skinned (crowd impostor) | — | From the instance transform (impostor quads, §3.4) |

**Why A1 and A2 need their own previous positions.** A reduced-rate tier's geometry moves in steps: it holds
a pose for 2 frames (A1) or 4 frames (A2) at 60 fps, and for 4 or 8 frames at 120 fps, then jumps to the next
pose. Motion taken only from the instance transform is zero for the pose part of every jump, so the upscaler
fetches history from where a limb *used to be*. With the numbers:
- **Size on screen.** An A1 figure at 15–30 m is ≈ 60–120 px tall at 1080p (≈ 75–150 px at 1440p). An A2
  figure at 30–80 m is ≈ 20–60 px tall. MIN (720p → 1080p, FSR 2 Quality, 1.5×) and Performance mode
  (1506 × 847 → 1440p, FSR 2 Balanced, 1.7×) are exactly the cases where FSR 2 leans hardest on the motion
  vectors.
- **Error per step.** A swinging forearm moves ≈ 2 m/s. At 30 Hz that is ≈ 6.7 cm per step, ≈ 3 display px
  at 20 m on 1080p. At 15 Hz it is ≈ 13 cm per step, ≈ 2.5 px at 50 m. Against a bright window or a
  hologram the result is a visible trail on every step, which is AAA-REN-8 L7 ("TAA ghosting and upscaler
  artefacts, on REF and on MIN"). Variance clipping is not a reliable fix. At 1.5–1.7× FSR 2 rectifies
  history against a render-resolution neighbourhood that is coarser than a 5–10 px limb, and its thin-feature
  locks exist to keep such features in history longer.

**The rule.** Each character's ring header holds `cur` (the slot displayed this frame) and `stepFrame` (the
frame of its last reskin). 02 bumps `AnimLod.poseSerial` on each pose update (02 §7.2):
- **Reskin frame** (`poseSerial` changed). Skinning writes the new pose into slot `cur ^ 1`, flips `cur` and
  sets `stepFrame` to this frame. The prepass reads the previous position from the other slot, which holds
  exactly the pose displayed last frame. Motion is `project(xform_prev · pos_prev) − project(xform · pos)`.
- **Hold frame.** Nothing is written. The prepass sees `stepFrame ≠ frame` and reads the previous position
  from slot `cur`, the same pose. Pose motion is therefore zero, which is exact, because the displayed pose did
  not change. Instance motion still comes from the previous transform (§2.6).
- **Slot acquire** (entering a tier, a LOD change, the swap from base mesh to baked rest mesh, a new
  `MeshOption`). The first dispatch skins the character twice: the *last displayed* palette into slot B and
  the new palette into slot A. The first frame in a new slot therefore also has exact motion. 03 keeps each
  character's current and last-displayed palettes on the GPU (2 × ≤ 12 KB; ≤ 6.4 MB for 260 characters, in the
  GPU-scene heap). The extra skin costs ≤ 0.02 ms for an A1 LOD, and 02's tier hysteresis limits how often it
  happens.
- **Discontinuities.** A teleport, `Reparent()` or camera cut sets the instance's `prevValid = 0`. Previous
  transform and previous position then equal the current ones, and the upscaler's `reset` or disocclusion
  handles the frame.
- **Cost.** Skinning writes the same 20 B per vertex as before, so the skinning line is unchanged. On a
  reskin frame the prepass reads 12 B more per A1–A2 vertex. That is ≤ 9 MB per frame on REF BENCH-1
  (≈ 0.74 M reskinned A1–A2 vertices), ≤ 0.02 ms, inside the prepass row. On hold frames both reads hit the
  same slot.

**Rejected alternative: FSR 2's reactive and composition masks for A1–A2 pixels.** Masks lower the history
weight, so they trade the trail for shimmer and aliasing on ≈ 200 small figures, which is the other half of
L7. They also leave Helios TAA, DLSS and XeSS, which ignore FSR 2's masks or weight them differently, with
the wrong vectors. Correct vectors fix every `IUpscaler` at once and cost ≈ 22 MB on REF.

**Ring size and skinning cost.**
- **Size, worst case in BENCH-1:** REF (4 × 80k + 32 × 40k + 96 × 12k + 128 × 5k) × 32 B = 10.2 + 41.0 + 36.9 +
  20.5 = **108.5 MB** (cap **112 MB**). MIN (4 × 50k + 16 × 25k + 48 × 8k + 192 × 3k) × 32 B = 6.4 + 12.8 +
  12.3 + 18.4 = **49.9 MB** (cap **56 MB**). The previous-position slots of A1 and A2 add 13.8 + 7.7 MB on REF
  and 4.6 + 6.9 MB on MIN, over round 3's 20 B layout. A character whose slot would overflow the cap stays in
  its lower tier. 02's governor already caps A0 and A1, and BENCH-1's 260 characters bound A2, so this never
  binds in the BENCH scenes.
- **Skinning cost:** linear blend, 4 weights, one thread per vertex, and the palette (≤ 256 joints) in
  groupshared memory. Throughput targets (measured in WP-2.3) are **REF ≥ 4 Mvertices/ms** and **MIN ≥ 1.6
  Mvertices/ms**. Skinned vertices per frame in BENCH-1:
  - REF: 4 × 80k + 32 × 40k + 96 × 12k / 2 + 128 × 5k / 4 ≈ 2.3 M, so ≈ 0.58 ms.
  - MIN: 4 × 50k + 16 × 25k + 48 × 8k / 2 + 192 × 3k / 4 ≈ 0.93 M, so ≈ 0.58 ms.

  Both fit the **≤ 0.6 ms** skinning line in §8.1's character sub-budgets.
- **Culling bounds:** extract sends each character's palette-derived bounds, inflated by 10 %, so
  skinned draws cull like any instance (§3.2).

**Acceptance:**
- **RC-10:** BENCH-1 on **MIN with 200 unique appearances**. No two avatars may share a composite key or a
  rest-mesh key.
  - The composite cache stays ≤ 96 MB, the rest-mesh cache ≤ 48 MB, the skinned-output ring ≤ 56 MB, the
    texture pool ≤ 1.8 GB, the geometry pool ≤ 0.8 GB and VRAM ≤ 5 GB.
  - Every A0–A2 character shows its own composite and its own baked shape ≤ 1 s after arrival.
  - Composite GPU time is ≤ 0.3 ms per frame at p99. The Characters row (skinning, faces and bakes) stays
    within its §8.1 line + 10 %.
  - **Crowd capture through FSR 2.** A 20 s walk through the concourse crowd is captured twice: on MIN at
    1080p Low (FSR 2 Quality, 720p → 1080p, 60 fps cadence), and on REF at 1440p with Performance mode's
    resolver (FSR 2 Balanced, 1506 × 847 → 1440p, 120 fps cadence). Performance mode's perf gate is BENCH-4
    (RC-9), but this capture is an image-quality check of its render scale on the densest crowd. Both
    captures pass §8.4a's **temporal-stability check** on the A0–A2 masks: motion-vector consistency ≥ 99.5 %,
    and the limb-mask ꟻLIP p95 ≤ 1.15 × the full-rate control.
- **Goldens (RC-1, per commit on lavapipe):** a 16-character lineup at each tier (textures and baked shapes,
  including extreme `Morph` and `BoneScale` values in clothing), the T22 stress grid through the runtime
  encoders, a crowd-impostor ring at 80–200 m, and the walk-away golden below.
- **Walk-away golden** (per commit from Ph2, when WP-2.3 lands the ring and `IUpscaler` + FSR 2):
  - **Scene.** 6 characters on scripted paths from 10 m to 90 m (A0 → A1 → A2 → A3). They walk, run, turn and
    wave (forearm speed ≥ 2 m/s) in front of a high-contrast backdrop: a bright window band over a dark
    bulkhead. `anim.forceTier` and `anim.forcePhase` hold each skinned tier for 1 s of scene time, then fade
    to A3 over 0.5 s (3.5 s per run), and put reskin steps on known frames. Until 02's crowd LOD lands (WP-3.4), the harness produces the same steps by holding each tier's
    palette and `poseSerial` for 2, 4 or 8 frames, so the reduced-rate cases gate from Ph2.
  - **Pixel scale.** Display 640 × 360 with a 21.8° vertical FOV, so every figure covers the same display
    pixels as at 1080p with a 60° FOV: an A1 figure at 20 m is ≈ 85 px tall.
  - **Resolvers.** Three runs, each at 60 and 120 fps output cadence (fixed `dt`), so A1 holds 2 and 4 frames
    and A2 holds 4 and 8:
    - Helios TAA at 1.0×;
    - FSR 2 Quality at MIN's 1.5× (427 × 240 → 640 × 360);
    - FSR 2 Balanced at Performance mode's 1.7× (376 × 212 → 640 × 360).

    FSR 2 uses its FP32 permutation, so the golden does not depend on lavapipe's FP16 support.
  - **Checks.** Each run passes RC-1 (ꟻLIP mean ≤ 0.01 against its golden, bit-identical when re-rendered)
    and §8.4a's temporal-stability check (motion-vector consistency ≥ 99.5 %; limb-mask ꟻLIP p95 ≤ 1.15 ×
    the full-rate control).
  - **Negative control.** The harness reruns the FSR 2 Quality case with `render.skin.transformOnlyMotion=1`,
    which is round 3's rule (A1–A2 motion from the instance transform only). It asserts that this run
    *fails* both checks, which proves the check sees the defect it exists for.
  - **Cost.** 13 runs: 6 resolver and cadence runs, their 6 full-rate controls and the negative control.
    Each is 210 or 420 frames, with ꟻLIP on 16 sampled frames per run. That is ≈ 3 min of lavapipe time on
    8 cores, counted inside RC-1's 10-minute suite.

### 7.6b Faces: FACS shapes, eyes and lip-sync on the GPU (Ph4; R04, AAA-REN-8 L5)

02 §7.2 owns the facial *runtime*: curves, visemes, eyes and face LOD. It hands 03 a per-face set of 52
weights plus joint overrides. This section is the GPU side.
- **Data.** Each head basis (per species, 06 §10) has 52 FACS shapes: the names the capture solver emits
  (09 §4.3.3). They are stored as sparse deltas (position f16 × 3 + normal f16 × 3 = 12 B per touched
  vertex). About 15 % of a 15k-vertex hero head is touched per shape, so a basis costs ≈ 1.4 MB. When a
  DNA-blended head becomes F0, its delta set is baked (the β-weighted union of its bases' deltas) into the
  rest-mesh cache (§7.6a), ≈ 1.4 MB per face.
- **Evaluation.** One compute dispatch per F0 face, before skinning: `rest + Σ wᵢ·Δᵢ` over the channels with
  wᵢ > 1/255, typically 10–25 of 52. Normal deltas are blended and then renormalized. Three wrinkle-map groups
  (brow, nasolabial, eye corners; one BC5 tile each per head basis) blend in the Skin shading model from the
  channel groups' weights.
- **Joints, not shapes:** jaw, tongue, teeth and eyes are driven by joints in the palette. The Eye shading
  model (refractive cornea and iris parallax, §7.6) adds a tear-line and eye-occlusion shell per eye, so
  saccades read at close range.
- **Face LOD.** F0 faces are capped at **8 on REF High and 4 on MIN Low and in Performance mode** (§8.2) and
  are chosen by 02's face LOD. F1 faces (other A0 and A1 characters) move only jaw, lid and eye joints, and
  F2 faces are neutral. Neither costs anything here.
- **Budget:** ≤ 0.02 ms per F0 face, so **≤ 0.15 ms on REF and ≤ 0.1 ms on MIN**, inside §8.1's Characters
  row. The BENCH-1 dialogue camera bookmark is the worst case (8 F0 faces in view).
- **Goldens (RC-11 Ph4):**
  - a 52-channel lineup per head basis (each channel alone at weight 1) against a DCC reference render;
  - a viseme sweep (15 visemes, 2 languages);
  - a DNA-blend head at 3 β settings with an animated FACS clip.

  All must meet RC-1's ꟻLIP limits. 02's RT-22 measures lip-to-audio sync and the CPU side.

---

## 8. Performance and quality infrastructure

### 8.1 Frame budgets (GPU ms p50)

Every scene, tier and frame rate that AAA-REN-1/2/3 gate has its own column below. RC-8 and RC-9 hold every
pass to its column + 10 %, and the nightly lab fails a regression > 5 % (§8.3).

**Frame targets.** The GPU wall target leaves the rest of the frame for p99 spikes and pacing margin. At
120 fps it also leaves CL-6's 0.3 ms queue slack (08 §1.3a).

| Gate | Scenes | Tier and preset | Render → output | Frame (average) | p99 | GPU wall target | CPU columns (02 §2.4) |
|---|---|---|---|---|---|---|---|
| AAA-REN-1 | BENCH-1, 2, 4, 5 | REF High | 1440p, TAA | 16.7 ms (60 fps) | ≤ 20 ms | ≤ 14.5 ms | REF |
| AAA-REN-1 | BENCH-3 | REF High | 1440p, TAA | 22.2 ms (45 fps) | ≤ 33 ms | ≤ 18.5 ms | REF BENCH-3 |
| AAA-REN-3 | BENCH-4 | REF Performance mode (below) | 1506 × 847 → 1440p, FSR 2 Balanced | 8.33 ms (120 fps) | ≤ 12.5 ms (03's own gate) | ≤ 7.2 ms | REF BENCH-4 Performance |
| AAA-REN-2 | BENCH-1, 2, 4 | MIN Low | 720p → 1080p, FSR 2 Quality | 16.7 ms (60 fps) | ≤ 33 ms | ≤ 12 ms | MIN BENCH-1, 2, 4 |
| AAA-REN-2 | BENCH-3, 5 | MIN Low | 720p → 1080p, FSR 2 Quality | 33.3 ms (30 fps) | ≤ 50 ms (03's own gate) | ≤ 22 ms | MIN BENCH-3, 5 |

#### 8.1.1 REF GPU budgets (1440p)

Async rows overlap graphics work, which is why the wall time is below the sum.

| Pass | BENCH-1 | BENCH-2 | BENCH-3 (45 fps) | BENCH-4 | BENCH-4 Performance (120 fps) | BENCH-5 |
|---|---|---|---|---|---|---|
| Scatter, 2-phase cull + HZB | 0.4 | 0.55 | 1.35 | 0.45 | 0.4 | 0.4 |
| Characters: skinning, FACS shapes, rest-mesh bakes (§7.6a–b; sub-budgets below) | 0.8 | 0.05 | 0.05 | 0.6 | 0.4 | 0.5 |
| Depth/normal/motion prepass | 1.4 | 1.2 | 2.0 | 1.3 | 0.6 | 1.4 |
| Shadows (§4.5) | 1.6 | 1.8 | 2.5 | 1.7 | 1.0 (≤ 16 atlas updates) | 2.0 |
| Binning + GTAO + wind field + character composites (async; composites p50, bursts ≤ 0.3, §7.6a) | 0.8 | 0.7 | 0.7 | 0.7 | 0.4 | 0.8 |
| Atmosphere + sky | 0.1 | 0.3 | 0.1 | 0.05 | 0.05 | 0.3 |
| Terrain tiles + horizon maps (async; horizons use ≤ 0.15 of the slice height tiles leave unused, §5.4a) | — | 0.8 | — | — | — | 0.3 |
| DDGI trace/update + AS builds (async, Ph3; §4.6a) | 0.8 | 0.8 | 0.3 | 0.8 | 0.4 (half update rate) | 0.9 |
| Forward opaque (incl. streaming feedback, weather modifier) | 4.0 | 3.0 | 4.5 | 3.4 | 1.5 | 3.4 |
| Froxel fog | 0.5 | 0.7 | 0.5 | 0.7 | 0.35 | 0.7 |
| Clouds / nebula | 0.2 (Harrow's 2D shell through windows) | 1.5 | 1.5 | — | — | 1.5 |
| Clouds, fast mode (> 500 m/s within 2 km of the layer; **replaces** the row above, never added to it; §5.8) | — | ≤ 1.0 (in-layer segment) | — | — | — | — |
| SSR | 0.6 | 0.6 | 0.4 | 0.6 | — (probe fallback) | 0.6 |
| Weather: precipitation, occlusion, lightning (Ph3; §5.8a) | — | 0.3 | — | — | — | 0.3 |
| Transparents + particles | 1.2 (hologram WBOIT) | 1.2 | 3.8 | 1.8 | 1.0 | 0.8 |
| TAA / upscale | 0.6 | 0.6 | 0.6 | 0.6 | 0.7 (FSR 2) | 0.6 |
| Post (incl. Burley screen-space SSS on High, §7.6) | 1.1 (SSS 0.3) | 0.8 | 0.8 | 0.9 (SSS 0.15) | 0.6 | 0.8 (SSS 0.1) |
| UI, HUD, diegetic | 0.9 (40 screens 0.5) | 0.4 | 0.8 | 0.5 | 0.4 | 0.5 |
| **Sum / wall with async overlap** | **15.0 / ~13.8** | **15.3 / ~13.8** | **19.9 / ~18.4** | **14.1 / ~13.0** | **7.8 / ~7.2** | **15.8 / ~14.5** |

- **Scene profiles.**
  - *BENCH-1* is Harrow High's concourse, a station interior. Harrow shows through the windows from orbit,
    using resident orbit-level tiles and its 2D cloud shell, so no tiles are produced.
  - *BENCH-4* is budgeted as the Hollow Vault's sealed interior cells, with no visible sky, terrain or
    clouds. The sky-view LUT still updates for probe relighting. If the content opens the Vault to the sky,
    the atmosphere and 2D-shell rows add ≤ 0.5 ms at 60 fps, inside the 1.5 ms left under the wall target.
    At 120 fps the opening's sky pixels displace forward shading, so it is close to neutral; T26 flags the
    column if it is not.
- **BENCH-3** is the full 2,000-ship, 20-capital battle (01 §3.2). It needs ≥ 45 fps (22.2 ms) with p99
  ≤ 33 ms, which leaves ~3.8 ms of headroom for volley peaks.
- **Phases.** Ph1–2 builds have no DDGI or weather rows, so REF BENCH-2's sum is 1.1 ms lower.
- **The 120 fps column** comes from Performance mode (below). It shades 0.35× the pixels, drops volumetric
  clouds and SSR, runs DDGI at half rate and holds the crowd to the performance governor (A0 ≤ 16, ≤ 4 F0
  faces). Its ~7.2 ms wall plus CL-6's 0.3 ms slack is the 7.5 ms that 08 §1.3a's latency budget assumes.

#### 8.1.2 MIN GPU budgets (1080p Low, 720p render + FSR 2)

The columns are measured on both MIN GPUs (GTX 1660 SUPER and RX 5600 XT), and each budget binds the slower
one. MIN has no mesh shaders (compute triangle culling, §3.2), RT, SSR or volumetric clouds. Its GI is relit
assembly-time probes, evaluated inside forward opaque.

| Pass | BENCH-1 | BENCH-2 | BENCH-3 (30 fps) | BENCH-4 | BENCH-5 (30 fps) |
|---|---|---|---|---|---|
| Scatter, 2-phase cull + HZB (compute triangle culling) | 0.5 | 0.55 | 1.6 | 0.5 | 0.7 |
| Characters: skinning, FACS shapes, rest-mesh bakes | 0.7 | 0.05 | 0.05 | 0.55 | 0.65 |
| Depth/normal/motion prepass | 1.1 | 1.2 | 2.2 | 1.0 | 1.4 |
| Shadows (§4.5 Low) | 1.2 | 1.6 | 2.4 (incl. 2 × 2048² D16 capital maps) | 1.3 | 1.8 |
| Binning + GTAO (¼ res) + wind field + composites (async) | 0.5 | 0.5 | 0.4 | 0.4 | 0.5 |
| Atmosphere + sky | 0.1 | 0.3 | 0.1 | 0.05 | 0.3 |
| Terrain tiles + horizon maps (async) | — | 0.8 | — | — | 0.3 |
| Forward opaque (incl. relit probes, streaming feedback, weather modifier) | 3.6 | 3.0 | 5.0 | 3.0 | 3.8 |
| Froxel fog (80 × 45 × 32) | 0.4 | 0.5 | 0.4 | 0.5 | 0.5 |
| Clouds / nebula | 0.2 (2D shell) | 0.4 (2D shell; also in fast mode) | 0.8 (nebula at ¼ res, 24 steps) | — | 0.4 (2D shell) |
| Weather (Ph3) | — | 0.2 | — | — | 0.2 |
| Transparents + particles | 1.0 | 1.0 | 2.6 (256k, half-res smoke) | 1.4 | 0.8 |
| TAA / upscale (FSR 2) | 0.9 | 0.9 | 0.9 | 0.9 | 0.9 |
| Post (pre-integrated skin is in forward) | 0.7 | 0.7 | 0.7 | 0.7 | 0.7 |
| UI, HUD, diegetic, brackets | 0.8 (40 screens at 15 Hz) | 0.4 | 1.0 (2,000 brackets 0.3) | 0.5 | 0.5 |
| **Sum / wall with async overlap** | **11.7 / ~11.3** | **12.1 / ~11.5** | **18.2 / ~17.9** | **10.8 / ~10.5** | **13.5 / ~13.0** |

- **MIN BENCH-3 is the likeliest AAA-REN-2 failure** (2,000 ships, 20 capitals, 2,000 brackets and 256k
  particles on a GTX 1660 SUPER), so it has its own plan:
  - Low starts the fleet mesh band at 64 display px (§3.4, §8.2), which keeps ≤ 120 hulls in it at the
    BENCH-3 camera. Fleet geometry is ≤ 6.5 ms across prepass, shadows and forward (REF ≤ 4.5 ms).
  - Only the 2 largest capitals within 20 km get dedicated maps (2 × 2048² D16, §4.5).
  - The significance pass keeps the top 256k particles, and smoke renders at half resolution (§6.1).
  - The 2,000 brackets are one instanced sprite pass.
  - The planned ~17.9 ms wall leaves ~15 ms under the 33.3 ms frame. Volley peaks are held to GPU p99
    ≤ 28 ms, which T26 reports per run.
- **At 30 fps, MIN BENCH-3 and BENCH-5 are bound by CPU and VRAM, not by the GPU.** CPU is set by 02 §2.4's
  30 fps columns (below) and VRAM by the check below. The preset stays Low; the spare GPU time is headroom,
  not quality.
- MIN's Ph1–2 BENCH-2 has no weather row (0.2 ms lower).

#### 8.1.3 Sub-budgets

**Planet-surface sub-budgets** (already included in the rows above; T26 reports them as child zones):

| Vegetation (§5.6a) | Row | REF BENCH-5 | REF BENCH-2 (surface segment) | MIN BENCH-2 | MIN BENCH-5 |
|---|---|---|---|---|---|
| Foliage instance cull + LOD select | Scatter/cull | 0.15 | 0.15 | 0.15 | 0.15 |
| Grass blade generation (mesh shader / compute) | Scatter/cull | 0.1 | 0.1 | 0.1 | 0.1 |
| Foliage prepass (alpha-tested) | Prepass | 0.35 | 0.3 | 0.3 | 0.3 |
| Foliage shadows (cascades 0–1 full, 2–3 impostor) | Shadows | 0.4 | 0.35 | 0.3 | 0.3 |
| Foliage and impostor forward shading | Forward opaque | 0.7 | 0.6 | 0.5 | 0.5 |
| Wind field + interaction map | Binning (async) | 0.05 | 0.05 | 0.05 | 0.05 |
| **Vegetation total** | | **1.75** | **1.55** | **1.4** | **1.4** |
| Weather surface modifier (wetness, puddles, snow) | Forward opaque | ≤ 0.15 | ≤ 0.15 | ≤ 0.1 | ≤ 0.1 |
| Streaming feedback write + compaction (§1.3a) | Forward opaque | ≤ 0.12 | ≤ 0.12 | ≤ 0.12 | ≤ 0.12 |
| Far terrain shadows: horizon-map generation (§5.4a) | Terrain tiles (async, unused share) | ≤ 0.15 | ≤ 0.15 | ≤ 0.15 | ≤ 0.15 |
| Far terrain shadows: horizon lookup in terrain, far receivers and froxels | Forward opaque / froxel fog | ≤ 0.05 | ≤ 0.05 | ≤ 0.05 | ≤ 0.05 |

**Character sub-budgets** (§7.6a–b; included in the rows named):

| Character item | Row | REF BENCH-1 | REF BENCH-5 | REF BENCH-4 Performance | MIN BENCH-1 |
|---|---|---|---|---|---|
| Skinning into the ring (A1 half and A2 a quarter per frame at 60 fps) | Characters | 0.6 (2.3 M vertices) | 0.4 (1.5 M) | 0.3 (1.1 M) | 0.6 (0.93 M) |
| FACS shapes (F0 faces, Ph4) | Characters | 0.15 (8 faces) | 0.08 (4) | 0.08 (4) | 0.1 (4) |
| Rest-mesh bakes, p50 (hard cap per frame) | Characters | 0.02 (0.1) | 0.02 (0.1) | 0.02 (0.1) | 0.02 (0.1) |
| Composite + BC encode, p50 (hard cap per frame) | Async (binning row) | 0.1 (0.3) | 0.1 (0.3) | 0.05 (0.3) | 0.1 (0.3) |
| Crowd-class impostors (A3) | Forward opaque | ≤ 0.1 | ≤ 0.05 | ≤ 0.05 | ≤ 0.1 |
| Burley screen-space SSS (pre-integrated skin on Low and in Performance mode is in forward) | Post | 0.3 | 0.1 | — | — |
| Composite cache, VRAM (cap) | Texture pool | 285 MB worst (320 MB) | ≤ 220 MB (320 MB) | ≤ 150 MB (320 MB) | 71 MB worst (96 MB) |
| Crowd impostor atlases, VRAM | Texture pool | ≤ 56 MB | ≤ 56 MB | ≤ 28 MB | ≤ 14 MB |
| Skinned-output ring, 32 B per vertex on every tier, VRAM (cap) | Geometry pool | 108.5 MB worst (112 MB) | ≤ 75 MB (112 MB) | ≤ 56 MB (112 MB) | 49.9 MB worst (56 MB) |
| Rest-mesh cache incl. FACS deltas, VRAM (cap) | Geometry pool | 79 MB worst (96 MB) | ≤ 55 MB (96 MB) | ≤ 40 MB (96 MB) | 37 MB worst (48 MB) |

#### 8.1.4 VRAM check (MIN, 5 GB)

These are the two MIN scenes most likely to break a §1.3 line. Figures are GB; RC-9 and RC-10 read them per
heap and per memory tag from `VK_EXT_memory_budget` and T26.

| §1.3 category | MIN line | MIN BENCH-1 | MIN BENCH-3 | What fills it |
|---|---|---|---|---|
| Render targets + transient heap | 0.5 | 0.40 | 0.45 | 720p HDR, D32, thin G-buffer and motion; FSR 2 history and locks at 1080p (≈ 60 MB). BENCH-3 adds half-res particle targets and the compute-culled transient index buffer (≤ 64 MB) |
| Texture streaming pool | 1.8 (cap) | ≤ 1.8 (working set ≤ 1.3) | ≤ 1.8 (working set ≤ 1.3) | The pool fills to its cap as a cache; the working set is mips requested in the last 5 s. BENCH-1: station materials ≈ 0.8, shared outfit textures 0.25, composites 0.07, crowd atlases 0.014, tails 0.09. BENCH-3: hull trims and liveries ≈ 0.6, impostor atlases (≈ 48 hull/livery groups at mip 1, ≈ 4 MB each) 0.2, VFX flipbooks and decal atlases 0.25, nebula cubemap and IBL 0.1, tails 0.09 |
| Geometry pool | 0.8 | 0.55 | 0.62 | BENCH-1: station 0.25, character base meshes 0.2, skinned ring ≤ 0.056, rest-mesh cache ≤ 0.048. BENCH-3: mesh-band hull LODs (≈ 16 hull types) 0.12, the 20 capitals' LOD chains (Ph4 cluster pages) 0.35, station and asteroid field 0.12, cockpit and avatar 0.03 |
| GPU scene, materials, lights | 0.15 | 0.08 | 0.1 | 512k slots × 64 B = 32 MB, materials, 1,024 lights; BENCH-3's 2,000 grid transforms are 0.2 MB |
| Terrain tiles + virtual texture | 0.3 | 0.3 (reserved) | 0.3 (reserved) | Allocated at startup and never lent to other pools; BENCH-1 uses orbit tiles only, BENCH-3 none |
| Shadows | 0.25 | 0.045 | 0.06 | Cascades 3 × 1024² D32 (12 MB), atlas 4096² D16 (32 MB); BENCH-3 adds the capital maps, 2 × 2048² D16 (16 MB) (§4.5) |
| Volumetrics, particles | 0.25 | 0.08 | 0.2 | BENCH-3: 256k-particle SoA and lists ≈ 24 MB, ribbons and trails 32 MB, nebula fly-through volume 64 MB, froxels 1 MB, shield-impact ring |
| Upload/readback, UI/RTT atlases | 0.25 | 0.2 | 0.12 | Upload ring 64 MB (counted even if the ReBAR heap is absent) and readback 16 MB. BENCH-1: 40 screens in 4 RTT pages (85 MB) and HUD 32 MB. BENCH-3: bracket and HUD atlases 40 MB |
| Reserve | 0.7 | 0.7 | 0.7 | Swapchain, descriptor heaps, PSO and driver memory, fragmentation |
| **Total** | **5.0** | **≤ 4.2** | **≤ 4.35** | A 6 GB card keeps ≥ 1.6 GB for the OS and other applications |

#### 8.1.5 CPU budgets (render side)

02 §2.4 holds the game-thread and per-system tables for the same columns. They are gated by RT-12's REF
clause, MIN clause, 30/45 fps clause and 120 fps clause. The render side:

| Render CPU item (ms, p50) | REF 60 fps (BENCH-1/2/4/5) | REF BENCH-3 (45 fps) | REF BENCH-4 Performance (120 fps) | MIN 60 fps (BENCH-1/2/4) | MIN BENCH-3/5 (30 fps) |
|---|---|---|---|---|---|
| Extract (game thread) | ≤ 1.0 | ≤ 1.5 | ≤ 0.8 | ≤ 1.5 | ≤ 2.0 |
| Prepare (wall) | ≤ 1.5 | ≤ 2.0 | ≤ 1.2 | ≤ 2.0 | ≤ 2.8 |
| Graph compile on a topology change (a cache hit is ≤ 0.05) | ≤ 0.3 | ≤ 0.3 | ≤ 0.2 | ≤ 0.4 | ≤ 0.4 |
| Recording across jobs (wall; MIN has 4 workers) | ≤ 2.0 | ≤ 2.5 | ≤ 1.5 | ≤ 3.0 | ≤ 3.5 |
| Submit | ≤ 0.3 | ≤ 0.3 | ≤ 0.3 | ≤ 0.4 | ≤ 0.4 |
| Streaming residency (parallel render job, §1.3a) | ≤ 0.5 | ≤ 0.5 | ≤ 0.5, run at 60 Hz (alternate frames) | ≤ 0.5 | ≤ 0.5 |
| **Render thread wall** | **≤ 4** | **≤ 5** (RC-5) | **≤ 3.5** | **≤ 6** | **≤ 7** |
| Render jobs, core-ms per frame (02 §2.4 per-system row) | 4.0 | 7.0 | 3.0 | 5.5 | 9.5 (BENCH-3), 8.0 (BENCH-5) |

On REF, prepare plus graph compile plus batch-0 recording is ≤ 2.2 ms at both 60 and 120 fps. That is the
render critical path in 08 §1.3a's latency budget (CL-6, row 5).

#### 8.1.6 Performance mode (AAA-REN-3; the 120 fps column)

Performance mode is High with these changes:
- **Resolution:** FSR 2 Balanced (1.7×: 1506 × 847 → 1440p).
- **Removed:** volumetric clouds (the 2D shell stays) and SSR (the probe fallback stays).
- **GI and shadows:** DDGI at half update rate; ≤ 16 local-light atlas updates per frame.
- **Characters:** pre-integrated skin instead of Burley SSS; ≤ 4 F0 faces.
- **Planets and effects:** Medium vegetation and precipitation; motion blur at half resolution.
- **CPU:** 02 §2.4's performance `cpu.*` settings. These are the performance crowd governor (A0 ≤ 16,
  A1 ≤ 48), 60 Hz cosmetic physics, HUD layout and streaming residency, and the tick/Phase A interleave.

It targets an 8.33 ms frame with a GPU wall ≤ 7.2 ms, a game thread ≤ 6 ms p50 and a render thread ≤ 3.5 ms.
The first-launch benchmark offers it on REF-class GPUs when a ≥ 120 Hz display is attached.

### 8.2 Scalability

| Setting | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Render scale | 0.5–0.67 + FSR 2 | 0.67 | 1.0 TAA | 1.0 |
| Sun shadows (`csmFar`, §4.5) | 3 × 1024², 1 km | 4 × 1536², 2 km | 4 × 2048², 2 km | 4 × 2048², 4 km → VSM (Ph4) |
| Capital-ship shadow maps (D16, §4.5) | 2 × 2048² | 4 × 2048² | 4 × 4096² | 4 × 4096²; dynamic VSM pages when VSM is on |
| Local-light shadow atlas (D16) / tile updates per frame | 4096² / 16 | 8192² / 32 | 8192² / 32 | 8192² / 32 |
| Fleet mesh band starts at (display px, §3.4) | 64 | 56 | 48 | 40 |
| Far terrain shadows (horizon map per tile, §5.4a) | 17² × 8 azimuths | 33² × 8 | 33² × 8 | 33² × 8 |
| Clouds (normal / fast mode, §5.8) | 2D shell | 24 / 12 steps | 48 / 20 steps | + fly-through detail / 20 steps |
| Froxels | 80 × 45 × 32 | 160 × 90 × 64 | same | 240 × 135 × 96 |
| GTAO / SSR | ¼ / off | ½ / off | ½ / ½ | full / ½ |
| Particles (active cap; pools per §6.1) | 64k (256k in 2 Hz fleet-battle zones, so MIN's BENCH-3 significance pass keeps its top 256k, §6.1) | 128k (256k in fleet-battle zones) | 512k | 1M |
| Terrain error / scatter | 4 px / 25 % | 2 px / 50 % | 1 px / 100 % | 0.75 px / 150 % |
| Vegetation: grass r_g / tree LOD0 / impostor end | 12 m / 15 m / 1.5 km | 20 m / 20 m / 2 km | 30 m / 30 m / 3 km | 45 m / 50 m / 5 km |
| Precipitation particles (rain / snow) | 20k / 15k | 30k / 20k | 60k / 40k | 100k / 60k |
| GI | Relit assembly-time probes | Relit assembly-time probes | DDGI if RT-capable, else probes | DDGI, 2× rays per probe |
| Texture pool (cap; §1.3a may shrink it under budget pressure) | 1.8 GB | 2.5 GB | 4 GB | 6 GB |
| Character composites: hero / A0 / A1–A2 resolution; cache cap, inside the texture pool (§7.6a) | 1024² / 512² / 256²; 96 MB | 2048² / 512² / 256²; 160 MB | 2048² / 1024² / 512²; 320 MB | 2048² / 1024² / 512²; 480 MB (longer retention) |
| Character geometry: skinned-output ring / rest-mesh cache caps, inside the geometry pool; F0 faces (§7.6a–b) | 56 / 48 MB; 4 | 80 / 64 MB; 4 | 112 / 96 MB; 8 | 144 / 128 MB; 8 |
| Skin | Pre-integrated | Pre-integrated | Burley SSS | Burley SSS |

On first launch a 10 s GPU benchmark picks the preset. MIN runs Low within the §1.3 MIN budgets (1.8 GB
texture pool). **Performance mode** is High with §8.1.6's changes; it is not a fifth column, because it
exists for one gate (AAA-REN-3) and inherits everything else from High.

### 8.3 Profiling

- Per-pass timestamp queries on every queue, read 2 frames late, feed **Tracy GPU zones** (0.14.1)
  and the T26 overlay (passes vs budget, per-heap VRAM, PSO misses, culling stats).
- **Nightly lab** (09): REF, MIN and SHOWCASE on Windows and Linux run the BENCH scenes and store
  per-pass p50/p99. A regression > 5 % or a budget breach fails the night (AAA-REN-1/2, AAA-PLT-5).

### 8.4 Golden images (AAA-REN-7)

- **Harness:** `helios-rendertest` renders deterministic scenes (fixed camera, time, seeds, jitter;
  temporal warm-up) to EXR/PNG.
- **Comparison:** ꟻLIP (NVIDIA, BSD-3, verify when vendoring), mean ≤ 0.01 plus per-test max error.
  Goldens live in Git LFS per backend and driver; CI pins the Mesa image; REF/MIN hardware keeps
  nightly goldens.
- **Lavapipe realities:** it rasterizes on the CPU, so tests run at 320 × 180 to 640 × 360 and the
  per-commit suite gets ≤ 10 min on 8 cores. Mesa 25.2 lets it cover the mesh-shader and ray-query
  paths as well as the cap-masked fallbacks.
- **Race check:** each test renders twice and must be bit-identical, which catches missing barriers.
- **Streaming determinism:** feature goldens run with `stream.forceResident=1` (every mip and LOD resident;
  feedback still executes). Separate **streaming goldens** fly scripted BENCH-2 and BENCH-6 camera paths
  against a simulated I/O device with a fixed latency schedule. They compare the residency-event trace
  (request, upload and evict order per frame) as a Null-style trace golden, plus the RC-12 histogram
  thresholds.
- Null trace goldens run on every toolchain, including GPU-less MSVC.

#### 8.4a Temporal-stability check (AAA-REN-8 L7; §7.3's motion-vector contract)

A still-image ꟻLIP golden cannot see ghosting that appears only between frames, and Helios TAA at 1.0× hides
what FSR 2 at 1.5–1.7× shows. This check runs on any `helios-rendertest` replay or T26 capture, on lavapipe and
on hardware, through the shipping `IUpscaler`. It needs two debug targets, which exist only in rendertest and
capture builds: a **reference motion** target and a **tag mask** (A0–A2 skinned characters by default).
- **Motion-vector consistency (MVC).** A rendertest-only prepass computes each tagged pixel's *reference*
  motion from first principles. It re-skins the rest mesh in the vertex shader with the palette that produced
  the pose displayed last frame, taken from the harness's per-frame log of displayed `poseSerial`s. This
  path is independent of the ring's slot logic. MVC is the share of tagged pixels whose shipped motion vector
  is within 0.25 render px of the reference. **Pass: ≥ 99.5 %** on every frame class (reskin, hold, slot
  acquire). Transform-only motion fails it on every reskin frame.
- **Limb-mask ꟻLIP (trail).** The limb mask is the tagged coverage of the current frame and the 8 previous
  frames, dilated by 2 display px, so it contains the trails as well as the limbs. On sampled frames the
  harness freezes the simulation and renders a **reference**: 16 jittered samples accumulated at display
  resolution with no history. The metric is the mean ꟻLIP between the shipped output and the reference inside
  the mask, reported per sampled frame.
- **Full-rate control.** The same capture is replayed with `anim.lodRate=full`, so A1–A2 evaluate and reskin
  every frame. That isolates what reduced-rate reskins add from what the upscaler costs on any moving limb.
  **Pass: the limb-mask ꟻLIP p95 ≤ 1.15 × the control's p95.** Helios TAA runs also report the
  history-rejection rate inside the mask, which is informational and trended by T26.
- **Where it gates.** Per commit: §7.6a's walk-away golden (RC-1). Nightly on hardware: RC-10's BENCH-1 crowd
  captures at MIN's and Performance mode's render scales, and RC-9's MIN and Performance-mode captures. A
  failure names the frame class and the tier.

---

## 9. Layout, ladder, acceptance, risks, traceability

### 9.1 Module and directory layout

```
engine/rhi/            include/helios/rhi/{device,command_list,handles,caps}.h
  src/vulkan/ (volk, VMA)  src/null/  src/d3d12/ (Ph3 seam test, Windows only)
engine/render/         L3, non-HEADLESS: graph/ pipeline/ scene/ culling/ streaming/ upscalers/ debug/
  features/{mesh,terrain,vegetation,atmosphere,clouds,weather,stars,nebula,ocean,particles,
            shields,decals,shadows,probes,ddgi,raytracing,fog,post,characters,ui_bridge}
engine/presentation/   L4 (02 owns, 03 co-owns extractors): flecs → RenderScene packet
engine/render_plugins/ aftermath, dlss, xess (optional; SDKs not vendored)
shaders/               core/ brdf/ lighting/ atmosphere/ materials/ passes/ vfx/ terrain/
apps/tools/            helios-shaderc, helios-rendertest, helios-bake (impostors, nebulae, probes)
tests/golden/          LFS goldens per backend/driver; Null trace goldens
```

### 9.2 MVP → AAA feature ladder

| Area | Ph0 | Ph1 First Light | Ph2 | Ph3 | Ph4 | Ph5 |
|---|---|---|---|---|---|---|
| RHI | Vulkan 1.3 + Null, bindless, triangle/compute goldens | Async + transfer queues, PSO lists, breadcrumbs | Crash plugins, VRAM budgets, shader hot reload | Mesh shaders, D3D12 seam test | **D3D12 gate** | RT |
| Graph | Barriers, culling, traces | Aliasing, async, parallel record, extract/prepare/submit, visualizer | Graph cache, multi-view | — | — | — |
| Geometry | Static draws | GPU scene, 2-phase HZB, MDI, discrete LOD | Meshlet culling, impostor baker | Fleet impostors, brackets, HLOD | Visibility buffer, Nanite-lite v1 | Streamed cluster LOD |
| Streaming | — | **Texture mip streaming** (GPU feedback + CPU prediction), mesh LOD streaming, transfer budgets, `StreamTexId` slots (RC-12 Ph1) | `StreamingInstaller` demand path, HD-pack caps, pool defrag (RC-12 Ph2) | BENCH-6 (RC-12 Ph3), PVT shares the feedback | Sparse-residency option | 128 KB cluster pages |
| Materials | PBR | **Clustered forward+ PBR**, clear coat, T16 instances | Layers/liveries, clustered decals, graph → Slang | Aniso, sheen, Foliage model, weather modifier | Skin, hair, eyes | — |
| Lighting | — | **CSM**, clustered lights, nebula IBL, GTAO | Shadow atlas, grid probes, SSR | Froxel fog, relit probes, **DDGI + AS management (REF), relit assembly-time probes (MIN)** | VSM | RT shadows, ReSTIR |
| Space | — | **Starfield, nebula skybox**, sun, flare | Galactic band | Nebula fly-through v1, rings, gas giants | Polish | Full volumetric nebulae |
| Planets | `hnoise` throughput spike (§5.5a) | **CDLOD + GPU tiles, Hillaire atmosphere orbit → ground**, 2D clouds, trajectory tile prefetch (RC-13), **horizon-map far terrain shadows** (§5.4a) | Biomes, scatter, stamps, foliage instances and grass (static) | Volumetric clouds (normal + fast mode), oceans, PVT, Earth-size, **foliage LODs/impostors, wind, interaction; weather effects** | Clouds golden incl. the 1,500 m/s in-layer segment | Nubis³ |
| VFX | — | GPU particles, shields, plumes, beams | T17 module stacks, ribbons, mesh particles, flipbooks, holograms, WBOIT | Simulation stages, warp tunnel | 400k particles (BENCH-3) | Volumetric explosions |
| Post | Tonemap | **HDR, histogram exposure, bloom, AgX/ACES + LUT**, TAA | `IUpscaler` + FSR 2, motion blur | DoF, exposure volumes | HDR10, DLSS/XeSS, 120 fps mode | Frame gen, local TM |
| Characters | — | Compute skinning, species base skin with the palette-tint fallback | **Composite tiers + cache + GPU BC7/BC1/BC5 encoders; rest-mesh bakes + cache and the tiered skinned-output ring with previous positions on every tier** (§7.6a; T22 MVP, one species); walk-away golden through Helios TAA and FSR 2 at 1.5× and 1.7× (§8.4a); BENCH-1 nightly | Crowd impostors (one generic class per species and body type), staggered reduced-rate skinning | T22 crowd bake per silhouette class, multi-species; **FACS faces, wrinkle maps, eye shells** (§7.6b); RC-10 BENCH-1 on MIN with 200 unique appearances | — |
| UI | **ImGui** | RmlUi, diegetic RTT, HUD, cockpit layer | — | Brackets for 1,000+ ships | — | — |
| QA | Lavapipe goldens, Null traces | Tracy GPU, budget overlay, BENCH-2 nightly | Hot reload ≤ 2 s | 30-min PSO run | Full BENCH matrix | SHOWCASE RT |

### 9.3 Acceptance criteria

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RC-1 | Every shipped feature has a lavapipe golden (ꟻLIP mean ≤ 0.01), rendered twice bit-identically; suite ≤ 10 min; Null traces on all toolchains | AAA-REN-7 | 0 |
| RC-2 | 10¹³ m test jitter < 0.05 px over 600 frames; BENCH-2 has no loading screen and no frame > 50 ms | AAA-REN-5 | 1 |
| RC-3 | BENCH-2 ≥ 60 fps at 1080p Medium on REF, Windows and Linux | AAA-REN-1 | 1 |
| RC-4 | GPU terrain matches the CPU `pcg` VM on 10⁶ samples (target bit-identical, limit ≤ 1 cm) on NVIDIA, AMD, Intel, lavapipe | AAA-CNT-1, PLT-4 | 1 |
| RC-5 | Culling 1M/200k ≤ 0.5 ms; render CPU ≤ 5 ms wall in BENCH-3 (2,000 ships, replayed capture) | AAA-REN-1 | 2 |
| RC-6 | Shader edit → visible ≤ 2 s p95; injected GPU hang names the hung pass | AAA-ITR-1, STB-1 | 2 |
| RC-7 | 30-min run: zero render-thread PSO creations, zero > 50 ms frames from PSOs, < 1 fallback use per hour warm | AAA-REN-4 | 3 |
| RC-8 | REF 1440p High: BENCH-1/2/4/5 ≥ 60 fps, p99 ≤ 20 ms; BENCH-3 (2,000 ships) ≥ 45 fps, p99 ≤ 33 ms; every pass within its §8.1.1 column + 10 % (BENCH-1, 2, 3, 4, 5, including the character and planet-surface sub-budgets); render thread within §8.1.5 (≤ 4 ms; ≤ 5 ms in BENCH-3). BENCH-2 is also gated **over its in-cloud-layer segment at 1,500 m/s** (§5.8) on its own: ≥ 60 fps, p99 ≤ 20 ms, and clouds within the fast-mode line (≤ 1.0 ms +10 %) with no ghost trails (reprojection-rejection rate reported) | AAA-REN-1 | 4 |
| RC-9 | **MIN 1080p Low:** BENCH-1/2/4 ≥ 60 fps, p99 ≤ 33 ms; BENCH-3/5 ≥ 30 fps, p99 ≤ 50 ms; every pass within its §8.1.2 column + 10 %; MIN BENCH-3 volley-peak GPU p99 ≤ 28 ms and ≤ 120 hulls in the mesh band; every §1.3 MIN line within §8.1.4's check in BENCH-1 and BENCH-3 (shadows ≤ 0.25 GB with 2 × 2048² D16 capital maps). **REF Performance mode:** BENCH-4 ≥ 120 fps, p99 ≤ 12.5 ms, every pass within the §8.1.1 Performance column + 10 %, GPU wall ≤ 7.2 ms. **Image stability (AAA-REN-8 L7 on MIN and in Performance mode):** the MIN BENCH-1/2/4 captures (FSR 2 Quality) and the REF BENCH-4 Performance capture (FSR 2 Balanced) pass §8.4a's temporal-stability check on their skinned characters, at the gate's own frame rate; the walk-away golden's FSR 2 runs at 1.5× and 1.7× (§7.6a) are green on the gate build. **CPU side:** 02 RT-12's MIN clause (Ph3), 30/45 fps clause (Ph3) and 120 fps clause (Ph4 midpoint), so every CPU plan is measured before this gate | AAA-REN-2/3 | 4 |
| RC-10 | VRAM ≤ 10 GB (REF) / 5 GB (MIN) in every BENCH scene; Linux within 10 % of Windows. **BENCH-1 on MIN runs with 200 unique appearances** (no two avatars share a §7.6a composite or rest-mesh key): composite cache ≤ 96 MB, rest-mesh cache ≤ 48 MB, skinned-output ring ≤ 56 MB, texture pool ≤ 1.8 GB, geometry pool ≤ 0.8 GB; every A0–A2 character shows its own composite and baked shape ≤ 1 s after arrival; composite GPU time ≤ 0.3 ms per frame p99; the Characters row within §8.1.2 + 10 %. The same run on REF High keeps the composite cache ≤ 320 MB, the ring ≤ 112 MB and the rest-mesh cache ≤ 96 MB, with skinning ≤ 0.6 ms. **Image stability:** the run's crowd capture through FSR 2 on MIN (Quality, 720p → 1080p) and on REF at Performance mode's render scale (Balanced, 1506 × 847 → 1440p) passes §8.4a's temporal-stability check on the A0–A2 masks (MVC ≥ 99.5 %; limb-mask ꟻLIP p95 ≤ 1.15 × the full-rate control) | AAA-CNT-3, PLT-5 | 4 |
| RC-11 | Phase feature sets complete, with real-GPU goldens nightly: Ph1 forward+ PBR and CSM, starfield and nebula IBL, HDR/bloom, GPU particles, **CDLOD cube-sphere planets with GPU tiles, Hillaire atmosphere from orbit to ground**, 2D clouds, **far terrain shadows (§5.4a: Harrow sunset goldens at 5 km and 50 km altitude, orbit terminator, no seam across `csmFar`; horizon generation inside the Terrain row's unused share)**; Ph3 Earth-size planets with oceans, froxel fog, **DDGI with §4.6a AS management (REF) and relit assembly-time probes (MIN)**, **vegetation (§5.6a: LOD and impostor bands, wind, interaction)** and **weather effects (§5.8a: precipitation, wetness and snow, lightning)**, each within its §8.1 row or sub-budget in BENCH-2 and BENCH-5; Ph4 visibility buffer, VSM, volumetric clouds **including fast mode** (a golden and a budget measurement of BENCH-2's in-layer segment at 1,500 m/s, §5.8), TAA + `IUpscaler`, HDR10 PQ pattern within 2 %, **FACS faces (§7.6b: 52-channel lineup, viseme sweep and DNA-blend goldens; 8 F0 faces ≤ 0.15 ms on REF and 4 ≤ 0.1 ms on MIN at BENCH-1's dialogue bookmark)**; Ph5 RT | AAA-REN-6; R04 | 1–5 |
| RC-12 | **Streaming quality** (§1.3a), nightly on REF and MIN, Windows and Linux. In BENCH-2 (1,500 m/s descent) and BENCH-6 (300 m/s under thrust): ≥ 99 % of feedback-sampled texels (coverage-weighted) are within 1 mip of their target, excluding requests younger than 500 ms; no request stays > 1 mip short for > 2 s; no draw is > 1 LOD coarser than desired for > 500 ms; zero missing geometry; the texture pool never exceeds its §1.3/§8.2 budget; no frame > 50 ms and no frame > 33 ms attributable to uploads or residency CPU; residency CPU ≤ 0.5 ms p99. **Ph1:** BENCH-2 from local paks. **Ph2:** also with BENCH-2's zone group absent at start (play while downloading, 100 Mbit/s test link): targets clamp to installed data, so lower mips and coarse LODs are acceptable while it downloads; holes and hitches are not. **Ph3:** BENCH-6 | AAA-REN-5, CNT-2, CNT-3 | 1–3 |
| RC-13 | **Terrain-generation throughput** (§5.5a) on MIN and REF inside BENCH-2: the adopted variant (green, F2 or F3) delivers its capacity within the 0.8 ms tile budget; tile demand averaged over any 1 s window without a camera cut stays ≤ 25 % of capacity; zero tile holes; the CPU VM is ≤ 0.5 ms per 65² tile per core on the MIN CPU (or F3 is adopted) | AAA-REN-1 (Ph1 slice), CNT-1 | 1 |

### 9.4 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Lavapipe is slow and differs from hardware | Small resolutions; cap-masked fallbacks; hardware goldens nightly; pinned Mesa image |
| Windows Vulkan driver bugs | `Caps` workaround table, launcher minimum-driver check, three-vendor lab, D3D12 gate |
| Shader stutter | §1.7 policy (vertex pulling, closed models, recorded lists, async + fallback PSO, telemetry); RC-7 |
| Scope (planets + atmosphere + clouds + VFX + vis buffer) | The ladder; Phase 4 bar is exactly AAA-REN-6 with no Nanite/Lumen parity (01 §5.3); 2D clouds and baked nebulae first |
| Slang churn | Pin + hash, weekly source build (R10 §14), GLSL-compatible core, costed plan B |
| GPU/CPU terrain mismatch | Single-source fixed-point `hnoise`, RC-4, CPU-upload fallback |
| `hnoise` too slow on MIN (integer throughput, 40-node graphs); CPU VM over 0.5 ms (09 K5b) | 32-bit-only twin, level-adaptive octaves, WP-0.9c spike with pre-decided F2/F3 fallbacks (§5.5a), RC-13 nightly |
| Texture/mesh pop-in or upload hitches at 1,500 m/s | Tails always resident, GPU feedback + CPU trajectory prediction, per-frame upload caps, descriptor-stable fade-in, streaming goldens, RC-12 |
| AS memory or build cost grows with the 1M-slot scene | Range-limited TLAS (16k cap), lazy compacted BLAS pool with LRU, skinned meshes excluded until Ph5, 0.5 ms async cap (§4.6a) |
| Foliage overdraw and alpha-test cost | Card-coverage lint, coverage-preserving mips, prepass-only alpha test with `EQUAL` forward, impostor bands, §8.1 sub-budgets |
| Per-character composites overrun VRAM at 200+ unique avatars (≈ 4 GB naive) | Tiered resolutions by animation-LOD tier, GPU BC7/BC1/BC5 encode, capped LRU cache inside the texture pool, crowd-class tinted impostors at A3, tint fallback while compositing (§7.6a); RC-10 BENCH-1 on MIN with 200 unique appearances |
| Character geometry (unique shapes, skinning, faces) overruns VRAM or GPU time at 260 characters | Morph and DNA baked once per appearance and LOD into a capped rest-mesh cache; `BoneScale` in the bind skeleton; tiered skinned-output ring with previous positions on every skinned tier (§7.3's motion-vector contract); staggered 30/15 Hz reskins; per-tier vertex maxima linted by T22; FACS shapes only for ≤ 8 F0 faces; base-mesh fallback while baking (§7.6a–b); RC-10, RC-11 Ph4 |
| Reduced-rate crowd reskins (A1 30 Hz, A2 15 Hz) ghost under FSR 2 at MIN's 1.5× and Performance mode's 1.7×, failing AAA-REN-8 L7 | Previous-position slot on every skinned tier with exact motion on reskin, hold and slot-acquire frames (§7.6a); masks kept for transparents only (§7.3); §8.4a's temporal-stability check per commit on lavapipe through FSR 2 (walk-away golden, with a negative control) and nightly on hardware (RC-9, RC-10) |
| MIN BENCH-3 (2,000 ships on a GTX 1660 SUPER) misses 30 fps or 5 GB | Own §8.1.2 column and §8.1.4 VRAM check; the mesh band starts at 64 px (≤ 120 hulls); 2 × 2048² D16 capital maps; 256k significance pass with half-res smoke; ~15 ms of planned GPU headroom; 02's 30 fps CPU columns gated at the Ph3 exit on the WP-2.15 capture (RT-12); RC-9 |
| Performance mode cannot fit 8.33 ms on the CPU | 02 §2.4's 120 fps column: game thread ≤ 6 ms (tick/Phase A interleave, 60 Hz ticks on alternate frames, performance governor), render thread ≤ 3.5 ms; RT-12's 120 fps clause gates at the Ph4 midpoint, ahead of RC-9 and CL-6's 120 fps gate |
| Cloud reprojection breaks at 1,500 m/s inside the layer | Fast mode: 1/8 resolution per axis, 20 steps, one multi-scatter octave, same ray count as normal mode; 2D shell above cloud top + 2 km (§5.8); RC-8 in-layer segment |
| Planet terrain unshadowed beyond CSM at low sun | Per-tile horizon maps from the tile producer, `min` blend with CSM, orbit path included, generation only from the tile slice's unused share (§5.4a); RC-11 Ph1 sunset goldens |
| Precision bugs at Reparent / anchor snaps | 10¹³ m and BENCH-6 goldens, frame-change events |
| Async regressions; 6 GB MIN oversubscription | Per-vendor toggle; §1.3 budgets with pool shrink |
| Proprietary SDK licences | Optional plugins; FSR 2 (MIT) default |

### 9.5 Traceability

| Requirement | Section |
|---|---|
| R09-B0, R09-RD-P0-1, R06-ENG-08/09, R04-P0-10, R01-P1-18, R10 §3 | §1, §2 |
| AAA-CNT-2/3 (residency, VRAM), BENCH-2/6 streaming | §1.3a, RC-12 |
| R09-A18, G15, R05 (customization and crowds at BENCH-1/5 scale), AAA-CNT-3 | §7.6, §7.6a, RC-10 |
| R04 (facial animation, lip-sync), AAA-REN-8 L5 | §7.6b (GPU), 02 §7.2 (runtime), RC-11 Ph4, 02 RT-22 |
| AAA-REN-1/2/3 at every gated scene, tier and frame rate | §8.1.1–8.1.6, RC-8, RC-9; 02 §2.4, RT-12 |
| W04/W05 far-terrain shadows, R09-B4 clouds at descent speed | §5.4a, §5.8 fast mode, RC-8, RC-11 |
| R09-B3/B4 (planet surface detail), T05, T18 per-biome weather | §5.5a, §5.6a, §5.8a, RC-13 |
| R09-B14 (GI) | §4.6, §4.6a |
| R09-B1, R09-RD-P0-2, R04-P0-2, R06-ENG-07, R05-P0-2 | §2.5–2.7 |
| R06-ENG-10/22/29, R09-B15, R09-RD-P1 (meshlets, impostors) | §3 |
| R09-B7/B11/B14, R09-RD-P0-4/5, R06-ENG-26/30 | §4 |
| R09-B2–B5, B12, R09-RD-P0-9/10, R04-P1-12, W04, W05 | §5 |
| R09-B8–B10, R09-RD-P0-6/7, R06-ENG-34, R08-T17 | §6 |
| R09-B6/B13/B16, R09-RD-P0-3/8, R04-P1-18, R01-P2-20 | §7 |
| ADR-012, AAA-REN-1…7, AAA-CNT-3, AAA-ITR-1 | §8, §9.3 |

### 9.6 Cross-section issues

- **02 Engine runtime:**
  - **Resolved:** 02 §5.2 now specifies two-level transforms: dirty grid-local transforms plus
    per-view `frameToView[F]`, composed on the GPU (§2.6), so a camera move never re-uploads every
    visible object.
  - 03 generates *all* visual terrain LODs on the GPU, not only the far field, so `det` noise and
    domain primitives must be integer/fixed-point with a Slang twin and a conformance corpus (§5.5).
  - A frame-change event on `Reparent()`.
  - Buoyancy on a Gerstner subset, not the FFT ocean.
  - The portal graph and crowd animation-LOD contracts.
  - **Round 1, edited in 02 §5.8:** the Slang twin is 32-bit-only, and generators take the tile level for
    level-adaptive octaves; the 0.5 ms/tile CPU figure is validated by the WP-0.9c spike.
  - Asks: `RenderScene` carries the `StreamingSource` list and a `cut` hint (§1.3a); 02's cloth and
    secondary-motion jobs evaluate the analytic wind function (§5.6a); `.htex` mip ranges and mesh LOD
    ranges fit 02 §6.3's blocks.
  - **Round 2 (no 02 edit needed):** §7.6a consumes 02 §7.2's A0–A3 tiers, the A0/A1 governor caps and A3's
    `{impostorPose, heading, phase}` as written; `impostorPose` indexes the 4 baked crowd poses (idle, walk,
    run, sit).
  - **Round 3, edited in 02:**
    - §2.4 gains CPU tables for REF BENCH-3 at 45 fps, MIN BENCH-3 and 5 at 30 fps, and REF BENCH-4 in
      Performance mode at 120 fps. They come with performance `cpu.*` settings and the tick/Phase A
      interleave.
    - RT-12 gains a 30/45 fps clause (Ph3) and a 120 fps clause (Ph4 midpoint).
    - §7.2 gains the facial runtime (FACS curves, viseme layer, emotion, eyes, face LOD F0–F2, 52 weights
      per F0 face in the extract packet) and RT-22.
    - `BoneScale` is folded into the appearance's bind skeleton, so §7.6a bakes only `Morph` and `DnaBlend`.
  - **Round 4, edited in 02 §7.2:** §7.6a's ring flips its position slots on `AnimLod.poseSerial` changes
    exactly as 02 §7.2 already emits them, and 03, not 02, keeps the last-displayed palette. 02 §7.2 gains
    the rendertest-only cvars `anim.forceTier`, `anim.forcePhase` and `anim.lodRate=full` that §8.4a's check
    and the walk-away golden use.
- **06 Gameplay:** `CueDef` payloads for shield impacts, decals and emitters; g-load and warp-state
  signals. *Ask (round 1):* own the replicated per-body `PlanetWeather` component and its front spawner
  (from T18's per-biome tables), the server-side gameplay effects of weather, and the wind-emitter cues
  (§5.8a, §5.6a). **Resolved:** 06 §9.9 (a stateless `PlanetWeather` per wall-clock epoch, the front
  spawner, `WeatherEffectDef`, overrides through world flags, GP-13) and 06 §1.5 (`windEmitter` cues).
- **07 Editor:**
  - T16 shows permutation and PSO budgets;
  - T17 compiles to bounded uber-kernels;
  - T04/T05 nodes are limited to the deterministic set; T04's validator enforces the F3 detail-octave
    amplitude bound if F3 is adopted (§5.5a);
  - T05 authors `FoliageDef` (LODs, impostor bake, wind, interaction) and T28 lints card coverage and
    texel density (`uvDensity`, §1.3a);
  - T18 owns atmosphere, post, nebula profiles and per-biome weather tables;
  - T26 hosts the graph visualizer and the streaming heatmap;
  - **Round 2:** T22's crowd-bake settings assign each outfit `MeshOption` a silhouette class (≤ 8 per body
    type) and map `PaletteColor` parameters to the ≤ 6 impostor tint slots (skin, hair, 4 outfit); the stress
    grid also runs through the runtime BC encoders (§7.6a). Edited in 07 T22.
- **08 Client:** RmlUi render interface, HDR settings, PSO warm-up and benchmark UI, bracket content;
  the residency manager uses `StreamingInstaller::demand` with priority 1 (visible) and 2 (lookahead).
- **09 Roadmap:** the REF/MIN/SHOWCASE Windows + Linux lab and a pinned Mesa CI image. **Edited in
  round 1:** WP-0.9c spike, K5b, RC-12 in WP-1.7/2.1/3.5, RC-13 in WP-1.3/1.8, and the phase exits.
  **Edited in round 2:** WP-1.8 adds horizon-map far terrain shadows; WP-2.3 adds character composite
  tiers, the cache and the GPU BC encoders; WP-3.5 adds cloud fast mode and generic crowd impostors (see
  CONSISTENCY.md). **Edited in round 3:**
  - WP-2.3 adds the rest-mesh bakes and the skinned-output ring (§7.6a).
  - WP-3.4 carries RT-12's 30/45 fps clause and WP-4.1 its 120 fps clause.
  - WP-4.6 adds the FACS runtime (02 §7.2, §7.6b) with RT-22, which also joins the Phase 4 exit list.
- **01 Scope:** **edited in round 1:** AAA-REN-6's Ph3 set adds DDGI/probes, vegetation and weather
  effects (mirrors RC-11).
