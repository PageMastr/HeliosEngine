// Package planted holds deliberate violations of the 06 §1.2 float rules. Every violation is marked
// with a `want` comment naming its rule; hxlfloat_test.go requires all of them to be reported and
// nothing else.
package planted

import "math"

const badConst = 2 * math.Pi     // want "const-arith"
const alsoBad = 1.0 / 3.0        // want "const-arith"
const nested = (1.5 + 2.5) * 4.0 // want "const-arith"

func f(a, b, c float64) float64 {
	x := a*b + c                   // want "unconverted-mul"
	y := a * b                     // want "unconverted-mul"
	z := c - (a * b)               // want "unconverted-mul"
	x *= y                         // want "unconverted-mul"
	v := float64(a * b * c)        // want "unconverted-mul"
	w := math.FMA(a, b, c)         // want "banned-math"
	e := math.Exp(a) + math.Log(b) // want "banned-math" "banned-math"
	p := math.Pow(a, 2)            // want "banned-math"
	fn := math.Asinh               // want "banned-math"
	m := math.Mod(a, 2)            // want "banned-math"
	l := math.Ldexp(a, 3)          // want "banned-math"
	var f32 = float32(a)
	g := f32 * f32 // want "unconverted-mul"
	half := 0.5
	h := half * 2 // want "unconverted-mul"
	return x + y + z + v + w + e + p + fn(c) + m + l + float64(g) + h + badConst + alsoBad + nested
}
