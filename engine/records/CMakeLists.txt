# engine/records — L2 record database (HEADLESS; 02 §1.1, §3.3, §3.7): the `.hrec` → `.hrdb` cook
# (inheritance, reference/tag/formula checks, the client/server split and its AAA-SEC-4 rules, the tag
# table, HXL bytecode) and the zero-copy, fully validated `.hrdb` loader. See README.md.

helios_module(records HEADLESS
  SOURCES
    src/cook.cpp
    src/hrdb_format.cpp
    src/layout.cpp
    src/record_db.cpp
  DEPS helios::core helios::reflect helios::hxl)

include(HeliosSchema)

# Test record types (tests/schema/records_test.hschema): every cooked encoding, client {} / server {}
# blocks, @server_only / @client_only records, @opaque, @keyed, @merge(append). The tests, the fuzz
# target and its seeds use them; nothing shipped links them.
add_library(helios_records_testtypes STATIC)
helios_schema(helios_records_testtypes
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/records_test.hschema
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema
  LOCK ${CMAKE_CURRENT_SOURCE_DIR}/tests/schema/schema.lock.jsonc)
set_target_properties(helios_records_testtypes PROPERTIES FOLDER tests)
helios_apply_warnings(helios_records_testtypes)

# The tests also link engine/gameplay: its TagDef records declare tags (and their audiences), and the
# cooked tag table is checked against gameplay::TagRegistry's numbering. A test binary is no shipped
# image, so the upward link is allowed (as engine/asset's tests link engine/assetpipe).
file(GLOB HELIOS_RECORDS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(records_tests SOURCES ${HELIOS_RECORDS_TEST_SOURCES}
  DEPS helios::records helios_records_testtypes helios::gameplay)
if(TARGET records_tests)
  target_compile_definitions(records_tests PRIVATE
    HELIOS_RECORDS_TEST_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/project"
    HELIOS_RECORDS_TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/records_test_work")
endif()

# ---------------------------------------------------------------------------------------------
# Fuzz target (02 §3.7: "loaders validate bounds and are fuzzed"): the `.hrdb` loader, built like
# engine/asset's. With HELIOS_RECORDS_LIBFUZZER=ON and Clang it links libFuzzer's driver; give the build
# coverage and sanitizers with -DCMAKE_C_FLAGS/-DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined".
# Otherwise it uses engine/asset's standalone driver (corpus replay plus deterministic mutations),
# registered as a CTest (label fuzz) that runs on every PR. Each input is also resealed (its checksums
# recomputed), so mutations reach the validators behind the checksums.
# ---------------------------------------------------------------------------------------------
option(HELIOS_RECORDS_LIBFUZZER "Build the engine/records fuzz target as a libFuzzer binary (Clang only)" OFF)
set(_recordsFuzz records_fuzz_hrdb_loader)
if(HELIOS_RECORDS_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(${_recordsFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_hrdb_loader.cpp)
  target_compile_options(${_recordsFuzz} PRIVATE -fsanitize=fuzzer)
  target_link_options(${_recordsFuzz} PRIVATE -fsanitize=fuzzer)
else()
  add_executable(${_recordsFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_hrdb_loader.cpp
                                 ${PROJECT_SOURCE_DIR}/engine/asset/fuzz/fuzz_main.cpp)
endif()
target_link_libraries(${_recordsFuzz} PRIVATE helios::records helios_records_testtypes)
set_target_properties(${_recordsFuzz} PROPERTIES FOLDER fuzz)
helios_apply_warnings(${_recordsFuzz})
if(HELIOS_BUILD_TESTS AND NOT (HELIOS_RECORDS_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
  add_test(NAME ${_recordsFuzz} COMMAND ${_recordsFuzz} --smoke 20000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/hrdb_loader)
  set_tests_properties(${_recordsFuzz} PROPERTIES LABELS fuzz TIMEOUT 600)
endif()
unset(_recordsFuzz)
