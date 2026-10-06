# Helios build helpers: engine modules, executables and tests (02 §1.1, WP-0.2).
#
# helios_declare_module(<name> <layer> [HEADLESS] [EDITOR_ONLY] [PEERS <module>...])
#   Records a row of 02 §1.1's layering table (engine/CMakeLists.txt holds the full table). A module
#   declared here gets its LAYER, flags and allowed same-layer peers from the row, so module
#   CMakeLists.txt files need not repeat them. A module has at most one row: declaring it again (for
#   example from its own CMakeLists.txt, with other peers or flags) fails configure.
#
# helios_module(<name> [HEADLESS] [EDITOR_ONLY] [LAYER <n>] [PEERS <module>...]
#               SOURCES ... DEPS ... PRIVATE_DEPS ...)
#   Declares engine module library `helios_<name>` with alias `helios::<name>`. Public headers live in
#   engine/<name>/include/helios/<name>/..., sources in engine/<name>/src. Shipping builds make it a
#   STATIC library; modular dev builds (HELIOS_MODULAR=ON) make it an OBJECT library inside its link
#   group's shared library, and helios::<name> links that library (cmake/HeliosModular.cmake, ADR-016).
#   Either way consumers link helios::<name>, and the module's own CMakeLists.txt configures
#   helios_<name> (private sources, definitions, dependencies).
#   * LAYER is optional when the module has a helios_declare_module row (it must then match).
#   * HEADLESS: linked by the cell server; may never reach a non-HEADLESS module or a graphics
#     third-party library (rhi/render/ui/audio/app/input, SDL3, ImGui, volk, ...).
#   * EDITOR_ONLY: never linked by the client, launcher, bot, cell, gateway or voice executables, and
#     never a dependency of a module that is not EDITOR_ONLY itself.
#   A module with a table row takes its flags and peers from the row; the call may repeat them but
#   may not add a flag or a peer the row lacks (like a contradicting LAYER, that fails configure).
#   PEERS in the call is for a module without a row, which then passes LAYER too.
#
# helios_executable(<name> [ROLE <role>] [ISA avx2|base] [CPU_GATE|NO_CPU_GATE] SOURCES ... DEPS ...)
#   ROLE is one of client launcher bootstrap bot cell gateway voice editor tool sample bench. When
#   omitted it is inferred from the directory (apps/client -> client, apps/cellserver -> cell,
#   apps/tools/* -> tool, apps/samples -> sample, engine/*/bench -> bench, ...); a directory with no
#   known role and no ROLE is a configure error, so every executable gets a role check. Client and server
#   roles may not link EDITOR_ONLY modules; cell/gateway/voice/bot may link only HEADLESS modules.
#   The role also picks the image's ISA level (02 §1.1, cmake/HeliosIsa.cmake): launcher and bootstrap
#   are `base` (x86-64-v1) and link `.base` copies of their modules; every other role is `avx2`, and so
#   is everything it links. ISA overrides the role's level for the ISA audit's fixtures only: it is a
#   configure error outside tools/lint/ (the calling listfile) and under apps/ and engine/, where the real
#   images live.
#   Windows executables get the Helios manifest (helios_windows_manifest). The CPU gate (a
#   pre-initializer that refuses CPUs without AVX2, 02 §1.1 / 08 §2.2) is linked into every avx2 image of
#   a gate role (client cell gateway voice editor bot tool) unless NO_CPU_GATE; CPU_GATE forces it into
#   any avx2 image. A base image never carries it. (Modular Windows builds carry the gate in
#   helios_runtime.dll instead; see helios_cpu_gate.)
#
# helios_test(<name> SOURCES ... DEPS ...)
#   Declares a doctest executable registered with CTest. Tests link runtime modules, so they are avx2
#   images (02 §1.1), like every target that is not a base image, a base copy or a gate object library.
#
# Configure-time checks (run once, deferred to the end of the top-level CMakeLists.txt): a module
# depends only on lower layers or listed peers; the module graph is acyclic; HELIOS_MODULE_ORDER lists
# every module in a topological order; the HEADLESS and EDITOR_ONLY rules above. Any violation stops
# configure with a message that names the dependency path.

include_guard(GLOBAL)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
include(HeliosIsa)
include(HeliosWindows)
include(HeliosModular)

# Third-party targets a HEADLESS module or a server executable must never reach (graphics, windowing,
# audio, UI). Real target names; aliases are resolved before the comparison.
set_property(GLOBAL PROPERTY HELIOS_GRAPHICS_THIRD_PARTY
  tp_volk tp_vma tp_vulkan_headers tp_imgui tp_imgui_node_editor SDL3-static SDL3-shared tp_sdl3
  tp_miniaudio tp_rmlui tp_freetype tp_harfbuzz tp_sheenbidi tp_libunibreak tp_opus)

# Roles (see helios_executable) and the rules that apply to them.
set_property(GLOBAL PROPERTY HELIOS_ROLES_NO_EDITOR_ONLY client launcher bootstrap bot cell gateway voice)
set_property(GLOBAL PROPERTY HELIOS_ROLES_HEADLESS_ONLY bot cell gateway voice)
set_property(GLOBAL PROPERTY HELIOS_ROLES_CPU_GATE client cell gateway voice editor bot tool)
set_property(GLOBAL PROPERTY HELIOS_ROLES_ISA_BASE launcher bootstrap)

function(helios_apply_warnings target)
  if(MSVC)
    # /permissive- and /Zc:__cplusplus are C++-only; C sources (the CPU gate) must not see them.
    target_compile_options(${target} PRIVATE /W4 /utf-8 /fp:precise
                           $<$<COMPILE_LANGUAGE:CXX>:/permissive- /Zc:__cplusplus>
                           $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/Zc:preprocessor>)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(HELIOS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    # Keep float results identical to MSVC's default (/fp:precise, no contraction) so procedural
    # generation and simulation stay bit-identical between Windows clients and Linux servers.
    target_compile_options(${target} PRIVATE -ffp-contract=off)
    if(HELIOS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# Module table and declarations
# ---------------------------------------------------------------------------------------------
function(helios_declare_module name layer)
  cmake_parse_arguments(D "HEADLESS;EDITOR_ONLY" "" "PEERS" ${ARGN})
  get_property(existing GLOBAL PROPERTY HELIOS_DECL_${name}_LAYER SET)
  if(existing)
    message(FATAL_ERROR "Module layering check failed:\n  helios layering: module '${name}' already has a row in the "
                        "layering table (engine/CMakeLists.txt, 02 §1.1). A second helios_declare_module(${name}) "
                        "would replace its layer, flags or peers\n")
  endif()
  if(NOT layer MATCHES "^[1-5]$")
    message(FATAL_ERROR "helios_declare_module(${name}): layer must be 1..5 (02 §1.1), got '${layer}'")
  endif()
  if(D_HEADLESS AND D_EDITOR_ONLY)
    message(FATAL_ERROR "helios_declare_module(${name}): HEADLESS and EDITOR_ONLY are exclusive")
  endif()
  set_property(GLOBAL PROPERTY HELIOS_DECL_${name}_LAYER ${layer})
  set_property(GLOBAL PROPERTY HELIOS_DECL_${name}_HEADLESS ${D_HEADLESS})
  set_property(GLOBAL PROPERTY HELIOS_DECL_${name}_EDITOR_ONLY ${D_EDITOR_ONLY})
  set_property(GLOBAL PROPERTY HELIOS_DECL_${name}_PEERS ${D_PEERS})
  set_property(GLOBAL APPEND PROPERTY HELIOS_DECLARED_MODULES ${name})
endfunction()

function(helios_module name)
  cmake_parse_arguments(M "HEADLESS;EDITOR_ONLY" "LAYER" "SOURCES;DEPS;PRIVATE_DEPS;PEERS" ${ARGN})
  get_property(declLayer GLOBAL PROPERTY HELIOS_DECL_${name}_LAYER)
  get_property(declHeadless GLOBAL PROPERTY HELIOS_DECL_${name}_HEADLESS)
  get_property(declEditorOnly GLOBAL PROPERTY HELIOS_DECL_${name}_EDITOR_ONLY)
  get_property(declPeers GLOBAL PROPERTY HELIOS_DECL_${name}_PEERS)
  # Quoted comparisons: get_property() leaves the variables UNDEFINED for a module without a table
  # row, and an unquoted undefined name would compare as its own literal text (so an explicit LAYER
  # for an undeclared module was reported as contradicting an empty table entry).
  set(layer "${M_LAYER}")
  if("${layer}" STREQUAL "")
    set(layer "${declLayer}")
  elseif(NOT "${declLayer}" STREQUAL "" AND NOT "${layer}" STREQUAL "${declLayer}")
    # Indented lines: CMake does not re-wrap them, so tools can match the text.
    message(FATAL_ERROR "Module layering check failed:\n  helios layering: helios_module(${name} LAYER ${layer}) "
                        "contradicts the layering table (engine/CMakeLists.txt, 02 §1.1), which puts '${name}' on "
                        "layer ${declLayer}\n")
  endif()
  if("${layer}" STREQUAL "")
    message(FATAL_ERROR "Module layering check failed:\n  helios layering: module '${name}' has no layer. Pass LAYER "
                        "<1..5> to helios_module() or add a helios_declare_module() row to engine/CMakeLists.txt "
                        "(02 §1.1)\n")
  endif()
  if(NOT layer MATCHES "^[1-5]$")
    message(FATAL_ERROR "Module layering check failed:\n  helios layering: module '${name}': LAYER must be 1..5, "
                        "got '${layer}'\n")
  endif()
  # The row is the declaration: a call may leave out the row's flags but may not add one it lacks.
  if(NOT "${declLayer}" STREQUAL "")
    foreach(flag HEADLESS EDITOR_ONLY)
      set(rowFlag "${declHeadless}")
      if(flag STREQUAL "EDITOR_ONLY")
        set(rowFlag "${declEditorOnly}")
      endif()
      if(M_${flag} AND NOT rowFlag)
        message(FATAL_ERROR "Module layering check failed:\n  helios layering: helios_module(${name} ${flag}) "
                            "contradicts the layering table (engine/CMakeLists.txt, 02 §1.1), whose row for "
                            "'${name}' is not ${flag}\n")
      endif()
    endforeach()
    foreach(peer IN LISTS M_PEERS)
      if(NOT peer IN_LIST declPeers)
        message(FATAL_ERROR "Module layering check failed:\n  helios layering: helios_module(${name} PEERS ${peer}) "
                            "contradicts the layering table (engine/CMakeLists.txt, 02 §1.1), whose row for "
                            "'${name}' does not list '${peer}' as a peer\n")
      endif()
    endforeach()
  endif()
  set(headless OFF)
  if(M_HEADLESS OR "${declHeadless}")
    set(headless ON)
  endif()
  set(editorOnly OFF)
  if(M_EDITOR_ONLY OR "${declEditorOnly}")
    set(editorOnly ON)
  endif()
  if(headless AND editorOnly)
    message(FATAL_ERROR "Module layering check failed:\n  helios layering: module '${name}' cannot be both HEADLESS "
                        "and EDITOR_ONLY\n")
  endif()

  set(target helios_${name})
  if(HELIOS_MODULAR)
    add_library(${target} OBJECT ${M_SOURCES})
    helios_link_group_of(group ${headless} ${editorOnly})
    _helios_modular_module(${target} ${name} ${group}) # also declares helios::${name}
  else()
    add_library(${target} STATIC ${M_SOURCES})
    add_library(helios::${name} ALIAS ${target})
  endif()
  # The generated export headers (helios/<group>_api.h) are visible to every module and its consumers.
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                              $<BUILD_INTERFACE:${HELIOS_MODULAR_INCLUDE_DIR}>
                                        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(${target} PUBLIC ${M_DEPS} PRIVATE ${M_PRIVATE_DEPS})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER engine
    HELIOS_MODULE_NAME ${name}
    HELIOS_LAYER ${layer}
    HELIOS_HEADLESS ${headless}
    HELIOS_EDITOR_ONLY ${editorOnly}
    HELIOS_PEERS "${declPeers};${M_PEERS}")
  helios_apply_warnings(${target})
  set_property(GLOBAL APPEND PROPERTY HELIOS_MODULE_TARGETS ${target})
endfunction()

# ---------------------------------------------------------------------------------------------
# Executables and tests
# ---------------------------------------------------------------------------------------------
function(_helios_infer_role out)
  file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
  set(role "")
  if(rel MATCHES "^apps/client(/|$)")
    set(role client)
  elseif(rel MATCHES "^apps/launcher/bootstrap(/|$)")
    set(role bootstrap)
  elseif(rel MATCHES "^apps/launcher(/|$)")
    set(role launcher)
  elseif(rel MATCHES "^apps/(cellserver|cell)(/|$)")
    set(role cell)
  elseif(rel MATCHES "^apps/gateway(/|$)")
    set(role gateway)
  elseif(rel MATCHES "^apps/voice(/|$)")
    set(role voice)
  elseif(rel MATCHES "^apps/editor(/|$)")
    set(role editor)
  elseif(rel MATCHES "^apps/bot(/|$)")
    set(role bot)
  elseif(rel MATCHES "^apps/tools(/|$)" OR rel MATCHES "^tools/")
    set(role tool)
  elseif(rel MATCHES "^apps/samples(/|$)")
    set(role sample)
  elseif(rel MATCHES "^engine/[^/]+(/bench)?(/|$)")
    set(role bench)
  endif()
  set(${out} "${role}" PARENT_SCOPE)
endfunction()

function(helios_executable name)
  cmake_parse_arguments(E "CPU_GATE;NO_CPU_GATE" "ROLE;ISA" "SOURCES;DEPS" ${ARGN})
  set(role "${E_ROLE}")
  if(role STREQUAL "")
    _helios_infer_role(role)
  endif()
  set(knownRoles client launcher bootstrap bot cell gateway voice editor tool sample bench)
  if(role STREQUAL "")
    # Without a role no role check (02 §1.1: EDITOR_ONLY, HEADLESS-only servers) would see it.
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    message(FATAL_ERROR "helios_executable(${name}): no ROLE given and none is known for '${rel}/' "
                        "(pass ROLE <one of: ${knownRoles}>, or add the directory to _helios_infer_role "
                        "in cmake/HeliosModule.cmake)")
  elseif(NOT role IN_LIST knownRoles)
    message(FATAL_ERROR "helios_executable(${name}): unknown ROLE '${role}' (one of: ${knownRoles})")
  endif()
  get_property(baseRoles GLOBAL PROPERTY HELIOS_ROLES_ISA_BASE)
  set(level avx2)
  if(role IN_LIST baseRoles)
    set(level base)
  endif()
  if(DEFINED E_ISA)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # The audit's fixtures live in tools/lint (lint_tests.cmake and the layering fixture project); the
    # listfile that calls helios_executable() must be there.
    set(fixtureDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/lint")
    cmake_path(IS_PREFIX fixtureDir "${CMAKE_CURRENT_LIST_FILE}" NORMALIZE inFixtures)
    if(NOT E_ISA MATCHES "^(avx2|base)$")
      message(FATAL_ERROR "helios_executable(${name}): ISA is avx2 or base, not '${E_ISA}'")
    elseif(rel MATCHES "^(apps|engine)(/|$)")
      message(FATAL_ERROR "helios_executable(${name}): ISA is for the ISA audit's fixtures only; the level of an "
                          "image under '${rel}/' comes from its ROLE (${role}: ${level}; 02 §1.1)")
    elseif(NOT inFixtures)
      message(FATAL_ERROR "helios_executable(${name}): ISA is for the ISA audit's fixtures only, which are declared "
                          "under tools/lint/, not in ${CMAKE_CURRENT_LIST_FILE}; the level of an image comes from "
                          "its ROLE (${role}: ${level}; 02 §1.1)")
    endif()
    set(level ${E_ISA})
  endif()
  add_executable(${name} ${E_SOURCES})
  target_link_libraries(${name} PRIVATE ${E_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER apps HELIOS_APP_ROLE "${role}" HELIOS_ISA_LEVEL ${level})
  helios_apply_warnings(${name})
  helios_windows_manifest(${name})
  get_property(gateRoles GLOBAL PROPERTY HELIOS_ROLES_CPU_GATE)
  if(E_CPU_GATE AND NOT level STREQUAL "avx2")
    message(FATAL_ERROR "helios_executable(${name}): CPU_GATE on a ${level} image (the gate guards avx2 images)")
  elseif(level STREQUAL "avx2" AND (E_CPU_GATE OR (role IN_LIST gateRoles AND NOT E_NO_CPU_GATE)))
    helios_cpu_gate(${name})
  endif()
  set_property(GLOBAL APPEND PROPERTY HELIOS_APP_TARGETS ${name})
endfunction()

function(helios_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEPS" ${ARGN})
  if(NOT HELIOS_BUILD_TESTS)
    return()
  endif()
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE helios::tp::doctest ${T_DEPS})
  set_target_properties(${name} PROPERTIES FOLDER tests HELIOS_APP_ROLE test)
  helios_apply_warnings(${name})
  # Tests run with the same manifest as shipped executables (UTF-8 code page, long paths), so
  # Windows-only behaviour that depends on it is exercised by the unit tests.
  helios_windows_manifest(${name})
  # Timing gates ("perf: ..." test cases) cannot share a loaded machine with other tests, so they are
  # split into <name>_perf: labelled `perf`, run serially, skipped by PR CI and run nightly. The main
  # entry excludes them. doctest passes when a filter matches no test cases.
  add_test(NAME ${name} COMMAND ${name} "--test-case-exclude=perf:*")
  add_test(NAME ${name}_perf COMMAND ${name} "--test-case=perf:*")
  set_tests_properties(${name}_perf PROPERTIES LABELS perf RUN_SERIAL TRUE)
endfunction()

# ---------------------------------------------------------------------------------------------
# Repository-wide checks, run once after every target exists.
# ---------------------------------------------------------------------------------------------
include(HeliosLayering)

get_property(_heliosFinalizeScheduled GLOBAL PROPERTY _HELIOS_FINALIZE_SCHEDULED)
if(NOT _heliosFinalizeScheduled)
  set_property(GLOBAL PROPERTY _HELIOS_FINALIZE_SCHEDULED ON)
  cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} ID helios_finalize CALL helios_finalize_build)
endif()
unset(_heliosFinalizeScheduled)
