# engine/hxl — L2 HEADLESS: the HXL formula language (06 §1.2): compiler, verified bytecode and VM.
# The Go twin is services/pkg/hxl; both run the shared corpus tests/corpus/hxl (GP-1).

helios_module(hxl HEADLESS
  SOURCES
    src/compiler.cpp
    src/program.cpp
    src/syntax.cpp
    src/vm.cpp
  DEPS helios::core helios::math)

# 06 §1.2 cross-language float rules: never contract a*b+c into an FMA (src/fp_control.h adds
# pragmas as a second line of defence). clang-cl maps /fp:precise to contract=on, so it gets the
# option explicitly; MSVC's /fp:precise never contracts.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(helios_hxl PRIVATE /clang:-ffp-contract=off)
elseif(NOT MSVC)
  target_compile_options(helios_hxl PRIVATE -ffp-contract=off)
endif()

helios_test(hxl_tests
  SOURCES
    tests/main.cpp
    tests/corpus.cpp
    tests/test_bytecode.cpp
    tests/test_compiler.cpp
    tests/test_corpus.cpp
    tests/test_vm.cpp
  DEPS helios::hxl helios::tp::yyjson)
if(TARGET hxl_tests)
  target_compile_definitions(hxl_tests PRIVATE HELIOS_HXL_CORPUS_DIR="${PROJECT_SOURCE_DIR}/tests/corpus/hxl")
  # The tests compute reference values with a*b+c too: same contraction rule as the library
  # (clang-cl included, whose /fp:precise means contract=on).
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(hxl_tests PRIVATE /clang:-ffp-contract=off)
  elseif(NOT MSVC)
    target_compile_options(hxl_tests PRIVATE -ffp-contract=off)
  endif()
endif()
