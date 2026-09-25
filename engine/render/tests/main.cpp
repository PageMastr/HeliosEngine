// doctest runner for engine/render. Suites named "gpu*" (reserved for tests needing a Vulkan device)
// are excluded unless a test-suite filter is given explicitly; there are none yet — GPU coverage of
// the renderer lives in tools/rendertest (ctest label `gpu`).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <string_view>

#include "helios/core/log.h"

int main(int argc, char** argv) {
    helios::log::setLevel(helios::log::Level::Warn);
    helios::log::setChannelLevel("RHI", helios::log::Level::Error);

    doctest::Context context;
    bool explicitSuite = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        for (std::string_view prefix : {"--test-suite=", "-ts=", "--dt-test-suite=", "--dt-ts="}) {
            if (arg.starts_with(prefix)) explicitSuite = true;
        }
    }
    if (!explicitSuite) context.addFilter("test-suite-exclude", "gpu*");
    context.applyCommandLine(argc, argv);
    return context.run();
}
