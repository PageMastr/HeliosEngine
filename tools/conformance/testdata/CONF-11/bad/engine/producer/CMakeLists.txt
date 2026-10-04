# A function or macro that hands flags back through an output argument and also grants its other arguments:
# the options its callers pass are grants, a literal one or one from $ENV{} (round 8).
function(_tp_simd out)
  set(${out} -mavx2 PARENT_SCOPE)
  target_compile_options(tp_volk PRIVATE ${ARGN})
endfunction()
_tp_simd(_tp_f -mfma)
_tp_simd(_tp_g $ENV{HELIOS_SIMD})
macro(_tp_simd_m out)
  set(${out} -mavx2)
  target_compile_options(tp_volk PRIVATE ${ARGN})
endmacro()
_tp_simd_m(_tp_h -mfma)
