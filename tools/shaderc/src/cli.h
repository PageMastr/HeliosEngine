#pragma once
// helios-shaderc command line (see tools/shaderc/README.md). Separated from main() so it can be
// driven in-process by tests.

#include <string>
#include <vector>

namespace helios::shaderc {

/// Runs the tool with `args` (without the program name). Normal output goes to `out`,
/// diagnostics to `err`. Returns the process exit code: 0 success, 1 compile/validation/I-O
/// failure, 2 usage error.
int runCli(const std::vector<std::string>& args, std::string& out, std::string& err);

} // namespace helios::shaderc
