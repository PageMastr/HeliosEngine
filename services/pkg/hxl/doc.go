// Package hxl is the Go interpreter of HXL, the Helios eXpression Language (06 §1.2): the pure,
// deterministic formula language that designers use for derived attributes, modifier magnitudes,
// industry durations and fees, vendor prices and crew-mission timers (05 §8).
//
// It is a twin of the C++ module engine/hxl: the same lexer, parser, type checker and code generator
// (Compile produces byte-identical bytecode, pinned by FNV-1a hashes in the shared corpus), the same
// verified bytecode format (Decode accepts exactly what C++ accepts) and the same VM (Eval returns
// bit-identical float64 results). The corpus tests/corpus/hxl/*.jsonc runs in both languages
// (corpus_test.go here, engine/hxl/tests/test_corpus.cpp in C++); GP-1 requires 100 % agreement,
// including the >= 1,000 FMA-sensitive vectors, on windows/amd64, linux/amd64 at GOAMD64=v1 and v3,
// and linux/arm64.
//
// Language summary (the grammar is in engine/hxl/include/helios/hxl/compiler.h):
//
//	formula TurretHitChance(src, tgt, ctx) =
//	  0.5 ^ ((ctx.angularVelocity * 40000 / (attr(src, TrackingSpeed) * attr(tgt, SignatureRadius)))^2
//	        + (max(0, ctx.distance - attr(src, OptimalRange)) / attr(src, FalloffRange))^2)
//
// Values are numbers (float64) and booleans. Built-ins: attr, tag, curve, stacks, level, select
// (lazy), min, max, clamp, lerp, pow (also `^`), exp, ln, sqrt, asinh, abs, floor, ceil. && and ||
// short-circuit. There are no loops, calls or recursion: bytecode jumps only forward, so the static
// Program.Cost bounds every evaluation, and Decode verifies untrusted bytecode completely.
//
// Float rules (06 §1.2, normative for this package and det): every float multiplication is the
// direct operand of an explicit float64() conversion, no constant arithmetic, no math.FMA and no
// libm-style math functions. The hxlfloat analyzer (package hxlfloat) enforces them; services
// build with GOAMD64=v1 as defence in depth (CheckGOAMD64).
//
// Concurrency: a Program is immutable and safe for concurrent use; an Env serves one evaluation at a
// time.
package hxl
