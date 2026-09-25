# engine/reflect — L2 runtime reflection (HEADLESS): TypeInfo/TypeRegistry, compiled + reflective
# codecs (canonical JSONC via yyjson, tagged binary), property paths, diff/patch, record files.
# helios-schemac (tools/schemac) generates the code that plugs into this module (02 §3, ADR-004).

helios_module(reflect HEADLESS
  SOURCES
    src/builder.cpp
    src/codec.cpp
    src/json.cpp
    src/patch.cpp
    src/path.cpp
    src/record.cpp
    src/registry.cpp
    src/serialize.cpp
    src/tagged.cpp
    src/type_info.cpp
  DEPS helios::core helios::math
  PRIVATE_DEPS helios::tp::yyjson)

file(GLOB HELIOS_REFLECT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(reflect_tests SOURCES ${HELIOS_REFLECT_TEST_SOURCES} DEPS helios::reflect)
