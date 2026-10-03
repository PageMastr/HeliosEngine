# Try-compile probes build no target, x86-64-v1 is the baseline, and text that names a flag is not a grant.
set(CMAKE_REQUIRED_FLAGS -mavx2)
target_compile_options(helios_gate PRIVATE -march=x86-64-v1)
function(_note text)
  message(STATUS "${text}")
endfunction()
_note("${unit}: -march=native (non-reproducible binaries)")
message(STATUS "kernels (ISA (avx2) are gone")
