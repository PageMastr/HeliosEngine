# helios-uitest: editor UI test driver (07 §4.4). Drives helios-editor over the remote-control
# socket; needs no graphics itself (ꟻLIP and PNG from engine/render). See README.md.
helios_executable(helios-uitest
  SOURCES main.cpp
  DEPS helios::toolsfw helios::render helios::core)

# ED-15 harness self-test: shell + property grid goldens at 100 % / 200 %, dark and high contrast,
# layout lints, UI-vs-RPC dual path, keyboard reachability. Needs a display and a Vulkan device:
# Linux CI runs it under xvfb-run on lavapipe (label "gpu"); Windows jobs skip the gpu label.
# Goldens: engine/editorui/tests/golden (refresh with --update-goldens and review the images).
# This directory is added only for graphics builds, which always build apps/editor; the editor is
# named by generator expression, so it does not matter whether apps/editor is configured before or
# after this directory (a missing helios-editor target fails generation instead of dropping the test).
if(HELIOS_BUILD_TESTS AND EXISTS ${PROJECT_SOURCE_DIR}/apps/editor/CMakeLists.txt)
  set(helios_uitest_args --editor=$<TARGET_FILE:helios-editor>
      --fixture=${PROJECT_SOURCE_DIR}/engine/toolsfw/samples/project
      --goldens=${PROJECT_SOURCE_DIR}/engine/editorui/tests/golden
      --out=${CMAKE_CURRENT_BINARY_DIR}/ed15)
  if(WIN32)
    add_test(NAME editorui_ed15 COMMAND helios-uitest ${helios_uitest_args})
  else()
    find_program(HELIOS_XVFB_RUN xvfb-run)
    if(HELIOS_XVFB_RUN)
      # Sanitizer builds: the same LeakSanitizer remedies as helios_editor_smoke (apps/editor); the
      # editor process inherits the session bus and the loader setting.
      set(helios_uitest_launcher ${HELIOS_XVFB_RUN} -a)
      if(HELIOS_SANITIZE)
        find_program(HELIOS_DBUS_RUN_SESSION dbus-run-session)
        if(HELIOS_DBUS_RUN_SESSION)
          list(PREPEND helios_uitest_launcher ${HELIOS_DBUS_RUN_SESSION} --)
        endif()
      endif()
      add_test(NAME editorui_ed15 COMMAND ${helios_uitest_launcher} $<TARGET_FILE:helios-uitest> ${helios_uitest_args})
    endif()
  endif()
  if(TEST editorui_ed15)
    set_tests_properties(editorui_ed15 PROPERTIES LABELS gpu TIMEOUT 600)
    if(HELIOS_SANITIZE AND NOT WIN32)
      set_tests_properties(editorui_ed15 PROPERTIES ENVIRONMENT_MODIFICATION
        "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=set:1")
    endif()
  endif()
endif()
