# engine/patch — L2 patch pipeline (HEADLESS; 05 §7, 08 §2.5–2.6, 02 §1.1): FastCDC chunking with
# BLAKE2b-256 chunk IDs and the `.hman` v0 manifest writer and reader (WP-0.16 part 1); the Ed25519 trust
# chain (keysets, signed pointers, ratchets) and the CDN read path (WP-0.16 part 2). install.db, the
# planner, HTTP clients and StreamingInstaller (08 §4.1) come later. The launcher links this module in its
# x86-64-v1 (`base`) image, so it has no ISA-specific code and depends only on core, Monocypher (BLAKE2b
# and Ed25519, portable C) and zstd.

helios_module(patch HEADLESS
  SOURCES
    src/blake2b.cpp
    src/cdn.cpp
    src/fastcdc.cpp
    src/manifest.cpp
    src/trust.cpp
  DEPS helios::core
  PRIVATE_DEPS helios::tp::monocypher helios::tp::zstd)

# Tests (doctest). The FastCDC, .hman and trust vectors are shared with Go's services/pkg/cdc,
# services/pkg/manifest and services/pkg/patchcdn (services/testdata/vectors), so a divergence fails both
# suites. Monocypher derives the test-only keys (Ed25519 from a seed) to check the shared vectors' roots.
file(GLOB HELIOS_PATCH_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(patch_tests SOURCES ${HELIOS_PATCH_TEST_SOURCES}
  DEPS helios::patch helios::tp::yyjson helios::tp::monocypher helios::tp::zstd)
if(TARGET patch_tests)
  target_compile_definitions(patch_tests PRIVATE
    HELIOS_PATCH_VECTORS_DIR="${PROJECT_SOURCE_DIR}/services/testdata/vectors"
    HELIOS_PATCH_FUZZ_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/manifest_reader"
    HELIOS_PATCH_TRUST_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/trust_parser")
  set_tests_properties(patch_tests PROPERTIES TIMEOUT 600)
endif()

# lint_patch_test_keys (label lint): TrustOptions::allowTestKeys, the opt-in that lets a verifier accept the
# shared vectors' test-only roots, appears only in trust.h, trust.cpp, tests/ and fuzz/ (tests/test_keys_lint.cmake;
# Go's TestTrustTestImportedOnlyByTests does the same for Options.AllowTestKeys). Seeded fixtures must fail with
# their message (tools/lint/expect_fail.cmake), so a lint that stops matching fails too.
if(HELIOS_BUILD_TESTS)
  set(_patchKeysLint ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keys_lint.cmake)
  set(_patchKeysFixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_keys_lint)
  add_test(NAME lint_patch_test_keys COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${_patchKeysLint})
  add_test(NAME lint_patch_test_keys_fixture_clean
           COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${_patchKeysFixtures}/clean -P ${_patchKeysLint})
  set(_patchKeysTests lint_patch_test_keys lint_patch_test_keys_fixture_clean)
  foreach(_case "outside|2 finding.*apps/launcher/src/main.cpp:4: allowTestKeys outside.*tools/patchcheck/check.h:3: allowTestKeys outside"
                "none|trust.h does not declare allowTestKeys")
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _fixture)
    list(GET _parts 1 _expect)
    add_test(NAME lint_patch_test_keys_fixture_${_fixture}
             COMMAND ${CMAKE_COMMAND} "-DEXPECT=${_expect}" -DNARGS=4 -DARG0=${CMAKE_COMMAND}
                     -DARG1=-DSOURCE_DIR=${_patchKeysFixtures}/${_fixture} -DARG2=-P -DARG3=${_patchKeysLint}
                     -P ${PROJECT_SOURCE_DIR}/tools/lint/expect_fail.cmake)
    list(APPEND _patchKeysTests lint_patch_test_keys_fixture_${_fixture})
  endforeach()
  set_tests_properties(${_patchKeysTests} PROPERTIES LABELS lint TIMEOUT 300)
  unset(_patchKeysLint)
  unset(_patchKeysFixtures)
  unset(_patchKeysTests)
  unset(_case)
  unset(_parts)
  unset(_fixture)
  unset(_expect)
endif()

# ---------------------------------------------------------------------------------------------
# Fuzz targets (02 §8.3 and 09 §6: manifests are among the fuzzed readers; CL-14 asks for fuzzed parsers):
# the .hman reader, and the keyset and pointer parsers with the trust checks behind them. libFuzzer entry
# points, built like engine/asset's: with HELIOS_PATCH_LIBFUZZER=ON and Clang they link libFuzzer's driver;
# give the build coverage and sanitizers with
#   -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" (and the same CMAKE_CXX_FLAGS).
# Otherwise each is a plain executable that replays its committed corpus plus deterministic mutations,
# registered as a CTest (label fuzz) that runs on every PR.
# ---------------------------------------------------------------------------------------------
option(HELIOS_PATCH_LIBFUZZER "Build the engine/patch fuzz targets as libFuzzer binaries (Clang only)" OFF)
foreach(_patchFuzzName manifest_reader trust_parser)
  set(_patchFuzz patch_fuzz_${_patchFuzzName})
  if(HELIOS_PATCH_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(${_patchFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_${_patchFuzzName}.cpp)
    target_compile_options(${_patchFuzz} PRIVATE -fsanitize=fuzzer)
    target_link_options(${_patchFuzz} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(${_patchFuzz} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_${_patchFuzzName}.cpp
                                 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz_main.cpp)
  endif()
  target_link_libraries(${_patchFuzz} PRIVATE helios::patch helios::tp::zstd helios::tp::monocypher)
  set_target_properties(${_patchFuzz} PROPERTIES FOLDER fuzz)
  helios_apply_warnings(${_patchFuzz})
  if(HELIOS_BUILD_TESTS AND NOT (HELIOS_PATCH_LIBFUZZER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    add_test(NAME ${_patchFuzz}
             COMMAND ${_patchFuzz} --smoke 20000 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${_patchFuzzName})
    set_tests_properties(${_patchFuzz} PROPERTIES LABELS fuzz TIMEOUT 600)
  endif()
endforeach()
unset(_patchFuzz)
unset(_patchFuzzName)
