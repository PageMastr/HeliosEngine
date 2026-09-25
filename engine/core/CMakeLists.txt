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

helios_module(core HEADLESS
  SOURCES ${HELIOS_CORE_SOURCES} ${HELIOS_CORE_PLATFORM_SOURCES}
  DEPS Threads::Threads ${CMAKE_DL_LIBS}
  PRIVATE_DEPS helios::tp::headers helios::tp::mimalloc)

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

  add_dependencies(core_tests helios_core_test_plugin core_crash_child)
  target_compile_definitions(core_tests PRIVATE
    HELIOS_TEST_PLUGIN_PATH="$<TARGET_FILE:helios_core_test_plugin>"
    HELIOS_CRASH_CHILD_PATH="$<TARGET_FILE:core_crash_child>")
  # Stress tests (1M jobs etc.) get generous time on slow CI machines.
  set_tests_properties(core_tests PROPERTIES TIMEOUT 600)
endif()
