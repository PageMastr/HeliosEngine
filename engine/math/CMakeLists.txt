# engine/math — leaf math library (std only; must not depend on any other Helios module).
# Header-only except for determinism-critical code (noise, packing, deterministic trig,
# cube-sphere mapping, colour tables), which lives in src/ so its floating-point code generation
# is pinned in one place (see src/fp_control.h and README.md).
helios_module(math HEADLESS
  SOURCES
    src/color.cpp
    src/det_exp.cpp
    src/deterministic.cpp
    src/fixed.cpp
    src/noise.cpp
    src/pack.cpp
    src/spherical.cpp
)

# Determinism: never contract a*b+c into FMA (results would differ between MSVC clients and
# GCC/Clang servers). PUBLIC so every TU that instantiates the header-inline math agrees too
# (inline functions are shared COMDATs across TUs). helios_apply_warnings already sets it for
# GCC/Clang; MSVC's /fp:precise does not contract; clang-cl maps /fp:precise to contract=on, so it
# gets the option explicitly. src/fp_control.h adds pragmas as a second line of defence.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(helios_math PUBLIC /clang:-ffp-contract=off)
elseif(NOT MSVC)
  target_compile_options(helios_math PUBLIC -ffp-contract=off)
endif()

helios_test(math_tests
  SOURCES
    tests/main.cpp
    tests/test_scalar.cpp
    tests/test_vec.cpp
    tests/test_quat.cpp
    tests/test_mat.cpp
    tests/test_transform.cpp
    tests/test_frame.cpp
    tests/test_geometry.cpp
    tests/test_color.cpp
    tests/test_pack.cpp
    tests/test_noise.cpp
    tests/test_spherical.cpp
    tests/test_precision.cpp
    tests/test_minmax_macros.cpp
    tests/test_core_coexistence.cpp
    tests/test_det_exp.cpp
    tests/test_fixed.cpp
  DEPS helios::math
)
