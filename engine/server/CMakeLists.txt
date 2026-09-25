# engine/server — L4 cell and gateway runtime (HEADLESS; 04 §1, §2.3–2.6, §3, §9; 05 §1.3–1.4, §2):
# ZoneHost / ZoneInstance / per-tick job graph, the trunk and client protocols, CellServer and
# GatewayServer (usable in-process), the control-plane bus (nats.c, and an in-process fake), the
# orchestrator client (lease holder rule, ID blocks) and the Go services' JSON contracts.
#
# 02 §1.1 names no module for the cell's zone host (it lists ZoneHost among what "the engine
# provides", 02 §2.4); until engine/CMakeLists.txt gets this row (and HELIOS_MODULE_ORDER the
# name, after authority), it is declared here. (Passing LAYER to helios_module() for an
# undeclared module trips a quoting bug in cmake/HeliosModule.cmake, so the row form is used.)
# nats.c and yyjson stay private (no third-party types in public headers).
get_property(_heliosServerRow GLOBAL PROPERTY HELIOS_DECL_server_LAYER SET)
if(NOT _heliosServerRow)
  helios_declare_module(server 4 HEADLESS PEERS authority)
endif()

file(GLOB HELIOS_SERVER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

helios_module(server HEADLESS
  SOURCES ${HELIOS_SERVER_SOURCES}
  DEPS helios::core helios::net helios::ecs helios::authority
  PRIVATE_DEPS helios::tp::natsc helios::tp::yyjson)

file(GLOB HELIOS_SERVER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(server_tests SOURCES ${HELIOS_SERVER_TEST_SOURCES} DEPS helios::server helios::tp::yyjson)
if(TARGET server_tests)
  target_include_directories(server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(server_tests PRIVATE
    HELIOS_SERVER_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  set_tests_properties(server_tests PROPERTIES TIMEOUT 600)
endif()
