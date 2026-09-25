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
      add_test(NAME rhi_triangle_smoke COMMAND ${HELIOS_XVFB_RUN} -a $<TARGET_FILE:rhi_triangle> --frames=30
               --resize-at=10 --screenshot=${CMAKE_CURRENT_BINARY_DIR}/rhi_triangle_smoke.png)
      set_tests_properties(rhi_triangle_smoke PROPERTIES LABELS gpu TIMEOUT 120)
    endif()
  endif()
endif()
