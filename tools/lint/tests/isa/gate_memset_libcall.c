/* ISA-audit fixture (02 §1.1 audit check 2, seeded failure): a gate-style unit with a byte-zeroing loop of
 * unknown length. Compiled for the MSVC ABI with clang in cl mode and the optimization flags of the MSVC-family
 * presets (tools/lint/lint_tests.cmake), LLVM's loop-idiom pass turns the loop into a call to memset, an
 * import from the CRT that does not exist yet when the gate runs. lint_isa_coff_gate_detects_memset_libcall
 * must therefore reject the object. It guards the test command itself: with -ffreestanding (-fno-builtin)
 * LLVM keeps the loop, the object comes out clean, and the test fails because it no longer finds the import. */
void helios_cpu_gate_run(unsigned char* buf, unsigned n);

void helios_cpu_gate_run(unsigned char* buf, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        buf[i] = 0;
    }
}
