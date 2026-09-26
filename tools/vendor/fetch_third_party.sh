#!/usr/bin/env bash
# Re-vendors pinned third-party sources into third_party/ (Godot-style: sources are committed,
# builds never touch the network). Run from repo root: tools/vendor/fetch_third_party.sh [name...]
#
# Helios never edits vendored code in place. A change it needs is a patch in
# third_party/<name>/patches/NNNN-<slug>.patch (a git diff relative to third_party/<name>), listed in
# third_party/MANIFEST.md ("Patches"). After copying a dependency, this script applies its patches in
# file-name order with `git apply` (exact context, no fuzz) and keeps the patches directory; a patch
# that no longer applies stops the script and must be rebased onto the new upstream. The
# `lint_vendor_patches` CTest checks that every listed patch is applied to the committed tree.
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
# Empties third_party/<name>/ for a fresh copy, keeping its patches/ (Helios-owned, not upstream).
REFRESHED=()
fresh() {
  rm -rf "$WORK/.patches/$1"
  if [ -d "$TP/$1/patches" ]; then mkdir -p "$WORK/.patches"; cp -r "$TP/$1/patches" "$WORK/.patches/$1"; fi
  rm -rf "$TP/$1"; mkdir -p "$TP/$1"
  REFRESHED+=("$1")
}
# Restores third_party/<name>/patches/ and applies *.patch in order. `git apply` resolves patch paths
# from the repository top, so they are prefixed with this tree's location inside it.
apply_patches() {
  local n="$1" p prefix
  [ -d "$WORK/.patches/$n" ] || return 0
  cp -r "$WORK/.patches/$n" "$TP/$n/patches"
  prefix="$(git -C "$ROOT" rev-parse --show-prefix 2>/dev/null || true)"
  for p in "$TP/$n/patches"/*.patch; do
    [ -e "$p" ] || continue
    (cd "$ROOT" && git apply --whitespace=nowarn --directory="${prefix}third_party/$n" "$p")
    echo "patched $n: $(basename "$p")"
  done
}

want() { [ ${#SELECTED[@]} -eq 0 ] && return 0; for w in "${SELECTED[@]}"; do [ "$w" = "$1" ] && return 0; done; return 1; }
SELECTED=("$@")

if want vulkan-headers; then clone vulkan-headers KhronosGroup/Vulkan-Headers v1.4.364; fresh vulkan-headers; copy vulkan-headers include/vulkan include/vk_video LICENSE.md; find "$TP/vulkan-headers" \( -name "*.hpp" -o -name "*.cppm" \) -delete; fi
if want volk;       then clone volk zeux/volk 1.4.350; fresh volk; copy volk volk.c volk.h LICENSE.md; fi
if want vma;        then clone vma GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator v3.4.0; fresh vma; copy vma include/vk_mem_alloc.h LICENSE.txt; fi
if want imgui;      then clone imgui ocornut/imgui v1.92.9-docking; fresh imgui
  (cd "$WORK/imgui" && cp imgui*.cpp imgui*.h imstb_*.h imconfig.h LICENSE.txt "$TP/imgui/")
  copy imgui backends/imgui_impl_sdl3.cpp backends/imgui_impl_sdl3.h backends/imgui_impl_vulkan.cpp backends/imgui_impl_vulkan.h misc/cpp; fi
if want imguizmo;   then clone imguizmo CedricGuillemet/ImGuizmo master; fresh imguizmo; (cd "$WORK/imguizmo" && cp src/*.cpp src/*.h LICENSE "$TP/imguizmo/"); fi
if want implot;     then clone implot epezent/implot v1.0; fresh implot; (cd "$WORK/implot" && cp implot.cpp implot.h implot_internal.h implot_items.cpp LICENSE "$TP/implot/"); fi
if want meshoptimizer; then clone meshoptimizer zeux/meshoptimizer v1.2; fresh meshoptimizer; copy meshoptimizer src LICENSE.md; fi
if want cgltf;      then clone cgltf jkuhlmann/cgltf v1.15; fresh cgltf; copy cgltf cgltf.h cgltf_write.h LICENSE; fi
if want stb;        then clone stb nothings/stb master; fresh stb; copy stb stb_image.h stb_image_write.h stb_truetype.h stb_rect_pack.h stb_perlin.h LICENSE; fi
if want miniaudio;  then clone miniaudio mackron/miniaudio 0.11.25; fresh miniaudio; copy miniaudio miniaudio.h LICENSE; fi
if want zstd;       then clone zstd facebook/zstd v1.5.7; fresh zstd; copy zstd lib/common lib/compress lib/decompress lib/zstd.h lib/zdict.h lib/zstd_errors.h LICENSE; fi
if want xxhash;     then clone xxhash Cyan4973/xxHash v0.8.4; fresh xxhash; copy xxhash xxhash.h LICENSE; fi
if want doctest;    then clone doctest doctest/doctest v2.5.3; fresh doctest; copy doctest doctest/doctest.h LICENSE.txt; fi
if want recast;     then clone recast recastnavigation/recastnavigation v1.6.0; fresh recast; copy recast Recast Detour DetourCrowd DetourTileCache License.txt; find "$TP/recast" -name CMakeLists.txt -delete; fi
if want jolt;       then clone jolt jrouwe/JoltPhysics v5.6.0; fresh jolt; copy jolt Jolt LICENSE; rm -rf "$TP/jolt/Jolt/Compute" "$TP/jolt/Jolt/Shaders" "$TP/jolt/Jolt/Physics/Hair"; fi
if want monocypher; then clone monocypher LoupVaillant/Monocypher 4.0.3; fresh monocypher; copy monocypher src/monocypher.c src/monocypher.h src/optional/monocypher-ed25519.c src/optional/monocypher-ed25519.h LICENCE.md; fi
if want tracy;      then clone tracy wolfpld/tracy v0.14.1; fresh tracy; copy tracy public LICENSE; fi
if want luau;       then clone luau luau-lang/luau 0.739; fresh luau; copy luau Common Ast Bytecode Inliner Compiler Config Analysis CodeGen VM Require CLI extern tools/natvis CMakeLists.txt Sources.cmake LICENSE.txt lua_LICENSE.txt; fi
if want sdl3;       then clone sdl3 libsdl-org/SDL release-3.4.16; fresh sdl3; copy sdl3 include src cmake CMakeLists.txt LICENSE.txt wayland-protocols build-scripts; fi
if want flecs;      then clone flecs SanderMertens/flecs v4.1.6; fresh flecs; copy flecs distr/flecs.c distr/flecs.h LICENSE; fi
if want mimalloc;   then clone mimalloc microsoft/mimalloc v3.5.3; fresh mimalloc; copy mimalloc include src LICENSE; fi
if want yyjson;     then clone yyjson ibireme/yyjson 0.13.0; fresh yyjson; copy yyjson src/yyjson.c src/yyjson.h LICENSE; fi
if want ozz;        then clone ozz guillaumeblanc/ozz-animation 0.17.0; fresh ozz; copy ozz include src/base src/animation/runtime src/animation/offline src/geometry LICENSE.md; rm -rf "$TP/ozz/src/animation/offline/fbx" "$TP/ozz/src/animation/offline/gltf" "$TP/ozz/src/animation/offline/tools" "$TP/ozz/include/ozz/animation/offline/fbx" "$TP/ozz/include/ozz/animation/offline/gltf" "$TP/ozz/include/ozz/animation/offline/tools"; find "$TP/ozz" -name CMakeLists.txt -delete; fi
if want netcode;    then clone netcode mas-bandwidth/netcode v1.4.8; fresh netcode; (cd "$WORK/netcode" && cp netcode.c netcode.h LICENCE "$TP/netcode/" 2>/dev/null || cp netcode.c netcode.h LICENSE "$TP/netcode/"; [ -d sodium ] && cp -r sodium "$TP/netcode/"); fi
if want reliable;   then clone reliable mas-bandwidth/reliable v1.4.5; fresh reliable; (cd "$WORK/reliable" && cp reliable.c reliable.h "$TP/reliable/" && (cp LICENCE "$TP/reliable/" 2>/dev/null || cp LICENSE "$TP/reliable/")); fi
# nats.c: the client library only (no NATS Streaming, no libuv/libevent adapters, no TLS/OpenSSL);
# third_party/CMakeLists.txt compiles it with its own target instead of nats.c's CMake.
if want natsc;      then clone natsc nats-io/nats.c v3.14.0; fresh nats.c; mkdir -p "$TP/nats.c/src"
  (cd "$WORK/natsc" && cp LICENSE "$TP/nats.c/" && cp src/*.c src/*.h "$TP/nats.c/src/" && cp -r src/include src/unix src/win src/glib "$TP/nats.c/src/"); fi
for n in ${REFRESHED[@]+"${REFRESHED[@]}"}; do apply_patches "$n"; done
echo "vendored into $TP"
