package det

// Constants of engine/math/src/det_exp.cpp as single hex-float literals (06 §1.2 rule 2: no constant
// arithmetic). TestConstantsMatchCpp compares every value with the C++ source; change both together.
const (
	ln2Head      = 0x1.62e42fefa4000p-1 // ln 2 to 40 bits: e * head is exact for |e| < 2^13
	ln2Mid       = -0x1.8432a1b0e2634p-43
	ln2Tail      = 0x1.f97b57a079a19p-103
	ln2Hi        = 0x1.62e42fefa39efp-1 // correctly rounded ln 2
	ln2Lo        = 0x1.abc9e3b39803fp-56
	invLn2       = 0x1.71547652b82fep+0
	twoThirdsHi  = 0x1.5555555555555p-1
	twoThirdsLo  = 0x1.5555555555555p-55
	sqrt2        = 0x1.6a09e667f3bcdp+0
	expOverflow  = 0x1.62e42fefa39efp+9  // 709.782712893384: exp beyond it is > MaxFloat64
	expUnderflow = -0x1.74910d52d3051p+9 // -745.1332191019411: exp below it rounds to 0
	two28        = 0x1p+28
	twoMinus28   = 0x1p-28
	two64        = 0x1p+64
	two53        = 0x1p+53
	two54        = 0x1p+54
	twoMinus1000 = 0x1p-1000
	splitter     = 0x1.0000002p+27 // 2^27 + 1 (Veltkamp)
)

// 2/(2k+1) for k = 2..14: ln m = 2u + 2u^3/3 + u^5 * (c5 + u^2 (c7 + ...)).
var atanhC = [13]float64{
	0x1.999999999999ap-2, 0x1.2492492492492p-2, 0x1.c71c71c71c71cp-3, 0x1.745d1745d1746p-3,
	0x1.3b13b13b13b14p-3, 0x1.1111111111111p-3, 0x1.e1e1e1e1e1e1ep-4, 0x1.af286bca1af28p-4,
	0x1.8618618618618p-4, 0x1.642c8590b2164p-4, 0x1.47ae147ae147bp-4, 0x1.2f684bda12f68p-4,
	0x1.1a7b9611a7b96p-4,
}

// 1/n! for n = 3..15: exp(r) - 1 - r - r^2/2 = r^3 * (c3 + r (c4 + ...)).
var expC = [13]float64{
	0x1.5555555555555p-3, 0x1.5555555555555p-5, 0x1.1111111111111p-7, 0x1.6c16c16c16c17p-10,
	0x1.a01a01a01a01ap-13, 0x1.a01a01a01a01ap-16, 0x1.71de3a556c734p-19, 0x1.27e4fb7789f5cp-22,
	0x1.ae64567f544e4p-26, 0x1.1eed8eff8d898p-29, 0x1.6124613a86d09p-33, 0x1.93974a8c07c9dp-37,
	0x1.ae7f3e733b81fp-41,
}
