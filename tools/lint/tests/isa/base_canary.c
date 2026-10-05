/* ISA-audit fixture (02 §1.1 audit check 4, seeded failure): a `base` image whose TUs carry only the base
 * flags, so check 1 passes, but one function asks for AVX2 through a target attribute, which no flag
 * shows. lint_isa_base_image_detects_avx2 must reject the linked image and name that symbol (and only
 * it: main is x86-64-v1). Not built where check 4 cannot run (MSVC, COFF). */
#include <immintrin.h>

__attribute__((target("avx2"), noinline)) int helios_isa_canary_avx2(const int* p);

__attribute__((target("avx2"), noinline)) int helios_isa_canary_avx2(const int* p) {
    __m256i v = _mm256_loadu_si256((const __m256i*)p);
    v = _mm256_add_epi32(v, v);
    return _mm256_extract_epi32(v, 3);
}

int main(int argc, char** argv) {
    (void)argv;
    int lanes[8] = {argc, 1, 2, 3, 4, 5, 6, 7};
    /* Reached through a volatile pointer, so the call cannot be folded away. */
    int (*volatile fn)(const int*) = helios_isa_canary_avx2;
    return fn(lanes) == 6 ? 0 : 1;
}
