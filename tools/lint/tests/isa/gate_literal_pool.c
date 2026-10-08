/* ISA-audit fixture (02 §1.1 audit check 2, seeded failure): a gate-style unit with a string literal.
 * Compiled for the MSVC ABI with clang in cl mode (tools/lint/lint_tests.cmake), the literal becomes an
 * external "pick any" COMDAT (??_C@...), so lint_isa_coff_gate_detects_literal_pool must reject the object
 * even though its only function is one of the gate's three exports. */
void helios_cpu_gate_run(const char** out);

void helios_cpu_gate_run(const char** out) {
    *out = "requires an AVX2 CPU";
}
