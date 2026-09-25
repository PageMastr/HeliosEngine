// The 8-lane AVX2 terrain VM kernel: pcg's default and the budgeted path (02 §5.8, 03 §5.5a). Built
// with helios_avx2_sources() (AVX2, BMI1/2, LZCNT, POPCNT, F16C, no FMA); dispatched to only after
// CPUID confirmed those features (kernel.cpp). 32x32 -> 64 products use _mm256_mul_epi32 /
// _mm256_mul_epu32 on the even and odd lanes.
#include "vm_kernels.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

namespace helios::pcg::vm {
namespace {

struct LanesAvx2 {
    static constexpr u32 kWidth = 8;
    static constexpr u32 kQWidth = 4;
    using V = __m256i;
    using Q = __m256i;

    static HELIOS_FORCEINLINE V set1(i32 x) { return _mm256_set1_epi32(x); }
    static HELIOS_FORCEINLINE V load(const i32* p) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
    static HELIOS_FORCEINLINE V loadU(const u32* p) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
    static HELIOS_FORCEINLINE void store(i32* p, V v) { _mm256_store_si256(reinterpret_cast<__m256i*>(p), v); }
    static HELIOS_FORCEINLINE void storeU(u32* p, V v) { _mm256_store_si256(reinterpret_cast<__m256i*>(p), v); }
    static HELIOS_FORCEINLINE V add(V a, V b) { return _mm256_add_epi32(a, b); }
    static HELIOS_FORCEINLINE V sub(V a, V b) { return _mm256_sub_epi32(a, b); }
    static HELIOS_FORCEINLINE V mullo(V a, V b) { return _mm256_mullo_epi32(a, b); }
    static HELIOS_FORCEINLINE V band(V a, V b) { return _mm256_and_si256(a, b); }
    static HELIOS_FORCEINLINE V bor(V a, V b) { return _mm256_or_si256(a, b); }
    static HELIOS_FORCEINLINE V bxor(V a, V b) { return _mm256_xor_si256(a, b); }
    template <int N>
    static HELIOS_FORCEINLINE V slli(V a) {
        return _mm256_slli_epi32(a, N);
    }
    template <int N>
    static HELIOS_FORCEINLINE V srli(V a) {
        return _mm256_srli_epi32(a, N);
    }
    template <int N>
    static HELIOS_FORCEINLINE V srai(V a) {
        return _mm256_srai_epi32(a, N);
    }
    static HELIOS_FORCEINLINE V sll(V a, i32 n) { return _mm256_sll_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V srl(V a, i32 n) { return _mm256_srl_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V sra(V a, i32 n) { return _mm256_sra_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V cmpgt(V a, V b) { return _mm256_cmpgt_epi32(a, b); }
    static HELIOS_FORCEINLINE V cmpeq(V a, V b) { return _mm256_cmpeq_epi32(a, b); }
    static HELIOS_FORCEINLINE V select(V mask, V ifTrue, V ifFalse) { return _mm256_blendv_epi8(ifFalse, ifTrue, mask); }
    static HELIOS_FORCEINLINE V abs(V a) { return _mm256_abs_epi32(a); }
    static HELIOS_FORCEINLINE V max(V a, V b) { return _mm256_max_epi32(a, b); }
    static HELIOS_FORCEINLINE bool uniform(V v, i32& out) {
        out = _mm256_cvtsi256_si32(v);
        return _mm256_movemask_epi8(_mm256_cmpeq_epi32(v, _mm256_set1_epi32(out))) == -1;
    }

    // Signed 32x32 -> 64 products of the even lanes and of the odd lanes.
    static HELIOS_FORCEINLINE void mulEvenOdd(V a, V b, V& even, V& odd) {
        even = _mm256_mul_epi32(a, b);
        odd = _mm256_mul_epi32(_mm256_srli_epi64(a, 32), _mm256_srli_epi64(b, 32));
    }
    static HELIOS_FORCEINLINE void mulEvenOddU(V a, V b, V& even, V& odd) {
        even = _mm256_mul_epu32(a, b);
        odd = _mm256_mul_epu32(_mm256_srli_epi64(a, 32), _mm256_srli_epi64(b, 32));
    }
    // Bits S..S+31 of (product + 2^(S-1)) for each lane, 0 < S < 32.
    template <int S>
    static HELIOS_FORCEINLINE V roundShift(V even, V odd) {
        const V half = _mm256_set1_epi64x(i64(1) << (S - 1));
        even = _mm256_add_epi64(even, half);
        odd = _mm256_add_epi64(odd, half);
        return _mm256_blend_epi32(_mm256_srli_epi64(even, S), _mm256_slli_epi64(odd, 32 - S), 0xAA);
    }
    static HELIOS_FORCEINLINE V mulQ16(V a, V b) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        return roundShift<16>(e, o);
    }
    /// hnoise::fade: t3 = (t^2 * t) >> 24, b = 10 * 2^32 + t (6t - 15 * 2^16), (t3 * b + 2^39) >> 40.
    static HELIOS_FORCEINLINE V fade(V t) {
        const V t2 = _mm256_mullo_epi32(t, t);
        V e, o;
        mulEvenOddU(t2, t, e, o);
        const V t3 = _mm256_blend_epi32(_mm256_srli_epi64(e, 24), _mm256_slli_epi64(o, 8), 0xAA);
        const V a = _mm256_sub_epi32(_mm256_add_epi32(_mm256_slli_epi32(t, 2), _mm256_slli_epi32(t, 1)),
                                     _mm256_set1_epi32(15 * 65536));
        V be, bo;
        mulEvenOdd(t, a, be, bo);
        const V ten = _mm256_set1_epi64x(i64(10) << 32);
        be = _mm256_add_epi64(be, ten);
        bo = _mm256_add_epi64(bo, ten);
        const V round = _mm256_set1_epi64x(i64(1) << 39);
        const V t3o = _mm256_srli_epi64(t3, 32);
        const V pe = _mm256_add_epi64(_mm256_add_epi64(_mm256_mul_epu32(t3, be),
                                                       _mm256_slli_epi64(_mm256_mul_epu32(t3, _mm256_srli_epi64(be, 32)), 32)),
                                      round);
        const V po = _mm256_add_epi64(_mm256_add_epi64(_mm256_mul_epu32(t3o, bo),
                                                       _mm256_slli_epi64(_mm256_mul_epu32(t3o, _mm256_srli_epi64(bo, 32)), 32)),
                                      round);
        // Bits 40..71 of each product: even lanes to the low dword, odd lanes to the high dword.
        return _mm256_blend_epi32(_mm256_srli_epi64(pe, 40), _mm256_srli_epi64(po, 8), 0xAA);
    }
    static HELIOS_FORCEINLINE V mulQ30(V a, V b) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        return roundShift<30>(e, o);
    }
    static HELIOS_FORCEINLINE V mulQ30u(V a, V b) {
        V e, o;
        mulEvenOddU(a, b, e, o);
        return roundShift<30>(e, o);
    }
    static HELIOS_FORCEINLINE V mulQ31u(V a, V b) {
        V e, o;
        mulEvenOddU(a, b, e, o);
        return roundShift<31>(e, o);
    }
    static HELIOS_FORCEINLINE void splitProduct(V even, V odd, V& lo, V& hi) {
        lo = _mm256_blend_epi32(even, _mm256_slli_epi64(odd, 32), 0xAA);
        hi = _mm256_blend_epi32(_mm256_srli_epi64(even, 32), odd, 0xAA);
    }
    static HELIOS_FORCEINLINE void mulWide(V a, V b, V& lo, V& hi) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        splitProduct(e, o, lo, hi);
    }
    static HELIOS_FORCEINLINE void mulWideU(V a, V b, V& lo, V& hi) {
        V e, o;
        mulEvenOddU(a, b, e, o);
        splitProduct(e, o, lo, hi);
    }

    struct Acc {
        __m256i even, odd; // 64-bit sums of lanes 0,2,4,6 and 1,3,5,7
    };
    static HELIOS_FORCEINLINE Acc accZero() { return {_mm256_setzero_si256(), _mm256_setzero_si256()}; }
    static HELIOS_FORCEINLINE void accMulAdd(Acc& acc, V a, V b) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        acc.even = _mm256_add_epi64(acc.even, e);
        acc.odd = _mm256_add_epi64(acc.odd, o);
    }
    static HELIOS_FORCEINLINE void accStore(i64* out, const Acc& acc) {
        const __m256i lo = _mm256_unpacklo_epi64(acc.even, acc.odd); // p0 p1 | p4 p5
        const __m256i hi = _mm256_unpackhi_epi64(acc.even, acc.odd); // p2 p3 | p6 p7
        _mm256_store_si256(reinterpret_cast<__m256i*>(out), _mm256_permute2x128_si256(lo, hi, 0x20));
        _mm256_store_si256(reinterpret_cast<__m256i*>(out + 4), _mm256_permute2x128_si256(lo, hi, 0x31));
    }

    static HELIOS_FORCEINLINE Q loadQ(const i64* p) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
    static HELIOS_FORCEINLINE void storeQ(i64* p, Q v) { _mm256_store_si256(reinterpret_cast<__m256i*>(p), v); }
    static HELIOS_FORCEINLINE Q set1Q(i64 v) { return _mm256_set1_epi64x(v); }
    static HELIOS_FORCEINLINE Q addQ(Q a, Q b) { return _mm256_add_epi64(a, b); }
    static HELIOS_FORCEINLINE Q subQ(Q a, Q b) { return _mm256_sub_epi64(a, b); }
    static HELIOS_FORCEINLINE Q minQ(Q a, Q b) { return _mm256_blendv_epi8(a, b, _mm256_cmpgt_epi64(a, b)); }
    static HELIOS_FORCEINLINE Q maxQ(Q a, Q b) { return _mm256_blendv_epi8(b, a, _mm256_cmpgt_epi64(a, b)); }
    static HELIOS_FORCEINLINE Q selectGeQ(Q cond, Q threshold, Q ifGe, Q ifLt) {
        return _mm256_blendv_epi8(ifGe, ifLt, _mm256_cmpgt_epi64(threshold, cond));
    }
    static HELIOS_FORCEINLINE Q mulQ32(Q a, Q b) {
        const __m256i ah = _mm256_srli_epi64(a, 32), bh = _mm256_srli_epi64(b, 32);
        const __m256i ll = _mm256_mul_epu32(a, b);
        const __m256i lh = _mm256_mul_epu32(a, bh);
        const __m256i hl = _mm256_mul_epu32(ah, b);
        const __m256i hh = _mm256_mul_epu32(ah, bh);
        const __m256i t = _mm256_srli_epi64(_mm256_add_epi64(ll, _mm256_set1_epi64x(i64(1) << 31)), 32);
        __m256i r = _mm256_add_epi64(_mm256_add_epi64(_mm256_slli_epi64(hh, 32), lh), _mm256_add_epi64(hl, t));
        const __m256i zero = _mm256_setzero_si256();
        r = _mm256_sub_epi64(r, _mm256_and_si256(_mm256_cmpgt_epi64(zero, a), _mm256_slli_epi64(b, 32)));
        r = _mm256_sub_epi64(r, _mm256_and_si256(_mm256_cmpgt_epi64(zero, b), _mm256_slli_epi64(a, 32)));
        return r;
    }
};

using L = LanesAvx2;
#include "vm_ops.inl"

constexpr Kernels kKernels = makeKernels("avx2");

} // namespace

const Kernels* const kAvx2Kernels = &kKernels;

} // namespace helios::pcg::vm

#else

namespace helios::pcg::vm {
const Kernels* const kAvx2Kernels = nullptr;
} // namespace helios::pcg::vm

#endif
