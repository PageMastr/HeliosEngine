# shaders/ — Helios Slang modules

Shared [Slang](https://shader-slang.org) modules imported by engine, sample and tool shaders
(ADR-003, docs/plan/03-rendering.md §1.7). Pass shaders live next to the code that owns them and
are compiled with `helios_shaders()` (`cmake/HeliosShaders.cmake`); this directory is always on
their import path, so `import core.bindless;` resolves to `core/bindless.slang`.

| Module | Contents |
|---|---|
| `core/bindless.slang` | The global bindless heap (set 0): sampled/storage image, storage buffer and sampler arrays plus accessors. Must match `engine/rhi`'s descriptor layout. |

Planned (03 §9.1): `core/` (GPU-scene structs, `BufferRef<T>`), `brdf/`, `lighting/`,
`atmosphere/`, `materials/`, `passes/`, `vfx/`, `terrain/`.

## Conventions (enforced by the slangc flags in `cmake/HeliosShaders.cmake`)

* **Entry points** are declared in the source with `[shader("vertex")]`, `[shader("fragment")]`,
  `[shader("compute")]` …; one file compiles to one SPIR-V 1.6 module holding all of them, with
  their source names preserved (pipelines select `vsMain`, `psMain`, `csMain` …).
* **Matrices are column-major**, identical to `helios::math::Mat4` in memory; `mul(M, v)` equals
  the C++ `M * v`. Upload matrices unchanged.
* **Scalar block layout** for push constants and buffers: a C++ struct with natural alignment maps
  1:1 (no std140/std430 padding rules). A `float3` occupies 12 bytes.
* **Push constants ≤ 128 bytes**, declared as `[[vk::push_constant]] ConstantBuffer<T>`. They
  carry bindless indices and buffer device addresses (`T*` pointers), never descriptors.
* **Clip space** follows the D3D/GL convention with +Y up; the Vulkan backend flips the viewport,
  so counter-clockwise triangles (seen from the front) are front faces. Depth is reverse-Z.
* Per-invocation-varying resource indices use the `*NonUniform` accessors.
