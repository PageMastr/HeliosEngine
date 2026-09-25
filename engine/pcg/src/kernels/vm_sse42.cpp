// The 4-lane terrain VM kernel: the width-4 conformance twin (02 §5.8). It proves results do not
// depend on the lane width and is used by runs forced with pcg.kernel=sse42; it is never budgeted.
// Compiled with -msse4.2 (GCC/Clang/clang-cl; MSVC needs no flag); at the avx2 image level (WP-0.2r)
// the same intrinsics encode as VEX-128 without changing any result. Uses SSE4.1 products and
// blends and the SSE4.2 64-bit compare.
#include "vm_kernels.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <nmmintrin.h>
#include <smmintrin.h>

namespace helios::pcg::vm {
namespace {

struct LanesSse42 {
    static constexpr u32 kWidth = 4;
    static constexpr u32 kQWidth = 2;
    using V = __m128i;
    using Q = __m128i;

    static HELIOS_FORCEINLINE V set1(i32 x) { return _mm_set1_epi32(x); }
    static HELIOS_FORCEINLINE V load(const i32* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
    static HELIOS_FORCEINLINE V loadU(const u32* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
    static HELIOS_FORCEINLINE void store(i32* p, V v) { _mm_store_si128(reinterpret_cast<__m128i*>(p), v); }
    static HELIOS_FORCEINLINE void storeU(u32* p, V v) { _mm_store_si128(reinterpret_cast<__m128i*>(p), v); }
    static HELIOS_FORCEINLINE V add(V a, V b) { return _mm_add_epi32(a, b); }
    static HELIOS_FORCEINLINE V sub(V a, V b) { return _mm_sub_epi32(a, b); }
    static HELIOS_FORCEINLINE V mullo(V a, V b) { return _mm_mullo_epi32(a, b); }
    static HELIOS_FORCEINLINE V band(V a, V b) { return _mm_and_si128(a, b); }
    static HELIOS_FORCEINLINE V bor(V a, V b) { return _mm_or_si128(a, b); }
    static HELIOS_FORCEINLINE V bxor(V a, V b) { return _mm_xor_si128(a, b); }
    template <int N>
    static HELIOS_FORCEINLINE V slli(V a) {
        return _mm_slli_epi32(a, N);
    }
    template <int N>
    static HELIOS_FORCEINLINE V srli(V a) {
        return _mm_srli_epi32(a, N);
    }
    template <int N>
    static HELIOS_FORCEINLINE V srai(V a) {
        return _mm_srai_epi32(a, N);
    }
    static HELIOS_FORCEINLINE V sll(V a, i32 n) { return _mm_sll_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V srl(V a, i32 n) { return _mm_srl_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V sra(V a, i32 n) { return _mm_sra_epi32(a, _mm_cvtsi32_si128(n)); }
    static HELIOS_FORCEINLINE V cmpgt(V a, V b) { return _mm_cmpgt_epi32(a, b); }
    static HELIOS_FORCEINLINE V cmpeq(V a, V b) { return _mm_cmpeq_epi32(a, b); }
    static HELIOS_FORCEINLINE V select(V mask, V ifTrue, V ifFalse) { return _mm_blendv_epi8(ifFalse, ifTrue, mask); }
    static HELIOS_FORCEINLINE V abs(V a) { return _mm_abs_epi32(a); }
    static HELIOS_FORCEINLINE V max(V a, V b) { return _mm_max_epi32(a, b); }
    static HELIOS_FORCEINLINE bool uniform(V v, i32& out) {
        out = _mm_cvtsi128_si32(v);
        return _mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_set1_epi32(out))) == 0xFFFF;
    }

    static HELIOS_FORCEINLINE void mulEvenOdd(V a, V b, V& even, V& odd) {
        even = _mm_mul_epi32(a, b);
        odd = _mm_mul_epi32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
    }
    static HELIOS_FORCEINLINE void mulEvenOddU(V a, V b, V& even, V& odd) {
        even = _mm_mul_epu32(a, b);
        odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
    }
    // Odd dwords from the second operand: 16-bit blend mask 0xCC.
    template <int S>
    static HELIOS_FORCEINLINE V roundShift(V even, V odd) {
        const V half = _mm_set1_epi64x(i64(1) << (S - 1));
        even = _mm_add_epi64(even, half);
        odd = _mm_add_epi64(odd, half);
        return _mm_blend_epi16(_mm_srli_epi64(even, S), _mm_slli_epi64(odd, 32 - S), 0xCC);
    }
    static HELIOS_FORCEINLINE V mulQ16(V a, V b) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        return roundShift<16>(e, o);
    }
    /// hnoise::fade: t3 = (t^2 * t) >> 24, b = 10 * 2^32 + t (6t - 15 * 2^16), (t3 * b + 2^39) >> 40.
    static HELIOS_FORCEINLINE V fade(V t) {
        const V t2 = _mm_mullo_epi32(t, t);
        V e, o;
        mulEvenOddU(t2, t, e, o);
        const V t3 = _mm_blend_epi16(_mm_srli_epi64(e, 24), _mm_slli_epi64(o, 8), 0xCC);
        const V a = _mm_sub_epi32(_mm_add_epi32(_mm_slli_epi32(t, 2), _mm_slli_epi32(t, 1)), _mm_set1_epi32(15 * 65536));
        V be, bo;
        mulEvenOdd(t, a, be, bo);
        const V ten = _mm_set1_epi64x(i64(10) << 32);
        be = _mm_add_epi64(be, ten);
        bo = _mm_add_epi64(bo, ten);
        const V round = _mm_set1_epi64x(i64(1) << 39);
        const V t3o = _mm_srli_epi64(t3, 32);
        const V pe = _mm_add_epi64(
            _mm_add_epi64(_mm_mul_epu32(t3, be), _mm_slli_epi64(_mm_mul_epu32(t3, _mm_srli_epi64(be, 32)), 32)), round);
        const V po = _mm_add_epi64(
            _mm_add_epi64(_mm_mul_epu32(t3o, bo), _mm_slli_epi64(_mm_mul_epu32(t3o, _mm_srli_epi64(bo, 32)), 32)), round);
        return _mm_blend_epi16(_mm_srli_epi64(pe, 40), _mm_srli_epi64(po, 8), 0xCC);
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
        lo = _mm_blend_epi16(even, _mm_slli_epi64(odd, 32), 0xCC);
        hi = _mm_blend_epi16(_mm_srli_epi64(even, 32), odd, 0xCC);
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
        __m128i even, odd;
    };
    static HELIOS_FORCEINLINE Acc accZero() { return {_mm_setzero_si128(), _mm_setzero_si128()}; }
    static HELIOS_FORCEINLINE void accMulAdd(Acc& acc, V a, V b) {
        V e, o;
        mulEvenOdd(a, b, e, o);
        acc.even = _mm_add_epi64(acc.even, e);
        acc.odd = _mm_add_epi64(acc.odd, o);
    }
    static HELIOS_FORCEINLINE void accStore(i64* out, const Acc& acc) {
        _mm_store_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi64(acc.even, acc.odd));
        _mm_store_si128(reinterpret_cast<__m128i*>(out + 2), _mm_unpackhi_epi64(acc.even, acc.odd));
    }

    static HELIOS_FORCEINLINE Q loadQ(const i64* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
    static HELIOS_FORCEINLINE void storeQ(i64* p, Q v) { _mm_store_si128(reinterpret_cast<__m128i*>(p), v); }
    static HELIOS_FORCEINLINE Q set1Q(i64 v) { return _mm_set1_epi64x(v); }
    static HELIOS_FORCEINLINE Q addQ(Q a, Q b) { return _mm_add_epi64(a, b); }
    static HELIOS_FORCEINLINE Q subQ(Q a, Q b) { return _mm_sub_epi64(a, b); }
    static HELIOS_FORCEINLINE Q minQ(Q a, Q b) { return _mm_blendv_epi8(a, b, _mm_cmpgt_epi64(a, b)); }
    static HELIOS_FORCEINLINE Q maxQ(Q a, Q b) { return _mm_blendv_epi8(b, a, _mm_cmpgt_epi64(a, b)); }
    static HELIOS_FORCEINLINE Q selectGeQ(Q cond, Q threshold, Q ifGe, Q ifLt) {
        return _mm_blendv_epi8(ifGe, ifLt, _mm_cmpgt_epi64(threshold, cond));
    }
    static HELIOS_FORCEINLINE Q mulQ32(Q a, Q b) {
        const __m128i ah = _mm_srli_epi64(a, 32), bh = _mm_srli_epi64(b, 32);
        const __m128i ll = _mm_mul_epu32(a, b);
        const __m128i lh = _mm_mul_epu32(a, bh);
        const __m128i hl = _mm_mul_epu32(ah, b);
        const __m128i hh = _mm_mul_epu32(ah, bh);
        const __m128i t = _mm_srli_epi64(_mm_add_epi64(ll, _mm_set1_epi64x(i64(1) << 31)), 32);
        __m128i r = _mm_add_epi64(_mm_add_epi64(_mm_slli_epi64(hh, 32), lh), _mm_add_epi64(hl, t));
        const __m128i zero = _mm_setzero_si128();
        r = _mm_sub_epi64(r, _mm_and_si128(_mm_cmpgt_epi64(zero, a), _mm_slli_epi64(b, 32)));
        r = _mm_sub_epi64(r, _mm_and_si128(_mm_cmpgt_epi64(zero, b), _mm_slli_epi64(a, 32)));
        return r;
    }
};

using L = LanesSse42;
#include "vm_ops.inl"

constexpr Kernels kKernels = makeKernels("sse42");

} // namespace

const Kernels* const kSse42Kernels = &kKernels;

} // namespace helios::pcg::vm

#else

namespace helios::pcg::vm {
const Kernels* const kSse42Kernels = nullptr;
} // namespace helios::pcg::vm

#endif
