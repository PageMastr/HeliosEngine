# Round 8: Clang's AVX implications (-mvaes, -mvpclmulqdq and -msm3 imply AVX, -msha512 and -msm4 AVX2), a
# target feature handed to the front end (also inside SHELL:), $ENV{} in the environment's CXXFLAGS, and
# $ENV{} in link options, which LTO compiles with.
target_compile_options(tp_volk PRIVATE -mvaes)
target_compile_options(tp_volk PRIVATE -mvpclmulqdq)
target_compile_options(tp_volk PRIVATE -msm3)
target_compile_options(tp_volk PRIVATE -msha512)
target_compile_options(tp_volk PRIVATE -msm4)
target_compile_options(tp_volk PRIVATE -Xclang -target-feature -Xclang +avx2)
target_compile_options(tp_volk PRIVATE "SHELL:-Xclang -target-feature -Xclang +avx2")
set(ENV{CXXFLAGS} "$ENV{CXXFLAGS} $ENV{HELIOS_SIMD}")
target_link_options(tp_volk PRIVATE $ENV{HELIOS_LINK})
add_link_options($ENV{HELIOS_LINK})
set_property(TARGET tp_volk APPEND PROPERTY LINK_OPTIONS $ENV{HELIOS_LINK})
target_compile_options(tp_volk PRIVATE -march=$ENV{HELIOS_ARCH})
_tp_wrap(tp_volk -march=$ENV{HELIOS_ARCH})
# Round 9: a quoted SHELL: group passed to a wrapper or given as a foreach item, and a non-AVX target feature.
_tp_wrap(tp_volk "SHELL:-Xclang -target-feature -Xclang +avx2")
_tp_wrap(tp_volk "SHELL:-mfma")
foreach(_tp_s "SHELL:-mavx2")
  target_compile_options(tp_volk PRIVATE ${_tp_s})
endforeach()
target_compile_options(tp_volk PRIVATE -Xclang -target-feature -Xclang +fma)
target_compile_options(tp_volk PRIVATE "SHELL:-Xclang -target-feature -Xclang +sha512")
