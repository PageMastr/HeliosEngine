#pragma once
// helios-schemac command line (also callable in-process by tests).

#include <span>
#include <string>

namespace helios::schemac {

/// Exit codes: 0 = success, 1 = schema errors (or out-of-date lock with --check-lock),
/// 2 = usage errors / not-yet-implemented emitters, 3 = I/O errors writing outputs.
int runCli(std::span<const std::string> args, std::string& out, std::string& err);

} // namespace helios::schemac
