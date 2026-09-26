# Repository lints (WP-0.2, 09 §5.3 DoD item 5): ISA audit, licence scanner, vendored-patch check,
# IP-name grep, Windows manifest check and the module-layering fixtures. Every lint is a CMake script
# (`cmake -P`), so it runs on Windows developer machines without Python. Included by
# helios_finalize_build() (cmake/HeliosLayering.cmake) at the end of the top-level CMakeLists.txt,
# once every target exists (a deferred call cannot add_subdirectory, hence an include). All tests
# carry the CTest label `lint`.

set(LINT ${CMAKE_CURRENT_LIST_DIR})
set(LINT_TESTS ${CMAKE_CURRENT_LIST_DIR}/tests)
set(LINT_WORK ${CMAKE_BINARY_DIR}/tools/lint/work)

function(helios_lint_test name)
  cmake_parse_arguments(L "" "EXPECT_FAIL" "COMMAND" ${ARGN})
  if(L_EXPECT_FAIL)
    # A seeded violation: pass only if the lint exits non-zero AND prints the expected diagnostic
    # (tools/lint/expect_fail.cmake; a regex alone would accept a lint that reports but exits 0).
    set(wrapped "")
    set(i 0)
    foreach(arg IN LISTS L_COMMAND)
      list(APPEND wrapped "-DARG${i}=${arg}")
      math(EXPR i "${i} + 1")
    endforeach()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} "-DEXPECT=${L_EXPECT_FAIL}" -DNARGS=${i} ${wrapped}
                                  -P ${LINT}/expect_fail.cmake)
  else()
    add_test(NAME ${name} COMMAND ${L_COMMAND})
  endif()
  set_tests_properties(${name} PROPERTIES LABELS lint TIMEOUT 300)
endfunction()

# ---------------------------------------------------------------------------------------------
# ISA audit (02 §1.1, RT-09): flags of every TU, disassembly + symbols of the CPU-gate objects,
# .preinit_array / IRELATIVE of every gated ELF executable.
# ---------------------------------------------------------------------------------------------
get_property(gated GLOBAL PROPERTY HELIOS_CPU_GATE_TARGETS)
set(imageLines "")
foreach(t IN LISTS gated)
  string(APPEND imageLines "$<TARGET_FILE:${t}>\n")
endforeach()
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/helios_generated/isa_images.txt CONTENT "${imageLines}")
set(isaTools "")
if(CMAKE_OBJDUMP AND NOT MSVC)
  list(APPEND isaTools -DOBJDUMP=${CMAKE_OBJDUMP})
endif()
if(CMAKE_NM AND NOT MSVC)
  list(APPEND isaTools -DNM=${CMAKE_NM})
endif()
if(CMAKE_READELF AND NOT WIN32)
  list(APPEND isaTools -DREADELF=${CMAKE_READELF})
endif()
set(isaCommon -DALLOWLIST=${PROJECT_SOURCE_DIR}/cmake/isa_allowlist.cmake)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  message(STATUS "lint_isa_audit needs compile_commands.json (Ninja or Makefile generators); "
                 "Visual Studio builds are audited by the Ninja CI presets")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  # Sanitizer runtimes add their own .preinit_array entries, so the image check (exactly one
  # pre-initializer: the gate) only runs in normal builds.
  set(isaImages -DIMAGES_FILE=${CMAKE_BINARY_DIR}/helios_generated/isa_images.txt)
  if(HELIOS_SANITIZE)
    set(isaImages "")
  endif()
  helios_lint_test(lint_isa_audit COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} ${isaImages}
    -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json -DREQUIRE_GATE=ON
    -P ${LINT}/isa_audit.cmake)

  # Seeded violations: each fixture compile database must be rejected with its diagnostic.
  foreach(case
      "unlisted_avx2|not on the ISA allowlist"
      "gate_avx|baseline unit .CPU gate. compiled with AVX-class flags"
      "kernel_fma|FMA enabled"
      "kernel_avx512|AVX-512 enabled"
      "march_native|-march=native"
      "fast_math|fast-math"
      "contract|FP contraction enabled"
      "msvc_arch|compiled with AVX-class flags .avx, avx2."
      "gate_sse42|baseline unit .CPU gate. compiled with flags above x86-64-v1 .-mpopcnt -msse4.2."
      "gate_march|baseline unit .CPU gate. compiled with flags above x86-64-v1 .-march=nehalem."
      "clang_cl_forwarded|compiled with AVX-class flags .avx2, bmi2. but not on the ISA allowlist")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 fixture)
    list(GET parts 1 expect)
    helios_lint_test(lint_isa_fixture_${fixture} EXPECT_FAIL "${expect}"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} -DSKIP_OBJECTS=ON
              -DCOMPILE_COMMANDS=${LINT_TESTS}/isa/${fixture}.json -P ${LINT}/isa_audit.cmake)
  endforeach()
  helios_lint_test(lint_isa_fixture_ok COMMAND ${CMAKE_COMMAND} ${isaCommon} -DSKIP_OBJECTS=ON -DREQUIRE_GATE=ON
    -DCOMPILE_COMMANDS=${LINT_TESTS}/isa/ok.json -P ${LINT}/isa_audit.cmake)

  # The disassembly check must catch AVX code: a designated kernel (allowlisted by name, so the
  # flags audit accepts it) audited as if it were a baseline object.
  if(isaTools MATCHES "OBJDUMP")
    add_library(lint_isa_fixture_kernel OBJECT)
    helios_avx2_sources(lint_isa_fixture_kernel ${LINT_TESTS}/isa/fixture_kernel_avx2.c)
    set_target_properties(lint_isa_fixture_kernel PROPERTIES FOLDER tests)
    add_custom_target(lint_isa_fixture_kernel_build ALL DEPENDS lint_isa_fixture_kernel)
    helios_lint_test(lint_isa_disasm_detects_avx EXPECT_FAIL "instruction not allowed at the x86-64-v1 baseline"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
              "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_kernel>" -P ${LINT}/isa_audit.cmake)
    # CMPXCHG16B behind a lock prefix, compiled at the baseline (the assembler accepts it anyway).
    add_library(lint_isa_fixture_prefixed OBJECT ${LINT_TESTS}/isa/fixture_cx16.c)
    set_target_properties(lint_isa_fixture_prefixed PROPERTIES FOLDER tests)
    add_dependencies(lint_isa_fixture_kernel_build lint_isa_fixture_prefixed)
    helios_lint_test(lint_isa_disasm_detects_prefixed EXPECT_FAIL "not allowed at the x86-64-v1 baseline.*cmpxchg16b"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
              "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_prefixed>" -P ${LINT}/isa_audit.cmake)
  endif()
  # Check 3's canary (ELF): one .preinit_array entry that is not the gate's hook. (Not in sanitizer
  # builds: their runtimes add .preinit_array entries of their own.)
  if(isaTools MATCHES "READELF" AND isaTools MATCHES "NM" AND NOT HELIOS_SANITIZE)
    add_executable(lint_isa_fixture_foreign_preinit ${LINT_TESTS}/isa/preinit_not_gate.c)
    set_target_properties(lint_isa_fixture_foreign_preinit PROPERTIES FOLDER tests)
    helios_lint_test(lint_isa_image_detects_foreign_preinit EXPECT_FAIL "is not the CPU gate .hcg_gate at"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=image
              "-DIMAGE=$<TARGET_FILE:lint_isa_fixture_foreign_preinit>" -P ${LINT}/isa_audit.cmake)
  endif()
endif()

# ---------------------------------------------------------------------------------------------
# Licence scanner (CLAUDE.md allowlist): third_party/*/LICENSE* (and nested licence files) and the
# licence column of third_party/MANIFEST.md.
# ---------------------------------------------------------------------------------------------
set(licenseCommon -DLINT_POLICY=${LINT}/license_policy.cmake)
helios_lint_test(lint_licenses COMMAND ${CMAKE_COMMAND} ${licenseCommon}
  -DTHIRD_PARTY_DIR=${PROJECT_SOURCE_DIR}/third_party -DMANIFEST=${PROJECT_SOURCE_DIR}/third_party/MANIFEST.md
  -P ${LINT}/licenses.cmake)
foreach(case
    "good|"
    "gpl|forbidden licence"
    "unknown|unrecognized licence"
    "missing|has no licence file"
    "unlisted|not listed in"
    "manifest_license|MANIFEST.md lists licence 'GPL-2.0'"
    "artistic|perlish/LICENSE: forbidden licence"
    "gpl_pd_mention|gnuish/COPYING: forbidden licence"
    "unlisted_mention|third_party/helper is not listed in")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(LENGTH parts n)
  set(expect "")
  if(n GREATER 1)
    list(GET parts 1 expect)
  endif()
  helios_lint_test(lint_licenses_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} ${licenseCommon} -DTHIRD_PARTY_DIR=${LINT_TESTS}/licenses/${fixture}/third_party
            -DMANIFEST=${LINT_TESTS}/licenses/${fixture}/MANIFEST.md -P ${LINT}/licenses.cmake)
endforeach()

# ---------------------------------------------------------------------------------------------
# Vendored patches (CLAUDE.md; third_party/MANIFEST.md "Patches"): every third_party/<dep>/patches/
# NNNN-<slug>.patch is listed in the manifest and applied to the committed tree.
# ---------------------------------------------------------------------------------------------
helios_lint_test(lint_vendor_patches COMMAND ${CMAKE_COMMAND}
  -DTHIRD_PARTY_DIR=${PROJECT_SOURCE_DIR}/third_party -DMANIFEST=${PROJECT_SOURCE_DIR}/third_party/MANIFEST.md
  -P ${LINT}/vendor_patches.cmake)
foreach(case
    "applied|"
    "unapplied|hunk 1 of third_party/demo/greeting.c is not applied"
    "unlisted|0001-greeting.patch: not listed in third_party/MANIFEST.md"
    "badname|greeting.patch: not named NNNN-<slug>.patch"
    "missing_file|patched file third_party/demo/greeting.c does not exist")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(LENGTH parts n)
  set(expect "")
  if(n GREATER 1)
    list(GET parts 1 expect)
  endif()
  helios_lint_test(lint_vendor_patches_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DTHIRD_PARTY_DIR=${LINT_TESTS}/vendor_patches/${fixture}/third_party
            -DMANIFEST=${LINT_TESTS}/vendor_patches/${fixture}/MANIFEST.md -P ${LINT}/vendor_patches.cmake)
endforeach()

# ---------------------------------------------------------------------------------------------
# IP-name grep (01 §4.1 rule 2, §5.2): reference-content names out of engine/Foundation code,
# franchise names out of everything, franchise titles out of content/.
# ---------------------------------------------------------------------------------------------
helios_lint_test(lint_ip_names COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
  -DLINT_POLICY=${LINT}/ip_names_policy.cmake -P ${LINT}/ip_names.cmake)
foreach(case
    "reference_in_code|reference-content name 'Kestrel'"
    "franchise_in_content|franchise name 'Tatooine'"
    "title_in_content|franchise title 'Star Wars'"
    "identifier_in_code|IP-name lint failed .4 finding")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_ip_names_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/ip_names/${fixture} -DLINT_POLICY=${LINT}/ip_names_policy.cmake
            -P ${LINT}/ip_names.cmake)
endforeach()
helios_lint_test(lint_ip_names_fixture_clean COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/ip_names/clean
  -DLINT_POLICY=${LINT}/ip_names_policy.cmake -P ${LINT}/ip_names.cmake)

# ---------------------------------------------------------------------------------------------
# Windows manifest (ADR-011): the source manifest's settings, and on Windows builds that a built
# executable embeds it (runs on the Linux host of the MinGW cross build too).
# ---------------------------------------------------------------------------------------------
set(manifestArgs -DMANIFEST=${PROJECT_SOURCE_DIR}/engine/platform/win/helios.manifest)
if(WIN32 AND TARGET core_tests)
  list(APPEND manifestArgs "-DBINARY=$<TARGET_FILE:core_tests>")
endif()
helios_lint_test(lint_windows_manifest COMMAND ${CMAKE_COMMAND} ${manifestArgs} -P ${LINT}/windows_manifest.cmake)
helios_lint_test(lint_windows_manifest_fixture EXPECT_FAIL "longPathAware"
  COMMAND ${CMAKE_COMMAND} -DMANIFEST=${LINT_TESTS}/manifest/bad.manifest -P ${LINT}/windows_manifest.cmake)

# ---------------------------------------------------------------------------------------------
# Module layering (02 §1.1): a fixture project configured once per case. Good graphs configure;
# each seeded violation must stop configure with its diagnostic.
# ---------------------------------------------------------------------------------------------
# The fixture declares LANGUAGES NONE and stops right after the checks, so no compiler is probed;
# the generator is passed through only so CMake does not go looking for a default one.
set(layeringGenerator "")
if(CMAKE_GENERATOR MATCHES "Ninja")
  set(layeringGenerator -G ${CMAKE_GENERATOR} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
else()
  find_program(HELIOS_LINT_NINJA ninja)
  if(HELIOS_LINT_NINJA)
    set(layeringGenerator -G Ninja -DCMAKE_MAKE_PROGRAM=${HELIOS_LINT_NINJA})
  endif()
endif()
foreach(case
    "good|HELIOS_FIXTURE_CONFIGURE_OK"
    "legacy_signature|HELIOS_FIXTURE_CONFIGURE_OK"
    "explicit_layer|HELIOS_FIXTURE_CONFIGURE_OK"
    "upward|depends upward on 'net'"
    "same_layer|depends on same-layer module 'ecs'"
    "headless_module|HEADLESS module 'world' reaches a non-HEADLESS module or graphics library: helios_world -> helios_physics -> helios_render"
    "headless_graphics_tp|HEADLESS module 'net' reaches a non-HEADLESS module or graphics library: helios_net -> fx_helper -> SDL3-static"
    "genex_build_interface|HEADLESS module 'net' reaches a non-HEADLESS module or graphics library: helios_net -> SDL3-static"
    "genex_nested_condition|HEADLESS module 'net' reaches a non-HEADLESS module or graphics library: helios_net -> SDL3-static"
    "genex_if|HEADLESS module 'net' reaches a non-HEADLESS module or graphics library: helios_net -> SDL3-static"
    "editor_only_client|client executable 'fx-client' links an EDITOR_ONLY module"
    "editor_only_server|cell executable 'fx-cell' links an EDITOR_ONLY module"
    "server_graphics|cell executable 'fx-cell' may link only HEADLESS modules"
    "runtime_to_editor|is not EDITOR_ONLY but depends on EDITOR_ONLY module 'assetpipe'"
    "cycle|dependency cycle between modules"
    "missing_layer|module 'mystery' has no layer"
    "layer_mismatch|contradicts the layering table"
    "bad_order|HELIOS_MODULE_ORDER lists 'ecs' before its dependency 'reflect'")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  add_test(NAME lint_layering_${fixture}
    COMMAND ${CMAKE_COMMAND} -S ${LINT_TESTS}/layering -B ${LINT_WORK}/layering/${fixture} ${layeringGenerator}
            -DHELIOS_FIXTURE_CASE=${fixture} -DHELIOS_SOURCE_DIR=${PROJECT_SOURCE_DIR})
  set_tests_properties(lint_layering_${fixture} PROPERTIES LABELS lint TIMEOUT 120
    PASS_REGULAR_EXPRESSION "${expect}")
  if(expect STREQUAL "HELIOS_FIXTURE_CONFIGURE_OK")
    set_tests_properties(lint_layering_${fixture} PROPERTIES FAIL_REGULAR_EXPRESSION "helios layering:")
  else()
    set_tests_properties(lint_layering_${fixture} PROPERTIES FAIL_REGULAR_EXPRESSION "HELIOS_FIXTURE_CONFIGURE_OK")
  endif()
endforeach()
