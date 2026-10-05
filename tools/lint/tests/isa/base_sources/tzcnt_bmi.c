/* ISA-audit fixture (MODE=base_sources, seeded failure; never compiled): the forms of TZCNT that audit check 4
 * cannot see, because it reads TZCNT's encoding as BSF. lint_isa_base_sources_detects_tzcnt must name lines 7,
 * 9, 10 and 15 and nothing else: not the comment below, __builtin_ctz or an SSE4.1 target. */
#include <immintrin.h>

/* _tzcnt_u32 in a comment is not code. */
#pragma GCC target("bmi")

__attribute__((target("bmi"), noinline)) unsigned helios_isa_canary_tzcnt(unsigned x) {
    return _tzcnt_u32(x); /* 32 for x == 0 with BMI1; undefined (BSF) without it */
}

static unsigned helios_isa_canary_asm(unsigned x) {
    unsigned r;
    __asm__("tzcnt %1, %0" : "=r"(r) : "r"(x));
    return r;
}

__attribute__((target("sse4.1"))) static unsigned helios_isa_canary_ctz(unsigned x) {
    return x ? (unsigned)__builtin_ctz(x) : 32u; // the compiler's own idiom, safe for x == 0
}
