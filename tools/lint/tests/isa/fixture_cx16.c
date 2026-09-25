/* ISA-audit fixture: CMPXCHG16B (not in x86-64-v1) behind a lock prefix, the only way compilers
 * emit it. lint_isa_disasm_detects_prefixed audits this object as a baseline unit and expects the
 * disassembly check to reject it: GNU objdump prints "lock cmpxchg16b" on one line (the check
 * must look past the prefix), llvm-objdump prints "lock" and "cmpxchg16b" on separate lines. */
void helios_lint_fixture_cas16(void* p);

void helios_lint_fixture_cas16(void* p) {
#if defined(__x86_64__)
    __asm__ volatile("lock cmpxchg16b (%0)" : : "r"(p) : "memory", "rax", "rbx", "rcx", "rdx");
#else
    (void)p;
#endif
}
