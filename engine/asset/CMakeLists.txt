# engine/asset — L2 asset runtime (HEADLESS; 02 §6.1, §6.3, §6.4): AssetId and its GUID fold with the
# packaging collision check, AssetHandle<T> / AssetStore<T> over core handles with frame-boundary swaps,
# and the `.hpak` v0 reader (header/TOC validation, per-block XXH3-64 verification with a re-fetch
# hook, zstd blocks) plus the overlay mount table. The pak writer is engine/assetpipe's.

helios_module(asset HEADLESS
  SOURCES
    src/asset_id.cpp
    src/hpak_format.cpp
    src/hpak_reader.cpp
    src/mount_table.cpp
  DEPS helios::core
  PRIVATE_DEPS helios::tp::zstd)

# The reader tests build their paks with assetpipe's writer (and corrupt them), so they link it. A
# test binary is no shipped image, so the EDITOR_ONLY rule (02 §1.1) does not apply to it.
file(GLOB HELIOS_ASSET_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(asset_tests SOURCES ${HELIOS_ASSET_TEST_SOURCES} DEPS helios::asset helios::assetpipe)

# ---------------------------------------------------------------------------------------------
# Fuzz target (02 §8.3, 09 §6: the `.hpak` reader is one of the fuzzed readers). A libFuzzer entry
# point (LLVMFuzzerTestOneInput), built like engine/net's targets: with HELIOS_ASSET_LIBFUZZER=ON and
# Clang it links libFuzzer's driver; give the build coverage and sanitizers with
#   -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" (and the same CMAKE_CXX_FLAGS).
# Otherwise it is a plain executable that replays the committed corpus plus deterministic mutations,
# registered as a CTest (label fuzz) that runs on every PR.
# ---------------------------------------------------------------------------------------------
option(HELIOS_ASSET_LIBFUZZER "Build the engine/asset fuzz target as a libFuzzer binary (Clang only)" OFF)
set(_assetFuzz asset_fuzz_hpak_reader)
if(HELIOS_ASSET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(${_assetFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_hpak_reader.cpp)
  target_compile_options(${_assetFuzz} PRIVATE -fsanitize=fuzzer)
  target_link_options(${_assetFuzz} PRIVATE -fsanitize=fuzzer)
else()
  add_executable(${_assetFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_hpak_reader.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_main.cpp)
endif()
# assetpipe supplies the seeds (--make-seeds) and the checksum reseal that lets mutations reach the
# field validators behind the header and TOC checksums.
target_link_libraries(${_assetFuzz} PRIVATE helios::asset helios::assetpipe)
set_target_properties(${_assetFuzz} PROPERTIES FOLDER fuzz)
helios_apply_warnings(${_assetFuzz})
if(HELIOS_BUILD_TESTS AND NOT (HELIOS_ASSET_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
  add_test(NAME ${_assetFuzz} COMMAND ${_assetFuzz} --smoke 20000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/hpak_reader)
  set_tests_properties(${_assetFuzz} PROPERTIES LABELS fuzz TIMEOUT 600)
endif()
unset(_assetFuzz)
