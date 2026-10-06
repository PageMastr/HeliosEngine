# After WP-0.2r: 02 §1.1's level sets and the one function that applies a level to an image.
set(HELIOS_ISA_AVX2 -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma -mfpmath=sse -ffp-contract=off)
function(helios_apply_isa_level target level)
  target_compile_options(${target} PRIVATE ${HELIOS_ISA_AVX2})
endfunction()
function(helios_isa_flags_for level out)
  set(${out} ${HELIOS_ISA_AVX2} PARENT_SCOPE)
endfunction()
# A per-target list whose name has no "avx", granted outside the level function.
set(HELIOS_FAST_TARGETS helios_pcg helios_physics)
foreach(t IN LISTS HELIOS_FAST_TARGETS)
  target_compile_options(${t} PRIVATE ${HELIOS_ISA_AVX2})
endforeach()
