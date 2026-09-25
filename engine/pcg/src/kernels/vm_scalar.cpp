// The width-1 terrain VM kernel: the reference build of vm_ops.inl and the template for a later NEON
// port (02 §5.8). Plain integer C++; unsigned arithmetic wherever two's complement wrapping is meant.
#include "vm_kernels.h"

namespace helios::pcg::vm {
namespace {

struct LanesScalar {
    static constexpr u32 kWidth = 1;
    static constexpr u32 kQWidth = 1;
    using V = i32;
    using Q = i64;

    static V set1(i32 x) { return x; }
    static V load(const i32* p) { return *p; }
    static V loadU(const u32* p) { return static_cast<i32>(*p); }
    static void store(i32* p, V v) { *p = v; }
    static void storeU(u32* p, V v) { *p = static_cast<u32>(v); }
    static V add(V a, V b) { return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b)); }
    static V sub(V a, V b) { return static_cast<i32>(static_cast<u32>(a) - static_cast<u32>(b)); }
    static V mullo(V a, V b) { return static_cast<i32>(static_cast<u32>(a) * static_cast<u32>(b)); }
    static V band(V a, V b) { return a & b; }
    static V bor(V a, V b) { return a | b; }
    static V bxor(V a, V b) { return a ^ b; }
    template <int N>
    static V slli(V a) {
        return static_cast<i32>(static_cast<u32>(a) << N);
    }
    template <int N>
    static V srli(V a) {
        return static_cast<i32>(static_cast<u32>(a) >> N);
    }
    template <int N>
    static V srai(V a) {
        return a >> N;
    }
    static V sll(V a, i32 n) { return static_cast<i32>(static_cast<u32>(a) << n); }
    static V srl(V a, i32 n) { return static_cast<i32>(static_cast<u32>(a) >> n); }
    static V sra(V a, i32 n) { return a >> n; }
    static V cmpgt(V a, V b) { return a > b ? -1 : 0; }
    static V cmpeq(V a, V b) { return a == b ? -1 : 0; }
    static V select(V mask, V ifTrue, V ifFalse) { return (ifTrue & mask) | (ifFalse & ~mask); }
    static V abs(V a) { return a < 0 ? static_cast<i32>(0u - static_cast<u32>(a)) : a; }
    static V max(V a, V b) { return a > b ? a : b; }
    static bool uniform(V v, i32& out) {
        out = v;
        return true;
    }

    static V mulQ16(V a, V b) { return static_cast<i32>((static_cast<i64>(a) * b + (i64(1) << 15)) >> 16); }
    static V fade(V t) {
        const u32 tu = static_cast<u32>(t);
        const u32 t2 = tu * tu;
        const u32 t3 = static_cast<u32>((static_cast<u64>(t2) * tu) >> 24);
        const i64 b = (i64(10) << 32) + static_cast<i64>(t) * (6 * t - 15 * 65536);
        return static_cast<i32>((static_cast<u64>(t3) * static_cast<u64>(b) + (u64(1) << 39)) >> 40);
    }
    static V mulQ30(V a, V b) { return static_cast<i32>((static_cast<i64>(a) * b + (i64(1) << 29)) >> 30); }
    static V mulQ30u(V a, V b) {
        return static_cast<i32>(static_cast<u32>(
            (static_cast<u64>(static_cast<u32>(a)) * static_cast<u32>(b) + (u64(1) << 29)) >> 30));
    }
    static V mulQ31u(V a, V b) {
        return static_cast<i32>(static_cast<u32>(
            (static_cast<u64>(static_cast<u32>(a)) * static_cast<u32>(b) + (u64(1) << 30)) >> 31));
    }
    static void mulWide(V a, V b, V& lo, V& hi) {
        const u64 p = static_cast<u64>(static_cast<i64>(a) * b);
        lo = static_cast<i32>(static_cast<u32>(p));
        hi = static_cast<i32>(static_cast<u32>(p >> 32));
    }
    static void mulWideU(V a, V b, V& lo, V& hi) {
        const u64 p = static_cast<u64>(static_cast<u32>(a)) * static_cast<u32>(b);
        lo = static_cast<i32>(static_cast<u32>(p));
        hi = static_cast<i32>(static_cast<u32>(p >> 32));
    }

    struct Acc {
        u64 v;
    };
    static Acc accZero() { return {0}; }
    static void accMulAdd(Acc& acc, V a, V b) { acc.v += static_cast<u64>(static_cast<i64>(a) * b); }
    static void accStore(i64* out, const Acc& acc) { *out = static_cast<i64>(acc.v); }

    static Q loadQ(const i64* p) { return *p; }
    static void storeQ(i64* p, Q v) { *p = v; }
    static Q set1Q(i64 v) { return v; }
    static Q addQ(Q a, Q b) { return static_cast<i64>(static_cast<u64>(a) + static_cast<u64>(b)); }
    static Q subQ(Q a, Q b) { return static_cast<i64>(static_cast<u64>(a) - static_cast<u64>(b)); }
    static Q minQ(Q a, Q b) { return a < b ? a : b; }
    static Q maxQ(Q a, Q b) { return a > b ? a : b; }
    static Q selectGeQ(Q cond, Q threshold, Q ifGe, Q ifLt) { return cond >= threshold ? ifGe : ifLt; }
    /// Q32.32 product: bits 32..95 of a*b + 2^31 (helios::Q32 rounding), from 32-bit partial products.
    static Q mulQ32(Q a, Q b) {
        const u64 ua = static_cast<u64>(a), ub = static_cast<u64>(b);
        const u64 al = ua & 0xFFFFFFFFu, ah = ua >> 32, bl = ub & 0xFFFFFFFFu, bh = ub >> 32;
        u64 r = ((ah * bh) << 32) + ah * bl + al * bh + ((al * bl + (u64(1) << 31)) >> 32);
        if (a < 0) r -= ub << 32;
        if (b < 0) r -= ua << 32;
        return static_cast<i64>(r);
    }
};

using L = LanesScalar;
#include "vm_ops.inl"

constexpr Kernels kKernels = makeKernels("scalar");

} // namespace

const Kernels* const kScalarKernels = &kKernels;

} // namespace helios::pcg::vm
