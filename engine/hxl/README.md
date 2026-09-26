# engine/hxl — HXL, the Helios eXpression Language

`helios::hxl` (CMake target `helios::hxl`, L2, HEADLESS; tests `hxl_tests`) is the C++ half of HXL
(06 §1.2): the pure, deterministic formula language designers use for derived attributes, modifier
magnitudes, industry durations and fees, vendor prices, crew-mission timers and hit chances. Its Go
twin is [`services/pkg/hxl`](../../services/pkg/hxl); both run the shared corpus
[`tests/corpus/hxl`](../../tests/corpus/hxl) and must agree **bit for bit** (GP-1).

| Header | Contents |
|---|---|
| `helios/hxl/compiler.h` | `compile(source, CompileOptions, Diagnostic*)`: lexer, parser, type checker, code generator; the grammar |
| `helios/hxl/program.h` | `Program` (verified bytecode + symbol tables), `Status` codes, `Diagnostic`, `Op`, `limits::`, `encode()`/`decode()`/`hash()`/`disassemble()` |
| `helios/hxl/vm.h` | `eval()`/`evaluate()`, the host `Env` interface, `Curve`, `MapEnv` (name-keyed env for tools and tests), `hxlMin`/`hxlMax` |
| `helios/hxl/hxl.h` | umbrella |

```cpp
#include "helios/hxl/hxl.h"
using namespace helios::hxl;

auto p = compile("formula TurretHitChance(src, tgt, ctx) = 0.5 ^ ((ctx.angularVelocity * 40000 / "
                 "(attr(src, TrackingSpeed) * attr(tgt, SignatureRadius)))^2 + (max(0, ctx.distance - "
                 "attr(src, OptimalRange)) / attr(src, FalloffRange))^2)");
MapEnv env;                                   // or your own Env over ECS components
env.attrs["src"]["TrackingSpeed"] = 0.05;     // ...
env.bind(*p);
Value v;
if (eval(*p, env, v) == Status::Ok) use(v.number);
std::vector<u8> bytes = p->encode();          // what the cook stores; Program::decode() verifies it
```

## Language

Values are numbers (f64) and booleans. A source is an expression over host-declared parameters
(default `self`) or a declaration `formula Name(a, b) = expr` (the `.hschema` `formula` form).

| Form | Meaning |
|---|---|
| `1.5`, `.5`, `2e-3`, `true`, `false` | literals (must be zero or a normal double) |
| `p.field` | context field of parameter `p` (`ctx.distance`) |
| `attr(p, Shield.Max)` | attribute of `p` |
| `tag(p, State.Debuff)` | `p` holds the tag or a descendant (06 §1.1) |
| `curve(Xp.Level, x)` | piecewise-linear curve, clamped at the ends |
| `stacks()`, `level()` | evaluation context (effect stacks and level) |
| `+ - * / ^`, unary `-` | arithmetic; `^` is `pow`, right-associative, binds tighter than unary minus (`-2^2 == -4`) |
| `< <= > >= == !=`, `&& \|\| !` | comparisons (IEEE: NaN compares false) and short-circuit logic |
| `select(c, a, b)` | lazy conditional (the untaken branch never reads inputs) |
| `min max` (2+ args), `clamp lerp pow exp ln sqrt asinh abs floor ceil` | built-ins |

06's built-in list is `min max clamp lerp select pow exp ln sqrt asinh curve attr tag stacks level`;
`abs`, `floor` and `ceil` are added because they are exact IEEE operations (06 §1.2 rule 3 allows
them in Go) and designers need rounding. Comments are `//` to the end of the line.

## Determinism (06 §1.2 cross-language float rules)

- Every op rounds to f64 once; no constant folding, reassociation or fusion (the compiler emits
  `1 + 2` as `Const Const Add`). The module compiles with `-ffp-contract=off` plus
  `src/fp_control.h` pragmas; MSVC's `/fp:precise` never contracts.
- `pow`, `exp`, `ln`, `asinh` are `helios::det` (06's `hmath`), never the CRT; `sqrt floor ceil abs`
  and `+ − × ÷` are exact IEEE-754.
- `min`/`max` propagate NaN and order `-0 < +0`; `clamp(x, lo, hi) = min(max(x, lo), hi)`;
  `lerp(a, b, t) = a + (b − a)·t`; curves `v[i] + (v[i+1] − v[i])·((x − k[i]) / (k[i+1] − k[i]))`.
- A NaN result is returned as the canonical `0x7ff8000000000000` (x86 and arm64 produce different NaN
  payloads, which must never be observable).

## Safety and cost

Bytecode jumps only forward and has no calls, so each op runs at most once and the static
`Program::cost()` bounds every evaluation (default budget `limits::kMaxCost` = 4096 units; `pow`,
`exp`, `ln`, `asinh` cost 8, `curve` 4, input reads 2, everything else 1). Parse depth (128), tree
depth (256), ops (4096), code size (16 KiB), constants (1024), symbols (256 per table), stack (256)
and source size (64 KiB) are limited (`E_LIMIT`). `Program::decode()` verifies untrusted bytecode
completely: opcodes and operand ranges, stack depth and types at every op and jump target, forward
jumps onto instruction boundaries, the header (result type, stack, cost) and canonical form
(constants and symbols in first-use order, no unused or duplicate entries), so decoded programs are
as safe to run as compiled ones. A 30,000-mutant fuzz test checks this on every run. `eval()` never
allocates (a 2 KiB stack array).

## Status codes

`E_LEX E_NUMBER E_SYNTAX E_UNKNOWN_NAME E_UNKNOWN_FUNCTION E_ARITY E_TYPE E_ENTITY_ARG E_SYMBOL_ARG
E_DUPLICATE_PARAM E_LIMIT E_RESULT_TYPE E_BYTECODE E_MISSING_INPUT`, with a 1-based `line:column`
(byte columns) for compile errors. Status names and positions are part of the C++/Go contract: the
corpus checks them. Semantic errors are reported in evaluation order (children left to right, then
the node). `toError()` maps them to `helios::Error` (`ParseError`, `InvalidArgument`,
`LimitExceeded`, `Corrupt`, `NotFound`).

## Bytecode (`HXL1`)

Little-endian: magic `HXL1`, u8 format version (1), u8 result type, u16 max stack, u32 cost,
str name, u8 param count + strs, u16 constant count + u64 bit patterns, four symbol tables (attrs,
tags, fields, curves; u16 count + strs), u32 code size + code. Strings are u16 length + bytes. Ops
and operands are listed in `program.h`. `Program::hash()` (FNV-1a 64 of the encoding) is what the
corpus pins, so both compilers must emit identical bytes.

## Tests

`hxl_tests`: compiler (precedence, every diagnostic with its position, limits), VM semantics (IEEE
edge cases, laziness, missing inputs, curves, det built-ins, the EVE turret formula), bytecode
(pinned encoding, verifier rejections, 30,000-mutant fuzz) and the shared corpus: 9 files, 406
cases, 1,497 evaluations including 1,050 FMA-sensitive vectors and 312 bytecode hashes.

## Known limitations

- schemac does not compile `formula` declarations and `HxlExpr` fields yet (02 §3.5 `records`
  emitter); hosts compile the source with this module until then.
- Only one level of context fields (`p.field`); no user functions or formula-to-formula calls.
- MSVC and clang-cl are verified only in CI (09 §5.4); locally the corpus runs on GCC and Clang
  (MinGW is compile/link only).

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
