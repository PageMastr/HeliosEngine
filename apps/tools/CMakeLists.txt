# Tool executables (ROLE tool, from the directory: may link EDITOR_ONLY modules). The headless
# preset builds this directory too, so a tool that needs graphics is added only when
# HELIOS_BUILD_GRAPHICS is on.
#   helios-tool    headless ToolsFramework CLI (07 §1.1): ToolsFramework without EditorUI.
#   helios-cook    cook CLI (07 §1.10); v0 cooks records (engine/records) into the client and server .hrdb.
#   helios-uitest  editor UI test driver (07 §4.4, ED-15): drives helios-editor over the
#                  remote-control socket; graphical builds only (its tests need the editor).
add_subdirectory(helios-tool)
add_subdirectory(helios-cook)
if(HELIOS_BUILD_GRAPHICS)
  add_subdirectory(helios-uitest)
endif()
