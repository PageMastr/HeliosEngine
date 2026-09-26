// Package det is the Go port of helios::det's exp, ln, pow and asinh (engine/math/src/det_exp.cpp),
// the transcendental built-ins of HXL (06 §1.2, "hmath"). Results are bit-identical with the C++
// implementation on every GOARCH and GOAMD64 level.
//
// Float rules (06 §1.2, enforced by the hxlfloat analyzer in ../hxlfloat):
//   - every float multiplication is the direct operand of an explicit float64() conversion, which the
//     Go spec defines as a rounding point, so the compiler can never fuse it into an FMA (gc does
//     fuse on arm64 and at GOAMD64=v3, "possibly across statements");
//   - no constant arithmetic: constants are single hex-float literals copied from the C++ tables
//     (det_test.go checks them against engine/math/src/det_exp.cpp);
//   - only exact math functions (Sqrt, Abs, Floor, Copysign, bit conversions); never math.Exp/Log/Pow.
//
// The algorithm (double-double intermediates from error-free transformations) is documented in the
// C++ source; this file mirrors it statement by statement.
package det

import "math"

type dd struct{ hi, lo float64 }

// twoSum: s + e == a + b exactly.
func twoSum(a, b float64) dd {
	s := a + b
	bb := s - a
	e := (a - (s - bb)) + (b - bb)
	return dd{s, e}
}

// fastTwoSum requires |a| >= |b| (or a == 0).
func fastTwoSum(a, b float64) dd {
	s := a + b
	return dd{s, b - (s - a)}
}

// split: a == hi + lo with both halves of <= 26 significant bits (Veltkamp).
func split(a float64) dd {
	c := float64(splitter * a)
	hi := c - (c - a)
	return dd{hi, a - hi}
}

// twoProd: p + e == a * b exactly (Dekker).
func twoProd(a, b float64) dd {
	p := float64(a * b)
	as := split(a)
	bs := split(b)
	e := ((float64(as.hi*bs.hi) - p) + float64(as.hi*bs.lo) + float64(as.lo*bs.hi)) + float64(as.lo*bs.lo)
	return dd{p, e}
}

func ddMul(a, b dd) dd {
	p := twoProd(a.hi, b.hi)
	p.lo += float64(a.hi*b.lo) + float64(a.lo*b.hi)
	return fastTwoSum(p.hi, p.lo)
}

// pow2i returns 2^k for -1022 <= k <= 1023 exactly.
func pow2i(k int) float64 {
	return math.Float64frombits(uint64(k+1023) << 52)
}

// logDD returns ln x as a double-double for finite x > 0.
func logDD(x float64) dd {
	bits := math.Float64bits(x)
	e := 0
	if bits>>52 == 0 { // subnormal: scale into the normal range (exact)
		x = float64(x * two54)
		bits = math.Float64bits(x)
		e = -54
	}
	e += int(bits>>52) - 1023
	m := math.Float64frombits((bits & 0x000fffffffffffff) | 0x3ff0000000000000) // [1, 2)
	if m > sqrt2 {
		m = float64(m * 0.5)
		e++
	}
	f := m - 1.0 // exact (Sterbenz)
	den := twoSum(2.0, f)
	uh := f / den.hi
	p := twoProd(uh, den.hi)
	ul := (((f - p.hi) - p.lo) - float64(uh*den.lo)) / den.hi
	u := dd{uh, ul}
	u2 := twoProd(uh, uh)
	u2.lo += float64(float64(2.0*uh) * ul)
	u3 := ddMul(u2, u)
	t3 := ddMul(u3, dd{twoThirdsHi, twoThirdsLo})
	z := u2.hi
	poly := atanhC[12]
	for i := 11; i >= 0; i-- {
		poly = atanhC[i] + float64(z*poly)
	}
	tail := float64(float64(u3.hi*z) * poly)
	s := twoSum(float64(2.0*uh), t3.hi)
	s.lo += float64(2.0*ul) + t3.lo + tail
	s = fastTwoSum(s.hi, s.lo)
	if e == 0 {
		return s
	}
	ef := float64(e)
	q := twoProd(ef, ln2Mid)
	q.lo += float64(ef * ln2Tail)
	r := twoSum(float64(ef*ln2Head), s.hi)
	r.lo += s.lo + (q.hi + q.lo)
	return fastTwoSum(r.hi, r.lo)
}

// expDD returns exp(x.hi + x.lo) for |x.lo| <= ulp(x.hi) and finite x.hi.
func expDD(x dd) float64 {
	if x.hi > expOverflow {
		return math.Inf(1)
	}
	if x.hi < expUnderflow {
		return 0
	}
	kf := math.Floor(float64(x.hi*invLn2) + 0.5)
	k := int(kf)
	rh0 := x.hi - float64(kf*ln2Head)
	km := twoProd(kf, ln2Mid)
	r := twoSum(rh0, -km.hi)
	r.lo += (x.lo - km.lo) - float64(kf*ln2Tail)
	r = fastTwoSum(r.hi, r.lo)
	rh := r.hi
	r2 := twoProd(rh, rh)
	r2.lo += float64(float64(2.0*rh) * r.lo)
	poly := expC[12]
	for i := 11; i >= 0; i-- {
		poly = expC[i] + float64(rh*poly)
	}
	tail := float64(float64(r2.hi*rh) * poly)
	s := twoSum(rh, float64(0.5*r2.hi))
	s.lo += r.lo + float64(0.5*r2.lo) + tail
	y := twoSum(1.0, s.hi)
	y.lo += s.lo
	v := y.hi + y.lo
	if k > 1023 {
		return float64(float64(v*pow2i(k-1)) * 2.0)
	}
	if k < -1021 {
		return float64(float64(v*pow2i(k+1000)) * twoMinus1000) // one rounding into the subnormals
	}
	return float64(v * pow2i(k))
}

func isInteger(y float64) bool { return math.Floor(y) == y }

// isOddInteger reports whether the integer y is odd. Integers with |y| >= 2^53 are even. Halving is
// exact here, so this equals C++'s fmod(y, 2) != 0 without calling math.Mod.
func isOddInteger(y float64) bool {
	if math.Abs(y) >= two53 {
		return false
	}
	h := float64(y * 0.5)
	return math.Floor(h) != h
}

// Exp returns e^x (helios::det::exp).
func Exp(x float64) float64 {
	if math.IsNaN(x) {
		return x + x
	}
	if math.IsInf(x, 1) {
		return x
	}
	if math.IsInf(x, -1) {
		return 0
	}
	return expDD(dd{x, 0})
}

// Ln returns the natural logarithm (helios::det::ln).
func Ln(x float64) float64 {
	if math.IsNaN(x) {
		return x + x
	}
	if x < 0 {
		return math.NaN()
	}
	if x == 0 {
		return math.Inf(-1)
	}
	if math.IsInf(x, 1) {
		return x
	}
	r := logDD(x)
	return r.hi + r.lo
}

// Pow returns x^y with C99 Annex F special cases (helios::det::pow).
func Pow(x, y float64) float64 {
	if y == 0 {
		return 1
	}
	if x == 1 {
		return 1
	}
	if math.IsNaN(x) || math.IsNaN(y) {
		return x + y
	}
	yInt := !math.IsInf(y, 0) && isInteger(y)
	yOdd := yInt && isOddInteger(y)
	if x == 0 {
		if y < 0 {
			if yOdd {
				return math.Copysign(math.Inf(1), x)
			}
			return math.Inf(1)
		}
		if yOdd {
			return x
		}
		return 0
	}
	if math.IsInf(y, 0) {
		ax := math.Abs(x)
		if ax == 1 {
			return 1 // pow(-1, +-inf)
		}
		if (ax < 1) == (y < 0) {
			return math.Inf(1)
		}
		return 0
	}
	if math.IsInf(x, 0) {
		if x > 0 {
			if y < 0 {
				return 0
			}
			return math.Inf(1)
		}
		if y < 0 {
			if yOdd {
				return math.Copysign(0, -1)
			}
			return 0
		}
		if yOdd {
			return math.Inf(-1)
		}
		return math.Inf(1)
	}
	sign := 1.0
	if x < 0 {
		if !yInt {
			return math.NaN()
		}
		if yOdd {
			sign = -1.0
		}
		x = -x
	}
	// Exactly rounded shortcuts (what callers expect from pow(x, 1) and friends).
	if y == 1 {
		return float64(sign * x)
	}
	if y == 2 {
		return float64(x * x)
	}
	if y == -1 {
		return sign / x
	}
	if y == 0.5 {
		return math.Sqrt(x)
	}
	l := logDD(x)
	if math.Abs(y) > two64 {
		// |y ln x| > 2^64 * 2^-53 > 745: certain overflow or underflow (x != 1).
		if (l.hi > 0) == (y > 0) {
			return float64(sign * math.Inf(1))
		}
		return float64(sign * 0)
	}
	w := twoProd(y, l.hi)
	w.lo += float64(y * l.lo)
	w = fastTwoSum(w.hi, w.lo)
	return float64(sign * expDD(w))
}

// Asinh returns the inverse hyperbolic sine (helios::det::asinh).
func Asinh(x float64) float64 {
	if math.IsNaN(x) || math.IsInf(x, 0) || x == 0 {
		return x // NaN, +-inf and +-0 map to themselves
	}
	a := math.Abs(x)
	var r float64
	if a < twoMinus28 {
		return x // asinh(x) = x - x^3/6 + ..., and x^2/6 < 2^-58
	} else if a > two28 {
		l := logDD(a)
		s := twoSum(l.hi, ln2Hi)
		s.lo += l.lo + ln2Lo
		r = s.hi + s.lo
	} else {
		// t = a + sqrt(a^2 + 1), all in dd.
		a2 := twoProd(a, a)
		q := twoSum(1.0, a2.hi)
		q.lo += a2.lo
		q = fastTwoSum(q.hi, q.lo)
		sq := math.Sqrt(q.hi)
		sq2 := twoProd(sq, sq)
		corr := (((q.hi - sq2.hi) - sq2.lo) + q.lo) / float64(2.0*sq)
		t := twoSum(a, sq)
		t.lo += corr
		t = fastTwoSum(t.hi, t.lo)
		l := logDD(t.hi)
		r = l.hi + (l.lo + t.lo/t.hi)
	}
	return math.Copysign(r, x)
}
