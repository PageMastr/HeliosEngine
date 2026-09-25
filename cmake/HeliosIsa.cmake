# ISA levels, AVX2 kernels and the CPU gate (02 §1.1 "ISA levels and the pre-gate audit", 08 §2.2).
#
# Today every Helios image is compiled for the compiler's x86-64 default (SSE2) except the entries on
# the ISA allowlist (cmake/isa_allowlist.cmake): the `tp_jolt` library and designated `*_avx2.c(pp)`
# kernel files. The CPU gate TUs (engine/core/src/cpugate/*.c) are pinned to the x86-64-v1 baseline
# with explicit per-file flags, so they stay AVX-free even if their target is ever built at `avx2`.
# The audit (tools/lint/isa_audit.cmake, CTest `lint_isa_audit`) checks all of this on every build.
#
# helios_avx2_sources(<target> <file>...)
#   Adds designated AVX2 kernel sources to <target> and compiles only them with HELIOS_ISA_AVX2_FLAGS.
#   File names must end in _avx2.c or _avx2.cpp (the allowlist matches on that). Callers must
#   dispatch to these kernels only after cpuGate() / CPUID confirmed AVX2 support.
#
# helios_isa_base_sources(<file>...)
#   Pins files to the x86-64-v1 baseline (HELIOS_ISA_BASE_FLAGS), overriding target-level ISA flags.
#
# helios_cpu_gate_sources(<file>...)
#   The CPU gate's C units (probe and pre-initializer hooks): the x86-64-v1 baseline plus the gate
#   object rules of 02 §1.1 — no stack protector (/GS-, -fno-stack-protector) and no sanitizer
#   instrumentation, because the hooks run before the runtimes those need. The ISA audit rejects
#   any import outside HELIOS_ISA_GATE_ALLOWED_IMPORTS (so __stack_chk_*, __security_cookie,
#   __asan_*, __ubsan_* references) in these objects.
#
# helios_cpu_gate(<target>)
#   Links the CPU-gate pre-initializer (engine/core/src/cpugate/cpu_gate_hook.c) into an executable:
#   it runs before any C++ initializer (.preinit_array on ELF, .CRT$XIB on Windows), prints the
#   "requires an AVX2 CPU" message and exits with code 78 on unsupported CPUs.

include_guard(GLOBAL)

set(HELIOS_CPU_GATE_EXIT_CODE 78)

# Flags for the `avx2` level. No FMA anywhere (determinism, 02 §7.1) and no FP contraction.
function(helios_isa_avx2_flags out)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(flags /arch:AVX2 /fp:precise)
  elseif(MSVC) # clang-cl
    set(flags /arch:AVX2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma /clang:-ffp-contract=off)
  else()
    set(flags -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma -mfpmath=sse -ffp-contract=off)
  endif()
  set(${out} "${flags}" PARENT_SCOPE)
endfunction()

# Flags for the x86-64-v1 baseline of the CPU gate (08 §2.1.1). MSVC x64 has no switch below its
# SSE2 default, so nothing is added there; the audit relies on no /arch flag reaching the gate TU.
function(helios_isa_base_flags out)
  if(MSVC)
    set(flags "")
  else()
    set(flags -march=x86-64 -mtune=generic -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-popcnt
              -mno-avx -mno-avx2 -mno-fma -mno-bmi -mno-bmi2 -mno-lzcnt -mno-f16c -mno-movbe -mno-avx512f
              -mno-cx16)
  endif()
  set(${out} "${flags}" PARENT_SCOPE)
endfunction()

function(helios_avx2_sources target)
  helios_isa_avx2_flags(flags)
  foreach(src IN LISTS ARGN)
    if(NOT src MATCHES "_avx2\\.(c|cpp)$")
      message(FATAL_ERROR "helios_avx2_sources(${target}): '${src}' must be named *_avx2.c or *_avx2.cpp "
                          "(the ISA allowlist matches on the name, cmake/isa_allowlist.cmake)")
    endif()
    target_sources(${target} PRIVATE ${src})
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
      set_source_files_properties(${src} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "${flags}")
    endif()
  endforeach()
endfunction()

function(helios_isa_base_sources)
  helios_isa_base_flags(flags)
  if(flags AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "${flags}")
  endif()
endfunction()

function(helios_cpu_gate_sources)
  set(flags "")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    helios_isa_base_flags(flags)
  endif()
  if(MSVC)
    list(APPEND flags /GS-)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
      list(APPEND flags -fno-sanitize=all)
    endif()
  else()
    list(APPEND flags -fno-stack-protector -fno-sanitize=all)
  endif()
  if(flags)
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "${flags}")
  endif()
endfunction()

function(helios_cpu_gate target)
  if(NOT TARGET helios_core_cpugate_hook)
    message(FATAL_ERROR "helios_cpu_gate(${target}): engine/core must be configured first "
                        "(target helios_core_cpugate_hook is missing)")
  endif()
  # An object library links its object file unconditionally; an archive member holding only an
  # initializer would be dropped by the linker.
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:helios_core_cpugate_hook>)
  target_link_libraries(${target} PRIVATE helios::core)
  set_target_properties(${target} PROPERTIES HELIOS_CPU_GATE ON)
  set_property(GLOBAL APPEND PROPERTY HELIOS_CPU_GATE_TARGETS ${target})
endfunction()
