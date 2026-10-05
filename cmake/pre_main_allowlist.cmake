# Attributed pre-main hooks (02 §1.1 "The audit", check 3 (c); WP-0.2r).
#
# Code that runs before main() in an avx2 image, other than the CRT's own and the CPU gate. Every entry
# runs after the gate, so its ISA level is free; the list makes a dependency update that adds a pre-main
# hook, or moves one ahead of the gate, fail review instead of passing silently. Audit check 3 (the
# Windows TLS-callback and .CRT$X* enumeration, `helios-tool isa-audit --pre-gate`, WP-0.2r part 2) fails
# an entry that is neither CRT-owned nor listed here. Each item is "<section>|<symbol>|<owner>|<builds>":
#   section  the section the entry is contributed to (`*` in a section name matches any suffix);
#   symbol   the contributing symbol (`*` for every dynamic initializer of the owner's objects);
#   owner    the library or module whose objects contribute it;
#   builds   `all`, or the option that must be ON for the entry to exist.
set(HELIOS_PRE_MAIN_HOOKS
  # mimalloc in its default MI_WIN_INIT_USE_CRT_TLS mode (third_party/mimalloc/src/prim/windows/prim.c).
  ".CRT$XLB|_mi_tls_callback_pre|tp_mimalloc|all"
  ".CRT$XLY|_mi_tls_callback_post|tp_mimalloc|all"
  ".CRT$XIB|_mi_crt_callback_init|tp_mimalloc|all"
  # Tracy (third_party/tracy/public/client/TracyProfiler.cpp): its init_seg(".CRT$XCB") statics and its
  # dynamic thread_locals (s_token, s_token_detail, s_threadHandle, s_gpuCtx), initialized by the CRT's
  # __dyn_tls_init TLS callback.
  ".CRT$XCB|*|tp_tracy|HELIOS_PROFILE"
  ".CRT$XD*|*|tp_tracy|HELIOS_PROFILE"
  # Ordinary dynamic initializers of Helios modules.
  ".CRT$XCU|*|helios_*|all")

# The ELF gate hook's static function, the single .preinit_array entry of every gated ELF image
# (engine/core/src/platform/posix/cpu_gate_hook.c; audit check 3 verifies it today).
set(HELIOS_PRE_MAIN_GATE_PREINIT_SYMBOL hcg_gate)
