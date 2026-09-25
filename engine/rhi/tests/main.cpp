// doctest runner for engine/rhi. Suites named "gpu*" need a Vulkan device (lavapipe in CI) and are
// excluded unless a test-suite filter is given explicitly: CTest runs them as `rhi_tests_gpu`
// (`rhi_tests --test-suite=gpu*`, label "gpu").
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <string_view>

#include "helios/core/log.h"

int main(int argc, char** argv) {
    // Keep output readable: engine Info chatter is hidden, and the Null backend's (expected)
    // validation warnings from negative tests only show up at Error level.
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
