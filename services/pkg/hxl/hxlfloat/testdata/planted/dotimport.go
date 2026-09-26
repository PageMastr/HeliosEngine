package planted

// Dot and aliased imports must not hide banned math functions (review regression).

import (
	. "math"
	m "math"
)

func dot(a float64) float64 {
	e := Exp(a)       // want "banned-math"
	l := m.Log(a)     // want "banned-math"
	s := Sqrt(Abs(a)) // allowed: exact
	return e + l + s + Floor(a)
}
