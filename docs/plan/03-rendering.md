# 03 — Rendering

> **Status:** draft v1. **Conforms to:** ADR-001, -003, -005, -006, -011, -012, -013.
> **Capabilities owned** (01 §2.6): R01, R02, R03, R07; the render half of W04, W05, R04, R05; the
> render-to-texture host for R06 (08 owns the UI).
> **Citations:** `R09-B5` = research 09 Part B §B5; `R09-RD-P0-3` = R09 renderer requirement P0 #3;
> `R06-ENG-09` = R06 requirement table; `R04-P0-10`, `R05-P0-2`, `R01-P1-15` = numbered requirements
> (01 §7). **Budgets are GPU ms on REF at 1440p High** unless marked MIN (1080p Low, upscaled).

## 0. Principles

1. **Budgets are features.** Every pass has REF and MIN budgets (§8.1); the nightly lab fails any
   regression > 5 %.
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
| **Required** | Vulkan 1.3: dynamic rendering, synchronization2, timeline semaphores, maintenance4, BDA, descriptor indexing (runtime arrays, partially bound, update-after-bind, non-uniform), draw-indirect-count, MDI, subgroup ballot/arithmetic, `samplerFilterMinmax`, BC formats | All tiers, lavapipe |
| Optional | `VK_EXT_mesh_shader` | Meshlet path (MIN/RDNA1 lacks it) |
| | `VK_KHR_acceleration_structure` + `ray_query` | DDGI (Ph3), RT effects (Ph5) |
| | `VK_EXT_descriptor_buffer`, `VK_EXT_graphics_pipeline_library`, `VK_KHR_pipeline_binary` | Heap updates, fast PSO link, binary cache |
| | int64 buffer/image atomics | Visibility-buffer variants |
| | `VK_EXT_memory_budget`, `_memory_priority`, `_pageable_device_local_memory` | Budgets, residency |
| | `VK_EXT_swapchain_colorspace`, `_hdr_metadata`, `_full_screen_exclusive` | HDR10/scRGB |
| | `VK_KHR_present_id/present_wait`, `VK_EXT_calibrated_timestamps` | Pacing, Tracy GPU zones |
| | `VK_EXT_device_fault`, NV checkpoints, `VK_AMD_buffer_marker` | Crash diagnostics |
| | `VK_KHR_fragment_shading_rate` | VRS for clouds/particles (Ph4) |

Vulkan 1.4 is used opportunistically through the same caps bits (R09 overview). `Caps` carries a
**driver-workaround table** (vendor × driver version) and a cap mask (`HELIOS_RHI_CAPS_MASK`) that
disables optional paths, so CI exercises every fallback. *Verified in this container:* lavapipe
(Mesa 25.2.8) reports Vulkan 1.4.318 with mesh shaders, ray query, acceleration structures,
descriptor buffer, GPL, int64 image atomics and 1M update-after-bind images, but no `present_wait`,
`device_fault` or `pipeline_binary`.

### 1.3 Memory (VMA 3.4) and VRAM budgets

Pools: **persistent** (GPU scene, geometry, textures; VMA custom pools, defragmented off-frame),
**transient** (graph aliasing heap), **upload ring** 256 MB (ReBAR, else transfer-queue staging),
**readback** 16 MB. Geometry is one suballocated pool read by vertex pulling. Allocations carry
ADR-011 tags; streaming pools shrink on `memory_budget` pressure before the OS evicts.

| VRAM category (AAA-CNT-3) | REF | MIN |
|---|---|---|
| Render targets + transient heap | 1.2 GB | 0.5 GB |
| Texture streaming pool | 4.0 | 1.8 |
| Geometry pool | 1.5 | 0.8 |
| GPU scene, materials, lights | 0.3 | 0.15 |
| Terrain tiles + virtual texture | 0.6 | 0.3 |
| Shadows | 0.5 | 0.25 |
| Volumetrics, particles | 0.65 | 0.25 |
| Upload/readback, UI/RTT atlases | 0.5 | 0.25 |
| RT acceleration structures | 0.5 | — |
| Reserve | 0.25 | 0.7 |
| **Total** | **10.0 GB** | **5.0 GB** |

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
| Shadow maps | Light/decal binning, GTAO, sky-view + AP LUTs, particle sim/sort |
| Prepass → forward opaque | Terrain tile producer, cloud raymarch, exposure histogram |
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
- **Access rules:** feature renderers read only the packet and immutable render data. Packet memory
  comes from tagged frame heaps freed on the frame's timeline value (R06 §6.4); there are no locks.
  The render thread needs ≤ 4 ms wall on REF, well inside the 16.6 ms frame (02 §2.4).
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
  96-byte record instead of every instance aboard; a 1,000-ship battle uploads ~100 KB.
- **Scatter:** `(slot, record)` pairs go through the upload ring into a compute scatter: 100k
  updates ≤ 0.2 ms.
- **Capacity:** 1M slots (REF) / 512k (MIN), with generational `RenderInstanceId`s. Procedural
  instances (scatter, asteroids, ring debris) are generated per frame into transient streams.
- Previous transforms for motion vectors are copied only on change.

### 2.7 Precision: camera-relative, reverse-Z, infinite far plane

- **Projection:** infinite reverse-Z,
  `P = [[f/a,0,0,0],[0,−f,0,0],[0,0,0,n],[0,0,−1,0]]` (`z_ndc = n/−z_view`), `D32_SFLOAT`, clear 0,
  `GREATER_OR_EQUAL`. Relative depth resolution ≈ 2⁻²³ everywhere, about 0.12 m at 1,000 km (R09-B1).
- **Near planes:** the world uses 0.1 m in depth range [0, 0.95]. The cockpit and first-person layer
  has its own FOV and a 1 cm near plane in [0.95, 1] (R09-B16).
- **Camera-relative:** frame chains (interior → grid → body → system → sector) compose in f64; only
  `frameOrigin − cameraPos` becomes f32. Error is one f32 ulp relative to distance, always sub-pixel.
- **Anchors:** GPU-persistent world data (particle space, froxel/cloud history, VSM pages, tile
  caches) lives in its own frame, or relative to a per-zone **render anchor** that snaps after 4 km
  of camera travel. Histories rebase by the exact f64 delta.
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
  ~12 shading models a view has < 60 buckets.
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
  content, 03 the instanced sprite pass).
- Hulls are instanced; liveries and damage are per-instance data (R01-P1-15).
- BENCH-3 (1,000 ships, 4 capitals): geometry ≤ 3.0 ms.

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
- **Closed set of ~12 shading models:** Standard, ClearCoat, Cloth, Skin, Hair, Eye, Unlit, Hologram,
  Shield, Glass, Terrain, Water. A new one needs render-lead review, because the set bounds PSOs.
- **Textures:** KTX2 (BC7, BC5, BC6H, BC4), streamed by screen-space texel density.

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
  - 4 cascades (2048² REF / 1024² MIN), PSSM λ 0.8, texel-snapped, 5 × 5 PCF with receiver-plane bias.
  - **In space**, cascades fit the player's grid plus nearby grids rather than the frustum.
  - **Capital ships** within 20 km get a dedicated 4096² map (≤ 4).
- **Eclipses:** analytic sphere occlusion (moons on planets, planets on rings).
- **Local lights:** an 8192² (REF) / 4096² (MIN) atlas with ≤ 32 tile updates per frame. **Static
  casters are cached in grid space**, so interior shadows stay valid while the ship flies.
- **Contact shadows:** a 16-step screen-space march for the sun.
- **Ph4 virtual shadow maps** (R06-ENG-26): 16k² virtual clipmap levels in 128² pages; physical pool
  8192² (REF) / 4096² (MIN). Pages are marked from depth and cached with GPU-scene invalidation;
  moving ships use dynamic pages, while terrain and stations stay cached.
- **Ph5:** ray-query shadows on SHOWCASE.

### 4.6 IBL, probes, GI (R09-B14)

- **Space IBL:** the region's nebula cubemap (§5.2), GGX-prefiltered plus L2 SH: nebula-tinted rim
  light against a hard sun, the EVE look (R09-B5). In atmosphere, a 128² sky cube from the sky-view
  LUT, one face per frame (≤ 0.15 ms).
- **Reflection probes** are box-projected and **parented to grids**; from Ph3 they are
  **relightable** (stored albedo/normal/depth cubes relit each frame).
- **GI:** Ph1 IBL + GTAO + probes → Ph3 grid-parented DDGI (Majercik et al. 2019) via ray queries on
  REF, raster-captured SH probes at assembly/cook time on MIN → Ph5 ReSTIR on SHOWCASE. No
  lightmaps: modular ships invalidate them.

### 4.7 Fog, GTAO, SSR

- **Froxel fog** (Wronski 2014; Hillaire 2015):
  - 160 × 90 × 64 at 1440p, exponential to 2 km.
  - Injects height fog, volumes, combat dust and nebula edges.
  - Lit by CSM and clustered lights, with blue-noise jitter and 0.9 temporal history.
  - ≤ 0.7 ms.
- **GTAO** (Jimenez et al. 2016): half resolution, denoised, bent normals. ≤ 0.5 ms.
- **SSR:** Hi-Z trace (Stachowiak 2015) on reprojected last-frame HDR at half resolution, with probe
  fallback. ≤ 0.6 ms.
- The prepass writes a **thin G-buffer** (normal/roughness, motion) for GTAO, SSR, contact shadows and
  upscaling.

### 4.8 Pass order (forward path)

Scatter → cull 1 + prepass → HZB → cull 2 + prepass → [async: binning, GTAO, sky/AP LUTs, tiles,
particle sim] → shadows → froxel fog → forward opaque → sky + far layer → clouds/nebula → SSR →
transparents (sorted, WBOIT, particles, shields, distortion) → TAA/upscale → star layer → exposure,
bloom, flares, motion blur, DoF, tonemap + grade → UI/HUD → present.

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
  - Throughput ≤ 64 tiles per frame, prioritized by screen error, in 0.8 ms.
  - Missing tiles fall back to the upsampled parent, so there are never holes or waits.
- **Runtime stamps** (housing flattening, craters; 06 §9) are replicated lists applied as the last
  affector; overlapping tiles regenerate.

### 5.5 Deterministic heights shared with the server

Collision on client and server uses 02's deterministic `pcg` bytecode VM for the T04 graph
(R08-ED-P0-07), bit-identical everywhere (AAA-CNT-1, AAA-PLT-4). The client also builds near-field
tiles on the CPU for collision. 03 generates **every visual LOD on the GPU** from the same graph
compiled to Slang, because a 1,500 m/s descent outruns CPU generation. Vulkan float math is not
bit-exact across vendors (2.5 ULP division, implementation-defined transcendentals and denormals).
Therefore:

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

### 5.6 Texturing, biomes, scatter

- **Biomes:** climate maps (temperature, humidity, geology; Genesis model, R04 §5) come from the
  same graph. Each planet has ≤ 16 tileable BC7/BC5 layers, ≤ 4 per tile, height-blended, with
  triplanar mapping only above 35° slope.
- **Orbit views** use a 64² baked albedo per tile, so they match the ground.
- **Ph3 procedural virtual texture** (Chen 2015, Far Cry 4): layers, decals and roads composited into
  128² pages on feedback demand.
- **Scatter** (T05 rules, deterministic seeds): visual-only instances are generated per tile on the
  GPU (≤ 500k visible on REF). Gameplay scatter (collidable rocks, resource nodes) comes from the CPU
  library as entities (02/06).

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
  - *Density:* a spherical shell. A rotating weather cubemap (coverage, type, precipitation, driven by
    server weather parameters) × a per-type height gradient × 128³ Perlin-Worley, eroded by 32³ Worley
    and curl noise.
  - *Lighting:* Beer × powder, dual-lobe HG, multi-scatter octaves (Wrenninge 2013), a 6-step sun cone,
    sky-view ambient.
  - *Temporal:* quarter resolution with 4 × 4 checkerboard updates (1/16 per frame) and reprojection.
    Above 500 m/s every quarter-res pixel updates.
  - *Consistency:* orbit shows the same weather map as a 2D shell, blended by altitude.
  - *Cloud shadows:* a top-down density map.
  - Budget 1.5 ms.
- **Ph4:** golden (AAA-REN-6). **Ph5:** voxel/SDF Nubis³ fly-through.

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
| > 100 km | CDLOD root levels + tile albedo, per-pixel atmosphere, 2D cloud shell |
| 100–10 km | Volumetric clouds and AP froxels fade in |
| < 10 km | CSM on terrain, scatter, virtual texture, froxel fog |
| Interior | 02's portal graph (W08) culls interiors and drives sky visibility, probe blend and exposure zones |

The same tiles, atmosphere and weather map are used throughout, so there is no representation swap.
Tiles prefetch along the trajectory with zero residency misses at 1,500 m/s (AAA-CNT-2) and no
frame > 50 ms.

### 5.12 Ship interiors

Clustered lights with grid-space shadow caching, relit grid probes, DDGI (REF) or assembly-time
probes (MIN); `Ship.Power.*` drives lights and emissives; windows and airlocks are portals.

---

## 6. Sci-fi VFX

### 6.1 GPU particles (R09-B10, R06-ENG-34)

- **Pools:** SoA, 1M particles on REF (BENCH-3: 200k active), 256k on MIN. Positions live in the
  **emitter's frame**, so thruster trails stay correct on moving ships.
- **Per frame, mostly async compute:**
  1. Emit from emitter tables and cue events, with deterministic seeds.
  2. Simulate: gravity, drag, curl noise, attractors, and **depth-buffer collision** against
     reprojected depth.
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

- **Skinning:** compute skinning and sparse blend shapes into per-frame buffers, from ozz joint
  matrices (02).
- **Skin:** pre-integrated (Penner & Borshukov 2011) on Low/Med; Burley screen-space SSS (Golubev
  2018) on High+.
- **Hair:** cards with dithered alpha, shaded with Karis 2016.
- **Eyes:** refractive cornea/iris parallax.
- **Customization:** layers composited into a 2048² per-character atlas (R09-A18).
- **Crowds** (BENCH-1): skinning at reduced rate beyond 30 m, impostors beyond 80 m. Animation LOD is
  owned by 02.

---

## 8. Performance and quality infrastructure

### 8.1 Frame budgets (GPU ms p50, 16.6 ms frame)

| Pass | REF BENCH-2 | REF BENCH-3 | MIN BENCH-2 (720p → 1080p) |
|---|---|---|---|
| Scatter + 2-phase cull + HZB | 0.6 | 1.0 | 0.6 |
| Depth/normal/motion prepass | 1.2 | 1.5 | 1.2 |
| Shadows | 1.8 | 2.0 | 1.6 |
| Binning + GTAO (async) | 0.7 | 0.7 | 0.5 |
| Atmosphere + sky | 0.3 | 0.1 | 0.3 |
| Terrain tiles (async) | 0.8 | — | 0.8 |
| Forward opaque | 3.0 | 3.5 | 3.0 |
| Froxel fog | 0.7 | 0.5 | 0.5 |
| Clouds / nebula | 1.5 | 1.5 | 0.4 (2D shell) |
| SSR | 0.6 | 0.4 | — |
| Transparents + particles | 1.2 | 2.5 | 1.0 |
| TAA / upscale | 0.6 | 0.6 | 0.9 (FSR 2) |
| Post | 0.8 | 0.8 | 0.7 |
| UI, HUD, diegetic | 0.4 | 0.6 | 0.4 |
| **Sum / wall with async overlap** | **14.2 / ~13.2** | **15.7 / ~14.5** | **11.9 / ~11.3** |

MIN figures are measured on MIN hardware. BENCH-3 needs only ≥ 45 fps (22 ms): headroom for peaks.

**CPU budgets (REF):** extract ≤ 1.0 ms; prepare ≤ 1.5 ms wall; graph compile ≤ 0.3 ms; recording
≤ 2.0 ms across jobs; submit ≤ 0.3 ms.

**Performance mode** (AAA-REN-3, BENCH-4 ≥ 120 fps): High minus volumetric clouds and SSR, with FSR 2
Balanced, for an 8.3 ms frame.

### 8.2 Scalability

| Setting | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Render scale | 0.5–0.67 + FSR 2 | 0.67 | 1.0 TAA | 1.0 |
| Sun shadows | 3 × 1024² | 4 × 1536² | 4 × 2048² | VSM (Ph4) |
| Clouds | 2D shell | 24 steps | 48 steps | + fly-through detail |
| Froxels | 80 × 45 × 32 | 160 × 90 × 64 | same | 240 × 135 × 96 |
| GTAO / SSR | ¼ / off | ½ / off | ½ / ½ | full / ½ |
| Particles | 64k | 128k | 512k | 1M |
| Terrain error / scatter | 4 px / 25 % | 2 px / 50 % | 1 px / 100 % | 0.75 px / 150 % |
| Texture pool | 1.5 GB | 2.5 GB | 4 GB | 6 GB |

On first launch a 10 s GPU benchmark picks the preset.

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
- Null trace goldens run on every toolchain, including GPU-less MSVC.

---

## 9. Layout, ladder, acceptance, risks, traceability

### 9.1 Module and directory layout

```
engine/rhi/            include/helios/rhi/{device,command_list,handles,caps}.h
  src/vulkan/ (volk, VMA)  src/null/  src/d3d12/ (Ph3 seam test, Windows only)
engine/render/         L3, non-HEADLESS: graph/ pipeline/ scene/ culling/ upscalers/ debug/
  features/{mesh,terrain,atmosphere,clouds,stars,nebula,ocean,particles,shields,decals,
            shadows,probes,fog,post,characters,ui_bridge}
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
| Materials | PBR | **Clustered forward+ PBR**, clear coat, T16 instances | Layers/liveries, clustered decals, graph → Slang | Aniso, sheen | Skin, hair, eyes | — |
| Lighting | — | **CSM**, clustered lights, nebula IBL, GTAO | Shadow atlas, grid probes, SSR | Froxel fog, relit probes, DDGI | VSM | RT shadows, ReSTIR |
| Space | — | **Starfield, nebula skybox**, sun, flare | Galactic band | Nebula fly-through v1, rings, gas giants | Polish | Full volumetric nebulae |
| Planets | — | **CDLOD + GPU tiles, Hillaire atmosphere orbit → ground**, 2D clouds | Biomes, scatter, stamps | Volumetric clouds, oceans, PVT, Earth-size | Clouds golden, weather | Nubis³ |
| VFX | — | GPU particles, shields, plumes, beams | T17 module stacks, ribbons, mesh particles, flipbooks, holograms, WBOIT | Simulation stages, warp tunnel | 200k particles | Volumetric explosions |
| Post | Tonemap | **HDR, histogram exposure, bloom, AgX/ACES + LUT**, TAA | `IUpscaler` + FSR 2, motion blur | DoF, exposure volumes | HDR10, DLSS/XeSS, 120 fps mode | Frame gen, local TM |
| UI | **ImGui** | RmlUi, diegetic RTT, HUD, cockpit layer | — | Brackets for 1,000+ ships | — | — |
| QA | Lavapipe goldens, Null traces | Tracy GPU, budget overlay, BENCH-2 nightly | Hot reload ≤ 2 s | 30-min PSO run | Full BENCH matrix | SHOWCASE RT |

### 9.3 Acceptance criteria

| ID | Criterion | Scorecard | Ph |
|---|---|---|---|
| RC-1 | Every shipped feature has a lavapipe golden (ꟻLIP mean ≤ 0.01), rendered twice bit-identically; suite ≤ 10 min; Null traces on all toolchains | AAA-REN-7 | 0 |
| RC-2 | 10¹³ m test jitter < 0.05 px over 600 frames; BENCH-2 has no loading screen and no frame > 50 ms | AAA-REN-5 | 1 |
| RC-3 | BENCH-2 ≥ 60 fps at 1080p Medium on REF, Windows and Linux | AAA-REN-1 | 1 |
| RC-4 | GPU terrain matches the CPU `pcg` VM on 10⁶ samples (target bit-identical, limit ≤ 1 cm) on NVIDIA, AMD, Intel, lavapipe | AAA-CNT-1, PLT-4 | 1 |
| RC-5 | Culling 1M/200k ≤ 0.5 ms; render CPU ≤ 4 ms wall in BENCH-3 | AAA-REN-1 | 2 |
| RC-6 | Shader edit → visible ≤ 2 s p95; injected GPU hang names the hung pass | AAA-ITR-1, STB-1 | 2 |
| RC-7 | 30-min run: zero render-thread PSO creations, zero > 50 ms frames from PSOs, < 1 fallback use per hour warm | AAA-REN-4 | 3 |
| RC-8 | REF 1440p High: BENCH-1/2/4/5 ≥ 60 fps, p99 ≤ 20 ms; BENCH-3 ≥ 45 fps; passes within §8.1 +10 % | AAA-REN-1 | 4 |
| RC-9 | MIN: BENCH-1/2/4 ≥ 60 fps, p99 ≤ 33 ms; BENCH-3/5 ≥ 30 fps; REF performance mode BENCH-4 ≥ 120 fps | AAA-REN-2/3 | 4 |
| RC-10 | VRAM ≤ 10 GB (REF) / 5 GB (MIN) in every BENCH scene; Linux within 10 % of Windows | AAA-CNT-3, PLT-5 | 4 |
| RC-11 | Phase feature goldens: Ph1 set; Ph3 planets, froxel fog; Ph4 visibility buffer, VSM, clouds, TAA + `IUpscaler`, HDR10 PQ pattern within 2 %; Ph5 RT | AAA-REN-6 | 1–5 |

### 9.4 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Lavapipe is slow and differs from hardware | Small resolutions; cap-masked fallbacks; hardware goldens nightly; pinned Mesa image |
| Windows Vulkan driver bugs | `Caps` workaround table, launcher minimum-driver check, three-vendor lab, D3D12 gate |
| Shader stutter | §1.7 policy (vertex pulling, closed models, recorded lists, async + fallback PSO, telemetry); RC-7 |
| Scope (planets + atmosphere + clouds + VFX + vis buffer) | The ladder; Phase 4 bar is exactly AAA-REN-6 with no Nanite/Lumen parity (01 §5.3); 2D clouds and baked nebulae first |
| Slang churn | Pin + hash, weekly source build (R10 §14), GLSL-compatible core, costed plan B |
| GPU/CPU terrain mismatch | Single-source fixed-point `hnoise`, RC-4, CPU-upload fallback |
| Precision bugs at Reparent / anchor snaps | 10¹³ m and BENCH-6 goldens, frame-change events |
| Async regressions; 6 GB MIN oversubscription | Per-vendor toggle; §1.3 budgets with pool shrink |
| Proprietary SDK licences | Optional plugins; FSR 2 (MIT) default |

### 9.5 Traceability

| Requirement | Section |
|---|---|
| R09-B0, R09-RD-P0-1, R06-ENG-08/09, R04-P0-10, R01-P1-18, R10 §3 | §1, §2 |
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
- **06 Gameplay:** `CueDef` payloads for shield impacts, decals and emitters; weather parameters;
  g-load and warp-state signals.
- **07 Editor:**
  - T16 shows permutation and PSO budgets;
  - T17 compiles to bounded uber-kernels;
  - T04/T05 nodes are limited to the deterministic set;
  - T18 owns atmosphere, post and nebula profiles;
  - T26 hosts the graph visualizer.
- **08 Client:** RmlUi render interface, HDR settings, PSO warm-up and benchmark UI, bracket content.
- **09 Roadmap:** the REF/MIN/SHOWCASE Windows + Linux lab and a pinned Mesa CI image.
