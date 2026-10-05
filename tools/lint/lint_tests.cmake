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
# ISA audit (02 §1.1, RT-09; WP-0.2r): check 1 (every TU against its image's level) over the whole
# compile database, check 2 (disassembly and symbols of the CPU-gate objects), the ELF part of check 3
# (.preinit_array, IRELATIVE of every gated executable) and check 4 (base images after linking). Each
# check has seeded fixtures that must fail with their diagnostic.
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
set(isaX86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(isaX86 ON)
endif()
# Check 4 reads ELF images with GNU binutils here; COFF images and PDBs are WP-0.2r part 2's. Sanitizer
# runtimes add code and pre-initializers of their own, so checks 3 and 4 run in normal builds only.
set(isaElfImages OFF)
if(isaX86 AND NOT WIN32 AND NOT HELIOS_SANITIZE AND isaTools MATCHES "OBJDUMP")
  set(isaElfImages ON)
endif()

# The base fixture image (check 4 "on a base fixture image now", 09 §2 WP-0.2r): a launcher-like image
# that links core and patch, built from their base copies on every toolchain, so the copies are compiled
# wherever tests are. ISA base is the fixtures-only override of helios_executable().
if(isaX86 AND TARGET helios::patch)
  helios_executable(lint_isa_fixture_base ROLE tool ISA base SOURCES ${LINT_TESTS}/isa/base_image.cpp
                    DEPS helios::core helios::patch)
  set_target_properties(lint_isa_fixture_base PROPERTIES FOLDER tests)
endif()
# Base images that lint_isa_audit checks after linking (every one but the seeded canary below).
set(baseImageLines "")
get_property(apps GLOBAL PROPERTY HELIOS_APP_TARGETS)
foreach(t IN LISTS apps)
  get_target_property(level ${t} HELIOS_ISA_LEVEL)
  if(level STREQUAL "base")
    string(APPEND baseImageLines "$<TARGET_FILE:${t}>\n")
  endif()
endforeach()

if(isaX86 AND NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  message(STATUS "lint_isa_audit needs compile_commands.json (Ninja or Makefile generators); "
                 "Visual Studio builds are audited by the Ninja CI presets")
endif()
if(isaX86 AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  # Generated only where lint_isa_audit reads them. The Visual Studio generators evaluate them once per
  # configuration, $<TARGET_FILE> differs between them, and one path cannot hold differing content.
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/helios_generated/isa_images.txt CONTENT "${imageLines}")
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/helios_generated/isa_base_images.txt CONTENT "${baseImageLines}")
  # Sanitizer runtimes add their own .preinit_array entries, so the image check (exactly one
  # pre-initializer: the gate) only runs in normal builds.
  set(isaImages -DIMAGES_FILE=${CMAKE_BINARY_DIR}/helios_generated/isa_images.txt)
  if(HELIOS_SANITIZE)
    set(isaImages "")
  endif()
  if(isaElfImages)
    list(APPEND isaImages -DBASE_IMAGES_FILE=${CMAKE_BINARY_DIR}/helios_generated/isa_base_images.txt)
  endif()
  # Modular dev builds (ELF): the shared libraries a gated executable loads at start-up, which the dynamic
  # linker relocates before .preinit_array runs: the link-group libraries and the shared libraries they reach
  # (SDL3, tp_imgui). Check 3 rejects IFUNC relocations in them (ADR-0.6c §3 item 6).
  if(HELIOS_MODULAR AND NOT WIN32 AND NOT HELIOS_SANITIZE)
    get_property(isaGroups GLOBAL PROPERTY HELIOS_LINK_GROUP_TARGETS)
    set(isaShared ${isaGroups})
    foreach(g IN LISTS isaGroups)
      _helios_isa_shared_reach(${g} reach)
      list(APPEND isaShared ${reach})
    endforeach()
    list(REMOVE_DUPLICATES isaShared)
    set(sharedLines "")
    foreach(t IN LISTS isaShared)
      string(APPEND sharedLines "$<TARGET_FILE:${t}>\n")
    endforeach()
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/helios_generated/isa_shared_images.txt CONTENT "${sharedLines}")
    list(APPEND isaImages -DSHARED_IMAGES_FILE=${CMAKE_BINARY_DIR}/helios_generated/isa_shared_images.txt)
  endif()
  # isa_levels.txt is written by helios_isa_finalize(), which runs right after this file.
  helios_lint_test(lint_isa_audit COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} ${isaImages}
    -DCOMPILE_COMMANDS=${CMAKE_BINARY_DIR}/compile_commands.json
    -DLEVELS=${CMAKE_BINARY_DIR}/helios_generated/isa_levels.txt -DREQUIRE_GATE=ON
    -P ${LINT}/isa_audit.cmake)

  # Check 1's seeded violations: each fixture compile database (the ok one plus one seeded unit, against
  # the fixture level map tests/isa/levels.txt) must be rejected with its diagnostic.
  set(isaLevels -DLEVELS=${LINT_TESTS}/isa/levels.txt)
  foreach(case
      "default_level|noise.cpp .helios_math.: avx2 unit built below its image's level .missing: avx2, bmi, bmi2, lzcnt, popcnt, f16c."
      "partial_level|PhysicsSystem.cpp .tp_jolt.: avx2 unit built below its image's level .missing: bmi2."
      "msvc_default|world.cpp .helios_world.: avx2 unit built below its image's level .missing: /arch:AVX2."
      "clang_cl_fma|world.cpp .helios_world.: FMA enabled"
      "extra_flag|world.cpp .helios_world.: flags outside the avx2 level set .-msha -mcx16."
      "no_level|target 'helios_mystery' has no ISA level"
      "base_avx2|entropy_common.c .tp_zstd.base.: baseline unit .base image. compiled with AVX-class flags .avx2, bmi, bmi2, f16c, lzcnt."
      "gate_avx|baseline unit .CPU gate. compiled with AVX-class flags"
      "gate_sse42|baseline unit .CPU gate. compiled with flags above x86-64-v1 .-mpopcnt -msse4.2."
      "gate_march|baseline unit .CPU gate. compiled with flags above x86-64-v1 .-march=nehalem."
      "gate_in_avx2|cpu_gate.c .helios_core.: CPU-gate unit compiled in a target of level 'avx2'"
      "kernel_fma|FMA enabled"
      "kernel_avx512|AVX-512 enabled"
      "march_native|-march=native"
      "fast_math|fast-math"
      "contract|FP contraction enabled"
      "msvc_arch|log.cpp .helios_core.base.: baseline unit .base image. compiled with AVX-class flags .avx, avx2."
      "clang_cl_forwarded|window.cpp .fx_launcher.: baseline unit .base image. compiled with AVX-class flags .avx2, bmi2."
      "base_default|ISA audit failed .1 violation.*log.cpp .helios_core.base.: baseline unit .base image. built without its level set .missing: -march=x86-64, -mtune=generic."
      "gate_default|ISA audit failed .1 violation.*cpu_gate.c .helios_core_cpugate.: baseline unit .CPU gate. built without its level set .missing: -march=x86-64, -mtune=generic."
      "gate_protector|ISA audit failed .1 violation.*cpu_gate_hook.c .helios_core_cpugate_hook.: baseline unit .CPU gate. built without its level set .missing: -fno-stack-protector."
      "msvc_gate_gs|ISA audit failed .1 violation.*cpu_gate.c .helios_core_cpugate.: baseline unit .CPU gate. built without its level set .missing: /GS-."
      "gate_sanitize|ISA audit failed .1 violation.*cpu_gate.c .helios_core_cpugate.: CPU-gate unit built with sanitizer instrumentation .-fsanitize=address.")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 fixture)
    list(GET parts 1 expect)
    helios_lint_test(lint_isa_fixture_${fixture} EXPECT_FAIL "${expect}"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaLevels} -DSKIP_OBJECTS=ON
              -DCOMPILE_COMMANDS=${LINT_TESTS}/isa/${fixture}.json -P ${LINT}/isa_audit.cmake)
  endforeach()
  helios_lint_test(lint_isa_fixture_ok COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaLevels} -DSKIP_OBJECTS=ON
    -DREQUIRE_GATE=ON -DCOMPILE_COMMANDS=${LINT_TESTS}/isa/ok.json -P ${LINT}/isa_audit.cmake)

  # Check 2's disassembly and symbol checks must catch what a gate object may not contain. Each fixture
  # is an ordinary avx2 library (never a gate one, or lint_isa_audit would audit it as a gate unit),
  # audited as if it were a gate object.
  if(isaTools MATCHES "OBJDUMP")
    add_library(lint_isa_fixture_kernel OBJECT ${LINT_TESTS}/isa/fixture_kernel_avx2.c)
    # CMPXCHG16B behind a lock prefix (the assembler accepts it at any level).
    add_library(lint_isa_fixture_prefixed OBJECT ${LINT_TESTS}/isa/fixture_cx16.c)
    set_target_properties(lint_isa_fixture_kernel lint_isa_fixture_prefixed PROPERTIES FOLDER tests)
    add_custom_target(lint_isa_fixture_kernel_build ALL DEPENDS lint_isa_fixture_kernel lint_isa_fixture_prefixed)
    helios_lint_test(lint_isa_disasm_detects_avx EXPECT_FAIL "instruction not allowed at the x86-64-v1 baseline"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
              "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_kernel>" -P ${LINT}/isa_audit.cmake)
    helios_lint_test(lint_isa_disasm_detects_prefixed EXPECT_FAIL "not allowed at the x86-64-v1 baseline.*cmpxchg16b"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
              "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_prefixed>" -P ${LINT}/isa_audit.cmake)
    # A stack-protector reference and AddressSanitizer references (02 §1.1: no __security_cookie or
    # __asan_* in the gate objects). GCC and Clang on ELF; MSVC's /GS cookie is WP-0.2r part 2's.
    if(NOT WIN32 AND isaTools MATCHES "NM")
      add_library(lint_isa_fixture_cookie OBJECT ${LINT_TESTS}/isa/gate_stack_protector.c)
      target_compile_options(lint_isa_fixture_cookie PRIVATE -fstack-protector-all)
      add_library(lint_isa_fixture_asan OBJECT ${LINT_TESTS}/isa/gate_asan.c)
      target_compile_options(lint_isa_fixture_asan PRIVATE -fsanitize=address)
      set_target_properties(lint_isa_fixture_cookie lint_isa_fixture_asan PROPERTIES FOLDER tests)
      add_dependencies(lint_isa_fixture_kernel_build lint_isa_fixture_cookie lint_isa_fixture_asan)
      helios_lint_test(lint_isa_object_detects_stack_protector
        EXPECT_FAIL "references '__stack_chk_fail': the gate objects run before the CRT"
        COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
                "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_cookie>" -P ${LINT}/isa_audit.cmake)
      helios_lint_test(lint_isa_object_detects_asan EXPECT_FAIL "references '__asan_[a-z_0-9]+': the gate objects run"
        COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=object
                "-DOBJECT=$<TARGET_OBJECTS:lint_isa_fixture_asan>" -P ${LINT}/isa_audit.cmake)
    endif()
  endif()
  # Check 3's canary for shared libraries (ELF, both link flavours): a shared library with an IFUNC, whose
  # R_X86_64_IRELATIVE relocation the dynamic linker would apply before a gated executable's .preinit_array.
  if(isaTools MATCHES "READELF" AND NOT WIN32 AND NOT HELIOS_SANITIZE)
    add_library(lint_isa_fixture_ifunc SHARED ${LINT_TESTS}/isa/shared_ifunc.c)
    set_target_properties(lint_isa_fixture_ifunc PROPERTIES FOLDER tests)
    helios_lint_test(lint_isa_shared_image_detects_ifunc
      EXPECT_FAIL "liblint_isa_fixture_ifunc.so: R_X86_64_IRELATIVE relocation in a shared library that gated executables load"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=shared_image
              "-DIMAGE=$<TARGET_FILE:lint_isa_fixture_ifunc>" -P ${LINT}/isa_audit.cmake)
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
  # Check 4: the base fixture image passes on its own (zstd's listed BMI2 decoders included), and the
  # seeded canary (an unlisted AVX2 symbol in a base image, 09 §2 WP-0.2r) fails, naming only its symbol.
  if(isaElfImages AND TARGET lint_isa_fixture_base)
    helios_lint_test(lint_isa_base_image COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=base_image
      "-DIMAGE=$<TARGET_FILE:lint_isa_fixture_base>" -P ${LINT}/isa_audit.cmake)
    helios_executable(lint_isa_fixture_base_canary ROLE tool ISA base SOURCES ${LINT_TESTS}/isa/base_canary.c)
    set_target_properties(lint_isa_fixture_base_canary PROPERTIES FOLDER tests)
    helios_lint_test(lint_isa_base_image_detects_avx2
      EXPECT_FAIL "base image: 1 symbol.s. with instructions above x86-64-v1 that are not on HELIOS_ISA_SELF_DISPATCH_SYMBOLS: helios_isa_canary_avx2 .[0-9]+ instruction"
      COMMAND ${CMAKE_COMMAND} ${isaCommon} ${isaTools} -DMODE=base_image
              "-DIMAGE=$<TARGET_FILE:lint_isa_fixture_base_canary>" -P ${LINT}/isa_audit.cmake)
  endif()
endif()

# Check 4's complement (MODE=base_sources): check 4 reads TZCNT's encoding as BSF, so the Helios base
# modules' include/ and src/ directories may hold no BMI target attribute or pragma, TZCNT intrinsic or
# TZCNT assembly. A text scan, so it runs on every toolchain; the seeded fixture holds each form once.
if(isaX86)
  set(baseSourceDirs "")
  foreach(m IN LISTS HELIOS_ISA_BASE_MODULES)
    if(TARGET helios_${m})
      get_target_property(dir helios_${m} SOURCE_DIR)
      foreach(sub include src)
        if(IS_DIRECTORY "${dir}/${sub}")
          list(APPEND baseSourceDirs "${dir}/${sub}")
        endif()
      endforeach()
    endif()
  endforeach()
  if(baseSourceDirs)
    string(REPLACE ";" "|" baseSourceDirs "${baseSourceDirs}")
    helios_lint_test(lint_isa_base_sources COMMAND ${CMAKE_COMMAND} ${isaCommon} -DMODE=base_sources
      "-DSOURCE_DIRS=${baseSourceDirs}" -P ${LINT}/isa_audit.cmake)
  endif()
  helios_lint_test(lint_isa_base_sources_detects_tzcnt
    EXPECT_FAIL "ISA audit failed .4 finding.s. in base-module sources.:.*tzcnt_bmi.c:7: a target attribute or pragma that enables BMI.*tzcnt_bmi.c:9: a target attribute or pragma that enables BMI.*tzcnt_bmi.c:10: a TZCNT intrinsic.*tzcnt_bmi.c:15: TZCNT in inline assembly"
    COMMAND ${CMAKE_COMMAND} ${isaCommon} -DMODE=base_sources -DSOURCE_DIRS=${LINT_TESTS}/isa/base_sources
            -P ${LINT}/isa_audit.cmake)
endif()

# ---------------------------------------------------------------------------------------------
# Link-model symbol audit (02 §1.4, ADR-016, WP-0.6c): every build runs the recorded-listing fixtures (ELF
# `nm` and PE `dumpbin /exports` output); a modular build (HELIOS_MODULAR=ON) also audits its own images.
# ---------------------------------------------------------------------------------------------
set(symLint -DPOLICY=${LINT}/symbol_audit_policy.cmake -P ${LINT}/symbol_audit.cmake)
foreach(fixture elf_ok pe_ok)
  helios_lint_test(lint_symbol_audit_fixture_${fixture} COMMAND ${CMAKE_COMMAND}
    -DFIXTURE=${LINT_TESTS}/symbols/${fixture} ${symLint})
endforeach()
# A known finding (policy HELIOS_SYMBOL_KNOWN_FINDINGS) is reported and passes; constants and typeinfo in
# .data.rel.ro are not state.
helios_lint_test(lint_symbol_audit_fixture_elf_known COMMAND ${CMAKE_COMMAND}
  -DFIXTURE=${LINT_TESTS}/symbols/elf_known ${symLint})
set_tests_properties(lint_symbol_audit_fixture_elf_known PROPERTIES
  PASS_REGULAR_EXPRESSION "1 known finding.*TypeOf.*owner WP-0.6c part 2.*2 image.s. passed")
foreach(case
    "elf_group_export|failed .3 finding.*R1 helios_runtime exports 'mi_malloc' .T.*R1 helios_runtime exports 'ZSTD_compress' .T.*R1 helios_runtime exports '_ZN3JPH7Factory9sInstanceE' .D."
    "elf_singleton|R2 SDL3 is in two images, helios_client and rhi_tests .'SDL_Init'."
    "elf_duplicate_state|failed .1 finding.*R3 ecs_tests has its own copy of '_ZZN6helios3ecs11componentIdINS0_8PositionEEEjvE2id' .b, .bss., which helios_runtime defines"
    "elf_group_duplicate_state|failed .1 finding.*R3 group helios_client also defines '_ZZN6helios3ecs8typeSlotINS0_11NetIdentityEEEjvE4slot' .u, .bss., which helios_runtime defines"
    "elf_group_weak_state|failed .2 finding.*R1 helios_runtime exports '_ZN3JPH9Character7sNextIDE' .u, .data., mutable data that is not Helios code.*R1 helios_runtime exports 'LogPcg' .u, .bss."
    "elf_game|failed .5 finding.*R6 game image game_bad has its own strong definition of '_ZN6helios3log5write.*R4 game image game_bad defines 'mi_malloc' from mimalloc.*R5 game image game_bad defines Helios data '_ZN6helios5probe8g_countsE' .B..*R4 game image game_bad defines '_Z12lua_pushnilP9lua_State' from Luau.*R5 game image game_bad defines a per-image ECS type key '_ZZN6helios3ecs6detail15perImageTypeKeyIN12_GLOBAL__N_18CooldownEEEmvE3key'"
    "elf_client_luau|failed .1 finding.*R1 helios_client exports '_Z12lua_pushnilP9lua_State' .T."
    "pe_c_export|failed .2 finding.*P1 helios_runtime exports 'mi_malloc', a C name that is not helios_.*P1 helios_runtime exports 'yyjson_read_opts'"
    "pe_no_exports|P1 helios_editor: no exports found")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_symbol_audit_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DFIXTURE=${LINT_TESTS}/symbols/${fixture} ${symLint})
endforeach()

if(HELIOS_MODULAR)
  get_property(symGroups GLOBAL PROPERTY HELIOS_LINK_GROUP_TARGETS)
  get_property(symStandalone GLOBAL PROPERTY HELIOS_SELF_CONTAINED_IMAGES)
  # Game images: link_model_probe keeps the game rules; link_model_bad_game breaks them (fixture below).
  set(symGames link_model_probe)
  set(symFixtures link_model_bad_game)
  set(symLines "")
  foreach(t IN LISTS symGroups)
    string(APPEND symLines "group ${t} $<TARGET_FILE:${t}>\n")
  endforeach()
  # Third-party libraries a group exports by design (cmake/HeliosModular.cmake): R1 accepts what their
  # archives define.
  set(symThirdParty "")
  foreach(entry IN LISTS HELIOS_GROUP_EXPORTED_THIRD_PARTY)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 group)
    list(GET parts 1 lib)
    if(TARGET helios_${group} AND TARGET ${lib})
      string(APPEND symThirdParty "thirdparty helios_${group} ${lib} $<TARGET_FILE:${lib}>\n")
    endif()
  endforeach()
  string(APPEND symLines "${symThirdParty}")
  set(symTool -DFORMAT=elf)
  if(MSVC)
    # PE images have no symbol table: the export tables of the groups (P1). The consumer and game rules
    # run on the Linux modular build (linux-dev).
    get_filename_component(symLinkerDir "${CMAKE_LINKER}" DIRECTORY)
    find_program(HELIOS_DUMPBIN dumpbin HINTS "${symLinkerDir}")
    set(symTool -DFORMAT=pe)
    if(HELIOS_DUMPBIN)
      list(APPEND symTool -DDUMPBIN=${HELIOS_DUMPBIN})
    endif()
  else()
    if(CMAKE_NM)
      list(APPEND symTool -DNM=${CMAKE_NM})
    endif()
    _helios_all_targets(symCandidates)
    foreach(t IN LISTS symCandidates)
      get_target_property(type ${t} TYPE)
      get_target_property(excluded ${t} EXCLUDE_FROM_ALL)
      # Base images (02 §1.1: the launcher, the bootstrap and the ISA audit's fixtures) link `.base` copies of
      # their modules and load no group (cmake/HeliosIsa.cmake), like the self-contained images.
      get_target_property(level ${t} HELIOS_ISA_LEVEL)
      if(NOT type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$" OR excluded OR t IN_LIST symGroups
         OR t IN_LIST symStandalone OR t IN_LIST symFixtures OR level STREQUAL "base")
        continue()
      endif()
      if(t IN_LIST symGames)
        string(APPEND symLines "game ${t} $<TARGET_FILE:${t}>\n")
        continue()
      endif()
      # A consumer: an image that links a module (and so its group library).
      _helios_find_path("${t}" _helios_is_module symPath)
      if(symPath)
        string(APPEND symLines "consumer ${t} $<TARGET_FILE:${t}>\n")
      endif()
    endforeach()
  endif()
  # Without its tool (nm, dumpbin) the audit fails and says so: a modular build never skips it.
  set(symImages ${CMAKE_BINARY_DIR}/helios_generated/symbol_images_$<CONFIG>.txt)
  file(GENERATE OUTPUT ${symImages} CONTENT "${symLines}")
  helios_lint_test(lint_symbol_audit COMMAND ${CMAKE_COMMAND} -DIMAGES_FILE=${symImages} ${symTool}
    -DWORK_DIR=${LINT_WORK}/symbols ${symLint})
  set_tests_properties(lint_symbol_audit PROPERTIES TIMEOUT 900)
  if(TARGET link_model_bad_game)
    # The game rules against a real image (ELF): each rule must fire on link_model_bad_game.
    set(symBad "")
    foreach(t IN LISTS symGroups)
      string(APPEND symBad "group ${t} $<TARGET_FILE:${t}>\n")
    endforeach()
    string(APPEND symBad "${symThirdParty}game link_model_bad_game $<TARGET_FILE:link_model_bad_game>\n")
    set(symBadImages ${CMAKE_BINARY_DIR}/helios_generated/symbol_images_bad_game_$<CONFIG>.txt)
    file(GENERATE OUTPUT ${symBadImages} CONTENT "${symBad}")
    foreach(case
        "r4|R4 game image link_model_bad_game defines '[^']*' from mimalloc"
        "r5|R5 game image link_model_bad_game defines Helios data '_ZN6helios5probe7g_ticksE'"
        "r5_type_key|R5 game image link_model_bad_game defines a per-image ECS type key '_ZZN6helios3ecs6detail15perImageTypeKeyIN12_GLOBAL__N_115PrivateCooldownE"
        "r6|R6 game image link_model_bad_game has its own strong definition of '_ZN6helios13memoryTagNameENS_9MemoryTagE'")
      string(REPLACE "|" ";" parts "${case}")
      list(GET parts 0 rule)
      list(GET parts 1 expect)
      helios_lint_test(lint_symbol_audit_game_${rule} EXPECT_FAIL "${expect}"
        COMMAND ${CMAKE_COMMAND} -DIMAGES_FILE=${symBadImages} ${symTool} -DWORK_DIR=${LINT_WORK}/symbols_${rule}
                ${symLint})
    endforeach()
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
# engine/docs/concept/ are engine code. project_file: the top-level helios.project.jsonc is the reference
# game's (reference names pass); project_file_title: a franchise title fails there, and a reference name
# still fails in any other top-level file. franchise_in_record: records (.hrec) are scanned.
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
    "identifier_in_code|IP-name lint failed .4 finding"
    "franchise_in_record|content/records/hull/raider.hrec:4: franchise name 'Tatooine'"
    "project_file_title|failed .2 finding.*helios.project.jsonc:3: franchise title 'Star Wars'.*notes.md:1: reference-content name 'Kestrel'")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  helios_lint_test(lint_ip_names_fixture_${fixture} EXPECT_FAIL "${expect}"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${LINT_TESTS}/ip_names/${fixture} -DLINT_POLICY=${LINT}/ip_names_policy.cmake
            -P ${LINT}/ip_names.cmake)
endforeach()
foreach(fixture clean concept_names project_file)
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
# Module layering and ISA levels (02 §1.1): a fixture project configured once per case. Good graphs
# configure; each seeded violation must stop configure with its diagnostic. The isa_* cases check the
# image levels of cmake/HeliosIsa.cmake: base copies (a generated source included), and the configure
# errors for a base image that links an avx2 library (tp_jolt, physics, tp_jolt through core) or compiles
# in an avx2 object library's objects, an ISA override outside the fixtures or with an unknown level,
# CPU_GATE on a base image, a level set by hand on a library, a gate target that is not an object library,
# and ISA options on a library's interface.
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
    "unknown_role|helios_executable.fx-zonehost.: no ROLE given and none is known for.*'apps/zonehost/'"
    "isa_base_ok|Helios ISA levels: 1 base image.s.: fx-launcher .helios_core.base, tp_yyjson.base..*HELIOS_FIXTURE_CONFIGURE_OK"
    "isa_fixture_base|Helios ISA levels: 1 base image.s.: fx-isa-fixture .helios_core.base, tp_yyjson.base..*HELIOS_FIXTURE_CONFIGURE_OK"
    "isa_base_links_jolt|ISA level check failed .1 violation.*helios isa: base image 'fx-launcher' links 'tp_jolt', which is built only at avx2: fx-launcher -> tp_jolt"
    "isa_base_links_physics|helios isa: base image 'fx-bootstrap' links 'helios_physics', which is built only at avx2: fx-bootstrap -> fx_helper -> helios_physics"
    "isa_avx2_links_copy|helios isa: 'fx-client' .avx2. links the base copy 'helios_core.base': only base images link base copies"
    "isa_override_in_apps|helios_executable.fx-isa-app.: ISA is for the ISA audit's fixtures only.*'apps/isa/'"
    "isa_interface_options|helios isa: 'tp_jolt' carries ISA compile options on its interface .-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c."
    "isa_override_elsewhere|helios_executable.fx-isa-elsewhere.: ISA is for the ISA audit's[ \n]+fixtures[ \n]+only.*isa_elsewhere.cmake"
    "isa_invalid_value|helios_executable.fx-isa-fixture.: ISA is avx2 or base, not 'sse4'"
    "isa_cpu_gate_on_base|helios_executable.fx-launcher.: CPU_GATE on a base image"
    "isa_level_on_library|helios isa: 'helios_math' has HELIOS_ISA_LEVEL 'base', which only helios_executable.. .avx2, base. and helios_cpu_gate_target.. .gate. set"
    "isa_gate_target_not_object|helios_cpu_gate_target.fx_gate.: the CPU gate's units live in[ \n]+OBJECT[ \n]+libraries"
    "isa_base_root_links_jolt|helios isa: base image 'fx-launcher' links 'tp_jolt', which is built only at avx2: fx-launcher -> helios_core -> tp_jolt"
    "isa_base_target_objects|helios isa: base image 'fx-launcher' compiles in the objects of 'fx_kernels' .*, which is built only at avx2"
    # The fixture's build directory is <flavour>_<case> (shipping_ or modular_).
    "isa_generated_source|FX_COPY_SOURCES: [^\n]*/layering/[a-z]+_isa_generated_source/fx_generated.cpp.*HELIOS_FIXTURE_CONFIGURE_OK"
    # Modular builds only (HELIOS_MODULAR=ON): an image that links a module's object library directly.
    "modular:direct_objects|'fx-cook' links the module object library 'helios_core' directly"
    "modular:direct_objects_genex|layering check failed .1 violation.*'fx-cook' links the module object library 'helios_core' directly"
    # Modular builds only, with WIN32 set: the base image and its copies get HELIOS_<GROUP>_BUILDING.
    "modular:isa_base_windows|Helios ISA levels: 1 base image.s.: fx-launcher .helios_core.base, tp_yyjson.base..*FX_OWN_COPY: fx-launcher=HELIOS_RUNTIME_BUILDING.HELIOS_CLIENT_BUILDING.HELIOS_EDITOR_BUILDING helios_core.base=HELIOS_RUNTIME_BUILDING.HELIOS_CLIENT_BUILDING.HELIOS_EDITOR_BUILDING\n.*HELIOS_FIXTURE_CONFIGURE_OK")
  string(REPLACE "|" ";" parts "${case}")
  list(GET parts 0 fixture)
  list(GET parts 1 expect)
  # Every case runs in both link flavours (ADR-016): the checks must see the same graph whether
  # helios::<module> names a static library or a link group's interface (cmake/HeliosModular.cmake).
  set(flavours "shipping;modular")
  if(fixture MATCHES "^modular:(.*)$")
    set(fixture "${CMAKE_MATCH_1}")
    set(flavours modular)
  endif()
  foreach(flavour IN LISTS flavours)
    if(flavour STREQUAL "modular")
      set(name lint_layering_modular_${fixture})
      set(modular ON)
    else()
      set(name lint_layering_${fixture})
      set(modular OFF)
    endif()
    add_test(NAME ${name}
      COMMAND ${CMAKE_COMMAND} -S ${LINT_TESTS}/layering -B ${LINT_WORK}/layering/${flavour}_${fixture}
              ${layeringGenerator} -DHELIOS_FIXTURE_CASE=${fixture} -DHELIOS_SOURCE_DIR=${PROJECT_SOURCE_DIR}
              -DHELIOS_MODULAR=${modular})
    set_tests_properties(${name} PROPERTIES LABELS lint TIMEOUT 120 PASS_REGULAR_EXPRESSION "${expect}")
    if(expect MATCHES "HELIOS_FIXTURE_CONFIGURE_OK")
      set_tests_properties(${name} PROPERTIES FAIL_REGULAR_EXPRESSION "helios (layering|isa):")
    else()
      set_tests_properties(${name} PROPERTIES FAIL_REGULAR_EXPRESSION "HELIOS_FIXTURE_CONFIGURE_OK")
    endif()
  endforeach()
endforeach()

# RC-1's shipped-pipelines lint (WP-0.12), registered by its owner.
include(${PROJECT_SOURCE_DIR}/tools/rendertest/tests/shipped_pipelines_tests.cmake)
