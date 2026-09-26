# engine/gameplay — L4 HEADLESS gameplay kernel (06): tags (§1.1), attributes and modifiers with
# Dogma operator order and stacking penalties (§1.2), and the reason-code registry (§4). Formulas
# are HXL (engine/hxl). The kernel records are defined in schemas/gameplay/*.hschema and compiled by
# helios-schemac into C++ (here) and Go (services/pkg/hxl/gamedef).

helios_module(gameplay HEADLESS
  SOURCES
    src/attributes.cpp
    src/economy.cpp
    src/items.cpp
    src/tags.cpp
  DEPS helios::core helios::math helios::hxl helios::reflect)

# Deterministic aggregation (06 §1.2): never contract a*b+c into an FMA (src/fp_control.h adds
# pragmas as a second line of defence; MSVC's /fp:precise never contracts).
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(helios_gameplay PRIVATE /clang:-ffp-contract=off)
elseif(NOT MSVC)
  target_compile_options(helios_gameplay PRIVATE -ffp-contract=off)
endif()

include(HeliosSchema)
set(HELIOS_GAMEPLAY_SCHEMAS
  ${PROJECT_SOURCE_DIR}/schemas/gameplay/tags.hschema
  ${PROJECT_SOURCE_DIR}/schemas/gameplay/attributes.hschema
  ${PROJECT_SOURCE_DIR}/schemas/gameplay/effects.hschema
  ${PROJECT_SOURCE_DIR}/schemas/gameplay/items.hschema
  ${PROJECT_SOURCE_DIR}/schemas/gameplay/economy.hschema)
# The Go types are committed in the services module (services/pkg/hxl/gamedef), so Go services and
# `go test` use them without a C++ build (02 §3.5: generated Go is committed and CI-checked). The build
# generates them into its own directory, never into the source tree; the `lint` test
# lint_gamedef_go_current fails when the committed copy differs, and the target
# gameplay_gamedef_sync copies the generated files over it (then commit them).
set(HELIOS_GAMEDEF_GO_OUT ${CMAKE_CURRENT_BINARY_DIR}/go/gamedef)
set(HELIOS_GAMEDEF_GO_COMMITTED ${PROJECT_SOURCE_DIR}/services/pkg/hxl/gamedef)
helios_schema(helios_gameplay
  FILES ${HELIOS_GAMEPLAY_SCHEMAS}
  INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/schemas
  LOCK ${PROJECT_SOURCE_DIR}/schemas/gameplay/schema.lock.jsonc
  GO_OUT ${HELIOS_GAMEDEF_GO_OUT}
  GO_PACKAGE gamedef)
set(_gamedefRefresh "cmake --build ${CMAKE_BINARY_DIR} --target gameplay_gamedef_sync")
add_custom_target(gameplay_gamedef_sync
  COMMAND ${CMAKE_COMMAND} -DMODE=sync -DGENERATED=${HELIOS_GAMEDEF_GO_OUT}
          -DCOMMITTED=${HELIOS_GAMEDEF_GO_COMMITTED} -P ${CMAKE_CURRENT_SOURCE_DIR}/gamedef_go.cmake
  COMMENT "Copying the generated gamedef Go files to services/pkg/hxl/gamedef"
  VERBATIM)
add_dependencies(gameplay_gamedef_sync helios_gameplay)
if(HELIOS_BUILD_TESTS)
  add_test(NAME lint_gamedef_go_current
    COMMAND ${CMAKE_COMMAND} -DMODE=check -DGENERATED=${HELIOS_GAMEDEF_GO_OUT}
            -DCOMMITTED=${HELIOS_GAMEDEF_GO_COMMITTED} "-DREFRESH=${_gamedefRefresh}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/gamedef_go.cmake)
  # Seeded violation: the check must fail on a stale copy (tools/lint/expect_fail.cmake).
  add_test(NAME lint_gamedef_go_fixture_stale
    COMMAND ${CMAKE_COMMAND} "-DEXPECT=stale: .*Regenerate with" -DNARGS=7 -DARG0=${CMAKE_COMMAND}
            -DARG1=-DMODE=check -DARG2=-DGENERATED=${HELIOS_GAMEDEF_GO_OUT}
            -DARG3=-DSEED_STALE=${CMAKE_CURRENT_BINARY_DIR}/gamedef_go_stale_fixture
            "-DARG4=-DREFRESH=${_gamedefRefresh}" -DARG5=-P -DARG6=${CMAKE_CURRENT_SOURCE_DIR}/gamedef_go.cmake
            -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
  set_tests_properties(lint_gamedef_go_current lint_gamedef_go_fixture_stale PROPERTIES LABELS lint)
endif()
unset(_gamedefRefresh)

helios_test(gameplay_tests
  SOURCES
    tests/main.cpp
    tests/test_attributes.cpp
    tests/test_attributes_perf.cpp
    tests/test_economy.cpp
    tests/test_items.cpp
    tests/test_records.cpp
    tests/test_tags.cpp
  DEPS helios::gameplay)
# The 8-worker budget of GP-1 as its own CTest entry, so machines with fewer than 8 hardware threads
# (where doctest skips the case, and "test cases: 0" is printed) report it as Skipped rather than
# Passed. gameplay_tests_perf runs it too where it is not skipped.
if(TARGET gameplay_tests)
  add_test(NAME gameplay_tests_perf_8workers
    COMMAND gameplay_tests "--test-case=perf: 10k entities x 40 attributes at 5 % dirty <= 1 ms on 8 workers")
  set_tests_properties(gameplay_tests_perf_8workers PROPERTIES
    LABELS perf RUN_SERIAL TRUE SKIP_REGULAR_EXPRESSION "test cases: +0 \\|")
endif()
# The tests compute reference values with a*b+c too: same contraction rule as the library
# (clang-cl included, whose /fp:precise means contract=on).
if(TARGET gameplay_tests)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(gameplay_tests PRIVATE /clang:-ffp-contract=off)
  elseif(NOT MSVC)
    target_compile_options(gameplay_tests PRIVATE -ffp-contract=off)
  endif()
endif()
