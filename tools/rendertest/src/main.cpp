// helios-rendertest entry point (arguments from the process command line: Unicode-safe on Windows).

#include <cstdio>
#include <string>

#include "cli.h"
#include "helios/core/cmdline.h"
#include "helios/core/log.h"

int main(int, char**) {
    helios::log::setLevel(helios::log::Level::Warn);
    const helios::CommandLine cmd = helios::CommandLine::fromProcess();
    std::string out;
    std::string err;
    const int status = helios::rendertest::runCli(cmd.arguments(), out, err);
    if (!out.empty()) std::fputs(out.c_str(), stdout);
    if (!err.empty()) std::fputs(err.c_str(), stderr);
    return status;
}
