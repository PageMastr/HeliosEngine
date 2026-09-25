// helios-shaderc entry point. Arguments come from the process command line so non-ASCII paths
// survive on Windows (main()'s argv is in the ANSI code page there).

#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "helios/core/cmdline.h"

int main(int, char**) {
    const helios::CommandLine cmd = helios::CommandLine::fromProcess();
    std::string out;
    std::string err;
    const int status = helios::shaderc::runCli(cmd.arguments(), out, err);
    if (!out.empty()) std::fputs(out.c_str(), stdout);
    if (!err.empty()) std::fputs(err.c_str(), stderr);
    return status;
}
