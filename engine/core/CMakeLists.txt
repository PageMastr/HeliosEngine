# engine/core — L1 core runtime (HEADLESS): platform layer, logging, asserts, results, hashing, names,
# handles, memory, time, threads, jobs, file system + VFS, dynamic libraries, cvars, command line,
# GUIDs, deterministic RNGs, UTF conversion, containers, crash handling and build info.
#
# All OS-specific code lives in src/platform/win32 (selected when WIN32, including MinGW) or
# src/platform/posix (Linux and other Unix-likes). Everything else is portable C++20.

find_package(Threads REQUIRED)

file(GLOB HELIOS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(WIN32)
  file(GLOB HELIOS_CORE_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/*.cpp)
else()
  file(GLOB HELIOS_CORE_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/*.cpp)
endif()

# CPU gate (02 §1.1, 08 §2.2): two C units at the x86-64-v1 baseline, so they run on any x86-64 CPU, in
# object libraries of the `gate` level (cmake/HeliosIsa.cmake): their own targets, because every other
# target of an avx2 image is compiled at avx2. The probe's object is archived into helios_core (a static
# library links the objects of an object library it links); helios_executable() links the per-OS
# pre-initializer hook's object into every gated avx2 executable (in a modular Windows build into
# helios_runtime.dll). The ISA audit checks both (checks 1 and 2), CONF-12 their source.
#
# No /RTC instrumentation in the gate (02 §1.1 gate-object rules): the checks call into the CRT, which does
# not exist yet when the gate runs. CMake's MSVC Debug default puts /RTC1 into CMAKE_C_FLAGS_DEBUG, which
# applies per directory and which no target property can take back; this directory's only C units are the
# gate's, so the C flags lose it here (C++ keeps it). Audit check 1 rejects a gate unit with /RTC.
if(MSVC)
  string(REGEX REPLACE "(^| )[-/]RTC[1csu]+( |$)" " " CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()
set(HELIOS_CORE_CPUGATE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/cpugate/cpu_gate.c)
if(WIN32)
  set(HELIOS_CORE_CPUGATE_HOOK ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/cpu_gate_hook.c)
else()
  set(HELIOS_CORE_CPUGATE_HOOK ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/cpu_gate_hook.c)
endif()
add_library(helios_core_cpugate OBJECT ${HELIOS_CORE_CPUGATE_SOURCES})
add_library(helios_core_cpugate_hook OBJECT ${HELIOS_CORE_CPUGATE_HOOK})
foreach(gate helios_core_cpugate helios_core_cpugate_hook)
  target_include_directories(${gate} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(${gate} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER engine)
  helios_apply_warnings(${gate})
  helios_cpu_gate_target(${gate})
endforeach()

helios_module(core HEADLESS
  SOURCES ${HELIOS_CORE_SOURCES} ${HELIOS_CORE_PLATFORM_SOURCES}
  DEPS Threads::Threads ${CMAKE_DL_LIBS}
  PRIVATE_DEPS helios::tp::headers helios::tp::mimalloc helios_core_cpugate)

if(WIN32)
  # dbghelp: MiniDumpWriteDump (crash.h); bcrypt: BCryptGenRandom (guid.h / secure random).
  target_link_libraries(helios_core PRIVATE dbghelp bcrypt)
endif()

# Version numbers come from the top-level project() so there is a single source of truth.
# Asserts stay enabled in the "development" configurations (Debug, RelWithDebInfo) and are compiled
# out of Release/MinSizeRel ("shipping"). PUBLIC so every consumer sees identical macro values.
target_compile_definitions(helios_core PUBLIC
  HELIOS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  HELIOS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  HELIOS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  $<$<CONFIG:Debug,RelWithDebInfo>:HELIOS_ENABLE_ASSERTS=1>)

# Build info: only version.cpp sees these, so changing the hash recompiles a single file.
# CI passes -DHELIOS_GIT_HASH=<sha>; local builds keep the placeholder (CMake never runs git here).
set(HELIOS_GIT_HASH "unknown" CACHE STRING "Git commit hash embedded in the build info (set by CI)")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp PROPERTIES COMPILE_DEFINITIONS
  "HELIOS_BUILD_CONFIG_NAME=$<CONFIG>;HELIOS_GIT_HASH_NAME=${HELIOS_GIT_HASH}")

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
file(GLOB HELIOS_CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(core_tests SOURCES ${HELIOS_CORE_TEST_SOURCES} DEPS helios::core)

if(TARGET core_tests)
  # A tiny plugin DLL/.so exercising DynamicLibrary + HELIOS_PLUGIN_EXPORT.
  add_library(helios_core_test_plugin SHARED ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/test_plugin.cpp)
  target_include_directories(helios_core_test_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(helios_core_test_plugin PROPERTIES FOLDER tests)
  helios_apply_warnings(helios_core_test_plugin)

  # A child process that installs the crash handler and crashes on purpose (crash.h test).
  add_executable(core_crash_child ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/crash_child.cpp)
  target_link_libraries(core_crash_child PRIVATE helios::core)
  set_target_properties(core_crash_child PROPERTIES FOLDER tests)
  helios_apply_warnings(core_crash_child)

  # Child for the process tests (echo arguments, cat, exit codes, env, cwd, inherited pipes).
  add_executable(core_process_child ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/process_child.cpp)
  target_link_libraries(core_process_child PRIVATE helios::core)
  set_target_properties(core_process_child PROPERTIES FOLDER tests)
  helios_apply_warnings(core_process_child)
  helios_windows_manifest(core_process_child)

  # CPU-gate children (02 §1.1; tests/test_cpu.cpp):
  #  - core_cpugate_child: the real pre-initializer (passes on the CI CPUs) through helios_executable()'s
  #    CPU_GATE, as a gated image gets it (in a modular Windows build from helios_runtime.dll);
  #  - core_cpugate_child_snb: the same hook built to evaluate a recorded Sandy Bridge CPUID instead of the
  #    live CPU, so the refusal path (message, exit code 78, nothing of main or C++ static initialization
  #    runs) is tested on any machine; on Windows also as a WINDOWS_GUI image (core_cpugate_child_snb_gui),
  #    whose refusal shows a dialog unless HELIOS_CPU_GATE_SILENT=1;
  #  - core_cpugate_child_nohook: no gate at all, the "dropped hook" case: core::platformInit() must stop it
  #    with "CPU gate did not run". Self-contained, so a modular Windows build does not hand it the gate of
  #    helios_runtime.dll.
  helios_executable(core_cpugate_child CPU_GATE
                    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child.cpp DEPS helios::core)
  get_target_property(gateInRuntime core_cpugate_child HELIOS_CPU_GATE_IN_RUNTIME)
  set(cpugateRuntimeDll "")
  if(NOT gateInRuntime)
    # The hook (and on Windows its TLS slot) is in this executable, not in helios_runtime.dll.
    target_compile_definitions(core_cpugate_child PRIVATE HELIOS_CPUGATE_CHILD_HOOK_IN_IMAGE=1)
  else()
    # The hook is in helios_runtime.dll, which exports its TLS slot (cmake/HeliosModular.cmake); the child
    # and the test both need the DLL's name.
    set(cpugateRuntimeDll HELIOS_CPUGATE_RUNTIME_DLL="$<TARGET_FILE_NAME:helios_runtime>")
    target_compile_definitions(core_cpugate_child PRIVATE ${cpugateRuntimeDll})
  endif()
  # The Sandy Bridge hook is a third gate object library: the gate's units are never compiled inside an
  # avx2 target.
  add_library(core_cpugate_hook_snb OBJECT ${HELIOS_CORE_CPUGATE_HOOK})
  target_compile_definitions(core_cpugate_hook_snb PRIVATE HELIOS_CPU_GATE_TEST_EMULATE_SANDY_BRIDGE=1)
  target_include_directories(core_cpugate_hook_snb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(core_cpugate_hook_snb PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER tests)
  helios_apply_warnings(core_cpugate_hook_snb)
  helios_cpu_gate_target(core_cpugate_hook_snb)
  add_executable(core_cpugate_child_snb ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child.cpp
                                         $<TARGET_OBJECTS:core_cpugate_hook_snb>)
  target_link_libraries(core_cpugate_child_snb PRIVATE helios::core)
  add_executable(core_cpugate_child_nohook ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child.cpp)
  target_link_libraries(core_cpugate_child_nohook PRIVATE helios::core)
  helios_self_contained(core_cpugate_child_nohook)
  set(cpugateChildren core_cpugate_child core_cpugate_child_snb core_cpugate_child_nohook)
  if(WIN32)
    add_executable(core_cpugate_child_snb_gui WIN32
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child_gui.cpp $<TARGET_OBJECTS:core_cpugate_hook_snb>)
    target_link_libraries(core_cpugate_child_snb_gui PRIVATE helios::core)
    list(APPEND cpugateChildren core_cpugate_child_snb_gui)
  endif()
  foreach(child IN LISTS cpugateChildren)
    set_target_properties(${child} PROPERTIES FOLDER tests)
    if(NOT child STREQUAL "core_cpugate_child") # helios_executable() did both
      helios_apply_warnings(${child})
      helios_windows_manifest(${child})
    endif()
  endforeach()

  add_dependencies(core_tests helios_core_test_plugin core_crash_child core_process_child ${cpugateChildren})
  target_compile_definitions(core_tests PRIVATE
    HELIOS_TEST_PLUGIN_PATH="$<TARGET_FILE:helios_core_test_plugin>"
    HELIOS_CRASH_CHILD_PATH="$<TARGET_FILE:core_crash_child>"
    HELIOS_PROCESS_CHILD_PATH="$<TARGET_FILE:core_process_child>"
    HELIOS_CPUGATE_CHILD_PATH="$<TARGET_FILE:core_cpugate_child>"
    HELIOS_CPUGATE_CHILD_SNB_PATH="$<TARGET_FILE:core_cpugate_child_snb>"
    HELIOS_CPUGATE_CHILD_NOHOOK_PATH="$<TARGET_FILE:core_cpugate_child_nohook>")
  if(WIN32)
    target_compile_definitions(core_tests PRIVATE
      HELIOS_CPUGATE_CHILD_SNB_GUI_PATH="$<TARGET_FILE:core_cpugate_child_snb_gui>" ${cpugateRuntimeDll})
  endif()
  # Stress tests (1M jobs etc.) get generous time on slow CI machines.
  set_tests_properties(core_tests PROPERTIES TIMEOUT 600)
endif()

# ---------------------------------------------------------------------------------------------
# Benchmarks. core_memory_bench compares sharded tag accounting with a replica of the pre-WP-0.5
# single-slot counters (A/B in one binary); `--gate` enforces the >= 2x speed-up at >= 4 threads.
# The CTest entry only checks that the accounting balances (timings are not asserted there).
# ---------------------------------------------------------------------------------------------
helios_executable(core_memory_bench SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/bench/memory_bench.cpp DEPS helios::core)
set_target_properties(core_memory_bench PROPERTIES FOLDER bench)
if(HELIOS_BUILD_TESTS)
  add_test(NAME core_memory_bench_smoke COMMAND core_memory_bench --ops=20000 --threads=4)
  set_tests_properties(core_memory_bench_smoke PROPERTIES LABELS bench TIMEOUT 300)
endif()
