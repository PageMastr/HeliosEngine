# helios-editor: the editor (07 §1). ROLE editor (from the directory): may link EDITOR_ONLY modules;
# the process is edui::EditorHost (engine/editorui). See README.md.
helios_executable(helios-editor
  SOURCES main.cpp
  DEPS helios::editorui helios::toolsfw helios_toolsfw_samples helios::core)

# Smoke test: 30 frames with a screenshot (needs a display; Linux CI runs it under xvfb-run with
# lavapipe, label "gpu"). The UI goldens (ED-15) run through helios-uitest (apps/tools).
if(HELIOS_BUILD_TESTS)
  set(helios_editor_smoke_args --no-journal --rpc=- --frames=30 --width=1024 --height=640
      --screenshot=${CMAKE_CURRENT_BINARY_DIR}/helios_editor_smoke.png
      --project-root=${PROJECT_SOURCE_DIR}/engine/toolsfw/samples/project)
  if(WIN32)
    add_test(NAME helios_editor_smoke COMMAND helios-editor ${helios_editor_smoke_args})
    set_tests_properties(helios_editor_smoke PROPERTIES LABELS gpu TIMEOUT 120)
  else()
    find_program(HELIOS_XVFB_RUN xvfb-run)
    if(HELIOS_XVFB_RUN)
      # Sanitizer builds keep LeakSanitizer clean without suppressions, as rhi_triangle_smoke does
      # (apps/samples/rhi_triangle): a private session bus for libdbus, and lavapipe kept loaded
      # (libX11 stays mapped through SDL_X11_SHARED=OFF in third_party/CMakeLists.txt).
      set(helios_editor_launcher ${HELIOS_XVFB_RUN} -a)
      if(HELIOS_SANITIZE)
        find_program(HELIOS_DBUS_RUN_SESSION dbus-run-session)
        if(HELIOS_DBUS_RUN_SESSION)
          list(PREPEND helios_editor_launcher ${HELIOS_DBUS_RUN_SESSION} --)
        endif()
      endif()
      add_test(NAME helios_editor_smoke COMMAND ${helios_editor_launcher} $<TARGET_FILE:helios-editor> ${helios_editor_smoke_args})
      set_tests_properties(helios_editor_smoke PROPERTIES LABELS gpu TIMEOUT 120)
      if(HELIOS_SANITIZE)
        set_tests_properties(helios_editor_smoke PROPERTIES ENVIRONMENT_MODIFICATION
          "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=set:1")
      endif()
    endif()
  endif()
endif()
