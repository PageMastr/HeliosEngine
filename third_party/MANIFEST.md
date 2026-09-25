# Third-party manifest

All engine runtime dependencies are vendored as source (Godot-style) so a build never touches the
network. Re-vendor with `tools/vendor/fetch_third_party.sh [name...]`. Only permissive licenses
(MIT / BSD / zlib / Apache-2.0 / Boost / public domain) are allowed in shipped runtime code.

| Name | Upstream | Pinned ref (commit) | License | Used for |
|---|---|---|---|---|
| Vulkan-Headers | KhronosGroup/Vulkan-Headers | v1.4.364 (b0c3dd6) | Apache-2.0 / MIT | Vulkan API headers (C only; `.hpp` pruned) |
| volk | zeux/volk | 1.4.350 (3ca312a) | MIT | Vulkan meta-loader; no link-time libvulkan dependency |
| VulkanMemoryAllocator | GPUOpen/VulkanMemoryAllocator | v3.4.0 (3aa9212) | MIT | GPU memory sub-allocation |
| Dear ImGui (docking) | ocornut/imgui | v1.92.9-docking (9b4eb24) | MIT | Editor / launcher / debug UI (SDL3 + Vulkan backends) |
| ImGuizmo suite | CedricGuillemet/ImGuizmo | master (18cef5e) | MIT | Transform gizmos, sequencer, curve & gradient editors, graph editor |
| ImPlot | epezent/implot | v1.0 (524f9fc) | MIT | Profiler / telemetry / economy charts |
| Jolt Physics | jrouwe/JoltPhysics | v5.6.0 (e77f175) | MIT | Physics (`JPH_DOUBLE_PRECISION` + `JPH_CROSS_PLATFORM_DETERMINISTIC`; Compute/Shaders/Hair removed) |
| meshoptimizer | zeux/meshoptimizer | v1.2 (9d9890c) | MIT | Mesh optimization, meshlets, LOD simplification |
| cgltf | jkuhlmann/cgltf | v1.15 (360db1a) | MIT | glTF 2.0 import |
| stb | nothings/stb | master (2c980bb) | MIT / public domain | Image load/write, font rasterization, Perlin noise |
| miniaudio | mackron/miniaudio | 0.11.25 (9634bed) | MIT-0 / public domain | Audio device + mixing + 3D spatialization |
| zstd | facebook/zstd | v1.5.7 (f8745da) | BSD-3 | Pak/chunk compression, network compression |
| xxHash | Cyan4973/xxHash | v0.8.3 (e626a72) | BSD-2 | Content hashing (XXH3/XXH128) |
| doctest | doctest/doctest | v2.4.12 (1da23a3) | MIT | Unit tests |
| Recast/Detour | recastnavigation/recastnavigation | v1.6.0 (6dc1667) | zlib | Navmesh generation, pathfinding, crowds |
| Monocypher | LoupVaillant/Monocypher | 4.0.3 (ab2b16d) | BSD-2 / CC0 | Ed25519 manifest signing, X25519/XChaCha20 utilities (4.0.3 fixes an EdDSA timing leak) |
| Tracy | wolfpld/tracy | v0.14.1 (30997d5) | BSD-3 | Frame/zone profiler (enabled with `HELIOS_PROFILE=ON`; viewer must match 0.14.1) |
| Luau | luau-lang/luau | 0.739 (a62362a) | MIT | Gameplay scripting VM + compiler + native codegen + type analysis (editor) |
| SDL3 | libsdl-org/SDL | release-3.4.16 (fa2c02b) | zlib | Windowing, input (IME, gamepads w/ rumble, raw mouse), replaces GLFW |
| flecs | SanderMertens/flecs | v4.1.6 (fb55f3c) | MIT | Archetype ECS with relationships (single-file distr build) |
| mimalloc | microsoft/mimalloc | v3.5.3 (d4881d3) | MIT | Heaps behind the tagged allocators (no global override) |
| yyjson | ibireme/yyjson | 0.13.0 (6447536) | MIT | JSONC text content / config parsing and writing |
| ozz-animation | guillaumeblanc/ozz-animation | 0.17.0 (744eb9d) | MIT | Skeletal animation runtime + offline builders (FBX/glTF tools excluded) |
| netcode | mas-bandwidth/netcode | v1.4.8 (47a156b) | BSD-3 (bundled libsodium subset: ISC) | Connect-token secured, encrypted UDP sessions |
| reliable | mas-bandwidth/reliable | v1.4.5 (e4e7092) | BSD-3 | Packet acks, reliability, fragmentation on top of netcode |

Planned additions (vendored when their phase starts, per docs/research/10-tech-selection.md §13): RmlUi 6.3,
FreeType VER-2-14-3 (FTL: credit required in product docs), HarfBuzz 14.5.0, SheenBidi v3.0.0, libunibreak 8.0,
basis_universal v2_50, nats.c v3.14.0, sentry-native 0.17.1; tools-only: Slang v2026.18.2 (prebuilt, SHA-256
pinned), bc7enc_rdo, tinyexr v3.2.0, ufbx v0.23.0, msdfgen v1.13.
