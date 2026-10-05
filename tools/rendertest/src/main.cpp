// helios-rendertest entry point (arguments from the process command line: Unicode-safe on Windows).

#include <cstdio>
#include <string>

#include "cli.h"
#include "helios/core/cmdline.h"
#include "helios/core/log.h"
#include "helios/core/platform_init.h"

int main(int, char**) {
    helios::core::platformInit(); // stops unless the CPU gate ran and passed (02 §1.1)
    helios::log::setLevel(helios::log::Level::Warn);
    const helios::CommandLine cmd = helios::CommandLine::fromProcess();
    std::string out;
    std::string err;
    const int status = helios::rendertest::runCli(cmd.arguments(), out, err);
    if (!out.empty()) std::fputs(out.c_str(), stdout);
    if (!err.empty()) std::fputs(err.c_str(), stderr);
    return status;
}
