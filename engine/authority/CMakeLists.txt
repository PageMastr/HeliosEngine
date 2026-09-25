# engine/authority — L4 authority (HEADLESS; 04 §6.1, §3.2, §11.2; 05 §1.4.2): AG / epoch / owner
# vocabulary, the IFence contract with an in-memory fence, the per-zone AG table with the fence
# gate, region-lease holder rule, and the dilatable ZoneClock with its TiDi controller. Its row in
# engine/CMakeLists.txt (02 §1.1) gives the layer and peers.

file(GLOB HELIOS_AUTHORITY_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

helios_module(authority HEADLESS
  SOURCES ${HELIOS_AUTHORITY_SOURCES}
  DEPS helios::core)

file(GLOB HELIOS_AUTHORITY_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(authority_tests SOURCES ${HELIOS_AUTHORITY_TEST_SOURCES} DEPS helios::authority)
if(TARGET authority_tests)
  set_tests_properties(authority_tests PROPERTIES TIMEOUT 300)
endif()
