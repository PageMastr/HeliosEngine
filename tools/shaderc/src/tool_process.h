#pragma once
// Runs an external tool (spirv-val; the tests run helios-shaderc itself) and captures its output,
// through the core process API (helios/core/process.h: CreateProcessW / posix_spawn). No shell
// is involved, so paths with spaces, quotes, '%', '$', '&' or ';' reach the child verbatim and
// can never be interpreted as commands.
//
// Threading: runProcess may be called from any thread.

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"

namespace helios::shaderc {

struct ProcessResult {
    int status = -1;     ///< Exit code of the child (0 on success).
    std::string output;  ///< Captured stdout + stderr (interleaved as the child wrote them).
    bool ok() const noexcept { return status == 0; }
};

/// Runs `args[0]` with the remaining arguments; `input` (binary-safe) is written to its stdin, which
/// is otherwise the null device. A bare program name (no directory part) is looked up in PATH.
/// Fails (NotFound, AccessDenied, ...) when the program cannot be started; a started program's
/// non-zero exit code is reported in the result.
Result<ProcessResult> runProcess(const std::vector<std::string>& args, std::string_view input = {});

} // namespace helios::shaderc
