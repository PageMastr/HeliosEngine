# Third-party manifest

All engine runtime dependencies are vendored as source (Godot-style) so a build never touches the
network. Re-vendor with `tools/vendor/fetch_third_party.sh [name...]`. Only permissive licenses
(MIT / BSD / zlib / Apache-2.0 / Boost / public domain) are allowed in shipped runtime code.

| Name | Upstream | Pinned ref (commit) | License | Used for |
|---|---|---|---|---|
| Vulkan-Headers | KhronosGroup/Vulkan-Headers | v1.4.364 (b0c3dd6) | Apache-2.0 / MIT | Vulkan API headers (C only; `.hpp` pruned) |
| volk | zeux/volk | 1.4.350 (3ca312a) | MIT | Vulkan meta-loader; no link-time libvulkan dependency |
| VulkanMemoryAllocator | GPUOpen/VulkanMemoryAllocator | v3.4.0 (3aa9212) | MIT | GPU memory sub-allocation |
| GLFW | glfw/glfw | 3.4 (7b6aead) | zlib | Window, input, gamepad (X11 + Win32 + Cocoa) |
| Dear ImGui (docking) | ocornut/imgui | v1.92.9-docking (9b4eb24) | MIT | Editor / launcher / debug UI |
| ImGuizmo suite | CedricGuillemet/ImGuizmo | master (18cef5e) | MIT | Transform gizmos, sequencer, curve & gradient editors, graph editor |
| ImPlot | epezent/implot | v1.0 (524f9fc) | MIT | Profiler / telemetry / economy charts |
| Jolt Physics | jrouwe/JoltPhysics | v5.3.0 (0373ec0) | MIT | Physics (built with `JPH_DOUBLE_PRECISION`) |
| meshoptimizer | zeux/meshoptimizer | v1.2 (9d9890c) | MIT | Mesh optimization, meshlets, LOD simplification |
| cgltf | jkuhlmann/cgltf | v1.15 (360db1a) | MIT | glTF 2.0 import |
| stb | nothings/stb | master (2c980bb) | MIT / public domain | Image load/write, font rasterization, Perlin noise |
| miniaudio | mackron/miniaudio | 0.11.25 (9634bed) | MIT-0 / public domain | Audio device + mixing + 3D spatialization |
| zstd | facebook/zstd | v1.5.7 (f8745da) | BSD-3 | Pak/chunk compression, network compression |
| xxHash | Cyan4973/xxHash | v0.8.3 (e626a72) | BSD-2 | Content hashing (XXH3/XXH128) |
| doctest | doctest/doctest | v2.4.12 (1da23a3) | MIT | Unit tests |
| Recast/Detour | recastnavigation/recastnavigation | v1.6.0 (6dc1667) | zlib | Navmesh generation, pathfinding, crowds |
| Monocypher | LoupVaillant/Monocypher | 4.0.2 (0d85f98) | BSD-2 / CC0 | X25519 key exchange, XChaCha20-Poly1305 packet encryption |
| Tracy | wolfpld/tracy | v0.11.1 (5d542dc) | BSD-3 | Frame/zone profiler (enabled with `HELIOS_PROFILE=ON`) |
| Luau | luau-lang/luau | 0.739 (a62362a) | MIT | Gameplay scripting VM + compiler + native codegen + type analysis (editor) |
