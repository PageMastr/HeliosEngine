# ISA audit lists (02 §1.1 "ISA levels and the pre-gate audit"; WP-0.2r; RT-09).
#
# Read by tools/lint/isa_audit.cmake (CTest `lint_isa_audit`). There is no per-target or per-file ISA
# list: images are built at one level (cmake/HeliosIsa.cmake). What is left are the gate's export and
# import lists (audit check 2) and the self-dispatching functions that base images may contain (check 4).
# Adding an entry is a reviewed change. The attributed pre-main hooks are in cmake/pre_main_allowlist.cmake.

# External symbols a gate object may define (everything else must be static). WP-0.5r moves the Windows
# entry to .CRT$XLA0 and changes these to helios_cpu_gate_run, helios_cpu_gate_verdict and
# helios_cpu_gate_tls_entry (02 §1.1).
set(HELIOS_ISA_GATE_EXPORTS helios_cpu_gate_run helios_cpu_gate_crt_entry)

# Undefined symbols a gate object may reference: the probe entry, the OS entry points of 02 §1.1
# (POSIX: write, _exit, sigaction; Windows: GetStdHandle, WriteFile, LoadLibraryExW, GetProcAddress,
# ExitProcess, AddVectoredExceptionHandler, via __imp_ import thunks) and the compiler's stack-probe
# helper. The gate is built without a stack protector and without sanitizers (the `gate` level), so no
# __stack_chk_* or __security_cookie reference is allowed (02 §1.1: the /GS cookie is not initialized
# before the entry point), and no __asan_*, __ubsan_* or other sanitizer runtime symbol either.
set(HELIOS_ISA_GATE_ALLOWED_IMPORTS
  helios_cpu_gate_run
  write _exit sigaction
  GetStdHandle WriteFile LoadLibraryExW GetProcAddress ExitProcess AddVectoredExceptionHandler
  __imp_GetStdHandle __imp_WriteFile __imp_LoadLibraryExW __imp_GetProcAddress __imp_ExitProcess
  __imp_AddVectoredExceptionHandler
  __chkstk __chkstk_ms ___chkstk_ms)

# Self-dispatching third-party functions that may contain instructions above x86-64-v1 in `base` images,
# because they check CPUID themselves before running them (audit check 4, 02 §1.1 "Self-dispatching
# third-party code"). Matched by symbol, with compiler clone suffixes (.constprop.0, .isra.0, .part.0,
# .cold) ignored. zstd is the known case on the launcher's patch path (08 §2): with DYNAMIC_BMI2 (every
# x86-64 build without -mbmi2) these functions carry BMI2_TARGET_ATTRIBUTE ("lzcnt,bmi,bmi2") and are
# called only when ZSTD_cpuid() reported BMI2 (the bmi2 argument or HUF_flags_bmi2). BLAKE2b's SIMD
# compressors and SDL3's blitters join when the launcher links them.
set(HELIOS_ISA_SELF_DISPATCH_SYMBOLS
  # Decompression (lib/decompress, lib/common).
  HUF_decompress4X1_usingDTable_internal_bmi2
  HUF_decompress4X2_usingDTable_internal_bmi2
  HUF_decompress1X1_usingDTable_internal_bmi2
  HUF_decompress1X2_usingDTable_internal_bmi2
  HUF_decompress4X1_usingDTable_internal_fast
  HUF_decompress4X2_usingDTable_internal_fast
  HUF_decompress4X1_usingDTable_internal_fast_c_loop
  HUF_decompress4X2_usingDTable_internal_fast_c_loop
  HUF_readStats_body_bmi2
  FSE_readNCount_body_bmi2
  FSE_decompress_wksp_body_bmi2
  ZSTD_buildFSETable_body_bmi2
  ZSTD_decompressSequences_bmi2
  ZSTD_decompressSequencesLong_bmi2
  ZSTD_decompressSequencesSplitLitBuffer_bmi2
  # Compression (lib/compress): a base image links all of zstd's objects.
  HUF_compress1X_usingCTable_internal_bmi2
  ZSTD_encodeSequences_bmi2)
