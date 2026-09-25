# ADR-0.9c: hnoise throughput spike

| | |
|---|---|
| **Status** | **Recorded, outcome RED (F3 trigger) on the CPU clause; GPU clause unconfirmed.** Measured 2026-09-25 in the development container. The CPU clause fails by about 6× on the only CPU measured. The GPU clause was measured only on lavapipe, which 03 §5.5a counts as a CI budget figure, never a performance claim. The **MIN confirmation (GTX 1660 S, RX 5600 XT, Ryzen 5 3600) is still due before WP-1.8 starts**, and the Phase 0 exit records this spike as "pending its MIN confirmation" (09 §2.1) |
| **Decides** | The WP-0.9c outcome of [03 §5.5a](../plan/03-rendering.md#55a-terrain-generation-throughput-32-bit-twin-cost-model-spike-fallbacks): green, amber (F2) or red (F3) |
| **Arms** | Risk **K5b** ([09 §7](../plan/09-roadmap-and-process.md#7-risk-register)): its trigger is "spike below green" |
| **Owner** | Render lead (03 §5.5a), with the runtime lead for the CPU VM (02 §5.8) |
| **Evidence** | `pcg_hnoise_bench` (`engine/pcg/bench/hnoise_bench.cpp`), `pcg_tests` `perf:` case, `pcg_gpu_tests`; code in `engine/pcg/` and `shaders/pcg/`; the shared corpus `tests/corpus/hnoise` |

## 1. What was measured

**Workload.** `makeReferenceGraph40()` (`engine/pcg/src/reference_graphs.cpp`): 40 nodes shaped like the
Harrow T04 preset. It has warped continents, ridged mountains behind a regional mask, hills, plains, fine
detail down to 0.25 m, a crater-like field (inverted ridges behind a noise mask), clamped terrace plateaus
and erosion-like ridges. There are 10 generators with 80 octaves and 2 domain warps, so **86 noise
evaluations per sample at full detail**. Phase 0 has no dedicated crater or terrace nodes, so those
shapes are built from the Phase 0 node set. The costs are the node set's real costs.

**Tiles.** 65 × 65 samples:
- *full detail*: Harrow (1,500 km) level-15 tiles, the collision level (1.12 m spacing), with every octave;
- *level-adaptive*: Harrow level-10 render tiles (36 m spacing). 03 §5.5a's rule skips 11 octaves,
  which leaves 75 evaluations. The skipped octaves can move the output by at most 1.86 m, within the
  0.06 × spacing budget of 2.2 m; the largest difference measured on a level-10 tile is 0.28 m.
  - Since the WP-0.9 review, the rule weighs each octave by its effect on the output height: its
    amplitude, times the 1.04 noise bound, times the generator's static sensitivity through remap
    and mul, and never for a select condition. See `engine/pcg/README.md`.
  - The old rule weighed octaves by their own amplitude only. At level 10 both rules skip the same 11
    octaves, so the timings below still apply.
  - At level 0 the new rule keeps octaves that feed masks. It keeps the crater-region noise, a select
    condition, and 4 of the mountain-region mask's 7 octaves instead of 1, because that mask multiplies
    3 km mountains. One corpus case, `ref40.r1500km.l0.face3.adaptive`, was regenerated.

**Implementation measured.** The fixed-point hnoise of 02 §5.8, bit-exact across every twin:
- **Lattice hash.** xxHash32 of the cell.
- **Gradients.** Perlin 2002 edge gradients.
- **Fade.** A single-rounding quintic.
- **Lerps.** Q16.16.
- **Positions.** Q32.32.
- **CPU kernels.** Scalar, 4-lane SSE4.2 and 8-lane AVX2. All three are built from one `vm_ops.inl`, and the
  wide kernels add a coherent-cell path.
- **GPU twin.** A 32-bit-only Slang twin that interprets the same bytecode per sample.
- **Conformance.** Every twin hashes identically: the `433cbc2f51be57b6` checksum of the benchmark tiles is
  the same on scalar, SSE4.2 and AVX2, under both GCC and Clang, and `pcg_gpu_tests` passes every corpus tile
  on lavapipe.

**Machine.** It was not a plan reference box:
- A shared 4-vCPU cloud VM, a Firecracker guest on an "Intel Xeon Processor @ 2.80 GHz" (Cascade Lake
  class, AVX-512 present but unused).
- Linux, `RelWithDebInfo` (`-O2`).
- One thread per CPU measurement. The median over 16 distinct tiles, after a warm-up tile:
  `pcg_hnoise_bench --tiles=16` (the GPU part uses the defaults, 64 tiles per dispatch and 5 repeats).
  The `433cbc2f51be57b6` checksum belongs to that 16-tile set; the default `--tiles=32` set hashes to
  `26b7213be843c5d5`, again on every kernel.
- lavapipe: Mesa 25.2.8 llvmpipe (LLVM 20.1, 256-bit) on the same 4 vCPUs.

## 2. Results

**CPU VM, ms per 65 × 65 tile per core** (median; `pcg.kernel=avx2` logged and asserted by the bench):

| Kernel | GCC 13.3 full detail | GCC 13.3 level-adaptive | Clang 18.1 full detail | Clang 18.1 level-adaptive |
|---|---|---|---|---|
| **avx2 (budgeted)** | **3.23** | 3.60 | **2.97** | 3.33 |
| sse42 (twin, information only) | 5.59 | 6.37 | 5.35 | 5.97 |
| scalar (reference) | 13.70 | 16.05 | 11.21 | 11.01 |

- The SSE4.2 twin runs at about 1.8× the AVX2 time, as 02 §5.8 expected.
- The level-adaptive tiles cost *more* than full detail at 36 m spacing, even though they have 11 fewer
  octaves. The 8 lanes of a block cover 280 m there, so fewer blocks share a lattice cell and fewer of them take
  the coherent path.

**GPU tiles per 0.8 ms**:

| Adapter | Full detail (64 tiles per dispatch) | Level-adaptive |
|---|---|---|
| lavapipe (llvmpipe, 4 vCPUs) | 119 ms per 64 tiles → **0.43 tiles per 0.8 ms** | 110 ms → 0.47 tiles per 0.8 ms |
| `win-gpu` runner | not run (WP-0.4 runner not available to this work package) | — |
| MIN: GTX 1660 S, RX 5600 XT | not run (the H1 lab has not landed) | — |

GPU time is the wall time of submit plus wait for one dispatch, minus an empty dispatch's (0.25 ms). The
RHI has no timestamp queries yet, so the figure slightly overstates the kernel's own time. lavapipe runs on
the same CPU cores as the CPU kernels, which says nothing about a real GPU. It also shares them with
everything else on the VM: a re-run while another build was compiling (load average about 7 on the 4
vCPUs) measured 253 ms per 64 tiles (0.20 tiles per 0.8 ms). The CPU medians in that run stayed within
3 % of the table above (AVX2 3.15 ms GCC, 3.11 ms Clang).

## 3. Outcome against 03 §5.5a

| Clause | Threshold on MIN | Measured | Verdict |
|---|---|---|---|
| CPU | ≤ 0.5 ms per tile per core, AVX2, full detail | 2.97–3.23 ms on a 2.8 GHz Cascade Lake core | **Red (about 6×)**. MIN (Ryzen 5 3600, 3.6–4.2 GHz) is unmeasured, but clock and IPC do not close a 6× gap |
| GPU | Green ≥ 64 tiles per 0.8 ms; amber ≥ 16; red < 16 | lavapipe only (0.43), no claim | **Unconfirmed**. The estimate below says amber is plausible on MIN |

**Outcome: RED (F3), pending the MIN confirmation.** The CPU clause alone decides red. The run was valid
by 03 §5.5a's own rule, because it logged `pcg.kernel=avx2`. **K5b is armed.**

**GPU estimate (not a measurement).**
- On the GPU the per-sample work is scalar 32-bit code. The instruction count per noise evaluation is about
  2× the 120 operations of 03 §5.5a's cost model (§4).
- A full-detail 64-tile burst therefore needs about 7 T integer ops/s, against a GTX 1660 S peak of about
  2.5 T.
- That points to roughly 20 tiles per 0.8 ms on MIN at full detail, which is **amber (F2)** territory.
  Level-adaptive tiles do better.
- Only the MIN run can settle it.

## 4. Why the CPU misses: the cost model

03 §5.5a assumes about 120 32-bit integer operations per noise evaluation, which is 42 M per tile, and
expects 0.5–0.8 ms per core at 8 lanes. The measured kernel needs:

| Path | Vector instructions per 8-lane evaluation | When |
|---|---|---|
| General (lanes in different lattice cells) | ≈ 600 | Mostly octaves shorter than 32 m at the collision level (16 of the reference graph's 86) |
| Coherent (every lane in one cell: one scalar hash per cell, cached per octave; bit-identical) | ≈ 200 | Every longer octave |

Where the general path spends its instructions:
- 33 `vpmulld` and about 130 other operations for the eight xxHash32 corners. The x, y and z rounds are
  shared, but eight avalanches remain.
- About 128 for eight gradient selections.
- About 75 for three single-rounding fades, built on 32×32→64 products that AVX2 only offers on even and odd
  lanes.
- About 80 for seven 64-bit-product lerps.
- Register spills.

Even a coherent-only kernel would take about 1 ms per tile on this core. The 0.5 ms budget is out of reach
for an 86-evaluation graph with this algorithm, whatever the tuning.

**Optimizations left, estimated.** Neither changes the outcome:
- 2×4 lane blocks instead of 8×1, for more coherent blocks on the 8–32 m octaves: about 1.2–1.3×.
- A per-tile lattice cache for octaves longer than the tile: saves only the scalar hashing, which is
  already cheap.

## 5. Decision

**Adopt F3, the pre-decided red action of 03 §5.5a, and extend it.** F3 as written does not close the gap for
this graph shape.
- **F3 as written.** Octaves with a wavelength below 4 m become `visualOnly`, and T04's validator bounds their
  summed amplitude to ≤ 5 cm. Collision then evaluates only the fixed-point base octaves. Far levels, with a
  minimum view distance of at least 2 km, use the float twin `hnoise_f32.slang`, held to ≤ 10 cm.
  - For the reference graph this removes only 5 of the 86 evaluations: the finest four octaves of `detail`
    and one of `erosion`.
  - The CPU cost stays at about 2.8 ms per tile.
- **Extension, proposed for 03 §5.5a and 02 §5.8.** These need the plan owners' sign-off, through an ADR
  amendment or a plan PR:
  1. **Budget collision graphs in evaluations, not only in nodes.** T04's budget analyzer caps a body's
     collision-level evaluations per sample. At the measured kernel speed, 0.5 ms per tile per core allows
     about 12–16 evaluations. Mountain and continent octaves above about 64 m then come from a coarser,
     cached parent evaluation (02 §5.8a's slab ancestors already evaluate level `L_c` − 3), and only the fine
     remainder runs per collision tile. The ancestor reuse has to stay bit-exact, which needs its own design
     and corpus cases.
  2. **Re-state 02 §5.8a's per-tile costs from measurements.** Its fence (≤ 8 core-ms per tick on cells) and
     prefetch budget (1,000 core-ms/s) assume a VM cost of ≤ 0.5 ms per tile, while the measured cost is
     about 3 ms. Either the evaluation cap above restores the 0.5 ms, or those budgets shrink about 6× in
     tiles per second. RT-20 (Phase 1) is the gate that will show which.
  3. **Keep the GPU path as the only producer of visual tiles.** The CPU produces collision tiles only.
     03 §5.5a already says this.

**Why not green by optimization alone.** The measured structure (§4) puts the floor near 1 ms per tile per
core for 86 evaluations. 03 §5.5a's instruction to measure first was right, and the measurement says the
budget and the graph size do not fit together.

## 6. Procedure for the pending runs

1. **`win-gpu` runner** (WP-0.4, the user's Windows PC).
   - Build `windows-msvc-release`.
   - Run `pcg_hnoise_bench --hardware-gpu --tiles=64 --gpu-tiles=64 --repeats=20 --json=hnoise-win-gpu.json`
     and set `HELIOS_RHI_ADAPTER` to the discrete GPU's name when more than one adapter exists.
   - The run is valid only if the log shows `pcg.kernel=avx2` (exit code 2 otherwise) and the GPU batch is
     bit-identical to the CPU (exit code 3 otherwise).
   - Record full-detail and level-adaptive tiles per 0.8 ms and the CPU ms per tile in this ADR.
2. **MIN confirmation**, due before WP-1.8 starts (K7's trigger counts an unmeasured MIN at the Phase 1
   midpoint as red).
   - Run the same command on the lab's MIN-NV box (Ryzen 5 3600 + GTX 1660 S) and MIN-AMD box (RX 5600 XT),
     on Windows 10 22H2, with the release binary.
   - Apply 03 §5.5a's table to those numbers: green only if the GPU reaches ≥ 64 tiles **and** the CPU is ≤ 0.5
     ms; amber (F2) if the GPU reaches ≥ 16; red (F3) otherwise.
   - Also run `--no-gpu` on the SERVER box and record the CPU figure for 02 §5.8a's cell budgets.
3. **Real-GPU conformance.**
   - `pcg_gpu_tests` with `HELIOS_RHI_ADAPTER` set to the vendor GPU must pass every corpus case bit for bit
     on NVIDIA, AMD and Intel (RT-04, 03 §5.5 "10⁶ hashed samples").
   - A failing vendor switches near-field tiles to the CPU collision tiles (03 §5.5 fallback). K5 records the
     vendor.

## 7. Consequences

- **Plan.** 03 §5.5a's outcome is red (F3) until the MIN run says otherwise, and the extension in §5 needs
  a decision before WP-1.3 (PCG for Harrow) sizes its collision graphs. RC-13's CPU clause (≤ 0.5 ms on MIN,
  "or F3 is adopted") is met only through F3 plus the evaluation cap.
- **Code.** The measurement guard exists: the bench refuses a non-`avx2` run. The `perf:` gate
  (`pcg_tests_perf`, nightly tier) asserts 03 §5.5a's 0.5 ms and **fails today**. The failure is intended: it
  is the K5b signal, and the threshold must not be relaxed to get green.
- **Twin.** Bit-exactness of the 32-bit twin is established on lavapipe for the whole corpus (46 tiles,
  231 lattice cases), which is RT-04's C++/Slang clause on the CI adapter.
