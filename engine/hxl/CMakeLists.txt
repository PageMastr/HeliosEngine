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

# Stack budget of the parser and code generator (README, "Safety and cost"): the deepest programs the
# limits admit must compile and run on a 256 KiB stack. The measured need is <= 72 KiB at -O2 and
# <= 160 KiB at -O0, plus the test runner; before WP-0.19's review the same programs needed up to
# 512 KiB. `ulimit -s` bounds the main thread, which runs doctest's cases. Sanitizer frames are
# several times larger (Clang ASan -O0 needs ~512 KiB), so those builds check against 1 MiB. The pass
# expression also fails the test if the filter stops matching exactly one test case. Linux and macOS
# only: Windows has no per-process soft stack limit to lower.
if(TARGET hxl_tests AND UNIX)
  if(HELIOS_SANITIZE)
    set(_hxlStackKiB 1024)
  else()
    set(_hxlStackKiB 256)
  endif()
  add_test(NAME hxl_tests_stack
    COMMAND sh -c "ulimit -s ${_hxlStackKiB} && exec \"$1\" \"--test-case=*deepest programs*\"" sh $<TARGET_FILE:hxl_tests>)
  set_tests_properties(hxl_tests_stack PROPERTIES
    PASS_REGULAR_EXPRESSION "test cases: +1 \\| +1 passed \\| +0 failed")
  unset(_hxlStackKiB)
endif()
