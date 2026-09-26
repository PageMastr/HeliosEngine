// Package clean follows every 06 §1.2 float rule, including the patterns the analyzer must not
// flag (explicitly converted products, integer arithmetic, exact math functions, single literals).
package clean

import "math"

const half = 0.5
const negLiteral = -0x1.8432a1b0e2634p-43
const count = 3 * 4 // integer constant arithmetic is fine

func g(a, b, c float64, i, j int) float64 {
	x := float64(a*b) + c
	y := float64((a * b)) - c
	z := float64(float64(a*b) * c)
	k := i * j
	var arr [count * 2]float64
	s := math.Sqrt(a) + math.Abs(b) + math.Floor(c) + math.Ceil(a) + math.Trunc(b) + math.Copysign(a, b)
	bits := math.Float64frombits(math.Float64bits(a))
	if math.IsNaN(a) || math.IsInf(b, 0) || math.Signbit(c) {
		return math.NaN() + math.Inf(1)
	}
	d := a/b + c - half
	u := -x
	x = float64(x * u)
	f := float32(float32(a) * float32(b))
	return x + y + z + float64(k) + arr[0] + s + bits + d + u + negLiteral + float64(f)
}
