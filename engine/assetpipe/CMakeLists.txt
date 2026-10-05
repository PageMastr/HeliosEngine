# engine/assetpipe — L4 asset pipeline (EDITOR_ONLY; 02 §6.1, §6.2, §6.3): the `.hpak` v0 writer that paks
# are built with (helios-pack drives it later), `.meta` sidecars, the DDC key, the local DDC store and the
# one-asset cook through it. Importers' real build steps, the job DAG (helios-assetd) and the shared DDC come
# with Phase 1 and WP-2.1.

helios_module(assetpipe EDITOR_ONLY
  SOURCES
    src/cook.cpp
    src/ddc.cpp
    src/hpak_writer.cpp
    src/importer.cpp
    src/meta.cpp
  DEPS helios::core helios::asset helios::reflect
  PRIVATE_DEPS helios::tp::zstd)

file(GLOB HELIOS_ASSETPIPE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(assetpipe_tests SOURCES ${HELIOS_ASSETPIPE_TEST_SOURCES} DEPS helios::assetpipe)

# ---------------------------------------------------------------------------------------------
# Fuzz target: the DDC entry reader (02 §8.3 fuzzes the cooked readers; a DDC entry is read back from a
# cache directory that any local process can write). Built like engine/asset's target and behind the same
# switch: with HELIOS_ASSET_LIBFUZZER=ON and Clang it links libFuzzer's driver (give the build coverage and
# sanitizers with -fsanitize=fuzzer-no-link,address,undefined); otherwise it links engine/asset's
# standalone driver and is a CTest (label fuzz, every PR) that replays the committed corpus plus
# deterministic mutations.
# ---------------------------------------------------------------------------------------------
set(_ddcFuzz assetpipe_fuzz_ddc_entry)
if(HELIOS_ASSET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(${_ddcFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_ddc_entry.cpp)
  target_compile_options(${_ddcFuzz} PRIVATE -fsanitize=fuzzer)
  target_link_options(${_ddcFuzz} PRIVATE -fsanitize=fuzzer)
else()
  add_executable(${_ddcFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_ddc_entry.cpp
                             ${CMAKE_CURRENT_SOURCE_DIR}/../asset/fuzz/fuzz_main.cpp)
endif()
target_link_libraries(${_ddcFuzz} PRIVATE helios::assetpipe)
set_target_properties(${_ddcFuzz} PROPERTIES FOLDER fuzz)
helios_apply_warnings(${_ddcFuzz})
if(HELIOS_BUILD_TESTS AND NOT (HELIOS_ASSET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
  add_test(NAME ${_ddcFuzz} COMMAND ${_ddcFuzz} --smoke 20000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/ddc_entry)
  set_tests_properties(${_ddcFuzz} PROPERTIES LABELS fuzz TIMEOUT 600)
endif()
unset(_ddcFuzz)
