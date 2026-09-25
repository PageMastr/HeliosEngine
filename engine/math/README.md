# engine/math — Helios math library

Leaf module (C++20 standard library only; no dependency on `engine/core`). CMake target
`helios_math` / `helios::math`, tests `math_tests`. Everything lives in namespace `helios`
(noise in `helios::noise`, deterministic trigonometry in `helios::det`).

| Header (`helios/math/…`) | Contents |
|---|---|
| `scalar.h` | `f32/f64/i32/u32…` aliases, constants (`kPi`, `kPiD`, `kPiT<T>` …), `lerp/clamp/saturate/smoothstep/remap`, `approxEqual`, `safeRsqrt`, `wrapAngle`, exact cast-based `floorToI32`, integer hashes (`hashU32`, `hashU64`, `hashCombineU32`, `deriveSeed`), deterministic `det::sin/cos/atan2/asin/acos` |
| `vec.h` | `Vec2/3/4` (f32), `DVec2/3/4` (f64), `IVec*` (i32), `UVec*` (u32); arithmetic, `dot/cross/length/normalize`, component-wise helpers, swizzles, orthonormal basis |
| `quat.h` | `Quat` / `DQuat`: axis-angle, Euler, basis, `fromTo`, `lookRotation`, `slerp/nlerp`, rotation vectors, `toEuler`, `integrate` |
| `mat.h` | `Mat3/Mat4/DMat3/DMat4`: builders, TRS compose/decompose, inverse (general/affine/rigid), `lookAt`, reverse-Z projections, `flipClipY` |
| `transform.h` | `Transform` (f32) / `DTransform` (f64 position), compose/inverse/relativeTo, camera-relative conversion |
| `frame.h` | `FrameId`, `FramePos`, `FrameTransform`, `KinematicState`: nested moving/rotating reference frames, re-parenting |
| `geometry.h` | `AABB/Sphere/Plane/Ray/OBB/Frustum` (+ f64 `D…` variants), ray casts, overlap tests, closest points |
| `color.h` | linear `Color`, exact sRGB, RGBA8/SRGBA8/RGB10A2/RGB9E5/RGBE/RGBM, kelvin → RGB, HSV |
| `pack.h` | half floats, UNORM/SNORM, octahedral normals, smallest-three quaternions, fixed-point positions |
| `noise.h` | deterministic Perlin, simplex, fBm, ridged, domain warp, cellular (Worley) |
| `spherical.h` | cube ↔ sphere mappings, planet quadtree tiles, lat/long, great circles, surface frames |
| `all.h` | umbrella include (tools/tests) |

Header-only except `src/` (noise, packing, deterministic trig, cube-sphere, colour tables), which
is out of line so its floating-point code generation is pinned in one place. The headers are
immune to `<windows.h>` `min`/`max` macros (they `push_macro`/`#undef`/`pop_macro` them), and
`HELIOS_MATH_ASSERT` (default: `assert`) can be redefined, e.g. to `HELIOS_ASSERT`, since this
module cannot depend on `engine/core`.

## Conventions

**Space.** Right-handed, **+Y up**, **forward = −Z**, right = +X (`kWorldUp/kWorldForward/
kWorldRight`). Cameras and objects look down −Z. Units: metres, seconds, radians.

**Vectors and matrices.** Column vectors, `v' = M * v`; `A * B` applies `B` first. Storage is
column-major (`m[c]` is column `c`, `m.at(row, col)` an element) — identical to GLSL/Slang
`mat4`, so matrices upload unchanged. Translation lives in column 3 (`data()[12..14]`).

**Quaternions.** Hamilton product, stored `x, y, z, w`; `q1 * q2` applies `q2` first. Positive
angles follow the right-hand rule. Euler angles are `Vec3(pitch, yaw, roll)` = rotations about
(X, Y, Z) composed as `R = Ry(yaw) · Rx(pitch) · Rz(roll)` (roll first). Positive pitch looks up,
positive yaw turns left.

**Clip space / projection (Vulkan).** Depth range **[0, 1]** with **reverse-Z**: the near plane
maps to depth 1, the far plane (or infinity) to 0. Use a `GREATER` (or `GREATER_OR_EQUAL`) depth
test, clear depth to 0, and a `D32_SFLOAT` buffer.
`Mat4::perspectiveReverseZ(fovY, aspect, zNear)` has an **infinite far plane**
(`depth = zNear / −z_view`); overloads exist for a finite far plane and for orthographic
projections. `linearDepthFromReverseZ` inverts the infinite projection.

**Y flip.** Projections produce clip space with **+Y up** (D3D/GL style). The Vulkan backend
renders with a **negative viewport height** (core since Vulkan 1.1 / `VK_KHR_maintenance1`), which
maps +Y to the top of the framebuffer and keeps counter-clockwise front faces. The same matrices
therefore also serve a future D3D12 backend. Passes that do not use the flipped viewport, or that
reconstruct positions from raw Vulkan NDC (+Y down), use `flipClipY(proj)`. Frustum extraction
works for both variants.

**Triangles** are counter-clockwise when seen from the front (`normal = cross(b − a, c − a)`).
Planes are `dot(n, p) + d` (positive on the normal side); frustum planes point inwards.

## Large worlds (ADR-005)

* Positions are **frame-local f64** (`DVec3`, `DTransform`, `FramePos`); local offsets,
  rotations and scales are f32. An f64 coordinate below 10^13 m lies on a 2^-9 m = 1.95 mm grid
  (≤ 0.98 mm rounding): sub-millimetre everywhere a single frame can reach.
* **Camera-relative rendering:** subtract the camera origin in f64, then round the small result
  to f32 (`toCameraRelative`, `toMatrixCameraRelative`); the view matrix holds only rotation
  (`cameraRelativeView`). The subtraction is exact for nearby points (Sterbenz' lemma), so the GPU
  never sees large numbers. `tests/test_precision.cpp` proves: at 10^13 m the conversion error is
  < 4 µm for objects within 100 m (end to end ≤ 0.98 mm including storage), the full f32
  model-view-projection chain stays within 0.0003 px of an exact f64 reference at 4K, and naive
  f32 world positions fail completely (their grid is 2^20 m ≈ 1049 km).
* Never form an absolute world-space vertex position on the way to the GPU: build relative
  vectors first (object origin − camera origin, then the local offset). Going through an absolute
  position at 10^13 m reintroduces the 1.95 mm grid.
* Things that move together (crew in a ship) must be expressed in the **shared frame**; computing
  their camera-relative offset through independent absolute f64 positions quantizes it to the
  global grid (up to one grid step of error, visible in a cockpit).

## Reference frames (`frame.h`)

A `FrameTransform` describes child frame C in parent P: `p_P = position + rotation · p_C`, plus
the velocity of C's origin and C's angular velocity (both relative to P, in P's axes). Frame
orientation is **f64** (`DQuat`): an f32 quaternion (~6e-8 rad) would displace points on a
6000 km planet by ~0.3 m (tested). `stateToParent/stateToChild` implement the transport theorem
(`v_P = v_f + ω × (R p) + R v_C`), `composeFrames/inverseFrame/relativeFrame` form frame chains,
and `reparent()` moves a body between frames while preserving its position and inertial
velocity — the math behind the world module's `Reparent()`. `rotatingBodyFrame()` builds a
planet frame (spin about its local +Y, optional axial tilt). The frame graph itself belongs to the
world module; by convention the root (galaxy) frame is `kRootFrame` (id 0).

## Determinism

Deterministic procedural generation (ADR-006) and quantize-on-both-sides networking (R07) need
**bit-identical** results on Windows clients and Linux servers.

* Code in `src/` uses only exactly specified IEEE operations (+ − × ÷, sqrt, floor, fmod,
  integer hashing) and exact cast-based floors; `src/fp_control.h` disables FMA contraction by
  pragma for Clang, GCC and MSVC. The module also sets `-ffp-contract=off` (GCC/Clang) or
  `/clang:-ffp-contract=off` (clang-cl) as a **PUBLIC** option so every TU instantiating the
  header-inline math agrees. Never compile with `-ffast-math` (it is rejected by `#error` in
  `src/`). The pragmas cover GCC in every mode and Clang/clang-cl in the default contraction
  mode; Clang's `-ffp-contract=fast` fuses in the backend regardless of pragmas, so the CMake
  flag is required there.
* Bit-identical by contract (golden-value tests): `helios::noise::*`, `helios::det::*`, all of
  `pack.h`, `spherical.h`, 8-bit sRGB tables, `kelvinToRgb`, `hashU32/hashU64`, and quaternions
  built from angles (`fromAxisAngle`, `fromEuler`, `fromRotationVector`, hence `integrate`,
  `advanceFrame`, `rotatingBodyFrame`), which use `det::sinCos`.
  The golden tables were generated with GCC 13 and verified identical with Clang 18 at -O0/-O2,
  `-O3 -march=native` and `-O2 -mavx2 -mfma`. If a golden test fails on one platform only, fix
  the build flags — never regenerate the tables. (The noise tables were regenerated once, when
  the lattice hash was fixed; see *Noise*.)
* Everything else that is header-inline and uses only + − × ÷ and `sqrt` (vector, matrix and
  quaternion arithmetic, `normalize`, inverses, transforms, frame conversions) is deterministic
  under the module's flags as well. Functions that call the CRT's transcendental functions are
  **not** bit-identical across CRTs: `slerp`, `toEuler`, `rotationAngle/angleBetween`,
  `toAxisAngle`, vector `angleBetween/signedAngle/rotate`, `Mat4::rotationX/Y/Z` and the
  projections (`std::tan`), `srgbToLinear/linearToSrgb` (`std::pow`). They are fine for rendering
  and presentation; use `det::` in anything two machines must agree on.
* `std::sin/cos/atan2/pow` differ in the last bit between the MSVC CRT and glibc. Use
  `det::sin/cos/sinCos/atan/atan2/asin/acos` in any code whose result must match across machines
  (≤ 2–3 ulp). `wrapAngle` uses IEEE `remainder`, which is exact.

## Packing (`pack.h`) and colour formats

| Encoding | Size | Error (tested) |
|---|---|---|
| `floatToHalf` / `halfToFloat` | 16 bit | IEEE binary16, round-to-nearest-even incl. denormals/inf/NaN; verified exhaustively + against an independent reference |
| UNORM/SNORM 8/16, `quantizeRange` | N bit | ≤ ½ step; SNORM −1 has two codes (Vulkan) |
| Octahedral normals | 8+8 / 12+12 / 16+16 | ≤ 0.64° / 0.039° / 0.0025°, axes exact |
| Smallest-three quaternion | 2 + 3N bit (N = 9: 29 bit, N = 10: 32 bit) | ≤ √6/(2^(N−1)−1) rad (0.55° / 0.28°), identity exact, q ≡ −q |
| `PositionQuantizer` | N bit/axis | ≤ step/2 in range (1 mm, 26 bit → ±33.5 km), re-encoding is idempotent |
| RGBA8 / SRGBA8 / RGB10A2 / RGB9E5 / RGBE / RGBM | 32 bit | layouts match the Vulkan formats (R in the low bits) |

## Noise (`noise.h`)

Improved Perlin (2D/3D/4D), simplex (2D/3D/4D, kernel r² = 0.5 so it is continuous), fBm,
ridged multifractal, domain warp and cellular F1/F2 (+ `cellularSite` for Voronoi placement).
All take a `u32` seed (derive per-layer seeds with `deriveSeed`). Gradients come from a seeded
lattice hash instead of a 256-entry permutation: each coordinate is mixed by its own seeded
bijective hash and the per-axis results are summed and mixed again, so the pattern only repeats
when a coordinate wraps at 2^32 cells. (The first implementation mixed one linear combination
`s + x·Px + y·Py + …` mod 2^32, which is constant along short kernel vectors: 4D noise repeated
every ~221 units and 3D noise every ~1566 — regression test *noise: no short lattice periods*.)
Outputs are in [−1, 1] (ridged [0, 1]). f32 and f64 overloads exist; f64 is recommended for
planet-scale coordinates. Budget ≈ 30–60 ns per 3D gradient-noise sample, ≤ 350 ns per 3D
cellular sample (see `noise.h`).

## Cube-sphere planets (`spherical.h`)

Faces `±X, ±Y, ±Z`, each with axes `(u, v)` where `cross(u, v) = normal` (side faces have
`v = +Y`). Face coordinates `st ∈ [−1, 1]²`. Mappings: `Gnomonic` (5.0× area distortion),
`EquiAngular` (default, 1.40×), `Nowell` (1.29×, trig-free, closed-form inverse). `CubeTile`
addresses quadtree tiles (64-bit key `[face:3][level:5][x:28][y:28]`), with parent/child and
edge neighbours that cross face borders. Latitude is positive towards +Y, longitude is 0 at +Z and
grows towards +X (east = prograde rotation about +Y).

## Threading

All types are plain values and all functions are pure (no global mutable state, no lazily built
tables); everything is safe to call concurrently.

## Known limitations

* TRS `compose/inverse/relativeTo` are exact only for uniform parent scale (non-uniform scale with
  rotation produces shear); use matrices when that matters.
* Only `Mat4 × Mat4` (f32) has an SSE2 path; it is bit-identical to the scalar path (tested).
* `det::sin/cos` are ≤ 2 ulp for |x| < 2^30 (≈ 1.07e9); larger arguments are folded with `fmod`
  (deterministic, not accurate).
* Only the functions listed under *Determinism* are pinned by golden tests; CRT-based helpers
  (listed there) may differ by an ulp between Windows and Linux.
* Noise inputs must stay below 2^31 (f32) / 2^52 (f64); the lattice hash wraps every 2^32 cells
  per axis.
* `PositionQuantizer::fromExtent` returns an invalid quantizer (`isValid() == false`, bits = 0)
  when 32 bits cannot cover the extent; check it before encoding.
