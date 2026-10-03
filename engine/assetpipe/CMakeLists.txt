# engine/assetpipe — L4 asset pipeline (EDITOR_ONLY; 02 §6.2, §6.3). v0 holds the `.hpak` v0 writer that
# paks are built with (helios-pack drives it later); importers, bakers, the DDC and the cooker come with
# later parts of WP-0.8 and with Phase 1.

helios_module(assetpipe EDITOR_ONLY
  SOURCES
    src/hpak_writer.cpp
  DEPS helios::core helios::asset
  PRIVATE_DEPS helios::tp::zstd)

file(GLOB HELIOS_ASSETPIPE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
helios_test(assetpipe_tests SOURCES ${HELIOS_ASSETPIPE_TEST_SOURCES} DEPS helios::assetpipe)
