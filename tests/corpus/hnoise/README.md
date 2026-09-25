# hnoise conformance corpus

The shared corpus of fixed-point `hnoise` and the terrain VM (02 §5.8, 03 §5.5, RT-04). Every twin must
reproduce every value **bit for bit**:

- C++ CPU kernels (scalar, 4-lane SSE4.2, 8-lane AVX2): `pcg_tests` (`engine/pcg/tests/test_corpus.cpp`);
- the 32-bit-only Slang twin (`shaders/pcg/hnoise.slang`) on the GPU: `pcg_gpu_tests`
  (`engine/pcg/tests/test_gpu.cpp`, CTest label `gpu`; lavapipe in CI, vendor GPUs on the lab runners).

CI runs the CPU side on every determinism toolchain (MSVC both toolsets, clang-cl, GCC, Clang, MinGW).
The MSVC and clang-cl jobs are the cross-compiler check for Windows; MinGW binaries are only built in
the Linux container.

## Files

| File | Contents |
|---|---|
| `lattice.jsonc` | `latticeHash` (xxHash32 of the cell) and `noise3` for edge cells (wrap at 2^32), edge fractions and random cells |
| `tiles.jsonc` | Per-node graphs (one per node kind) and the WP-0.9c reference graph on cube-sphere and planar tiles: program hash, base-position hash, height hash and three probe heights |

## Format

All 32/64-bit values are hex strings (`"0x..."`), so no JSON number precision is involved; i64 values
are their two's complement bit pattern. Both readers reject unknown or missing keys.

```jsonc
// lattice.jsonc
{"cases": [{"seed": "0x...", "cell": ["0x...", "0x...", "0x..."], "frac": [0, 65535, 23130],
            "hash": "0x...", "noise": "0x..."}]}
// tiles.jsonc
{"cases": [{"name": "ref40.r1500km.l15.face0.collision", "graph": "reference40" | "node:<kind>",
            "collision": true,   // CompileOptions.collisionLevel; otherwise level-adaptive at the domain's spacing
            "domain": {"kind": "cube", "face": 0, "level": 15, "x": 16384, "y": 16000, "radiusQ8": 384000000}
                    | {"kind": "planar", "origin": ["0x..", "0x..", "0x.."], "spacing": "0x..", "x": 3, "y": 7},
            "programHash": "0x...", "positionsHash": "0x...", "heightsHash": "0x...",
            "probes": ["0x...", "0x...", "0x..."]}]}   // heights of samples 0, 2112, 4224
```

Hashes are `helios::hash64` (XXH3-64) of the little-endian values: heights as Q32.32 `i64[4225]`
(sample `j * 65 + i`), positions as the x, y and z planes one after another.

## Updating

The corpus pins the algorithm. Regenerate it only for a deliberate change of hnoise or the VM, in the
same change that updates the scalar reference, all three kernels and the Slang twin. The `*.adaptive`
cases also pin the compiler's level-adaptive octave choice (their `programHash`), so a deliberate change
of that rule regenerates them too. The WP-0.9 review did this once, when the rule started weighting
octaves by their effect on the output; only `ref40.r1500km.l0.face3.adaptive` changed. Regenerate with:

```
HELIOS_UPDATE_HNOISE_CORPUS=1 pcg_tests -tc="corpus:*"
```

`pcg_tests` also regenerates the corpus in memory on every run and compares it with the checked-in
files, so an accidental change fails even if every twin changed the same way.
