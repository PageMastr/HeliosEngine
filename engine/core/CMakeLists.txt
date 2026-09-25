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

# CPU gate (02 §1.1, 08 §2.2): the probe is a C unit pinned to the x86-64-v1 baseline so it runs on
# any x86-64 CPU; the per-OS pre-initializer hook is an object library that helios_cpu_gate()
# (cmake/HeliosIsa.cmake) links into every AVX2 executable. The ISA audit checks both.
set(HELIOS_CORE_CPUGATE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/cpugate/cpu_gate.c)
if(WIN32)
  set(HELIOS_CORE_CPUGATE_HOOK ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/cpu_gate_hook.c)
else()
  set(HELIOS_CORE_CPUGATE_HOOK ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/cpu_gate_hook.c)
endif()
helios_cpu_gate_sources(${HELIOS_CORE_CPUGATE_SOURCES} ${HELIOS_CORE_CPUGATE_HOOK})

helios_module(core HEADLESS
  SOURCES ${HELIOS_CORE_SOURCES} ${HELIOS_CORE_PLATFORM_SOURCES} ${HELIOS_CORE_CPUGATE_SOURCES}
  DEPS Threads::Threads ${CMAKE_DL_LIBS}
  PRIVATE_DEPS helios::tp::headers helios::tp::mimalloc)

add_library(helios_core_cpugate_hook OBJECT ${HELIOS_CORE_CPUGATE_HOOK})
target_include_directories(helios_core_cpugate_hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(helios_core_cpugate_hook PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER engine)
helios_apply_warnings(helios_core_cpugate_hook)

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

  # CPU-gate children: the real pre-initializer (passes on the CI CPUs), and the same hook built to
  # evaluate a recorded Sandy Bridge CPUID instead of the live CPU, so the refusal path (message,
  # exit code 78, nothing of main or C++ static initialization runs) is tested on any machine.
  add_executable(core_cpugate_child ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child.cpp)
  target_link_libraries(core_cpugate_child PRIVATE helios::core)
  helios_cpu_gate(core_cpugate_child)
  add_executable(core_cpugate_child_snb ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/cpugate_child.cpp
                                         ${HELIOS_CORE_CPUGATE_HOOK})
  target_compile_definitions(core_cpugate_child_snb PRIVATE HELIOS_CPU_GATE_TEST_EMULATE_SANDY_BRIDGE=1)
  target_include_directories(core_cpugate_child_snb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(core_cpugate_child_snb PRIVATE helios::core)
  foreach(child core_cpugate_child core_cpugate_child_snb)
    set_target_properties(${child} PROPERTIES FOLDER tests)
    helios_apply_warnings(${child})
    helios_windows_manifest(${child})
  endforeach()

  add_dependencies(core_tests helios_core_test_plugin core_crash_child core_process_child core_cpugate_child
                   core_cpugate_child_snb)
  target_compile_definitions(core_tests PRIVATE
    HELIOS_TEST_PLUGIN_PATH="$<TARGET_FILE:helios_core_test_plugin>"
    HELIOS_CRASH_CHILD_PATH="$<TARGET_FILE:core_crash_child>"
    HELIOS_PROCESS_CHILD_PATH="$<TARGET_FILE:core_process_child>"
    HELIOS_CPUGATE_CHILD_PATH="$<TARGET_FILE:core_cpugate_child>"
    HELIOS_CPUGATE_CHILD_SNB_PATH="$<TARGET_FILE:core_cpugate_child_snb>")
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
