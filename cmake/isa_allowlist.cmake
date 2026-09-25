# ISA allowlist (02 §1.1 "ISA levels and the pre-gate audit", WP-0.2, RT-09).
#
# Read by tools/lint/isa_audit.cmake (CTest `lint_isa_audit`). Adding an entry is a reviewed
# change: every allowlisted unit may only run after the CPU gate has confirmed AVX2 support.

# Targets whose every translation unit may be compiled with AVX/AVX2/BMI/F16C/LZCNT flags. Jolt's
# headers select AVX paths inline (JPH_USE_AVX2), so the whole library is built at that level.
set(HELIOS_ISA_AVX2_TARGETS tp_jolt)

# Designated AVX2 kernel files (added with helios_avx2_sources(), cmake/HeliosIsa.cmake): pcg's
# vm_avx2.cpp and any other CPUID-dispatched kernel. Regular expressions on the source path.
set(HELIOS_ISA_AVX2_SOURCE_PATTERNS "_avx2\\.(c|cc|cpp)$")

# Units that must stay at the x86-64-v1 baseline: the CPU gate probe and its pre-initializer hooks.
# The audit checks their flags, and (GNU binutils) their disassembly and symbol tables.
set(HELIOS_ISA_BASE_SOURCE_PATTERNS
  "/engine/core/src/cpugate/[^/]+\\.c$"
  "/engine/core/src/platform/(posix|win32)/cpu_gate_hook\\.c$")

# The function the ELF gate hook puts in .preinit_array (static; audit check 3 verifies that the
# image's single .preinit_array entry points at it).
set(HELIOS_ISA_GATE_PREINIT_SYMBOL hcg_gate)

# External symbols a gate object may define (everything else must be static).
set(HELIOS_ISA_GATE_EXPORTS helios_cpu_gate_run helios_cpu_gate_crt_entry)

# Undefined symbols a gate object may reference: the probe entry, the OS entry points of 02 §1.1
# (POSIX: write, _exit, sigaction; Windows: GetStdHandle, WriteFile, LoadLibraryExW, GetProcAddress,
# ExitProcess, AddVectoredExceptionHandler, via __imp_ import thunks) and the compiler's stack-probe
# helper. The gate is built without a stack protector (helios_cpu_gate_sources), so no
# __stack_chk_* or __security_cookie reference is allowed (02 §1.1: the /GS cookie is not
# initialized before the entry point), and no sanitizer runtime symbol either.
set(HELIOS_ISA_GATE_ALLOWED_IMPORTS
  helios_cpu_gate_run
  write _exit sigaction
  GetStdHandle WriteFile LoadLibraryExW GetProcAddress ExitProcess AddVectoredExceptionHandler
  __imp_GetStdHandle __imp_WriteFile __imp_LoadLibraryExW __imp_GetProcAddress __imp_ExitProcess
  __imp_AddVectoredExceptionHandler
  __chkstk __chkstk_ms ___chkstk_ms)

# Self-dispatching third-party functions that may contain AVX2/BMI2 code in `base` images, because
# they check CPUID themselves before running it (audit check 4, used once the launcher exists).
# zstd's DYNAMIC_BMI2 Huffman decoders are the known case (08 §2 patch path).
set(HELIOS_ISA_SELF_DISPATCH_SYMBOLS
  HUF_decompress4X1_usingDTable_internal_bmi2
  HUF_decompress4X2_usingDTable_internal_bmi2
  HUF_decompress1X1_usingDTable_internal_bmi2
  HUF_decompress1X2_usingDTable_internal_bmi2
  ZSTD_decompressSequences_bmi2
  ZSTD_decompressSequencesLong_bmi2
  ZSTD_decompressSequencesSplitLitBuffer_bmi2)
