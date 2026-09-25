# Engine modules. The layering table below is 02 §1.1's DAG (normative); helios_module() takes each
# module's LAYER, HEADLESS / EDITOR_ONLY flags and allowed same-layer peers from its row, and
# HeliosLayering.cmake fails configure on any violation (upward or unlisted same-layer dependency,
# cycle, HEADLESS module reaching graphics, EDITOR_ONLY module in a client or server image).
#
#   helios_declare_module(<name> <layer> [HEADLESS] [EDITOR_ONLY] [PEERS <same-layer deps>...])
#
# L1 core → L2 foundation → L3 servers/subsystems → L4 framework (→ L5 apps, in apps/).

# --- L1 ----------------------------------------------------------------------------------------
helios_declare_module(core         1 HEADLESS)
helios_declare_module(math         1 HEADLESS)
# --- L2 ----------------------------------------------------------------------------------------
helios_declare_module(reflect      2 HEADLESS)
helios_declare_module(hxl          2 HEADLESS PEERS reflect)
helios_declare_module(asset        2 HEADLESS PEERS reflect)
helios_declare_module(records      2 HEADLESS PEERS reflect hxl asset)
helios_declare_module(ecs          2 HEADLESS PEERS reflect)
helios_declare_module(loc          2 HEADLESS PEERS asset)
helios_declare_module(telemetry    2 HEADLESS)
helios_declare_module(patch        2 HEADLESS)
helios_declare_module(crash        2 HEADLESS)
helios_declare_module(replay       2 HEADLESS PEERS reflect)
helios_declare_module(app          2 PEERS reflect)
helios_declare_module(input        2 PEERS reflect app)
# --- L3 ----------------------------------------------------------------------------------------
helios_declare_module(physics      3 HEADLESS)
helios_declare_module(anim         3 HEADLESS)
helios_declare_module(nav          3 HEADLESS)
helios_declare_module(pcg          3 HEADLESS)
helios_declare_module(script       3 HEADLESS)
helios_declare_module(net          3 HEADLESS)
helios_declare_module(audio        3)
helios_declare_module(voice        3 PEERS audio net)
helios_declare_module(text         3)
helios_declare_module(ui           3 PEERS text script)
helios_declare_module(rhi          3)
helios_declare_module(render       3 PEERS rhi ui)
# --- L4 ----------------------------------------------------------------------------------------
helios_declare_module(world        4 HEADLESS)
helios_declare_module(gameplay     4 HEADLESS PEERS world)
helios_declare_module(assembly     4 HEADLESS PEERS gameplay)
helios_declare_module(replication  4 HEADLESS PEERS world)
helios_declare_module(authority    4 HEADLESS PEERS world)
helios_declare_module(netgame      4 HEADLESS PEERS world replication authority)
helios_declare_module(clientcore   4 HEADLESS PEERS world replication authority netgame)
helios_declare_module(presentation 4 PEERS world)
helios_declare_module(assetpipe    4 EDITOR_ONLY)
helios_declare_module(toolsfw      4 EDITOR_ONLY PEERS world assetpipe)
helios_declare_module(editorui     4 EDITOR_ONLY PEERS toolsfw)
helios_declare_module(edtools      4 EDITOR_ONLY PEERS toolsfw editorui)

# Build order: a topological order of the table (checked at configure time). Modules listed in
# HELIOS_GRAPHICS_MODULES are skipped for headless builds (HELIOS_BUILD_GRAPHICS=OFF, the
# linux-headless preset: servers and tools).
set(HELIOS_MODULE_ORDER
  core math
  reflect hxl asset records ecs loc telemetry patch crash replay app input
  physics anim nav pcg script net audio voice text ui rhi render
  world gameplay assembly replication authority netgame clientcore presentation assetpipe toolsfw editorui edtools)
set(HELIOS_GRAPHICS_MODULES app input audio voice text ui rhi render presentation editorui edtools)
set_property(GLOBAL PROPERTY HELIOS_MODULE_ORDER ${HELIOS_MODULE_ORDER})

foreach(m ${HELIOS_MODULE_ORDER})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt)
    if(NOT HELIOS_BUILD_GRAPHICS AND m IN_LIST HELIOS_GRAPHICS_MODULES)
      continue()
    endif()
    add_subdirectory(${m})
  endif()
endforeach()

# Any module directory not in the ordered list is still picked up (appended last). Its
# helios_module() call must then pass LAYER explicitly (or the module gets a row above).
file(GLOB children RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(m ${children})
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${m} AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt
     AND NOT m IN_LIST HELIOS_MODULE_ORDER)
    add_subdirectory(${m})
  endif()
endforeach()
