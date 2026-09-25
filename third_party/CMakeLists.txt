# Vendored third-party libraries. Every target here is built static, with warnings silenced,
# and exposed under a helios::tp:: alias so engine code never depends on vendor target names.
# Versions and licenses are recorded in third_party/MANIFEST.md.

set(TP ${CMAKE_CURRENT_SOURCE_DIR})

function(helios_tp_quiet target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W0)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER third_party)
endfunction()

# --- Vulkan headers + volk (dynamic loader; we never link libvulkan directly) -----------------
add_library(tp_vulkan_headers INTERFACE)
target_include_directories(tp_vulkan_headers SYSTEM INTERFACE ${TP}/vulkan-headers/include)
add_library(helios::tp::vulkan_headers ALIAS tp_vulkan_headers)

add_library(tp_volk STATIC ${TP}/volk/volk.c)
target_include_directories(tp_volk SYSTEM PUBLIC ${TP}/volk)
target_link_libraries(tp_volk PUBLIC tp_vulkan_headers ${CMAKE_DL_LIBS})
target_compile_definitions(tp_volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
  target_compile_definitions(tp_volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
helios_tp_quiet(tp_volk)
add_library(helios::tp::volk ALIAS tp_volk)

# --- Vulkan Memory Allocator (header-only; implementation compiled in rhi) --------------------
add_library(tp_vma INTERFACE)
target_include_directories(tp_vma SYSTEM INTERFACE ${TP}/vma/include)
target_link_libraries(tp_vma INTERFACE tp_volk)
add_library(helios::tp::vma ALIAS tp_vma)

# --- GLFW (windowing/input) -----------------------------------------------------------------
if(HELIOS_BUILD_GRAPHICS)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  add_subdirectory(${TP}/glfw ${CMAKE_CURRENT_BINARY_DIR}/glfw EXCLUDE_FROM_ALL)
  helios_tp_quiet(glfw)
  add_library(helios::tp::glfw ALIAS glfw)

  # --- Dear ImGui (docking) + backends + ImGuizmo + ImPlot ----------------------------------
  add_library(tp_imgui STATIC
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_demo.cpp
    ${TP}/imgui/misc/cpp/imgui_stdlib.cpp
    ${TP}/imgui/backends/imgui_impl_glfw.cpp ${TP}/imgui/backends/imgui_impl_vulkan.cpp
    ${TP}/imguizmo/ImGuizmo.cpp ${TP}/imguizmo/ImSequencer.cpp ${TP}/imguizmo/ImCurveEdit.cpp
    ${TP}/imguizmo/ImGradient.cpp ${TP}/imguizmo/GraphEditor.cpp
    ${TP}/implot/implot.cpp ${TP}/implot/implot_items.cpp)
  target_include_directories(tp_imgui SYSTEM PUBLIC ${TP}/imgui ${TP}/imgui/backends ${TP}/imgui/misc/cpp
                                                    ${TP}/imguizmo ${TP}/implot)
  target_compile_definitions(tp_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DEFINE_MATH_OPERATORS)
  target_link_libraries(tp_imgui PUBLIC tp_volk glfw)
  helios_tp_quiet(tp_imgui)
  add_library(helios::tp::imgui ALIAS tp_imgui)
endif()

# --- Jolt Physics (double precision for large worlds) ---------------------------------------
file(GLOB_RECURSE JOLT_SRC CONFIGURE_DEPENDS ${TP}/jolt/Jolt/*.cpp)
add_library(tp_jolt STATIC ${JOLT_SRC})
target_include_directories(tp_jolt SYSTEM PUBLIC ${TP}/jolt)
# These definitions change Jolt's ABI, so they must be PUBLIC and identical for every consumer.
target_compile_definitions(tp_jolt PUBLIC JPH_DOUBLE_PRECISION JPH_OBJECT_STREAM
                                          $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>)
# x86-64-v3 (AVX2/FMA/F16C/BMI) baseline for every target, since Jolt's inline SIMD paths are
# selected by compiler macros and must match between the library and its consumers.
if(MSVC)
  target_compile_options(tp_jolt PUBLIC /arch:AVX2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_compile_options(tp_jolt PUBLIC -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma)
endif()
helios_tp_quiet(tp_jolt)
add_library(helios::tp::jolt ALIAS tp_jolt)

# --- meshoptimizer ---------------------------------------------------------------------------
file(GLOB MESHOPT_SRC ${TP}/meshoptimizer/src/*.cpp)
add_library(tp_meshoptimizer STATIC ${MESHOPT_SRC})
target_include_directories(tp_meshoptimizer SYSTEM PUBLIC ${TP}/meshoptimizer/src)
helios_tp_quiet(tp_meshoptimizer)
add_library(helios::tp::meshoptimizer ALIAS tp_meshoptimizer)

# --- Header-only: cgltf, stb, miniaudio, xxhash, doctest -------------------------------------
add_library(tp_headers INTERFACE)
target_include_directories(tp_headers SYSTEM INTERFACE ${TP}/cgltf ${TP}/stb ${TP}/miniaudio ${TP}/xxhash)
add_library(helios::tp::headers ALIAS tp_headers)

add_library(tp_doctest INTERFACE)
target_include_directories(tp_doctest SYSTEM INTERFACE ${TP}/doctest)
add_library(helios::tp::doctest ALIAS tp_doctest)

# --- zstd -----------------------------------------------------------------------------------
file(GLOB ZSTD_SRC ${TP}/zstd/lib/common/*.c ${TP}/zstd/lib/compress/*.c ${TP}/zstd/lib/decompress/*.c)
add_library(tp_zstd STATIC ${ZSTD_SRC})
target_include_directories(tp_zstd SYSTEM PUBLIC ${TP}/zstd/lib)
target_compile_definitions(tp_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
helios_tp_quiet(tp_zstd)
add_library(helios::tp::zstd ALIAS tp_zstd)

# --- Recast/Detour navigation ------------------------------------------------------------------
file(GLOB RECAST_SRC ${TP}/recast/Recast/Source/*.cpp ${TP}/recast/Detour/Source/*.cpp
                     ${TP}/recast/DetourCrowd/Source/*.cpp ${TP}/recast/DetourTileCache/Source/*.cpp)
add_library(tp_recast STATIC ${RECAST_SRC})
target_include_directories(tp_recast SYSTEM PUBLIC ${TP}/recast/Recast/Include ${TP}/recast/Detour/Include
                                                   ${TP}/recast/DetourCrowd/Include ${TP}/recast/DetourTileCache/Include)
helios_tp_quiet(tp_recast)
add_library(helios::tp::recast ALIAS tp_recast)

# --- Monocypher (X25519 + ChaCha20-Poly1305 for the wire protocol) ---------------------------
add_library(tp_monocypher STATIC ${TP}/monocypher/src/monocypher.c)
target_include_directories(tp_monocypher SYSTEM PUBLIC ${TP}/monocypher/src)
helios_tp_quiet(tp_monocypher)
add_library(helios::tp::monocypher ALIAS tp_monocypher)

# --- Tracy profiler (client compiled in; zones are no-ops unless HELIOS_PROFILE=ON) ----------
add_library(tp_tracy STATIC ${TP}/tracy/public/TracyClient.cpp)
target_include_directories(tp_tracy SYSTEM PUBLIC ${TP}/tracy/public)
if(HELIOS_PROFILE)
  target_compile_definitions(tp_tracy PUBLIC TRACY_ENABLE TRACY_ON_DEMAND)
endif()
find_package(Threads REQUIRED)
target_link_libraries(tp_tracy PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
helios_tp_quiet(tp_tracy)
add_library(helios::tp::tracy ALIAS tp_tracy)

# --- Luau (typed, sandboxable Lua dialect: gameplay scripting on client, server and editor) --
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/luau ${CMAKE_CURRENT_BINARY_DIR}/luau EXCLUDE_FROM_ALL)
foreach(t Luau.Common Luau.Ast Luau.Bytecode Luau.Inliner Luau.Compiler Luau.Config Luau.Analysis
          Luau.CodeGen Luau.VM Luau.Require)
  if(TARGET ${t})
    helios_tp_quiet(${t})
  endif()
endforeach()
add_library(tp_luau INTERFACE)
target_link_libraries(tp_luau INTERFACE Luau.Compiler Luau.VM Luau.CodeGen)
add_library(helios::tp::luau ALIAS tp_luau)
add_library(tp_luau_analysis INTERFACE)
target_link_libraries(tp_luau_analysis INTERFACE Luau.Analysis)
add_library(helios::tp::luau_analysis ALIAS tp_luau_analysis)
