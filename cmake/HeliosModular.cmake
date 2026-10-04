# Link model: shipping (monolithic) and dev (modular) builds (ADR-016, 02 §1.4, WP-0.6c).
#
# HELIOS_MODULAR (declared in the top-level CMakeLists.txt, before project(), because it picks the MSVC
# runtime) selects the flavour:
#
#   OFF (shipping, the default). helios_module() declares a STATIC library per module and every image
#       links the modules it uses. /MT on MSVC. This file only writes the export headers, whose macros
#       expand to nothing.
#   ON (dev). Each module is an OBJECT library, and the modules form three shared libraries, one per
#       link group (02 §1.4 "Link groups"):
#         helios_runtime  every HEADLESS module (core ... authority, server, netgame, clientcore)
#         helios_client   every module that is neither HEADLESS nor EDITOR_ONLY (rhi, render, ...)
#         helios_editor   every EDITOR_ONLY module (assetpipe, toolsfw, editorui, edtools)
#       Each third-party library a module links is linked into that module's group, so it has one copy.
#       /MD for every image (CMAKE_MSVC_RUNTIME_LIBRARY in the top-level CMakeLists.txt).
#
# Consumers are unchanged: they link helios::<module>. In a modular build that alias names an INTERFACE
# target, helios_<module>_api, which carries the module's compile usage requirements ($<COMPILE_ONLY:...>,
# never its objects) and links the module's group library. A module of the same group gets only the
# compile requirements (its objects already sit in the group), which keeps the group libraries free of
# self-links and cycles. The layering checks (HeliosLayering.cmake) map helios_<module>_api back to the
# module, so they see the same module graph in both flavours.
#
# Exports (the spike's part-1 decision, docs/adr/ADR-0.6c-link-model-spike.md):
#   * Every external symbol the group's own (Helios) objects define is exported: MSVC through CMake's
#     WINDOWS_EXPORT_ALL_SYMBOLS, which scans only the target's own objects; ELF through default
#     visibility for module code. Symbols of the third-party archives a group links are not exported
#     (WINDOWS_EXPORT_ALL_SYMBOLS does not scan archives; ELF: --exclude-libs,ALL), except the libraries
#     in HELIOS_GROUP_EXPORTED_THIRD_PARTY (Luau's VM). The symbol audit (tools/lint/symbol_audit.cmake,
#     CTest lint_symbol_audit) checks both.
#   * Everything that is not module code (tools, tests, apps, fixtures) builds with -fvisibility=hidden
#     -fvisibility-inlines-hidden, so on Linux an image keeps its own copy of header-defined inline and
#     template statics, as every image does on Windows. The audit reports such statics that a group also
#     defines (02 §1.4 "No per-image caches of global state").
#   * helios/<group>_api.h (generated into ${CMAKE_BINARY_DIR}/helios_generated/include) defines
#     HELIOS_<GROUP>_API: __declspec(dllexport) while building the group and __declspec(dllimport) when
#     consuming it on Windows, visibility("default") elsewhere, empty in shipping builds. It is required on
#     data that code outside the group references (MSVC cannot import data without dllimport) and on C
#     entry points; functions are exported without it.
#
# helios_self_contained(<target>)
#   In a modular build, <target> carries its own copy of every module it reaches (their objects and
#   third-party libraries, as a shipping image does) instead of linking the group libraries. Two kinds
#   of image need it:
#     * a build-time tool whose output feeds a module (helios-schemac generates gameplay's sources), which
#       would otherwise depend on its own output through helios_runtime (a target cycle);
#     * a white-box test or bench that calls a module's third-party library directly (flecs, Luau,
#       mimalloc, netcode): a group never exports third-party code (02 §1.4, symbol audit R1/P1).
#   The module closure is computed at the end of configure (helios_modular_finalize). Such an image shares
#   no state with the groups and is not audited. A static library marked self-contained only stops its
#   helios::<module> links from naming the groups (its consumers decide). No-op in shipping builds.
#
# Toolchains: MSVC and clang-cl (windows-msvc-dev), GCC and Clang on Linux (linux-dev). MinGW builds only
# the shipping flavour: the cross build is a portability check of what ships, and the dev flavour's
# Windows behaviour is MSVC's (09 §5.6).

include_guard(GLOBAL)

set(HELIOS_LINK_GROUPS runtime client editor)

# Third-party libraries whose API a group exports: <group>|<target>. Their objects are linked into the
# group as its own objects, so they are exported like module code (WINDOWS_EXPORT_ALL_SYMBOLS on MSVC; ELF:
# not hidden by --exclude-libs). Every other third-party library stays hidden inside its group.
#   Luau.VM: engine/script's public API (helios/script/binding.h) is the Luau C API, and modules of the
#   other groups (toolsfw's automation bindings) and generated binding glue call it. The VM must stay one
#   copy (02 §1.4), so helios_runtime exports it; tools/lint/symbol_audit_policy.cmake lists the names.
set(HELIOS_GROUP_EXPORTED_THIRD_PARTY "runtime|Luau.VM")
set(HELIOS_MODULAR_INCLUDE_DIR "${CMAKE_BINARY_DIR}/helios_generated/include")

# Sets <out> to the link group of a module with the given flags (02 §1.4). EDITOR_ONLY wins over HEADLESS:
# an editor-only module must never reach helios_runtime, which every server and the client link.
function(helios_link_group_of out headless editorOnly)
  if(editorOnly)
    set(${out} editor PARENT_SCOPE)
  elseif(headless)
    set(${out} runtime PARENT_SCOPE)
  else()
    set(${out} client PARENT_SCOPE)
  endif()
endfunction()

# Writes helios/<group>_api.h for every group (both flavours; unchanged content leaves the file alone so
# nothing recompiles).
function(_helios_write_api_headers)
  if(HELIOS_MODULAR)
    set(modular 1)
  else()
    set(modular 0)
  endif()
  foreach(group IN LISTS HELIOS_LINK_GROUPS)
    string(TOUPPER "${group}" G)
    set(text "/* helios/${group}_api.h: generated by cmake/HeliosModular.cmake. Do not edit.
 *
 * HELIOS_${G}_API marks a declaration that the helios_${group} link group exports to other images in a
 * modular dev build (HELIOS_MODULAR=ON; ADR-016, 02 §1.4). Use it on data that code outside the group
 * references (MSVC imports data only through __declspec(dllimport)) and on C entry points. Functions are
 * exported without it in this build. It expands to nothing in shipping builds. Thread-safe by nature:
 * it is a declaration attribute.
 */
#ifndef HELIOS_${G}_API_H
#define HELIOS_${G}_API_H

/* 1 in a modular dev build, 0 in a shipping build. */
#define HELIOS_LINK_MODULAR ${modular}

#if HELIOS_LINK_MODULAR
#  if defined(_WIN32)
#    if defined(HELIOS_${G}_BUILDING)
#      define HELIOS_${G}_API __declspec(dllexport)
#    else
#      define HELIOS_${G}_API __declspec(dllimport)
#    endif
#  else
#    define HELIOS_${G}_API __attribute__((visibility(\"default\")))
#  endif
#else
#  define HELIOS_${G}_API
#endif

#if HELIOS_LINK_MODULAR
#ifdef __cplusplus
extern \"C\" {
#endif
/* Names the modules linked into helios_${group}, separated by spaces (generated with the group). Any
 * thread. Modular builds only. */
HELIOS_${G}_API const char* helios_${group}_link_group_modules(void);
#ifdef __cplusplus
}
#endif
#endif

#endif /* HELIOS_${G}_API_H */
")
    set(path "${HELIOS_MODULAR_INCLUDE_DIR}/helios/${group}_api.h")
    set(old "")
    if(EXISTS "${path}")
      file(READ "${path}" old)
    endif()
    if(NOT old STREQUAL text)
      file(WRITE "${path}" "${text}")
    endif()
  endforeach()
endfunction()

# Called from the top-level CMakeLists.txt right after the vendored libraries: the settings below apply
# to modules, tools, tests and apps, never to third-party code (whose objects stay identical to the
# shipping build's, so compiler caches serve both flavours).
macro(helios_modular_after_third_party)
  _helios_write_api_headers()
  if(HELIOS_MODULAR)
    if(MINGW)
      message(FATAL_ERROR "HELIOS_MODULAR=ON is not supported with MinGW: the cross build checks the shipping "
                          "flavour; dev builds use MSVC or clang-cl (windows-msvc-dev) or Linux (linux-dev). "
                          "See cmake/HeliosModular.cmake")
    endif()
    if(NOT WIN32)
      set(CMAKE_C_VISIBILITY_PRESET hidden)
      set(CMAKE_CXX_VISIBILITY_PRESET hidden)
      set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
    endif()
    if(MSVC)
      # Dev link (02 §1.4): incremental, no folding or stripping, full PDBs.
      add_link_options(/INCREMENTAL /OPT:NOREF /OPT:NOICF /DEBUG:FULL)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # lld when the toolchain has it (02 §1.4: lld or mold); split DWARF keeps the links small. Clang only:
      # GCC splits with an objcopy pass after compiling, which failed intermittently under ccache here.
      include(CheckLinkerFlag)
      check_linker_flag(CXX "-fuse-ld=lld" HELIOS_MODULAR_HAVE_LLD)
      if(HELIOS_MODULAR_HAVE_LLD)
        add_link_options(-fuse-ld=lld)
      else()
        message(STATUS "HELIOS_MODULAR: lld not found; linking with the default linker")
      endif()
      add_compile_options("$<$<AND:$<CONFIG:Debug,RelWithDebInfo>,$<COMPILE_LANG_AND_ID:C,Clang>>:-gsplit-dwarf>"
                          "$<$<AND:$<CONFIG:Debug,RelWithDebInfo>,$<COMPILE_LANG_AND_ID:CXX,Clang>>:-gsplit-dwarf>")
    endif()
    message(STATUS "HELIOS_MODULAR: dev link model (link groups ${HELIOS_LINK_GROUPS}; ADR-016)")
  endif()
endmacro()

# Creates helios_<group> on first use. Its one source of its own is generated at the end of configure
# (helios_modular_finalize): helios_<group>_link_group_modules(), which names the group's modules and
# guarantees that the library exports something (a DLL without exports gets no import library).
function(_helios_modular_group group)
  if(TARGET helios_${group})
    return()
  endif()
  string(TOUPPER "${group}" G)
  set(src "${CMAKE_BINARY_DIR}/helios_generated/link_groups/helios_${group}_group.cpp")
  add_library(helios_${group} SHARED "${src}")
  set_source_files_properties("${src}" TARGET_DIRECTORY helios_${group} PROPERTIES GENERATED ON)
  target_include_directories(helios_${group} PRIVATE "${HELIOS_MODULAR_INCLUDE_DIR}")
  target_compile_definitions(helios_${group} PRIVATE HELIOS_${G}_BUILDING)
  set_target_properties(helios_${group} PROPERTIES
    FOLDER engine
    HELIOS_LINK_GROUP ${group}
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN ON)
  if(MSVC)
    set_target_properties(helios_${group} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  elseif(NOT APPLE)
    # Third-party archives are linked in but never exported; -z defs makes a missing symbol a link error,
    # as it is on Windows (02 §1.4). Sanitizer runtimes are linked into executables only, so the group
    # libraries cannot resolve their symbols at link time: no -z defs there.
    target_link_options(helios_${group} PRIVATE "LINKER:--exclude-libs,ALL")
    if(NOT HELIOS_SANITIZE)
      target_link_options(helios_${group} PRIVATE "LINKER:-z,defs")
    endif()
  endif()
  set_property(GLOBAL APPEND PROPERTY HELIOS_LINK_GROUP_TARGETS helios_${group})
endfunction()

# helios_module()'s modular part: <target> is the module's OBJECT library.
function(_helios_modular_module target name group)
  string(TOUPPER "${group}" G)
  target_compile_definitions(${target} PRIVATE HELIOS_${G}_BUILDING)
  set_target_properties(${target} PROPERTIES
    HELIOS_LINK_GROUP ${group}
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN ON)
  _helios_modular_group(${group})
  target_link_libraries(helios_${group} PRIVATE ${target})

  # The consumers' view of the module. The group library is linked unless the consumer is the group
  # itself, a module of the same group, or a self-contained image (helios_self_contained).
  add_library(${target}_api INTERFACE)
  set(skip "$<OR:$<STREQUAL:$<TARGET_PROPERTY:HELIOS_LINK_GROUP>,${group}>,$<BOOL:$<TARGET_PROPERTY:HELIOS_SELF_CONTAINED>>>")
  target_link_libraries(${target}_api INTERFACE "$<COMPILE_ONLY:${target}>" "$<$<NOT:${skip}>:helios_${group}>")
  set_target_properties(${target}_api PROPERTIES HELIOS_API_OF ${target} HELIOS_LINK_GROUP_OF ${group})
  add_library(helios::${name} ALIAS ${target}_api)
  set_property(GLOBAL APPEND PROPERTY HELIOS_LINK_GROUP_${group}_MODULES ${name})
endfunction()

function(helios_self_contained target)
  if(NOT HELIOS_MODULAR)
    return()
  endif()
  set_target_properties(${target} PROPERTIES HELIOS_SELF_CONTAINED ON)
  get_target_property(type ${target} TYPE)
  if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
    set_property(GLOBAL APPEND PROPERTY HELIOS_SELF_CONTAINED_IMAGES ${target})
  endif()
endfunction()

# Links into a self-contained image the object libraries of every module it reaches (helios_finalize_build
# runs after every target exists, so the closure is complete).
function(_helios_modular_link_self_contained target)
  set(queue "${target}")
  set(seen "${target}")
  set(modules "")
  while(queue)
    list(POP_FRONT queue cur)
    _helios_direct_deps("${cur}" deps) # HeliosLayering.cmake: helios::<module> resolves to the module
    foreach(d IN LISTS deps)
      if(d IN_LIST seen)
        continue()
      endif()
      list(APPEND seen "${d}")
      get_target_property(m "${d}" HELIOS_MODULE_NAME)
      if(m)
        list(APPEND modules "${d}")
      endif()
      get_target_property(dtype "${d}" TYPE)
      if(m OR dtype MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|INTERFACE_LIBRARY)$")
        list(APPEND queue "${d}")
      endif()
    endforeach()
  endwhile()
  # The objects themselves, with their third-party libraries ($<LINK_ONLY:...> usage of each OBJECT library).
  if(modules)
    target_link_libraries(${target} PRIVATE ${modules})
  endif()
  # Windows: its sources define what they declare, so no dllimport of data the image itself carries (the
  # macros expand to the same visibility attribute either way on ELF).
  if(WIN32)
    foreach(group IN LISTS HELIOS_LINK_GROUPS)
      string(TOUPPER "${group}" G)
      target_compile_definitions(${target} PRIVATE HELIOS_${G}_BUILDING)
    endforeach()
  endif()
  # A gated Windows image normally gets the CPU-gate hook from helios_runtime.dll (helios_cpu_gate); this
  # one loads no group, so it links the hook itself, as a shipping image does.
  get_target_property(gateInRuntime ${target} HELIOS_CPU_GATE_IN_RUNTIME)
  if(gateInRuntime AND TARGET helios_core_cpugate_hook)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:helios_core_cpugate_hook>)
  endif()
  set_property(TARGET ${target} PROPERTY HELIOS_SELF_CONTAINED_MODULES "${modules}")
endfunction()

# Run once at the end of configure (helios_finalize_build).
function(helios_modular_finalize)
  if(NOT HELIOS_MODULAR)
    return()
  endif()
  get_property(groups GLOBAL PROPERTY HELIOS_LINK_GROUP_TARGETS)
  foreach(tgt IN LISTS groups)
    get_target_property(group ${tgt} HELIOS_LINK_GROUP)
    string(TOUPPER "${group}" G)
    get_property(modules GLOBAL PROPERTY HELIOS_LINK_GROUP_${group}_MODULES)
    string(REPLACE ";" " " moduleText "${modules}")
    set(text "// Generated by cmake/HeliosModular.cmake: the helios_${group} link group (02 §1.4). Do not edit.
#include \"helios/${group}_api.h\"

/// Names the modules linked into this group library, separated by spaces. Any thread.
extern \"C\" const char* helios_${group}_link_group_modules(void) {
    return \"${moduleText}\";
}
")
    set(path "${CMAKE_BINARY_DIR}/helios_generated/link_groups/helios_${group}_group.cpp")
    set(old "")
    if(EXISTS "${path}")
      file(READ "${path}" old)
    endif()
    if(NOT old STREQUAL text)
      file(WRITE "${path}" "${text}")
    endif()
  endforeach()

  # A group links the groups its modules depend on (through their helios::<module> interfaces). Its
  # consumers need them too: a test of a client module calls core directly, and a shared library's own
  # dependencies are not on its consumers' link line (ELF: no DT_NEEDED walk; PE: imports come from the
  # import libraries named on the link line). So each group passes those groups on (INTERFACE).
  get_property(moduleTargets GLOBAL PROPERTY HELIOS_MODULE_TARGETS)
  foreach(m IN LISTS moduleTargets)
    get_target_property(group ${m} HELIOS_LINK_GROUP)
    if(NOT group)
      continue()
    endif()
    _helios_module_deps(${m} deps) # HeliosLayering.cmake: module -> module edges
    foreach(d IN LISTS deps)
      get_target_property(depGroup ${d} HELIOS_LINK_GROUP)
      if(depGroup AND NOT depGroup STREQUAL group)
        get_property(passed TARGET helios_${group} PROPERTY HELIOS_PASSED_GROUPS)
        if(NOT depGroup IN_LIST passed)
          target_link_libraries(helios_${group} INTERFACE helios_${depGroup})
          set_property(TARGET helios_${group} APPEND PROPERTY HELIOS_PASSED_GROUPS ${depGroup})
        endif()
      endif()
    endforeach()
  endforeach()

  foreach(entry IN LISTS HELIOS_GROUP_EXPORTED_THIRD_PARTY)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 group)
    list(GET parts 1 lib)
    if(TARGET helios_${group} AND TARGET ${lib})
      target_sources(helios_${group} PRIVATE $<TARGET_OBJECTS:${lib}>)
    endif()
  endforeach()

  get_property(selfContained GLOBAL PROPERTY HELIOS_SELF_CONTAINED_IMAGES)
  foreach(tgt IN LISTS selfContained)
    _helios_modular_link_self_contained(${tgt})
  endforeach()

  # The CPU gate (02 §1.1 "Which image"): on Windows the hook lives in helios_runtime.dll, the first
  # Helios image the loader initializes, and gated executables carry none (helios_cpu_gate imports the
  # DLL instead). ELF executables keep the hook: .preinit_array exists only there, and glibc runs it
  # before the initializers of libhelios_runtime.so.
  if(WIN32 AND TARGET helios_runtime AND TARGET helios_core_cpugate_hook)
    target_sources(helios_runtime PRIVATE $<TARGET_OBJECTS:helios_core_cpugate_hook>)
  endif()
endfunction()

# Every build-system target of the project, in every directory.
function(_helios_all_targets out)
  set(found "")
  set(queue "${CMAKE_SOURCE_DIR}")
  while(queue)
    list(POP_FRONT queue dir)
    get_property(sub DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    list(APPEND queue ${sub})
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND found ${targets})
  endwhile()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Modular rule for the layering check (HeliosLayering.cmake): only a group library or a self-contained
# image may link a module's OBJECT library directly; anything else would carry a second copy of its code
# and state beside the group's. Appends "helios layering: ..." messages to <errorsVar>.
function(helios_modular_check_direct_objects errorsVar)
  if(NOT HELIOS_MODULAR)
    return()
  endif()
  set(errors ${${errorsVar}})
  get_property(modules GLOBAL PROPERTY HELIOS_MODULE_TARGETS)
  get_property(groups GLOBAL PROPERTY HELIOS_LINK_GROUP_TARGETS)
  get_property(standalone GLOBAL PROPERTY HELIOS_SELF_CONTAINED_IMAGES)
  _helios_all_targets(candidates)
  foreach(tgt IN LISTS candidates)
    if(tgt IN_LIST groups OR tgt IN_LIST standalone)
      continue()
    endif()
    get_target_property(type ${tgt} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(ll ${tgt} LINK_LIBRARIES)
    if(NOT ll)
      continue()
    endif()
    foreach(item IN LISTS ll)
      if(TARGET "${item}")
        get_target_property(aliased "${item}" ALIASED_TARGET)
        if(aliased)
          set(item "${aliased}")
        endif()
        if(item IN_LIST modules)
          list(APPEND errors "helios layering: '${tgt}' links the module object library '${item}' directly; in a modular build that copies the module into a second image (link helios::<module>, or declare the image with helios_self_contained)")
        endif()
      endif()
    endforeach()
  endforeach()
  set(${errorsVar} "${errors}" PARENT_SCOPE)
endfunction()
