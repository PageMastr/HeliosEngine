# A plain executable under the fixture's apps/ directory (case plain_executable): it links an
# EDITOR_ONLY module, and without the apps/ check no role rule would see it.
add_executable(fx-rogue ${src})
target_link_libraries(fx-rogue PRIVATE helios::world helios::assetpipe)
