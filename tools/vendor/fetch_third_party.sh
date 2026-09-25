#!/usr/bin/env bash
# Re-vendors pinned third-party sources into third_party/ (Godot-style: sources are committed,
# builds never touch the network). Run from repo root: tools/vendor/fetch_third_party.sh [name...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TP="$ROOT/third_party"
WORK="${VENDOR_WORK:-$(mktemp -d)}"
mkdir -p "$TP"

clone() { # name repo tag
  rm -rf "$WORK/$1"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$3" "https://github.com/$2.git" "$WORK/$1"
}
copy() { # name src... (relative to clone) -> third_party/name/
  local n="$1"; shift
  for p in "$@"; do
    mkdir -p "$TP/$n/$(dirname "$p")"
    cp -r "$WORK/$n/$p" "$TP/$n/$(dirname "$p")/"
  done
}
fresh() { rm -rf "$TP/$1"; mkdir -p "$TP/$1"; }

want() { [ ${#SELECTED[@]} -eq 0 ] && return 0; for w in "${SELECTED[@]}"; do [ "$w" = "$1" ] && return 0; done; return 1; }
SELECTED=("$@")

if want vulkan-headers; then clone vulkan-headers KhronosGroup/Vulkan-Headers v1.4.364; fresh vulkan-headers; copy vulkan-headers include/vulkan include/vk_video LICENSE.md; find "$TP/vulkan-headers" \( -name "*.hpp" -o -name "*.cppm" \) -delete; fi
if want volk;       then clone volk zeux/volk 1.4.350; fresh volk; copy volk volk.c volk.h LICENSE.md; fi
if want vma;        then clone vma GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator v3.4.0; fresh vma; copy vma include/vk_mem_alloc.h LICENSE.txt; fi
if want glfw;       then clone glfw glfw/glfw 3.4; fresh glfw; copy glfw include src CMake CMakeLists.txt LICENSE.md; fi
if want imgui;      then clone imgui ocornut/imgui v1.92.9-docking; fresh imgui
  (cd "$WORK/imgui" && cp imgui*.cpp imgui*.h imstb_*.h imconfig.h LICENSE.txt "$TP/imgui/")
  copy imgui backends/imgui_impl_glfw.cpp backends/imgui_impl_glfw.h backends/imgui_impl_vulkan.cpp backends/imgui_impl_vulkan.h misc/cpp; fi
if want imguizmo;   then clone imguizmo CedricGuillemet/ImGuizmo master; fresh imguizmo; (cd "$WORK/imguizmo" && cp src/*.cpp src/*.h LICENSE "$TP/imguizmo/"); fi
if want implot;     then clone implot epezent/implot v1.0; fresh implot; (cd "$WORK/implot" && cp implot.cpp implot.h implot_internal.h implot_items.cpp LICENSE "$TP/implot/"); fi
if want meshoptimizer; then clone meshoptimizer zeux/meshoptimizer v1.2; fresh meshoptimizer; copy meshoptimizer src LICENSE.md; fi
if want cgltf;      then clone cgltf jkuhlmann/cgltf v1.15; fresh cgltf; copy cgltf cgltf.h cgltf_write.h LICENSE; fi
if want stb;        then clone stb nothings/stb master; fresh stb; copy stb stb_image.h stb_image_write.h stb_truetype.h stb_rect_pack.h stb_perlin.h LICENSE; fi
if want miniaudio;  then clone miniaudio mackron/miniaudio 0.11.25; fresh miniaudio; copy miniaudio miniaudio.h LICENSE; fi
if want zstd;       then clone zstd facebook/zstd v1.5.7; fresh zstd; copy zstd lib/common lib/compress lib/decompress lib/zstd.h lib/zdict.h lib/zstd_errors.h LICENSE; fi
if want xxhash;     then clone xxhash Cyan4973/xxHash v0.8.3; fresh xxhash; copy xxhash xxhash.h LICENSE; fi
if want doctest;    then clone doctest doctest/doctest v2.4.12; fresh doctest; copy doctest doctest/doctest.h LICENSE.txt; fi
if want recast;     then clone recast recastnavigation/recastnavigation v1.6.0; fresh recast; copy recast Recast Detour DetourCrowd DetourTileCache License.txt; find "$TP/recast" -name CMakeLists.txt -delete; fi
if want jolt;       then clone jolt jrouwe/JoltPhysics v5.3.0; fresh jolt; copy jolt Jolt LICENSE; fi
if want monocypher; then clone monocypher LoupVaillant/Monocypher 4.0.2; fresh monocypher; copy monocypher src/monocypher.c src/monocypher.h LICENCE.md; fi
if want tracy;      then clone tracy wolfpld/tracy v0.11.1; fresh tracy; copy tracy public LICENSE; fi
if want luau;       then clone luau luau-lang/luau 0.739; fresh luau; copy luau Common Ast Bytecode Inliner Compiler Config Analysis CodeGen VM Require CLI extern CMakeLists.txt Sources.cmake LICENSE.txt lua_LICENSE.txt; fi
echo "vendored into $TP"
