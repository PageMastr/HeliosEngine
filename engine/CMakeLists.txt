# Engine modules are discovered automatically; order matters only for layering, which each module
# enforces through its DEPS. Modules listed in HELIOS_GRAPHICS_MODULES are skipped for headless builds.
set(HELIOS_MODULE_ORDER core math reflect ecs asset physics net script world gameplay
                        rhi render anim audio ui tools)
set(HELIOS_GRAPHICS_MODULES rhi render audio ui tools)

foreach(m ${HELIOS_MODULE_ORDER})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt)
    if(NOT HELIOS_BUILD_GRAPHICS AND m IN_LIST HELIOS_GRAPHICS_MODULES)
      continue()
    endif()
    add_subdirectory(${m})
  endif()
endforeach()

# Any module directory not in the ordered list is still picked up (appended last).
file(GLOB children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(m ${children})
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${m} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt
     AND NOT m IN_LIST HELIOS_MODULE_ORDER)
    add_subdirectory(${m})
  endif()
endforeach()
