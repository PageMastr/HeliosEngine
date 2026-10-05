/* ISA-audit fixture (02 §1.1 audit check 4, seeded failure): a `base` image whose TUs carry only the base
 * flags, so check 1 passes, but one function asks for AVX2 through a target attribute, which no flag
 * shows. lint_isa_base_image_detects_avx2 must reject the linked image and name that symbol (and only
 * it: main is x86-64-v1). Not built where check 4 cannot run (MSVC, COFF). */
#include <immintrin.h>

__attribute__((target("avx2"), noinline)) void helios_isa_canary_avx2(int* dst, const int* a, const int* b);

/* A 256-bit integer add stored to memory: GCC and Clang both keep it as a VEX-encoded vpaddd ymm. */
__attribute__((target("avx2"), noinline)) void helios_isa_canary_avx2(int* dst, const int* a, const int* b) {
    __m256i va = _mm256_loadu_si256((const __m256i*)a);
    __m256i vb = _mm256_loadu_si256((const __m256i*)b);
    _mm256_storeu_si256((__m256i*)dst, _mm256_add_epi32(va, vb));
}

int main(int argc, char** argv) {
    (void)argv;
    int a[8] = {argc, 1, 2, 3, 4, 5, 6, 7};
    int b[8] = {7, 6, 5, 4, 3, 2, 1, argc};
    int sum[8];
    /* Reached through a volatile pointer, so the call cannot be folded away. */
    void (*volatile fn)(int*, const int*, const int*) = helios_isa_canary_avx2;
    fn(sum, a, b);
    return sum[3] == 7 ? 0 : 1;
}
