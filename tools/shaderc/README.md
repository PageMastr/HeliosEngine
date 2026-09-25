# helios-shaderc — Slang → SPIR-V + Helios shader reflection

`helios-shaderc` (ADR-003, `docs/plan/03-rendering.md` §1.7) compiles a Slang file into one SPIR-V
1.6 module holding every `[shader("...")]` entry point, with exactly the flags the build uses
(`cmake/HeliosShaders.cmake`: entry points keep their names, column-major matrices, scalar block
layout, warnings 41012/39001 off, `-O2` or `-g2 -O0`) — the output is byte-identical to the build's
`slangc` run (tested) — and writes the Helios reflection blob (`.hsr`) next to it.

```
helios-shaderc shaders/passes/foo.slang -I shaders -o out/foo.spv [--jsonc out/foo.hsr.jsonc] [--depfile out/foo.d]
helios-shaderc --reflect-spirv existing.spv [--hsr existing.hsr]
helios-shaderc --dump out/foo.hsr          # JSONC to stdout
helios-shaderc --help
```

| Option | Meaning |
|---|---|
| `-o <file>` | SPIR-V output |
| `--hsr <file>` / `--no-hsr` | reflection blob (default: the output with extension `.hsr`) |
| `--jsonc <file>` | JSONC rendering of the reflection (review, diffs) |
| `--depfile <file>` | Makefile-style dependencies: the source and every imported module (Ninja/Make) |
| `-I <dir>`, `-D NAME[=VALUE]` | import roots and defines |
| `--entry <name>` | compile only these entry points (default: all declared ones) |
| `-g` | debug information, no optimization |
| `--validate auto\|on\|off` | run `spirv-val --target-env vulkan1.3 --scalar-block-layout` (the module goes through stdin); `auto` (default) skips it with a note when the tool is missing, `on` fails |
| `--spirv-val <path>` | validator executable (default `$HELIOS_SPIRV_VAL`, else `spirv-val` on `PATH`) |
| `--slang-root <dir>` | Slang release (default `$HELIOS_SLANG_ROOT`, the tool's directory, then the pinned release it was configured with) |

Exit codes: 0 success, 1 compile/validation/I-O failure (an output rejected by `spirv-val` is
deleted so builds never treat it as up to date), 2 usage error. Modules whose push constants exceed
128 bytes are rejected (one shared pipeline layout, 03 §1.1).

## Reflection blob (`.hsr` v1)

Binary, little-endian, deterministic; the full layout is documented in
`engine/render/include/helios/render/shader_reflection.h` (owner of the format; readers reject bad
magic, version 0 and newer versions, truncation, bad string offsets, unknown enums and blobs whose
string references would expand past a decode limit — `.hsr` and SPIR-V are parsed as untrusted
input, in time and memory linear in their size). Contents: SPIR-V version,
XXH3-128 content hash (DDC key), entry points (stage, workgroup size, push-constant use), the
push-constant block (members: offset, size, kind, components, array count), specialization
constants (id, name, kind, default bits) and descriptor bindings (set, binding, kind, count / runtime
array, access, entry-point mask). Reflection is computed from the SPIR-V itself, so it describes
exactly what the driver sees. The material parameter layout (T16) is added by a later version.

## Slang library

The Slang compiler library of the pinned prebuilt release (`tools/prebuilt/fetch_slang.cmake`,
SHA-256 verified) is **loaded at run time** (`slang-compiler.dll` / `libslang-compiler.so`, with
`slang.dll`/`libslang.so` as fallbacks): one code path on Windows and Linux, cross builds link
without the target's Slang package, and the editor's hot reload uses the same mechanism. Only COM
interfaces and the exported `slang_createGlobalSession` / `spGetBuildTagString` are used; Slang's C++
reflection wrappers (link-time exports) are avoided.

`spirv-val` is started through the core process API (`helios/core/process.h`, CreateProcessW /
posix_spawn; `src/tool_process.cpp`) with its output captured: no shell is involved, so paths with
spaces, quotes, `%`, `$`, `&` or `;` reach it verbatim (tested).

Tests: `render_tests` (`engine/render/tests/test_shaderc.cpp`) drive the built executable.

## Plan conformance

Plan-Rev: 6

Reconciled by hand with plan revision 6 (the round-5 minor revisions) on 2026-09-25, under
`docs/plan/09-roadmap-and-process.md` §5.10.2 D7. No conformance delta is open; see §5.10.4 (c) there.
