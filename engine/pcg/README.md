# engine/pcg — deterministic PCG runtime

L3, HEADLESS (02 §1.1). Fixed-point `hnoise`, the terrain graph and its register VM (02 §5.8), with a
32-bit-only Slang twin for GPU tiles (03 §5.5, §5.5a) and the shared conformance corpus
(`tests/corpus/hnoise`). Work package WP-0.9 and its sub-WP WP-0.9c (the throughput spike;
[`docs/adr/ADR-0.9c-hnoise-throughput.md`](../../docs/adr/ADR-0.9c-hnoise-throughput.md)).

Everything that feeds heights is integer arithmetic, so every CPU kernel, compiler and GPU produces
the same bits. There is no floating point in the deterministic path (the only `f64` use is the
compile-time level-adaptive octave choice, which is a function of `(graph, level)` computed once on the
CPU and shipped to both twins inside the bytecode).

## hnoise (`include/helios/pcg/hnoise.h`, the normative scalar reference)

| Element | Definition |
|---|---|
| Lattice hash | xxHash32 of the 12-byte little-endian cell `(x, y, z)` with the octave seed (tests prove equality with the vendored `XXH32`) |
| Gradients | Perlin 2002: 12 cube-edge gradients from the top four hash bits |
| Fade | 6t⁵ − 15t⁴ + 10t³ with a single rounding (t³ with 24 fraction bits, the quadratic factor exact in 64 bits): monotonic, ≤ 0.54 ulp |
| Interpolation | Q16.16 lerps `a + (w (b − a) + 2^15) >> 16` (helios::Q16 rounding) |
| Positions | Q32.32 metres per axis; octave k splits them at wavelength 2^(e−k) m into a 32-bit cell and a Q16 fraction |
| Octaves | seed + k·0x9E3779B9; amplitude recurrence `amp_{k+1} = amp_k · gain` in Q16 |
| Cube-sphere domain | math's `CubeFace` bases; Q2.30 face coordinates `(tile·64 + i)·2^(25−level) − 2^30` (levels 0–25); an odd degree-11 polynomial EquiAngular warp (≤ 1.2e-7 from tan, exactly ±1 at the edges, so faces meet bit-exactly); a 4-step integer Newton rsqrt (≤ 2 ulp of math's exact `rsqrtQ30`); radius in Q24.8 metres |
| Heights | Q32.32 metres (`helios::Q32`), exact in f64 below 2^20 m |

## Terrain graph and VM

`TerrainGraph` builds a DAG of value nodes (constant, noise, fbm, ridged, add, sub, mul, min, max,
clamp, remap, select) and position nodes (the base position and domain warps).
`compileTerrainGraph()` validates it, drops dead nodes, applies the level-adaptive octave rule
(03 §5.5a; the skipped amplitude budget scales with the sample spacing, 12 cm at 2 m) and allocates
registers by linear scan into 16-word instructions (layout in `program.h`; the GPU interprets the same
words).

The level-adaptive budget is spent on each octave's effect on the **output**, not on its own
amplitude. A static analysis bounds every live node's value and the output's sensitivity to it
(Lipschitz 1 through add, sub, min, max and clamp, `|scale|` through remap, the other operand's bound
through mul). A generator that feeds a select condition is never skipped, because the select can flip
by the full difference of its branches. An octave costs `|amp| × 1.04 × sensitivity`, where 1.04 bounds
`|noise3|` (the true maximum is 1.0363). Before this rule, an fbm stretched ×50,000 by a remap lost two
octaves at 3 m spacing and moved the output by 125 m against a 0.18 m budget.
`TerrainProgram::skippedAmplitudeMetres` is that output bound, and a test checks it against measured
tiles of the reference graph at levels 0–12.

**Validation before evaluation.** `validateTileDomain()` rejects cube tiles outside face 0–5, level
0–25 and `x, y < 2^level`, which were previously clamped or wrapped into some other tile.
`validateTerrainProgram()` rejects malformed bytecode. That includes any read of a register before it
is written and an output register that is never written: the CPU's scratch registers persist between
tiles while the Slang twin starts from zero or the base position, so such a program would make the
twins diverge. `TileEvaluator::evaluate()` and the GPU twin harness call both. `TileEvaluator` evaluates a program over a 65×65 tile op by op (one indirect call per op per
tile). `evaluateSampleReference()` re-evaluates single samples from `hnoise.h` and `helios::Q16/Q32`
alone, sharing no code with the kernels.

**Kernels** (`src/kernels/`): the op bodies are written once in `vm_ops.inl` against a lane type and
compiled three times: `vm_avx2.cpp` (8 × i32; products via `_mm256_mul_epi32`/`_mm256_mul_epu32` on
even and odd lanes; the budgeted default), `vm_sse42.cpp` (4 × i32, the width-4 twin, never
budgeted) and `vm_scalar.cpp` (the reference width and NEON template). When the eight lanes of a block
share a lattice cell, a coherent path hashes the cell once with scalar code (and caches it per octave)
instead of hashing per lane; results are bit-identical. The kernel TUs hold integer code only (CTest
`pcg_lint_kernels`) and no inline function that could leak into other TUs.

**Selection** (`kernel.h`): `initializePcgModule()` reads the `pcg.kernel` CVar (default `avx2`) and
logs `pcg.kernel=<name>`. Until WP-0.2r builds whole images at the `avx2` level, the choice is also
checked against CPUID (`core::cpuGate()`; the AVX2 kernel needs the full `Avx2Image` feature set
because it is built with the whole avx2 flag set) and falls back to the widest supported kernel with a
warning. `vm_avx2.cpp` is a designated `*_avx2.cpp` unit (`helios_avx2_sources`), `vm_sse42.cpp` gets
`-msse4.2` (not an AVX-class flag).

## GPU twin (`shaders/pcg/`)

`hnoise.slang` (module `pcg.hnoise`) implements the same functions and a per-sample VM interpreter with
32-bit integer operations only: Q32.32 values are `uint2 {lo, hi}` with explicit carries, and products
use `umulExtended` / `imulExtended`. `hnoise_tile.slang` holds the compute entry points (`csTile`,
`csLattice`). Three checks keep it 32-bit: the source lint (`pcg_lint_twin`), the SPIR-V capability
check (`pcg_lint_twin_spirv`: no `Int64`/`Float64`/float types) and the conformance test itself.

## Tests and tools

| Target | What |
|---|---|
| `pcg_tests` | hnoise reference properties (XXH32 equality, gradients, fade, splitting, warp, rsqrt, face seams), graph validation and compiler, kernel equivalence (every node kind, six domains, random graphs, Q32 extremes), seamless tiles, the corpus on every CPU kernel, and `perf:` timing |
| `pcg_gpu_tests` (label `gpu`) | the Slang twin on the Vulkan adapter (lavapipe in CI): every corpus lattice case and every corpus tile's positions and heights bit for bit, plus a batched dispatch |
| `pcg_hnoise_bench` | the WP-0.9c spike: CPU ms per tile per core per kernel (full detail and level-adaptive), the twin hashes, GPU tiles per 0.8 ms; exits 2 unless the dispatched kernel is `avx2` (`--allow-non-avx2` for smoke runs) |

Regenerating the corpus: `HELIOS_UPDATE_HNOISE_CORPUS=1 pcg_tests -tc="corpus:*"` (see
`tests/corpus/hnoise/README.md`; only for a deliberate algorithm change).

## Deviations from the plan text

- CPUID-checked kernel selection (02 §5.8 says no CPUID is needed once whole images are `avx2`; that
  arrives with WP-0.2r).
- Phase 0 node set only: no dedicated crater, terrace, cellular, erosion or region nodes yet; the
  reference graph approximates those shapes with the Phase 0 nodes.
- The GPU twin interprets bytecode per sample (uniform control flow across a dispatch) rather than
  compiling each graph to Slang, so the client needs no shader compiler for new graphs.
- WP-0.9c's CPU clause is red on the development machine (see the ADR): 03 §5.5a's cost model assumes
  ≈ 120 integer operations per noise evaluation; the kernels need ≈ 200 (coherent) to ≈ 600 (general)
  vector instructions per 8-lane evaluation.

## Plan conformance

Plan-Rev: 6

Written to plan revision 6 (02 §5.8; 03 §5.5a; 09 §2.1 WP-0.9 and WP-0.9c) on 2026-09-25,
ahead of its round (it needs WP-0.2r), under `docs/plan/09-roadmap-and-process.md` §5.10.2 D7. The
open deviations are listed in this README; the module still needs its row in 09 §8.1.
