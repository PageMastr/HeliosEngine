/* ISA-audit fixture (02 §1.1 audit check 2, seeded failure): a gate-style object built with a stack
 * protector, so it references __stack_chk_fail. lint_isa_object_detects_stack_protector must reject it:
 * the gate runs before the CRT sets the canary up (the /GS cookie on Windows). */
int helios_isa_fixture_guarded(const char* text);

int helios_isa_fixture_guarded(const char* text) {
    char buffer[64];
    int n = 0;
    while (n < 63 && text[n]) {
        buffer[n] = text[n];
        ++n;
    }
    buffer[n] = 0;
    return buffer[n / 2];
}
