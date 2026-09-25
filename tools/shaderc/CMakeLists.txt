# tools/shaderc — helios-shaderc (ADR-003, docs/plan/03-rendering.md §1.7): Slang -> SPIR-V 1.6 plus the
# Helios reflection blob (.hsr, format in engine/render/include/helios/render/shader_reflection.h),
# spirv-val and depfiles. The Slang compiler library of the pinned prebuilt release
# (tools/prebuilt/fetch_slang.cmake) is loaded at run time; only its headers are needed to build,
# so cross builds (MinGW) link without the target platform's Slang package.

if(TARGET helios-shaderc)
  return()  # already added (engine/render adds the tool until tools/CMakeLists.txt lists it)
endif()

include(HeliosShaders)  # resolves the pinned Slang release (no-op when already included)
get_property(helios_slangc GLOBAL PROPERTY HELIOS_SLANGC)
get_filename_component(helios_slang_root "${helios_slangc}" DIRECTORY)
get_filename_component(helios_slang_root "${helios_slang_root}" DIRECTORY)
if(NOT EXISTS "${helios_slang_root}/include/slang.h")
  message(FATAL_ERROR "helios-shaderc: ${helios_slang_root}/include/slang.h not found (Slang release layout changed?)")
endif()

helios_executable(helios-shaderc ROLE tool
  SOURCES src/main.cpp src/cli.cpp src/slang_compiler.cpp src/tool_process.cpp
  DEPS helios::render helios::core)
target_include_directories(helios-shaderc SYSTEM PRIVATE "${helios_slang_root}/include")
target_include_directories(helios-shaderc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Default Slang location for native builds (the tool also searches $HELIOS_SLANG_ROOT and its own directory).
if(NOT CMAKE_CROSSCOMPILING)
  target_compile_definitions(helios-shaderc PRIVATE HELIOS_SHADERC_DEFAULT_SLANG_ROOT="${helios_slang_root}")
endif()
