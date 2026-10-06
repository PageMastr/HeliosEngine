# Try-compile probes build no target, x86-64-v1 is the baseline, and text that names a flag is not a grant.
set(CMAKE_REQUIRED_FLAGS -mavx2)
target_compile_options(helios_gate PRIVATE -march=x86-64-v1)
function(_note text)
  message(STATUS "${text}")
endfunction()
_note("${unit}: -march=native (non-reproducible binaries)")
message(STATUS "kernels (ISA (avx2) are gone")
# A test table whose quoted items name flags in expected messages: not options, so not flags.
foreach(case "march_native|-march=native" "fma|FMA enabled (-mfma)")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  add_test(NAME lint_${fixture} COMMAND check ${fixture})
endforeach()
# A loop variable is restored when the loop ends (CMP0124): after it, _o holds no flags.
foreach(_o -mavx2)
  message(STATUS "${_o}")
endforeach()
target_compile_options(helios_gate PRIVATE ${_o})
