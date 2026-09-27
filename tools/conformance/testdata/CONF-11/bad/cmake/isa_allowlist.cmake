# Verbatim from cmake/isa_allowlist.cmake at WP-0.2 (main 1425608), lines 1-14: the per-target and
# per-file AVX2 lists that CONF-11 exists to catch (09 §2 WP-0.2 acceptance).
# ISA allowlist (02 §1.1 "ISA levels and the pre-gate audit", WP-0.2, RT-09).
#
# Read by tools/lint/isa_audit.cmake (CTest `lint_isa_audit`). Adding an entry is a reviewed
# change: every allowlisted unit may only run after the CPU gate has confirmed AVX2 support.

# Targets whose every translation unit may be compiled with AVX/AVX2/BMI/F16C/LZCNT flags. Jolt's
# headers select AVX paths inline (JPH_USE_AVX2), so the whole library is built at that level, and so
# is helios_physics, whose sources include those headers and inherit tp_jolt's PUBLIC flags. Interim:
# WP-0.2r replaces this list with whole-image ISA levels (09 §5.10.4).
set(HELIOS_ISA_AVX2_TARGETS tp_jolt helios_physics)

# Designated AVX2 kernel files (added with helios_avx2_sources(), cmake/HeliosIsa.cmake): pcg's
# vm_avx2.cpp and any other CPUID-dispatched kernel. Regular expressions on the source path.
set(HELIOS_ISA_AVX2_SOURCE_PATTERNS "_avx2\\.(c|cc|cpp)$")
