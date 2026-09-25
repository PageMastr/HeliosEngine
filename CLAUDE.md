# Helios — sci-fi MMO engine: contributor guide (humans and agents)

Helios is an integrated engine + editor + client + launcher + MMO backend. The binding
architecture is `docs/plan/00-decisions.md`; the master plan is `docs/PLAN.md` with sections in
`docs/plan/`. Research that motivates decisions is in `docs/research/`.

## Platforms (non-negotiable)
- **Windows x64 is the primary platform** (MSVC 2022 / clang-cl). Linux x64 (GCC 13+, Clang 17+)
  must also build and pass tests. Never write POSIX-only or Win32-only code outside
  `engine/core/src/platform/{win32,posix}/`; everything else goes through the platform layer.
- Use `std::filesystem` for paths, fixed-width integer types, no `long` in serialized data, no
  `#pragma once`-incompatible tricks, no GCC-only extensions (`__int128`, VLAs, statement
  expressions). Guard compiler-specific code with `HELIOS_COMPILER_MSVC/CLANG/GCC` macros.
- File names are case-sensitive on Linux: match `#include` spelling exactly.

## Build
```
cmake --preset linux-gcc        # or windows-msvc-release / windows-vs2022 on Windows
cmake --build --preset linux-gcc
ctest --preset linux-gcc
cmake --preset cross-mingw && cmake --build --preset cross-mingw   # Win32 portability check on Linux
```
- Lints (layering fixtures, ISA audit, licences, IP names, Windows manifest) are CTests with the
  label `lint` (`ctest -L lint`); `cmake -P tools/ci/run_lints.cmake` runs the build-independent ones.
  See `tools/lint/README.md`.
- Agents working in parallel must use **their own build directory** (e.g.
  `cmake -S . -B build/<your-task> -G Ninja`), never share one. ccache is enabled automatically.
- Software Vulkan (lavapipe) is available in this container; GPU tests run under `xvfb-run -a`
  and carry the CTest label `gpu`.
- Go services: `cd services && go build ./... && go test ./...` (Go 1.27.1 via go.mod toolchain, no CGO).

## Layout
```
engine/<module>/include/helios/<module>/*.h   public headers   (namespace helios::<module> or helios)
engine/<module>/src/*.cpp                     implementation
engine/<module>/tests/*.cpp                   doctest unit tests (helios_test)
apps/{client,editor,launcher,cellserver,gateway,tools/*}
services/                                     Go module (cmd/, internal/, pkg/)
schemas/                                      *.hschema — the single source of truth for data types
content/                                      sample game content (text sources; cooked output is ignored)
third_party/                                  vendored deps (see MANIFEST.md); never edit in place
```
- Declare modules with `helios_module(name [HEADLESS|EDITOR_ONLY] [LAYER n] SOURCES … DEPS …)`,
  executables with `helios_executable(name [ROLE role] SOURCES … DEPS …)` and tests with
  `helios_test(...)` (see `cmake/HeliosModule.cmake`). Each module's layer, flags and allowed
  same-layer peers come from the table in `engine/CMakeLists.txt` (02 §1.1); a new module adds a row
  there (or passes LAYER).
- Module layering is a strict DAG (L1 core → L2 foundation → L3 servers → L4 framework → L5 apps),
  checked at configure time (`cmake/HeliosLayering.cmake`): no upward or unlisted same-layer
  dependency, no cycle. HEADLESS modules (everything a cell server links) must never reach
  app/input/rhi/render/ui/audio or a graphics library. EDITOR_ONLY modules never link into the client,
  launcher, bot or servers.
- AVX/AVX2 flags only on the ISA allowlist (`cmake/isa_allowlist.cmake`: `tp_jolt` and `*_avx2.cpp`
  kernels added with `helios_avx2_sources`); the CPU gate (`engine/core/src/cpugate`) stays at the
  x86-64-v1 baseline.

## Code style
- C++20. `snake_case` files, `PascalCase` types, `camelCase` functions and variables,
  `kPascalCase` constants, `m_` prefix only for private members of non-trivial classes.
  4-space indent, 110-column soft limit, braces on the same line.
- No exceptions across module boundaries and none on hot paths; use `helios::Result<T>` /
  error codes. No RTTI reliance in runtime code. Prefer handles over raw pointers across
  subsystem boundaries.
- Log via `HELIOS_LOG_{TRACE,DEBUG,INFO,WARN,ERROR}` and assert via `HELIOS_ASSERT` /
  `HELIOS_VERIFY`. Never `printf`/`std::cout` in engine code.
- Every public API gets a short doc comment saying what it does and its threading rules.
- Match the surrounding code's comment density; explain *why*, not *what*.

## Tests and quality gates
- Every module ships doctest unit tests; new behavior needs a test that could fail.
- Code must compile warning-clean on GCC and Clang with `-Wall -Wextra`, and must not break the
  MinGW cross build.
- Rendering changes add or update a golden-image test where practical.
- Performance-sensitive code states its budget (e.g. "≤ 0.5 ms per 10k entities") and has a
  benchmark or test that measures it.

## Legal / IP hygiene
- Only permissive licenses (MIT, BSD, ISC, zlib, Apache-2.0, Boost, PostgreSQL, public domain) in shipped code
  (`ctest -L lint` enforces this over third_party/ and MANIFEST.md; fonts may also be SIL OFL-1.1).
- Never copy code from leaked Star Wars Galaxies source, SWGEmu/Core3 (AGPL), or any GPL/AGPL
  project. Learn architecture from public descriptions only. No Star Wars or other third-party IP
  in sample content.
