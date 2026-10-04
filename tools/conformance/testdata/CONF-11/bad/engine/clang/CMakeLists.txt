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
