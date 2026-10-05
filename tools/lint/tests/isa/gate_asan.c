/* ISA-audit fixture (02 §1.1 audit check 2, seeded failure): a gate-style object built with
 * AddressSanitizer, so it references __asan_* runtime symbols. lint_isa_object_detects_asan must reject
 * it: the gate runs before any sanitizer runtime is initialized. */
int helios_isa_fixture_load(const int* p, int i);

int helios_isa_fixture_load(const int* p, int i) {
    return p[i];
}
