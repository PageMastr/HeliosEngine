# helios-uitest: editor UI test driver (07 §4.4). Drives helios-editor over the remote-control
# socket; needs no graphics itself (ꟻLIP and PNG from engine/render). See README.md.
helios_executable(helios-uitest
  SOURCES main.cpp
  DEPS helios::toolsfw helios::render helios::core)

# ED-15 harness self-test: shell + property grid goldens at 100 % / 200 %, dark and high contrast,
# layout lints, UI-vs-RPC dual path, keyboard reachability. Needs a display and a Vulkan device:
# Linux CI runs it under xvfb-run on lavapipe (label "gpu"); Windows jobs skip the gpu label.
# Goldens: engine/editorui/tests/golden (refresh with --update-goldens and review the images).
if(HELIOS_BUILD_TESTS AND TARGET helios-editor)
  set(helios_uitest_args --editor=$<TARGET_FILE:helios-editor>
      --fixture=${PROJECT_SOURCE_DIR}/engine/toolsfw/samples/project
      --goldens=${PROJECT_SOURCE_DIR}/engine/editorui/tests/golden
      --out=${CMAKE_CURRENT_BINARY_DIR}/ed15)
  if(WIN32)
    add_test(NAME editorui_ed15 COMMAND helios-uitest ${helios_uitest_args})
  else()
    find_program(HELIOS_XVFB_RUN xvfb-run)
    if(HELIOS_XVFB_RUN)
      add_test(NAME editorui_ed15 COMMAND ${HELIOS_XVFB_RUN} -a $<TARGET_FILE:helios-uitest> ${helios_uitest_args})
    endif()
  endif()
  if(TEST editorui_ed15)
    set_tests_properties(editorui_ed15 PROPERTIES LABELS gpu TIMEOUT 600)
  endif()
endif()
