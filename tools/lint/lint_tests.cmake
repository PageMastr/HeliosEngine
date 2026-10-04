# Repository lints (WP-0.2, 09 §5.3 DoD item 5): ISA audit, licence scanner, vendored-patch check,
# IP-name grep, test-namespace check, Windows manifest check and the module-layering fixtures. Every
# lint is a CMake script (`cmake -P`), so it runs on Windows developer machines without Python. Included by
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
  # Generated only where lint_isa_audit reads it. The Visual Studio generators evaluate it once per
  # configuration, $<TARGET_FILE> differs between them, and one path cannot hold differing content.
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/helios_generated/isa_images.txt CONTENT "${imageLines}")
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
# NNNN-<slug>.patch is listed in the manifest and applied to the committed tree. The fixtures with an
# empty expectation must pass (a correctly applied patch, a CRLF checkout, a deleted file, hunks that
# reach the unterminated last line of a file).
# ---------------------------------------------------------------------------------------------
helios_lint_test(lint_vendor_patches COMMAND ${CMAKE_COMMAND}
  -DTHIRD_PARTY_DIR=${PROJECT_SOURCE_DIR}/third_party -DMANIFEST=${PROJECT_SOURCE_DIR}/third_party/MANIFEST.md
  -P ${LINT}/vendor_patches.cmake)
foreach(case
    "applied|"
    "crlf|"
    "deleted|"
    "noeol_edit_last|"
    "noeol_context|"
    "noeol_append|"
    "noeol_unapplied|hunk 1 of third_party/demo/f.c is not applied"
    "unapplied|hunk 1 of third_party/demo/greeting.c is not applied"
    "reordered|hunk 2 of third_party/demo/order.c is not applied"
    "unlisted|0001-greeting.patch: not listed in the table under MANIFEST.md"
    "manifest_ghost|MANIFEST.md lists third_party/demo/patches/0002-ghost.patch, which does not exist"
    "badname|greeting.patch: not named NNNN-<slug>.patch"
    "hunkless|0002-nothing.patch: has no hunks"
    "missing_file|patched file third_party/demo/greeting.c does not exist"
    "deleted_exists|third_party/demo/gone.c should be deleted by this patch but exists"
    "truncated|0001-greeting.patch:7: truncated hunk"
    "malformed|0001-greeting.patch:9: malformed hunk line"
    "headerless|0001-headerless.patch:3: hunk before any \\+\\+\\+ line"
    "devnull_orphan|0001-orphan.patch:12: \\+\\+\\+ /dev/null without a --- a/<file> line")
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
# franchise names out of everything, franchise titles out of content/ and docs/concept/.
# concept_anchor pins that a content directory counts only at the root: engine/content/ and
# engine/docs/concept/ are engine code.
# ---------------------------------------------------------------------------------------------
helios_lint_test(lint_ip_names COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
  -DLINT_POLICY=${LINT}/ip_names_policy.cmake -P ${LINT}/ip_names.cmake)
foreach(case
    "reference_in_code|reference-content name 'Kestrel'"
    "franchise_in_content|franchise name 'Tatooine'"
    "title_in_content|franchise title 'Star Wars'"
    "franchise_in_concept|vista-dusk-v01.webp.concept.jsonc:2: franchise name 'Coruscant'"
    "title_in_concept|t01-world-v01.png.concept.jsonc:2: franchise title 'Star Citizen'"
    "concept_anchor|failed .2 finding.*engine/content/notes.md:1: reference-content name 'Kestrel'.*engine/docs/concept/notes.md:1: reference-content name 'Kestrel'"
    "identifier_in_code|IP-name lint failed .4 finding")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_ip_names_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/ip_names/${fixture} -DLINT_POLICY=${LINT}/ip_names_policy.cmake
            -P ${LINT}/ip_names.cmake)
endforeach()
foreach(fixture clean concept_names)
  helios_lint_test(lint_ip_names_fixture_${fixture} COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${LINT_TESTS}/ip_names/${fixture} -DLINT_POLICY=${LINT}/ip_names_policy.cmake -P ${LINT}/ip_names.cmake)
endforeach()

# ---------------------------------------------------------------------------------------------
# Concept-art references (01 §5.2 provenance, docs/concept/README.md): every image under docs/concept
# has a sidecar with its sha256 and provenance, keeps to the size limits and the folder's names, and is
# in the index. The ok fixture has every format, source kind and optional field, JSONC comments and
# trailing commas, and the 2,560 px edge; each other fixture seeds the violations its expectation names.
# ---------------------------------------------------------------------------------------------
set(crLint -P ${LINT}/concept_refs.cmake)
set(crSide "docs/concept/editor/t01-demo-v01.png.concept.jsonc")
helios_lint_test(lint_concept_refs COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} ${crLint})
helios_lint_test(lint_concept_refs_fixture_ok COMMAND ${CMAKE_COMMAND}
  -DSOURCE_DIR=${LINT_TESTS}/concept_refs/ok ${crLint})
foreach(case
    "no_sidecar|docs/concept/editor/t01-demo-v01.png: no sidecar"
    "orphan|README.md: missing .the index.*TEMPLATE.concept.jsonc: missing .the sidecar template.*orphan sidecar: docs/concept/editor/t01-demo-v01.png does not exist.*notes.md.concept.jsonc: orphan sidecar: docs/concept/world/notes.md is not an image"
    "sha_mismatch|${crSide}:4: sha256 [0-9a-f]+ does not match the image .ed26f33e"
    "fields|failed .11 finding.*:23: unknown field 'license'.*:3: 'image' is 'other.png', not this sidecar's image.*:7: 'created' is '4 Oct 2026'.*:1: missing required field 'ipReview'.*:15: 'status' is 'final', not one of binding, directional, mood-only.*:19: 'phase' is 'Phase 1'.*:13: 'tools' must be an array.*:20: 'planRefs' needs at least 1.*:1: missing required field 'licence'.*:16: 'elements.0..status' is 'maybe'.*:22: 'review' names docs/concept/editor/missing.review.md, which does not exist"
    "provenance|failed .10 finding.*t01-ai-v01.png.concept.jsonc:12: missing required field 'ai.prompt'.*:15: 'ai.inputs.0..source' is 'screenshot'.*:15: 'ai.inputs.1.' is ai-assisted, so it must be an image in docs/concept.*:15: 'ai.inputs.2..sha256' does not match docs/concept/editor/t01-cc0-v01.png.*:15: 'ai.inputs.3.' is ai-assisted, so it must be an image.*t01-cc0-v01.png.concept.jsonc:9: source.kind 'cc0' needs 'source.url'.*:13: a CC0 image keeps licence 'CC0-1.0', not 'MIT'.*t01-owner-v01.png.concept.jsonc:14: 'ai' must be null unless.*t01-paid-v01.png.concept.jsonc:9: source.kind 'commissioned' needs 'source.rights'.*:13: 'licence' is 'CC-BY-4.0'"
    "limits|failed .5 finding.*hud-tall-v01.jpg: 1 x 2600 px, over 2560 px.*t01-wide-v01.png: 2561 x 1 px.*vista-wide-v01.webp: 2600 x 1 px.*vista-wide-v02.webp: 1 x 2600 px.*vista-wide-v03.webp: 2600 x 1 px"
    "files|failed .8 finding.*editor/source.psd: not allowed here.*t01-demo-v01.PNG: not allowed here.*t01-demo-v01.gif: not allowed here.*editor/T01_World.png: name is not.*t01-fake-v01.png: is not a readable PNG file.*editor/t01-world.png: name is not.*ships/x-y-v01.png: not directly in an area directory.*docs/concept/t01-root-v01.png: not directly in an area"
    "headers|failed .4 finding.*hud-cut-v01.jpg: is a JPEG file without a readable frame header.*t01-cut-v01.png: is not a readable PNG.*vista-cut-v01.webp: is a truncated WebP.*vista-cut-v02.webp: is a WebP file whose size cannot be read"
    "index|failed .1 finding.*docs/concept/editor/t01-demo-v01.png: concept 't01-demo' is not in the index"
    "no_index|docs/concept/README.md: no '## Index' section"
    "jsonc|${crSide}:3: not valid JSONC: Missing.*t01-demo-v02.png.concept.jsonc:1: not a JSON object"
    "template|TEMPLATE.concept.jsonc:7: 'created' is '2026-13-01', not a YYYY-MM-DD date.*TEMPLATE.concept.jsonc:1: missing required field 'ipReview'")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_concept_refs_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/concept_refs/${fixture} ${crLint})
endforeach()
helios_lint_test(lint_concept_refs_fixture_no_dir EXPECT_FAIL "concept_refs/docs/concept does not exist"
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/concept_refs ${crLint})
# The 1 MB limit at its edge, written here rather than committed: the ok fixture's demo PNG padded after
# IEND (which readers ignore) to exactly 1,048,576 bytes passes, and to one byte more fails.
set(crOk ${LINT_TESTS}/concept_refs/ok/docs/concept)
foreach(case "at_limit|1048576|" "over_limit|1048577|t01-demo-v01.png: 1048577 bytes, over the 1048576-byte .1 MB. limit")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 target)
  list(GET parts 2 expect)
  set(crDir ${LINT_WORK}/concept_refs/bytes_${fixture}/docs/concept)
  file(REMOVE_RECURSE ${LINT_WORK}/concept_refs/bytes_${fixture})
  file(MAKE_DIRECTORY ${crDir}/editor)
  file(COPY_FILE ${crOk}/README.md ${crDir}/README.md)
  file(COPY_FILE ${crOk}/TEMPLATE.concept.jsonc ${crDir}/TEMPLATE.concept.jsonc)
  file(COPY_FILE ${crOk}/editor/t01-demo.review.md ${crDir}/editor/t01-demo.review.md)
  file(COPY_FILE ${crOk}/editor/t01-demo-v01.png ${crDir}/editor/t01-demo-v01.png)
  file(SIZE ${crDir}/editor/t01-demo-v01.png crSize)
  math(EXPR crPad "${target} - ${crSize}")
  string(REPEAT "x" ${crPad} crPadding)
  file(APPEND ${crDir}/editor/t01-demo-v01.png "${crPadding}")
  file(SHA256 ${crDir}/editor/t01-demo-v01.png crHash)
  file(READ ${crOk}/editor/t01-demo-v01.png.concept.jsonc crSidecar)
  string(REGEX REPLACE "\"sha256\": \"[0-9a-f]+\"" "\"sha256\": \"${crHash}\"" crSidecar "${crSidecar}")
  file(WRITE ${crDir}/editor/t01-demo-v01.png.concept.jsonc "${crSidecar}")
  helios_lint_test(lint_concept_refs_fixture_bytes_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_WORK}/concept_refs/bytes_${fixture} ${crLint})
endforeach()
unset(crPadding)
unset(crSidecar)
# Copies of the ok fixture with one thing changed, written here: no template; an index without its
# Concepts table; and a sidecar nested 1,001 deep (valid JSON, but past the 1,000 levels at which CMake's
# JSON reader throws and cmake aborts without naming the file), which the lint reports by name instead.
foreach(case
    "no_template|failed .1 finding.*docs/concept/TEMPLATE.concept.jsonc: missing"
    "no_concepts|failed .8 finding.*README.md: no '### Concepts' table in the '## Index' section.*t01-demo-v01.png: concept 't01-demo' is not in the index"
    "deep|failed .1 finding.*${crSide}: 1001 '.' and '.', over the 256 a sidecar may hold")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  set(crRoot ${LINT_WORK}/concept_refs/${fixture})
  file(REMOVE_RECURSE ${crRoot})
  file(COPY ${LINT_TESTS}/concept_refs/ok/docs DESTINATION ${crRoot})
  if(fixture STREQUAL "no_template")
    file(REMOVE ${crRoot}/docs/concept/TEMPLATE.concept.jsonc)
  elseif(fixture STREQUAL "no_concepts")
    file(READ ${crRoot}/docs/concept/README.md crReadme)
    string(REPLACE "### Concepts" "### Images" crReadme "${crReadme}")
    file(WRITE ${crRoot}/docs/concept/README.md "${crReadme}")
  else()
    string(REPEAT "[" 1001 crOpen)
    string(REPEAT "]" 1001 crClose)
    file(WRITE ${crRoot}/${crSide} "${crOpen}${crClose}\n")
  endif()
  helios_lint_test(lint_concept_refs_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${crRoot} ${crLint})
endforeach()
unset(crReadme)
unset(crOpen)
unset(crClose)
# git mode, in a small repository written here: an ignored file (.DS_Store) is not looked at, an
# untracked one that is not ignored (a working file about to be committed) is.
find_program(HELIOS_LINT_GIT git)
if(HELIOS_LINT_GIT)
  foreach(repo ignored untracked)
    set(crGit ${LINT_WORK}/concept_refs/git_${repo})
    file(REMOVE_RECURSE ${crGit})
    file(COPY ${LINT_TESTS}/concept_refs/ok/docs DESTINATION ${crGit})
    file(WRITE ${crGit}/.gitignore ".DS_Store\n")
    execute_process(COMMAND ${HELIOS_LINT_GIT} init -q WORKING_DIRECTORY ${crGit} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${HELIOS_LINT_GIT} add -A WORKING_DIRECTORY ${crGit} OUTPUT_QUIET ERROR_QUIET)
  endforeach()
  file(WRITE ${LINT_WORK}/concept_refs/git_ignored/docs/concept/editor/.DS_Store "junk")
  file(WRITE ${LINT_WORK}/concept_refs/git_untracked/docs/concept/editor/t01-demo-v03.psd "8BPS")
  helios_lint_test(lint_concept_refs_git_ignored COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${LINT_WORK}/concept_refs/git_ignored ${crLint})
  helios_lint_test(lint_concept_refs_git_untracked EXPECT_FAIL "editor/t01-demo-v03.psd: not allowed here"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_WORK}/concept_refs/git_untracked ${crLint})
endif()

# ---------------------------------------------------------------------------------------------
# Test namespaces (AAA-PLT-1): in a tests/ source with a doctest test macro, every declaration sits in
# an unnamed namespace (or a waiver region), and test macros do so in every tests/ file, so MSVC
# cannot merge one test file's lambdas or helpers with another's (#15). The ok fixture hides braces,
# test macros and `namespace {` in comments, literals, directives and #if 0; literal_brace,
# raw_comment_hides, directive_comment and if0_comment_endif are the reverse traps. long_tokens
# (128 KB comments, literals, raw string and directive) and spliced_escapes are written here rather
# than committed, and also run under a 1 MB stack where `ulimit` exists.
# ---------------------------------------------------------------------------------------------
set(tnLint ${CMAKE_COMMAND} -DREQUIRE_TESTS=ON -P ${LINT}/test_namespaces.cmake)
helios_lint_test(lint_test_namespaces COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} ${tnLint})
foreach(fixture ok raw_comment_mention)
  helios_lint_test(lint_test_namespaces_fixture_${fixture} COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${LINT_TESTS}/test_namespaces/${fixture} ${tnLint})
endforeach()
foreach(case
    "outside|test_outside.cpp:4: TEST_CASE outside an unnamed namespace"
    "closed_early|test_closed_early.cpp:11: TEST_CASE outside an unnamed namespace"
    "named_only|test_named_only.cpp:6: TEST_CASE outside an unnamed namespace"
    "suite|test_suite.cpp:4: TEST_SUITE outside an unnamed namespace"
    "literal_brace|test_literal_brace.cpp:16: TEST_CASE outside an unnamed namespace"
    "doctest_prefix|test_doctest_prefix.cpp:10: DOCTEST_TEST_CASE_FIXTURE outside an unnamed namespace"
    "subcase_helper|test_subcase_helper.cpp:6: SUBCASE outside an unnamed namespace"
    "unbalanced|test_unbalanced.cpp:4: unbalanced braces"
    "stray_close|test_stray_close.cpp:9: unbalanced braces: '}' closes nothing"
    "crlf|test_crlf.cpp:6: TEST_CASE outside an unnamed namespace"
    "empty|no doctest test macro under"
    "helper_outside|test_helper_outside.cpp:5: declaration outside .* .starts with 'static'."
    "header_macro|tests/cases.h:5: TEST_CASE outside an unnamed namespace"
    "if0_opener|test_if0_opener.cpp:8: TEST_CASE outside an unnamed namespace"
    "macro_generated|test_macro_generated.cpp:6: declaration outside .* .starts with 'MAKE_TEST'."
    "bdd_helper|test_bdd_helper.cpp:5: GIVEN outside an unnamed namespace"
    "test_dir|engine/demo/test/test_singular.cpp:4: TEST_CASE outside an unnamed namespace"
    "raw_unterminated|test_raw_unterminated.cpp:5: unterminated raw string literal"
    "raw_comment_hides|test_raw_comment_hides.cpp:7: TEST_CASE outside an unnamed namespace"
    "waiver_test_macro|test_waiver_test_macro.cpp:5: TEST_CASE outside an unnamed namespace"
    "waiver_no_reason|test_waiver_no_reason.cpp:4: malformed helios-lint comment"
    "waiver_unclosed|test_waiver_unclosed.cpp:10: waiver region not closed"
    "after_waiver|test_after_waiver.cpp:9: declaration outside an unnamed namespace"
    "unterminated_literal|test_unterminated_literal.cpp:5: unterminated literal"
    "waiver_nested|test_waiver_nested.cpp:11: waiver regions do not nest"
    "waiver_scope|test_waiver_scope.cpp:9: this '}' closes the scope of the waiver opened on line 8"
    "waiver_end_depth|test_waiver_end_depth.cpp:7: waiver end at another brace depth than its begin"
    "macro_at_end|test_macro_at_end.cpp:12: declaration outside .* .starts with 'MAKE_TEST'."
    "split_macro|test_split_macro.cpp:5: declaration outside an unnamed namespace"
    "macro_in_named_ns|test_macro_in_named_ns.cpp:10: declaration outside .* .starts with 'DEFINE_HELPER'."
    "macro_before_waiver|test_macro_before_waiver.cpp:7: declaration outside .* .starts with 'HELPER'."
    "if0_comment_endif|test_if0_comment_endif.cpp:10: TEST_CASE outside an unnamed namespace"
    "pp_after_comment|test_pp_after_comment.cpp:7: TEST_CASE outside.*test_pp_after_comment.cpp:13: TEST_CASE"
    "directive_comment|test_directive_comment.cpp:5: TEST_CASE outside an unnamed namespace"
    "pp_unbalanced|test_pp_unbalanced.cpp:4: #endif without #if.*test_pp_unbalanced.cpp:6: #if without #endif"
    "waiver_stray_end|test_waiver_stray_end.cpp:9: waiver end without a begin"
    "string_unterminated|test_string_unterminated.cpp:5: unterminated string literal"
    "raw_long_delim|test_raw_long_delim.cpp:5: raw string delimiter longer than 16 characters"
    "waiver_paren_reason|test_waiver_paren_reason.cpp:4: malformed helios-lint comment"
    "waiver_body_after_end|end.cpp:11: declaration outside.*end.cpp:17: declaration.*end.cpp:22: decl"
    "raw_eol|test_raw_eol.cpp:10: TEST_CASE outside.*test_raw_eol.cpp:14: TEST_CASE outside"
    "pp_spliced_code|test_pp_spliced_code.cpp:14: TEST_CASE outside an unnamed namespace"
    "if0_expression|test_if0_expression.cpp:10: TEST_CASE outside an unnamed namespace")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_test_namespaces_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/test_namespaces/${fixture} ${tnLint})
endforeach()
# Generated fixtures: each construct 128 KB long (a single-class repeat in the regex engine, however
# long), and a line with more escapes than the lexer's recursion bound.
string(REPEAT "x" 131072 tnBig)
string(REPEAT "x123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789\n"
       1638 tnBigLines)
string(REPLACE "\\n" "\n" tnBigLines "${tnBigLines}")
string(REPEAT "\\x41" 400 tnEscapes)
file(WRITE ${LINT_WORK}/test_namespaces/long_tokens/engine/demo/tests/test_long_tokens.cpp
  "// Generated by tools/lint/lint_tests.cmake: 128 KB tokens must lex without deep recursion.\n"
  "#include <doctest/doctest.h>\n#define HELIOS_DEMO_LONG \"${tnBig}\"\n"
  "namespace {\n// ${tnBig}\n/* ${tnBig} */\n/*\n${tnBigLines}*/\n"
  "const char* const kLong = \"${tnBig}\";\nconst char* const kEscapes = \"${tnEscapes}\";\n"
  "const char* const kRaw = R\"(${tnBig}\n${tnBigLines})\";\n"
  "TEST_CASE(\"demo: long tokens\") { CHECK(kLong != nullptr); }\n} // namespace\n")
string(REPEAT "\\x41" 600 tnEscapes)
file(WRITE ${LINT_WORK}/test_namespaces/too_many_escapes/engine/demo/tests/test_too_many_escapes.cpp
  "#include <doctest/doctest.h>\nnamespace {\nconst char* const kEscapes = \"${tnEscapes}\";\n"
  "TEST_CASE(\"demo: escapes\") { CHECK(kEscapes != nullptr); }\n} // namespace\n")
# A spliced string's continuation line with 5000 escapes: without the bound, matching it overflows a
# 1 MB stack (and on an 8 MB one the lint misses the bound).
string(REPEAT "\\x41" 5000 tnEscapes)
file(WRITE ${LINT_WORK}/test_namespaces/spliced_escapes/engine/demo/tests/test_spliced_escapes.cpp
  "#include <doctest/doctest.h>\nnamespace {\nconst char* const kSpliced = \"a\\\n${tnEscapes}\";\n"
  "TEST_CASE(\"demo: escapes\") { CHECK(kSpliced != nullptr); }\n} // namespace\n")
unset(tnBig)
unset(tnBigLines)
unset(tnEscapes)
helios_lint_test(lint_test_namespaces_fixture_long_tokens COMMAND ${CMAKE_COMMAND}
  -DSOURCE_DIR=${LINT_WORK}/test_namespaces/long_tokens ${tnLint})
helios_lint_test(lint_test_namespaces_fixture_too_many_escapes
  EXPECT_FAIL "test_too_many_escapes.cpp:3: more than 500 escapes"
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_WORK}/test_namespaces/too_many_escapes ${tnLint})
set(tnSplicedExpect "test_spliced_escapes.cpp:4: more than 500 escapes on one line")
helios_lint_test(lint_test_namespaces_fixture_spliced_escapes EXPECT_FAIL "${tnSplicedExpect}"
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_WORK}/test_namespaces/spliced_escapes ${tnLint})
# git mode, in two small repositories written here: an untracked test file is scanned; an ignored
# one, and a tracked one deleted from the working tree, are not.
find_program(HELIOS_LINT_GIT git)
if(HELIOS_LINT_GIT)
  set(tnGit ${LINT_WORK}/test_namespaces/git)
  set(tnOutside ${LINT_TESTS}/test_namespaces/outside/engine/demo/tests/test_outside.cpp)
  file(REMOVE_RECURSE ${tnGit})
  foreach(repo untracked skipped)
    file(COPY ${LINT_TESTS}/test_namespaces/ok/engine DESTINATION ${tnGit}/${repo})
    file(WRITE ${tnGit}/${repo}/.gitignore "/engine/demo/tests/ignored/\n")
    file(COPY ${tnOutside} DESTINATION ${tnGit}/${repo}/engine/demo/tests/deleted)
    execute_process(COMMAND ${HELIOS_LINT_GIT} init -q WORKING_DIRECTORY ${tnGit}/${repo}
                    OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND ${HELIOS_LINT_GIT} add -A WORKING_DIRECTORY ${tnGit}/${repo}
                    OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE_RECURSE ${tnGit}/${repo}/engine/demo/tests/deleted)
  endforeach()
  file(COPY ${tnOutside} DESTINATION ${tnGit}/untracked/engine/demo/tests/new)
  file(COPY ${tnOutside} DESTINATION ${tnGit}/skipped/engine/demo/tests/ignored)
  helios_lint_test(lint_test_namespaces_git_untracked EXPECT_FAIL "tests/new/test_outside.cpp:4: TEST_CASE"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${tnGit}/untracked ${tnLint})
  helios_lint_test(lint_test_namespaces_git_skipped COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${tnGit}/skipped
    ${tnLint})
endif()
find_program(HELIOS_LINT_SH sh)
if(HELIOS_LINT_SH AND NOT WIN32)
  set(tn1mb "ulimit -s 1024 && exec \"$0\" -DSOURCE_DIR=\"$1\" -DREQUIRE_TESTS=ON -P \"$2\"")
  helios_lint_test(lint_test_namespaces_fixture_long_tokens_1mb_stack COMMAND ${HELIOS_LINT_SH} -c "${tn1mb}"
    ${CMAKE_COMMAND} ${LINT_WORK}/test_namespaces/long_tokens ${LINT}/test_namespaces.cmake)
  helios_lint_test(lint_test_namespaces_fixture_spliced_escapes_1mb_stack EXPECT_FAIL "${tnSplicedExpect}"
    COMMAND ${HELIOS_LINT_SH} -c "${tn1mb}"
    ${CMAKE_COMMAND} ${LINT_WORK}/test_namespaces/spliced_escapes ${LINT}/test_namespaces.cmake)
endif()

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

# tools/ci/run_lints.cmake forwards -DHELIOS_STATUS_PYTHON to the D6 status check (for a Python that is
# not on PATH): a path that runs no Python must fail that check with its own diagnostic.
helios_lint_test(lint_run_lints_status_python
  EXPECT_FAIL "check_status:.*HELIOS_STATUS_PYTHON=.*no-such-python.*does not run Python.*lints failed:.*status"
  COMMAND ${CMAKE_COMMAND} -DHELIOS_STATUS_PYTHON=${LINT_WORK}/no-such-python
          -P ${PROJECT_SOURCE_DIR}/tools/ci/run_lints.cmake)

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
    "editor_only_gateway|gateway executable 'fx-gateway' links an EDITOR_ONLY module: fx-gateway -> helios_assetpipe"
    "server_graphics|cell executable 'fx-cell' may link only HEADLESS modules"
    "runtime_to_editor|is not EDITOR_ONLY but depends on EDITOR_ONLY module 'assetpipe'"
    "cycle|dependency cycle between modules"
    "missing_layer|module 'mystery' has no layer"
    "layer_mismatch|contradicts the layering table"
    "flag_mismatch|helios_module.net EDITOR_ONLY. contradicts the layering table .* is not EDITOR_ONLY"
    "peers_mismatch|helios_module.net PEERS physics. contradicts the layering table .* does not list 'physics' as a peer"
    "redeclared_row|module 'net' already has a row in the layering table"
    "unordered|layering check failed .1 violation.s.. rules.*module 'net' is not in HELIOS_MODULE_ORDER .add it"
    "plain_executable|executable 'fx-rogue' under apps/ is not declared with helios_executable.., so no role check"
    "bad_order|HELIOS_MODULE_ORDER lists 'ecs' before its dependency 'reflect'"
    "bridge_upward|module 'reflect' .layer 2. depends upward on 'world' .layer 4. .through fx_bridge."
    "bridge_same_layer|module 'net' depends on same-layer module 'physics' .*.through fx_bridge."
    "bridge_cycle|dependency cycle between modules: helios_ecs -> fx_bridge -> helios_app -> helios_ecs"
    "unknown_role|helios_executable.fx-zonehost.: no ROLE given and none is known for.*'apps/zonehost/'")
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

# RC-1's shipped-pipelines lint (WP-0.12), registered by its owner.
include(${PROJECT_SOURCE_DIR}/tools/rendertest/tests/shipped_pipelines_tests.cmake)
