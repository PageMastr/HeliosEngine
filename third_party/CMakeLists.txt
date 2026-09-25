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

# --- SDL3 (windowing, input incl. IME text, gamepads w/ rumble, raw mouse) ----------------------
if(HELIOS_BUILD_GRAPHICS)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)       # X11 on Linux for now; Wayland later
  set(SDL_RENDER OFF CACHE BOOL "" FORCE)        # we render with our own Vulkan RHI
  set(SDL_GPU OFF CACHE BOOL "" FORCE)
  set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
  set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  set(SDL_AUDIO OFF CACHE BOOL "" FORCE)         # audio goes through miniaudio
  set(SDL_VULKAN ON CACHE BOOL "" FORCE)
  add_subdirectory(${TP}/sdl3 ${CMAKE_CURRENT_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
  add_library(helios::tp::sdl3 ALIAS SDL3-static)

  # --- Dear ImGui (docking) + backends + ImGuizmo suite + ImPlot ------------------------------
  add_library(tp_imgui STATIC
    ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp
    ${TP}/imgui/imgui_widgets.cpp ${TP}/imgui/imgui_demo.cpp
    ${TP}/imgui/misc/cpp/imgui_stdlib.cpp
    ${TP}/imgui/backends/imgui_impl_sdl3.cpp ${TP}/imgui/backends/imgui_impl_vulkan.cpp
    ${TP}/imguizmo/ImGuizmo.cpp ${TP}/imguizmo/ImSequencer.cpp ${TP}/imguizmo/ImCurveEdit.cpp
    ${TP}/imguizmo/ImGradient.cpp ${TP}/imguizmo/GraphEditor.cpp
    ${TP}/implot/implot.cpp ${TP}/implot/implot_items.cpp)
  target_include_directories(tp_imgui SYSTEM PUBLIC ${TP}/imgui ${TP}/imgui/backends ${TP}/imgui/misc/cpp
                                                    ${TP}/imguizmo ${TP}/implot)
  target_compile_definitions(tp_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DEFINE_MATH_OPERATORS)
  target_link_libraries(tp_imgui PUBLIC tp_volk SDL3-static)
  helios_tp_quiet(tp_imgui)
  add_library(helios::tp::imgui ALIAS tp_imgui)
endif()

# --- Jolt Physics 5.6 (double precision + cross-platform determinism) -----------------------------
# Client and server must produce bit-identical physics on MSVC, clang-cl, GCC and Clang, so FMA is
# off (JPH_CROSS_PLATFORM_DETERMINISTIC) and the ISA feature macros are emitted explicitly: Jolt's
# headers select SIMD paths from them, so every consumer must see the same set (hence PUBLIC).
file(GLOB_RECURSE JOLT_SRC CONFIGURE_DEPENDS ${TP}/jolt/Jolt/*.cpp)
list(FILTER JOLT_SRC EXCLUDE REGEX "/Jolt/(Compute|Shaders|Physics/Hair)/")
add_library(tp_jolt STATIC ${JOLT_SRC})
target_include_directories(tp_jolt SYSTEM PUBLIC ${TP}/jolt)
target_compile_definitions(tp_jolt PUBLIC JPH_DOUBLE_PRECISION JPH_CROSS_PLATFORM_DETERMINISTIC JPH_OBJECT_STREAM
                                          $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS>)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  target_compile_definitions(tp_jolt PUBLIC JPH_USE_AVX2 JPH_USE_AVX JPH_USE_SSE4_1 JPH_USE_SSE4_2
                                            JPH_USE_LZCNT JPH_USE_TZCNT JPH_USE_F16C)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(tp_jolt PUBLIC /arch:AVX2)
  elseif(MSVC) # clang-cl
    target_compile_options(tp_jolt PUBLIC /arch:AVX2 -mbmi -mpopcnt -mlzcnt -mf16c /clang:-ffp-contract=off)
  else()
    target_compile_options(tp_jolt PUBLIC -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfpmath=sse -ffp-contract=off)
  endif()
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
# Include real <ostream>/<string> instead of doctest's std forward declarations, which newer MSVC STLs reject.
target_compile_definitions(tp_doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)
add_library(helios::tp::doctest ALIAS tp_doctest)

# --- zstd -----------------------------------------------------------------------------------
file(GLOB ZSTD_SRC ${TP}/zstd/lib/common/*.c ${TP}/zstd/lib/compress/*.c ${TP}/zstd/lib/decompress/*.c)
add_library(tp_zstd STATIC ${ZSTD_SRC})
target_include_directories(tp_zstd SYSTEM PUBLIC ${TP}/zstd/lib)
# ZSTD_MULTITHREAD is tested with #ifdef, so it must be absent (not =0) for a single-threaded lib.
target_compile_definitions(tp_zstd PRIVATE ZSTD_DISABLE_ASM)
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
add_library(tp_monocypher STATIC ${TP}/monocypher/src/monocypher.c ${TP}/monocypher/src/optional/monocypher-ed25519.c)
target_include_directories(tp_monocypher SYSTEM PUBLIC ${TP}/monocypher/src ${TP}/monocypher/src/optional)
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

# --- flecs ECS (archetype storage + relationships; systems run on the Helios job system) -------
add_library(tp_flecs STATIC ${TP}/flecs/distr/flecs.c)
target_include_directories(tp_flecs SYSTEM PUBLIC ${TP}/flecs/distr)
target_compile_definitions(tp_flecs PUBLIC flecs_STATIC)
if(WIN32)
  target_link_libraries(tp_flecs PRIVATE ws2_32 dbghelp)
endif()
target_link_libraries(tp_flecs PUBLIC Threads::Threads)
helios_tp_quiet(tp_flecs)
add_library(helios::tp::flecs ALIAS tp_flecs)

# --- mimalloc (backing heaps for the tagged allocators; never a global override) ---------------
add_library(tp_mimalloc STATIC ${TP}/mimalloc/src/static.c)
target_include_directories(tp_mimalloc SYSTEM PUBLIC ${TP}/mimalloc/include)
target_compile_definitions(tp_mimalloc PRIVATE MI_STATIC_LIB)
if(WIN32)
  target_link_libraries(tp_mimalloc PRIVATE psapi shell32 user32 advapi32 bcrypt)
endif()
target_link_libraries(tp_mimalloc PUBLIC Threads::Threads)
helios_tp_quiet(tp_mimalloc)
add_library(helios::tp::mimalloc ALIAS tp_mimalloc)

# --- yyjson (JSONC text content, config, tooling I/O) ----------------------------------------
add_library(tp_yyjson STATIC ${TP}/yyjson/src/yyjson.c)
target_include_directories(tp_yyjson SYSTEM PUBLIC ${TP}/yyjson/src)
helios_tp_quiet(tp_yyjson)
add_library(helios::tp::yyjson ALIAS tp_yyjson)

# --- ozz-animation (runtime sampling/blending/IK + offline builders for the importer) ----------
file(GLOB_RECURSE OZZ_RUNTIME_SRC ${TP}/ozz/src/base/*.cc ${TP}/ozz/src/animation/runtime/*.cc
                                  ${TP}/ozz/src/geometry/runtime/*.cc)
add_library(tp_ozz STATIC ${OZZ_RUNTIME_SRC})
target_include_directories(tp_ozz SYSTEM PUBLIC ${TP}/ozz/include PRIVATE ${TP}/ozz/src)
helios_tp_quiet(tp_ozz)
add_library(helios::tp::ozz ALIAS tp_ozz)
file(GLOB OZZ_OFFLINE_SRC ${TP}/ozz/src/animation/offline/*.cc)
add_library(tp_ozz_offline STATIC ${OZZ_OFFLINE_SRC})
target_include_directories(tp_ozz_offline SYSTEM PUBLIC ${TP}/ozz/include PRIVATE ${TP}/ozz/src)
target_link_libraries(tp_ozz_offline PUBLIC tp_ozz)
helios_tp_quiet(tp_ozz_offline)
add_library(helios::tp::ozz_offline ALIAS tp_ozz_offline)

# --- netcode + reliable (connect-token secured UDP sessions + reliability/fragmentation) -------
add_library(tp_netcode STATIC ${TP}/netcode/netcode.c ${TP}/reliable/reliable.c)
target_include_directories(tp_netcode SYSTEM PUBLIC ${TP}/netcode ${TP}/reliable PRIVATE ${TP}/netcode/sodium)
file(GLOB NETCODE_SODIUM_SRC ${TP}/netcode/sodium/*.c)
target_sources(tp_netcode PRIVATE ${NETCODE_SODIUM_SRC})
target_compile_definitions(tp_netcode PRIVATE NETCODE_ENABLE_TESTS=0 RELIABLE_ENABLE_TESTS=0
                                              $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
if(WIN32)
  target_link_libraries(tp_netcode PUBLIC ws2_32 iphlpapi advapi32)
endif()
helios_tp_quiet(tp_netcode)
add_library(helios::tp::netcode ALIAS tp_netcode)

# --- nats.c 3.14 (cells and gateways -> Go services over NATS request/reply; ADR-013) -----------
# Client library only, built with our own target instead of nats.c's CMake: static, no TLS (no
# OpenSSL; dev NATS listens on loopback and production links run inside the private network,
# 05 §6.5), no NATS Streaming, no libsodium (nats.c's built-in ed25519 serves NKeys). nats.h is
# included only by engine .cpp files, never by public headers (02 §1.1).
file(GLOB NATSC_SRC ${TP}/nats.c/src/*.c ${TP}/nats.c/src/glib/*.c)
if(WIN32)
  file(GLOB NATSC_PLATFORM_SRC ${TP}/nats.c/src/win/*.c)
else()
  file(GLOB NATSC_PLATFORM_SRC ${TP}/nats.c/src/unix/*.c)
endif()
add_library(tp_natsc STATIC ${NATSC_SRC} ${NATSC_PLATFORM_SRC})
target_include_directories(tp_natsc SYSTEM PUBLIC ${TP}/nats.c/src
                                         PRIVATE ${TP}/nats.c/src/include ${TP}/nats.c/src/glib)
target_compile_definitions(tp_natsc PRIVATE NATS_STATIC _REENTRANT $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
if(WIN32)
  target_compile_definitions(tp_natsc PRIVATE _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(tp_natsc PUBLIC ws2_32)
else()
  target_compile_definitions(tp_natsc PRIVATE _GNU_SOURCE $<$<PLATFORM_ID:Linux>:LINUX>)
  find_package(Threads REQUIRED)
  target_link_libraries(tp_natsc PUBLIC Threads::Threads)
endif()
helios_tp_quiet(tp_natsc)
add_library(helios::tp::natsc ALIAS tp_natsc)
