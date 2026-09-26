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
| `hxl/gamedef` | **generated** by helios-schemac from `schemas/gameplay/*.hschema` (do not edit; the C++ build regenerates it) |

## Float rules (06 §1.2)

Every float multiplication is the direct operand of `float64()` (an explicit conversion is a rounding
point, so the compiler cannot fuse `x*y + z` into an FMA on arm64 or at `GOAMD64=v3`); constants
are single hex-float literals (Go folds constant expressions exactly); only exact `math` functions
(`Sqrt`, `Abs`, `Floor`, `Ceil`, `Trunc`, `Copysign`, bit conversions). `hxlfloat` enforces all three
and its test checks 17 planted violations and a clean fixture. Services should still build with
`GOAMD64=v1` and call `hxl.CheckGOAMD64(production)` at start (defence in depth, rule 6).

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
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
