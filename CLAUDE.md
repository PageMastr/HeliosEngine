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
- Agents working in parallel must use **their own build directory** (e.g.
  `cmake -S . -B build/<your-task> -G Ninja`), never share one. ccache is enabled automatically.
- Software Vulkan (lavapipe) is available in this container; GPU tests run under `xvfb-run -a`
  and carry the CTest label `gpu`.
- Go services: `cd services && go build ./... && go test ./...` (Go 1.24, no CGO).

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
- Declare modules with `helios_module(name [HEADLESS] SOURCES … DEPS …)` and tests with
  `helios_test(...)` (see `cmake/HeliosModule.cmake`).
- Module layering is a strict DAG (L1 core → L2 foundation → L3 servers → L4 framework → L5 apps).
  HEADLESS modules (everything a cell server links) must never depend on rhi/render/ui/audio.
  Editor-only code never links into client or servers.

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
- Only permissive licenses (MIT, BSD, zlib, Apache-2.0, Boost, public domain) in shipped code.
- Never copy code from leaked Star Wars Galaxies source, SWGEmu/Core3 (AGPL), or any GPL/AGPL
  project. Learn architecture from public descriptions only. No Star Wars or other third-party IP
  in sample content.
