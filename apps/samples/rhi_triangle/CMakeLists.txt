# rhi_triangle: SDL3 window + Vulkan swapchain + a spinning vertex-pulled triangle, with resize
# handling. `rhi_triangle --frames=60 --screenshot=out.png` renders 60 frames, saves the last one
# and exits (works under `xvfb-run -a` on headless Linux).
include(HeliosShaders)

helios_executable(rhi_triangle
  SOURCES main.cpp
  DEPS helios::rhi helios::core helios::tp::sdl3 helios::tp::headers)
helios_shaders(rhi_triangle NAMESPACE rhi_triangle_shaders FILES triangle.slang)
set_target_properties(rhi_triangle PROPERTIES FOLDER apps/samples)

# Smoke test: needs a display; on Linux CI it runs under xvfb-run with lavapipe (label "gpu").
if(HELIOS_BUILD_TESTS)
  if(WIN32)
    add_test(NAME rhi_triangle_smoke COMMAND rhi_triangle --frames=30
             --screenshot=${CMAKE_CURRENT_BINARY_DIR}/rhi_triangle_smoke.png)
    set_tests_properties(rhi_triangle_smoke PROPERTIES LABELS gpu TIMEOUT 120)
  else()
    find_program(HELIOS_XVFB_RUN xvfb-run)
    if(HELIOS_XVFB_RUN)
      set(rhi_triangle_launcher ${HELIOS_XVFB_RUN} -a)
      # Sanitizer builds keep LeakSanitizer clean without suppressions. Each external report seen in
      # the diagnosis (nightly 36309067888 and a local container without a session bus) has its own
      # remedy: libX11 stays mapped (SDL_X11_SHARED=OFF, third_party/CMakeLists.txt), lavapipe stays
      # loaded (below), and libdbus gets a session bus.
      if(HELIOS_SANITIZE)
        # libdbus loses a read buffer when SDL_DBus_Init finds no session bus, as under a bare
        # xvfb-run in a container (not in the nightly log). A private session bus avoids that path.
        find_program(HELIOS_DBUS_RUN_SESSION dbus-run-session)
        if(HELIOS_DBUS_RUN_SESSION)
          list(PREPEND rhi_triangle_launcher ${HELIOS_DBUS_RUN_SESSION} --)
        else()
          message(STATUS "rhi_triangle_smoke: dbus-run-session not found (package dbus-daemon); without a "
                         "session bus LeakSanitizer may report a libdbus read buffer")
        endif()
      endif()
      add_test(NAME rhi_triangle_smoke COMMAND ${rhi_triangle_launcher} $<TARGET_FILE:rhi_triangle> --frames=30
               --resize-at=10 --screenshot=${CMAKE_CURRENT_BINARY_DIR}/rhi_triangle_smoke.png)
      set_tests_properties(rhi_triangle_smoke PROPERTIES LABELS gpu TIMEOUT 120)
      if(HELIOS_SANITIZE)
        # The Vulkan loader's switch for leak tools: the ICDs (lavapipe) stay loaded at
        # vkDestroyInstance, so a driver thread's allocations stay reachable and symbolised. Seen only
        # locally (not in the nightly log). Added to the job's environment, not replacing it.
        set_tests_properties(rhi_triangle_smoke PROPERTIES ENVIRONMENT_MODIFICATION
          "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=set:1")
      endif()
    endif()
  endif()
endif()
