/* ISA-audit fixture: a designated AVX2 kernel (allowlisted by its *_avx2.c name). The test
 * lint_isa_disasm_detects_avx audits its object as if it were a baseline unit and expects the
 * disassembly check to reject the VEX-encoded instructions below. */
#include <immintrin.h>

void helios_lint_fixture_add8(float* dst, const float* a, const float* b);

void helios_lint_fixture_add8(float* dst, const float* a, const float* b) {
    _mm256_storeu_ps(dst, _mm256_add_ps(_mm256_loadu_ps(a), _mm256_loadu_ps(b)));
}
