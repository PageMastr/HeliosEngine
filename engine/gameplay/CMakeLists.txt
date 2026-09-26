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
# The Go types are generated into the services module so Go services and `go test` use them
# without a C++ build; the files only change when the schemas do.
helios_schema(helios_gameplay
  FILES ${HELIOS_GAMEPLAY_SCHEMAS}
  INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/schemas
  LOCK ${PROJECT_SOURCE_DIR}/schemas/gameplay/schema.lock.jsonc
  GO_OUT ${PROJECT_SOURCE_DIR}/services/pkg/hxl/gamedef
  GO_PACKAGE gamedef)

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
# The tests compute reference values with a*b+c too: same contraction rule as the library
# (clang-cl included, whose /fp:precise means contract=on).
if(TARGET gameplay_tests)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(gameplay_tests PRIVATE /clang:-ffp-contract=off)
  elseif(NOT MSVC)
    target_compile_options(gameplay_tests PRIVATE -ffp-contract=off)
  endif()
endif()
