# ISA levels (02 §1.1 "ISA levels and the pre-gate audit", ADR-011 amendment, 08 §2.1.1 and §2.2; WP-0.2r).
#
# Helios builds whole images at one ISA level. No list grants a target or a file flags of its own:
#   avx2  Every image that links a runtime module (helios_executable() roles client, cell, gateway, voice,
#         editor, bot, tool, sample and bench; the tests and their child processes) and everything those
#         images link: every engine module, gem, game module and third-party library. That is every
#         compiled target of the build except the two kinds below.
#   base  x86-64-v1, for images that must run on a CPU without AVX2 so that they can explain the refusal:
#         the launcher and the `Helios` bootstrap (ROLE launcher|bootstrap) and, for the audit's fixtures
#         only, `helios_executable(... ISA base)`. A base image links `<target>.base` copies (OBJECT
#         libraries) of what it links: 02 §1.1's `<module>@base`, spelled with a dot because CMake target
#         names cannot contain `@`. Copies exist only for the modules in HELIOS_ISA_BASE_MODULES, the
#         libraries in HELIOS_ISA_BASE_THIRD_PARTY and the libraries those link, and only once a base image
#         is configured. Configure fails when a base image links anything else (physics, pcg, tp_jolt, ...).
#   gate  The CPU gate's C objects inside every avx2 image (helios_cpu_gate_target()): the base flags plus
#         the gate-object rules (no stack protector or /GS cookie, no sanitizer instrumentation; 02 §1.1).
#         Two more rules live outside this file because CMake has no per-target switch for them: MSVC's
#         /RTC1 (CMake's Debug default) is removed from engine/core's C flags, whose only C units are the
#         gate's, and MSVC's /fsanitize=address is added to every target but the gate's (top-level
#         CMakeLists.txt). Audit check 1 rejects a gate unit that still carries either.
#
# Both link flavours (ADR-016, cmake/HeliosModular.cmake). In a modular dev build (HELIOS_MODULAR=ON) the
# link-group libraries helios_runtime, helios_client and helios_editor, the module object libraries inside
# them and the third-party shared libraries SDL3 and tp_imgui are avx2 like every other library; the gate's
# object libraries keep the gate level inside helios_runtime (the probe, and on Windows the hook). A base
# image never imports a group library: there helios::<module> names the module's consumer interface, which
# links the group, so the image's links to it are replaced by the module's `.base` copy, and configure fails
# if its link line still reaches any shared library built here (all of them are avx2). On Windows the base
# image and its copies define the HELIOS_<GROUP>_API data themselves (helios_modular_defines_own_copy).
#
# HELIOS_ISA_AVX2, HELIOS_ISA_BASE
#   02 §1.1's level sets for this compiler. Only helios_apply_isa_level() puts them on a target: CONF-11
#   exempts that function by name and reports every other use.
#
# helios_apply_isa_level(<target> avx2|base|gate)
#   Puts the level's flags on <target>'s C and C++ compiles, PRIVATE so that no consumer inherits them
#   (each consumer gets its own image's level), and records the level in the HELIOS_ISA_LEVEL target
#   property. helios_isa_finalize() calls it for every target; nothing else does.
#
# helios_cpu_gate_target(<target>)
#   Marks <target>, an OBJECT library that holds only CPU-gate C units, as the `gate` level.
#
# _helios_cpu_gate(<target>)  (internal: only helios_executable() calls it, for gated images)
#   Links the CPU-gate pre-initializer (engine/core/src/platform/*/cpu_gate_hook.c) into an avx2
#   executable: it runs before any other code of the image (ELF: the single .preinit_array entry; Windows:
#   the first TLS callback, .CRT$XLA0), records the verdict that core::platformInit() checks, and on an
#   unsupported CPU prints the "requires an AVX2 CPU" message and exits with code 78 (02 §1.1).
#   In a modular Windows build (HELIOS_MODULAR=ON) the hook lives in helios_runtime.dll (02 §1.1 "Which
#   image"; helios_modular_finalize in cmake/HeliosModular.cmake), and the executable gets a forced import
#   of the gate probe instead, so the loader always initializes that DLL before any code of the executable.
#   That also gates every other process that loads helios_runtime.dll (tests, samples, benches, a modular
#   launcher; ADR-0.6c §3 item 7). On Linux only gated executables carry the hook, so the role decides.
#
# helios_isa_finalize()
#   The configure-time propagation, run once by helios_finalize_build() after every target exists:
#   1. no target's INTERFACE_COMPILE_OPTIONS carries an ISA option (a consumer would inherit it);
#   2. each base image's link closure is checked, then replaced by `.base` copies (in a modular build its
#      link line must then reach no shared library: no group, SDL3 or tp_imgui);
#   3. every target gets its level, and helios_generated/isa_levels.txt records "<target> <level>" for
#      tools/lint/isa_audit.cmake (audit check 1).
#   Violations stop configure with "helios isa:" lines.
#
# The audit (tools/lint/isa_audit.cmake, CTest `lint_isa_audit`) checks the result on every build.

include_guard(GLOBAL)

set(HELIOS_CPU_GATE_EXIT_CODE 78)

# The level sets (02 §1.1's table). No FMA anywhere and no FP contraction (determinism, 02 §7.1). The base
# set also turns every extension above x86-64-v1 off by name, because -march does not undo an -m option
# that reaches the same command line (08 §2.1.1; no -mcx16). x64 MSVC has no switch below its /arch:SSE2
# default, so its base set is empty, and the audit relies on no /arch option reaching a base unit.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(HELIOS_ISA_AVX2 "")
  set(HELIOS_ISA_BASE "")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  set(HELIOS_ISA_AVX2 /arch:AVX2 /fp:precise)
  set(HELIOS_ISA_BASE "")
elseif(MSVC) # clang-cl, whose /arch:AVX2 selects a Haswell CPU: FMA is turned off again
  set(HELIOS_ISA_AVX2 /arch:AVX2 /fp:precise -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma /clang:-ffp-contract=off)
  set(HELIOS_ISA_BASE "")
else()
  set(HELIOS_ISA_AVX2 -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -mf16c -mno-fma -mfpmath=sse -ffp-contract=off)
  set(HELIOS_ISA_BASE -march=x86-64 -mtune=generic -mno-sse3 -mno-ssse3 -mno-sse4.1 -mno-sse4.2 -mno-popcnt
                      -mno-avx -mno-avx2 -mno-fma -mno-bmi -mno-bmi2 -mno-lzcnt -mno-f16c -mno-movbe -mno-avx512f
                      -mno-cx16)
endif()

# What a base image may link (02 §1.1's `base` row (a); 08 §2.1.1's launcher closure). Modules by name:
# core, app, ui, text, loc, patch and crash, of which core and patch exist today. The others get copies
# once they exist: app is named by WP-0.11's row but is not in the tree yet (the launcher, WP-0.17, is the
# first base image to need it); ui, text and loc come with WP-1.6 (02 §7.5–7.6), and the launcher links ui
# from WP-1.21 (RmlUi on SDL_Renderer); crash comes in Phase 2. Third-party libraries by target name, where
# they exist: SDL3, RmlUi, FreeType, HarfBuzz, SheenBidi, libunibreak, zstd, Monocypher, yyjson and
# sentry-native. The libraries these link (mimalloc, say) get copies too.
set(HELIOS_ISA_BASE_MODULES core app ui text loc patch crash)
set(HELIOS_ISA_BASE_THIRD_PARTY SDL3-static tp_rmlui tp_freetype tp_harfbuzz tp_sheenbidi tp_libunibreak tp_zstd
                                tp_monocypher tp_yyjson tp_sentry)
# Libraries that are never base-eligible, even when the closure of a module or library above reaches them
# (02 §1.1 names physics, pcg and tp_jolt): if core ever linked tp_jolt, a base image that links core must
# fail configure instead of getting a tp_jolt copy.
set(HELIOS_ISA_BASE_DENY helios_physics helios_pcg tp_jolt)

function(helios_apply_isa_level target level)
  if(level STREQUAL "avx2")
    set(flags ${HELIOS_ISA_AVX2})
  elseif(level STREQUAL "base")
    set(flags ${HELIOS_ISA_BASE})
  elseif(level STREQUAL "gate")
    # The gate runs before the CRT and the sanitizer runtimes exist: no /GS cookie or stack protector and
    # no instrumentation (02 §1.1's gate-object rules; audit check 2 rejects their symbols).
    set(flags ${HELIOS_ISA_BASE})
    if(MSVC)
      list(APPEND flags /GS-)
      if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        list(APPEND flags -fno-sanitize=all)
      endif()
    else()
      list(APPEND flags -fno-stack-protector -fno-sanitize=all)
    endif()
  else()
    message(FATAL_ERROR "helios_apply_isa_level(${target}): the level is avx2, base or gate, not '${level}'")
  endif()
  get_target_property(type ${target} TYPE)
  if(flags AND NOT type STREQUAL "INTERFACE_LIBRARY")
    # C and C++ only: resource compilers (rc, windres) take no ISA flags.
    target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${flags}>")
  endif()
  set_target_properties(${target} PROPERTIES HELIOS_ISA_LEVEL ${level})
endfunction()

function(helios_cpu_gate_target target)
  get_target_property(type ${target} TYPE)
  if(NOT type STREQUAL "OBJECT_LIBRARY")
    message(FATAL_ERROR "helios_cpu_gate_target(${target}): the CPU gate's units live in OBJECT libraries")
  endif()
  set_target_properties(${target} PROPERTIES HELIOS_ISA_LEVEL gate)
endfunction()

function(_helios_cpu_gate target)
  if(NOT TARGET helios_core_cpugate_hook)
    message(FATAL_ERROR "helios_executable(${target}): engine/core must be configured before a gated image "
                        "(target helios_core_cpugate_hook is missing)")
  endif()
  if(HELIOS_MODULAR AND MSVC)
    # The linker drops a DLL whose symbols an image never references; /INCLUDE keeps the import. (A
    # self-contained image links the hook itself: cmake/HeliosModular.cmake, helios_self_contained.)
    target_link_options(${target} PRIVATE /INCLUDE:helios_cpu_gate_run)
    set_target_properties(${target} PROPERTIES HELIOS_CPU_GATE_IN_RUNTIME ON)
  else()
    # An object library links its object file unconditionally; an archive member holding only an
    # initializer would be dropped by the linker.
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:helios_core_cpugate_hook>)
  endif()
  target_link_libraries(${target} PRIVATE helios::core)
  set_target_properties(${target} PROPERTIES HELIOS_CPU_GATE ON)
  set_property(GLOBAL APPEND PROPERTY HELIOS_CPU_GATE_TARGETS ${target})
endfunction()

# ---------------------------------------------------------------------------------------------
# Configure-time propagation (helios_isa_finalize). Uses the link-item resolver and path search of
# cmake/HeliosLayering.cmake.
# ---------------------------------------------------------------------------------------------

# Every target the project defines, in every directory: libraries of each kind and executables.
# Imported, alias and utility targets compile nothing here.
function(_helios_isa_project_targets out)
  set(found "")
  set(queue "${CMAKE_SOURCE_DIR}")
  while(queue)
    list(POP_FRONT queue dir)
    get_property(sub DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    list(APPEND queue ${sub})
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
      get_target_property(type "${t}" TYPE)
      if(type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|INTERFACE_LIBRARY)$")
        list(APPEND found "${t}")
      endif()
    endforeach()
  endwhile()
  set(${out} "${found}" PARENT_SCOPE)
endfunction()

# Every target reachable from `from` through its link items (aliases resolved, generator expressions
# looked through), without `from` itself, in breadth-first order.
function(_helios_isa_closure from out)
  set(queue "${from}")
  set(seen "${from}")
  set(result "")
  while(queue)
    list(POP_FRONT queue cur)
    _helios_direct_deps("${cur}" deps)
    foreach(d IN LISTS deps)
      if(NOT d IN_LIST seen)
        list(APPEND seen "${d}")
        list(APPEND result "${d}")
        list(APPEND queue "${d}")
      endif()
    endforeach()
  endwhile()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The targets a base image may link: HELIOS_ISA_BASE_MODULES, HELIOS_ISA_BASE_THIRD_PARTY, and the
# libraries in their closures that are not modules, except HELIOS_ISA_BASE_DENY. A module that only their
# closure reaches stays ineligible, so a base image that reaches it (ui -> script, say) fails configure.
function(_helios_isa_base_eligible out)
  set(roots "")
  foreach(m IN LISTS HELIOS_ISA_BASE_MODULES)
    if(TARGET helios_${m})
      list(APPEND roots helios_${m})
    endif()
  endforeach()
  foreach(t IN LISTS HELIOS_ISA_BASE_THIRD_PARTY)
    if(TARGET ${t})
      get_target_property(aliased ${t} ALIASED_TARGET)
      if(aliased)
        set(t ${aliased})
      endif()
      list(APPEND roots ${t})
    endif()
  endforeach()
  set(eligible ${roots})
  foreach(r IN LISTS roots)
    _helios_isa_closure(${r} reach)
    foreach(d IN LISTS reach)
      get_target_property(m ${d} HELIOS_MODULE_NAME)
      if(NOT m)
        list(APPEND eligible ${d})
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES eligible)
  foreach(t IN LISTS HELIOS_ISA_BASE_DENY)
    if(TARGET ${t})
      get_target_property(aliased ${t} ALIASED_TARGET)
      if(aliased)
        set(t ${aliased})
      endif()
      list(REMOVE_ITEM eligible ${t})
    endif()
  endforeach()
  set(${out} "${eligible}" PARENT_SCOPE)
endfunction()

# Whether a base image may link `tgt`: an eligible library, an imported one (no code built here), an
# interface library (its headers compile at the consumer's level; what it links is checked on its own)
# or a gate object library (already x86-64-v1).
function(_helios_isa_base_allowed tgt out)
  get_property(eligible GLOBAL PROPERTY _HELIOS_ISA_BASE_ELIGIBLE)
  get_target_property(imported "${tgt}" IMPORTED)
  get_target_property(level "${tgt}" HELIOS_ISA_LEVEL)
  get_target_property(type "${tgt}" TYPE)
  if(imported OR level STREQUAL "gate" OR type STREQUAL "INTERFACE_LIBRARY" OR tgt IN_LIST eligible)
    set(${out} ON PARENT_SCOPE)
  else()
    set(${out} OFF PARENT_SCOPE)
  endif()
endfunction()

# Predicate for _helios_find_path: the target named by _HELIOS_ISA_TARGET.
function(_helios_isa_pred_is_target tgt out)
  get_property(want GLOBAL PROPERTY _HELIOS_ISA_TARGET)
  if(tgt STREQUAL want)
    set(${out} ON PARENT_SCOPE)
  else()
    set(${out} OFF PARENT_SCOPE)
  endif()
endfunction()

# Rewrites one link item: every target with a `.base` copy becomes the copy, also inside the generator
# expressions _helios_resolve_link_item looks through.
function(_helios_isa_remap_item item out)
  if(item MATCHES "^\\$<")
    _helios_split_genex("${item}" g)
    set(keep 0)
    if(g_head MATCHES "^(LINK_LIBRARY|LINK_GROUP|IF)$")
      set(keep 1) # the feature name, or the condition
    elseif(g_head STREQUAL "" OR NOT (g_head MATCHES "^\\$<" OR
           g_head MATCHES "^(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE|TARGET_NAME_IF_EXISTS|COMPILE_ONLY|1)$"))
      set(${out} "${item}" PARENT_SCOPE) # INSTALL_INTERFACE, 0, ...: nothing linked in the build tree
      return()
    endif()
    if(keep)
      _helios_split_top_commas("${g_body}" parts)
    else()
      set(parts "${g_body}")
    endif()
    set(mapped "")
    set(i 0)
    foreach(p IN LISTS parts)
      if(i LESS keep)
        list(APPEND mapped "${p}")
      else()
        _helios_isa_remap_item("${p}" r)
        list(APPEND mapped "${r}")
      endif()
      math(EXPR i "${i} + 1")
    endforeach()
    string(REPLACE ";" "," body "${mapped}")
    set(${out} "$<${g_head}:${body}>" PARENT_SCOPE)
    return()
  endif()
  set(result "${item}")
  if(item MATCHES "::" AND NOT TARGET "${item}")
    # An imported target found below the top level (find_package in a subdirectory) is not visible to
    # the base copies, which live in the top-level directory.
    message(FATAL_ERROR "helios isa: a base copy links '${item}', which is not visible in the top-level "
                        "directory: find_package() it in the top-level CMakeLists.txt")
  endif()
  if(TARGET "${item}")
    get_target_property(real "${item}" ALIASED_TARGET)
    if(NOT real)
      set(real "${item}")
    endif()
    # Modular builds: helios::<module> names the module's consumer interface, which links the module's
    # avx2 group library (cmake/HeliosModular.cmake). A base copy stands for the module itself.
    get_target_property(apiOf "${real}" HELIOS_API_OF)
    if(apiOf)
      set(real "${apiOf}")
    endif()
    get_property(copy GLOBAL PROPERTY "_HELIOS_ISA_BASE_COPY_${real}")
    if(copy)
      set(result "${copy}")
    endif()
  endif()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Points a target's link items (a base copy's, or a base image's) at the `.base` copies.
function(_helios_isa_relink tgt)
  foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_property(isSet TARGET ${tgt} PROPERTY ${prop} SET)
    if(isSet)
      get_target_property(items ${tgt} ${prop})
      set(mapped "")
      foreach(item IN LISTS items)
        _helios_isa_remap_item("${item}" r)
        list(APPEND mapped "${r}")
      endforeach()
      set_property(TARGET ${tgt} PROPERTY ${prop} "${mapped}")
    endif()
  endforeach()
endfunction()

# Creates `<tgt>.base`, a library's base copy, in the top-level directory: an OBJECT library with the
# library's sources, their properties, and its target properties and link items (compile options without
# a level: levels are applied afterwards), or an INTERFACE library with the same usage requirements.
# _helios_isa_relink points its link items at the other copies once they all exist.
function(_helios_isa_make_base_copy tgt out)
  set(copy "${tgt}.base")
  get_target_property(type ${tgt} TYPE)
  if(type STREQUAL "INTERFACE_LIBRARY")
    add_library(${copy} INTERFACE)
  elseif(type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY)$")
    get_target_property(srcs ${tgt} SOURCES)
    get_target_property(srcDir ${tgt} SOURCE_DIR)
    get_target_property(binDir ${tgt} BINARY_DIR)
    set(files "")
    foreach(s IN LISTS srcs)
      if(s MATCHES "\\$<")
        message(FATAL_ERROR "helios isa: no base copy of '${tgt}': its source '${s}' is a generator expression "
                            "(link an object library instead of listing its objects)")
      endif()
      # A relative source is in the library's source directory if it is there, else in its build directory,
      # where a generated one is written (it need not exist yet at configure time). Asking for the GENERATED
      # property of the build-directory path instead would create a source entry there, which CMake then
      # matches for the library's own relative source.
      set(path "${s}")
      if(NOT IS_ABSOLUTE "${path}")
        if(EXISTS "${srcDir}/${s}")
          set(path "${srcDir}/${s}")
        else()
          set(path "${binDir}/${s}")
        endif()
      endif()
      list(APPEND files "${path}")
      # Source properties belong to a directory, and the copy lives in the top-level one.
      foreach(prop COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS INCLUDE_DIRECTORIES LANGUAGE
                   HEADER_FILE_ONLY OBJECT_DEPENDS SKIP_PRECOMPILE_HEADERS)
        get_source_file_property(value "${path}" TARGET_DIRECTORY ${tgt} ${prop})
        if(NOT value STREQUAL "NOTFOUND")
          set_source_files_properties("${path}" PROPERTIES ${prop} "${value}")
        endif()
      endforeach()
    endforeach()
    add_library(${copy} OBJECT ${files})
    foreach(prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES PRECOMPILE_HEADERS
                 C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
                 C_VISIBILITY_PRESET CXX_VISIBILITY_PRESET VISIBILITY_INLINES_HIDDEN POSITION_INDEPENDENT_CODE
                 MSVC_RUNTIME_LIBRARY MSVC_DEBUG_INFORMATION_FORMAT LINK_LIBRARIES)
      get_property(isSet TARGET ${tgt} PROPERTY ${prop} SET)
      if(isSet)
        get_target_property(value ${tgt} ${prop})
        set_property(TARGET ${copy} PROPERTY ${prop} "${value}")
      endif()
    endforeach()
  else()
    message(FATAL_ERROR "helios isa: no base copy of '${tgt}' (${type}): base images link static, object and "
                        "interface libraries only")
  endif()
  foreach(prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS
               INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES INTERFACE_LINK_OPTIONS
               INTERFACE_LINK_DIRECTORIES INTERFACE_POSITION_INDEPENDENT_CODE INTERFACE_PRECOMPILE_HEADERS
               INTERFACE_LINK_LIBRARIES)
    get_property(isSet TARGET ${tgt} PROPERTY ${prop} SET)
    if(isSet)
      get_target_property(value ${tgt} ${prop})
      set_property(TARGET ${copy} PROPERTY ${prop} "${value}")
    endif()
  endforeach()
  get_target_property(folder ${tgt} FOLDER)
  if(NOT folder)
    set(folder "")
  endif()
  set_target_properties(${copy} PROPERTIES FOLDER "${folder}/base" HELIOS_ISA_BASE_OF ${tgt} HELIOS_ISA_LEVEL base)
  set_property(GLOBAL PROPERTY "_HELIOS_ISA_BASE_COPY_${tgt}" ${copy})
  set_property(GLOBAL APPEND PROPERTY _HELIOS_ISA_BASE_COPIES ${copy})
  set(${out} ${copy} PARENT_SCOPE)
endfunction()

# An ISA option in a library's interface reaches every consumer, whatever its image's level: tp_jolt's
# PUBLIC /arch:AVX2 and -mavx2 options did that until WP-0.2r. Appends one error per such target.
function(_helios_isa_check_interface tgt errorsVar)
  get_target_property(opts ${tgt} INTERFACE_COMPILE_OPTIONS)
  if(NOT opts)
    return()
  endif()
  string(REGEX MATCHALL "(-march=|[-/]arch:|-m(no-)?(sse[0-9.]*|ssse3|avx[0-9a-z.]*|bmi2?|fma4?|f16c|lzcnt|popcnt|movbe|cx16|aes|pclmul|sha[0-9]*|xop|vaes|vpclmulqdq|gfni|adx|rdrnd|rdseed|xsave[a-z]*|tbm|abm))[A-Za-z0-9.=]*"
         hits "${opts}")
  if(hits)
    list(JOIN hits " " text)
    set(errors ${${errorsVar}})
    list(APPEND errors "helios isa: '${tgt}' carries ISA compile options on its interface (${text}), which every consumer would inherit: image levels supply ISA flags, and a library's interface keeps only defines (02 §1.1)")
    set(${errorsVar} "${errors}" PARENT_SCOPE)
  endif()
endfunction()

# The shared libraries built here that an image's link line reaches: its own link items and, transitively,
# the interface link items of what they name (generator expressions looked through; helios::<module>
# interfaces kept, so their group library counts). Every shared library is built at avx2 (only images are
# base), so a base image may reach none: the backstop for modular builds, where helios::<module> links a
# group library (cmake/HeliosModular.cmake) and _helios_isa_closure sees the module instead.
function(_helios_isa_shared_reach img out)
  set(result "")
  set(queue "")
  set(seen "${img}")
  get_target_property(items ${img} LINK_LIBRARIES)
  if(NOT items)
    set(items "")
  endif()
  foreach(item IN LISTS items)
    _helios_modular_linked_targets("${item}" linked)
    list(APPEND queue ${linked})
  endforeach()
  while(queue)
    list(POP_FRONT queue cur)
    if(cur IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${cur}")
    get_target_property(type ${cur} TYPE)
    get_target_property(imported ${cur} IMPORTED)
    if(NOT imported AND type MATCHES "^(SHARED|MODULE)_LIBRARY$")
      list(APPEND result "${cur}")
    endif()
    get_target_property(iface ${cur} INTERFACE_LINK_LIBRARIES)
    if(iface)
      foreach(item IN LISTS iface)
        _helios_modular_linked_targets("${item}" linked)
        list(APPEND queue ${linked})
      endforeach()
    endif()
  endwhile()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

function(helios_isa_finalize)
  _helios_isa_project_targets(all)
  set(errors "")

  # Levels set before this point: images (helios_executable) and the gate's object libraries.
  set(baseImages "")
  foreach(t IN LISTS all)
    get_target_property(level ${t} HELIOS_ISA_LEVEL)
    get_target_property(type ${t} TYPE)
    get_target_property(role ${t} HELIOS_APP_ROLE)
    if(NOT level)
    elseif(level STREQUAL "gate" AND type STREQUAL "OBJECT_LIBRARY")
    elseif(level MATCHES "^(avx2|base)$" AND type STREQUAL "EXECUTABLE" AND role)
      if(level STREQUAL "base")
        list(APPEND baseImages ${t})
      endif()
    else()
      list(APPEND errors "helios isa: '${t}' has HELIOS_ISA_LEVEL '${level}', which only helios_executable() (avx2, base) and helios_cpu_gate_target() (gate) set: a library takes its image's level")
    endif()
    _helios_isa_check_interface(${t} errors)
  endforeach()

  # Base images: check each closure, then build it from `.base` copies.
  set(report "")
  if(baseImages)
    _helios_isa_base_eligible(eligible)
    set_property(GLOBAL PROPERTY _HELIOS_ISA_BASE_ELIGIBLE "${eligible}")
  endif()
  foreach(img IN LISTS baseImages)
    _helios_isa_closure(${img} reach)
    set(bad OFF)
    # Objects named in the image's own sources are not link items: $<TARGET_OBJECTS:x> compiles x's units
    # at x's level, and only gate object libraries and base copies are built below avx2.
    get_target_property(imgSources ${img} SOURCES)
    if(imgSources)
      string(REGEX MATCHALL "\\$<TARGET_OBJECTS:[^>]+>" objRefs "${imgSources}")
      foreach(ref IN LISTS objRefs)
        string(REGEX REPLACE "^\\$<TARGET_OBJECTS:(.+)>$" "\\1" lib "${ref}")
        set(libLevel "")
        if(TARGET "${lib}")
          get_target_property(libLevel "${lib}" HELIOS_ISA_LEVEL)
        endif()
        if(NOT libLevel MATCHES "^(gate|base)$")
          set(bad ON)
          list(APPEND errors "helios isa: base image '${img}' compiles in the objects of '${lib}' (${ref}), which is built only at avx2: link the library instead, so that the image gets its base copy (02 §1.1)")
        endif()
      endforeach()
    endif()
    foreach(d IN LISTS reach)
      _helios_isa_base_allowed(${d} ok)
      if(ok)
        continue()
      endif()
      set(bad ON)
      set_property(GLOBAL PROPERTY _HELIOS_ISA_TARGET "${d}")
      _helios_find_path(${img} _helios_isa_pred_is_target path)
      # Report the first avx2 target on each path only: the libraries behind it follow from it.
      string(REPLACE " -> " ";" nodes "${path}")
      list(POP_FRONT nodes)
      list(POP_BACK nodes)
      set(frontier ON)
      foreach(n IN LISTS nodes)
        _helios_isa_base_allowed(${n} nOk)
        if(NOT nOk)
          set(frontier OFF)
        endif()
      endforeach()
      if(frontier)
        list(JOIN HELIOS_ISA_BASE_MODULES ", " allowedModules)
        list(APPEND errors "helios isa: base image '${img}' links '${d}', which is built only at avx2: ${path} (a base image links only the modules ${allowedModules} and their third-party libraries, 02 §1.1)")
      endif()
    endforeach()
    if(bad)
      continue()
    endif()
    set(copies "")
    set(objects "")
    foreach(d IN LISTS reach)
      get_target_property(imported ${d} IMPORTED)
      get_target_property(level ${d} HELIOS_ISA_LEVEL)
      get_target_property(type ${d} TYPE)
      if(imported)
        continue()
      elseif(level STREQUAL "gate")
        list(APPEND objects ${d})
        continue()
      endif()
      get_property(copy GLOBAL PROPERTY "_HELIOS_ISA_BASE_COPY_${d}")
      if(NOT copy)
        _helios_isa_make_base_copy(${d} copy)
        list(APPEND all ${copy})
      endif()
      list(APPEND copies ${copy})
      if(NOT type STREQUAL "INTERFACE_LIBRARY")
        list(APPEND objects ${copy})
      endif()
    endforeach()
    foreach(c IN LISTS copies)
      _helios_isa_relink(${c})
    endforeach()
    _helios_isa_relink(${img})
    # An image links the objects of the object libraries it names itself, not of those they link in
    # turn, so it names every one.
    get_target_property(direct ${img} LINK_LIBRARIES)
    if(NOT direct)
      set(direct "")
    endif()
    foreach(o IN LISTS objects)
      if(NOT o IN_LIST direct)
        target_link_libraries(${img} PRIVATE ${o})
      endif()
    endforeach()
    # Modular Windows builds: the image and its copies carry their own module code, so no dllimport of it.
    foreach(t IN ITEMS ${img} ${copies})
      helios_modular_defines_own_copy(${t})
    endforeach()
    _helios_isa_shared_reach(${img} shared)
    foreach(lib IN LISTS shared)
      list(APPEND errors "helios isa: base image '${img}' imports '${lib}', a shared library built at avx2 (02 §1.1: a base image links only `.base` copies; in a modular build helios::<module> names the module's group library, which the copies replace)")
    endforeach()
    list(JOIN copies ", " text)
    list(APPEND report "${img} (${text})")
  endforeach()

  # Only base images and other copies link a base copy: an avx2 target that named one (the copies are
  # targets by generate time) would mix levels in one image.
  get_property(copiesMade GLOBAL PROPERTY _HELIOS_ISA_BASE_COPIES)
  if(copiesMade)
    list(JOIN copiesMade "|" alt)
    string(REPLACE "." "\\." alt "${alt}")
    string(REPLACE "+" "\\+" alt "${alt}")
    foreach(t IN LISTS all)
      get_target_property(level ${t} HELIOS_ISA_LEVEL)
      if(level STREQUAL "base")
        continue()
      elseif(NOT level)
        set(level avx2)
      endif()
      foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(items ${t} ${prop})
        if(items AND items MATCHES "(^|[;:,>])(${alt})([;,>]|$)")
          list(APPEND errors "helios isa: '${t}' (${level}) links the base copy '${CMAKE_MATCH_2}': only base images link base copies (02 §1.1)")
        endif()
      endforeach()
    endforeach()
  endif()

  if(errors)
    list(REMOVE_DUPLICATES errors)
    list(LENGTH errors count)
    string(REPLACE ";" "\n  " text "${errors}")
    message(FATAL_ERROR "ISA level check failed (${count} violation(s); rules in docs/plan/02-engine-runtime.md §1.1):\n  ${text}\n")
  endif()
  if(report)
    list(LENGTH report n)
    list(JOIN report "; " text)
    message(STATUS "Helios ISA levels: ${n} base image(s): ${text}")
  endif()

  # Every target gets its level: avx2 unless it is a base image, a base copy or a gate object library.
  set(lines "")
  foreach(t IN LISTS all)
    get_target_property(level ${t} HELIOS_ISA_LEVEL)
    if(NOT level)
      set(level avx2)
    endif()
    helios_apply_isa_level(${t} ${level})
    string(APPEND lines "${t} ${level}\n")
  endforeach()
  file(WRITE ${CMAKE_BINARY_DIR}/helios_generated/isa_levels.txt "${lines}")
endfunction()
