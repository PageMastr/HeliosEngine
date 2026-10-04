# What stays after WP-0.2r: self-dispatch symbols and the gate's export and import lists (02 §1.1).
# (No HELIOS_ISA_AVX2_TARGETS any more; a comment may still name it.)
set(HELIOS_ISA_SELF_DISPATCH_SYMBOLS blake2b_compress_avx2 SDL_HasAVX2)
set(HELIOS_ISA_GATE_EXPORTS helios_cpu_gate_run helios_cpu_gate_verdict helios_cpu_gate_tls_entry)
