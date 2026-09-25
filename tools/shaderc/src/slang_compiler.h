#pragma once
// Slang -> SPIR-V through the Slang compiler library, loaded at run time from the pinned prebuilt
// release (ADR-003: "helios-shaderc (C++, links slang)"). Loading the shared library instead of
// linking its import library keeps one code path on Windows (slang-compiler.dll) and Linux
// (libslang-compiler.so), lets cross builds link without the target's Slang package, and is the
// same mechanism the editor uses for shader hot reload.
//
// The compile flags match cmake/HeliosShaders.cmake (the build's slangc invocation): SPIR-V 1.6,
// entry points keep their source names, column-major matrices, scalar block layout, warnings
// 41012/39001 disabled, -O2 (or -g2 -O0 for debug builds).
//
// Only COM interfaces and exported C entry points are used (slang_createGlobalSession,
// spGetBuildTagString): Slang's C++ reflection wrappers call link-time exports and are avoided;
// reflection comes from the SPIR-V (helios/render/shader_reflection.h).
//
// Threading: a SlangCompiler (one Slang global session) is used by one thread at a time.

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::shaderc {

struct CompileRequest {
    std::filesystem::path source;
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::pair<std::string, std::string>> defines;  ///< NAME -> value ("" = defined).
    /// Entry points to compile; empty = every entry point declared with [shader("...")].
    std::vector<std::string> entryPoints;
    bool debugInfo = false;  ///< -g2 -O0 instead of -O2.
};

struct CompileOutput {
    std::vector<u32> spirv;
    std::string diagnostics;  ///< Warnings (errors fail the compile and are in the Error message).
    std::vector<std::filesystem::path> dependencies;  ///< The source and every imported module file.
};

class SlangCompiler {
public:
    /// Loads the Slang library. Search order: `slangRoot` (if not empty), the HELIOS_SLANG_ROOT
    /// environment variable, the directory of this executable, the Slang release this tool was
    /// configured with, and finally the system library search path.
    static Result<std::unique_ptr<SlangCompiler>> load(const std::filesystem::path& slangRoot = {});
    ~SlangCompiler();
    SlangCompiler(const SlangCompiler&) = delete;
    SlangCompiler& operator=(const SlangCompiler&) = delete;

    Result<CompileOutput> compile(const CompileRequest& request);

    /// Path of the loaded library.
    const std::filesystem::path& libraryPath() const noexcept;
    /// Slang build tag (e.g. "2026.18.2").
    std::string version() const;

    struct Impl;

private:
    SlangCompiler();
    std::unique_ptr<Impl> m_impl;
};

/// Makefile-style dependency file: "<target>: <dep> <dep> ..." with spaces, '#' and '$' escaped.
std::string makeDepfile(const std::filesystem::path& target, const std::vector<std::filesystem::path>& deps);

} // namespace helios::shaderc
