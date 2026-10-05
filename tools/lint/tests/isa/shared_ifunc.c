/* ISA-audit fixture (ELF): a shared library with an IFUNC. The resolver of a locally bound IFUNC runs when
 * the dynamic linker applies its R_X86_64_IRELATIVE relocation, which happens for every library of a
 * process's start-up set before the executable's .preinit_array, and so before the CPU gate.
 * lint_isa_shared_image_detects_ifunc must reject it, as lint_isa_audit rejects such a relocation in the
 * link-group libraries of a modular dev build (ADR-0.6c §3 item 6). Never loaded. The IFUNC and its
 * resolver are hidden, not static: Clang drops a static resolver as unused. */
static int lint_isa_ifunc_impl(void) { return 1; }

typedef int (*lint_isa_ifunc_fn)(void);
lint_isa_ifunc_fn lint_isa_ifunc_resolve(void) __attribute__((visibility("hidden")));
lint_isa_ifunc_fn lint_isa_ifunc_resolve(void) { return lint_isa_ifunc_impl; }

int lint_isa_ifunc_picked(void) __attribute__((ifunc("lint_isa_ifunc_resolve"), visibility("hidden")));

int lint_isa_fixture_ifunc_call(void);
int lint_isa_fixture_ifunc_call(void) { return lint_isa_ifunc_picked(); }
