# pkg/hxl — the Go HXL interpreter

Go twin of [`engine/hxl`](../../../engine/hxl) (06 §1.2, 05 §8): the same compiler, bytecode format,
verifier and VM, bit-identical with C++ on the shared corpus [`tests/corpus/hxl`](../../../tests/corpus/hxl).
The Industry service uses it for job durations and fees, vendors for price checks, crew missions for
timers; content validators use it for `HxlExpr` fields.

```go
p, err := hxl.Compile("formula JobDuration(bp, pilot) = bp.baseSecs * (1 - 0.04 * attr(pilot, Skill.Industry))",
	hxl.CompileOptions{})
env := &hxl.MapEnv{Fields: map[string]map[string]float64{"bp": {"baseSecs": 3600}},
	Attrs: map[string]map[string]float64{"pilot": {"Skill.Industry": 5}}}
env.Bind(p)
v, status := p.Eval(env)              // v.Number, hxl.OK
b := p.Encode()                        // canonical bytecode; hxl.Decode(b) verifies untrusted bytes
```

| Package | Contents |
|---|---|
| `hxl` | `Compile`, `Program` (`Encode`, `Decode`, `Hash`, `Eval`, `Disassemble`), `Env`, `MapEnv`, `Curve`, `Diagnostic`/`Status`, `CheckGOAMD64`, record helpers (`CompileDerived`, `CompileMagnitude`, `ValidateReasonCode`) |
| `hxl/det` | port of `helios::det` exp/ln/pow/asinh; its tests reproduce the C++ golden hashes over ~95k inputs and check every constant against `engine/math/src/det_exp.cpp` |
| `hxl/hxlfloat` | the `hxlfloat` analyzer (06 §1.2 rule 5), std-lib only; `go run ./pkg/hxl/hxlfloat/cmd/hxlfloat ./pkg/hxl ./pkg/hxl/det` |
| `hxl/gamedef` | **generated** by helios-schemac from `schemas/gameplay/*.hschema`; do not edit. After a schema change, run `cmake --build <build dir> --target gameplay_gamedef_sync` and commit the result; the `lint` CTest `lint_gamedef_go_current` fails while the committed copy is stale. Not `gofmt`-clean: that comes from schemac's Go emitter (follow-up in `tools/schemac`; CI does not run `gofmt`) |

## Float rules (06 §1.2)

Every float multiplication is the direct operand of `float64()` (an explicit conversion is a rounding
point, so the compiler cannot fuse `x*y + z` into an FMA on arm64 or at `GOAMD64=v3`); constants
are single hex-float literals (Go folds constant expressions exactly); only exact `math` functions
(`Sqrt`, `Abs`, `Floor`, `Ceil`, `Trunc`, `Copysign`, bit conversions). `hxlfloat` enforces all three
and its test checks 22 planted violations (including type parameters with float type sets) and a
clean fixture. It skips test files and generated files (marker before the package clause); in
`pkg/hxl` that exempts only `gamedef`, whose single float product (a JSON duration in
`helios_runtime.go`) is not HXL arithmetic and is still covered by the machine-code FMA check. Services should still build with
`GOAMD64=v1` and call `hxl.CheckGOAMD64(production)` at start (defence in depth, rule 6).

Open items against 06 §1.2's tooling, each with a proposed owner (the Director assigns them):

| Rule | Open item | Proposed owner |
|---|---|---|
| 2 | The `det` constants are typed hex literals copied from `engine/math/src/det_exp.cpp` by hand and pinned by `TestConstantsMatchCpp`; there is no `helios-tool hxl-gen-consts` | The tools WP that creates `helios-tool` (WP-0.7b or later) |
| 5 | `hxlfloat` is a standard-library analyzer that runs as a unit test (`TestHXLPackagesClean`, so every `go test ./...` enforces it) and through `go run`, not a `go/analysis` pass under `go vet -vettool`; there is no pre-commit hook | WP-0.1 (CI and hooks) |
| 6 | `hxl.CheckGOAMD64` exists, but `helios-backend` does not call it and log the level at start (05 §8) | The backend WP that owns `services/cmd/helios-backend` (WP-0.15r) |

## GP-1 coverage

| GP-1 run | Where |
|---|---|
| `linux/amd64`, `GOAMD64=v1` | CI (`services` job, `go test ./...`) |
| `windows/amd64` | CI (`services` job on `windows-latest`) |
| `linux/amd64` (and `windows/amd64`), `GOAMD64=v3` | CI, through `TestCorpusAtGOAMD64v3`: every `go test ./pkg/hxl/` on amd64 builds the test binary at `GOAMD64=v3` and runs `TestCorpus` and `TestFMAHazardIsReal` in it (skipped with `-short` or on a CPU without x86-64-v3) |
| `linux/arm64` | Not in CI yet (needs an arm64 runner, WP-0.1); not run |

Every `go test` also compiles `pkg/hxl` and `pkg/hxl/det` for `arm64` and `amd64`/`v3` and checks that
the listings contain no fused multiply-add (`TestNoFusedMultiplyAddInMachineCode`), and checks that an
unconverted `a*b + c` does fuse on the FMA-sensitive vectors when the target fuses
(`TestFMAHazardIsReal`). That is evidence for the missing runs, not a substitute for them.

## Tests

```
go test ./pkg/hxl/...                     # corpus, unit tests, det goldens, hxlfloat, generated types
GOAMD64=v3 go test ./pkg/hxl/...          # FMA-capable codegen must give the same bits
go test -fuzz FuzzDecode ./pkg/hxl/       # also FuzzCompile
HXL_CORPUS_FILL=1 go test ./pkg/hxl/ -run TestCorpusFill          # fill "?" placeholders (see the corpus README)
HXL_CORPUS_GEN_FMA=1 go test ./pkg/hxl/ -run TestGenerateFMACorpus # regenerate fma_sensitive.jsonc
```

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7, and re-checked against 06 §1.2 in WP-0.19's review
on 2026-09-26. No D7 conformance delta is open (§5.10.4 (c)), meaning no plan change since Plan-Rev 6
contradicts this code. That does not mean nothing is open: the GP-1 gaps (the `linux/arm64` run) and
the tooling items above are open WP-0.19 follow-ups.
