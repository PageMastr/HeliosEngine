# The gate hook uses .preinit_array on ELF (a comment, not code).
target_link_options(helios_core INTERFACE $<$<PLATFORM_ID:Windows>:/INCLUDE:helios_cpu_gate_tls_entry>)
