# Tool executables (ROLE tool, from the directory: may link EDITOR_ONLY modules). The headless
# preset builds this directory too, so a tool that needs graphics is added only when
# HELIOS_BUILD_GRAPHICS is on.
#   helios-tool    headless ToolsFramework CLI (07 §1.1): ToolsFramework without EditorUI.
add_subdirectory(helios-tool)
