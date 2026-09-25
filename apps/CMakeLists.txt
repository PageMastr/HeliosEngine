# Applications (layer 5). Graphical apps are skipped in headless builds (HELIOS_BUILD_GRAPHICS=OFF).
#
# Declare executables with helios_executable(); its ROLE is inferred from the directory
# (client, launcher, cellserver -> cell, gateway, voice, editor, bot, tools/* -> tool, samples ->
# sample) and decides the configure-time checks (cmake/HeliosLayering.cmake, 02 §1.1):
#   * client, launcher, bot, cell, gateway, voice: never link an EDITOR_ONLY module;
#   * cell, gateway, voice, bot: link only HEADLESS modules and no graphics third-party library.
# It also embeds the Windows manifest and, for AVX2 image roles, links the CPU gate.
set(HELIOS_HEADLESS_APPS cellserver gateway voice bot tools)
file(GLOB children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(a ${children})
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${a} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${a}/CMakeLists.txt)
    if(NOT HELIOS_BUILD_GRAPHICS AND NOT a IN_LIST HELIOS_HEADLESS_APPS)
      continue()
    endif()
    add_subdirectory(${a})
  endif()
endforeach()
