/* ISA-audit fixture (ELF): an executable whose single .preinit_array entry is not the CPU gate,
 * although a symbol named like the gate's hook exists. lint_isa_image_detects_foreign_preinit must
 * reject it ("is not the CPU gate"): a sanitizer runtime or a library that adds its own
 * pre-initializer would otherwise pass a size-only check. */
void hcg_gate(int argc, char** argv, char** envp);
void hcg_gate(int argc, char** argv, char** envp) {
    (void)argc;
    (void)argv;
    (void)envp;
}

static void foreign_preinit(int argc, char** argv, char** envp) {
    (void)argc;
    (void)argv;
    (void)envp;
}

__attribute__((section(".preinit_array"), used)) static void (*foreign_entry)(int, char**, char**) = foreign_preinit;

int main(void) { return 0; }
