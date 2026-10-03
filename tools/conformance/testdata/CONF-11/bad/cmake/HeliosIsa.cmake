# Verbatim from cmake/HeliosIsa.cmake at WP-0.2 (main 1425608; unchanged at c2cbff5): helios_isa_avx2_flags() (lines 33-43)
# and helios_avx2_sources() (lines 58-70).
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
