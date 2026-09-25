#pragma once
// helios-rendertest command line, separated from main() for testability.

#include <string>
#include <vector>

namespace helios::rendertest {

/// Runs the tool with `args` (without the program name). Returns 0 when every scene passed (or was
/// skipped), 1 on failures, 2 on usage errors.
int runCli(const std::vector<std::string>& args, std::string& out, std::string& err);

} // namespace helios::rendertest
