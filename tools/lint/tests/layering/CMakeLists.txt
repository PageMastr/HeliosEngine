# Module-layering fixture (tools/lint/CMakeLists.txt runs it once per HELIOS_FIXTURE_CASE). It uses the
# real cmake/HeliosModule.cmake and a miniature copy of engine/CMakeLists.txt's table. Good cases
# print HELIOS_FIXTURE_CONFIGURE_OK after the checks; bad cases must stop configure with the
# "helios layering:" diagnostic the test expects. LANGUAGES NONE keeps it fast: no compiler is
# probed, and the fixture stops before generation (its sources are never compiled).
cmake_minimum_required(VERSION 3.28)
project(HeliosLayeringFixture LANGUAGES NONE)

if(NOT HELIOS_SOURCE_DIR)
  message(FATAL_ERROR "pass -DHELIOS_SOURCE_DIR=<repository root>")
endif()
set(HELIOS_BUILD_TESTS OFF)
list(APPEND CMAKE_MODULE_PATH ${HELIOS_SOURCE_DIR}/cmake)
include(HeliosModule)

set(src ${CMAKE_CURRENT_SOURCE_DIR}/dummy.cpp)
set(CASE "${HELIOS_FIXTURE_CASE}")

# Stand-ins for graphics and ordinary third-party targets.
add_library(SDL3-static INTERFACE)
add_library(helios::tp::sdl3 ALIAS SDL3-static)
add_library(tp_yyjson INTERFACE)
add_library(helios::tp::yyjson ALIAS tp_yyjson)
add_library(fx_helper INTERFACE) # a non-module helper target between a module and a library

# Miniature layering table (same shape as engine/CMakeLists.txt).
helios_declare_module(core 1 HEADLESS)
helios_declare_module(math 1 HEADLESS)
helios_declare_module(reflect 2 HEADLESS)
helios_declare_module(asset 2 HEADLESS PEERS reflect)
if(CASE STREQUAL "cycle" OR CASE STREQUAL "bridge_cycle")
  # Declared peers of each other, so only the cycle check can catch it.
  helios_declare_module(ecs 2 HEADLESS PEERS reflect app)
  helios_declare_module(app 2 PEERS reflect ecs)
else()
  helios_declare_module(ecs 2 HEADLESS PEERS reflect)
  helios_declare_module(app 2 PEERS reflect)
endif()
helios_declare_module(physics 3 HEADLESS)
helios_declare_module(net 3 HEADLESS)
helios_declare_module(render 3)
helios_declare_module(world 4 HEADLESS)
helios_declare_module(presentation 4 PEERS world)
helios_declare_module(assetpipe 4 EDITOR_ONLY)
helios_declare_module(toolsfw 4 EDITOR_ONLY PEERS world assetpipe)
set(order core math reflect asset ecs app physics net render world presentation assetpipe toolsfw)
if(CASE STREQUAL "bad_order")
  set(order core math ecs reflect asset app physics net render world presentation assetpipe toolsfw)
elseif(CASE STREQUAL "explicit_layer")
  list(APPEND order zonehost)
elseif(CASE STREQUAL "unordered")
  list(REMOVE_ITEM order net) # declared, built and depended on, but missing from the order
endif()
set_property(GLOBAL PROPERTY HELIOS_MODULE_ORDER ${order})

# A valid graph (every case starts from it).
helios_module(core SOURCES ${src} DEPS helios::tp::yyjson)
helios_module(math SOURCES ${src})
if(CASE STREQUAL "legacy_signature")
  # The pre-WP-0.2 call shape: HEADLESS SOURCES DEPS, no LAYER (taken from the table).
  helios_module(reflect HEADLESS SOURCES ${src} DEPS helios::core helios::math PRIVATE_DEPS helios::tp::yyjson)
elseif(CASE STREQUAL "layer_mismatch")
  helios_module(reflect HEADLESS LAYER 3 SOURCES ${src} DEPS helios::core)
else()
  helios_module(reflect SOURCES ${src} DEPS helios::core helios::math)
endif()
helios_module(asset SOURCES ${src} DEPS helios::core PRIVATE_DEPS helios::reflect)
if(CASE STREQUAL "same_layer")
  helios_module(ecs SOURCES ${src} DEPS helios::core helios::reflect)
  helios_module(app SOURCES ${src} DEPS helios::core helios::ecs) # ecs is not a listed peer of app
elseif(CASE STREQUAL "cycle")
  helios_module(ecs SOURCES ${src} DEPS helios::core helios::app)
  helios_module(app SOURCES ${src} DEPS helios::core helios::ecs)
else()
  helios_module(ecs SOURCES ${src} DEPS helios::core helios::reflect)
  helios_module(app SOURCES ${src} DEPS helios::core helios::reflect helios::tp::sdl3)
endif()
if(CASE STREQUAL "headless_module")
  helios_module(physics SOURCES ${src} DEPS helios::core PRIVATE_DEPS helios::render)
else()
  helios_module(physics SOURCES ${src} DEPS helios::core helios::math)
endif()
if(CASE STREQUAL "genex_build_interface")
  # Dependencies hidden in generator expressions still count (regression: they were skipped).
  helios_module(net SOURCES ${src} DEPS helios::core PRIVATE_DEPS $<BUILD_INTERFACE:helios::tp::sdl3>)
elseif(CASE STREQUAL "genex_nested_condition")
  helios_module(net SOURCES ${src} DEPS helios::core
    PRIVATE_DEPS "$<$<AND:$<BOOL:1>,$<NOT:$<CONFIG:Shipping>>>:helios::tp::sdl3>")
elseif(CASE STREQUAL "genex_if")
  helios_module(net SOURCES ${src} DEPS helios::core
    PRIVATE_DEPS "$<IF:$<AND:$<BOOL:0>,$<CONFIG:Debug>>,helios::tp::yyjson,helios::tp::sdl3>")
elseif(CASE STREQUAL "flag_mismatch")
  # The table does not make net EDITOR_ONLY; the call may not add the flag.
  helios_module(net EDITOR_ONLY SOURCES ${src} DEPS helios::core)
elseif(CASE STREQUAL "peers_mismatch")
  # net's row lists no peers; the call may not grant itself physics (same layer).
  helios_module(net PEERS physics SOURCES ${src} DEPS helios::core helios::physics)
elseif(CASE STREQUAL "redeclared_row")
  # A second row for net, with a peer the table does not give it.
  helios_declare_module(net 3 HEADLESS PEERS physics)
  helios_module(net SOURCES ${src} DEPS helios::core helios::physics)
elseif(CASE STREQUAL "headless_graphics_tp")
  target_link_libraries(fx_helper INTERFACE helios::tp::sdl3)
  helios_module(net SOURCES ${src} DEPS helios::core PRIVATE_DEPS fx_helper)
else()
  helios_module(net SOURCES ${src} DEPS helios::core helios::reflect)
endif()
if(CASE STREQUAL "upward")
  helios_module(render SOURCES ${src} DEPS helios::core helios::app)
  target_link_libraries(helios_reflect PRIVATE helios::net) # layer 2 -> layer 3
else()
  helios_module(render SOURCES ${src} DEPS helios::core helios::app helios::asset)
endif()
if(CASE STREQUAL "headless_module")
  # world is HEADLESS; physics (HEADLESS by the table) now reaches render: world -> physics -> render.
  helios_module(world SOURCES ${src} DEPS helios::ecs helios::physics)
elseif(CASE STREQUAL "runtime_to_editor")
  helios_module(world SOURCES ${src} DEPS helios::ecs helios::physics helios::assetpipe)
else()
  helios_module(world SOURCES ${src} DEPS helios::ecs helios::physics helios::net)
endif()
helios_module(presentation SOURCES ${src} DEPS helios::world helios::render)
helios_module(assetpipe SOURCES ${src} DEPS helios::asset helios::physics)
helios_module(toolsfw SOURCES ${src} DEPS helios::world helios::assetpipe)
if(CASE STREQUAL "missing_layer")
  helios_module(mystery SOURCES ${src} DEPS helios::core)
elseif(CASE STREQUAL "explicit_layer")
  # A module without a table row that passes LAYER itself (the documented alternative to a row).
  # Regression: this failed with "contradicts the layering table ... on layer " (empty).
  helios_module(zonehost HEADLESS LAYER 4 PEERS world SOURCES ${src} DEPS helios::world)
endif()

# Module edges that only a non-module target carries (an INTERFACE or STATIC helper between two
# modules) are module edges too.
if(CASE STREQUAL "bridge_upward")
  add_library(fx_bridge INTERFACE) # reflect (L2) -> fx_bridge -> world (L4); also a cycle through ecs
  target_link_libraries(fx_bridge INTERFACE helios::world)
  target_link_libraries(helios_reflect PRIVATE fx_bridge)
elseif(CASE STREQUAL "bridge_same_layer")
  add_library(fx_bridge INTERFACE) # net -> fx_bridge -> physics, both L3, not peers
  target_link_libraries(fx_bridge INTERFACE helios::physics)
  target_link_libraries(helios_net PRIVATE fx_bridge)
elseif(CASE STREQUAL "bridge_cycle")
  add_library(fx_bridge STATIC ${src}) # ecs -> fx_bridge -> app -> ecs (declared peers): a cycle
  target_link_libraries(fx_bridge PRIVATE helios::app)
  target_link_libraries(helios_ecs PRIVATE fx_bridge)
  target_link_libraries(helios_app PRIVATE helios::ecs)
endif()

# Executables. CPU gate off: the fixture has no engine/core hook target.
if(CASE STREQUAL "editor_only_client")
  helios_executable(fx-client ROLE client NO_CPU_GATE SOURCES ${src} DEPS helios::presentation helios::toolsfw)
else()
  helios_executable(fx-client ROLE client NO_CPU_GATE SOURCES ${src} DEPS helios::presentation)
endif()
if(CASE STREQUAL "editor_only_server")
  helios_executable(fx-cell ROLE cell NO_CPU_GATE SOURCES ${src} DEPS helios::world helios::assetpipe)
elseif(CASE STREQUAL "server_graphics")
  helios_executable(fx-cell ROLE cell NO_CPU_GATE SOURCES ${src} DEPS helios::world helios::presentation)
else()
  helios_executable(fx-cell ROLE cell NO_CPU_GATE SOURCES ${src} DEPS helios::world)
endif()
if(CASE STREQUAL "editor_only_gateway")
  helios_executable(fx-gateway ROLE gateway NO_CPU_GATE SOURCES ${src} DEPS helios::net helios::assetpipe)
endif()
if(CASE STREQUAL "plain_executable")
  add_subdirectory(apps/rogue) # add_executable() under apps/, outside every role check
elseif(CASE STREQUAL "unknown_role")
  add_subdirectory(apps/zonehost) # helios_executable() without ROLE where no role can be inferred
endif()
# Tools may link editor-only modules.
helios_executable(fx-cook ROLE tool NO_CPU_GATE SOURCES ${src} DEPS helios::assetpipe)

# ISA levels (02 §1.1, cmake/HeliosIsa.cmake; WP-0.2r). A base image (launcher, bootstrap) links the base
# copies of core and its libraries; linking anything built only at avx2 stops configure.
add_library(tp_jolt STATIC ${src}) # stands in for Jolt: an avx2-only third-party library
if(CASE STREQUAL "isa_base_ok")
  helios_executable(fx-launcher ROLE launcher SOURCES ${src} DEPS helios::core)
elseif(CASE STREQUAL "isa_base_links_jolt")
  # 09 §2 WP-0.2r's seeded fixture: a base image that links tp_jolt.
  helios_executable(fx-launcher ROLE launcher SOURCES ${src} DEPS helios::core tp_jolt)
elseif(CASE STREQUAL "isa_base_links_physics")
  # Through a helper that is not a module: the path names it.
  target_link_libraries(fx_helper INTERFACE helios::physics)
  helios_executable(fx-bootstrap ROLE bootstrap SOURCES ${src} DEPS helios::core fx_helper)
elseif(CASE STREQUAL "isa_fixture_base")
  # The fixtures-only override: a tool image at base, outside apps/ and engine/.
  helios_executable(fx-isa-fixture ROLE tool ISA base SOURCES ${src} DEPS helios::core)
elseif(CASE STREQUAL "isa_override_in_apps")
  add_subdirectory(apps/isa) # helios_executable(... ISA base) where real images live
elseif(CASE STREQUAL "isa_interface_options")
  # tp_jolt's options before WP-0.2r, on its interface: every consumer inherited them.
  target_compile_options(tp_jolt INTERFACE -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c) # conformance:allow CONF-11 the configure check's seeded fixture (09 §5.10.3)
endif()

# Runs after helios_finalize_build (deferred calls run in scheduling order), so it is only reached
# when the checks passed. Stop before generation: the dummy sources have no enabled language.
cmake_language(DEFER CALL message STATUS "HELIOS_FIXTURE_CONFIGURE_OK")
cmake_language(DEFER CALL message FATAL_ERROR "fixture stops here (not an error of the case under test)")
