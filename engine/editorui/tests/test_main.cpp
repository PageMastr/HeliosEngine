// doctest runner for engine/editorui. Suites named "gpu*" create a real EditorHost, which needs a
// Vulkan device (lavapipe in CI) and SDL video (CTest sets SDL's offscreen driver). They are
// excluded unless a test-suite filter is given explicitly: CTest runs them as `editorui_tests_gpu`
// (`editorui_tests --test-suite=gpu*`, label "gpu"), as engine/rhi does.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <string_view>

#include "helios/core/log.h"

int main(int argc, char** argv) {
    helios::log::setLevel(helios::log::Level::Error);
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
