cmake_minimum_required(VERSION 3.24)
project(Helios VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------------
option(HELIOS_BUILD_GRAPHICS "Build graphical targets (client, editor, launcher). OFF = headless server/tools only" ON)
option(HELIOS_BUILD_TESTS    "Build unit tests" ON)
option(HELIOS_PROFILE        "Enable Tracy profiling zones" OFF)
option(HELIOS_WARNINGS_AS_ERRORS "Treat engine warnings as errors" OFF)
option(HELIOS_SANITIZE       "Build with address+undefined sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

if(HELIOS_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(HeliosModule)

# ---------------------------------------------------------------------------------------------
# Third-party (vendored, see third_party/MANIFEST.md)
# ---------------------------------------------------------------------------------------------
add_subdirectory(third_party)

if(HELIOS_BUILD_TESTS)
  enable_testing()
endif()

# ---------------------------------------------------------------------------------------------
# Engine modules, applications, tests. Directories are added only if present so the tree can
# grow module by module.
# ---------------------------------------------------------------------------------------------
foreach(dir engine apps tests)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
